//! The scan: classify files, plan KTGL entries, run one job per input
//! file (or per `.fdata` file with the shader entries the indexes name),
//! and merge the results.
//!
//! Every job's result is cached under a key made of the file's path, size
//! and identity (the SHA-256 a NUP records, or the content hash of a
//! directory file), the dictionaries in use and the ranges asked of it. A
//! cached job decompresses and reads nothing; a cached result is used only
//! while every program it names is still in the output directory.

use std::collections::{BTreeMap, HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Instant;

use relift_switch_formats::nso::Nso;
use serde_json::{Value, json};

use crate::carve::{self, Kind, find_all};
use crate::error::{Error, Result};
use crate::sha256_hex;
use crate::source::{SNIFF_WHOLE, Source, SourceFile};
use crate::{ktgl, sarc, zst};

const CACHE_VERSION: u32 = 2;
const MAX_DEPTH: u32 = 6;
const MAX_SRCS: usize = 4;
const GAP: u64 = 256 << 10;
const SPAN: u64 = 32 << 20;
const CTRL: [u8; 4] = [0x34, 0x12, 0x76, 0x98];

/// How to run a scan.
#[derive(Clone, Debug, Default)]
pub struct Options {
    /// Worker threads; 0 for the `relift-proc` setting.
    pub jobs: usize,
    /// Where results are cached; no caching when `None`.
    pub cache: Option<PathBuf>,
    /// Only scan this package of a NUP.
    pub package: Option<usize>,
}

/// One unique program.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Program {
    /// Graphics or compute.
    pub kind: Kind,
    /// The SPH stage (1 VS, 2 TCS, 3 TES, 4 GS, 5 PS), 0 for compute.
    pub stage: u8,
    /// The SPH size inside the program (0x80 or more for graphics).
    pub sph: usize,
    /// The size in bytes.
    pub size: usize,
    /// How many times it was found.
    pub n: u64,
    /// Up to four places it was found.
    pub srcs: Vec<String>,
}

/// What went wrong, in summary.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ErrorSummary {
    /// How many times.
    pub count: u64,
    /// A few places it happened.
    pub examples: Vec<String>,
}

/// Counters of one scan.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Stats {
    /// Files the source listed.
    pub files: u64,
    /// Jobs run or restored.
    pub jobs: u64,
    /// Jobs restored from the cache.
    pub hits: u64,
    /// Seconds the scan took.
    pub millis: u64,
}

/// The result of a scan.
#[derive(Clone, Debug, Default)]
pub struct Report {
    /// Unique programs by SHA-256 (hex).
    pub programs: BTreeMap<String, Program>,
    /// What containers were seen, and how often.
    pub containers: BTreeMap<String, u64>,
    /// Errors by message.
    pub errors: BTreeMap<String, ErrorSummary>,
    /// Counters.
    pub stats: Stats,
}

impl Report {
    /// The manifest: per program SHA-256 its kind, stage, sizes, count and
    /// up to four sources.
    pub fn manifest_json(&self) -> Value {
        let mut m = serde_json::Map::new();
        for (sha, p) in &self.programs {
            m.insert(
                sha.clone(),
                json!({"kind": p.kind.name(), "stage": p.stage, "sph": p.sph, "size": p.size, "n": p.n, "srcs": p.srcs}),
            );
        }
        Value::Object(m)
    }

    /// The summary: counts, containers seen, errors and timing.
    pub fn summary_json(&self) -> Value {
        let count = |f: &dyn Fn(&Program) -> bool| self.programs.values().filter(|p| f(p)).count();
        let stages: Vec<usize> = (1..=5)
            .map(|s| count(&|p| p.kind == Kind::Graphics && p.stage == s))
            .collect();
        let errors: serde_json::Map<String, Value> = self
            .errors
            .iter()
            .map(|(k, v)| (k.clone(), json!({"count": v.count, "examples": v.examples})))
            .collect();
        json!({
            "unique": self.programs.len(),
            "graphics": count(&|p| p.kind == Kind::Graphics),
            "compute": count(&|p| p.kind == Kind::Compute),
            "stages": {"vs": stages[0], "tcs": stages[1], "tes": stages[2], "gs": stages[3], "ps": stages[4]},
            "containers": self.containers,
            "errors": errors,
            "files": self.stats.files,
            "jobs": self.stats.jobs,
            "cache_hits": self.stats.hits,
            "millis": self.stats.millis,
        })
    }
}

#[derive(Default)]
struct Out {
    programs: BTreeMap<String, Program>,
    containers: BTreeMap<String, u64>,
    errors: Vec<(String, String)>,
}

impl Out {
    fn container(&mut self, name: &str, n: u64) {
        if n > 0 {
            *self.containers.entry(name.to_string()).or_default() += n;
        }
    }

    fn error(&mut self, msg: String, at: &str) {
        self.errors.push((msg, at.to_string()));
    }

    fn to_json(&self) -> Value {
        let programs: Vec<Value> = self
            .programs
            .iter()
            .map(|(sha, p)| json!([sha, p.kind.name(), p.stage, p.sph, p.size, p.n, p.srcs]))
            .collect();
        json!({"v": CACHE_VERSION, "programs": programs, "containers": self.containers, "errors": self.errors})
    }

    fn from_json(v: &Value) -> Option<Out> {
        if v.get("v")?.as_u64()? != u64::from(CACHE_VERSION) {
            return None;
        }
        let mut out = Out::default();
        for p in v.get("programs")?.as_array()? {
            let p = p.as_array()?;
            let srcs = p
                .get(6)?
                .as_array()?
                .iter()
                .map(|s| s.as_str().map(str::to_string))
                .collect::<Option<Vec<_>>>()?;
            out.programs.insert(
                p.first()?.as_str()?.to_string(),
                Program {
                    kind: Kind::from_name(p.get(1)?.as_str()?)?,
                    stage: u8::try_from(p.get(2)?.as_u64()?).ok()?,
                    sph: usize::try_from(p.get(3)?.as_u64()?).ok()?,
                    size: usize::try_from(p.get(4)?.as_u64()?).ok()?,
                    n: p.get(5)?.as_u64()?,
                    srcs,
                },
            );
        }
        for (k, n) in v.get("containers")?.as_object()? {
            out.containers.insert(k.clone(), n.as_u64()?);
        }
        for e in v.get("errors")?.as_array()? {
            let e = e.as_array()?;
            out.errors.push((
                e.first()?.as_str()?.to_string(),
                e.get(1)?.as_str()?.to_string(),
            ));
        }
        Some(out)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Class {
    Nso,
    Container,
    Sniff,
}

fn strip_zstd(name: &str) -> (&str, bool) {
    for suf in [".zstd", ".zst", ".zs"] {
        if let Some(s) = name.strip_suffix(suf) {
            return (s, true);
        }
    }
    (name, false)
}

fn classify(path: &str) -> Option<Class> {
    let (dir, name) = path.rsplit_once('/').unwrap_or(("", path));
    let parent = dir.rsplit('/').next().unwrap_or("");
    let lower = name.to_ascii_lowercase();
    let (stem, zstd) = strip_zstd(&lower);
    let ext = stem.rsplit_once('.').map(|x| x.1);
    if ext == Some("npdm") || matches!(ext, Some("rdb" | "rdx" | "fdata")) {
        return None;
    }
    let nso_name =
        ext.is_none() && (matches!(stem, "main" | "rtld" | "sdk") || stem.starts_with("subsdk"));
    if ext == Some("nso") || parent == "exefs" || nso_name {
        return Some(Class::Nso);
    }
    match ext {
        Some(
            "bfsha" | "sarc" | "sharc" | "sharcb" | "byml" | "ptcl" | "dat" | "bnsh" | "esetb"
            | "vsetb",
        ) => Some(Class::Container),
        Some("bin") if zstd => Some(Class::Container),
        Some("bin") | None => Some(Class::Sniff),
        _ => None,
    }
}

fn sniff(head: &[u8]) -> Option<Class> {
    if head.starts_with(b"NSO0") {
        return Some(Class::Nso);
    }
    if head.starts_with(b"SARC")
        || head.starts_with(b"FSHA")
        || head.starts_with(b"BNSH")
        || head.starts_with(b"BAHS")
        || head.get(0x10..0x14) == Some(&CTRL)
    {
        return Some(Class::Container);
    }
    None
}

struct Ctx<'a> {
    source: &'a dyn Source,
    dicts: zst::Dicts,
    out_dir: &'a Path,
    cache: Option<&'a Path>,
    claimed: Mutex<HashSet<String>>,
    hits: AtomicU64,
    packages: bool,
}

impl Ctx<'_> {
    fn bin_path(&self, sha: &str) -> PathBuf {
        self.out_dir.join(format!("{sha}.bin"))
    }

    fn store(&self, sha: &str, bytes: &[u8]) -> Result<()> {
        let first = self
            .claimed
            .lock()
            .map_err(|_| Error::Format("scan lock poisoned".into()))?
            .insert(sha.to_string());
        let path = self.bin_path(sha);
        if first && !path.exists() {
            relift_cache::write_if_changed(&path, bytes)?;
        }
        Ok(())
    }

    fn cache_path(&self, key: &str) -> Option<PathBuf> {
        self.cache
            .map(|c| c.join("jobs").join(&key[..2]).join(format!("{key}.json")))
    }

    fn lookup(&self, key: &str) -> Option<Out> {
        let bytes = std::fs::read(self.cache_path(key)?).ok()?;
        let out = Out::from_json(&serde_json::from_slice(&bytes).ok()?)?;
        if out.programs.keys().all(|s| self.bin_path(s).exists()) {
            self.hits.fetch_add(1, Ordering::Relaxed);
            return Some(out);
        }
        None
    }

    fn save(&self, key: &str, out: &Out) -> Result<()> {
        let Some(path) = self.cache_path(key) else {
            return Ok(());
        };
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir)?;
        }
        let bytes = serde_json::to_vec(&out.to_json())
            .map_err(|e| Error::Format(format!("cache record: {e}")))?;
        relift_cache::write_if_changed(&path, &bytes)?;
        Ok(())
    }

    fn leaf(&self, out: &mut Out, name: &str, b: &[u8]) -> Result<()> {
        if b.starts_with(b"FSHA") {
            out.container("bfsha", 1);
        } else if b.starts_with(b"BAHS") {
            out.container("bahs", 1);
        } else if b.get(0x10..0x14) == Some(&CTRL) {
            out.container("sead-bin", 1);
        }
        out.container("bnsh", find_all(b, b"BNSH").count() as u64);
        for f in carve::carve(b) {
            let sha = carve::program_sha(b, &f);
            let p = out.programs.entry(sha.clone()).or_insert_with(|| Program {
                kind: f.kind,
                stage: f.stage(b),
                sph: f.code - f.start,
                size: f.end - f.start,
                n: 0,
                srcs: Vec::new(),
            });
            p.n += 1;
            if p.srcs.len() < MAX_SRCS {
                p.srcs.push(name.to_string());
            }
            self.store(&sha, &b[f.start..f.end])?;
        }
        Ok(())
    }

    fn unwrap(&self, out: &mut Out, name: &str, b: &[u8], depth: u32) -> Result<()> {
        if depth > MAX_DEPTH {
            return Ok(());
        }
        if b.starts_with(&zst::FRAME_MAGIC) {
            out.container("zstd", 1);
            return match zst::decode(b, &self.dicts) {
                Ok(d) => self.unwrap_plain(out, name, &d, depth),
                Err(e) => {
                    out.error(e.to_string(), name);
                    Ok(())
                }
            };
        }
        self.unwrap_plain(out, name, b, depth)
    }

    fn unwrap_plain(&self, out: &mut Out, name: &str, b: &[u8], depth: u32) -> Result<()> {
        if sarc::is_sarc(b) {
            out.container("sarc", 1);
            match sarc::members(b) {
                Ok(ms) => {
                    for (n, m) in ms {
                        self.unwrap(out, &format!("{name}/{n}"), m, depth + 1)?;
                    }
                }
                Err(e) => out.error(e.to_string(), name),
            }
            return Ok(());
        }
        self.leaf(out, name, b)
    }

    fn key(&self, parts: &[&str]) -> String {
        sha256_hex(parts.join("\u{1}").as_bytes())
    }

    fn run_file(&self, f: &SourceFile, class: Class) -> Result<Out> {
        let id = self.source.identity(f)?;
        let key = self.key(&[
            "file",
            &f.pkg.to_string(),
            &f.path,
            &f.size.to_string(),
            &id,
            self.dicts.digest(),
            &format!("{class:?}"),
        ]);
        if let Some(o) = self.lookup(&key) {
            return Ok(o);
        }
        let out = self.scan_file(f, class)?;
        self.save(&key, &out)?;
        Ok(out)
    }

    fn scan_file(&self, f: &SourceFile, class: Class) -> Result<Out> {
        let mut out = Out::default();
        let label = f.label(self.packages);
        let mut whole = None;
        let mut class = class;
        if class == Class::Sniff {
            let head = if f.size <= SNIFF_WHOLE {
                let b = self.source.read(f)?;
                let head = b[..b.len().min(crate::source::SNIFF_HEAD)].to_vec();
                whole = Some(b);
                Some(head)
            } else {
                self.source.head(f)?
            };
            match head.as_deref().and_then(sniff) {
                Some(c) => class = c,
                None => return Ok(out),
            }
        }
        let bytes = match whole {
            Some(b) => b,
            None => self.source.read(f)?,
        };
        if class == Class::Nso {
            out.container("nso", 1);
            match Nso::parse(&bytes) {
                Ok(n) => self.leaf(&mut out, &label, &n.flat_image())?,
                Err(e) => out.error(format!("nso: {e}"), &label),
            }
        } else {
            self.unwrap(&mut out, &label, &bytes, 0)?;
        }
        Ok(out)
    }
}

struct Plan {
    rows: Vec<(u8, u32, u32, u32)>,
    unmapped: u64,
    bad: Option<String>,
}

fn plan_rdb(cx: &Ctx, rdb: &SourceFile, rdx: &SourceFile) -> Result<Plan> {
    let key = cx.key(&[
        "plan",
        &rdb.pkg.to_string(),
        &rdb.path,
        &cx.source.identity(rdb)?,
        &cx.source.identity(rdx)?,
    ]);
    if let Some(path) = cx.cache_path(&key)
        && let Ok(bytes) = std::fs::read(path)
        && let Ok(v) = serde_json::from_slice::<Value>(&bytes)
        && let Some(plan) = plan_from_json(&v)
    {
        cx.hits.fetch_add(1, Ordering::Relaxed);
        return Ok(plan);
    }
    let map: HashMap<u16, (u8, u32)> = ktgl::rdx(&cx.source.read(rdx)?)
        .into_iter()
        .map(|(i, m, id)| (i, (m, id)))
        .collect();
    let locs = ktgl::shader_locations(&cx.source.read(rdb)?);
    let mut plan = Plan {
        rows: Vec::new(),
        unmapped: 0,
        bad: locs.bad.map(str::to_string),
    };
    for l in locs.found {
        match map.get(&l.index) {
            Some(&(m, id)) => plan.rows.push((m, id, l.offset, l.size)),
            None => plan.unmapped += 1,
        }
    }
    if let Some(path) = cx.cache_path(&key) {
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir)?;
        }
        let v = json!({"v": CACHE_VERSION, "rows": plan.rows, "unmapped": plan.unmapped, "bad": plan.bad});
        let bytes = serde_json::to_vec(&v).map_err(|e| Error::Format(e.to_string()))?;
        relift_cache::write_if_changed(&path, &bytes)?;
    }
    Ok(plan)
}

fn plan_from_json(v: &Value) -> Option<Plan> {
    if v.get("v")?.as_u64()? != u64::from(CACHE_VERSION) {
        return None;
    }
    let mut rows = Vec::new();
    for r in v.get("rows")?.as_array()? {
        let r = r.as_array()?;
        rows.push((
            u8::try_from(r.first()?.as_u64()?).ok()?,
            u32::try_from(r.get(1)?.as_u64()?).ok()?,
            u32::try_from(r.get(2)?.as_u64()?).ok()?,
            u32::try_from(r.get(3)?.as_u64()?).ok()?,
        ));
    }
    Some(Plan {
        rows,
        unmapped: v.get("unmapped")?.as_u64()?,
        bad: match v.get("bad")? {
            Value::Null => None,
            b => Some(b.as_str()?.to_string()),
        },
    })
}

struct Fdata {
    file: SourceFile,
    entries: Vec<(u32, u32)>,
}

fn run_fdata(cx: &Ctx, job: &Fdata) -> Result<Out> {
    let f = &job.file;
    let id = cx.source.identity(f)?;
    let ranges: String = job
        .entries
        .iter()
        .map(|(o, n)| format!("{o}+{n};"))
        .collect();
    let key = cx.key(&[
        "fdata",
        &f.pkg.to_string(),
        &f.path,
        &f.size.to_string(),
        &id,
        &ranges,
    ]);
    if let Some(o) = cx.lookup(&key) {
        return Ok(o);
    }
    let mut out = Out::default();
    let label = f.label(cx.packages);
    let wanted: Vec<(u64, u64)> = job
        .entries
        .iter()
        .map(|&(o, n)| (u64::from(o), u64::from(n)))
        .collect();
    let mut spans: Vec<(u64, u64)> = Vec::new();
    if cx.source.ranges_are_cheap(f) {
        for &(o, n) in &wanted {
            match spans.last_mut() {
                Some(s) if o <= s.0 + s.1 + GAP && o + n - s.0 <= SPAN => {
                    s.1 = s.1.max(o + n - s.0);
                }
                _ => spans.push((o, n)),
            }
        }
    } else {
        spans.push((0, f.size));
    }
    let spans = spans
        .into_iter()
        .map(|(o, n)| (o.min(f.size), n.min(f.size.saturating_sub(o))))
        .collect::<Vec<_>>();
    let data = cx.source.read_ranges(f, &spans)?;
    let mut at = 0;
    for &(o, n) in &wanted {
        while at + 1 < spans.len() && o >= spans[at + 1].0 {
            at += 1;
        }
        let name = format!("{label}@{o:#x}");
        let Some(span) = spans.get(at) else {
            out.error("KTGL: the .fdata is empty".into(), &name);
            continue;
        };
        let lo = usize::try_from(o - span.0.min(o)).unwrap_or(usize::MAX);
        let Some(entry) = usize::try_from(n)
            .ok()
            .and_then(|n| data[at].get(lo..lo.checked_add(n)?))
        else {
            out.error("KTGL: an entry runs past the .fdata".into(), &name);
            continue;
        };
        out.container("ktgl-entry", 1);
        match ktgl::inflate_entry(entry) {
            Ok(inf) => cx.leaf(&mut out, &name, &inf)?,
            Err(e) => out.error(e.to_string(), &name),
        }
    }
    cx.save(&key, &out)?;
    Ok(out)
}

enum Job {
    File(SourceFile, Class),
    Fdata(Fdata),
}

impl Job {
    fn name(&self) -> String {
        match self {
            Job::File(f, _) => f.path.clone(),
            Job::Fdata(d) => d.file.path.clone(),
        }
    }
}

fn load_dicts(source: &dyn Source, files: &[SourceFile]) -> Result<zst::Dicts> {
    let mut dicts = zst::Dicts::new();
    for f in files {
        let lower = f.path.to_ascii_lowercase();
        if lower.ends_with("zsdic.pack.zs") {
            let b = source.read(f)?;
            let b = if b.starts_with(&zst::FRAME_MAGIC) {
                zst::decode(&b, &zst::Dicts::new())?
            } else {
                b
            };
            if sarc::is_sarc(&b) {
                for (_, m) in sarc::members(&b)? {
                    dicts.add(m);
                }
            }
        } else if lower.ends_with(".zsdic") {
            dicts.add(&source.read(f)?);
        }
    }
    Ok(dicts)
}

/// Scan `source`: write every unique program to `out_dir` as `<sha>.bin`
/// and return the manifest and summary.
///
/// # Errors
///
/// The source cannot be listed or read, or a file cannot be written.
/// Malformed containers are not errors: they land in
/// [`Report::errors`].
pub fn scan(source: &dyn Source, out_dir: &Path, opts: &Options) -> Result<Report> {
    let started = Instant::now();
    std::fs::create_dir_all(out_dir)?;
    if opts.jobs > 0 {
        relift_proc::workers::set_jobs(u32::try_from(opts.jobs).unwrap_or(u32::MAX));
    }
    let mut files = source.files()?;
    if let Some(p) = opts.package {
        files.retain(|f| f.pkg == p);
    }
    let dicts = load_dicts(source, &files)?;
    let cx = Ctx {
        source,
        dicts,
        out_dir,
        cache: opts.cache.as_deref(),
        claimed: Mutex::new(HashSet::new()),
        hits: AtomicU64::new(0),
        packages: source.has_packages(),
    };
    let by_path: HashMap<(usize, &str), &SourceFile> = files
        .iter()
        .map(|f| ((f.pkg, f.path.as_str()), f))
        .collect();
    let mut jobs: Vec<Job> = Vec::new();
    let mut plans: Vec<(&SourceFile, &SourceFile)> = Vec::new();
    for f in &files {
        if f.path.to_ascii_lowercase().ends_with(".rdb") {
            let rdx = format!("{}.rdx", &f.path[..f.path.len() - 4]);
            if let Some(x) = by_path.get(&(f.pkg, rdx.as_str())) {
                plans.push((f, x));
            }
        } else if let Some(c) = classify(&f.path) {
            jobs.push(Job::File(f.clone(), c));
        }
    }
    let mut extra: Vec<(String, String)> = Vec::new();
    let planned = relift_proc::workers::parallel(&plans, |&(rdb, rdx)| plan_rdb(&cx, rdb, rdx));
    let mut groups: BTreeMap<(usize, String), Vec<(u32, u32)>> = BTreeMap::new();
    for (&(rdb, _), plan) in plans.iter().zip(planned) {
        let plan = plan?;
        let dir = rdb.path.rsplit_once('/').map_or("", |x| x.0);
        let pre = if dir.is_empty() {
            String::new()
        } else {
            format!("{dir}/")
        };
        if let Some(bad) = plan.bad {
            extra.push((bad, rdb.path.clone()));
        }
        for _ in 0..plan.unmapped {
            extra.push((
                "KTGL: an entry names an index the .rdx lacks".into(),
                rdb.path.clone(),
            ));
        }
        for (mnt, id, off, size) in plan.rows {
            let sub = if mnt >= 0xFE {
                String::new()
            } else {
                format!("MountPoint_{mnt}/")
            };
            let name = format!("{pre}{sub}0x{id:08x}.fdata");
            let found = (0..=rdb.pkg)
                .rev()
                .find_map(|p| by_path.get(&(p, name.as_str())).map(|f| f.pkg));
            match found {
                Some(p) => groups.entry((p, name)).or_default().push((off, size)),
                None => extra.push(("KTGL: the .fdata is not in the input".into(), name)),
            }
        }
    }
    for ((pkg, name), mut entries) in groups {
        entries.sort_unstable();
        entries.dedup();
        if let Some(f) = by_path.get(&(pkg, name.as_str())) {
            jobs.push(Job::Fdata(Fdata {
                file: (*f).clone(),
                entries,
            }));
        }
    }
    jobs.sort_by_key(|j| match j {
        Job::File(f, _) => (f.pkg, f.path.clone()),
        Job::Fdata(d) => (d.file.pkg, d.file.path.clone()),
    });
    let results = relift_proc::workers::parallel(&jobs, |j| match j {
        Job::File(f, c) => cx.run_file(f, *c),
        Job::Fdata(d) => run_fdata(&cx, d),
    });
    let mut report = Report::default();
    let add_error = |report: &mut Report, msg: String, at: String| {
        let e = report.errors.entry(msg).or_default();
        e.count += 1;
        if e.examples.len() < MAX_SRCS {
            e.examples.push(at);
        }
    };
    for (msg, at) in extra {
        add_error(&mut report, msg, at);
    }
    for (job, r) in jobs.iter().zip(results) {
        let out = match r {
            Ok(o) => o,
            Err(Error::Io(e)) => return Err(Error::Io(e)),
            Err(e) => {
                add_error(&mut report, e.to_string(), job.name());
                continue;
            }
        };
        for (sha, p) in out.programs {
            match report.programs.get_mut(&sha) {
                Some(have) => {
                    have.n += p.n;
                    for s in p.srcs {
                        if have.srcs.len() < MAX_SRCS {
                            have.srcs.push(s);
                        }
                    }
                }
                None => {
                    report.programs.insert(sha, p);
                }
            }
        }
        for (k, n) in out.containers {
            *report.containers.entry(k).or_default() += n;
        }
        for (msg, at) in out.errors {
            add_error(&mut report, msg, at);
        }
    }
    report.stats = Stats {
        files: files.len() as u64,
        jobs: (jobs.len() + plans.len()) as u64,
        hits: cx.hits.load(Ordering::Relaxed),
        millis: u64::try_from(started.elapsed().as_millis()).unwrap_or(u64::MAX),
    };
    Ok(report)
}
