//! The NVN programs a table is translated from, in a folder or in one file
//! (the program pack of a Switch 2 game).
//!
//! A build keeps them as `programs/<key>.bin` with the scan's `meshes.json`
//! beside (see [`Programs::dir`]). A package carries them as one pack file
//! ([`PACK`]), all little-endian:
//!
//! * the magic `RLSPP001` (8 bytes), the program count and the length of
//!   the pipelines JSON in bytes (u32 each);
//! * per program, sorted by key: the key (32 bytes, the SHA-256 of the
//!   program's bytes), the offset of its zstd frame from the start of the
//!   file (u64), the frame's length and the program's length in bytes (u32
//!   each);
//! * the pipelines JSON (the scan's `meshes.json`);
//! * the frames, each at a 4-byte boundary.

use std::path::{Path, PathBuf};

use crate::{BoxError, hex, hex_key, sha256};

/// The pack's file name in a package's shader folder.
pub const PACK: &str = "programs.rlsp";

/// The scan's file of pipelines, in a build's shader folder.
pub const MESHES: &str = "meshes.json";

const MAGIC: &[u8; 8] = b"RLSPP001";
const HEAD: usize = 16;
const ENTRY: usize = 48;

/// Where the programs are.
enum Store {
    /// `programs/<key>.bin` files in a folder.
    Dir(PathBuf),
    /// A pack's bytes and, by key order, each frame's offset, frame length
    /// and program length.
    Pack(Vec<u8>, Vec<(usize, usize, usize)>),
}

/// The programs a table is translated from, and their pipelines.
pub struct Programs {
    keys: Vec<[u8; 32]>,
    meshes: Vec<u8>,
    store: Store,
}

fn u32_at(b: &[u8], at: usize) -> Option<usize> {
    Some(u32::from_le_bytes(b.get(at..at + 4)?.try_into().ok()?) as usize)
}

impl Programs {
    /// The programs of a build's shader folder `dir`: `dir`/programs and
    /// `dir`/meshes.json, either of which may be missing.
    ///
    /// # Errors
    ///
    /// The programs folder exists but cannot be read.
    pub fn dir(dir: &Path) -> Result<Programs, BoxError> {
        let mut keys = Vec::new();
        if let Ok(rd) = std::fs::read_dir(dir.join("programs")) {
            for e in rd {
                if let Some(key) = e?.file_name().to_str().and_then(hex_key) {
                    keys.push(key);
                }
            }
        }
        keys.sort_unstable();
        Ok(Programs {
            keys,
            meshes: std::fs::read(dir.join(MESHES)).unwrap_or_default(),
            store: Store::Dir(dir.join("programs")),
        })
    }

    /// No programs, and no pipelines.
    #[must_use]
    pub fn none() -> Programs {
        Programs {
            keys: Vec::new(),
            meshes: Vec::new(),
            store: Store::Pack(Vec::new(), Vec::new()),
        }
    }

    /// The programs of a pack's bytes.
    ///
    /// # Errors
    ///
    /// The bytes are not a pack, or an entry points past them.
    pub fn pack(bytes: Vec<u8>) -> Result<Programs, BoxError> {
        if bytes.get(..8) != Some(&MAGIC[..]) {
            return Err("not a program pack (no RLSPP001 magic)".into());
        }
        let short = || -> BoxError { "the program pack is cut short".into() };
        let n = u32_at(&bytes, 8).ok_or_else(short)?;
        let meshes_len = u32_at(&bytes, 12).ok_or_else(short)?;
        let meshes_at = HEAD + ENTRY * n;
        let meshes = bytes
            .get(meshes_at..meshes_at + meshes_len)
            .ok_or_else(short)?
            .to_vec();
        let mut keys = Vec::with_capacity(n);
        let mut at = Vec::with_capacity(n);
        for i in 0..n {
            let e = HEAD + ENTRY * i;
            let key: [u8; 32] = bytes[e..e + 32].try_into().expect("32 bytes");
            let off = u64::from_le_bytes(bytes[e + 32..e + 40].try_into().expect("8 bytes"));
            let off = usize::try_from(off).map_err(|_| short())?;
            let frame = u32_at(&bytes, e + 40).ok_or_else(short)?;
            let len = u32_at(&bytes, e + 44).ok_or_else(short)?;
            if off.checked_add(frame).is_none_or(|end| end > bytes.len()) {
                return Err(short());
            }
            keys.push(key);
            at.push((off, frame, len));
        }
        if keys.windows(2).any(|w| w[0] >= w[1]) {
            return Err("the program pack's keys are not sorted".into());
        }
        Ok(Programs {
            keys,
            meshes,
            store: Store::Pack(bytes, at),
        })
    }

    /// The programs at `path`: a pack file, or a build's shader folder.
    ///
    /// # Errors
    ///
    /// As [`Programs::dir`] and [`Programs::pack`].
    pub fn open(path: &Path) -> Result<Programs, BoxError> {
        if path.is_file() {
            Programs::pack(std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?)
        } else {
            Programs::dir(path)
        }
    }

    /// The programs' keys, sorted.
    #[must_use]
    pub fn keys(&self) -> &[[u8; 32]] {
        &self.keys
    }

    /// The scan's pipelines JSON (`meshes.json`), empty when there is none.
    #[must_use]
    pub fn meshes(&self) -> &[u8] {
        &self.meshes
    }

    /// The bytes of program `key`, or none when it is not here or its
    /// bytes do not hash to its key.
    ///
    /// # Errors
    ///
    /// Its file cannot be read, or its frame does not decompress.
    pub fn code(&self, key: &[u8; 32]) -> Result<Option<Vec<u8>>, BoxError> {
        let code = match &self.store {
            Store::Dir(dir) => match std::fs::read(dir.join(format!("{}.bin", hex(key)))) {
                Ok(c) => c,
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
                Err(e) => return Err(e.into()),
            },
            Store::Pack(bytes, at) => {
                let Ok(i) = self.keys.binary_search(key) else {
                    return Ok(None);
                };
                let (off, frame, len) = at[i];
                zstd::bulk::decompress(&bytes[off..off + frame], len)?
            }
        };
        Ok((sha256(&code) == *key).then_some(code))
    }

    /// These programs as a pack, each compressed at zstd `level`.
    ///
    /// # Errors
    ///
    /// A program cannot be read or compressed.
    pub fn to_pack(&self, level: i32) -> Result<Vec<u8>, BoxError> {
        let mut frames = Vec::with_capacity(self.keys.len());
        for k in &self.keys {
            if let Some(code) = self.code(k)? {
                let f = zstd::bulk::compress(&code, level)?;
                frames.push((*k, f, code.len()));
            }
        }
        let mut out = Vec::new();
        out.extend(MAGIC);
        out.extend((frames.len() as u32).to_le_bytes());
        out.extend((self.meshes.len() as u32).to_le_bytes());
        let mut at = (HEAD + ENTRY * frames.len() + self.meshes.len()).next_multiple_of(4) as u64;
        for (k, f, len) in &frames {
            out.extend(k);
            out.extend(at.to_le_bytes());
            out.extend((f.len() as u32).to_le_bytes());
            out.extend((*len as u32).to_le_bytes());
            at += (f.len() as u64).next_multiple_of(4);
        }
        out.extend(&self.meshes);
        for (_, f, _) in &frames {
            out.resize(out.len().next_multiple_of(4), 0);
            out.extend(f);
        }
        Ok(out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_pack_holds_what_the_folder_holds() {
        let d = crate::tests::scratch("pack");
        std::fs::create_dir_all(d.join("programs")).unwrap();
        let progs: [&[u8]; 3] = [b"one", b"second program", b"x"];
        for p in progs {
            relift_cache::write_if_changed(
                &d.join("programs").join(format!("{}.bin", hex(&sha256(p)))),
                p,
            )
            .unwrap();
        }
        relift_cache::write_if_changed(&d.join("programs").join("bad.bin"), b"no key").unwrap();
        relift_cache::write_if_changed(&d.join(MESHES), b"[]").unwrap();
        let dir = Programs::dir(&d).unwrap();
        assert_eq!(dir.keys().len(), 3);
        let pack = Programs::pack(dir.to_pack(3).unwrap()).unwrap();
        assert_eq!(pack.keys(), dir.keys());
        assert_eq!(pack.meshes(), b"[]");
        for k in dir.keys() {
            assert_eq!(pack.code(k).unwrap(), dir.code(k).unwrap());
            assert!(pack.code(k).unwrap().is_some());
        }
        assert_eq!(pack.code(&[0; 32]).unwrap(), None);
        let mut cut = dir.to_pack(3).unwrap();
        cut.truncate(cut.len() - 2);
        assert!(Programs::pack(cut).is_err());
        assert!(Programs::pack(b"RLSPV002".to_vec()).is_err());
        std::fs::remove_dir_all(&d).unwrap();
    }
}
