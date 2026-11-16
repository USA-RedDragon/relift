//! End to end for the Nintendo DS guest: a two-CPU cartridge built at test
//! time with clang and ld.lld (the ARM9 code for ARMv5TE, the ARM7 code for
//! ARMv4T, a linker script each; no SDK) goes through `relift import`,
//! `relift build` (unit discovery, lifting each unit, the C emitter, clang
//! and the runtime) and runs headless with both CPUs on the ordered
//! scheduler. The ARM9 starts timer 0, divides with the DIV unit, sends the
//! quotient over the IPC FIFO, waits for the ARM7's answer (three times the
//! word plus one, which the ARM7 gets from the BIOS's `Div`), writes it, the timer's progress and a done mark to main
//! RAM, stops the timer and waits for an interrupt in CP15; the ARM7
//! halts. With no event left that could wake either CPU, the run ends. The test checks
//! the words, that both CPUs halted, and that no code was missed or left
//! unlifted. Skipped with a note when clang, ld.lld or llvm-objcopy is not
//! on `PATH`.

mod tiny;

use std::path::{Path, PathBuf};

const ARM9_C: &str = r#"
#define R16(a) (*(volatile unsigned short *)(a))
#define R32(a) (*(volatile unsigned *)(a))

static unsigned divide(unsigned n, unsigned d)
{
    R16(0x04000280) = 0;
    R32(0x04000290) = n;
    R32(0x04000294) = 0;
    R32(0x04000298) = d;
    while (R16(0x04000280) & 0x8000)
        ;
    return R32(0x040002A0);
}

__attribute__((section(".text.start"), noreturn)) void _start(void)
{
    R16(0x04000184) = 0xC008;
    R16(0x04000100) = 0;
    R16(0x04000102) = 0x0080;
    unsigned q = divide(1000, 7);
    R32(0x04000188) = q;
    while (R16(0x04000184) & 0x100)
        ;
    unsigned answer = R32(0x04100000);
    unsigned ticks = R16(0x04000100);
    R32(0x02100000) = answer;
    R32(0x02100004) = ticks != 0;
    R32(0x02100008) = 0xC0DE;
    R16(0x04000102) = 0;
    for (;;)
        __asm__ volatile("mcr p15, 0, %0, c7, c0, 4" ::"r"(0));
}
"#;

const ARM7_C: &str = r#"
#define R8(a) (*(volatile unsigned char *)(a))
#define R16(a) (*(volatile unsigned short *)(a))
#define R32(a) (*(volatile unsigned *)(a))

__attribute__((section(".text.start"), noreturn)) void _start(void)
{
    R16(0x04000184) = 0xC008;
    while (R16(0x04000184) & 0x100)
        ;
    unsigned v = R32(0x04100000);
    register unsigned n __asm__("r0") = v * 21 + 7;
    register unsigned d __asm__("r1") = 7;
    __asm__ volatile("swi 0x090000" : "+r"(n), "+r"(d)::"r3", "memory");
    R32(0x04000188) = n;
    for (;;)
        R8(0x04000301) = 0x80;
}
"#;

fn script(base: u32) -> String {
    format!(
        "ENTRY(_start)\nSECTIONS {{\n  . = {base:#x};\n  .text : {{ *(.text.start) *(.text*) }}\n  .rodata : {{ *(.rodata*) }}\n  .data : {{ *(.data*) }}\n  .bss : {{ *(.bss*) *(COMMON) }}\n  /DISCARD/ : {{ *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }}\n}}\n"
    )
}

fn run(cmd: &mut std::process::Command) {
    let out = cmd.output().expect("the tool runs");
    assert!(
        out.status.success(),
        "{cmd:?}\n{}{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );
}

/// The flat binary of `src` built for `target` and `cpu` at `base`.
fn binary(dir: &Path, name: &str, src: &str, target: &str, cpu: &str, base: u32) -> Vec<u8> {
    let c = dir.join(format!("{name}.c"));
    let o = dir.join(format!("{name}.o"));
    let ld = dir.join(format!("{name}.ld"));
    let elf = dir.join(format!("{name}.elf"));
    let bin = dir.join(format!("{name}.bin"));
    relift_cache::write_if_changed(&c, src.as_bytes()).expect("the source is writable");
    relift_cache::write_if_changed(&ld, script(base).as_bytes()).expect("the script is writable");
    run(relift_proc::command("clang")
        .arg(format!("--target={target}"))
        .arg(format!("-mcpu={cpu}"))
        .args([
            "-marm",
            "-O2",
            "-ffreestanding",
            "-fno-builtin",
            "-nostdlib",
            "-fno-pic",
            "-fno-exceptions",
            "-fno-unwind-tables",
            "-fno-asynchronous-unwind-tables",
            "-c",
        ])
        .arg(&c)
        .arg("-o")
        .arg(&o));
    run(relift_proc::command("ld.lld")
        .arg("-T")
        .arg(&ld)
        .arg("-o")
        .arg(&elf)
        .arg(&o));
    run(relift_proc::command("llvm-objcopy")
        .args(["-O", "binary"])
        .arg(&elf)
        .arg(&bin));
    std::fs::read(&bin).expect("the binary")
}

fn put32(r: &mut [u8], at: usize, v: u32) {
    r[at..at + 4].copy_from_slice(&v.to_le_bytes());
}

/// The cartridge image: a header, the ARM9 binary at `0x200` loaded to
/// `0x02000000`, the ARM7 binary loaded to `0x02380000`, no file system.
fn image(arm9: &[u8], arm7: &[u8]) -> Vec<u8> {
    let mut r = vec![0u8; 0x200];
    r[..12].copy_from_slice(b"RELIFTTEST\0\0");
    r[0x0C..0x10].copy_from_slice(b"TEST");
    r[0x10..0x12].copy_from_slice(b"00");
    let off9 = r.len();
    r.extend_from_slice(arm9);
    r.resize(r.len().next_multiple_of(0x200), 0);
    let off7 = r.len();
    r.extend_from_slice(arm7);
    r.resize(r.len().next_multiple_of(0x200), 0);
    for (at, off, ram, len) in [
        (0x20, off9, 0x0200_0000u32, arm9.len()),
        (0x30, off7, 0x0238_0000, arm7.len()),
    ] {
        put32(&mut r, at, u32::try_from(off).expect("small"));
        put32(&mut r, at + 4, ram);
        put32(&mut r, at + 8, ram);
        put32(&mut r, at + 12, u32::try_from(len).expect("small"));
    }
    let total = u32::try_from(r.len()).expect("small");
    put32(&mut r, 0x80, total);
    put32(&mut r, 0x84, 0x200);
    relift_nds_formats::synth::fix_crcs(&mut r);
    r
}

fn tools() -> bool {
    ["clang", "ld.lld", "llvm-objcopy"]
        .iter()
        .all(|t| relift_build::find_tool(t).is_some())
}

#[test]
fn two_cpus_trade_words_over_the_fifo_lifted_end_to_end() {
    if !tools() {
        eprintln!("skipped: clang, ld.lld or llvm-objcopy is not on PATH");
        return;
    }
    let root = tiny::scratch("e2e-nds-fifo");
    let src = root.join("src");
    std::fs::create_dir_all(&src).expect("the source dir is creatable");
    let arm9 = binary(
        &src,
        "arm9",
        ARM9_C,
        "armv5te-none-eabi",
        "arm946e-s",
        0x0200_0000,
    );
    let arm7 = binary(
        &src,
        "arm7",
        ARM7_C,
        "armv4t-none-eabi",
        "arm7tdmi",
        0x0238_0000,
    );
    let project = root.join("game");
    std::fs::create_dir_all(&project).expect("the project dir is creatable");
    let nds: PathBuf = root.join("fifo.nds");
    relift_cache::write_if_changed(&nds, &image(&arm9, &arm7)).expect("the image is writable");
    let cache = root.join("cache").display().to_string();
    let out = tiny::relift(
        &project,
        &["--cache-dir", &cache, "import", &nds.display().to_string()],
    );
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stderr}");
    assert!(stderr.contains("Nintendo DS"), "{stderr}");
    let manifest = project.join("game.toml");
    let text = std::fs::read_to_string(&manifest).expect("game.toml");
    assert!(text.contains("guest = \"nds\""), "{text}");
    let text = text.replace("peek = []", "peek = [0x02100000, 0x02100004, 0x02100008]");
    relift_cache::write_if_changed(&manifest, text.as_bytes()).expect("game.toml is writable");
    let out = tiny::relift(&project, &["--cache-dir", &cache, "-j", "4", "build"]);
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stderr}");
    assert!(
        stderr.contains("in 2 units, 0 instructions not lifted"),
        "{stderr}"
    );
    let host = root.join("cache/projects/game/build/host");
    let out = relift_proc::command(host.join("game"))
        .args(["--headless", "--stats", "--max-ticks", "200000000"])
        .output()
        .expect("the game runs");
    let stdout = String::from_utf8_lossy(&out.stdout);
    let err = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stdout}{err}");
    assert!(
        stdout.contains("peek 0x02100000 = 0x000001ab"),
        "{stdout}{err}"
    );
    assert!(
        stdout.contains("peek 0x02100004 = 0x00000001"),
        "{stdout}{err}"
    );
    assert!(
        stdout.contains("peek 0x02100008 = 0x0000c0de"),
        "{stdout}{err}"
    );
    for cpu in ["arm9", "arm7"] {
        let line = err
            .lines()
            .find(|l| l.starts_with(&format!("relift-nds: {cpu} ")))
            .unwrap_or_default();
        assert!(
            line.contains("halted")
                && line.contains("unfound 0")
                && line.contains(if cpu == "arm7" {
                    "swis 1 (0 unknown)"
                } else {
                    "swis 0"
                }),
            "{cpu}: {err}"
        );
    }
    assert!(err.contains("misses 0"), "{err}");
}
