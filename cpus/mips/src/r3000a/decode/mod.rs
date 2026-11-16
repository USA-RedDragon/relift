//! Decoding: the MIPS I decode clauses of sail-cheri-mips that the R3000A
//! overlay keeps, generated into `decode/generated.rs`. The clauses share
//! no word (the generator checks it), so a word's encoding is the one whose
//! fixed bits match; a word that matches none is a reserved instruction.

mod generated;

pub use generated::Encoding;

/// The R3000A encoding of `w`, or `None` when it is reserved.
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
    /// The R3000A mnemonic.
    pub fn name(self) -> &'static str {
        generated::NAMES[self as usize]
    }

    /// The template class: `int`, `ldst`, `branch` or `system`.
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
