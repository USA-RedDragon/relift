//! The C of every emitted function, kept between builds so the next build
//! emits again only the functions that changed ([`relift_emit::Store`]).
//!
//! One directory per module holds an index and frames: each frame is the
//! zstd-compressed C of the functions of one shard window, under a name no
//! earlier build used. A build reads the frames it needs as it goes,
//! keeps a frame whose functions all came back unchanged as it is, and
//! writes the others; frames no index names are removed.

use std::collections::VecDeque;
use std::collections::{BTreeSet, HashMap};
use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Condvar, Mutex, PoisonError};
use std::thread::JoinHandle;

use relift_emit::Key;
use relift_ir::{BinError, Reader, put_str, put_uvar};

use crate::import::BoxError;

const MAGIC: &[u8; 4] = b"RLES";
const FORMAT: u32 = 1;
const INDEX: &str = "index.zst";
const WORKERS: usize = 8;

/// One frame of an index.
struct Frame {
    name: String,
    last: u64,
    entries: Vec<(Key, usize, usize)>,
}

/// A function kept in the frame being filled.
struct Kept {
    entry: u64,
    key: Key,
    bytes: Vec<u8>,
    from: Option<(usize, usize)>,
}

/// A frame's slot in the index and its written name, or why it was not.
type Named = (usize, Result<String, String>);

struct Job {
    slot: usize,
    name: String,
    content: Vec<Vec<u8>>,
}

/// The frames waiting to be compressed: at most [`WORKERS`], so a build
/// that emits faster than it compresses holds a bounded amount of C.
struct Queue {
    state: Mutex<(VecDeque<Job>, bool)>,
    more: Condvar,
    room: Condvar,
}

impl Queue {
    fn new() -> Queue {
        Queue {
            state: Mutex::new((VecDeque::new(), false)),
            more: Condvar::new(),
            room: Condvar::new(),
        }
    }

    fn push(&self, job: Job) {
        let mut st = lock(&self.state);
        while st.0.len() >= WORKERS {
            st = self.room.wait(st).unwrap_or_else(PoisonError::into_inner);
        }
        st.0.push_back(job);
        self.more.notify_one();
    }

    fn close(&self) {
        lock(&self.state).1 = true;
        self.more.notify_all();
    }

    fn pop(&self) -> Option<Job> {
        let mut st = lock(&self.state);
        loop {
            if let Some(j) = st.0.pop_front() {
                self.room.notify_one();
                return Some(j);
            }
            if st.1 {
                return None;
            }
            st = self.more.wait(st).unwrap_or_else(PoisonError::into_inner);
        }
    }
}

/// What a build has kept so far.
struct Build {
    window: Option<u64>,
    cur: Vec<Kept>,
    frames: Vec<Frame>,
    reused: usize,
    total: usize,
}

/// How many functions a build took from the store.
#[derive(Clone, Copy, Debug, Default)]
pub struct Counts {
    /// Functions whose C came from the store.
    pub reused: usize,
    /// Every function kept.
    pub total: usize,
}

/// The store of one module.
pub struct EmitStore {
    dir: PathBuf,
    tool: [u8; 32],
    base: u64,
    window: u64,
    /// Names this build's new frames apart from every earlier one.
    build_id: String,
    old: Vec<Frame>,
    by_key: HashMap<Key, (usize, usize)>,
    loaded: Mutex<HashMap<usize, Arc<Vec<u8>>>>,
    build: Mutex<Build>,
    names: Arc<Mutex<Vec<Named>>>,
    queue: Arc<Queue>,
    workers: Vec<JoinHandle<()>>,
}

fn lock<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    m.lock().unwrap_or_else(PoisonError::into_inner)
}

/// A name no earlier build used: the time and the process.
fn build_id() -> String {
    let t = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| d.as_nanos());
    format!("{t:x}_{:x}", std::process::id())
}

fn read_index(bytes: &[u8], tool: &[u8; 32], window: u64) -> Result<Vec<Frame>, BinError> {
    let mut r = Reader::new(bytes);
    if r.bytes(4)? != MAGIC || r.u32()? != FORMAT || r.bytes(32)? != tool || r.uvar()? != window {
        return Err(BinError("another store"));
    }
    let mut frames = Vec::new();
    for _ in 0..r.count()? {
        let name = r.str()?.to_string();
        let last = r.uvar()?;
        let n = r.count()?;
        let mut entries = Vec::with_capacity(n);
        for _ in 0..n {
            let key: Key = r.bytes(32)?.try_into().map_err(|_| BinError("bad key"))?;
            let off = usize::try_from(r.uvar()?).map_err(|_| BinError("bad offset"))?;
            let len = usize::try_from(r.uvar()?).map_err(|_| BinError("bad length"))?;
            entries.push((key, off, len));
        }
        frames.push(Frame {
            name,
            last,
            entries,
        });
    }
    if !r.is_empty() {
        return Err(BinError("trailing bytes"));
    }
    Ok(frames)
}

fn write_frame(dir: &Path, name: String, content: &[Vec<u8>]) -> Result<String, String> {
    let path = dir.join(format!("{name}.zst"));
    let mut enc = zstd::stream::Encoder::new(Vec::new(), 1).map_err(|e| e.to_string())?;
    for piece in content {
        enc.write_all(piece).map_err(|e| e.to_string())?;
    }
    let z = enc.finish().map_err(|e| e.to_string())?;
    relift_cache::write_if_changed(&path, &z).map_err(|e| format!("{}: {e}", path.display()))?;
    Ok(name)
}

impl EmitStore {
    /// The store in `dir` for a module at `base` emitted in shard windows
    /// of `window` bytes by the relift whose hash is `tool`; empty when
    /// there is none or it was kept by another relift.
    ///
    /// # Errors
    ///
    /// The directory cannot be made.
    pub fn open(dir: &Path, tool: [u8; 32], base: u64, window: u64) -> Result<EmitStore, BoxError> {
        std::fs::create_dir_all(dir)?;
        let old = std::fs::read(dir.join(INDEX))
            .ok()
            .and_then(|z| zstd::decode_all(z.as_slice()).ok())
            .and_then(|b| read_index(&b, &tool, window).ok())
            .unwrap_or_default();
        let mut by_key = HashMap::new();
        for (f, fr) in old.iter().enumerate() {
            for (k, e) in fr.entries.iter().enumerate() {
                by_key.insert(e.0, (f, k));
            }
        }
        let queue = Arc::new(Queue::new());
        let names = Arc::new(Mutex::new(Vec::new()));
        let workers = (0..WORKERS)
            .map(|_| {
                let queue = Arc::clone(&queue);
                let names = Arc::clone(&names);
                let dir = dir.to_path_buf();
                std::thread::spawn(move || {
                    while let Some(job) = queue.pop() {
                        let r = write_frame(&dir, job.name, &job.content);
                        lock(&names).push((job.slot, r));
                    }
                })
            })
            .collect();
        Ok(EmitStore {
            dir: dir.to_path_buf(),
            tool,
            base,
            window: window.max(1),
            build_id: build_id(),
            old,
            by_key,
            loaded: Mutex::new(HashMap::new()),
            build: Mutex::new(Build {
                window: None,
                cur: Vec::new(),
                frames: Vec::new(),
                reused: 0,
                total: 0,
            }),
            names,
            queue,
            workers,
        })
    }

    fn frame(&self, f: usize) -> Option<Arc<Vec<u8>>> {
        if let Some(b) = lock(&self.loaded).get(&f) {
            return Some(Arc::clone(b));
        }
        let path = self.dir.join(format!("{}.zst", self.old[f].name));
        let z = std::fs::read(path).ok()?;
        let b = Arc::new(zstd::decode_all(z.as_slice()).ok()?);
        lock(&self.loaded).insert(f, Arc::clone(&b));
        Some(b)
    }

    /// Closes the frame being filled.
    fn close(&self, b: &mut Build) {
        let cur = std::mem::take(&mut b.cur);
        let Some(first) = cur.first() else { return };
        let last = cur.last().map_or(first.entry, |k| k.entry);
        let same = first.from.and_then(|(f, _)| {
            let whole = cur.len() == self.old[f].entries.len()
                && cur.iter().enumerate().all(|(i, k)| k.from == Some((f, i)));
            whole.then_some(f)
        });
        if let Some(f) = same {
            let old = &self.old[f];
            b.frames.push(Frame {
                name: old.name.clone(),
                last,
                entries: old.entries.clone(),
            });
            return;
        }
        let mut content = Vec::with_capacity(cur.len());
        let mut entries = Vec::with_capacity(cur.len());
        let mut at = 0;
        for k in cur {
            entries.push((k.key, at, k.bytes.len()));
            at += k.bytes.len();
            content.push(k.bytes);
        }
        let slot = b.frames.len();
        b.frames.push(Frame {
            name: String::new(),
            last,
            entries,
        });
        let name = format!("{}_{slot}", self.build_id);
        self.queue.push(Job {
            slot,
            name,
            content,
        });
    }

    /// Writes the index of this build and removes the frames it no longer
    /// names. Returns how many functions came from the store.
    ///
    /// # Errors
    ///
    /// A frame or the index cannot be written.
    pub fn finish(self) -> Result<Counts, BoxError> {
        let mut b = std::mem::replace(
            &mut *lock(&self.build),
            Build {
                window: None,
                cur: Vec::new(),
                frames: Vec::new(),
                reused: 0,
                total: 0,
            },
        );
        self.close(&mut b);
        self.queue.close();
        for w in self.workers {
            w.join().map_err(|_| "an emit store worker panicked")?;
        }
        for (slot, name) in std::mem::take(&mut *lock(&self.names)) {
            b.frames[slot].name = name?;
        }
        let mut out = MAGIC.to_vec();
        put_uvar(&mut out, u64::from(FORMAT));
        out.extend_from_slice(&self.tool);
        put_uvar(&mut out, self.window);
        put_uvar(&mut out, b.frames.len() as u64);
        for f in &b.frames {
            put_str(&mut out, &f.name);
            put_uvar(&mut out, f.last);
            put_uvar(&mut out, f.entries.len() as u64);
            for (key, off, len) in &f.entries {
                out.extend_from_slice(key);
                put_uvar(&mut out, *off as u64);
                put_uvar(&mut out, *len as u64);
            }
        }
        relift_cache::write_if_changed(
            &self.dir.join(INDEX),
            &zstd::encode_all(out.as_slice(), 3)?,
        )?;
        let named: BTreeSet<String> = b.frames.iter().map(|f| format!("{}.zst", f.name)).collect();
        for e in std::fs::read_dir(&self.dir)?.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if n != INDEX && !named.contains(&n) {
                let _ = std::fs::remove_file(e.path());
            }
        }
        Ok(Counts {
            reused: b.reused,
            total: b.total,
        })
    }
}

impl relift_emit::Store for EmitStore {
    fn key(&self, input: &[u8]) -> Key {
        use sha2::{Digest as _, Sha256};
        let mut h = Sha256::new();
        h.update(self.tool);
        h.update(input);
        h.finalize().into()
    }

    fn get(&self, key: &Key) -> Option<Vec<u8>> {
        let &(f, k) = self.by_key.get(key)?;
        let (_, off, len) = self.old[f].entries[k];
        self.frame(f)?.get(off..off + len).map(<[u8]>::to_vec)
    }

    fn keep(&self, entry: u64, key: &Key, output: Vec<u8>, reused: bool) {
        let mut b = lock(&self.build);
        let w = entry.saturating_sub(self.base) / self.window;
        if b.window != Some(w) {
            self.close(&mut b);
            b.window = Some(w);
            lock(&self.loaded).retain(|&f, _| self.old[f].last >= entry);
        }
        b.total += 1;
        b.reused += usize::from(reused);
        let from = if reused {
            self.by_key.get(key).copied()
        } else {
            None
        };
        b.cur.push(Kept {
            entry,
            key: *key,
            bytes: output,
            from,
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use relift_emit::Store;

    fn frames(dir: &Path) -> BTreeSet<String> {
        std::fs::read_dir(dir)
            .unwrap()
            .flatten()
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .filter(|n| n != INDEX)
            .collect()
    }

    fn build(dir: &Path, fns: &[(u64, &[u8])]) -> (Vec<Option<Vec<u8>>>, Counts) {
        let s = EmitStore::open(dir, [1; 32], 0x1000, 0x100).unwrap();
        let mut got = Vec::new();
        for &(entry, out) in fns {
            let key = s.key(out);
            let old = s.get(&key);
            let reused = old.as_deref() == Some(out);
            got.push(old);
            s.keep(entry, &key, out.to_vec(), reused);
        }
        (got, s.finish().unwrap())
    }

    #[test]
    fn unchanged_windows_keep_their_frames() {
        let dir = std::env::temp_dir().join(format!("relift-emitstore-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let fns: [(u64, &[u8]); 3] = [(0x1000, b"a"), (0x1010, b"b"), (0x1200, b"c")];
        let (got, n) = build(&dir, &fns);
        assert!(got.iter().all(Option::is_none));
        assert_eq!((n.reused, n.total), (0, 3));
        let first = frames(&dir);
        assert_eq!(first.len(), 2);
        let (got, n) = build(&dir, &fns);
        assert_eq!(got[2].as_deref(), Some(&b"c"[..]));
        assert_eq!((n.reused, n.total), (3, 3));
        assert_eq!(frames(&dir), first);
        let changed: [(u64, &[u8]); 3] = [(0x1000, b"a"), (0x1010, b"B"), (0x1200, b"c")];
        let (_, n) = build(&dir, &changed);
        assert_eq!(n.reused, 2);
        let now = frames(&dir);
        assert_eq!(now.len(), 2);
        assert_eq!(now.intersection(&first).count(), 1);
        let s = EmitStore::open(&dir, [2; 32], 0x1000, 0x100).unwrap();
        assert!(
            s.get(&s.key(b"a")).is_none(),
            "another relift reads nothing"
        );
        drop(s);
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
