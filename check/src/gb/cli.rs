//! `relift-check --oracle rustyboi step|rom ...`

use std::process::ExitCode;

use super::machine::{hardware_name, parse_hardware};
use super::{Hardware, Regs, RomSpec, Segment, StepSpec, Stop, run_rom, run_step};

pub const USAGE: &str = "\
usage: relift-check --oracle rustyboi step --hw HW --code HEX [--at ADDR] [--mem ADDR=HEX]...
                    [--a|--f|--b|--c|--d|--e|--h|--l HEX] [--sp HEX] [--pc HEX] [--ime 0|1] [--fill HEX] [--io]
       relift-check --oracle rustyboi rom FILE --hw HW [--boot-rom FILE]
                    [--cycles N] [--frames N] [--pc ADDR] [--until ADDR=HEX] [--dump ADDR:LEN]...
HW is dmg, dmg0, mgb, sgb, sgb2, cgb0, cgbb, cgb (CGB-C), cgbe or agb";

fn hex(s: &str) -> Option<u32> {
    u32::from_str_radix(s.trim_start_matches("0x"), 16).ok()
}

fn hex_bytes(s: &str) -> Option<Vec<u8>> {
    let s: String = s
        .chars()
        .filter(|c| !matches!(c, ',' | ' ' | '_'))
        .collect();
    if !s.len().is_multiple_of(2) {
        return None;
    }
    (0..s.len() / 2)
        .map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).ok())
        .collect()
}

fn addr(s: &str) -> Option<u16> {
    hex(s).and_then(|v| u16::try_from(v).ok())
}

fn byte(s: &str) -> Option<u8> {
    hex(s).and_then(|v| u8::try_from(v).ok())
}

/// Splits `--name value` pairs; a flag may repeat.
fn pairs(args: &[String]) -> Result<Vec<(String, String)>, String> {
    let mut out = Vec::new();
    let mut i = 0;
    while i < args.len() {
        let k = args[i]
            .strip_prefix("--")
            .ok_or(format!("unexpected argument {}", args[i]))?;
        let v = args.get(i + 1).ok_or(format!("--{k} needs a value"))?;
        out.push((k.to_string(), v.clone()));
        i += 2;
    }
    Ok(out)
}

fn hardware(opts: &[(String, String)]) -> Result<Hardware, String> {
    let name = opts
        .iter()
        .find(|(k, _)| k == "hw")
        .map(|(_, v)| v.as_str())
        .ok_or("--hw is required")?;
    parse_hardware(name).ok_or(format!("unknown hardware {name}"))
}

pub fn parse_step(args: &[String]) -> Result<StepSpec, String> {
    let opts = pairs(args)?;
    let hardware = hardware(&opts)?;
    let mut regs = Regs {
        pc: 0x0100,
        sp: 0xFFFE,
        ..Regs::default()
    };
    let mut image = Vec::new();
    let mut code = None;
    let mut at = None;
    let mut fill = 0u8;
    for (k, v) in &opts {
        let bad = || format!("bad value for --{k}: {v}");
        match k.as_str() {
            "hw" => {}
            "code" => code = Some(hex_bytes(v).ok_or_else(bad)?),
            "at" => at = Some(addr(v).ok_or_else(bad)?),
            "mem" => {
                let (a, b) = v.split_once('=').ok_or_else(bad)?;
                image.push(Segment {
                    addr: addr(a).ok_or_else(bad)?,
                    bytes: hex_bytes(b).ok_or_else(bad)?,
                });
            }
            "a" => regs.a = byte(v).ok_or_else(bad)?,
            "f" => regs.f = byte(v).ok_or_else(bad)?,
            "b" => regs.b = byte(v).ok_or_else(bad)?,
            "c" => regs.c = byte(v).ok_or_else(bad)?,
            "d" => regs.d = byte(v).ok_or_else(bad)?,
            "e" => regs.e = byte(v).ok_or_else(bad)?,
            "h" => regs.h = byte(v).ok_or_else(bad)?,
            "l" => regs.l = byte(v).ok_or_else(bad)?,
            "sp" => regs.sp = addr(v).ok_or_else(bad)?,
            "pc" => regs.pc = addr(v).ok_or_else(bad)?,
            "ime" => regs.ime = v == "1",
            "fill" => fill = byte(v).ok_or_else(bad)?,
            _ => return Err(format!("unknown option --{k}")),
        }
    }
    let code = code.ok_or("--code is required")?;
    image.insert(
        0,
        Segment {
            addr: at.unwrap_or(regs.pc),
            bytes: code,
        },
    );
    Ok(StepSpec {
        hardware,
        regs,
        image,
        fill,
    })
}

pub fn parse_rom(args: &[String]) -> Result<RomSpec, String> {
    let path = args.first().ok_or("rom needs a file")?;
    let opts = pairs(&args[1..])?;
    let hardware = hardware(&opts)?;
    let rom = std::fs::read(path).map_err(|e| format!("{path}: {e}"))?;
    let mut boot_rom = None;
    let mut stops = Vec::new();
    let mut dump = Vec::new();
    for (k, v) in &opts {
        let bad = || format!("bad value for --{k}: {v}");
        match k.as_str() {
            "hw" => {}
            "boot-rom" => boot_rom = Some(std::fs::read(v).map_err(|e| format!("{v}: {e}"))?),
            "cycles" => stops.push(Stop::Cycles(v.parse().map_err(|_| bad())?)),
            "frames" => stops.push(Stop::Frames(v.parse().map_err(|_| bad())?)),
            "pc" => stops.push(Stop::Pc(addr(v).ok_or_else(bad)?)),
            "until" => {
                let (a, b) = v.split_once('=').ok_or_else(bad)?;
                stops.push(Stop::Mem {
                    addr: addr(a).ok_or_else(bad)?,
                    value: byte(b).ok_or_else(bad)?,
                });
            }
            "dump" => {
                let (a, n) = v.split_once(':').ok_or_else(bad)?;
                dump.push((addr(a).ok_or_else(bad)?, n.parse().map_err(|_| bad())?));
            }
            _ => return Err(format!("unknown option --{k}")),
        }
    }
    Ok(RomSpec {
        hardware,
        rom,
        boot_rom,
        stops,
        dump,
    })
}

fn regs_line(r: &Regs) -> String {
    format!(
        "a={:02x} f={:02x} b={:02x} c={:02x} d={:02x} e={:02x} h={:02x} l={:02x} sp={:04x} pc={:04x} ime={}",
        r.a,
        r.f,
        r.b,
        r.c,
        r.d,
        r.e,
        r.h,
        r.l,
        r.sp,
        r.pc,
        u8::from(r.ime)
    )
}

fn flags_line(r: &Regs) -> String {
    let [z, n, h, c] = r.flags();
    format!(
        "z={} n={} h={} c={}",
        u8::from(z),
        u8::from(n),
        u8::from(h),
        u8::from(c)
    )
}

fn stop_text(s: Stop) -> String {
    match s {
        Stop::Cycles(n) => format!("cycles {n}"),
        Stop::Frames(n) => format!("frames {n}"),
        Stop::Pc(p) => format!("pc {p:04x}"),
        Stop::Mem { addr, value } => format!("mem {addr:04x}={value:02x}"),
    }
}

fn run(sub: &str, rest: &[String]) -> Result<(), String> {
    match sub {
        "step" => {
            let show_io = rest.iter().any(|a| a == "--io");
            let rest: Vec<String> = rest.iter().filter(|a| *a != "--io").cloned().collect();
            let spec = parse_step(&rest)?;
            let r = run_step(&spec).map_err(|e| e.to_string())?;
            println!("oracle rustyboi step hw={}", hardware_name(spec.hardware));
            println!("regs {}", regs_line(&r.regs));
            println!("flags {}", flags_line(&r.regs));
            println!("cycles {}", r.cycles);
            for w in &r.writes {
                let m = w.mcycle.map_or("?".to_string(), |m| m.to_string());
                println!("write {:04x}={:02x} mcycle={m}", w.addr, w.value);
            }
            if show_io {
                let io: Vec<String> =
                    r.io.iter()
                        .map(|(a, v)| format!("{a:04x}={v:02x}"))
                        .collect();
                println!("io {}", io.join(" "));
            }
            Ok(())
        }
        "rom" => {
            let spec = parse_rom(rest)?;
            let r = run_rom(&spec).map_err(|e| e.to_string())?;
            println!("oracle rustyboi rom hw={}", hardware_name(spec.hardware));
            println!("stop {}", stop_text(r.reason.0));
            println!("regs {}", regs_line(&r.regs));
            println!("flags {}", flags_line(&r.regs));
            println!(
                "cycles {} steps {} master_cc {}",
                r.cycles, r.steps, r.master_cc
            );
            println!("frame_hash {:016x}", r.frame_hash);
            for (a, bytes) in &r.dump {
                let hex: String = bytes.iter().map(|b| format!("{b:02x}")).collect();
                println!("dump {a:04x} {hex}");
            }
            Ok(())
        }
        _ => Err(format!("unknown oracle command {sub}")),
    }
}

/// Handles the command line when it selects the rustyboi oracle. Returns
/// `None` for any other oracle so the A64 commands run unchanged.
pub fn dispatch(args: &[String]) -> Option<ExitCode> {
    let i = args.iter().position(|a| a == "--oracle")?;
    if args.get(i + 1).map(String::as_str) != Some("rustyboi") {
        return None;
    }
    let rest: Vec<String> = args
        .iter()
        .enumerate()
        .filter(|(j, _)| *j != i && *j != i + 1)
        .map(|(_, a)| a.clone())
        .collect();
    let Some((sub, rest)) = rest.split_first() else {
        eprintln!("{USAGE}");
        return Some(ExitCode::from(2));
    };
    Some(match run(sub, rest) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{e}\n{USAGE}");
            ExitCode::FAILURE
        }
    })
}
