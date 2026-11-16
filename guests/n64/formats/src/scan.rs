//! A scan of an image for Yay0, Yaz0 and MIO0 streams.

use crate::compress::{Format, HEADER};

/// The kind of a stream.
pub type Kind = Format;

/// A stream found in an image.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Stream {
    /// Where the tag is.
    pub offset: usize,
    /// The container format.
    pub kind: Kind,
    /// The decoded size from the header; the stream decoded to it.
    pub decoded_size: usize,
}

/// Every offset of `bytes` that holds a stream of one of the three formats
/// which decodes completely. Offsets are tried at every byte; a tag that
/// does not decode is not reported.
#[must_use]
pub fn scan(bytes: &[u8]) -> Vec<Stream> {
    let mut found = Vec::new();
    let mut at = 0;
    while at + HEADER <= bytes.len() {
        if matches!(bytes[at], b'Y' | b'M')
            && let Some(kind) = Format::of(&bytes[at..])
            && let Ok(out) = kind.decode(&bytes[at..])
        {
            found.push(Stream {
                offset: at,
                kind,
                decoded_size: out.len(),
            });
        }
        at += 1;
    }
    found
}
