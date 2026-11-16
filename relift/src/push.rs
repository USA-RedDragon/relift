//! `relift push`: copies a project's game data (the imported romfs and DLC)
//! onto a phone where the app built by `relift build --target <t>` reads it,
//! and checks every file against the data manifest (crate::datamanifest).
//!
//! - iOS: into the app's Documents (`romfs/`, `dlc/`) with `xcrun devicectl`,
//!   one folder of at most [`CHUNK`] files per copy, since one copy of a
//!   large tree can end early without an error. The device's file list is
//!   then compared with the manifest, missing or short files are copied
//!   again, and what is still wrong fails the push, naming the first file.
//! - Android: into the app's external files folder with `adb push`, checked
//!   the same way with `stat` on the device, one stream of names through
//!   `xargs` (one `find -exec ... +` overflows the device shell's argument
//!   list on a large tree). `adb push` makes its folders the shell's, mode
//!   2770, which the app cannot enter, so every folder under the data that
//!   the app does not own is opened to all (`a+rx`; the files are already
//!   readable by all) and checked again.
//!
//! The app checks the data against the same manifest at start, so data a
//! push did not finish never runs.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use crate::datamanifest::{self, Entry};
use crate::import::BoxError;
use crate::settings::Settings;
use crate::ui::Ui;
use relift_progress::Line;

/// The most files one device copy carries.
pub const CHUNK: usize = 1500;
const ROUNDS: usize = 3;

/// The folders of `root` (relative to `base`) to copy one at a time so that
/// none holds more than `limit` files, and the loose files of folders that
/// had to be split.
fn chunks(base: &Path, rel: &str, limit: usize) -> std::io::Result<(Vec<String>, Vec<String>)> {
    let dir = base.join(rel);
    let mut n = 0usize;
    let mut stack = vec![dir.clone()];
    while let Some(d) = stack.pop() {
        for e in std::fs::read_dir(&d)? {
            let e = e?;
            if e.file_type()?.is_dir() {
                stack.push(e.path());
            } else {
                n += 1;
            }
        }
    }
    let mut kids = Vec::new();
    let mut loose = Vec::new();
    for e in std::fs::read_dir(&dir)? {
        let e = e?;
        let name = e.file_name().to_string_lossy().into_owned();
        if e.file_type()?.is_dir() {
            kids.push(format!("{rel}/{name}"));
        } else {
            loose.push(format!("{rel}/{name}"));
        }
    }
    if n <= limit || kids.is_empty() {
        return Ok((vec![rel.to_string()], Vec::new()));
    }
    kids.sort();
    loose.sort();
    let mut dirs = Vec::new();
    for k in kids {
        let (d, l) = chunks(base, &k, limit)?;
        dirs.extend(d);
        loose.extend(l);
    }
    Ok((dirs, loose))
}

/// A phone the data goes to.
trait Device {
    fn copy(&self, local: &Path, remote: &str) -> Result<(), BoxError>;
    /// Every file under the data folders, by manifest name, with its size.
    fn sizes(&self) -> Result<BTreeMap<String, u64>, BoxError>;
    /// Makes the copied data readable by the app; the folders it still
    /// cannot read, by manifest name.
    fn open_up(&self) -> Result<Vec<String>, BoxError> {
        Ok(Vec::new())
    }
}

struct Ios {
    device: String,
    bundle: String,
    work: PathBuf,
}

impl Ios {
    fn base(&self) -> Vec<String> {
        [
            "--device",
            &self.device,
            "--domain-type",
            "appDataContainer",
            "--domain-identifier",
            &self.bundle,
        ]
        .map(String::from)
        .into()
    }
}

fn run(cmd: &mut std::process::Command, what: &str) -> Result<std::process::Output, BoxError> {
    let out = cmd.output().map_err(|e| format!("{what}: {e}"))?;
    if !out.status.success() {
        let err = String::from_utf8_lossy(&out.stderr);
        let tail: String = err
            .chars()
            .rev()
            .take(600)
            .collect::<String>()
            .chars()
            .rev()
            .collect();
        return Err(format!("{what} failed: {tail}").into());
    }
    Ok(out)
}

fn walk_json<'a>(v: &'a serde_json::Value, out: &mut Vec<&'a serde_json::Value>) {
    match v {
        serde_json::Value::Object(m) => {
            out.push(v);
            for x in m.values() {
                walk_json(x, out);
            }
        }
        serde_json::Value::Array(a) => {
            for x in a {
                walk_json(x, out);
            }
        }
        _ => {}
    }
}

/// The files of a `devicectl device info files --json-output` listing of
/// `Documents/<root>`, named `<root>/<path>`.
fn ios_listing(json: &str, root: &str) -> Result<BTreeMap<String, u64>, BoxError> {
    let v: serde_json::Value = serde_json::from_str(json)?;
    let mut all = Vec::new();
    walk_json(&v, &mut all);
    let mut out = BTreeMap::new();
    for o in all {
        let (Some(rel), Some(dir), Some(size)) = (
            o.get("relativePath").and_then(|x| x.as_str()),
            o.pointer("/resources/isDirectory")
                .and_then(serde_json::Value::as_bool),
            o.pointer("/metadata/size")
                .and_then(serde_json::Value::as_u64),
        ) else {
            continue;
        };
        if !dir {
            out.insert(format!("{root}/{rel}"), size);
        }
    }
    Ok(out)
}

impl Device for Ios {
    fn copy(&self, local: &Path, remote: &str) -> Result<(), BoxError> {
        run(
            relift_proc::command("xcrun")
                .args(["devicectl", "device", "copy", "to"])
                .args(self.base())
                .arg("--source")
                .arg(local)
                .args(["--destination", &format!("Documents/{remote}")]),
            "devicectl copy",
        )?;
        Ok(())
    }

    fn sizes(&self) -> Result<BTreeMap<String, u64>, BoxError> {
        let mut out = BTreeMap::new();
        for root in ["romfs", "dlc"] {
            let json = self.work.join(format!("push-{root}.json"));
            let _ = std::fs::remove_file(&json);
            let _ = relift_proc::command("xcrun")
                .args(["devicectl", "device", "info", "files"])
                .args(self.base())
                .args([
                    "--subdirectory",
                    &format!("Documents/{root}"),
                    "--json-output",
                ])
                .arg(&json)
                .output();
            if let Ok(text) = std::fs::read_to_string(&json) {
                out.extend(ios_listing(&text, root)?);
            }
        }
        Ok(out)
    }
}

struct Android {
    adb: PathBuf,
    serial: Option<String>,
    dir: String,
}

impl Android {
    fn adb(&self) -> std::process::Command {
        let mut c = relift_proc::command(&self.adb);
        if let Some(s) = &self.serial {
            c.args(["-s", s]);
        }
        c
    }
}

/// The `<size> <path>` lines `stat -c '%s %n'` prints, named relative to `dir`.
fn android_listing(text: &str, dir: &str) -> BTreeMap<String, u64> {
    let prefix = format!("{}/", dir.trim_end_matches('/'));
    text.lines()
        .filter_map(|l| {
            let (size, path) = l.split_once(' ')?;
            Some((path.strip_prefix(&prefix)?.to_string(), size.parse().ok()?))
        })
        .collect()
}

impl Device for Android {
    fn copy(&self, local: &Path, remote: &str) -> Result<(), BoxError> {
        let dst = format!("{}/{remote}", self.dir);
        let parent = dst
            .rsplit_once('/')
            .map_or(dst.as_str(), |(p, _)| p)
            .to_string();
        run(
            self.adb().args(["shell", "mkdir", "-p", &parent]),
            "adb shell mkdir",
        )?;
        run(self.adb().arg("push").arg(local).arg(&parent), "adb push")?;
        Ok(())
    }

    fn sizes(&self) -> Result<BTreeMap<String, u64>, BoxError> {
        let out = run(
            self.adb()
                .args(["shell", &android_sizes_command(&self.dir)]),
            "adb shell stat",
        )?;
        Ok(android_listing(
            &String::from_utf8_lossy(&out.stdout),
            &self.dir,
        ))
    }

    fn open_up(&self) -> Result<Vec<String>, BoxError> {
        run(
            self.adb().args(["shell", &android_open_command(&self.dir)]),
            "adb shell chmod",
        )?;
        let out = run(
            self.adb()
                .args(["shell", &android_closed_command(&self.dir)]),
            "adb shell find",
        )?;
        Ok(android_names(
            &String::from_utf8_lossy(&out.stdout),
            &self.dir,
        ))
    }
}

/// The paths of `text`, one per line, named relative to `dir`.
fn android_names(text: &str, dir: &str) -> Vec<String> {
    let prefix = format!("{}/", dir.trim_end_matches('/'));
    text.lines()
        .filter_map(|l| l.strip_prefix(&prefix).map(String::from))
        .collect()
}

/// The data folders under `dir` that exist, for a device shell `for` list.
fn android_roots(dir: &str) -> String {
    format!("'{dir}'/romfs '{dir}'/dlc")
}

/// The device command that prints `<size> <path>` for every data file: one
/// `find` streams the names to `xargs`, which runs `stat` on as many as fit
/// in one argument list at a time.
fn android_sizes_command(dir: &str) -> String {
    format!(
        "for r in {}; do [ -d \"$r\" ] || continue; find \"$r\" -type f -print0 | xargs -0 -r stat -c '%s %n' || exit 1; done",
        android_roots(dir)
    )
}

/// The device command that opens to all every data folder the app (the
/// owner of `dir`) does not own and that is not yet open.
fn android_open_command(dir: &str) -> String {
    format!(
        "u=$(stat -c %u '{dir}') || exit 1; for r in {}; do [ -d \"$r\" ] || continue; find \"$r\" -type d ! -user \"$u\" ! -perm -005 -print0 | xargs -0 -r chmod a+rx || exit 1; done",
        android_roots(dir)
    )
}

/// The device command that prints the data folders the app still cannot
/// enter: owned by another user and closed to others.
fn android_closed_command(dir: &str) -> String {
    format!(
        "u=$(stat -c %u '{dir}') || exit 1; for r in {}; do [ -d \"$r\" ] || continue; find \"$r\" -type d ! -user \"$u\" ! -perm -005 || exit 1; done",
        android_roots(dir)
    )
}

fn first_bad(bad: &[(&Entry, Option<u64>)]) -> String {
    match bad.first() {
        Some((e, None)) => format!("{} is missing", e.name),
        Some((e, Some(s))) => format!("{} is {s} bytes, expected {}", e.name, e.size),
        None => String::new(),
    }
}

/// Opens the data on `dev` to the app, failing on a folder it still cannot
/// enter.
fn opened(dev: &dyn Device) -> Result<(), BoxError> {
    let closed = dev.open_up()?;
    match closed.first() {
        None => Ok(()),
        Some(first) => Err(format!(
            "{} data folders stay closed to the app after chmod; first: {first}",
            closed.len()
        )
        .into()),
    }
}

fn push_to(
    dev: &dyn Device,
    import: &Path,
    entries: &[Entry],
    ui: &mut Ui,
) -> Result<(), BoxError> {
    let mut have = dev.sizes()?;
    for round in 0..ROUNDS {
        let bad = datamanifest::mismatches(entries, &have);
        if bad.is_empty() {
            return opened(dev);
        }
        let mut dirty: Vec<String> = Vec::new();
        for root in ["romfs", "dlc"] {
            if !import.join(root).is_dir() {
                continue;
            }
            let (dirs, loose) = chunks(import, root, CHUNK)?;
            for d in dirs {
                if bad
                    .iter()
                    .any(|(e, _)| e.name.starts_with(&format!("{d}/")))
                {
                    dirty.push(d);
                }
            }
            for f in loose {
                if bad.iter().any(|(e, _)| e.name == f) {
                    dirty.push(f);
                }
            }
        }
        ui.note(&Line::new().text(format!(
            "push round {}: {} files differ, copying {} folders",
            round + 1,
            bad.len(),
            dirty.len()
        )));
        for d in &dirty {
            dev.copy(&import.join(d), d)?;
        }
        have = dev.sizes()?;
    }
    let bad = datamanifest::mismatches(entries, &have);
    if bad.is_empty() {
        return opened(dev);
    }
    Err(format!(
        "{} of {} files still differ after {ROUNDS} copies; first: {}",
        bad.len(),
        entries.len(),
        first_bad(&bad)
    )
    .into())
}

/// Pushes the project's game data to `target`'s app on `device` (default:
/// the one connected device).
///
/// # Errors
///
/// No data, no device, a failed copy or data that still differs from the
/// manifest after the copies.
pub fn run_push(
    settings: &Settings,
    target: &str,
    device: Option<&str>,
    ui: &mut Ui,
) -> Result<(), BoxError> {
    let import = crate::build::import_dir(settings)?;
    let (name, out) = crate::build::project_out(settings)?;
    let work = out.join(target);
    std::fs::create_dir_all(&work)?;
    let row = ui.row("");
    ui.begin(row, "push");
    let entries = datamanifest::entries(&import, &work)?;
    if entries.is_empty() {
        return Err(format!("{}: no romfs or dlc folder to push", import.display()).into());
    }
    let bytes: u64 = entries.iter().map(|e| e.size).sum();
    let what = match crate::cross::target(target)? {
        Some(crate::cross::Target::Ios(_)) => {
            let bundle = if settings.package.ios_id.is_empty() {
                crate::ios::default_bundle_id(&name)
            } else {
                settings.package.ios_id.clone()
            };
            let device = match device {
                Some(d) => d.to_string(),
                None => ios_device(&work)?,
            };
            let dev = Ios {
                device,
                bundle: bundle.clone(),
                work: work.clone(),
            };
            push_to(&dev, &import, &entries, ui)?;
            format!("{bundle} Documents")
        }
        Some(crate::cross::Target::Android(_)) => {
            let package = if settings.package.android_id.is_empty() {
                crate::android::default_package(&name)
            } else {
                settings.package.android_id.clone()
            };
            let dir = format!("/sdcard/Android/data/{package}/files");
            let dev = Android {
                adb: adb(settings),
                serial: device.map(String::from),
                dir: dir.clone(),
            };
            push_to(&dev, &import, &entries, ui)?;
            dir
        }
        None => return Err("relift push needs --target ios-arm64 or android-arm64".into()),
    };
    ui.done(
        row,
        Line::new().text(format!(
            "{} files, {} MiB in {what}, all sizes match the manifest",
            entries.len(),
            bytes >> 20
        )),
    );
    Ok(())
}

fn adb(settings: &Settings) -> PathBuf {
    let sdk = &settings.toolchain.android_sdk;
    let p = sdk.join("platform-tools").join("adb");
    if !sdk.as_os_str().is_empty() && p.is_file() {
        return p;
    }
    std::env::var_os("ANDROID_HOME")
        .map(|h| PathBuf::from(h).join("platform-tools").join("adb"))
        .filter(|p| p.is_file())
        .unwrap_or_else(|| PathBuf::from("adb"))
}

/// The identifier of the one connected iOS device.
fn ios_device(work: &Path) -> Result<String, BoxError> {
    let json = work.join("push-devices.json");
    let _ = std::fs::remove_file(&json);
    run(
        relift_proc::command("xcrun")
            .args(["devicectl", "list", "devices", "--json-output"])
            .arg(&json),
        "devicectl list devices",
    )?;
    let v: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(&json)?)?;
    let ids: Vec<String> = v
        .pointer("/result/devices")
        .and_then(serde_json::Value::as_array)
        .into_iter()
        .flatten()
        .filter(|d| {
            d.pointer("/connectionProperties/tunnelState")
                .and_then(serde_json::Value::as_str)
                .is_some_and(|s| s != "unavailable")
        })
        .filter_map(|d| {
            d.get("identifier")
                .and_then(serde_json::Value::as_str)
                .map(String::from)
        })
        .collect();
    match ids.as_slice() {
        [one] => Ok(one.clone()),
        [] => Err("no iOS device connected (xcrun devicectl list devices)".into()),
        _ => Err(format!(
            "{} iOS devices connected: pick one with --device",
            ids.len()
        )
        .into()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ios_listing_keeps_files_named_under_their_root() {
        let j = r#"{"result":{"files":[
            {"relativePath":"as","resources":{"isDirectory":true},"metadata":{"size":96}},
            {"relativePath":"as/boot.asb","resources":{"isDirectory":false},"metadata":{"size":30864}}]}}"#;
        let m = ios_listing(j, "romfs").unwrap();
        assert_eq!(m.len(), 1);
        assert_eq!(m["romfs/as/boot.asb"], 30864);
    }

    #[test]
    fn android_listing_strips_the_data_folder() {
        let m = android_listing(
            "3 /sdcard/x/files/romfs/a\n2 /elsewhere/b\n",
            "/sdcard/x/files",
        );
        assert_eq!(m.len(), 1);
        assert_eq!(m["romfs/a"], 3);
    }

    #[test]
    fn android_sizes_stream_names_through_xargs() {
        let cmd = android_sizes_command("/sdcard/x/files");
        assert!(!cmd.contains("-exec"), "{cmd}");
        assert!(cmd.contains("-print0 | xargs -0 -r stat"), "{cmd}");
    }

    /// The device's `stat -c` and `xargs -r` are toybox's, which GNU's match;
    /// the BSD tools of macOS lack them.
    #[cfg(target_os = "linux")]
    #[test]
    fn android_sizes_list_every_file_of_a_large_tree() {
        let d = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("..")
            .join("target")
            .join(format!("relift-push-sizes-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(d.join("romfs/a b")).unwrap();
        for i in 0..3000 {
            relift_cache::write_if_changed(&d.join(format!("romfs/a b/file {i:05}")), b"xy")
                .unwrap();
        }
        relift_cache::write_if_changed(&d.join("romfs/top"), b"xyz").unwrap();
        let dir = d.to_str().unwrap();
        let out = relift_proc::command("sh")
            .arg("-c")
            .arg(android_sizes_command(dir))
            .output()
            .unwrap();
        assert!(
            out.status.success(),
            "{}",
            String::from_utf8_lossy(&out.stderr)
        );
        let m = android_listing(&String::from_utf8_lossy(&out.stdout), dir);
        assert_eq!(m.len(), 3001);
        assert_eq!(m["romfs/a b/file 02999"], 2);
        assert_eq!(m["romfs/top"], 3);
        std::fs::remove_dir_all(&d).unwrap();
    }

    #[test]
    fn android_open_up_only_touches_folders_the_app_does_not_own() {
        for cmd in [
            android_open_command("/sdcard/x/files"),
            android_closed_command("/sdcard/x/files"),
        ] {
            assert!(
                cmd.starts_with("u=$(stat -c %u '/sdcard/x/files')"),
                "{cmd}"
            );
            assert!(cmd.contains("-type d ! -user \"$u\" ! -perm -005"), "{cmd}");
            assert!(!cmd.contains("-exec"), "{cmd}");
        }
        assert!(android_open_command("/d").contains("xargs -0 -r chmod a+rx"));
        assert_eq!(
            android_names("/sdcard/x/files/romfs/a\n/elsewhere/b\n", "/sdcard/x/files"),
            ["romfs/a"]
        );
    }

    struct Fake {
        files: BTreeMap<String, u64>,
        closed: Vec<String>,
        opened: std::cell::Cell<usize>,
    }

    impl Device for Fake {
        fn copy(&self, _: &Path, _: &str) -> Result<(), BoxError> {
            Ok(())
        }
        fn sizes(&self) -> Result<BTreeMap<String, u64>, BoxError> {
            Ok(self.files.clone())
        }
        fn open_up(&self) -> Result<Vec<String>, BoxError> {
            self.opened.set(self.opened.get() + 1);
            Ok(self.closed.clone())
        }
    }

    #[test]
    fn a_push_whose_files_match_still_opens_the_folders_and_fails_on_closed_ones() {
        let entries = [Entry {
            name: "romfs/a".into(),
            size: 3,
            sha256: String::new(),
        }];
        let mut ui = relift_progress::Reporter::new(
            std::io::stderr(),
            Box::new(relift_progress::ManualClock::new()),
            relift_progress::Mode::Plain,
            false,
            80,
            relift_progress::Verbosity::Quiet,
        );
        let ok = Fake {
            files: [("romfs/a".to_string(), 3)].into(),
            closed: Vec::new(),
            opened: 0.into(),
        };
        push_to(&ok, Path::new("/nonexistent"), &entries, &mut ui).unwrap();
        assert_eq!(ok.opened.get(), 1);
        let closed = Fake {
            closed: vec!["romfs".into()],
            ..ok
        };
        let e = push_to(&closed, Path::new("/nonexistent"), &entries, &mut ui).unwrap_err();
        assert!(e.to_string().contains("first: romfs"), "{e}");
    }

    #[test]
    fn big_folders_split_into_chunks_with_their_loose_files() {
        let d = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("..")
            .join("target")
            .join(format!("relift-push-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        for (i, sub) in ["romfs/a", "romfs/b"].iter().enumerate() {
            std::fs::create_dir_all(d.join(sub)).unwrap();
            for f in 0..3 {
                relift_cache::write_if_changed(&d.join(sub).join(format!("{i}{f}")), b"x").unwrap();
            }
        }
        relift_cache::write_if_changed(&d.join("romfs/top"), b"x").unwrap();
        assert_eq!(
            chunks(&d, "romfs", 10).unwrap(),
            (vec!["romfs".into()], vec![])
        );
        let (dirs, loose) = chunks(&d, "romfs", 4).unwrap();
        assert_eq!(dirs, ["romfs/a", "romfs/b"]);
        assert_eq!(loose, ["romfs/top"]);
        std::fs::remove_dir_all(&d).unwrap();
    }
}
