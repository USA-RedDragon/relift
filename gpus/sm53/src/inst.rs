//! A decoded instruction: its encoding, its word and where it sits.

use crate::field::{Field, Form};
use crate::form::{Encoding, Op, classify};

/// One instruction of a program.
#[derive(Clone, Copy, Debug)]
pub struct Inst {
    pub enc: &'static Encoding,
    pub word: u64,
    /// Code offset of the instruction (scheduling words included).
    pub at: u64,
}

impl Inst {
    /// The instruction `word` at code offset `at`, or none when no
    /// encoding matches.
    pub fn decode(word: u64, at: u64) -> Option<Inst> {
        classify(word).map(|enc| Inst { enc, word, at })
    }

    pub fn op(&self) -> Op {
        self.enc.op
    }

    pub fn form(&self) -> Form {
        self.enc.form
    }

    /// The field `name` of the instruction's form.
    pub fn field(&self, name: &str) -> Option<&'static Field> {
        self.enc.form.field(name)
    }

    /// The value of the field `name`, or none when the form has no such
    /// field.
    pub fn get(&self, name: &str) -> Option<u64> {
        self.field(name).map(|f| f.get(self.word))
    }

    /// The guard predicate and whether it is negated: bits 16 to 18 and
    /// 19 in every form that has one; predicate 7 is always true.
    pub fn guard(&self) -> Option<(u8, bool)> {
        let p = self.get("Pred")?;
        Some((p as u8, self.get("PredInv")? != 0))
    }
}
