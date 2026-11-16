//! Decoding: the decode clauses of sail-cheri-mips kept on the RSP and of
//! the RSP overlay, generated into `decode/generated.rs`. The clauses share
//! no word (the generator checks it), so a word's encoding is the one whose
//! fixed bits match; a word that matches none is no RSP instruction.

mod generated;

pub use generated::Encoding;

/// The RSP encoding of `w`, or `None` when it is no RSP instruction.
pub fn decode(w: u32) -> Option<Encoding> {
    generated::FORMS
        .iter()
        .find(|(_, m, v, _)| w & m == *v)
        .map(|f| f.0)
}

/// Every encoding with its fixed bits and the class of its template.
pub fn forms() -> &'static [(Encoding, u32, u32, &'static str)] {
    generated::FORMS
}

impl Encoding {
    /// The RSP mnemonic.
    pub fn name(self) -> &'static str {
        generated::NAMES[self as usize]
    }

    /// The template class: `int`, `ldst`, `branch`, `system`, `vector` or
    /// `vmem`.
    pub fn class(self) -> &'static str {
        generated::FORMS
            .iter()
            .find(|f| f.0 == self)
            .map_or("", |f| f.3)
    }

    /// Whether the instruction has a delay slot: every branch and jump.
    pub fn has_delay_slot(self) -> bool {
        self.class() == "branch"
    }
}
