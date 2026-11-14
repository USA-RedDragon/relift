//! Checks that the generator's parser reads the SM83 model in
//! `cpus/sm83/sail/sm83.sail`, and that its decode clauses take each of the
//! 256 base and 256 CB opcodes to the execute function of the opcode
//! table's mnemonic.

use std::collections::HashMap;
use std::path::PathBuf;

use relift_cpu_sm83::{BASE, CB, Mnemonic};

use relift_sail::ast::{Expr, Lit, Pat, Typ, TypArg};
use relift_sail::model::Model;
use relift_sail::stage;

fn model() -> Model {
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../sail/sm83.sail");
    let mut m = Model::default();
    m.load_file(&path).expect("reads sm83.sail");
    m
}

fn bits_width(t: &Typ) -> Option<u32> {
    match t {
        Typ::App(n, args) if n == "bits" => match args.as_slice() {
            [TypArg::Expr(Expr::Lit(Lit::Int(w)))] => u32::try_from(*w).ok(),
            _ => None,
        },
        _ => None,
    }
}

/// Matches `pat` against the `width`-bit value `v`, binding names.
fn matches(pat: &Pat, width: u32, v: u128, env: &mut HashMap<String, (u32, u128)>) -> bool {
    match pat {
        Pat::Wild => true,
        Pat::Id(n) => {
            env.insert(n.clone(), (width, v));
            true
        }
        Pat::Lit(Lit::Bits { width: w, value }) => *w == width && *value == v,
        Pat::Tuple(ps) if ps.len() == 1 => matches(&ps[0], width, v, env),
        Pat::Typed(p, t) => bits_width(t) == Some(width) && matches(p, width, v, env),
        Pat::Concat(parts) => {
            let mut left = width;
            for p in parts {
                let w = match p {
                    Pat::Lit(Lit::Bits { width, .. }) => *width,
                    Pat::Typed(_, t) => match bits_width(t) {
                        Some(w) => w,
                        None => return false,
                    },
                    _ => return false,
                };
                if w > left {
                    return false;
                }
                left -= w;
                let part = (v >> left) & ((1u128 << w) - 1);
                if !matches(p, w, part, env) {
                    return false;
                }
            }
            left == 0
        }
        _ => false,
    }
}

#[derive(Debug, PartialEq)]
enum Val {
    Bits(u32, u128),
    Bool(bool),
}

fn eval_guard(e: &Expr, env: &HashMap<String, (u32, u128)>) -> Option<Val> {
    Some(match e {
        Expr::Id(n) => {
            let (w, v) = env.get(n)?;
            Val::Bits(*w, *v)
        }
        Expr::Lit(Lit::Bits { width, value }) => Val::Bits(*width, *value),
        Expr::Binop(op, a, b) => {
            let a = eval_guard(a, env)?;
            let b = eval_guard(b, env)?;
            match (op.as_str(), a, b) {
                ("==", x, y) => Val::Bool(x == y),
                ("!=", x, y) => Val::Bool(x != y),
                ("|", Val::Bool(x), Val::Bool(y)) => Val::Bool(x || y),
                ("&", Val::Bool(x), Val::Bool(y)) => Val::Bool(x && y),
                _ => return None,
            }
        }
        _ => return None,
    })
}

/// The function the first matching clause of `decoder` calls for `op`.
fn decoded(m: &Model, decoder: &str, op: u8) -> String {
    let clauses = m
        .func(decoder)
        .expect("decoder is defined")
        .expect("decoder parses");
    for c in clauses.iter() {
        let mut env = HashMap::new();
        if !matches(&c.params, 8, u128::from(op), &mut env) {
            continue;
        }
        if let Some(g) = &c.guard
            && eval_guard(g, &env) != Some(Val::Bool(true))
        {
            continue;
        }
        return match &c.body {
            Expr::Call(name, _) => name.clone(),
            other => panic!("{decoder} {op:#04x}: clause body is not a call: {other:?}"),
        };
    }
    panic!("{decoder}: no clause takes {op:#04x}");
}

fn base_mnemonics(f: &str) -> &'static [Mnemonic] {
    use Mnemonic as M;
    match f {
        "execute_NOP" => &[M::Nop],
        "execute_STOP" => &[M::Stop],
        "execute_HALT" => &[M::Halt],
        "execute_DI" => &[M::Di],
        "execute_EI" => &[M::Ei],
        "execute_DAA" => &[M::Daa],
        "execute_CPL" => &[M::Cpl],
        "execute_SCF" => &[M::Scf],
        "execute_CCF" => &[M::Ccf],
        "execute_ROT_A" => &[M::Rlca, M::Rrca, M::Rla, M::Rra],
        "execute_INC_r" | "execute_INC_rr" => &[M::Inc],
        "execute_DEC_r" | "execute_DEC_rr" => &[M::Dec],
        "execute_ADD_HL_rr" | "execute_ADD_SP_e" => &[M::Add],
        "execute_ALU_r" | "execute_ALU_n" => {
            &[M::Add, M::Adc, M::Sub, M::Sbc, M::And, M::Xor, M::Or, M::Cp]
        }
        "execute_PUSH" => &[M::Push],
        "execute_POP" => &[M::Pop],
        "execute_JR" | "execute_JR_cc" => &[M::Jr],
        "execute_JP" | "execute_JP_cc" | "execute_JP_HL" => &[M::Jp],
        "execute_CALL" | "execute_CALL_cc" => &[M::Call],
        "execute_RET" | "execute_RET_cc" => &[M::Ret],
        "execute_RETI" => &[M::Reti],
        "execute_RST" => &[M::Rst],
        "__DecodeSM83_CB" => &[M::Prefix],
        "SM83_Illegal" => &[M::Illegal],
        n if n.starts_with("execute_LD") => &[M::Ld],
        _ => &[],
    }
}

fn cb_mnemonics(f: &str) -> &'static [Mnemonic] {
    use Mnemonic as M;
    match f {
        "execute_CB_SHIFT" => &[
            M::Rlc,
            M::Rrc,
            M::Rl,
            M::Rr,
            M::Sla,
            M::Sra,
            M::Swap,
            M::Srl,
        ],
        "execute_CB_BIT" => &[M::Bit],
        "execute_CB_RES" => &[M::Res],
        "execute_CB_SET" => &[M::Set],
        _ => &[],
    }
}

#[test]
fn every_function_and_signature_parses() {
    let m = model();
    let (ok, bad) = m.parse_all();
    assert!(bad.is_empty(), "functions that do not parse: {bad:#?}");
    assert!(ok > 60, "only {ok} functions");
    let mut names: Vec<&String> = m.vals.keys().collect();
    names.sort();
    for n in names {
        let sig = m.sig(n).expect("val is recorded");
        assert!(sig.fun.is_some(), "val {n} does not parse");
    }
    assert_eq!(m.registers.len(), 13);
    assert_eq!(m.enums.get("BranchType").map(Vec::len), Some(3));
}

#[test]
fn decode_clauses_follow_the_opcode_table() {
    let m = model();
    for op in 0..=255u8 {
        let f = decoded(&m, "__DecodeSM83", op);
        let want = BASE[usize::from(op)].mnemonic;
        assert!(
            base_mnemonics(&f).contains(&want),
            "{op:#04x}: decodes to {f}, the table says {want:?}"
        );
        let f = decoded(&m, "__DecodeSM83_CB", op);
        let want = CB[usize::from(op)].mnemonic;
        assert!(
            cb_mnemonics(&f).contains(&want),
            "CB {op:#04x}: decodes to {f}, the table says {want:?}"
        );
    }
}

#[test]
fn clause_patterns_give_their_fixed_bits() {
    let m = model();
    for group in ["__DecodeSM83", "__DecodeSM83_CB"] {
        let clauses = m.func(group).expect("defined").expect("parses");
        let pats = stage::clauses(&m, group, 8);
        let catch_all = clauses.len() - 1;
        let n = if group == "__DecodeSM83" {
            catch_all
        } else {
            clauses.len()
        };
        assert_eq!(pats.len(), n, "{group}: every clause but a catch-all");
        for p in &pats {
            for op in 0..=255u8 {
                let mut env = HashMap::new();
                let takes = matches(&clauses[p.index].params, 8, u128::from(op), &mut env);
                assert_eq!(
                    takes,
                    u32::from(op) & p.mask == p.value,
                    "{group} clause {} on {op:#04x}",
                    p.index
                );
            }
        }
    }
}
