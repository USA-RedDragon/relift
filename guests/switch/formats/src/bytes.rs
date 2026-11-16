//! Little-endian field reads with bounds checks, hex and SHA-256.

use crate::error::{Result, malformed};

/// `N` bytes at `at`, or a format error naming `what`.
pub(crate) fn array<const N: usize>(b: &[u8], at: usize, what: &str) -> Result<[u8; N]> {
    match at.checked_add(N).and_then(|end| b.get(at..end)) {
        Some(s) => {
            let mut out = [0u8; N];
            out.copy_from_slice(s);
            Ok(out)
        }
        None => malformed(format!("{what}: field at {at:#x} runs past the end")),
    }
}

pub(crate) fn u8_at(b: &[u8], at: usize, what: &str) -> Result<u8> {
    Ok(array::<1>(b, at, what)?[0])
}

pub(crate) fn u16_at(b: &[u8], at: usize, what: &str) -> Result<u16> {
    Ok(u16::from_le_bytes(array(b, at, what)?))
}

pub(crate) fn u32_at(b: &[u8], at: usize, what: &str) -> Result<u32> {
    Ok(u32::from_le_bytes(array(b, at, what)?))
}

pub(crate) fn i32_at(b: &[u8], at: usize, what: &str) -> Result<i32> {
    Ok(i32::from_le_bytes(array(b, at, what)?))
}

pub(crate) fn u64_at(b: &[u8], at: usize, what: &str) -> Result<u64> {
    Ok(u64::from_le_bytes(array(b, at, what)?))
}

/// `len` bytes at `at`.
pub(crate) fn slice<'a>(b: &'a [u8], at: usize, len: usize, what: &str) -> Result<&'a [u8]> {
    match at.checked_add(len).and_then(|end| b.get(at..end)) {
        Some(s) => Ok(s),
        None => malformed(format!(
            "{what}: {len:#x} bytes at {at:#x} run past the end"
        )),
    }
}

/// A `u64` as a `usize`, or a format error.
pub(crate) fn to_usize(v: u64, what: &str) -> Result<usize> {
    usize::try_from(v).or_else(|_| malformed(format!("{what}: {v:#x} does not fit in memory")))
}

/// Lowercase hex of `b`.
pub fn hex(b: &[u8]) -> String {
    use std::fmt::Write;
    let mut s = String::with_capacity(b.len() * 2);
    for x in b {
        let _ = write!(s, "{x:02x}");
    }
    s
}

/// SHA-256 of `data`.
pub fn sha256(data: &[u8]) -> [u8; 32] {
    use sha2::{Digest, Sha256};
    Sha256::digest(data).into()
}

/// A SHA-256 of bytes written to it, for streamed data.
#[derive(Default)]
pub struct Sha256Writer(sha2::Sha256);

impl Sha256Writer {
    /// The digest of everything written.
    pub fn finish(self) -> [u8; 32] {
        sha2::Digest::finalize(self.0).into()
    }
}

impl std::io::Write for Sha256Writer {
    fn write(&mut self, b: &[u8]) -> std::io::Result<usize> {
        sha2::Digest::update(&mut self.0, b);
        Ok(b.len())
    }

    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}
