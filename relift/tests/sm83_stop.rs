//! A tiny synthetic ROM that exercises STOP, lifted with the SM83 frontend,
//! emitted as C, linked with the runtime's core, the SM83 layer and a mock
//! machine, and run. It checks the three STOP paths: a speed switch, stop
//! mode woken by a joypad line, and STOP with an interrupt already requested
//! (the byte after it runs). Skipped with a note when clang is not on `PATH`.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use relift_cpu_sm83::{Options, Sm83, lift_bytes};
use relift_emit::{Function, Insn, Options as EmitOptions, emit};
use relift_guest::Frontend;
use relift_ir::{AddrClass, IoMap, IoRange};

const ROM: [u8; 15] = [
    0x21, 0x00, 0xC0, 0x3E, 0x01, 0x10, 0x00, 0x3C, 0x10, 0x3C, 0x3C, 0x77, 0x10, 0x3C, 0x77,
];

const MACHINE: &str = r#"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <relift/sm83/abi.h>
#include <sm83.h>

extern const rl_unit sm83_unit;

static const rl_guest_desc guest = {.name = "synthetic", .page_size = 0x1000, .address_bits = 16, .tick_hz = 4194304};
static uint8_t mem[0x10000];
static uint8_t iflag, ie;
static int stops;
static rl_sched *sched;

static uint8_t pending(void *o) { (void)o; return iflag & ie; }
static void ack(void *o, uint32_t line) { (void)o; iflag &= (uint8_t)~(1u << line); }

static void stop_hook(void *o, rl_sm83_stop_info *r)
{
    (void)o;
    stops++;
    if (stops == 1) {
        r->kind = RL_SM83_STOP_SWITCH;
        r->ratio = 1;
    } else {
        r->kind = RL_SM83_STOP_MODE;
    }
}

static void joypad(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s; (void)e; (void)when;
    ie = 0x10;
    iflag |= 0x10;
    rl_sched_raise(sched, 4);
}


int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (rl_core_init(&guest, rl_sm83_cpu()) != 0) return 10;
    rl_units_reset();
    const rl_unit *list[] = {&sm83_unit};
    if (rl_units_register(list, 1) != 0) return 11;
    static const rl_sm83_ints ints = {.pending = pending, .ack = ack, .stop = stop_hook};
    struct rl_thread *t = rl_thread_alloc();
    sched = rl_sched_create(NULL);
    rl_sched_cpu *k = rl_sched_add_cpu(sched, 2);
    if (!t || !sched || !k || rl_sm83_attach(k, t, &ints) != 0) return 12;
    rl_device dev = {.name = "joypad", .line = RL_LINE_NONE};
    rl_sched_add_device(sched, &dev);
    rl_event ev = {.fire = joypad, .dev = &dev};
    rl_sched_post(sched, &ev, 600000);
    t->ctx->sp = 0xFFFE;
    rl_sched_run(k);
    int rc = rl_sm83_run(t->ctx, mem, 0x0100);
    printf("rc=%d a=%u mem=%u stops=%d cycles=%llu now=%llu pc=%04x\n", rc, t->ctx->a, mem[0xC000], stops,
           (unsigned long long)rl_sched_cycles(k), (unsigned long long)rl_sched_now(k), t->ctx->pc);
    return 0;
}
"#;

fn gb_map() -> IoMap {
    let r = |first, last, read, write| IoRange {
        first,
        last,
        read,
        write,
    };
    IoMap {
        ranges: vec![
            r(0x0000, 0x7fff, AddrClass::Memory, AddrClass::Device),
            r(0xff00, 0xff7f, AddrClass::Device, AddrClass::Device),
            r(0xffff, 0xffff, AddrClass::Device, AddrClass::Device),
        ],
        default: AddrClass::Memory,
    }
}

fn clang(cc: &Path, args: &[String]) {
    let out = relift_proc::command(cc)
        .args(args)
        .output()
        .expect("clang runs");
    assert!(
        out.status.success(),
        "clang {}:\n{}",
        args.join(" "),
        String::from_utf8_lossy(&out.stderr)
    );
}

#[test]
fn a_lifted_rom_with_stop_links_and_runs() {
    let Some(cc) = relift_build::find_tool("clang") else {
        eprintln!("skipped: no clang on PATH");
        return;
    };
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let dir = std::path::PathBuf::from(std::env::var_os("HOME").expect("HOME")).join(format!(
        ".cache/relift/gb-cpu-lift-flow/stop-{}",
        std::process::id()
    ));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).expect("the scratch dir is creatable");

    let fe = Sm83::new(gb_map());
    let opts = Options::new(gb_map());
    let mut blocks = Vec::new();
    let mut pc = 0usize;
    let starts = [0usize, 3, 5, 7, 8, 10, 11, 12, 14];
    while pc < ROM.len() {
        assert!(starts.contains(&pc), "{pc} is an instruction start");
        let b = lift_bytes(&ROM[pc..], 0x100 + pc as u64, &opts).expect("lifts");
        pc += usize::from(b.len as u8);
        blocks.push(b);
    }
    blocks.push(lift_bytes(&[0x3C], 0x10D, &opts).expect("lifts"));
    blocks.push(lift_bytes(&[0xC9], 0x10F, &opts).expect("lifts"));
    let insts: BTreeMap<u64, Insn<'_>> = blocks
        .iter()
        .map(|b| (b.pc, Insn::Lifted { block: b, raw: 0 }))
        .collect();
    let f = Function {
        entry: 0x100,
        name: "F_100".into(),
        guest_name: None,
        span: 0x10,
        insts,
        tables: BTreeMap::new(),
        pads: Default::default(),
    };
    let out = emit(
        &fe,
        &[f],
        &[],
        &EmitOptions {
            io_map: gb_map(),
            unit_symbol: "sm83_unit".into(),
            ..EmitOptions::default()
        },
    );
    assert_eq!(out.undefined, 0, "every instruction is expressed");

    let mut sources = Vec::new();
    for file in &out.files {
        let p = dir.join(&file.name);
        std::fs::write(&p, &file.text).expect("writes");
        if file.name.ends_with(".c") {
            sources.push(p);
        }
    }
    let m = dir.join("machine.c");
    std::fs::write(&m, MACHINE).expect("writes");
    sources.push(m);
    let rt = root.join("runtime");
    for entry in std::fs::read_dir(rt.join("core")).expect("lists the core") {
        let p = entry.expect("entry").path();
        if matches!(p.extension().and_then(|e| e.to_str()), Some("c" | "S")) {
            sources.push(p);
        }
    }
    sources.push(rt.join("cpus/sm83/sm83.c"));
    let flags: Vec<String> = [
        "-std=gnu11".to_string(),
        "-O1".into(),
        "-w".into(),
        "-mcx16".into(),
        "-D_GNU_SOURCE".into(),
        "-DRL_BUILDING_RUNTIME".into(),
        format!("-I{}", rt.join("core/include").display()),
        format!("-I{}", rt.join("core").display()),
        format!("-I{}", rt.join("cpus/sm83/include").display()),
        format!("-I{}", rt.join("cpus/sm83").display()),
        format!("-I{}", dir.display()),
    ]
    .into();
    let objs: Vec<PathBuf> = (0..sources.len())
        .map(|i| dir.join(format!("o{i}.o")))
        .collect();
    std::thread::scope(|s| {
        for (src, obj) in sources.iter().zip(&objs) {
            let mut a = flags.clone();
            a.extend([
                "-c".into(),
                src.display().to_string(),
                "-o".into(),
                obj.display().to_string(),
            ]);
            let cc = &cc;
            s.spawn(move || clang(cc, &a));
        }
    });
    let exe = dir.join("rom");
    let mut a: Vec<String> = objs.iter().map(|o| o.display().to_string()).collect();
    a.extend([
        "-lm".into(),
        "-lpthread".into(),
        "-o".into(),
        exe.display().to_string(),
    ]);
    clang(&cc, &a);
    let run = relift_proc::command(&exe).output().expect("the ROM runs");
    let text = String::from_utf8_lossy(&run.stdout).to_string();
    assert!(
        run.status.success(),
        "{text}\n{}",
        String::from_utf8_lossy(&run.stderr)
    );
    let _ = std::fs::remove_dir_all(&dir);
    assert!(text.starts_with("rc=0 a=4 mem=4 stops=2 "), "{text}");
    let now: u64 = text
        .split("now=")
        .nth(1)
        .and_then(|t| t.split_whitespace().next())
        .and_then(|n| n.parse().ok())
        .expect("a time");
    assert!(
        now >= 600_000,
        "stop mode waited for the joypad event: {text}"
    );
}
