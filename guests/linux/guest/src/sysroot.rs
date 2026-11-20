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

/// The files of a program besides its executable.
#[derive(Clone, Debug, Default)]
pub struct Closure {
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

fn candidates(nodes: &[Node], at: usize, name: &str, conf: &[String]) -> Vec<String> {
    if name.contains('/') {
        return vec![name.to_string()];
    }
    let mut dirs = rpath_chain(nodes, Some(at));
    dirs.extend(run_path(&nodes[at]));
    dirs.extend(conf.iter().cloned());
    dirs.extend(DEFAULT_DIRS.iter().map(|d| (*d).to_string()));
    dirs.iter()
        .map(|d| format!("{}/{name}", d.trim_end_matches('/')))
        .collect()
}

/// The library names a file spells out: strings of its bytes that name a
/// shared object (`libfoo.so`, `libfoo.so.1`, or such a name after a
/// directory), the names a program hands to `dlopen`.
fn spelled_libraries(bytes: &[u8]) -> BTreeSet<String> {
    let mut out = BTreeSet::new();
    for s in bytes.split(|&b| b == 0) {
        if s.len() < 4 || s.len() > 255 {
            continue;
        }
        let Ok(s) = std::str::from_utf8(s) else {
            continue;
        };
        if !s
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || b"_.+-/".contains(&c))
        {
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

struct Walk {
    out: Closure,
    libs: Vec<Library>,
    nodes: Vec<Node>,
    files: Vec<Vec<u8>>,
    seen_real: BTreeSet<String>,
    names: BTreeSet<String>,
    queue: std::collections::VecDeque<usize>,
}

impl Walk {
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
}
