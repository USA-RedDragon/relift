//! Zstandard frames with the dictionaries a game ships: the dictionary id
//! in a frame header picks the dictionary, found by the scan in the
//! game's own `ZsDic.pack.zs`.

use std::collections::BTreeMap;
use std::io::Read;

use zstd::dict::DecoderDictionary;

use crate::error::{Error, Result, malformed};
use crate::sha256_hex;

/// The magic of a zstd frame.
pub const FRAME_MAGIC: [u8; 4] = [0x28, 0xB5, 0x2F, 0xFD];
/// The magic of a zstd dictionary.
pub const DICT_MAGIC: [u8; 4] = [0x37, 0xA4, 0x30, 0xEC];
/// The largest output of one frame.
pub const MAX_OUT: u64 = 1 << 30;

/// The dictionaries found so far, by id.
#[derive(Default)]
pub struct Dicts {
    by_id: BTreeMap<u32, DecoderDictionary<'static>>,
    digest: String,
}

impl Dicts {
    /// No dictionaries.
    pub fn new() -> Dicts {
        Dicts::default()
    }

    /// Add the dictionary in `bytes`; false when it is not one.
    pub fn add(&mut self, bytes: &[u8]) -> bool {
        if !bytes.starts_with(&DICT_MAGIC) || bytes.len() < 8 {
            return false;
        }
        let id = u32::from_le_bytes([bytes[4], bytes[5], bytes[6], bytes[7]]);
        self.by_id.insert(id, DecoderDictionary::copy(bytes));
        let mut text = self.digest.clone();
        text.push_str(&sha256_hex(bytes));
        self.digest = sha256_hex(text.as_bytes());
        true
    }

    /// A digest of the dictionaries held, for cache keys.
    pub fn digest(&self) -> &str {
        &self.digest
    }

    /// How many dictionaries are held.
    pub fn len(&self) -> usize {
        self.by_id.len()
    }

    /// Whether no dictionary is held.
    pub fn is_empty(&self) -> bool {
        self.by_id.is_empty()
    }

    /// The ids held.
    pub fn ids(&self) -> Vec<u32> {
        self.by_id.keys().copied().collect()
    }
}

/// The dictionary id a frame header names, 0 for none.
///
/// # Errors
///
/// The header is cut off.
pub fn frame_dict_id(b: &[u8]) -> Result<u32> {
    let Some(&fhd) = b.get(4) else {
        return malformed("zstd: no frame header");
    };
    let at = 5 + usize::from(fhd & 0x20 == 0);
    let n = match fhd & 3 {
        0 => 0,
        1 => 1,
        2 => 2,
        _ => 4,
    };
    let Some(s) = b.get(at..at + n) else {
        return malformed("zstd: the frame header is cut off");
    };
    Ok(s.iter().rev().fold(0u32, |a, &x| (a << 8) | u32::from(x)))
}

/// Decode the first frame of `b`, with its dictionary when it names one.
///
/// # Errors
///
/// [`Error::MissingDictionary`] for an unknown id, or a bad frame.
pub fn decode(b: &[u8], dicts: &Dicts) -> Result<Vec<u8>> {
    let id = frame_dict_id(b)?;
    let bad = |e: std::io::Error| Error::Format(format!("zstd: {e}"));
    let mut out = Vec::new();
    if id == 0 {
        let dec = zstd::stream::read::Decoder::new(b)
            .map_err(bad)?
            .single_frame();
        dec.take(MAX_OUT).read_to_end(&mut out).map_err(bad)?;
    } else {
        let Some(d) = dicts.by_id.get(&id) else {
            return Err(Error::MissingDictionary(id));
        };
        let dec = zstd::stream::read::Decoder::with_prepared_dictionary(b, d)
            .map_err(bad)?
            .single_frame();
        dec.take(MAX_OUT).read_to_end(&mut out).map_err(bad)?;
    }
    Ok(out)
}
