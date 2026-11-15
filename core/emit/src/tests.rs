use std::collections::{BTreeMap, BTreeSet};

use relift_guest::toy::{Toy, branch, word};
use relift_guest::{Frontend, Home, LiftFailure};
use relift_ir::{FpOp, Reg, RegFile, Width};

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
        pads: BTreeSet::new(),
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
    assert!(s.contains("if (v2) { goto L_10c; }\n}\nL_108: {"), "{s}");
    assert!(s.contains("return RL_OK;"), "{s}");
    assert!(
        s.contains("#define RL_STORE() do { c->x[0] = r0; } while (0)"),
        "{s}"
    );
    assert_eq!(out.undefined, 0);
    assert_eq!(out.files.len(), 3);
}

#[test]
fn registers_in_the_context_are_macros_with_nothing_to_store() {
    let code = [
        word(0x04, 0, 5),
        branch(0x07, 1, 2),
        word(0x05, 0, 1),
        word(0x03, 0, 0),
    ];
    let blocks = lift_all(&code, 0x100);
    let opts = Options {
        regs_in_context: true,
        ..Options::default()
    };
    let out = emit(&Toy, &[function(0x100, &blocks)], &[], &opts);
    let s = shard(&out);
    assert!(s.contains("#define r0 (c->x[0])"), "{s}");
    assert!(!s.contains("uint64_t r0 ="), "{s}");
    assert!(s.contains("#define RL_STORE() do { } while (0)"), "{s}");
    assert!(s.contains("#undef r0\n#undef r1\n#undef r3\n}"), "{s}");
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
        s.contains("rc = F_108(c, mem);\n    if (RL_UNLIKELY(rc))\n        return RL_PENDING;\n    RL_RELOAD();\n}\nL_104: {"),
        "{s}"
    );
    assert!(s.contains("RL_MUSTTAIL return F_108(c, mem);"), "{s}");
    assert!(s.contains("rc = rl_svc(c, mem, 0x27u);"), "{s}");
    assert!(
        s.contains("RL_MUSTTAIL return rl_indirect(c, r2)(c, mem);"),
        "{s}"
    );
    let unit = &out.files.last().expect("unit").text;
    assert!(unit.contains("{ UINT64_C(0x10c), 8u, RL_FN_MAY_UNWIND, F_10c },"));
    assert!(unit.contains("const rl_unit rl_unit_module = {"));
    assert!(unit.contains("{ 0x00, 0x00, "), "{unit}");
}

#[test]
fn a_loop_back_edge_fences_and_a_forward_branch_does_not() {
    let code = [
        word(0x0a, 0, 1),
        word(0x0d, 0, 0),
        branch(0x0e, 0, -2),
        branch(0x07, 0, 2),
        word(0x09, 0, 1),
        word(0x03, 0, 0),
    ];
    let blocks = lift_all(&code, 0x100);
    let out = emit(&Toy, &[function(0x100, &blocks)], &[], &Options::default());
    let s = shard(&out);
    let at = |pc: u64| {
        let from = s.find(&format!("L_{pc:x}: {{")).expect("label");
        &s[from..from + s[from..].find("\n}\n").expect("end")]
    };
    assert!(
        at(0x108).contains("    RL_LOOP_FENCE();\n    if (r4) { goto L_100; }"),
        "{s}"
    );
    assert!(!at(0x10c).contains("RL_LOOP_FENCE"), "{s}");
    assert_eq!(s.matches("RL_LOOP_FENCE();").count(), 1, "{s}");
}

/// The toy frontend with a preemption clock.
struct Preempt;

impl Frontend for Preempt {
    fn granule(&self) -> u64 {
        Toy.granule()
    }
    fn regs(&self) -> RegFile {
        Toy.regs()
    }
    fn stack_pointer(&self) -> Option<Reg> {
        Toy.stack_pointer()
    }
    fn preserved(&self) -> Vec<Reg> {
        Toy.preserved()
    }
    fn lift(&self, bytes: &[u8], pc: u64) -> Result<Block, LiftFailure> {
        Toy.lift(bytes, pc)
    }
    fn abi_headers(&self) -> &'static [&'static str] {
        Toy.abi_headers()
    }
    fn home(&self, reg: Reg) -> Home {
        Toy.home(reg)
    }
    fn intrinsic(&self, name: &str, args: &[String]) -> Option<String> {
        Toy.intrinsic(name, args)
    }
    fn trap(&self, code: u32) -> String {
        Toy.trap(code)
    }
    fn undefined(&self, pc: u64, insn: u64) -> String {
        Toy.undefined(pc, insn)
    }
    fn monitor(&self) -> &'static str {
        Toy.monitor()
    }
    fn fp(
        &self,
        op: FpOp,
        from: Width,
        to: Width,
        args: &[String],
        reg: &dyn Fn(Reg) -> String,
    ) -> Option<String> {
        Toy.fp(op, from, to, args, reg)
    }
    fn preempt_clock(&self) -> Option<&'static str> {
        Some("c->time")
    }
    fn fp_state(&self) -> Vec<Reg> {
        Toy.fp_state()
    }
    fn guard_state(&self, code: u32) -> Option<Reg> {
        Toy.guard_state(code)
    }
    fn guard_slow(&self, code: u32, pc: u64, insn: u64) -> Option<String> {
        Toy.guard_slow(code, pc, insn)
    }
}

#[test]
fn loops_with_work_computed_jumps_and_calls_back_are_preemption_points() {
    let code = [
        word(0x0a, 0, 1),    // 0x100 ld r0, [r1]
        word(0x0d, 0, 0),    // 0x104 cmpz r0
        branch(0x0e, 0, -2), // 0x108 bz 0x100: a loop without work
        branch(0x02, 0, 3),  // 0x10c bl 0x118
        branch(0x07, 0, -4), // 0x110 cbz r0, 0x100: a loop with a call
        word(0x06, 0, 0),    // 0x114 br r0
        branch(0x02, 0, -6), // 0x118 bl 0x100: a call back
        word(0x03, 0, 0),    // 0x11c ret
    ];
    let blocks = lift_all(&code, 0x100);
    let out = emit(
        &Preempt,
        &[function(0x100, &blocks)],
        &[],
        &Options::default(),
    );
    let s = shard(&out);
    let at = |pc: u64| {
        let from = s.find(&format!("L_{pc:x}: {{")).expect("label");
        &s[from..from + s[from..].find("\n}\n").expect("end")]
    };
    let check = "    if (RL_UNLIKELY(rl_time_spent(&c->time)))\n        rl_preempt(c);\n";
    assert!(
        at(0x108).contains(&format!("{check}    RL_LOOP_FENCE();")),
        "{s}"
    );
    assert!(
        at(0x110).contains(&format!("{check}    RL_LOOP_FENCE();")),
        "{s}"
    );
    assert!(at(0x114).contains(check), "{s}");
    assert!(
        at(0x118).contains(&format!("{check}    RL_STORE();\n    rc = F_100(c, mem);")),
        "{s}"
    );
    assert_eq!(s.matches("rl_time_spent").count(), 4, "{s}");
    let plain = emit(&Toy, &[function(0x100, &blocks)], &[], &Options::default());
    assert!(!shard(&plain).contains("rl_time_spent"));
}

#[test]
fn a_loop_that_leaves_on_a_loaded_value_waits_on_memory() {
    let code = [
        word(0x0a, 2, 1),    // 0x100 ld r2, [r1]
        word(0x0d, 2, 0),    // 0x104 cmpz r2
        branch(0x0e, 0, 2),  // 0x108 bz 0x110
        branch(0x01, 0, -3), // 0x10c b 0x100
        word(0x03, 0, 0),    // 0x110 ret
    ];
    let blocks = lift_all(&code, 0x100);
    let out = emit(
        &Preempt,
        &[function(0x100, &blocks)],
        &[],
        &Options::default(),
    );
    assert_eq!(shard(&out).matches("rl_time_spent").count(), 1);
}

#[test]
fn a_loop_that_leaves_on_a_value_loaded_from_moving_addresses_does_not_wait() {
    let code = [
        word(0x0a, 2, 1),    // 0x100 ld r2, [r1]
        word(0x09, 1, 8),    // 0x104 addi r1, 8
        word(0x0d, 2, 0),    // 0x108 cmpz r2
        branch(0x0e, 0, 2),  // 0x10c bz 0x114
        branch(0x01, 0, -4), // 0x110 b 0x100
        word(0x03, 0, 0),    // 0x114 ret
    ];
    let blocks = lift_all(&code, 0x100);
    let out = emit(
        &Preempt,
        &[function(0x100, &blocks)],
        &[],
        &Options::default(),
    );
    assert!(!shard(&out).contains("rl_time_spent"));
}

#[test]
fn a_counted_loop_without_calls_has_no_preemption_point() {
    let code = [
        word(0x0a, 2, 1),      // 0x100 ld r2, [r1]
        word(0x09, 0, 0xffff), // 0x104 addi r0, -1
        word(0x0d, 0, 0),      // 0x108 cmpz r0
        branch(0x0e, 0, 2),    // 0x10c bz 0x114
        branch(0x01, 0, -4),   // 0x110 b 0x100
        word(0x03, 0, 0),      // 0x114 ret
    ];
    let blocks = lift_all(&code, 0x100);
    let out = emit(
        &Preempt,
        &[function(0x100, &blocks)],
        &[],
        &Options::default(),
    );
    let s = shard(&out);
    assert!(!s.contains("rl_time_spent"), "{s}");
    assert!(s.contains("RL_LOOP_FENCE();"), "{s}");
}

#[test]
fn a_jump_to_an_entry_inside_the_function_stays_a_goto() {
    // 0x104 is another function's entry (an address-taken label) and also
    // part of this function's extent: the loop back to it keeps the locals.
    let code = [
        word(0x04, 0, 5),
        word(0x09, 0, 1),
        branch(0x07, 0, -1),
        word(0x03, 0, 0),
    ];
    let blocks = lift_all(&code, 0x100);
    let fns = [function(0x100, &blocks), function(0x104, &blocks[1..])];
    let out = emit(&Toy, &fns, &[], &Options::default());
    let s = shard(&out);
    let body = &s[s.find("RL_LIFTED(F_100)\n{").expect("F_100")..];
    let body = &body[..body.find("RL_LIFTED(F_104)\n{").unwrap_or(body.len())];
    assert!(body.contains("goto L_104;"), "{body}");
    assert!(!body.contains("return F_104(c, mem);"), "{body}");
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
        pads: BTreeSet::new(),
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
            shard_bytes: 1,
            piece_bytes: PIECE_BYTES,
            unit_name: "t".into(),
            unit_symbol: "u".into(),
            module_base: 0x100,
            parts: false,
            io_map: relift_ir::IoMap::memory(),
            image_id: [0; 32],
            test_fault: None,
            window: 0,
            enter_anywhere: None,
            regs_in_context: false,
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
        "(~(rl_u128)0)"
    );
    assert_eq!(
        value::lit(5, relift_ir::Width::W128),
        "((rl_u128)UINT64_C(0x5))"
    );
    assert_eq!(
        value::lit(1 << 64, relift_ir::Width::W128),
        "(((rl_u128)UINT64_C(0x1) << 64) | UINT64_C(0x0))"
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
        pads: BTreeSet::new(),
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
        s.contains("    switch (r0) {\n    case UINT64_C(0x104): goto L_104;\n    case UINT64_C(0x108): goto L_108;\n    default: break;\n    }\n    RL_STORE();\n    RL_MUSTTAIL return rl_indirect(c, r0)(c, mem);"),
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
        s.contains("{ UINT64_C(0x100), UINT64_C(0x100), 4u, RL_FN_MAY_UNWIND, F_100, \"F_100\", \"main\" },"),
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
        declare: false,
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
        s.contains("{ UINT64_C(0x100), UINT64_C(0x100), 4u, RL_FN_MAY_UNWIND, F_100,"),
        "{s}"
    );
    let unit = &out.files.last().expect("unit").text;
    assert!(
        unit.contains("{ UINT64_C(0x100), 12u, RL_FN_MAY_UNWIND, F_100 },"),
        "{unit}"
    );
}

#[test]
fn emit_with_reports_the_functions_done_after_each_shard() {
    let code = [word(0x03, 0, 0), word(0x03, 0, 0), word(0x03, 0, 0)];
    let blocks = lift_all(&code, 0x100);
    let fns = [
        function(0x100, &blocks[0..1]),
        function(0x104, &blocks[1..2]),
        function(0x108, &blocks[2..3]),
    ];
    let opts = Options {
        shard_bytes: 1,
        piece_bytes: PIECE_BYTES,
        unit_name: "t".into(),
        unit_symbol: "u".into(),
        module_base: 0x100,
        parts: false,
        io_map: relift_ir::IoMap::memory(),
        image_id: [7; 32],
        test_fault: None,
        window: 0,
        enter_anywhere: None,
        regs_in_context: false,
    };
    let mut seen = Vec::new();
    let out = emit_with(&Toy, &fns, &[], &opts, &mut |d, t| seen.push((d, t)));
    assert_eq!(seen, [(1, 3), (2, 3), (3, 3)]);
    assert_eq!(out.files.len(), 5);
    let unit = &out.files.last().expect("unit").text;
    assert!(
        unit.contains(&format!("{{ {} }}", ["0x07"; 32].join(", "))),
        "{unit}"
    );
}

#[test]
fn a_large_function_is_split_into_pieces_in_shards_of_their_own() {
    let code = [
        word(0x04, 0, 5),
        branch(0x07, 1, 2),
        word(0x05, 0, 1),
        branch(0x01, 0, -3),
        word(0x03, 0, 0),
    ];
    let blocks = lift_all(&code, 0x100);
    let fns = [
        function(0x100, &blocks[0..4]),
        function(0x110, &blocks[4..5]),
    ];
    let opts = Options {
        shard_bytes: 1200,
        piece_bytes: 1,
        ..Options::default()
    };
    let out = emit(&Toy, &fns, &[], &opts);
    let all: String = out.files.iter().map(|f| f.text.as_str()).collect();
    // The stub keeps the guest function's name and enters its piece.
    assert!(
        all.contains("RL_TEXT RL_LIFTED(F_100)\n{\n    c->pc = UINT64_C(0x100);\n    RL_MUSTTAIL return F_100_part1(c, mem);\n}"),
        "{all}"
    );
    // One piece per instruction; each enters at its own instruction and
    // leaves for the next through the context.
    for k in 1..=4 {
        assert!(
            all.contains(&format!("RL_TEXT RL_LIFTED(F_100_part{k})")),
            "{all}"
        );
    }
    assert!(
        all.contains("    switch (c->pc) {\n    case UINT64_C(0x104): goto L_104;\n    default: goto L_104;\n    }"),
        "{all}"
    );
    assert!(
        all.contains(
            "RL_STORE(); c->pc = UINT64_C(0x104); RL_MUSTTAIL return F_100_part2(c, mem);"
        ),
        "{all}"
    );
    // The back edge from the last piece enters the second at 0x104.
    assert!(
        all.contains("if (v2) { RL_STORE(); c->pc = UINT64_C(0x10c); RL_MUSTTAIL return F_100_part4(c, mem); }"),
        "{all}"
    );
    let shards: Vec<&File> = out
        .files
        .iter()
        .filter(|f| f.name.starts_with("shard"))
        .collect();
    assert!(shards.len() >= 3, "{}", shards.len());
    // Every piece has a map entry over the guest range it holds, naming
    // the guest function; the stub's entry has an empty range.
    let maps: String = shards.iter().map(|s| s.text.as_str()).collect();
    let entry = |range: &str, host: &str| {
        let want = format!("{range}, RL_FN_MAY_UNWIND, {host}, \"{host}\"");
        assert_eq!(maps.matches(&want).count(), 1, "{want}\n{maps}");
    };
    entry("{ UINT64_C(0x100), UINT64_C(0x100), 0u", "F_100");
    entry("{ UINT64_C(0x100), UINT64_C(0x100), 4u", "F_100_part1");
    entry("{ UINT64_C(0x104), UINT64_C(0x100), 4u", "F_100_part2");
    entry("{ UINT64_C(0x108), UINT64_C(0x100), 4u", "F_100_part3");
    entry("{ UINT64_C(0x10c), UINT64_C(0x100), 4u", "F_100_part4");
    entry("{ UINT64_C(0x110), UINT64_C(0x110), 4u", "F_110");
    assert!(
        shards[0]
            .text
            .contains("RL_FN_MAY_UNWIND, F_100, \"F_100\"")
    );
    let unit = &out.files.last().expect("unit").text;
    assert!(
        unit.contains("{ UINT64_C(0x100), 16u, RL_FN_MAY_UNWIND, F_100 },"),
        "{unit}"
    );
    assert!(!unit.contains("part"), "{unit}");
}

#[test]
fn shards_close_at_their_byte_budget() {
    let code = [word(0x03, 0, 0), word(0x03, 0, 0), word(0x03, 0, 0)];
    let blocks = lift_all(&code, 0x100);
    let fns = [
        function(0x100, &blocks[0..1]),
        function(0x104, &blocks[1..2]),
        function(0x108, &blocks[2..3]),
    ];
    let one = emit(&Toy, &fns, &[], &Options::default());
    assert_eq!(one.files.len(), 3, "all in one shard by default");
    let each = emit(
        &Toy,
        &fns,
        &[],
        &Options {
            shard_bytes: 1,
            ..Options::default()
        },
    );
    assert_eq!(
        each.files.len(),
        5,
        "one function per shard at a tiny budget"
    );
}

#[test]
fn a_shard_in_parts_defines_and_maps_each_function_in_one_part() {
    let code = [word(0x03, 0, 0), word(0x03, 0, 0)];
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
            unit_symbol: "u".into(),
            parts: true,
            ..Options::default()
        },
    );
    let s = shard(&out);
    assert!(s.contains("#include \"relift_parts.h\"\n#ifndef RL_PART\n#define RL_PART 0\n#endif"));
    assert!(
        s.contains("#if defined(RL_PART_F_100) == RL_PART\n\nRL_TEXT RL_LIFTED(F_100)"),
        "{s}"
    );
    assert!(
        s.contains("#if defined(RL_PART_F_104) == RL_PART\n    { UINT64_C(0x104)"),
        "{s}"
    );
    assert!(s.contains("#define RL_PART_MAP u_map0_p1"));
    assert!(s.contains("(uint32_t)(sizeof fns / sizeof fns[0]) - 1u"));
    assert_eq!(
        out.files[1].fns,
        [(0x100, "F_100".to_string()), (0x104, "F_104".to_string())]
    );
    let unit = &out.files.last().expect("unit").text;
    assert!(
        unit.contains("#ifdef RL_PART1_shard0000\n &u_map0_p1,\n#endif"),
        "{unit}"
    );
    assert_eq!(
        parts_header(&[("shard0000.c", vec!["F_104"]), ("shard0001.c", vec![])]),
        "/* Generated by the build: the part-1 functions of each shard. */\n#define RL_PART1_shard0000 1\n#define RL_PART_F_104 1\n"
    );
}

#[test]
fn a_split_function_entered_above_its_first_instruction_keeps_its_map_sorted() {
    let code = [
        word(0x04, 0, 5),
        word(0x05, 0, 1),
        word(0x05, 0, 1),
        word(0x03, 0, 0),
    ];
    let blocks = lift_all(&code, 0x100);
    let fns = [function(0x108, &blocks[0..4])];
    let opts = Options {
        shard_bytes: 1200,
        piece_bytes: 1,
        ..Options::default()
    };
    let out = emit(&Toy, &fns, &[], &opts);
    let maps: String = out.files.iter().map(|f| f.text.as_str()).collect();
    let want = |range: &str, host: &str| {
        let line = format!("{range}, RL_FN_MAY_UNWIND, {host}, \"{host}\"");
        assert_eq!(maps.matches(&line).count(), 1, "{line}\n{maps}");
    };
    want("{ UINT64_C(0x100), UINT64_C(0x108), 0u", "F_108");
    want("{ UINT64_C(0x100), UINT64_C(0x108), 4u", "F_108_part1");
    want("{ UINT64_C(0x104), UINT64_C(0x108), 4u", "F_108_part2");
    want("{ UINT64_C(0x10c), UINT64_C(0x108), 12u", "F_108_part4");
}

#[test]
fn a_split_function_in_parts_lands_whole_in_its_part() {
    let code = [
        word(0x04, 0, 5),
        branch(0x07, 1, 2),
        word(0x05, 0, 1),
        branch(0x01, 0, -3),
        word(0x03, 0, 0),
    ];
    let blocks = lift_all(&code, 0x100);
    let fns = [
        function(0x100, &blocks[0..4]),
        function(0x110, &blocks[4..5]),
    ];
    let opts = Options {
        shard_bytes: 1,
        piece_bytes: 1,
        parts: true,
        ..Options::default()
    };
    let out = emit(&Toy, &fns, &[], &opts);
    let shards: Vec<&File> = out
        .files
        .iter()
        .filter(|f| f.name.starts_with("shard"))
        .collect();
    let holding: Vec<&&File> = shards
        .iter()
        .filter(|f| f.text.contains("RL_TEXT RL_LIFTED(F_100"))
        .collect();
    assert_eq!(holding.len(), 5);
    let mut defs = 0;
    for f in &holding {
        // The build puts a shard in part 1 by the names in File::fns.
        assert!(f.fns.contains(&(0x100, "F_100".into())), "{:?}", f.fns);
        let lines: Vec<&str> = f.text.lines().collect();
        for (i, l) in lines.iter().enumerate() {
            if l.starts_with("RL_TEXT RL_LIFTED(F_100") || l.starts_with("    { UINT64_C(0x1") {
                let guard = lines[..i]
                    .iter()
                    .rev()
                    .find(|l| l.starts_with("#if ") || l.starts_with("#endif"));
                assert_eq!(
                    guard,
                    Some(&"#if defined(RL_PART_F_100) == RL_PART"),
                    "{}",
                    f.text
                );
                defs += 1;
            }
        }
    }
    // The stub, four pieces and one map entry for each of them.
    assert_eq!(defs, 10);
    let header = parts_header(
        &holding
            .iter()
            .map(|f| (f.name.as_str(), vec!["F_100"]))
            .collect::<Vec<_>>(),
    );
    for f in &holding {
        assert!(
            header.contains(&format!(
                "#define RL_PART1_{} 1",
                f.name.trim_end_matches(".c")
            )),
            "{header}"
        );
    }
}

#[test]
fn a_write_every_path_overwrites_before_a_read_is_dropped() {
    let code = [
        word(0x0d, 0, 0),
        word(0x0d, 1, 0),
        branch(0x0e, 0, 2),
        word(0x03, 0, 0),
        word(0x03, 0, 0),
    ];
    let blocks = lift_all(&code, 0x100);
    let out = emit(&Toy, &[function(0x100, &blocks)], &[], &Options::default());
    let s = shard(&out);
    let body = |pc: &str| {
        let start = s.find(&format!("L_{pc}: {{")).expect("the label");
        let end = s[start..].find("\n}\n").expect("the block end");
        s[start..start + end].to_string()
    };
    // The first compare's zero flag is overwritten by the second before
    // anything reads or stores it; the second's is read by the branch.
    assert!(!body("100").contains("r4 ="), "{s}");
    assert!(body("104").contains("r4 ="), "{s}");
}

#[test]
fn landing_pads_are_resume_points_with_a_table_entry_of_their_own() {
    let code = [
        branch(0x02, 0, 3),
        word(0x03, 0, 0),
        word(0x03, 0, 0),
        word(0x03, 0, 0),
    ];
    let blocks = lift_all(&code, 0x100);
    let mut f = function(0x100, &blocks[0..3]);
    f.pads = BTreeSet::from([0x108]);
    let g = function(0x10c, &blocks[3..4]);
    let out = emit(&Toy, &[f, g], &[], &Options::default());
    let s = shard(&out);
    assert!(
        s.contains("if (c->pend.sp == r2)\n            goto lp_dispatch;"),
        "{s}"
    );
    assert!(
        s.contains("lp_dispatch:\n    switch (c->pend.pc) {\n    case UINT64_C(0x108):\n        c->pend.kind = RL_PEND_NONE;\n        RL_RELOAD();\n        goto L_108;"),
        "{s}"
    );
    assert!(
        s.contains("RL_FN_MAY_UNWIND | RL_FN_LANDING_PADS, F_100"),
        "{s}"
    );
    let unit = &out.files.last().expect("unit").text;
    assert!(
        unit.contains(
            "{ UINT64_C(0x108), 0u, RL_FN_MAY_UNWIND | RL_FN_FRAGMENT, rl_toy_pad_resume },"
        ),
        "{unit}"
    );
}

#[test]
fn a_declared_override_is_declared_where_it_is_called() {
    let code = [branch(0x02, 0, 2), word(0x03, 0, 0), word(0x03, 0, 0)];
    let blocks = lift_all(&code, 0x100);
    let fns = [function(0x100, &blocks[0..2])];
    let o = Override {
        entry: 0x108,
        host_fn: "rl_modsw_108".into(),
        may_unwind: true,
        resume_point: false,
        declare: true,
    };
    let out = emit(&Toy, &fns, &[o], &Options::default());
    let s = shard(&out);
    assert!(s.contains("RL_LIFTED(rl_modsw_108);"), "{s}");
    assert!(s.contains("rc = rl_modsw_108(c, mem);"), "{s}");
    let unit = &out.files.last().expect("unit").text;
    assert!(unit.contains("RL_LIFTED(rl_modsw_108);"), "{unit}");
}

#[test]
fn shards_hold_one_window_each_and_are_named_by_it() {
    let code = [word(0x03, 0, 0); 3];
    let mut blocks = lift_all(&code[..2], 0x100);
    blocks.extend(lift_all(&code[2..], 0x140));
    let fns = [
        function(0x100, &blocks[0..1]),
        function(0x104, &blocks[1..2]),
        function(0x140, &blocks[2..3]),
    ];
    let names = |opts: &Options| -> Vec<String> {
        let out = emit(&Toy, &fns, &[], opts);
        out.files.iter().map(|f| f.name.clone()).collect()
    };
    let windowed = Options {
        module_base: 0x100,
        window: 0x40,
        ..Options::default()
    };
    assert_eq!(
        names(&windowed),
        [PRELUDE_NAME, "shard0000.c", "shard0001.c", "unit.c"]
    );
    let tiny = Options {
        shard_bytes: 1,
        parts: true,
        ..windowed
    };
    assert_eq!(
        names(&tiny),
        [
            PRELUDE_NAME,
            "shard0000.c",
            "shard0000_1.c",
            "shard0001.c",
            "unit.c"
        ]
    );
    let out = emit(&Toy, &fns, &[], &tiny);
    assert!(
        out.files[2]
            .text
            .contains("RL_PART_MAP rl_unit_module_map0_1\n")
    );
    let unit = &out.files[4].text;
    assert!(
        unit.contains(
            "#ifdef RL_PART1_shard0000_1\nextern const rl_fn_map rl_unit_module_map0_1_p1;"
        ),
        "{unit}"
    );
}

#[derive(Default)]
struct Mem {
    kept: std::sync::Mutex<BTreeMap<Key, Vec<u8>>>,
    reused: std::sync::Mutex<Vec<(u64, bool)>>,
}

impl Store for Mem {
    fn key(&self, input: &[u8]) -> Key {
        let mut k = [0u8; 32];
        for (i, b) in input.iter().enumerate() {
            k[i % 32] = k[i % 32].wrapping_mul(31).wrapping_add(*b);
        }
        k[0] ^= input.len() as u8;
        k
    }

    fn get(&self, key: &Key) -> Option<Vec<u8>> {
        self.kept.lock().unwrap().get(key).cloned()
    }

    fn keep(&self, entry: u64, key: &Key, output: Vec<u8>, reused: bool) {
        self.kept.lock().unwrap().insert(*key, output);
        self.reused.lock().unwrap().push((entry, reused));
    }
}

fn emit_with_store(fns: &[Function<'_>], overrides: &[Override], store: &Mem) -> Vec<File> {
    emit_with_store_opts(fns, overrides, store, &Options::default())
}

fn emit_with_store_opts(
    fns: &[Function<'_>],
    overrides: &[Override],
    store: &Mem,
    opts: &Options,
) -> Vec<File> {
    let heads: Vec<Head> = fns.iter().map(Head::of).collect();
    let lend = |ks: &[usize], sink: &mut dyn FnMut(usize, &Function<'_>)| {
        for &k in ks {
            sink(k, &fns[k]);
        }
    };
    let mut files = Vec::new();
    store.reused.lock().unwrap().clear();
    let from = Stream {
        heads: &heads,
        lend: &lend,
        store: Some(store),
    };
    emit_reusing(&Toy, &from, overrides, opts, &mut |_, _| {}, &mut |f| {
        files.push(f)
    });
    files
}

#[test]
fn a_store_gives_back_the_c_of_unchanged_functions() {
    let code = [
        branch(0x02, 0, 2),
        word(0x03, 0, 0),
        word(0x03, 0, 0),
        word(0x04, 1, 7),
        word(0x03, 0, 0),
    ];
    let blocks = lift_all(&code, 0x100);
    let fns = [
        function(0x100, &blocks[0..2]),
        function(0x108, &blocks[2..3]),
        function(0x10c, &blocks[3..5]),
    ];
    let store = Mem::default();
    let first = emit_with_store(&fns, &[], &store);
    assert_eq!(first, emit(&Toy, &fns, &[], &Options::default()).files);
    assert!(store.reused.lock().unwrap().iter().all(|r| !r.1));
    let again = emit_with_store(&fns, &[], &store);
    assert_eq!(again, first);
    assert_eq!(
        *store.reused.lock().unwrap(),
        [(0x100, true), (0x108, true), (0x10c, true)]
    );
    let other = lift_all(&[word(0x04, 1, 8), word(0x03, 0, 0)], 0x10c);
    let changed = [
        function(0x100, &blocks[0..2]),
        function(0x108, &blocks[2..3]),
        function(0x10c, &other),
    ];
    let out = emit_with_store(&changed, &[], &store);
    assert_eq!(out, emit(&Toy, &changed, &[], &Options::default()).files);
    assert_eq!(
        *store.reused.lock().unwrap(),
        [(0x100, true), (0x108, true), (0x10c, false)]
    );
    let host = [Override {
        entry: 0x108,
        host_fn: "host_108".into(),
        may_unwind: true,
        resume_point: true,
        declare: false,
    }];
    let out = emit_with_store(&fns, &host, &store);
    assert_eq!(out, emit(&Toy, &fns, &host, &Options::default()).files);
    assert_eq!(
        *store.reused.lock().unwrap(),
        [(0x100, false), (0x10c, true)],
        "the caller of an overridden function is emitted again"
    );
}

#[test]
fn a_store_keeps_apart_the_c_of_functions_entered_anywhere() {
    let code = [word(0x03, 0, 0), word(0x03, 0, 0), word(0x03, 0, 0)];
    let blocks = lift_all(&code, 0x100);
    let fns = [function(0x100, &blocks)];
    let anywhere = Options {
        enter_anywhere: Some(u64::MAX),
        ..Options::default()
    };
    let store = Mem::default();
    let plain = emit_with_store(&fns, &[], &store);
    let out = emit_with_store_opts(&fns, &[], &store, &anywhere);
    assert_eq!(out, emit(&Toy, &fns, &[], &anywhere).files);
    assert_ne!(out, plain);
    assert_eq!(*store.reused.lock().unwrap(), [(0x100, false)]);
    let again = emit_with_store_opts(&fns, &[], &store, &anywhere);
    assert_eq!(again, out);
    assert_eq!(*store.reused.lock().unwrap(), [(0x100, true)]);
}

#[test]
fn a_function_entered_anywhere_lists_each_instruction_with_its_piece() {
    let code = [word(0x03, 0, 0), word(0x03, 0, 0), word(0x03, 0, 0)];
    let blocks = lift_all(&code, 0x100);
    let fns = [function(0x100, &blocks)];
    let out = emit(
        &Toy,
        &fns,
        &[],
        &Options {
            unit_symbol: "u".into(),
            enter_anywhere: Some(u64::MAX),
            ..Options::default()
        },
    );
    let unit = &out
        .files
        .iter()
        .find(|f| f.name == "unit.c")
        .expect("unit")
        .text;
    for pc in [0x104, 0x108] {
        assert!(
            unit.contains(&format!(
                "{{ UINT64_C({pc:#x}), 0u, RL_FN_MAY_UNWIND | RL_FN_FRAGMENT, F_100_part1 }},"
            )),
            "{unit}"
        );
    }
    let c = shard(&out);
    assert!(c.contains("case UINT64_C(0x108): goto L_108;"), "{c}");
    assert_eq!(out.files.len(), 3, "the pieces share the shard");
}

#[test]
fn a_function_without_its_entry_instruction_is_not_split_to_be_entered_anywhere() {
    let code = [word(0x03, 0, 0), word(0x03, 0, 0), word(0x03, 0, 0)];
    let blocks = lift_all(&code, 0x100);
    let fns = [function(0x100, &blocks[1..])];
    let out = emit(
        &Toy,
        &fns,
        &[],
        &Options {
            enter_anywhere: Some(u64::MAX),
            ..Options::default()
        },
    );
    assert!(!shard(&out).contains("_part1"), "{}", shard(&out));
}

#[test]
fn a_continuation_stays_in_the_piece_of_its_instruction() {
    let code = [word(0x03, 0, 0), word(0x03, 0, 0), word(0x03, 0, 0)];
    let mut blocks = lift_all(&code, 0x100);
    let mut cont = blocks[1].1.clone();
    cont.pc = 0x1_0104;
    blocks.push((0x1_0104, cont));
    let fns = [function(0x100, &blocks)];
    let out = emit(
        &Toy,
        &fns,
        &[],
        &Options {
            unit_symbol: "u".into(),
            piece_bytes: 1,
            enter_anywhere: Some(0xFFFF),
            ..Options::default()
        },
    );
    let c: String = out.files.iter().map(|f| f.text.as_str()).collect();
    let piece = c
        .split("RL_TEXT RL_LIFTED(")
        .find(|p| p.contains("L_104: {"))
        .expect("the piece of 0x104");
    assert!(piece.contains("L_10104: {"), "{c}");
    let unit = &out
        .files
        .iter()
        .find(|f| f.name == "unit.c")
        .expect("unit")
        .text;
    assert!(!unit.contains("0x10104"), "{unit}");
}
