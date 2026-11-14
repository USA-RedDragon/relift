//! A64 instruction decoder driven by the tables generated from Arm's AARCHMRS.
//!
//! [`decode`] maps a 32-bit word to the [`Encoding`] leaf of the specification
//! with the most fixed bits whose field constraints hold.

mod generated;
#[cfg(test)]
mod tests;

use std::sync::OnceLock;

pub use generated::Encoding;

/// A named bit field of an encoding: `width` bits starting at bit `lo`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Field {
    /// The field name as written in the specification, for example `Rn`.
    pub name: &'static str,
    /// The index of the least significant bit.
    pub lo: u8,
    /// The number of bits.
    pub width: u8,
}

impl Field {
    /// Returns this field's bits of `word`, right aligned.
    pub fn extract(self, word: u32) -> u32 {
        let mask = if self.width >= 32 {
            u32::MAX
        } else {
            (1u32 << self.width) - 1
        };
        (word >> self.lo) & mask
    }
}

impl Encoding {
    /// The number of encodings.
    pub const COUNT: usize = generated::COUNT;

    /// Iterates over every encoding in specification order.
    pub fn all() -> impl Iterator<Item = Encoding> {
        generated::ALL.iter().copied()
    }

    /// The leaf name used by the specification, for example `ADD_64_addsub_imm`.
    pub fn name(self) -> &'static str {
        generated::NAMES[self as usize]
    }

    /// The assembly mnemonic, for example `ADD`.
    pub fn mnemonic(self) -> &'static str {
        generated::MNEMONICS[usize::from(generated::MNEMONIC_INDEX[self as usize])]
    }

    /// The top-level group under A64, for example `dpimm`, `ldst` or `sve`.
    pub fn group(self) -> &'static str {
        generated::GROUPS[usize::from(generated::GROUP_INDEX[self as usize])]
    }

    /// The names of the groups from the top-level group down to this leaf's parent.
    pub fn path(self) -> &'static [&'static str] {
        generated::PATHS[usize::from(generated::PATH_INDEX[self as usize])]
    }

    /// The bits that are fixed by this encoding, including those fixed by its groups.
    pub fn mask(self) -> u32 {
        generated::MASKS[self as usize]
    }

    /// The values of the fixed bits; zero outside [`Encoding::mask`].
    pub fn value(self) -> u32 {
        generated::VALUES[self as usize]
    }

    /// The named fields of this leaf's own encodeset.
    pub fn fields(self) -> &'static [Field] {
        let start = generated::FIELD_START[self as usize] as usize;
        let len = usize::from(generated::FIELD_LEN[self as usize]);
        &generated::FIELDS[start..start + len]
    }

    /// Looks up a field of this encoding by name.
    pub fn field(self, name: &str) -> Option<Field> {
        self.fields().iter().copied().find(|f| f.name == name)
    }

    /// Whether the encoding belongs to the SVE or SME groups.
    pub fn is_sve_or_sme(self) -> bool {
        matches!(self.group(), "sve" | "sme")
    }

    /// Whether `word` matches the fixed bits and the field constraints of this encoding.
    pub fn matches(self, word: u32) -> bool {
        word & self.mask() == self.value() && generated::condition(self, word)
    }
}

/// A decoded instruction word together with the encoding it matched.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Insn {
    /// The raw instruction word.
    pub word: u32,
    /// The matched encoding.
    pub encoding: Encoding,
}

impl Insn {
    /// Returns the named field of the encoding, or `None` when it has no such field.
    pub fn field(&self, name: &str) -> Option<u32> {
        self.encoding.field(name).map(|f| f.extract(self.word))
    }

    /// Returns the named field of the encoding.
    ///
    /// # Panics
    ///
    /// Panics when the encoding has no field `name`. Lifters use it where the
    /// encoding fixes the field set.
    pub fn get(&self, name: &str) -> u32 {
        match self.field(name) {
            Some(v) => v,
            None => panic!("encoding {} has no field {}", self.encoding.name(), name),
        }
    }
}

const KEY_SHIFT: u32 = 21;
const KEY_COUNT: usize = 1 << (32 - KEY_SHIFT);

struct Index {
    starts: Vec<u32>,
    entries: Vec<Entry>,
}

#[derive(Clone, Copy)]
struct Entry {
    mask: u32,
    value: u32,
    encoding: Encoding,
}

fn index() -> &'static Index {
    static INDEX: OnceLock<Index> = OnceLock::new();
    INDEX.get_or_init(|| {
        let mut order: Vec<Encoding> = Encoding::all().collect();
        order.sort_by_key(|e| std::cmp::Reverse(e.mask().count_ones()));
        let key_mask = u32::MAX << KEY_SHIFT;
        let mut starts = Vec::with_capacity(KEY_COUNT + 1);
        let mut entries = Vec::new();
        for key in 0..KEY_COUNT {
            starts.push(entries.len() as u32);
            let bits = (key as u32) << KEY_SHIFT;
            for &e in &order {
                let (m, v) = (e.mask(), e.value());
                if bits & m & key_mask == v & m & key_mask {
                    entries.push(Entry {
                        mask: m,
                        value: v,
                        encoding: e,
                    });
                }
            }
        }
        starts.push(entries.len() as u32);
        Index { starts, entries }
    })
}

/// Decodes `word` to the matching encoding with the most fixed bits.
///
/// Returns `None` for unallocated words. Equally specific matches resolve to
/// the one earlier in the specification; [`matching`] lists all matches.
pub fn decode(word: u32) -> Option<Insn> {
    let idx = index();
    let key = (word >> KEY_SHIFT) as usize;
    let lo = idx.starts[key] as usize;
    let hi = idx.starts[key + 1] as usize;
    idx.entries[lo..hi]
        .iter()
        .find(|e| word & e.mask == e.value && generated::condition(e.encoding, word))
        .map(|e| Insn {
            word,
            encoding: e.encoding,
        })
}

/// Returns every encoding that matches `word`, most fixed bits first.
pub fn matching(word: u32) -> Vec<Encoding> {
    let idx = index();
    let key = (word >> KEY_SHIFT) as usize;
    let lo = idx.starts[key] as usize;
    let hi = idx.starts[key + 1] as usize;
    idx.entries[lo..hi]
        .iter()
        .filter(|e| word & e.mask == e.value && generated::condition(e.encoding, word))
        .map(|e| e.encoding)
        .collect()
}
