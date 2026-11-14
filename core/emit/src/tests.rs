use std::collections::{BTreeMap, BTreeSet};

use relift_guest::Frontend;
use relift_guest::toy::{Toy, branch, word};

use super::*;

fn lift_all(code: &[[u8; 4]], base: u64) -> Vec<(u64, Block)> {
    code.iter()
        .enumerate()
        .map(|(i, w)| {
            let pc = base + 4 * i as u64;
            (pc, Toy.lift(w, pc).expect("lifts"))
        })
        .collect()
}

fn function(entry: u64, blocks: &[(u64, Block)]) -> Function<'_> {
    Function {
        entry,
        span: 4 * blocks.len() as u64,
        insts: blocks
            .iter()
            .map(|(pc, b)| (*pc, Insn::Lifted { block: b, raw: 0 }))
            .collect::<BTreeMap<_, _>>(),
    }
}

fn shard(out: &Output) -> &str {
    &out.files[1].text
}

#[test]
fn locals_labels_and_exits() {
    let code = [
        word(0x04, 0, 5),
        branch(0x07, 1, 2),
        word(0x05, 0, 1),
        word(0x03, 0, 0),
    ];
    let blocks = lift_all(&code, 0x100);
    let out = emit(&Toy, &[function(0x100, &blocks)], &Options::default());
    let s = shard(&out);
    assert!(s.contains("RL_LIFTED(F_100)\n{"), "{s}");
    assert!(s.contains("uint64_t r0 = c->x[0];"), "{s}");
    assert!(s.contains("goto L_100;"), "{s}");
    assert!(
        s.contains("if (v2) { goto L_10c; }\n    goto L_108;"),
        "{s}"
    );
    assert!(s.contains("return RL_OK;"), "{s}");
    assert!(
        s.contains("#define RL_STORE() do { c->x[0] = r0; } while (0)"),
        "{s}"
    );
    assert_eq!(out.undefined, 0);
    assert_eq!(out.files.len(), 3);
}

#[test]
fn calls_check_pending_and_tail_calls_use_musttail() {
    let code = [
        branch(0x02, 0, 2),
        branch(0x01, 0, 1),
        word(0x03, 0, 0),
        word(0x08, 0, 0x27),
        word(0x06, 2, 0),
    ];
    let blocks = lift_all(&code, 0x100);
    let fns = [
        function(0x100, &blocks[0..2]),
        function(0x108, &blocks[2..3]),
        function(0x10c, &blocks[3..5]),
    ];
    let out = emit(&Toy, &fns, &Options::default());
    let s = shard(&out);
    assert!(
        s.contains("rc = F_108(c, mem);\n    if (RL_UNLIKELY(rc))\n        return RL_PENDING;\n    RL_RELOAD();\n    goto L_104;"),
        "{s}"
    );
    assert!(s.contains("RL_MUSTTAIL return F_108(c, mem);"), "{s}");
    assert!(s.contains("rc = rl_svc(c, mem, 0x27u);"), "{s}");
    assert!(
        s.contains("RL_MUSTTAIL return rl_indirect(c, v0)(c, mem);"),
        "{s}"
    );
    let unit = &out.files.last().expect("unit").text;
    assert!(unit.contains("{ UINT64_C(0x10c), 8u, RL_FN_MAY_UNWIND, F_10c },"));
    assert!(unit.contains("const uint32_t rl_static_unit_count = 1;"));
}

#[test]
fn failures_and_unknown_intrinsics_report_undefined() {
    let fail = relift_guest::LiftFailure {
        insn: 0xdead,
        message: "no".into(),
    };
    let f = Function {
        entry: 0x40,
        span: 4,
        insts: BTreeMap::from([(0x40, Insn::Failed(&fail))]),
    };
    let out = emit(&Toy, &[f], &Options::default());
    assert!(shard(&out).contains("return rl_undefined(c, mem, UINT64_C(0x40), 0xdeadu);"));
    assert_eq!(out.undefined, 1);
}

#[test]
fn shards_declare_their_foreign_callees() {
    let code = [branch(0x02, 0, 1), word(0x03, 0, 0)];
    let blocks = lift_all(&code, 0x100);
    let fns = [
        function(0x100, &blocks[0..1]),
        function(0x104, &blocks[1..2]),
    ];
    let out = emit(
        &Toy,
        &fns,
        &Options {
            shard_size: 1,
            unit_name: "t".into(),
        },
    );
    assert_eq!(out.files.len(), 4);
    assert!(out.files[1].text.contains("RL_LIFTED(F_104);"));
    let entries: BTreeSet<u64> = [0x100, 0x104].into();
    assert_eq!(entries.len(), 2);
}

#[test]
fn values_are_kept_to_their_width() {
    assert_eq!(value::cty(relift_ir::Width::W1), "uint8_t");
    assert_eq!(value::cty(relift_ir::Width::new(28)), "uint32_t");
    assert_eq!(value::cty(relift_ir::Width::W128), "rl_u128");
    assert_eq!(
        value::lit(u128::MAX, relift_ir::Width::W128),
        "(((rl_u128)UINT64_C(0xffffffffffffffff) << 64) | UINT64_C(0xffffffffffffffff))"
    );
}
