//! The frontend's context names and the C runtime's `rl_ctx` agree.
//!
//! The header `runtime/cpus/sm83/include/relift/sm83/abi.h` pins every offset
//! with `_Static_assert`, so reading the header here checks the frontend
//! against what the C compiler checks.

use std::collections::BTreeMap;

use relift_cpu_sm83::{Sm83, regs};
use relift_guest::{Frontend, Home};
use relift_ir::{IoMap, Width};

const HEADER: &str = include_str!("../../../runtime/cpus/sm83/include/relift/sm83/abi.h");

fn offsets() -> BTreeMap<String, u64> {
    let mut out = BTreeMap::new();
    for line in HEADER.lines() {
        let Some(rest) = line.trim().strip_prefix("_Static_assert(offsetof(rl_ctx, ") else {
            continue;
        };
        let Some((name, rest)) = rest.split_once(") == ") else {
            continue;
        };
        let Some((value, _)) = rest.split_once(',') else {
            continue;
        };
        if let Ok(v) = value.parse() {
            out.insert(name.to_string(), v);
        }
    }
    out
}

fn field_types() -> BTreeMap<String, String> {
    let body = HEADER
        .split_once("struct rl_ctx {")
        .and_then(|(_, rest)| rest.split_once("\n};"))
        .map(|(b, _)| b)
        .unwrap_or_default();
    let mut out = BTreeMap::new();
    for line in body.lines() {
        let line = line.trim();
        for ty in ["uint8_t", "uint16_t"] {
            if let Some(name) = line
                .strip_prefix(ty)
                .and_then(|r| r.trim().strip_suffix(';'))
            {
                out.insert(name.to_string(), ty.to_string());
            }
        }
    }
    out
}

fn field(home: &str) -> &str {
    home.strip_prefix("c->").expect("a field of c")
}

#[test]
fn header_pins_the_offsets_the_test_reads() {
    let o = offsets();
    assert_eq!(o.get("a"), Some(&0));
    assert_eq!(o.get("f"), Some(&7));
    assert_eq!(o.get("sp"), Some(&8));
    assert_eq!(o.get("time"), Some(&72));
}

#[test]
fn every_register_home_is_a_field_of_the_right_width() {
    let fe = Sm83::new(IoMap::default());
    let rf = regs::regs();
    let ty = field_types();
    let off = offsets();
    for i in 0..rf.len() {
        let reg = relift_ir::Reg(i as u16);
        let decl = rf.decl(reg);
        match fe.home(reg) {
            Home::Field(lv) => {
                let name = field(&lv);
                assert!(off.contains_key(name), "{} has no pinned offset", lv);
                let want = match decl.width {
                    Width::W8 => "uint8_t",
                    Width::W16 => "uint16_t",
                    w => panic!("{} has width {w:?}", decl.name),
                };
                assert_eq!(ty.get(name).map(String::as_str), Some(want), "{lv}");
                assert_eq!(name, decl.name, "field and register names differ");
            }
            Home::Flag { word, bit } => {
                let name = field(&word);
                assert_eq!(ty.get(name).map(String::as_str), Some("uint8_t"), "{word}");
                let want = match decl.name {
                    "fz" => 7,
                    "fn" => 6,
                    "fh" => 5,
                    "fc" => 4,
                    n => panic!("unknown flag {n}"),
                };
                assert_eq!(bit, want, "{}", decl.name);
                assert_eq!(decl.width, Width::W1);
            }
        }
    }
}

#[test]
fn the_flag_byte_follows_the_registers_in_the_context() {
    let off = offsets();
    for (i, n) in ["a", "b", "c", "d", "e", "h", "l", "f"].iter().enumerate() {
        assert_eq!(off[*n], i as u64, "{n}");
    }
}

#[test]
fn the_clock_is_the_context_rl_time() {
    let fe = Sm83::new(IoMap::default());
    let clock = fe.clock().expect("the SM83 has a clock");
    assert_eq!(clock, "c->time");
    let off = offsets();
    assert_eq!(off["time"], 72);
    assert!(HEADER.contains("#define RL_SM83_TIME_OFFSET 72u"));
}

#[test]
fn the_frontend_emits_the_headers_and_helpers_the_runtime_defines() {
    let fe = Sm83::new(IoMap::default());
    assert_eq!(fe.abi_headers(), ["relift/sm83/abi.h"]);
    assert!(HEADER.contains("int rl_trap(rl_ctx *c, uint8_t *mem, uint32_t code);"));
    assert!(
        HEADER.contains("int rl_undefined(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn);")
    );
    assert!(fe.trap(5).starts_with("rl_trap(c, mem, "));
    assert!(
        fe.undefined(0x150, 0xd3)
            .starts_with("rl_undefined(c, mem, UINT64_C(")
    );
    assert_eq!(fe.granule(), 1);
    assert!(HEADER.contains("#define RL_SM83_CODE_SHIFT 0u"));
}
