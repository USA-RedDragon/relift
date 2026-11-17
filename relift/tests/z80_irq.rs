//! A hand-assembled Z80 program lifted with the Z80 frontend (with a sync
//! point after every instruction) together with the four interrupt entry
//! blocks, emitted as C, linked with the runtime's core, the Z80 layer and a
//! mock machine, and run. Two runs: IM 1 with a timer raising INT while the
//! CPU waits in `HALT`, and INT already asserted when `EI` runs, which must
//! be taken after the one instruction `EI` lets through. Registers,
//! memory, the stacked return address and the T-state count are checked.
//! Skipped with a note when neither clang nor zig is on `PATH`.

mod z80_support;

use relift_cpu_z80::{ENTRY, Entry, Options, SyncPolicy, Z80};
use relift_emit::{Options as EmitOptions, emit};
use relift_ir::IoMap;

/// Address and bytes of each part of the program image.
const PARTS: &[(usize, &[u8])] = &[
    (
        0x0038,
        &[
            0xF5, 0x3A, 0x10, 0xC0, 0x3C, 0x32, 0x10, 0xC0, 0xD3, 0xBF, 0xF1, 0xFB, 0xC9,
        ],
    ),
    (0x0066, &[0xED, 0x45]),
    (
        0x0100,
        &[
            0x31, 0x00, 0xF0, 0xED, 0x56, 0x3E, 0x12, 0xFB, 0x76, 0x32, 0x00, 0xC0, 0x3A, 0x10,
            0xC0, 0x32, 0x01, 0xC0, 0xDB, 0xBE, 0x32, 0x02, 0xC0, 0x3E, 0x07, 0xED, 0x47, 0xC9,
        ],
    ),
    (
        0x0200,
        &[
            0x31, 0x00, 0xF0, 0xED, 0x56, 0xFB, 0x3E, 0x55, 0x32, 0x21, 0xC0, 0x3A, 0x10, 0xC0,
            0x32, 0x20, 0xC0, 0xC9,
        ],
    ),
];

const MACHINE: &str = r#"
#include <stdio.h>
#include <string.h>
#include <relift/z80/abi.h>
#include <z80.h>

extern const rl_unit z80_unit;

static const rl_guest_desc guest = {.name = "synthetic", .page_size = 0x1000, .address_bits = 18, .tick_hz = 3579545};
static uint8_t mem[0x10000];
static uint8_t level;
static rl_sched *sched;
static int acks;

static uint8_t pending(void *o) { (void)o; return level ? RL_Z80_PEND_INT : 0; }
static void ack(void *o, uint32_t bit) { (void)o; (void)bit; acks++; }
static uint8_t port_in(void *o, uint16_t p) { (void)o; return (p & 0xFF) == 0xBE ? 0x5A : 0xFF; }
static void port_out(void *o, uint16_t p, uint8_t v)
{
    (void)o; (void)v;
    if ((p & 0xFF) == 0xBF) {
        level = 0;
        rl_sched_lower(sched, RL_Z80_LINE_INT);
    }
}

static void timer(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s; (void)e; (void)when;
    level = 1;
    rl_sched_raise(sched, RL_Z80_LINE_INT);
}

int main(int argc, char **argv)
{
    int early = argc > 1 && !strcmp(argv[1], "pending");
    if (rl_core_init(&guest, rl_z80_cpu()) != 0) return 10;
    rl_units_reset();
    const rl_unit *list[] = {&z80_unit};
    if (rl_units_register(list, 1) != 0) return 11;
    static const rl_z80_ints ints = {.pending = pending, .ack = ack, .in = port_in, .out = port_out};
    struct rl_thread *t = rl_thread_alloc();
    sched = rl_sched_create(NULL);
    rl_sched_cpu *k = rl_sched_add_cpu(sched, 1);
    if (!t || !sched || !k || rl_z80_attach(k, t, &ints) != 0) return 12;
    rl_device dev = {.name = "timer", .line = RL_LINE_NONE};
    rl_sched_add_device(sched, &dev);
    rl_event ev = {.fire = timer, .dev = &dev};
    if (early) {
        level = 1;
        rl_sched_raise(sched, RL_Z80_LINE_INT);
    } else {
        rl_sched_post(sched, &ev, 1000);
    }
    memcpy(mem, IMG, sizeof IMG);
    rl_sched_run(k);
    rl_ctx *c = t->ctx;
    int rc = rl_z80_run(c, mem, early ? 0x0200 : 0x0100);
    printf("rc=%d a=%02x i=%02x r=%02x sp=%04x iff1=%u ret=%02x%02x mem=%02x,%02x,%02x,%02x,%02x level=%u acks=%d cycles=%llu irqs=%u\n",
           rc, c->a, c->i, c->r, c->sp, c->iff1, mem[0xEFFF], mem[0xEFFE], mem[0xC000], mem[0xC001], mem[0xC002],
           mem[0xC010], mem[0xC020] | mem[0xC021], level, acks, (unsigned long long)rl_sched_cycles(k),
           rl_sched_get_stats(sched)->irqs);
    return 0;
}
"#;

fn image() -> Vec<u8> {
    let mut mem = vec![0u8; 0x10000];
    for (at, bytes) in PARTS {
        mem[*at..*at + bytes.len()].copy_from_slice(bytes);
    }
    mem
}

fn c_array(mem: &[u8]) -> String {
    let body: Vec<String> = mem.iter().map(|b| format!("{b:#04x}")).collect();
    format!(
        "static const uint8_t IMG[{}] = {{{}}};\n",
        mem.len(),
        body.join(",")
    )
}

#[test]
fn irq_halt_and_ei_delay_in_emitted_c() {
    let mem = image();
    let opts = Options {
        map: IoMap::memory(),
        sync: SyncPolicy::Every,
        live_operands: false,
    };
    let mut roots = vec![0x38u64, 0x100, 0x200];
    roots.extend([Entry::Nmi, Entry::Im0, Entry::Im1, Entry::Im2].map(Entry::addr));
    let found = z80_support::discover(&mem, &roots, &|_| false, &opts);
    assert!(found.failed.is_empty(), "{:?}", found.failed);
    assert!(
        found.shared_entries.is_empty(),
        "{:?}",
        found.shared_entries
    );
    assert!(found.functions.iter().any(|f| f.entry == ENTRY + 2));

    let fe = Z80::new(IoMap::memory());
    let fns = z80_support::functions(&found.functions);
    let out = emit(
        &fe,
        &fns,
        &[],
        &EmitOptions {
            io_map: IoMap::memory(),
            unit_symbol: "z80_unit".into(),
            ..EmitOptions::default()
        },
    );
    assert_eq!(out.undefined, 0, "every instruction is expressed");

    let dir = z80_support::scratch("z80-irq");
    let used = &mem[..0x300];
    let machine = MACHINE.replacen(
        "extern const rl_unit z80_unit;",
        &format!("extern const rl_unit z80_unit;\n{}", c_array(used)),
        1,
    );
    let Some(exe) = z80_support::build(&dir, &out, &machine, "-O1") else {
        return;
    };
    let run = |arg: &str| {
        let run = relift_proc::command(&exe).arg(arg).output().expect("runs");
        let text = String::from_utf8_lossy(&run.stdout).to_string();
        assert!(
            run.status.success(),
            "{text}\n{}",
            String::from_utf8_lossy(&run.stderr)
        );
        text
    };
    let timer = run("timer");
    assert_eq!(
        timer.trim_end(),
        "rc=0 a=07 i=07 r=0a sp=f002 iff1=1 ret=0109 mem=12,01,5a,01,00 level=0 acks=1 cycles=1179 irqs=1",
        "{timer}"
    );
    let pending = run("pending");
    assert_eq!(
        pending.trim_end(),
        "rc=0 a=01 i=00 r=12 sp=f002 iff1=1 ret=0208 mem=00,00,00,01,55 level=0 acks=1 cycles=167 irqs=1",
        "{pending}"
    );
    let _ = std::fs::remove_dir_all(&dir);
}
