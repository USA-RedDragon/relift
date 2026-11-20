//! Total AOT coverage of a Switch module: superset roots for the words
//! discovery leaves out, and the class of every aligned word of the
//! module's executable pages (docs/abi.md section 5.1).

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;

use relift_analyze::Outline;
use relift_guest::{Frontend, Module};

/// The guest page the class map is cut into: `RL_CODE_PAGE_SLOTS` words.
const PAGE_WORDS: u64 = 1024;

pub(crate) use relift_analyze::total::{Bits, Spare, Superset, exec_range, spare, superset};

/// How many words of a module's executable pages fall in each class.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct Census {
    pub(crate) words: u64,
    pub(crate) hot: u64,
    pub(crate) cold: u64,
    pub(crate) zero: u64,
    pub(crate) pad: u64,
    pub(crate) undecodable: u64,
}

impl Census {
    pub(crate) fn add(&mut self, o: &Census) {
        self.words += o.words;
        self.hot += o.hot;
        self.cold += o.cold;
        self.zero += o.zero;
        self.pad += o.pad;
        self.undecodable += o.undecodable;
    }

    pub(crate) fn line(&self) -> String {
        format!(
            "{} words: {} hot entries, {} cold entries, {} traps ({} zero, {} padding, {} undecodable)",
            self.words,
            self.hot,
            self.cold,
            self.zero + self.pad + self.undecodable,
            self.zero,
            self.pad,
            self.undecodable
        )
    }
}

/// The class of every word of a module's executable pages.
#[derive(Debug)]
pub(crate) struct CodeMap {
    pub(crate) lo: u64,
    /// `RL_CODE_PAGE_BYTES` per page: two bits per word.
    pub(crate) bits: Vec<u8>,
    pub(crate) census: Census,
}

/// The classes of `module`'s words: `RL_CODE_HOT` at each of `hot` (the
/// hot unit's entries), else `RL_CODE_COLD` at each instruction of the
/// `cold` functions, else `RL_CODE_TRAP`.
///
/// # Errors
///
/// A word left a trap holds an instruction: coverage is not total.
pub(crate) fn code_map(
    fe: &dyn Frontend,
    module: &Module,
    hot: &BTreeSet<u64>,
    cold: &[&Outline],
) -> Result<CodeMap, String> {
    let Some((lo, hi)) = exec_range(module) else {
        return Ok(CodeMap {
            lo: 0,
            bits: Vec::new(),
            census: Census::default(),
        });
    };
    let mut covered = Bits::new(lo, hi);
    for o in cold {
        covered.cover(o);
    }
    let words = (hi - lo) / 4;
    let pages = words.div_ceil(PAGE_WORDS);
    let mut bits = vec![0u8; usize::try_from(pages * PAGE_WORDS / 4).unwrap_or(0)];
    let mut c = Census {
        words,
        ..Census::default()
    };
    let mut open = Vec::new();
    for k in 0..words {
        let pc = lo + 4 * k;
        let class = if hot.contains(&pc) {
            c.hot += 1;
            1u8
        } else if covered.get(pc) {
            c.cold += 1;
            2u8
        } else {
            match spare(fe, module, pc) {
                Spare::Zero => c.zero += 1,
                Spare::Pad => c.pad += 1,
                Spare::Undecodable => c.undecodable += 1,
                Spare::Code { .. } => open.push(pc),
            }
            0u8
        };
        let i = usize::try_from(k).unwrap_or(0);
        bits[i / 4] |= class << (2 * (i % 4));
    }
    if let Some(first) = open.first() {
        return Err(format!(
            "coverage of {} is not total: {} words that hold an instruction have no entry, the first at {:#x} ({}+{:#x})",
            module.name,
            open.len(),
            first,
            module.name,
            first - module.base
        ));
    }
    Ok(CodeMap {
        lo,
        bits,
        census: c,
    })
}

/// The C of `map` as the `rl_code_map` named `sym` over the units `hot`
/// and `cold`, with identical pages kept once.
pub(crate) fn code_map_c(
    headers: &[&str],
    map: &CodeMap,
    sym: &str,
    hot: &str,
    cold: &str,
    module: &str,
) -> String {
    let page = (PAGE_WORDS / 4) as usize;
    let mut index: BTreeMap<&[u8], u32> = BTreeMap::new();
    let mut patterns: Vec<&[u8]> = Vec::new();
    let mut pages = Vec::new();
    for p in map.bits.chunks(page) {
        let n = u32::try_from(patterns.len()).unwrap_or(u32::MAX);
        let k = *index.entry(p).or_insert_with(|| {
            patterns.push(p);
            n
        });
        pages.push(k);
    }
    let mut t = String::new();
    for h in headers {
        let _ = writeln!(t, "#include <{h}>");
    }
    let _ = writeln!(
        t,
        "extern const rl_unit {hot};\nextern const rl_unit {cold};"
    );
    t.push_str("static const uint32_t pages[] = {");
    for (k, p) in pages.iter().enumerate() {
        if k % 16 == 0 {
            t.push_str("\n   ");
        }
        let _ = write!(t, " {p}u,");
    }
    if pages.is_empty() {
        t.push_str(" 0u");
    }
    let _ = write!(
        t,
        "\n}};\nstatic const uint8_t patterns[{}] = {{",
        patterns.len().max(1) * page
    );
    for p in &patterns {
        for row in p.chunks(32) {
            t.push_str("\n   ");
            for b in row {
                let _ = write!(t, " {b:#04x},");
            }
        }
    }
    if patterns.is_empty() {
        t.push_str(" 0");
    }
    let _ = write!(
        t,
        "\n}};\nconst rl_code_map {sym};\nconst rl_code_map {sym} = {{ RL_CODE_MAP_VERSION, {}u, {}u, UINT64_C({:#x}), pages, patterns, &{hot}, &{cold}, {module:?} }};\n",
        pages.len(),
        patterns.len(),
        map.lo
    );
    t
}

#[cfg(test)]
mod tests {
    use super::*;
    use relift_cpu_arm64::frontend::Arm64;
    use relift_guest::{Root, RootKind, Section, SectionKind};

    const BASE: u64 = 0x8000_0000;
    const RET: u32 = 0xD65F_03C0;
    const NOP: u32 = 0xD503_201F;
    const MOVZ: u32 = 0xD280_0020;

    fn module(words: &[u32]) -> Module {
        let image: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
        Module {
            name: "m".into(),
            base: BASE,
            sections: vec![Section {
                addr: BASE,
                size: image.len() as u64,
                kind: SectionKind::Code,
            }],
            image,
            roots: vec![Root {
                addr: BASE,
                kind: RootKind::Entry,
                name: None,
            }],
            pads: Vec::new(),
        }
    }

    fn cpu() -> Arm64 {
        Arm64 {
            cpu: relift_cpu_arm64::cpu::Cpu::new(
                relift_switch::cpu::Console::Switch.features(),
                relift_cpu_arm64::cpu::Va::BITS39,
            ),
        }
    }

    #[test]
    fn superset_roots_cover_every_instruction_discovery_left() {
        let m = module(&[RET, 0, MOVZ, MOVZ, RET, NOP, MOVZ, RET, 0xFFFF_FFFF]);
        let cpu = cpu();
        let hot = relift_analyze::outline_with(&cpu, &m, &mut |_| {});
        assert_eq!(hot.functions.len(), 1);
        let s = superset(&cpu, &m, &hot);
        let entries: Vec<u64> = s.layout.functions.keys().map(|e| e - BASE).collect();
        assert_eq!(entries, vec![8, 0x18]);
        assert_eq!(s.roots, 2);
        let cold: Vec<&Outline> = hot
            .functions
            .values()
            .chain(s.layout.functions.values())
            .collect();
        let map = code_map(&cpu, &m, &BTreeSet::from([BASE]), &cold).expect("total");
        let c = map.census;
        assert_eq!(c.words, 1024);
        assert_eq!((c.hot, c.cold, c.pad), (1, 5, 1));
        assert_eq!(c.zero + c.pad + c.undecodable + c.hot + c.cold, c.words);
        assert_eq!(map.bits[0], 1 | 2 << 4 | 2 << 6);
        assert_eq!(map.bits[1], 2 | 2 << 4 | 2 << 6);
        let only_hot: Vec<&Outline> = hot.functions.values().collect();
        let e = code_map(&cpu, &m, &BTreeSet::from([BASE]), &only_hot).expect_err("open");
        assert!(
            e.contains(
                "5 words that hold an instruction have no entry, the first at 0x80000008 (m+0x8)"
            ),
            "{e}"
        );
    }

    #[test]
    fn identical_pages_share_one_pattern() {
        let map = CodeMap {
            lo: BASE,
            bits: vec![0xAA; 3 * 256],
            census: Census::default(),
        };
        let c = code_map_c(&["relift/arm64/abi.h"], &map, "rl_code_0", "h", "c", "main");
        assert!(c.contains("static const uint8_t patterns[256] = {"), "{c}");
        assert!(c.contains(" 0u, 0u, 0u,"), "{c}");
        assert!(c.contains("{ RL_CODE_MAP_VERSION, 3u, 1u, UINT64_C(0x80000000), pages, patterns, &h, &c, \"main\" };"), "{c}");
    }
}
