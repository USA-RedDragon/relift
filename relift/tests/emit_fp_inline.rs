//! FP operations past the FPCR guard use the inline fast paths of
//! `relift/arm64/abi.h`, scalar and vector lanes alike: random words of every
//! form in `cpus/arm64/gen/forms.txt` are lifted for the emitter and emitted,
//! and the C must not call the out-of-line exact layer for an operation that
//! has an inline form (add, subtract, multiply, divide, fused multiply-add,
//! square root, conversions between integers and floats and between single
//! and double), and must pass those forms FPCR as the constant zero. The
//! values the inline forms give are checked against the exact layer by the
//! runtime's `test_fp` and `test_fp_flags`, and the emitted C against the
//! exact layer by `emit_flags`.

use std::collections::BTreeMap;

use relift_cpu_arm64::decode::{Encoding, Features, decode};
use relift_cpu_arm64::frontend::Arm64;
use relift_emit::{Function, Insn, Options, emit};
use relift_guest::Frontend;
use relift_ir::{Block, Op};

/// Random words per form.
const WORDS: usize = 16;

/// The out-of-line helpers that have an inline fast path.
const OUT_OF_LINE: [&str; 15] = [
    "rl_fp32_add(",
    "rl_fp32_sub(",
    "rl_fp32_mul(",
    "rl_fp32_div(",
    "rl_fp32_muladd(",
    "rl_fp32_sqrt(",
    "rl_fp64_add(",
    "rl_fp64_sub(",
    "rl_fp64_mul(",
    "rl_fp64_div(",
    "rl_fp64_muladd(",
    "rl_fp64_sqrt(",
    "rl_fp_to_fixed(",
    "rl_fixed_to_fp(",
    "rl_fp_convert(",
];

/// The inline fast paths, which end in the FPCR and the FPSR pointer.
const INLINE: [&str; 15] = [
    "rl_fadd32(",
    "rl_fsub32(",
    "rl_fmul32(",
    "rl_fdiv32(",
    "rl_fadd64(",
    "rl_fsub64(",
    "rl_fmul64(",
    "rl_fdiv64(",
    "rl_fmuladd32(",
    "rl_fmuladd64(",
    "rl_fsqrt32(",
    "rl_fsqrt64(",
    "rl_ftoint(",
    "rl_inttof(",
    "rl_fcvt(",
];

/// A small xorshift generator.
struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
}

fn has_fp(b: &Block) -> bool {
    b.insts.iter().any(|i| matches!(i.op, Op::Fp(..)))
}

/// The C of the word `w` lifted at `pc` and emitted as one function.
fn c_of(fe: &Arm64, w: u32, b: &Block, pc: u64) -> String {
    let f = Function {
        entry: pc,
        name: "F".into(),
        guest_name: None,
        span: 4,
        insts: BTreeMap::from([(
            pc,
            Insn::Lifted {
                block: b,
                raw: u64::from(w),
            },
        )]),
        tables: BTreeMap::new(),
        pads: Default::default(),
        guesses: Default::default(),
        entries: Default::default(),
    };
    emit(fe, &[f], &[], &Options::default())
        .files
        .iter()
        .filter(|f| f.name.starts_with("shard"))
        .map(|f| f.text.clone())
        .collect()
}

/// The calls of `name` in `c`, each with its argument text.
fn calls<'a>(c: &'a str, name: &str) -> Vec<&'a str> {
    c.match_indices(name)
        .filter(|(i, _)| {
            !c[..*i]
                .chars()
                .next_back()
                .is_some_and(|p| p.is_ascii_alphanumeric() || p == '_')
        })
        .map(|(i, _)| {
            let rest = &c[i + name.len()..];
            &rest[..rest.find(';').unwrap_or(rest.len())]
        })
        .collect()
}

#[test]
fn fp_operations_past_the_guard_use_the_inline_fast_paths() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let forms = std::fs::read_to_string(root.join("cpus/arm64/gen/forms.txt"))
        .expect("forms.txt is readable");
    let by_name: BTreeMap<&str, Encoding> = Encoding::all().map(|e| (e.name(), e)).collect();
    let fe = Arm64 {
        cpu: relift_cpu_arm64::cpu::Cpu::new(Features::ALL, relift_cpu_arm64::cpu::Va::BITS39),
    };
    let mut r = Rng(0x0fa5_7f9e);
    let mut bad = Vec::new();
    let mut seen: BTreeMap<(&str, &str), usize> = BTreeMap::new();
    let mut lifted = 0;
    for name in forms.lines().map(str::trim).filter(|l| !l.is_empty()) {
        let enc = *by_name
            .get(name)
            .unwrap_or_else(|| panic!("form {name} decodes"));
        let mut got = 0;
        for _ in 0..400 {
            if got == WORDS {
                break;
            }
            let w = enc.value() | (r.next() as u32 & !enc.mask());
            if decode(w).map(|i| i.encoding) != Some(enc) {
                continue;
            }
            let pc = 0x40_0000;
            let Ok(b) = fe.lift(&w.to_le_bytes(), pc) else {
                continue;
            };
            if !has_fp(&b) {
                continue;
            }
            got += 1;
            lifted += 1;
            let c = c_of(&fe, w, &b, pc);
            for o in OUT_OF_LINE {
                if !calls(&c, o).is_empty() {
                    bad.push(format!("{name} {w:#010x}: calls {o}"));
                }
            }
            for inline in INLINE {
                for args in calls(&c, inline) {
                    *seen.entry((name, inline)).or_default() += 1;
                    if !args.ends_with(", 0u, 0)") {
                        bad.push(format!(
                            "{name} {w:#010x}: {inline}{args} does not take FPCR zero"
                        ));
                    }
                }
            }
        }
    }
    assert!(lifted > 1000, "only {lifted} FP words lifted");
    for (form, inline) in [
        ("FMLA_asimdelem_R_SD", "rl_fmuladd32("),
        ("FMLA_asimdelem_R_SD", "rl_fmuladd64("),
        ("FMLS_asimdelem_R_SD", "rl_fmuladd32("),
        ("FMLA_asimdsame_only", "rl_fmuladd32("),
        ("FMLA_asimdsame_only", "rl_fmuladd64("),
        ("FMLS_asimdsame_only", "rl_fmuladd32("),
        ("FMLA_asisdelem_R_SD", "rl_fmuladd32("),
        ("FMADD_S_floatdp3", "rl_fmuladd32("),
        ("FMADD_D_floatdp3", "rl_fmuladd64("),
        ("FSQRT_asimdmisc_R", "rl_fsqrt32("),
        ("FSQRT_asimdmisc_R", "rl_fsqrt64("),
        ("FSQRT_S_floatdp1", "rl_fsqrt32("),
        ("FSQRT_D_floatdp1", "rl_fsqrt64("),
        ("FCVTZS_asimdmisc_R", "rl_ftoint("),
        ("FCVTZU_32S_float2int", "rl_ftoint("),
        ("SCVTF_asimdmisc_R", "rl_inttof("),
        ("UCVTF_D64_float2int", "rl_inttof("),
        ("FCVTL_asimdmisc_L", "rl_fcvt("),
        ("FCVTN_asimdmisc_N", "rl_fcvt("),
        ("FCVT_SD_floatdp1", "rl_fcvt("),
        ("FADD_asimdsame_only", "rl_fadd32("),
        ("FMUL_asimdelem_R_SD", "rl_fmul32("),
    ] {
        if !seen.contains_key(&(form, inline)) {
            bad.push(format!("{form}: no {inline}"));
        }
    }
    assert!(
        bad.is_empty(),
        "{} problems:\n{}",
        bad.len(),
        bad.join("\n")
    );
}
