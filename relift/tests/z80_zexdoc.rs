//! Frank Cringle's ZEXDOC Z80 instruction exerciser (GPL-2, test data only:
//! never committed, fetched by CI) built through relift: its code found by
//! recursive descent, lifted with the Z80 frontend, emitted as C, linked
//! with the runtime's core and Z80 layer and run natively. All 67 groups
//! must print OK. Skipped with a note when the .com is absent (the path in
//! `RELIFT_ZEXDOC`, or `~/spare/relift-spike-sega/dl/zex/zexdoc.com`), or
//! neither clang nor zig is on `PATH`.
//!
//! The exerciser patches the instruction under test into four bytes of its
//! own code, and the flag mask into the immediate of an `AND`. The lifted
//! program therefore has no fixed code there: both places are entry points
//! the machine supplies, which pick the lifted variant for the bytes in
//! memory. The variants are every instruction the test descriptors can put
//! there (the descriptor's bytes with each subset of its counter mask bits
//! flipped, and each single bit of its shift mask) and the 256 immediates.
//! The file is checked against its SHA-256, which these addresses belong to.

mod z80_support;

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;
use std::path::PathBuf;

use relift_cpu_z80::{Options, SyncPolicy, Z80, lift_bytes};
use relift_emit::{Function, Insn, Options as EmitOptions, Override, emit};
use relift_ir::{Block, Flow, IoMap, Next};
use sha2::{Digest, Sha256};

const SHA256: &str = "34923a7ed82285d3038b2d54bd64899e12173eebb61f9d07b4fc72e78af2ae8f";
/// The four bytes of the instruction under test and the code after them.
const IUT: u64 = 0x1d42;
const IUT_END: u64 = 0x1d46;
/// The `AND` whose immediate is the flag mask, and the code after it.
const MASK_AND: u64 = 0x1d64;
const MASK_AND_END: u64 = 0x1d66;
/// The table of test descriptor addresses, and the number of tests.
const TESTS: usize = 0x13a;
const GROUPS: usize = 67;
const BDOS: u64 = 5;
/// Where the variants of the two patched places are lifted: addresses the
/// program does not use.
const IUT_BASE: u64 = 0x8000;
const MASK_BASE: u64 = 0xB000;

const MACHINE: &str = r#"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <relift/z80/abi.h>
#include <z80.h>

extern const rl_unit z80_unit;

static const rl_guest_desc guest = {.name = "cpm", .page_size = 0x1000, .address_bits = 18, .tick_hz = 4000000};
static uint8_t mem[0x10000];

typedef struct { uint32_t key; uint16_t at; } variant;
/*VARIANTS*/

static uint8_t pending(void *o) { (void)o; return 0; }

/* The call pushed its return address; this host function is the RET. */
int zex_bdos(rl_ctx *c, uint8_t *m)
{
    if (c->c == 2) {
        putchar(c->e);
    } else if (c->c == 9) {
        for (uint16_t a = (uint16_t)(c->d << 8 | c->e); m[a] != '$'; a++)
            putchar(m[a]);
    }
    fflush(stdout);
    c->sp = (uint16_t)(c->sp + 2u);
    return RL_OK;
}

int zex_iut(rl_ctx *c, uint8_t *m)
{
    uint32_t key = m[0x1d42] | (uint32_t)m[0x1d43] << 8 | (uint32_t)m[0x1d44] << 16 | (uint32_t)m[0x1d45] << 24;
    size_t lo = 0, hi = sizeof iut / sizeof iut[0];
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (iut[mid].key < key) lo = mid + 1; else hi = mid;
    }
    if (lo == sizeof iut / sizeof iut[0] || iut[lo].key != key)
        return rl_guest_fatal(c, "zex", "no lifted variant of the instruction under test %08x", key);
    rl_fn f = rl_dispatch_find(iut[lo].at);
    if (!f)
        return rl_guest_fatal(c, "zex", "variant %04x is not registered", iut[lo].at);
    RL_MUSTTAIL return f(c, m);
}

int zex_and(rl_ctx *c, uint8_t *m)
{
    if (m[0x1d64] != 0xE6)
        return rl_guest_fatal(c, "zex", "the flag mask AND was overwritten");
    rl_fn f = rl_dispatch_find(0xB000u + 4u * m[0x1d65]);
    if (!f)
        return rl_guest_fatal(c, "zex", "no lifted AND for immediate %02x", m[0x1d65]);
    RL_MUSTTAIL return f(c, m);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (rl_core_init(&guest, rl_z80_cpu()) != 0) return 10;
    rl_units_reset();
    const rl_unit *list[] = {&z80_unit};
    if (rl_units_register(list, 1) != 0) return 11;
    static const rl_z80_ints ints = {.pending = pending};
    struct rl_thread *t = rl_thread_alloc();
    rl_sched *sched = rl_sched_create(NULL);
    rl_sched_cpu *k = rl_sched_add_cpu(sched, 1);
    if (!t || !sched || !k || rl_z80_attach(k, t, &ints) != 0) return 12;
    memcpy(mem, IMG, sizeof IMG);
    rl_sched_run(k);
    t->ctx->sp = 0xF000;
    int rc = rl_z80_run(t->ctx, mem, 0x0100);
    printf("\n[zexdoc] rc=%d cycles=%llu\n", rc, (unsigned long long)rl_sched_cycles(k));
    return rc;
}
"#;

fn com_path() -> Option<PathBuf> {
    if let Some(p) = std::env::var_os("RELIFT_ZEXDOC") {
        return Some(PathBuf::from(p));
    }
    let home = std::env::var_os("HOME")?;
    let p = PathBuf::from(home).join("spare/relift-spike-sega/dl/zex/zexdoc.com");
    p.is_file().then_some(p)
}

fn subsets(mask: u8) -> Vec<u8> {
    let mut out = Vec::new();
    let mut x = 0u8;
    loop {
        out.push(x);
        x = x.wrapping_sub(mask) & mask;
        if x == 0 {
            return out;
        }
    }
}

/// Every four-byte content the exerciser can put at [`IUT`]: per test
/// descriptor (flag mask byte, 20-byte vector, counter mask, shift mask,
/// CRC, name) the vector's instruction bytes with each subset of the counter
/// mask's bits flipped and each of the shift mask's single bits.
fn iut_variants(com: &[u8]) -> BTreeSet<[u8; 4]> {
    let at = |a: usize, n: usize| &com[a - 0x100..a - 0x100 + n];
    let mut out = BTreeSet::new();
    for g in 0..GROUPS {
        let p = usize::from(at(TESTS + 2 * g, 2)[0]) | usize::from(at(TESTS + 2 * g, 2)[1]) << 8;
        let (base, count, shift) = (at(p + 1, 4), at(p + 21, 4), at(p + 41, 4));
        let mut flips: Vec<[u8; 4]> = vec![[0; 4]];
        for k in 0..4 {
            flips = flips
                .iter()
                .flat_map(|f| {
                    subsets(count[k]).into_iter().map(move |s| {
                        let mut f = *f;
                        f[k] = s;
                        f
                    })
                })
                .collect();
        }
        let mut shifts: Vec<[u8; 4]> = vec![[0; 4]];
        for k in 0..4 {
            for bit in 0..8 {
                if shift[k] >> bit & 1 == 1 {
                    let mut s = [0; 4];
                    s[k] = 1 << bit;
                    shifts.push(s);
                }
            }
        }
        for f in &flips {
            for s in &shifts {
                out.insert(std::array::from_fn(|k| base[k] ^ f[k] ^ s[k]));
            }
        }
    }
    out
}

/// The instructions that fill `window` (up to `end`), lifted at `base`
/// onward as one function that continues at `end`; a repeating instruction
/// (`LDIR`) repeats in place.
fn variant(mem: &[u8], window: &[u8], base: u64, end: u64, opts: &Options) -> Vec<(u64, Block)> {
    let mut bytes = window.to_vec();
    bytes.extend_from_slice(&mem[end as usize..end as usize + 4]);
    let mut blocks = Vec::new();
    let mut off = 0usize;
    while off < window.len() {
        let pc = base + off as u64;
        let mut l = lift_bytes(&bytes[off..], pc, opts).expect("lifts");
        let len = usize::from(l.len);
        let after = pc + len as u64;
        let tail = base + window.len() as u64;
        let map = |t: u64| if t == tail { end } else { t };
        l.block.exit.next = match l.block.exit.next {
            Next::Const(n) if n == after => Next::Const(map(n)),
            Next::Select {
                cond,
                taken,
                not_taken,
            } if taken == pc && not_taken == after => Next::Select {
                cond,
                taken,
                not_taken: map(not_taken),
            },
            _ => panic!("a variant {window:02x?} leaves its straight line"),
        };
        assert_eq!(l.block.exit.flow, Flow::Jump, "{window:02x?}");
        blocks.push((pc, l.block));
        off += len;
    }
    assert_eq!(off, window.len(), "{window:02x?} overruns its window");
    blocks
}

fn c_array(name: &str, bytes: &[u8]) -> String {
    let body: Vec<String> = bytes.iter().map(|b| format!("{b:#04x}")).collect();
    format!(
        "static const uint8_t {name}[{}] = {{{}}};\n",
        bytes.len(),
        body.join(",")
    )
}

#[test]
fn zexdoc_passes_as_emitted_c() {
    let Some(path) = com_path() else {
        eprintln!("skipped: no zexdoc.com (set RELIFT_ZEXDOC)");
        return;
    };
    let com = std::fs::read(&path).expect("reads the .com");
    let sum: String = Sha256::digest(&com).iter().fold(String::new(), |mut s, b| {
        let _ = write!(s, "{b:02x}");
        s
    });
    assert_eq!(sum, SHA256, "{} is not ZEXDOC", path.display());

    let mut mem = vec![0u8; 0x10000];
    mem[0x100..0x100 + com.len()].copy_from_slice(&com);
    mem[0] = 0xC9;
    mem[6] = 0x00;
    mem[7] = 0xF0;
    let opts = Options {
        map: IoMap::memory(),
        sync: SyncPolicy::default(),
    };
    let external = |a: u64| matches!(a, BDOS | IUT | MASK_AND);
    let started = std::time::Instant::now();
    let roots = [0x100u64, 0, IUT_END, MASK_AND_END];
    let found = z80_support::discover(&mem, &roots, &external, &opts);
    assert!(found.failed.is_empty(), "{:x?}", found.failed);
    eprintln!(
        "[zexdoc] {} functions, {} blocks, shared entries {:x?}",
        found.functions.len(),
        found
            .functions
            .iter()
            .map(|f| f.blocks.len())
            .sum::<usize>(),
        found.shared_entries
    );
    assert!(
        found.shared_entries.is_empty(),
        "{:x?}",
        found.shared_entries
    );

    let variants = iut_variants(&com);
    let mut at: Vec<([u8; 4], u64)> = Vec::new();
    let mut owned: Vec<(u64, Vec<(u64, Block)>)> = Vec::new();
    for (i, v) in variants.iter().enumerate() {
        let base = IUT_BASE + 8 * i as u64;
        at.push((*v, base));
        owned.push((base, variant(&mem, v, base, IUT_END, &opts)));
    }
    for imm in 0..=255u8 {
        let base = MASK_BASE + 4 * u64::from(imm);
        owned.push((base, variant(&mem, &[0xE6, imm], base, MASK_AND_END, &opts)));
    }
    eprintln!(
        "[zexdoc] {} instruction variants, discovered and lifted in {:.1} s",
        variants.len(),
        started.elapsed().as_secs_f64()
    );

    let mut fns: Vec<Function<'_>> = z80_support::functions(&found.functions);
    for (base, blocks) in &owned {
        fns.push(Function {
            entry: *base,
            name: format!("V_{base:x}"),
            guest_name: None,
            span: 8,
            insts: blocks
                .iter()
                .map(|(pc, b)| (*pc, Insn::Lifted { block: b, raw: 0 }))
                .collect(),
            tables: BTreeMap::new(),
            pads: Default::default(),
        });
    }
    fns.sort_by_key(|f| f.entry);
    let ovr = |entry, host_fn: &str, may_unwind| Override {
        entry,
        host_fn: host_fn.into(),
        may_unwind,
        resume_point: false,
        declare: true,
    };
    let fe = Z80::new(IoMap::memory());
    let out = emit(
        &fe,
        &fns,
        &[
            ovr(BDOS, "zex_bdos", false),
            ovr(IUT, "zex_iut", true),
            ovr(MASK_AND, "zex_and", true),
        ],
        &EmitOptions {
            io_map: IoMap::memory(),
            unit_symbol: "z80_unit".into(),
            ..EmitOptions::default()
        },
    );
    assert_eq!(out.undefined, 0, "every instruction is expressed");
    let c_bytes: usize = out.files.iter().map(|f| f.text.len()).sum();
    eprintln!("[zexdoc] {} bytes of C", c_bytes);

    at.sort_by_key(|(v, _)| u32::from_le_bytes(*v));
    let mut table = String::from("static const variant iut[] = {\n");
    for (v, base) in &at {
        let _ = writeln!(
            table,
            "    {{{:#010x}u, {base:#06x}}},",
            u32::from_le_bytes(*v)
        );
    }
    table.push_str("};\n");
    let machine = MACHINE.replacen("/*VARIANTS*/", &table, 1).replacen(
        "extern const rl_unit z80_unit;",
        &format!(
            "extern const rl_unit z80_unit;\n{}",
            c_array("IMG", &mem[..0x2400])
        ),
        1,
    );
    let dir = z80_support::scratch("z80-zexdoc");
    let build_started = std::time::Instant::now();
    let Some(exe) = z80_support::build(&dir, &out, &machine, "-O2") else {
        return;
    };
    eprintln!(
        "[zexdoc] C compiled in {:.1} s",
        build_started.elapsed().as_secs_f64()
    );
    let run_started = std::time::Instant::now();
    let run = relift_proc::command(&exe).output().expect("runs");
    let secs = run_started.elapsed().as_secs_f64();
    let text = String::from_utf8_lossy(&run.stdout).to_string();
    eprintln!("{text}\n[zexdoc] ran in {secs:.1} s");
    assert!(
        run.status.success(),
        "{text}\n{}",
        String::from_utf8_lossy(&run.stderr)
    );
    let ok = text
        .lines()
        .filter(|l| l.trim_end().ends_with("OK"))
        .count();
    assert!(!text.contains("ERROR"), "{text}");
    assert_eq!(ok, GROUPS, "{text}");
    assert!(text.contains("Tests complete"), "{text}");
    let _ = std::fs::remove_dir_all(&dir);
}
