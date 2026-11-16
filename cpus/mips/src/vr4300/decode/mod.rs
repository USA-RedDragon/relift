//! Decoding: the decode clauses of sail-cheri-mips and the VR4300 overlay,
//! generated into `decode/generated.rs`. The clauses share no word (the
//! generator checks it), so a word's encoding is the one whose fixed bits
//! match; a word that matches none is a reserved instruction.

mod generated;

pub use generated::Encoding;

/// The VR4300 encoding of `w`, or `None` when it is reserved.
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
    /// The VR4300 mnemonic; `COP1.fmt` for all arithmetic, conversion and
    /// compare operations of one format.
    pub fn name(self) -> &'static str {
        generated::NAMES[self as usize]
    }

    /// The template class: `int`, `ldst`, `branch`, `system` or `cop1`.
    pub fn class(self) -> &'static str {
        generated::FORMS
            .iter()
            .find(|f| f.0 == self)
            .map_or("", |f| f.3)
    }

    /// Whether the instruction has a delay slot: every branch and jump
    /// except ERET.
    pub fn has_delay_slot(self) -> bool {
        self.class() == "branch"
    }
}
