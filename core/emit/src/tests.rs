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
        name: format!("F_{entry:x}"),
        guest_name: None,
        span: 4 * blocks.len() as u64,
        insts: blocks
            .iter()
            .map(|(pc, b)| (*pc, Insn::Lifted { block: b, raw: 0 }))
            .collect::<BTreeMap<_, _>>(),
        tables: BTreeMap::new(),
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
    let out = emit(&Toy, &[function(0x100, &blocks)], &[], &Options::default());
    let s = shard(&out);
    assert!(s.contains("RL_TEXT RL_LIFTED(F_100)\n{"), "{s}");
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
    let out = emit(&Toy, &fns, &[], &Options::default());
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
    assert!(unit.contains("const rl_unit rl_unit_module = {"));
}

#[test]
fn failures_and_unknown_intrinsics_report_undefined() {
    let fail = relift_guest::LiftFailure {
        insn: 0xdead,
        message: "no".into(),
    };
    let f = Function {
        entry: 0x40,
        name: "F_40".into(),
        guest_name: Some("broken".into()),
        span: 4,
        insts: BTreeMap::from([(0x40, Insn::Failed(&fail))]),
        tables: BTreeMap::new(),
    };
    let out = emit(&Toy, &[f], &[], &Options::default());
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
        &[],
        &Options {
            shard_size: 1,
            unit_name: "t".into(),
            unit_symbol: "u".into(),
            module_base: 0x100,
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

#[test]
fn guards_leave_for_the_slow_path_and_continue_after() {
    let mut b = relift_ir::Builder::new(Toy.regs(), relift_ir::Width::W64, 0x200);
    let x = b.get(relift_ir::Reg(1));
    let z = b.konst(relift_ir::Width::W64, 0);
    let ne = b.cmp(relift_ir::CmpOp::Ne, x, z);
    b.guard(ne, 7);
    let blk = b.jump(4, 0x204);
    let ret = Toy.lift(&word(0x03, 0, 0), 0x204).expect("lifts");
    let f = Function {
        entry: 0x200,
        name: "F_200".into(),
        guest_name: None,
        span: 8,
        insts: BTreeMap::from([
            (
                0x200,
                Insn::Lifted {
                    block: &blk,
                    raw: 0x55,
                },
            ),
            (
                0x204,
                Insn::Lifted {
                    block: &ret,
                    raw: 0,
                },
            ),
        ]),
        tables: BTreeMap::new(),
    };
    let out = emit(&Toy, &[f], &[], &Options::default());
    let s = shard(&out);
    assert!(
        s.contains("if (RL_UNLIKELY(v2)) {\n        RL_STORE();\n        rc = toy_slow(c, mem, UINT64_C(0x200), 0x55u);"),
        "{s}"
    );
    assert!(
        s.contains("        RL_RELOAD();\n        goto L_204;\n    }"),
        "{s}"
    );
}

#[test]
fn jump_tables_branch_to_their_targets_inside_the_function() {
    let code = [word(0x06, 0, 0), word(0x03, 0, 0), word(0x03, 0, 0)];
    let blocks = lift_all(&code, 0x100);
    let mut f = function(0x100, &blocks);
    f.tables.insert(0x100, vec![0x108, 0x104, 0x108, 0x900]);
    let out = emit(&Toy, &[f], &[], &Options::default());
    let s = shard(&out);
    assert!(
        s.contains("    switch (v0) {\n    case UINT64_C(0x104): goto L_104;\n    case UINT64_C(0x108): goto L_108;\n    default: break;\n    }\n    RL_STORE();\n    RL_MUSTTAIL return rl_indirect(c, v0)(c, mem);"),
        "{s}"
    );
}

#[test]
fn shards_carry_a_function_map_and_the_unit_lists_them() {
    let code = [word(0x03, 0, 0)];
    let blocks = lift_all(&code, 0x100);
    let mut f = function(0x100, &blocks);
    f.guest_name = Some("main".into());
    let out = emit(&Toy, &[f], &[], &Options::default());
    let s = shard(&out);
    assert!(s.contains("RL_TEXT_BOUNDS_DECLARE();"), "{s}");
    assert!(
        s.contains("{ UINT64_C(0x100), 4u, RL_FN_MAY_UNWIND, F_100, \"F_100\", \"main\" },"),
        "{s}"
    );
    assert!(s.contains("const rl_fn_map rl_unit_module_map0 = { RL_FN_MAP_VERSION, 1u, fns, \"module\", \"shard0000\", UINT64_C(0x0), RL_TEXT_LO, RL_TEXT_HI };"), "{s}");
    let unit = &out.files.last().expect("unit").text;
    assert!(
        unit.contains("static const rl_fn_map *const maps[] = { &rl_unit_module_map0, };"),
        "{unit}"
    );
    assert!(unit.contains("    1u,\n    maps,\n};"), "{unit}");
}

#[test]
fn calls_to_a_resume_point_override_go_through_the_landing_pad_dispatch() {
    let code = [branch(0x02, 0, 2), word(0x03, 0, 0), word(0x03, 0, 0)];
    let blocks = lift_all(&code, 0x100);
    let fns = [
        function(0x100, &blocks[0..2]),
        function(0x108, &blocks[2..3]),
    ];
    let o = Override {
        entry: 0x108,
        host_fn: "rl_a64_setjmp".into(),
        may_unwind: false,
        resume_point: true,
    };
    let out = emit(&Toy, &fns, &[o], &Options::default());
    let s = shard(&out);
    assert!(!s.contains("RL_LIFTED(F_108)"), "{s}");
    assert!(
        s.contains("rc = rl_a64_setjmp(c, mem);\n    if (RL_UNLIKELY(rc)) {\n        if (c->pend.sp == r2)\n            goto lp_dispatch;\n        return RL_PENDING;\n    }"),
        "{s}"
    );
    assert!(
        s.contains("lp_dispatch:\n    switch (c->pend.pc) {\n    case UINT64_C(0x104):\n        c->pend.kind = RL_PEND_NONE;\n        RL_RELOAD();\n        goto L_104;"),
        "{s}"
    );
    assert!(s.contains("rl_bad_resume(c, mem, UINT64_C(0x100));"), "{s}");
    assert!(
        s.contains("RL_FN_MAY_UNWIND | RL_FN_LANDING_PADS, F_100"),
        "{s}"
    );
    let unit = &out.files.last().expect("unit").text;
    assert!(
        unit.contains("{ UINT64_C(0x108), 0u, RL_FN_OVERRIDE, rl_a64_setjmp },"),
        "{unit}"
    );
}

#[test]
fn map_ranges_stop_at_the_next_function() {
    let code = [branch(0x01, 0, 2), word(0x03, 0, 0), word(0x03, 0, 0)];
    let blocks = lift_all(&code, 0x100);
    let mut a = function(0x100, &blocks[0..1]);
    a.span = 12;
    let b = function(0x104, &blocks[1..2]);
    let out = emit(&Toy, &[a, b], &[], &Options::default());
    let s = shard(&out);
    assert!(
        s.contains("{ UINT64_C(0x100), 4u, RL_FN_MAY_UNWIND, F_100,"),
        "{s}"
    );
    let unit = &out.files.last().expect("unit").text;
    assert!(
        unit.contains("{ UINT64_C(0x100), 12u, RL_FN_MAY_UNWIND, F_100 },"),
        "{unit}"
    );
}

#[test]
fn emit_with_reports_each_shard_against_the_real_total() {
    let code = [word(0x03, 0, 0), word(0x03, 0, 0), word(0x03, 0, 0)];
    let blocks = lift_all(&code, 0x100);
    let fns = [
        function(0x100, &blocks[0..1]),
        function(0x104, &blocks[1..2]),
        function(0x108, &blocks[2..3]),
    ];
    let opts = Options {
        shard_size: 2,
        unit_name: "t".into(),
        unit_symbol: "u".into(),
        module_base: 0x100,
    };
    let mut seen = Vec::new();
    let out = emit_with(&Toy, &fns, &[], &opts, &mut |d, t| seen.push((d, t)));
    assert_eq!(seen, [(1, 2), (2, 2)]);
    assert_eq!(out.files.len(), 4);
}
