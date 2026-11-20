//! The files a dynamically linked program loads, found the way the program
//! interpreter finds them: the interpreter the program names, then every
//! library of its `DT_NEEDED` closure, searched in the order `ld.so` uses
//! (the path entry of the needing file and of the files that needed it, the
//! run path, the directories of `ld.so.conf`, the default directories), in
//! a sysroot: an arm64 Linux root file system or a cross toolchain's
//! sysroot. Paths are guest paths; the sysroot is the guest's `/`.

use std::collections::BTreeSet;
use std::path::{Path, PathBuf};

use relift_objfile::{Machine, Object, elf};

/// The directories searched last, as `ld.so` does without a cache, in the
/// order of its built-in list.
const DEFAULT_DIRS: [&str; 6] = [
    "/lib/aarch64-linux-gnu",
    "/usr/lib/aarch64-linux-gnu",
    "/lib",
    "/usr/lib",
    "/lib64",
    "/usr/lib64",
];

/// The `ld.so` configuration files whose absence in the sysroot shadows the
/// host's: the guest never reads the machine relift runs on.
pub const LOADER_CONFIG: [&str; 3] = ["/etc/ld.so.cache", "/etc/ld.so.preload", "/etc/ld.so.conf"];

const MAX_LINKS: usize = 40;
const MAX_FILE: u64 = 1 << 30;
const MAX_CONFIG: usize = 1 << 20;

/// One file of the closure.
#[derive(Clone, Debug)]
pub struct Library {
    /// The name in reports: the soname, else the file name.
    pub name: String,
    /// Every guest path the file answers to: where the search found it,
    /// where its symbolic links point and its canonical path.
    pub paths: Vec<String>,
    /// The file.
    pub bytes: Vec<u8>,
}

/// A file the program reads that is not code: the configuration of a
/// plugin directory.
#[derive(Clone, Debug)]
pub struct DataFile {
    /// Every guest path the file answers to.
    pub paths: Vec<String>,
    /// The file.
    pub bytes: Vec<u8>,
}

/// The files of a program besides its executable.
#[derive(Clone, Debug, Default)]
pub struct Closure {
    /// The files of the plugin directories the libraries name (the
    /// configuration next to the plugins).
    pub data: Vec<DataFile>,
    /// The program interpreter, when the executable names one.
    pub interpreter: Option<Library>,
    /// The libraries in the order the loader loads them.
    pub libraries: Vec<Library>,
}

/// Resolves guest paths in a sysroot.
#[derive(Clone, Debug)]
pub struct Sysroot {
    root: PathBuf,
    overlays: Vec<(String, PathBuf)>,
}

impl Sysroot {
    /// The sysroot at `root`.
    ///
    /// # Errors
    ///
    /// `root` is not a directory.
    pub fn new(root: &Path) -> Result<Sysroot, String> {
        let root = root
            .canonicalize()
            .map_err(|e| format!("{}: {e}", root.display()))?;
        if !root.is_dir() {
            return Err(format!("{}: not a directory", root.display()));
        }
        Ok(Sysroot {
            root,
            overlays: Vec::new(),
        })
    }

    /// The same sysroot with the host directory `host` also visible in the
    /// guest at `guest`, ahead of the sysroot: a program's own directory,
    /// where `$ORIGIN` points, is not in the sysroot.
    ///
    /// # Errors
    ///
    /// `host` cannot be resolved.
    pub fn with_overlay(mut self, guest: &str, host: &Path) -> Result<Sysroot, String> {
        let host = host
            .canonicalize()
            .map_err(|e| format!("{}: {e}", host.display()))?;
        self.overlays
            .push((guest.trim_end_matches('/').to_string(), host));
        Ok(self)
    }

    /// The host path of the file or directory the guest path `path` names,
    /// following symbolic links inside the sysroot (an absolute target is
    /// the sysroot's own), and the guest path of what it reached.
    pub fn resolve(&self, path: &str) -> Option<(PathBuf, String)> {
        let guest = normalize(path);
        for (prefix, host) in &self.overlays {
            if let Some(rest) = guest
                .strip_prefix(prefix.as_str())
                .filter(|r| r.is_empty() || r.starts_with('/'))
                && let Ok(found) = host.join(rest.trim_start_matches('/')).canonicalize()
                && found.starts_with(host)
            {
                return Some((found, guest));
            }
        }
        let mut pending: Vec<String> = path
            .split('/')
            .rev()
            .filter(|s| !s.is_empty())
            .map(String::from)
            .collect();
        let mut at: Vec<String> = Vec::new();
        let mut links = 0;
        while let Some(part) = pending.pop() {
            match part.as_str() {
                "." => continue,
                ".." => {
                    at.pop();
                    continue;
                }
                _ => {}
            }
            let mut host = self.root.clone();
            host.extend(&at);
            host.push(&part);
            let meta = std::fs::symlink_metadata(&host).ok()?;
            if meta.file_type().is_symlink() {
                links += 1;
                if links > MAX_LINKS {
                    return None;
                }
                let target = std::fs::read_link(&host).ok()?;
                let target = target.to_str()?;
                if target.starts_with('/') {
                    at.clear();
                }
                pending.extend(
                    target
                        .split('/')
                        .rev()
                        .filter(|s| !s.is_empty())
                        .map(String::from),
                );
            } else {
                at.push(part);
            }
        }
        let mut host = self.root.clone();
        host.extend(&at);
        Some((host, format!("/{}", at.join("/"))))
    }

    /// The names in the guest directory `path` and whether each is a
    /// directory, sorted; empty when `path` is not a directory.
    fn list(&self, path: &str) -> Vec<(String, bool)> {
        let Some((host, _)) = self.resolve(path) else {
            return Vec::new();
        };
        let Ok(rd) = std::fs::read_dir(host) else {
            return Vec::new();
        };
        let mut out: Vec<(String, bool)> = rd
            .filter_map(|e| {
                let e = e.ok()?;
                let name = e.file_name().into_string().ok()?;
                let dir = self
                    .resolve(&format!("{}/{name}", path.trim_end_matches('/')))
                    .is_some_and(|(h, _)| h.is_dir());
                Some((name, dir))
            })
            .collect();
        out.sort();
        out
    }

    fn read(&self, path: &str) -> Option<(Vec<u8>, String)> {
        let (host, real) = self.resolve(path)?;
        let meta = std::fs::metadata(&host).ok()?;
        if !meta.is_file() || meta.len() > MAX_FILE {
            return None;
        }
        Some((std::fs::read(host).ok()?, real))
    }

    /// The directories of `/etc/ld.so.conf`, with its `include` lines
    /// expanded, in order.
    pub fn conf_dirs(&self) -> Vec<String> {
        let mut out = Vec::new();
        self.conf_file("/etc/ld.so.conf", &mut out, 0);
        out
    }

    fn conf_file(&self, path: &str, out: &mut Vec<String>, depth: usize) {
        let Some((bytes, _)) = self.read(path) else {
            return;
        };
        let text = String::from_utf8_lossy(&bytes).into_owned();
        for line in text.lines() {
            let line = line.split('#').next().unwrap_or("").trim();
            if let Some(pattern) = line.strip_prefix("include") {
                if depth < 8 && pattern.starts_with(char::is_whitespace) {
                    for f in self.glob(pattern.trim()) {
                        self.conf_file(&f, out, depth + 1);
                    }
                }
            } else if line.starts_with('/') {
                out.push(line.trim_end_matches('/').to_string());
            }
        }
    }

    fn glob(&self, pattern: &str) -> Vec<String> {
        let pattern = if pattern.starts_with('/') {
            pattern.to_string()
        } else {
            format!("/etc/{pattern}")
        };
        let (dir, file) = pattern.rsplit_once('/').unwrap_or(("", &pattern));
        let Some((host, real)) = self.resolve(if dir.is_empty() { "/" } else { dir }) else {
            return Vec::new();
        };
        let Ok(rd) = std::fs::read_dir(host) else {
            return Vec::new();
        };
        let mut names: Vec<String> = rd
            .filter_map(|e| e.ok()?.file_name().into_string().ok())
            .filter(|n| matches(file, n))
            .collect();
        names.sort();
        names
            .into_iter()
            .map(|n| format!("{}/{n}", real.trim_end_matches('/')))
            .collect()
    }
}

/// The absolute path `path` with `.`, `..` and doubled slashes folded away,
/// as the runtime's loader compares paths.
pub fn normalize(path: &str) -> String {
    let mut parts: Vec<&str> = Vec::new();
    for p in path.split('/') {
        match p {
            "" | "." => {}
            ".." => {
                parts.pop();
            }
            p => parts.push(p),
        }
    }
    format!("/{}", parts.join("/"))
}

fn matches(pattern: &str, name: &str) -> bool {
    fn go(p: &[u8], n: &[u8]) -> bool {
        match p.split_first() {
            None => n.is_empty(),
            Some((b'*', rest)) => (0..=n.len()).any(|i| go(rest, &n[i..])),
            Some((b'?', rest)) => !n.is_empty() && go(rest, &n[1..]),
            Some((c, rest)) => n.first() == Some(c) && go(rest, &n[1..]),
        }
    }
    go(pattern.as_bytes(), name.as_bytes())
}

/// Expands `$ORIGIN`, `$PLATFORM` and `$LIB` (and the braced forms) in a
/// search path entry.
fn expand(entry: &str, origin: &str) -> String {
    let mut s = entry.to_string();
    for (name, value) in [
        ("ORIGIN", origin),
        ("PLATFORM", "aarch64"),
        ("LIB", "lib64"),
    ] {
        s = s
            .replace(&format!("${{{name}}}"), value)
            .replace(&format!("${name}"), value);
    }
    s
}

fn dir_of(path: &str) -> &str {
    match path.rfind('/') {
        Some(0) => "/",
        Some(i) => &path[..i],
        None => ".",
    }
}

struct Node {
    object: Object,
    origin: String,
    parent: Option<usize>,
}

fn rpath_chain(nodes: &[Node], mut at: Option<usize>) -> Vec<String> {
    let mut out = Vec::new();
    while let Some(i) = at {
        let n = &nodes[i];
        if let Some(d) = n.object.dynamic.as_ref().filter(|d| d.rpath)
            && let Some(p) = &d.search_path
        {
            out.extend(
                p.split(':')
                    .filter(|e| !e.is_empty())
                    .map(|e| expand(e, &n.origin)),
            );
        }
        at = n.parent;
    }
    out
}

fn run_path(node: &Node) -> Vec<String> {
    node.object
        .dynamic
        .as_ref()
        .filter(|d| !d.rpath)
        .and_then(|d| d.search_path.as_ref())
        .map(|p| {
            p.split(':')
                .filter(|e| !e.is_empty())
                .map(|e| expand(e, &node.origin))
                .collect()
        })
        .unwrap_or_default()
}

fn is_aarch64(bytes: &[u8]) -> Option<Object> {
    let o = elf::parse(bytes).ok()?;
    (o.machine == Machine::Aarch64).then_some(o)
}

struct Found {
    searched: String,
    real: String,
    bytes: Vec<u8>,
    object: Object,
}

fn find(sysroot: &Sysroot, candidates: &[String]) -> Option<Found> {
    candidates.iter().find_map(|c| {
        let (bytes, real) = sysroot.read(c)?;
        let object = is_aarch64(&bytes)?;
        Some(Found {
            searched: c.clone(),
            real,
            bytes,
            object,
        })
    })
}

fn search_dirs(nodes: &[Node], at: usize, conf: &[String]) -> Vec<String> {
    let mut dirs = rpath_chain(nodes, Some(at));
    dirs.extend(run_path(&nodes[at]));
    dirs.extend(conf.iter().cloned());
    dirs.extend(DEFAULT_DIRS.iter().map(|d| (*d).to_string()));
    dirs
}

/// The files under the guest directory `dir`, to a few levels, as guest
/// paths.
fn collect_files(sysroot: &Sysroot, dir: &str, depth: usize, out: &mut Vec<String>) {
    for (name, is_dir) in sysroot.list(dir) {
        let path = format!("{}/{name}", dir.trim_end_matches('/'));
        if !is_dir {
            out.push(path);
        } else if depth < 3 {
            collect_files(sysroot, &path, depth + 1, out);
        }
    }
}

fn candidates(nodes: &[Node], at: usize, name: &str, conf: &[String]) -> Vec<String> {
    if name.contains('/') {
        return vec![name.to_string()];
    }
    search_dirs(nodes, at, conf)
        .iter()
        .map(|d| format!("{}/{name}", d.trim_end_matches('/')))
        .collect()
}

/// The strings of `bytes` made of the characters of path names and format
/// templates, at least four of them.
fn spelled_strings(bytes: &[u8]) -> impl Iterator<Item = &str> {
    bytes.split(|&b| b == 0).filter_map(|s| {
        if s.len() < 4 || s.len() > 255 {
            return None;
        }
        let s = std::str::from_utf8(s).ok()?;
        s.bytes()
            .all(|c| c.is_ascii_alphanumeric() || b"_.+-/%".contains(&c))
            .then_some(s)
    })
}

/// The library names a file spells out: strings of its bytes that name a
/// shared object (`libfoo.so`, `libfoo.so.1`, or such a name after a
/// directory), the names a program hands to `dlopen`.
fn spelled_libraries(bytes: &[u8]) -> BTreeSet<String> {
    let mut out = BTreeSet::new();
    for s in spelled_strings(bytes) {
        if s.contains('%') {
            continue;
        }
        let leaf = s.rsplit('/').next().unwrap_or(s);
        let Some(at) = leaf.find(".so") else {
            continue;
        };
        let (stem, version) = leaf.split_at(at + 3);
        if stem.len() > 3
            && stem.starts_with("lib")
            && version
                .split('.')
                .skip(1)
                .all(|v| !v.is_empty() && v.bytes().all(|c| c.is_ascii_digit()))
        {
            out.insert(s.to_string());
        }
    }
    out
}

/// The library file name templates a file spells out (`libnss_%s.so%s`:
/// the loader builds a plugin's name from a configuration value), as globs.
fn spelled_templates(bytes: &[u8]) -> BTreeSet<String> {
    spelled_strings(bytes)
        .filter(|s| !s.contains('/') && s.starts_with("lib") && s.contains(".so"))
        .filter(|s| s.contains("%s"))
        .map(|s| s.replace("%s", "*"))
        .collect()
}

/// The absolute directory paths a file spells out (a default plugin
/// directory).
fn spelled_directories(bytes: &[u8]) -> BTreeSet<String> {
    spelled_strings(bytes)
        .filter(|s| s.starts_with('/') && !s.contains('%') && s.matches('/').count() >= 2)
        .map(normalize)
        .collect()
}

/// The absolute paths under `/etc` a file spells out: the files of the
/// system's configuration glibc reads (`/etc/nsswitch.conf`,
/// `/etc/passwd`), without the loader's own, which are never present.
fn spelled_configs(bytes: &[u8]) -> BTreeSet<String> {
    spelled_strings(bytes)
        .filter(|s| s.starts_with("/etc/") && !s.contains('%') && s.len() > 5)
        .map(normalize)
        .filter(|s| !LOADER_CONFIG.contains(&s.as_str()))
        .collect()
}

struct Walk {
    out: Closure,
    plugin_dirs: BTreeSet<String>,
    libs: Vec<Library>,
    nodes: Vec<Node>,
    files: Vec<Vec<u8>>,
    seen_real: BTreeSet<String>,
    names: BTreeSet<String>,
    queue: std::collections::VecDeque<usize>,
}

impl Walk {
    /// Adds every file under `dir` when it holds a shared object: the
    /// shared objects as libraries, the rest as data files.
    fn add_plugin_dir(&mut self, sysroot: &Sysroot, parent: usize, dir: &str) {
        let mut files = Vec::new();
        collect_files(sysroot, dir, 0, &mut files);
        let plugins: Vec<String> = files
            .iter()
            .filter(|f| {
                f.rsplit('/').next().is_some_and(|n| n.contains(".so"))
                    && sysroot
                        .read(f)
                        .is_some_and(|(b, _)| is_aarch64(&b).is_some())
            })
            .cloned()
            .collect();
        if plugins.is_empty() {
            return;
        }
        for f in files {
            let Some((bytes, real)) = sysroot.read(&f) else {
                continue;
            };
            if let Some(object) = is_aarch64(&bytes) {
                let name = file_name(&f);
                if self.names.contains(&name) {
                    continue;
                }
                self.add(
                    parent,
                    &name,
                    Found {
                        searched: f,
                        real,
                        bytes,
                        object,
                    },
                );
            } else {
                let mut paths = vec![normalize(&f)];
                if !paths.contains(&real) {
                    paths.push(real);
                }
                self.out.data.push(DataFile { paths, bytes });
            }
        }
    }

    fn add(&mut self, parent: usize, need: &str, found: Found) {
        let Found {
            searched,
            real,
            bytes,
            object,
        } = found;
        self.names.insert(need.to_string());
        if !self.seen_real.insert(real.clone()) {
            add_path(&mut self.out, &mut self.libs, &real, &searched);
            return;
        }
        let name = object
            .dynamic
            .as_ref()
            .and_then(|d| d.soname.clone())
            .unwrap_or_else(|| need.to_string());
        self.names.insert(name.clone());
        self.nodes.push(Node {
            origin: dir_of(&real).to_string(),
            object,
            parent: Some(parent),
        });
        self.files.push(bytes.clone());
        self.queue.push_back(self.nodes.len() - 1);
        let mut paths = vec![normalize(&searched)];
        if !paths.contains(&real) {
            paths.push(real);
        }
        self.libs.push(Library { name, paths, bytes });
    }
}

/// The closure of the executable `exe` that sits in the guest directory
/// `origin` (what `$ORIGIN` names for it): the interpreter, the libraries
/// of `DT_NEEDED` in the order the loader loads them, then the libraries
/// that any of these files names as a string (what `dlopen` of a constant
/// name takes) and the sysroot has.
///
/// # Errors
///
/// The interpreter or a needed library is not in the sysroot or not an
/// aarch64 ELF file.
pub fn closure(
    exe: &Object,
    exe_bytes: &[u8],
    origin: &str,
    sysroot: &Sysroot,
) -> Result<Closure, String> {
    let mut w = Walk {
        out: Closure::default(),
        plugin_dirs: BTreeSet::new(),
        libs: Vec::new(),
        nodes: vec![Node {
            object: exe.clone(),
            origin: origin.to_string(),
            parent: None,
        }],
        files: vec![exe_bytes.to_vec()],
        seen_real: BTreeSet::new(),
        names: BTreeSet::new(),
        queue: std::collections::VecDeque::from([0usize]),
    };
    if let Some(interp) = &exe.interpreter {
        let (bytes, real) = sysroot
            .read(interp)
            .ok_or_else(|| format!("the program interpreter {interp} is not in the sysroot"))?;
        let object = is_aarch64(&bytes).ok_or_else(|| {
            format!("the program interpreter {interp} is not an aarch64 ELF file")
        })?;
        let name = object
            .dynamic
            .as_ref()
            .and_then(|d| d.soname.clone())
            .unwrap_or_else(|| file_name(&real));
        w.seen_real.insert(real.clone());
        w.nodes.push(Node {
            object,
            origin: dir_of(&real).to_string(),
            parent: None,
        });
        w.files.push(bytes.clone());
        w.names.insert(name.clone());
        w.names.insert(file_name(interp));
        let mut paths = vec![normalize(interp), real];
        paths.dedup();
        w.out.interpreter = Some(Library { name, paths, bytes });
    }
    let conf = sysroot.conf_dirs();
    let mut scanned = 0;
    loop {
        while let Some(i) = w.queue.pop_front() {
            let needed: Vec<String> = w.nodes[i]
                .object
                .dynamic
                .as_ref()
                .map(|d| d.needed.clone())
                .unwrap_or_default();
            for need in needed {
                if w.names.contains(&need) {
                    continue;
                }
                let tried = candidates(&w.nodes, i, &need, &conf);
                let Some(found) = find(sysroot, &tried) else {
                    return Err(format!(
                        "{need} (needed by {}) is not in the sysroot; searched {}",
                        w.nodes[i]
                            .object
                            .dynamic
                            .as_ref()
                            .and_then(|d| d.soname.clone())
                            .unwrap_or_else(|| w.nodes[i].origin.clone()),
                        tried.join(", ")
                    ));
                };
                w.add(i, &need, found);
            }
        }
        let Some(file) = w.files.get(scanned) else {
            break;
        };
        let at = scanned;
        scanned += 1;
        for name in spelled_libraries(file) {
            if w.names.contains(&name) {
                continue;
            }
            let tried = candidates(&w.nodes, at, &name, &conf);
            if let Some(found) = find(sysroot, &tried) {
                w.add(at, &name, found);
            }
        }
        let dirs = search_dirs(&w.nodes, at, &conf);
        for glob in spelled_templates(&w.files[at]) {
            for d in &dirs {
                for (n, is_dir) in sysroot.list(d) {
                    if is_dir || !matches(&glob, &n) || w.names.contains(&n) {
                        continue;
                    }
                    let path = format!("{}/{n}", d.trim_end_matches('/'));
                    if let Some(found) = find(sysroot, &[path]) {
                        w.add(at, &n, found);
                    }
                }
            }
        }
        for path in spelled_configs(&w.files[at]) {
            if let Some((bytes, real)) = sysroot.read(&path)
                && bytes.len() <= MAX_CONFIG
                && !w.out.data.iter().any(|d| d.paths.contains(&path))
            {
                let mut paths = vec![path];
                if !paths.contains(&real) {
                    paths.push(real);
                }
                w.out.data.push(DataFile { paths, bytes });
            }
        }
        for dir in spelled_directories(&w.files[at]) {
            let below = dir
                .rsplit_once('/')
                .is_some_and(|(parent, _)| dirs.iter().any(|d| normalize(d) == normalize(parent)));
            if below && w.plugin_dirs.insert(dir.clone()) {
                w.add_plugin_dir(sysroot, at, &dir);
            }
        }
    }
    w.out.libraries = w.libs;
    Ok(w.out)
}

fn add_path(out: &mut Closure, libs: &mut [Library], real: &str, searched: &str) {
    let hit = |l: &mut Library| {
        let searched = normalize(searched);
        if l.paths.iter().any(|p| p == real) && !l.paths.contains(&searched) {
            l.paths.push(searched);
        }
    };
    if let Some(i) = out.interpreter.as_mut() {
        hit(i);
    }
    for l in libs.iter_mut() {
        hit(l);
    }
}

fn file_name(path: &str) -> String {
    path.rsplit('/').next().unwrap_or(path).to_string()
}

#[cfg(test)]
mod tests {
    use super::*;
    use relift_objfile::synth::{Builder, SegmentSpec};

    fn words(v: &[u64]) -> Vec<u8> {
        v.iter().flat_map(|w| w.to_le_bytes()).collect()
    }

    fn elf(needed: &[&str], run_path: Option<(&str, bool)>, interp: Option<&str>) -> Vec<u8> {
        let mut strtab = vec![0u8];
        let at = |s: &str, strtab: &mut Vec<u8>| {
            let off = strtab.len() as u64;
            strtab.extend_from_slice(s.as_bytes());
            strtab.push(0);
            off
        };
        let mut dynamic = Vec::new();
        for n in needed {
            dynamic.extend([1, at(n, &mut strtab)]);
        }
        if let Some((p, rpath)) = run_path {
            dynamic.extend([if rpath { 15 } else { 29 }, at(p, &mut strtab)]);
        }
        dynamic.extend([5, 0x1000, 10, strtab.len() as u64, 0, 0]);
        let mut data = vec![0u8; 0x400];
        data[..strtab.len()].copy_from_slice(&strtab);
        let dyn_bytes = words(&dynamic);
        data[0x200..0x200 + dyn_bytes.len()].copy_from_slice(&dyn_bytes);
        Builder {
            shared: true,
            entry: 0x1000,
            segments: vec![SegmentSpec {
                addr: 0x1000,
                data,
                mem_size: 0x400,
                flags: 7,
            }],
            dynamic: Some((0x1200, dyn_bytes.len() as u64)),
            interpreter: interp.map(String::from),
            ..Builder::default()
        }
        .build()
    }

    fn root(name: &str, files: &[(&str, Vec<u8>)]) -> PathBuf {
        let dir =
            std::env::temp_dir().join(format!("relift-sysroot-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        for (path, bytes) in files {
            let p = dir.join(path.trim_start_matches('/'));
            std::fs::create_dir_all(p.parent().expect("a parent")).expect("dirs");
            relift_cache::write_if_changed(&p, bytes).expect("file");
        }
        dir
    }

    fn names(libs: &[Library]) -> Vec<&str> {
        libs.iter().map(|l| l.name.as_str()).collect()
    }

    #[test]
    fn the_closure_follows_needed_libraries_in_loader_order() {
        let exe = elf(&["liba.so.1", "libb.so.1"], None, Some("/lib/ld.so.1"));
        let dir = root(
            "order",
            &[
                ("/lib/ld.so.1", elf(&[], None, None)),
                ("/lib/liba.so.1", elf(&["libc.so.6"], None, None)),
                ("/lib/libb.so.1", elf(&["libc.so.6"], None, None)),
                ("/lib/libc.so.6", elf(&["ld.so.1"], None, None)),
            ],
        );
        let sys = Sysroot::new(&dir).expect("sysroot");
        let object = elf::parse(&exe).expect("parses");
        let c = closure(&object, &exe, "/app", &sys).expect("closure");
        assert_eq!(
            c.interpreter.as_ref().map(|l| l.name.as_str()),
            Some("ld.so.1")
        );
        assert_eq!(names(&c.libraries), ["liba.so.1", "libb.so.1", "libc.so.6"]);
        let _ = std::fs::remove_dir_all(dir);
    }

    #[cfg(unix)]
    #[test]
    fn a_symbolic_link_answers_to_its_own_path_and_its_targets() {
        let exe = elf(&["libc.so.6"], None, None);
        let dir = root("link", &[("/lib/libc-2.so", elf(&[], None, None))]);
        std::os::unix::fs::symlink("libc-2.so", dir.join("lib/libc.so.6")).expect("link");
        let sys = Sysroot::new(&dir).expect("sysroot");
        let object = elf::parse(&exe).expect("parses");
        let c = closure(&object, &exe, "/app", &sys).expect("closure");
        assert_eq!(c.libraries[0].paths, ["/lib/libc.so.6", "/lib/libc-2.so"]);
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn a_run_path_with_origin_beats_the_default_directories() {
        let exe = elf(&["libx.so"], Some(("$ORIGIN/../private", false)), None);
        let dir = root(
            "origin",
            &[
                ("/lib/libx.so", elf(&["libdefault.so"], None, None)),
                ("/opt/private/libx.so", elf(&[], None, None)),
                ("/opt/bin/app", exe.clone()),
            ],
        );
        let sys = Sysroot::new(&dir).expect("sysroot");
        let object = elf::parse(&exe).expect("parses");
        let c = closure(&object, &exe, "/opt/bin", &sys).expect("closure");
        assert_eq!(c.libraries[0].paths[0], "/opt/private/libx.so");
        assert_eq!(c.libraries.len(), 1);
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn the_ld_so_conf_directories_come_before_the_defaults() {
        let exe = elf(&["libx.so"], None, None);
        let dir = root(
            "conf",
            &[
                (
                    "/etc/ld.so.conf",
                    b"include /etc/ld.so.conf.d/*.conf\n".to_vec(),
                ),
                ("/etc/ld.so.conf.d/a.conf", b"# a\n/opt/libs\n".to_vec()),
                ("/opt/libs/libx.so", elf(&[], None, None)),
                ("/lib/libx.so", elf(&[], None, None)),
            ],
        );
        let sys = Sysroot::new(&dir).expect("sysroot");
        assert_eq!(sys.conf_dirs(), ["/opt/libs"]);
        let object = elf::parse(&exe).expect("parses");
        let c = closure(&object, &exe, "/app", &sys).expect("closure");
        assert_eq!(c.libraries[0].paths[0], "/opt/libs/libx.so");
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn a_missing_library_names_what_needed_it_and_where_it_looked() {
        let exe = elf(&["libgone.so.3"], None, None);
        let dir = root("missing", &[("/lib/other.so", elf(&[], None, None))]);
        let sys = Sysroot::new(&dir).expect("sysroot");
        let object = elf::parse(&exe).expect("parses");
        let e = closure(&object, &exe, "/app", &sys).expect_err("not found");
        assert!(
            e.contains("libgone.so.3") && e.contains("/lib/libgone.so.3"),
            "{e}"
        );
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn a_library_a_file_spells_by_name_joins_the_closure() {
        let mut exe = elf(&[], None, None);
        exe.extend_from_slice(b"\0libplugin.so.2\0/usr/lib/libskip.so.x\0notalib.so\0");
        let dir = root(
            "spelled",
            &[
                ("/lib/libplugin.so.2", elf(&[], None, None)),
                ("/lib/libskip.so.x", elf(&[], None, None)),
            ],
        );
        let sys = Sysroot::new(&dir).expect("sysroot");
        let object = elf::parse(&exe).expect("parses");
        let c = closure(&object, &exe, "/app", &sys).expect("closure");
        assert_eq!(names(&c.libraries), ["libplugin.so.2"]);
        let _ = std::fs::remove_dir_all(dir);
    }

    #[cfg(unix)]
    #[test]
    fn an_absolute_link_resolves_inside_the_sysroot() {
        let dir = root("abs", &[("/real/lib.so", vec![1])]);
        std::fs::create_dir_all(dir.join("lib")).expect("dir");
        std::os::unix::fs::symlink("/real/lib.so", dir.join("lib/lib.so")).expect("link");
        let sys = Sysroot::new(&dir).expect("sysroot");
        let (host, guest) = sys.resolve("/lib/lib.so").expect("resolves");
        assert_eq!(guest, "/real/lib.so");
        assert!(host.starts_with(sys.root.clone()));
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn the_configuration_files_a_library_spells_join_as_data() {
        let mut libc = elf(&[], None, None);
        libc.extend_from_slice(b"\0/etc/passwd\0/etc/hosts\0/etc/ld.so.conf\0/etc/gone\0");
        let exe = elf(&["libc.so.6"], None, None);
        let dir = root(
            "etc",
            &[
                ("/lib/libc.so.6", libc),
                ("/etc/passwd", b"root:x:0:0::/:/bin/sh\n".to_vec()),
                ("/etc/hosts", b"127.0.0.1 localhost\n".to_vec()),
                ("/etc/ld.so.conf", b"/opt\n".to_vec()),
            ],
        );
        let sys = Sysroot::new(&dir).expect("sysroot");
        let object = elf::parse(&exe).expect("parses");
        let c = closure(&object, &exe, "/app", &sys).expect("closure");
        let mut paths: Vec<&str> = c.data.iter().map(|d| d.paths[0].as_str()).collect();
        paths.sort_unstable();
        assert_eq!(paths, ["/etc/hosts", "/etc/passwd"]);
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn a_wildcard_matches_the_way_a_glob_does() {
        assert!(matches("*.conf", "a.conf"));
        assert!(!matches("*.conf", "a.cfg"));
        assert!(matches("lib?.so", "libc.so"));
    }

    #[test]
    fn the_origin_and_platform_expand_in_a_path_entry() {
        assert_eq!(expand("$ORIGIN/../lib", "/a/b"), "/a/b/../lib");
        assert_eq!(expand("${PLATFORM}/x", "/"), "aarch64/x");
    }

    #[test]
    fn the_directory_of_a_path() {
        assert_eq!(dir_of("/a/b"), "/a");
        assert_eq!(dir_of("/a"), "/");
    }

    #[test]
    fn paths_normalize_lexically() {
        assert_eq!(normalize("/a/./b/../c//d"), "/a/c/d");
        assert_eq!(normalize("/"), "/");
    }
}
