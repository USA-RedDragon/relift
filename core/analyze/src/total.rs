//! Total AOT coverage: superset roots for the words discovery leaves out
//! of a module's executable pages (`docs/abi.md` section 5.2).

use std::collections::BTreeMap;

use relift_guest::{Frontend, Module, SectionKind};
use relift_ir::{Flow, Next};

use crate::{EntryKind, Layout, Outline, discover_more};

/// How many rounds of superset roots run before the rest is rooted at
/// every word.
const ROUNDS: usize = 6;

/// What a word holds when no function covers it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Spare {
    /// Zero.
    Zero,
    /// A nop, a `udf` or a `brk`: alignment padding.
    Pad,
    /// No instruction the frontend lifts.
    Undecodable,
    /// An instruction; `falls` when it may go on to the next word.
    Code {
        /// Whether it may go on to the next word.
        falls: bool,
    },
}

pub fn word_at(module: &Module, pc: u64) -> Option<u32> {
    module
        .bytes_at(pc)
        .and_then(|b| b.get(..4))
        .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

pub fn spare(fe: &dyn Frontend, module: &Module, pc: u64) -> Spare {
    let Some(w) = word_at(module, pc) else {
        return Spare::Zero;
    };
    if w == 0 {
        return Spare::Zero;
    }
    if w == 0xD503_201F || w & 0xFFE0_001F == 0xD420_0000 || w & 0xFFFF_0000 == 0 {
        return Spare::Pad;
    }
    match module.bytes_at(pc).map(|b| fe.lift(b, pc)) {
        Some(Ok(b)) => Spare::Code {
            falls: !matches!(
                (b.exit.flow, b.exit.next),
                (Flow::Return, _) | (Flow::Jump, Next::Dynamic(_))
            ) && !matches!((b.exit.flow, b.exit.next), (Flow::Jump, Next::Const(t)) if t != pc + u64::from(b.len)),
        },
        _ => Spare::Undecodable,
    }
}

/// The executable pages of `module`: its code sections, each up to the
/// end of its last page.
pub fn exec_range(module: &Module) -> Option<(u64, u64)> {
    let code: Vec<_> = module
        .sections
        .iter()
        .filter(|s| s.kind == SectionKind::Code)
        .collect();
    let lo = code.iter().map(|s| s.addr).min()?;
    let hi = code.iter().map(|s| s.addr + s.size).max()?;
    Some((lo, hi.div_ceil(0x1000) * 0x1000))
}

/// One bit per word of `[lo, hi)`.
pub struct Bits {
    lo: u64,
    v: Vec<u64>,
}

impl Bits {
    pub fn new(lo: u64, hi: u64) -> Bits {
        Bits {
            lo,
            v: vec![0; ((hi - lo) / 4).div_ceil(64) as usize],
        }
    }

    fn index(&self, pc: u64) -> Option<usize> {
        let i = usize::try_from(pc.checked_sub(self.lo)? / 4).ok()?;
        (i / 64 < self.v.len()).then_some(i)
    }

    pub fn set(&mut self, pc: u64) {
        if let Some(i) = self.index(pc) {
            self.v[i / 64] |= 1 << (i % 64);
        }
    }

    pub fn get(&self, pc: u64) -> bool {
        self.index(pc)
            .is_some_and(|i| self.v[i / 64] >> (i % 64) & 1 != 0)
    }

    pub fn cover(&mut self, o: &Outline) {
        for &(first, n) in &o.runs {
            for k in 0..u64::from(n) {
                self.set(first + 4 * k);
            }
        }
    }
}

/// The superset functions of a module and how they were found.
#[derive(Clone, Debug, Default)]
pub struct Superset {
    /// The functions that are not functions of the hot layout.
    pub layout: Layout,
    /// The roots given to discovery.
    pub roots: usize,
}

/// The functions that cover every word of `module`'s executable pages
/// that the functions of `hot` leave out and that holds an instruction:
/// a root at each such word that no such word before it falls into, then
/// discovery from them, until none is left.
pub fn superset(fe: &(dyn Frontend + Sync), module: &Module, hot: &Layout) -> Superset {
    let mut out = Superset {
        layout: Layout::default(),
        roots: 0,
    };
    let Some((lo, hi)) = exec_range(module) else {
        return out;
    };
    let mut covered = Bits::new(lo, hi);
    for o in hot.functions.values() {
        covered.cover(o);
    }
    for round in 0..=ROUNDS {
        let mut roots = Vec::new();
        let mut falls = false;
        let mut pc = lo;
        while pc < hi {
            if covered.get(pc) {
                falls = false;
            } else {
                match spare(fe, module, pc) {
                    Spare::Code { falls: f } => {
                        if !falls || round == ROUNDS {
                            roots.push(pc);
                        }
                        falls = f;
                    }
                    _ => falls = false,
                }
            }
            pc += 4;
        }
        if roots.is_empty() {
            break;
        }
        out.roots += roots.len();
        let known: BTreeMap<u64, EntryKind> = hot
            .functions
            .values()
            .chain(out.layout.functions.values())
            .map(|o| (o.entry, o.kind))
            .collect();
        let found = discover_more(fe, module, &known, &roots);
        let more = Layout::of(&found, fe.granule());
        for o in more.functions.values() {
            covered.cover(o);
        }
        out.layout.functions.extend(more.functions);
    }
    out
}
