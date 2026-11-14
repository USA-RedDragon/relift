//! The emitted C means what the IR means: random words of every form in
//! `cpus/a64/gen/forms.txt` are lifted, emitted as one-instruction
//! functions, compiled with clang at the lifted-code flags and run on random
//! states, and every register, the flags, guest memory, the exclusive
//! monitor and the exit target must match the reference interpreter on the
//! same states. Skipped with a note when clang is not on `PATH`.

mod common;
mod seq;

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use relift_cpu_a64::decode::{Encoding, Features, decode};
use relift_cpu_a64::frontend::A64;
use relift_emit::{Function, Insn, Options, emit};
use relift_guest::Frontend;
use relift_ir::interp::{FlatMemory, NoIntrinsics, run};
use relift_ir::{Block, Flow, Next, Op};

use common::{
    BASE, CTX, Expect, Harness, Rng, SHARD, STATES, WORDS, diffs, fill, fnv, left_for_runtime,
    pack, slots, state,
};

/// Whether the block leaves through a constant target equal to its own pc,
/// which the one-instruction function would run forever.
fn loops(b: &Block) -> bool {
    match b.exit.next {
        Next::Const(t) => t == b.pc,
        Next::Select {
            taken, not_taken, ..
        } => taken == b.pc || not_taken == b.pc,
        Next::Dynamic(_) => false,
    }
}

#[test]
fn emitted_c_matches_the_interpreter_on_every_form() {
    let Some(cc) = relift_build::find_tool("clang") else {
        eprintln!("skipped: no clang on PATH");
        return;
    };
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let forms = std::fs::read_to_string(root.join("cpus/a64/gen/forms.txt"))
        .expect("forms.txt is readable");
    let by_name: BTreeMap<&str, Encoding> = Encoding::all().map(|e| (e.name(), e)).collect();
    let fe = A64 {
        features: Features::ALL,
    };
    let mut r = Rng(0x5eed_0e0e);
    // Cases spaced 1 GiB apart so no branch reaches another case.
    let mut cases: Vec<(u64, u32, Block)> = Vec::new();
    for name in forms.lines().map(str::trim).filter(|l| !l.is_empty()) {
        let enc = *by_name
            .get(name)
            .unwrap_or_else(|| panic!("form {name} decodes"));
        let mut got = 0;
        for _ in 0..200 {
            if got == WORDS {
                break;
            }
            let pc = 0x40_0000_0000 + (cases.len() as u64) * 0x4000_0000;
            let w = enc.value() | (r.next() as u32 & !enc.mask());
            if decode(w).map(|i| i.encoding) != Some(enc) {
                continue;
            }
            let Ok(b) = fe.lift(&w.to_le_bytes(), pc) else {
                continue;
            };
            let special = b.insts.iter().any(|i| matches!(i.op, Op::Intrinsic(..)));
            if loops(&b) || special || matches!(b.exit.flow, Flow::Trap(_)) {
                continue;
            }
            cases.push((pc, w, b));
            got += 1;
        }
    }
    let slots = slots();
    let mut input = Vec::new();
    let mut expect = Vec::new();
    for (k, (_, w, b)) in cases.iter().enumerate() {
        for _ in 0..STATES {
            let s0 = state(*w, &slots, &mut r);
            let seed = r.next();
            let bytes = fill(seed);
            let mut mem = FlatMemory {
                base: BASE,
                bytes: bytes.clone(),
            };
            let mut s1 = s0.clone();
            let Ok(o) = run(b, &mut s1, &mut mem, &mut NoIntrinsics) else {
                continue;
            };
            input.push(k as u64);
            input.push(seed);
            pack(&s0, &slots, &bytes, &mut input);
            expect.push(Expect {
                case: k,
                state: s1,
                mem: fnv(&mem.bytes),
                next: (o.flow != Flow::Return).then_some(o.next),
            });
        }
    }
    let fns: Vec<Function<'_>> = cases
        .iter()
        .enumerate()
        .map(|(k, (pc, w, b))| Function {
            entry: *pc,
            name: format!("F_{k}"),
            guest_name: None,
            span: 4,
            insts: BTreeMap::from([(
                *pc,
                Insn::Lifted {
                    block: b,
                    raw: u64::from(*w),
                },
            )]),
            tables: BTreeMap::new(),
            pads: Default::default(),
        })
        .collect();
    let out = emit(
        &fe,
        &fns,
        &[],
        &Options {
            shard_bytes: SHARD,
            ..Options::default()
        },
    );
    let dir = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("emit-interp");
    let tc = Harness::new(cc, &root, &dir.join("rt"));
    let files: Vec<(String, String)> = out.files.into_iter().map(|f| (f.name, f.text)).collect();
    let names: Vec<String> = fns.iter().map(|f| f.name.clone()).collect();
    let got = tc.run(&dir.join("run"), &files, &names, &input);
    assert_eq!(got.len(), expect.len() * (4 + CTX), "one record per state");
    let mut compared = 0;
    let mut skipped = 0;
    let mut forms_seen = std::collections::BTreeSet::new();
    let mut bad = Vec::new();
    for (e, rec) in expect.iter().zip(got.as_chunks::<{ 4 + CTX }>().0) {
        let (pc, w, _) = &cases[e.case];
        if left_for_runtime(rec) {
            skipped += 1;
            continue;
        }
        compared += 1;
        forms_seen.insert(decode(*w).map(|i| i.encoding.name()));
        let diffs = diffs(&fe, &slots, e, rec);
        if !diffs.is_empty() {
            bad.push(format!(
                "{w:08x} at {pc:#x} (F_{}): {}",
                e.case,
                diffs.join("; ")
            ));
        }
    }
    eprintln!(
        "{} cases, {compared} runs compared over {} forms, {skipped} left for the runtime",
        cases.len(),
        forms_seen.len()
    );
    assert!(
        bad.is_empty(),
        "{} mismatches:\n{}",
        bad.len(),
        bad.join("\n")
    );
    assert!(
        forms_seen.len() * 10 >= forms.lines().count() * 8,
        "only {} forms compared",
        forms_seen.len()
    );
}
