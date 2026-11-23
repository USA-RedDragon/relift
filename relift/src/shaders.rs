//! The Switch 2 GPU programs of a build: the runtime records each program a
//! game sets that the build has no translation of, with the capabilities of
//! the device it ran on, and the next build translates every recorded program
//! for that device into the table the program embeds.
//!
//! The folder is `shaders/` in the build directory:
//!
//! * `device.json`, the runtime's dump of its Vulkan device (the shape
//!   `DeviceCaps::from_json` reads);
//! * `programs/<key>.bin`, each program the runtime met (its shader program
//!   header, if any, and its code), named by the hex SHA-256 of those bytes;
//! * the tables (`table.rlspv`, `table.rlspi`, `table.stamp`,
//!   `table.device`) and `failed.txt`, written here by
//!   `relift_gpu_table::translate`;
//! * `table-native.rlspv`, `table-native.rlspi` and `table-native.stamp`,
//!   the same for native float math (`Math::Native`), which the runtime
//!   reads instead when its `gpu.math` setting is `native`; `failed-native.txt`
//!   lists that table's refusals.
//!
//! The tables stay files the runtime reads a module at a time (one
//! program's translation runs to megabytes, a game's to gigabytes). The
//! program does not name the folder: the runtime looks in `shaders/` beside
//! its executable, which is this folder for the program `relift build` leaves
//! in the build directory, and a package carries [`tables`] with the program
//! and points the runtime at them (see [`env`]).

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::io::{Read as _, Write as _};
use std::path::{Path, PathBuf};

use relift_gpu_nv::translate::Math;
use relift_gpu_table::{BOTH, DEVICE_FILE, Programs, table_file, translate_math};
pub use relift_gpu_table::{STAMP, STAMP_FILE, stamp, stamp_of, stamp_of_mode};
use relift_switch_shaders::{ImportSource, Options as ScanOptions, scan};

use crate::import::BoxError;

/// The folder in the build directory.
pub const DIR: &str = "shaders";

/// The scan's per-file results in the folder.
const SCAN_CACHE: &str = "scan";

/// The environment variable that names the folder the runtime reads the
/// tables from.
pub const TABLES_ENV: &str = "RELIFT_NVN_TABLES";

/// The environment variable that names the folder the runtime records the
/// programs the tables lack and the device capabilities in.
pub const SHADERS_ENV: &str = "RELIFT_NVN_SHADERS";

/// What a build's shader table holds.
pub use relift_gpu_table::Built;

/// The keys of the programs in `dir`/programs (found by the scan or
/// recorded by a run), sorted.
fn program_keys(dir: &Path) -> Result<Vec<[u8; 32]>, BoxError> {
    Ok(Programs::dir(dir)?.keys().to_vec())
}

/// What the scan of an import found.
pub struct Discovered {
    /// Programs found (unique by content).
    pub programs: usize,
    /// What went wrong in the scan, by message and count.
    pub errors: BTreeMap<String, u64>,
}

/// Finds every shader program of the Switch 2 game in `import`, by its
/// content alone (see `relift_switch_shaders`): `out`/shaders/programs
/// gets one `<sha256>.bin` per program, and the scan keeps its per-file
/// results in `out`/shaders/scan so that a rebuild reads only what
/// changed. Nothing a run recorded is needed.
///
/// # Errors
///
/// The import or the folder cannot be read or written.
pub fn discover(import: &Path, out: &Path) -> Result<Discovered, BoxError> {
    let dir = out.join(DIR);
    let programs = dir.join("programs");
    std::fs::create_dir_all(&programs)?;
    let source = ImportSource::new(import)?;
    let report = scan(
        &source,
        &programs,
        &ScanOptions {
            cache: Some(dir.join(SCAN_CACHE)),
            ..ScanOptions::default()
        },
    )?;
    relift_cache::write_if_changed(
        &dir.join(relift_gpu_table::MESHES),
        &serde_json::to_vec(&report.meshes_json())?,
    )?;
    Ok(Discovered {
        programs: report.programs.len(),
        errors: report
            .errors
            .iter()
            .map(|(k, v)| (k.clone(), v.count))
            .collect(),
    })
}

/// Translates the programs in `out`/shaders/programs for the device of its
/// `device.json` and writes the tables of each mode in `modes` beside them, a
/// batch of programs at a time: a game's translations run to gigabytes, more
/// than memory holds. Without a device file the table is empty: the game has
/// not run on a device yet. The [`Built`] is that of the first mode.
///
/// # Errors
///
/// A file cannot be read or written, or `device.json` is not capabilities.
pub fn build(out: &Path, jobs: usize, modes: &[Math]) -> Result<Built, BoxError> {
    let dir = out.join(DIR);
    build_in(&dir, &dir, jobs, modes)
}

/// The modes the host build translates: exact math, and native math too
/// when `math` (the `shader-math` setting of the tool) is `both` or
/// `native`, or when the build makes a package, which carries both so the
/// player can pick on the machine it is installed on.
#[must_use]
pub fn host_modes(math: &str, package: bool) -> Vec<Math> {
    if package || matches!(math, "both" | "native") {
        BOTH.to_vec()
    } else {
        vec![Math::Exact]
    }
}

/// Translates the native-math tables of the host build in `out` when they
/// are missing or another translator's, for a package, which carries both
/// modes.
///
/// # Errors
///
/// As [`build`].
pub fn ensure_native(out: &Path, jobs: usize) -> Result<(), BoxError> {
    let dir = out.join(DIR);
    if program_keys(&dir)?.is_empty() || !dir.join("device.json").is_file() {
        return Ok(());
    }
    if stamp_of_mode(&dir, Math::Native).as_deref() != Some(stamp(Math::Native).as_str()) {
        build(out, jobs, &[Math::Native])?;
    }
    Ok(())
}

/// The folder of the tables a build for `target` (an `android-*` or `ios-*`
/// name) carries: `out`/`target`/shaders. Its `device.json` is the
/// capabilities of the target device, which the app writes at its first start
/// to its own shader folder; the host's table never goes to another device.
#[must_use]
pub fn target_dir(out: &Path, target: &str) -> PathBuf {
    out.join(target).join(DIR)
}

/// What [`build_target`] did.
pub enum TargetBuilt {
    /// The game has no GPU programs (not a Switch 2 game).
    NoPrograms,
    /// No `device.json` for the target: the path it is wanted at.
    NoDevice(PathBuf),
    /// The table for the target's device.
    Built(Built),
}

/// Translates the programs of the build in `out` for `target`'s device
/// (the `device.json` in [`target_dir`]) into that folder's tables, of both
/// modes ([`BOTH`]): the app carries both, and its `gpu.math` setting picks.
///
/// # Errors
///
/// As [`build`].
pub fn build_target(out: &Path, target: &str, jobs: usize) -> Result<TargetBuilt, BoxError> {
    let programs = out.join(DIR);
    if program_keys(&programs)?.is_empty() {
        return Ok(TargetBuilt::NoPrograms);
    }
    let dir = target_dir(out, target);
    let device = dir.join("device.json");
    if !device.is_file() {
        for m in BOTH {
            for ext in ["rlspv", "rlspi", "stamp"] {
                let _ = std::fs::remove_file(dir.join(table_file(m, ext)));
            }
        }
        let _ = std::fs::remove_file(dir.join(DEVICE_FILE));
        return Ok(TargetBuilt::NoDevice(device));
    }
    Ok(TargetBuilt::Built(build_in(&programs, &dir, jobs, &BOTH)?))
}

/// Translates the programs in `programs`/programs (with the scan's
/// `meshes.json` there) for the device of `dir`/device.json, writing the
/// tables of each of `modes` and their `failed.txt` in `dir`. The [`Built`]
/// is that of the first mode.
fn build_in(programs: &Path, dir: &Path, jobs: usize, modes: &[Math]) -> Result<Built, BoxError> {
    std::fs::create_dir_all(programs.join("programs"))?;
    let set = Programs::dir(programs)?;
    let device = std::fs::read(dir.join("device.json")).ok();
    let mut first = None;
    for &m in modes {
        let b = translate_math(&set, device.as_deref(), dir, jobs, m, &|_, _| true)
            .map_err(|e| format!("{}: {e}", dir.display()))?;
        first.get_or_insert(b);
    }
    first.ok_or_else(|| "no shader math mode to translate".into())
}

/// `relift shaders`: translates the programs recorded in `out`/shaders again
/// and says how many came out and how long it took.
///
/// # Errors
///
/// As [`build`].
pub fn run(out: &Path, math: &str, ui: &mut crate::ui::Ui) -> Result<(), BoxError> {
    let jobs = relift_proc::workers::jobs().max(1);
    let start = std::time::Instant::now();
    let modes = match math {
        "native" => vec![Math::Native],
        m => host_modes(m, false),
    };
    let built = build(out, jobs, &modes)?;
    ui.note(&relift_progress::Line::new().text(format!(
        "{} of {} programs translated on {jobs} threads in {:.1} s",
        built.translated,
        built.programs,
        start.elapsed().as_secs_f64()
    )));
    for (why, n) in &built.refused {
        ui.note(&relift_progress::Line::new().text(format!("{n} refused: {why}")));
    }
    Ok(())
}

/// The table files of the build in `out` that a package carries: both
/// tables and their stamp when the build translated at least one program,
/// else none.
#[must_use]
pub fn tables(out: &Path) -> Vec<PathBuf> {
    tables_in(&out.join(DIR))
}

/// The table files a package for `target` carries: the host's for `None`,
/// else only those translated for the target's own device ([`target_dir`]),
/// never the host's.
#[must_use]
pub fn tables_for(out: &Path, target: Option<&str>) -> Vec<PathBuf> {
    match target {
        None => tables(out),
        Some(t) => tables_in(&target_dir(out, t)),
    }
}

fn tables_in(dir: &Path) -> Vec<PathBuf> {
    let mut out = Vec::new();
    for m in BOTH {
        let files = [
            dir.join(table_file(m, "rlspv")),
            dir.join(table_file(m, "rlspi")),
        ];
        let head = std::fs::metadata(&files[0]).map_or(0, |m| m.len());
        if head > TABLE_HEAD_BYTES && files[1].is_file() {
            out.extend(files);
            let stamp = dir.join(table_file(m, "stamp"));
            if stamp.is_file() {
                out.push(stamp);
            }
        }
    }
    if !out.is_empty() && dir.join(DEVICE_FILE).is_file() {
        out.push(dir.join(DEVICE_FILE));
    }
    out
}

/// The length of a table with no module: its magic and its count.
const TABLE_HEAD_BYTES: u64 = 12;

/// Copies the table `files` into `dir`, which is made, streaming them (a
/// table runs to gigabytes) and leaving a file that already holds the same
/// bytes alone.
///
/// # Errors
///
/// A folder cannot be made or a file cannot be copied.
pub fn install(files: &[PathBuf], dir: &Path) -> std::io::Result<()> {
    std::fs::create_dir_all(dir)?;
    for f in files {
        let Some(name) = f.file_name() else {
            continue;
        };
        let mut from = std::fs::File::open(f)?;
        let mut to = relift_cache::StreamWriter::new(&dir.join(name))?;
        let mut buf = vec![0u8; 1 << 20];
        loop {
            let n = from.read(&mut buf)?;
            if n == 0 {
                break;
            }
            to.write_all(&buf[..n])?;
        }
        to.finish()?;
    }
    Ok(())
}

/// Points a run of the program built in `out` at the build's shader folder,
/// the one the next build reads, where the program built elsewhere (the dev
/// build, whose folder is below `out`) would not look.
pub fn set_env(cmd: &mut std::process::Command, out: &Path) {
    cmd.env(SHADERS_ENV, out.join(DIR));
}

/// `s` as a C string literal.
pub(crate) fn c_string(s: &str) -> String {
    let mut t = String::from("\"");
    for b in s.bytes() {
        match b {
            b'"' | b'\\' => {
                t.push('\\');
                t.push(char::from(b));
            }
            0x20..=0x7e => t.push(char::from(b)),
            _ => {
                let _ = write!(t, "\\{b:03o}");
            }
        }
    }
    t.push('"');
    t
}

#[cfg(test)]
mod tests {
    use super::*;
    use relift_gpu_nv::translate::Options;
    use relift_gpu_nv::translate::{DeviceCaps, HostProfile};
    use relift_gpu_table::{
        INTERFACES, SPOOL, TABLE, ZSTD_LEVEL, hex, interface_table, records, write_stamp,
    };

    fn scratch(tag: &str) -> PathBuf {
        let d = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("..")
            .join("target")
            .join(format!("relift-shaders-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(d.join(DIR)).unwrap();
        d
    }

    #[test]
    fn a_package_carries_the_tables_only_when_something_was_translated() {
        let out = scratch("tables");
        assert!(tables(&out).is_empty());
        let head = b"RLSPV002\0\0\0\0";
        relift_cache::write_if_changed(&out.join(DIR).join(TABLE), head).unwrap();
        relift_cache::write_if_changed(
            &out.join(DIR).join(INTERFACES),
            relift_gpu_table::INTERFACE_MAGIC,
        )
        .unwrap();
        assert!(tables(&out).is_empty());
        relift_cache::write_if_changed(
            &out.join(DIR).join(TABLE),
            &[head.as_slice(), b"x"].concat(),
        )
        .unwrap();
        let t = tables(&out);
        assert_eq!(
            t,
            [out.join(DIR).join(TABLE), out.join(DIR).join(INTERFACES)]
        );
        let dest = out.join("pkg").join("shaders");
        install(&t, &dest).unwrap();
        assert_eq!(std::fs::read(dest.join(TABLE)).unwrap().len(), 13);
        assert!(dest.join(INTERFACES).is_file());
        std::fs::remove_dir_all(&out).unwrap();
    }

    #[test]
    fn a_package_carries_the_stamp_beside_the_tables() {
        let out = scratch("stamp-package");
        let head = b"RLSPV002\0\0\0\0";
        relift_cache::write_if_changed(
            &out.join(DIR).join(TABLE),
            &[head.as_slice(), b"x"].concat(),
        )
        .unwrap();
        relift_cache::write_if_changed(
            &out.join(DIR).join(INTERFACES),
            relift_gpu_table::INTERFACE_MAGIC,
        )
        .unwrap();
        assert_eq!(tables(&out).len(), 2);
        write_stamp(&out.join(DIR)).unwrap();
        let t = tables(&out);
        assert_eq!(t.last().unwrap(), &out.join(DIR).join(STAMP_FILE));
        let dest = out.join("pkg").join("shaders");
        install(&t, &dest).unwrap();
        assert_eq!(stamp_of(&dest).as_deref(), Some(STAMP));
        std::fs::remove_dir_all(&out).unwrap();
    }

    #[test]
    fn the_stamp_is_the_hash_the_script_computes_for_the_tree() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
        let Ok(run) = relift_proc::command("bash")
            .arg(root.join("scripts/shader-stamp.sh"))
            .arg(&root)
            .output()
        else {
            return;
        };
        if !run.status.success() {
            return;
        }
        assert_eq!(String::from_utf8(run.stdout).unwrap().trim(), STAMP);
    }

    #[test]
    fn a_table_without_a_stamp_reads_as_unstamped() {
        let out = scratch("stamp-read");
        let dir = out.join(DIR);
        assert_eq!(stamp_of(&dir), None);
        relift_cache::write_if_changed(&dir.join(STAMP_FILE), b"\n").unwrap();
        assert_eq!(stamp_of(&dir), None);
        relift_cache::write_if_changed(&dir.join(STAMP_FILE), b"0123456789abcdef\n").unwrap();
        assert_eq!(stamp_of(&dir).as_deref(), Some("0123456789abcdef"));
        std::fs::remove_dir_all(&out).unwrap();
    }

    #[test]
    fn a_run_is_pointed_at_the_build_folder() {
        let mut cmd = relift_proc::command("true");
        set_env(&mut cmd, Path::new("/b/host"));
        let env: Vec<_> = cmd.get_envs().collect();
        assert_eq!(
            env,
            [(
                std::ffi::OsStr::new(SHADERS_ENV),
                Some(Path::new("/b/host/shaders").as_os_str())
            )]
        );
    }

    const CAPS: &str =
        r#"{"capabilities": {"device": {"extensions": {}, "features": {}, "properties": {}}}}"#;

    fn words(ws: &[u128]) -> Vec<u8> {
        ws.iter().flat_map(|w| w.to_le_bytes()).collect()
    }

    fn exit() -> u128 {
        0x14d | (4 << 9) | (7 << 12) | (7 << 87) | (7 << 110) | (7 << 113)
    }

    fn bra_self() -> u128 {
        0x147 | (4 << 9) | (7 << 12) | (0xffff_ffff_fffc << 34) | (7 << 87)
    }

    fn adler32(b: &[u8]) -> u32 {
        let (mut a, mut s) = (1u32, 0u32);
        for &x in b {
            a = (a + u32::from(x)) % 65521;
            s = (s + a) % 65521;
        }
        (s << 16) | a
    }

    fn zlib_stored(b: &[u8]) -> Vec<u8> {
        let n = u16::try_from(b.len()).unwrap();
        let mut v = vec![0x78, 0x01, 1];
        v.extend_from_slice(&n.to_le_bytes());
        v.extend_from_slice(&(!n).to_le_bytes());
        v.extend_from_slice(b);
        v.extend_from_slice(&adler32(b).to_be_bytes());
        v
    }

    #[test]
    fn the_table_is_what_translating_the_whole_set_in_memory_gives() {
        use relift_gpu_nv::translate::build::translate_set;
        let out = scratch("table");
        let dir = out.join(DIR);
        relift_cache::write_if_changed(&dir.join("device.json"), CAPS.as_bytes()).unwrap();
        std::fs::create_dir_all(dir.join("programs")).unwrap();
        let good = words(&[exit(), bra_self()]);
        let longer = words(&[exit(), exit(), bra_self()]);
        let bad = words(&[0x1ff, exit(), bra_self()]);
        let mut inputs = Vec::new();
        for code in [&good, &longer, &bad] {
            let key = relift_switch_formats::sha256(code);
            relift_cache::write_if_changed(
                &dir.join("programs").join(format!("{}.bin", hex(&key))),
                code,
            )
            .unwrap();
            inputs.push(relift_gpu_nv::translate::build::Input {
                key,
                code: code.clone(),
                options: Options::default(),
            });
        }
        let built = build(&out, 3, &[Math::Exact]).unwrap();
        assert_eq!((built.programs, built.translated), (3, 2));
        assert_eq!(built.refused.values().sum::<usize>(), 1);
        let host = HostProfile::from_device("device", &DeviceCaps::from_json(CAPS).unwrap());
        let whole = translate_set(&host, &inputs, relift_gpu_sm86::program::parse);
        assert_eq!(
            std::fs::read(dir.join(TABLE)).unwrap(),
            whole.to_compressed_bytes(ZSTD_LEVEL).unwrap()
        );
        let entries: Vec<_> = whole
            .shaders
            .iter()
            .map(|(k, (_, _, i))| (*k, records(i)))
            .collect();
        assert_eq!(
            std::fs::read(dir.join(INTERFACES)).unwrap(),
            interface_table(&entries)
        );
        assert_eq!(
            std::fs::read_to_string(dir.join("failed.txt"))
                .unwrap()
                .lines()
                .count(),
            1
        );
        assert!(!dir.join(SPOOL).exists());
        assert_eq!(stamp_of(&dir).as_deref(), Some(STAMP));
        relift_cache::write_if_changed(&dir.join(STAMP_FILE), b"0000000000000000\n").unwrap();
        build(&out, 1, &[Math::Exact]).unwrap();
        assert_eq!(
            stamp_of(&dir).as_deref(),
            Some(STAMP),
            "a rebuild refreshes an old translator's stamp"
        );
        std::fs::remove_dir_all(&out).unwrap();
    }

    #[test]
    fn a_target_gets_a_table_for_its_own_device_and_never_the_hosts() {
        let out = scratch("target");
        let dir = out.join(DIR);
        std::fs::create_dir_all(dir.join("programs")).unwrap();
        let t = "android-arm64";
        assert!(matches!(
            build_target(&out, t, 1).unwrap(),
            TargetBuilt::NoPrograms
        ));
        let code = words(&[exit(), bra_self()]);
        let key = relift_switch_formats::sha256(&code);
        relift_cache::write_if_changed(
            &dir.join("programs").join(format!("{}.bin", hex(&key))),
            &code,
        )
        .unwrap();
        relift_cache::write_if_changed(&dir.join("device.json"), CAPS.as_bytes()).unwrap();
        assert_eq!(build(&out, 1, &[Math::Exact]).unwrap().translated, 1);
        assert_eq!(tables_for(&out, None).len(), 4);
        assert_eq!(stamp_of(&dir).as_deref(), Some(STAMP));
        let want = target_dir(&out, t).join("device.json");
        match build_target(&out, t, 1).unwrap() {
            TargetBuilt::NoDevice(p) => assert_eq!(p, want),
            _ => panic!("a target without its device file has no table"),
        }
        assert!(tables_for(&out, Some(t)).is_empty());
        std::fs::create_dir_all(target_dir(&out, t)).unwrap();
        relift_cache::write_if_changed(&want, CAPS.as_bytes()).unwrap();
        match build_target(&out, t, 1).unwrap() {
            TargetBuilt::Built(b) => assert_eq!((b.programs, b.translated), (1, 1)),
            _ => panic!("a target with its device file gets a table"),
        }
        let files = tables_for(&out, Some(t));
        let td = target_dir(&out, t);
        assert_eq!(
            files,
            [
                td.join(TABLE),
                td.join(INTERFACES),
                td.join(STAMP_FILE),
                td.join("table-native.rlspv"),
                td.join("table-native.rlspi"),
                td.join("table-native.stamp"),
                td.join(DEVICE_FILE),
            ],
            "a target carries the tables of both float math modes"
        );
        assert_eq!(stamp_of_mode(&td, Math::Native), Some(stamp(Math::Native)));
        std::fs::remove_file(&want).unwrap();
        assert!(matches!(
            build_target(&out, t, 1).unwrap(),
            TargetBuilt::NoDevice(_)
        ));
        assert!(
            tables_for(&out, Some(t)).is_empty(),
            "a stale target table is removed"
        );
        assert!(
            stamp_of(&target_dir(&out, t)).is_none(),
            "and so is its stamp"
        );
        std::fs::remove_dir_all(&out).unwrap();
    }

    #[test]
    fn each_float_math_mode_has_its_own_stamp_and_files() {
        assert_eq!(stamp(Math::Exact), STAMP);
        assert_ne!(stamp(Math::Native), stamp(Math::Exact));
        assert!(stamp(Math::Native).starts_with(STAMP));
        assert_eq!(table_file(Math::Exact, "rlspv"), TABLE);
        assert_eq!(table_file(Math::Exact, "stamp"), STAMP_FILE);
        assert_eq!(table_file(Math::Native, "rlspi"), "table-native.rlspi");
        assert_eq!(host_modes("exact", false), [Math::Exact]);
        assert_eq!(host_modes("both", false), BOTH);
        assert_eq!(host_modes("exact", true), BOTH, "a package carries both");
    }

    #[test]
    fn a_native_build_writes_its_own_table_beside_the_exact_one() {
        let out = scratch("native");
        let dir = out.join(DIR);
        std::fs::create_dir_all(dir.join("programs")).unwrap();
        let code = words(&[exit(), bra_self()]);
        let key = relift_switch_formats::sha256(&code);
        relift_cache::write_if_changed(
            &dir.join("programs").join(format!("{}.bin", hex(&key))),
            &code,
        )
        .unwrap();
        relift_cache::write_if_changed(&dir.join("device.json"), CAPS.as_bytes()).unwrap();
        build(&out, 1, &[Math::Exact]).unwrap();
        assert_eq!(stamp_of_mode(&dir, Math::Native), None);
        ensure_native(&out, 1).unwrap();
        assert_eq!(stamp_of(&dir).as_deref(), Some(STAMP));
        assert_eq!(stamp_of_mode(&dir, Math::Native), Some(stamp(Math::Native)));
        assert!(dir.join("failed-native.txt").is_file());
        std::fs::remove_dir_all(&out).unwrap();
    }

    #[test]
    fn a_build_without_a_device_has_an_empty_table() {
        let out = scratch("nodevice");
        let key = relift_switch_formats::sha256(b"x");
        std::fs::create_dir_all(out.join(DIR).join("programs")).unwrap();
        relift_cache::write_if_changed(
            &out.join(DIR)
                .join("programs")
                .join(format!("{}.bin", hex(&key))),
            b"x",
        )
        .unwrap();
        let built = build(&out, 2, &[Math::Exact]).unwrap();
        assert_eq!((built.programs, built.translated), (1, 0));
        assert!(tables(&out).is_empty());
        std::fs::remove_dir_all(&out).unwrap();
    }

    #[test]
    fn the_scan_of_an_import_needs_nothing_a_run_recorded() {
        let out = scratch("discover");
        let import = out.join("import");
        let program = words(&[exit(), bra_self()]);
        let mut data = vec![0x80u8; 100];
        data.extend(zlib_stored(&program));
        let img = relift_switch_formats::romfs::build(&[("any/file.dat", &data)]);
        std::fs::create_dir_all(&import).unwrap();
        relift_cache::write_if_changed(&import.join("romfs.img"), &img).unwrap();
        let found = discover(&import, &out).unwrap();
        assert_eq!(found.programs, 1);
        let key = relift_switch_formats::sha256(&program);
        let bin = out
            .join(DIR)
            .join("programs")
            .join(format!("{}.bin", hex(&key)));
        assert_eq!(std::fs::read(bin).unwrap(), program);
        assert_eq!(program_keys(&out.join(DIR)).unwrap(), [key]);
        std::fs::remove_dir_all(&out).unwrap();
    }

    #[test]
    fn a_c_string_escapes_quotes_and_bytes_outside_ascii() {
        assert_eq!(c_string("a\"b\\é"), "\"a\\\"b\\\\\\303\\251\"");
    }
}
