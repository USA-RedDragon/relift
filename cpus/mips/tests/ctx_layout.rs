//! The C runtime's VR4300 context (runtime/cpus/mips/include/relift/mips/abi.h)
//! lists the register slots of this crate in slot order, with their names
//! and IR widths: `RL_MIPS_SLOTS(X)` holds one `X(name, field, bits)` per
//! slot. The C test (runtime/cpus/mips/tests/test_mips.c) checks that the
//! fields are in that order in `rl_ctx`.

use relift_cpu_mips::regs;

fn slots_in_header() -> Vec<(String, String, u32)> {
    let path = concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../runtime/cpus/mips/include/relift/mips/abi.h"
    );
    let text = std::fs::read_to_string(path).expect("reads abi.h");
    let start = text
        .find("#define RL_MIPS_SLOTS(X)")
        .expect("abi.h defines RL_MIPS_SLOTS");
    let mut out = Vec::new();
    for line in text[start..].lines().skip(1) {
        let line = line.trim().trim_end_matches('\\').trim();
        let Some(body) = line.strip_prefix("X(").and_then(|l| l.strip_suffix(')')) else {
            break;
        };
        let parts: Vec<&str> = body.split(',').map(str::trim).collect();
        assert_eq!(parts.len(), 3, "slot line {line}");
        out.push((
            parts[0].to_string(),
            parts[1].to_string(),
            parts[2].parse().expect("a width in bits"),
        ));
    }
    out
}

#[test]
fn every_slot_is_in_the_c_context_in_slot_order() {
    let file = regs::regs();
    let slots = slots_in_header();
    assert_eq!(slots.len(), file.len());
    for (i, (name, field, bits)) in slots.iter().enumerate() {
        let d = file.decls()[i];
        assert_eq!(name, d.name, "slot {i}");
        assert_eq!(*bits, d.width.bits(), "slot {i} ({name})");
        println!("{i:2} {name:14} {bits:2} c->{field}");
    }
    assert_eq!(slots[66].1, "fcr31");
    assert_eq!(slots[89].1, "cp0_ll_bit");
}
