//! `relift-check --oracle nba rom ...`

use std::path::PathBuf;
use std::process::ExitCode;

use super::{Event, Nba, Options, binary};

pub const USAGE: &str = "\
usage: relift-check --oracle nba rom FILE (--bios FILE | --skip-bios) [--keys HEX]
                    [--poke ADDR=HEX]... [--set REG=HEX]... [--jump ADDR | --jump-thumb ADDR]
                    [--cycles N] [--frames N] [--until ADDR] [--trace N]
                    [--dump ADDR:LEN]... [--io] [--pixels FILE.ppm] [--save FILE] [--bin FILE]
The oracle is --bin, RELIFT_NBA_ORACLE or ~/.cache/relift/gba-oracle/bin/nba-oracle
(build it with check/oracle/nba/build.sh). The BIOS may also come from RELIFT_GBA_BIOS.";

#[derive(Debug, Default)]
pub struct RomRun {
    pub options: Options,
    pub bin: Option<PathBuf>,
    pub keys: u16,
    pub pokes: Vec<(u32, Vec<u8>)>,
    pub sets: Vec<(String, u32)>,
    pub jump: Option<(u32, bool)>,
    pub cycles: Option<u64>,
    pub frames: Option<u64>,
    pub until: Option<u32>,
    pub trace: u64,
    pub dump: Vec<(u32, u32)>,
    pub io: bool,
    pub pixels: Option<PathBuf>,
    pub save: Option<PathBuf>,
}

fn hex(s: &str) -> Option<u32> {
    u32::from_str_radix(s.trim_start_matches("0x"), 16).ok()
}

fn hex_bytes(s: &str) -> Option<Vec<u8>> {
    if !s.len().is_multiple_of(2) {
        return None;
    }
    (0..s.len() / 2)
        .map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).ok())
        .collect()
}

pub fn parse_rom(args: &[String]) -> Result<RomRun, String> {
    let path = args.first().ok_or("rom needs a file")?;
    let mut r = RomRun {
        options: Options {
            rom: PathBuf::from(path),
            bios: std::env::var_os("RELIFT_GBA_BIOS").map(PathBuf::from),
            ..Options::default()
        },
        ..RomRun::default()
    };
    let mut i = 1;
    while i < args.len() {
        let k = args[i]
            .strip_prefix("--")
            .ok_or(format!("unexpected argument {}", args[i]))?;
        if k == "skip-bios" || k == "io" {
            match k {
                "skip-bios" => r.options.skip_bios = true,
                _ => r.io = true,
            }
            i += 1;
            continue;
        }
        let v = args.get(i + 1).ok_or(format!("--{k} needs a value"))?;
        let bad = || format!("bad value for --{k}: {v}");
        match k {
            "bios" => r.options.bios = Some(PathBuf::from(v)),
            "bin" => r.bin = Some(PathBuf::from(v)),
            "keys" => r.keys = hex(v).and_then(|k| u16::try_from(k).ok()).ok_or_else(bad)?,
            "cycles" => r.cycles = Some(v.parse().map_err(|_| bad())?),
            "frames" => r.frames = Some(v.parse().map_err(|_| bad())?),
            "until" => r.until = Some(hex(v).ok_or_else(bad)?),
            "trace" => r.trace = v.parse().map_err(|_| bad())?,
            "dump" => {
                let (a, n) = v.split_once(':').ok_or_else(bad)?;
                r.dump
                    .push((hex(a).ok_or_else(bad)?, n.parse().map_err(|_| bad())?));
            }
            "poke" => {
                let (a, b) = v.split_once('=').ok_or_else(bad)?;
                r.pokes
                    .push((hex(a).ok_or_else(bad)?, hex_bytes(b).ok_or_else(bad)?));
            }
            "set" => {
                let (n, x) = v.split_once('=').ok_or_else(bad)?;
                r.sets.push((n.to_string(), hex(x).ok_or_else(bad)?));
            }
            "jump" => r.jump = Some((hex(v).ok_or_else(bad)?, false)),
            "jump-thumb" => r.jump = Some((hex(v).ok_or_else(bad)?, true)),
            "pixels" => r.pixels = Some(PathBuf::from(v)),
            "save" => r.save = Some(PathBuf::from(v)),
            _ => return Err(format!("unknown option --{k}")),
        }
        i += 2;
    }
    if r.options.bios.is_none() && !r.options.skip_bios {
        return Err("give --bios FILE (or RELIFT_GBA_BIOS) or --skip-bios".into());
    }
    Ok(r)
}

/// A 240x160 binary PPM of BGR555 pixels.
pub fn ppm(px: &[u16]) -> Vec<u8> {
    let mut out = b"P6\n240 160\n255\n".to_vec();
    for &p in px {
        for c in [p & 31, (p >> 5) & 31, (p >> 10) & 31] {
            out.push(((c << 3) | (c >> 2)) as u8);
        }
    }
    out
}

fn hexdump(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

pub fn run_rom(r: &RomRun) -> Result<Vec<String>, String> {
    let bin = r
        .bin
        .clone()
        .or_else(binary)
        .ok_or("no nba-oracle: build it with check/oracle/nba/build.sh")?;
    let e = |e: super::Error| e.0;
    let mut nba = Nba::spawn(&bin, &r.options).map_err(e)?;
    let mut out = Vec::new();
    for (addr, data) in &r.pokes {
        nba.poke(*addr, data).map_err(e)?;
    }
    if !r.sets.is_empty() {
        let sets: Vec<(&str, u32)> = r.sets.iter().map(|(n, v)| (n.as_str(), *v)).collect();
        nba.set(&sets).map_err(e)?;
    }
    if let Some((pc, thumb)) = r.jump {
        nba.jump(pc, thumb).map_err(e)?;
    }
    nba.keys(r.keys).map_err(e)?;
    if let Some(n) = r.cycles {
        nba.run_cycles(n).map_err(e)?;
    }
    if let Some(n) = r.frames {
        nba.run_frames(n).map_err(e)?;
    }
    if let Some(pc) = r.until {
        nba.run_until(pc, 1 << 32).map_err(e)?;
    }
    for ev in nba.step(r.trace).map_err(e)? {
        out.push(match ev {
            Event::Insn {
                cycles,
                pc,
                opcode,
                regs,
                cpsr,
            } => {
                let rs: Vec<String> = regs.iter().map(|v| format!("{v:08x}")).collect();
                format!(
                    "trace {cycles} {pc:08x} {opcode:08x} {} cpsr={cpsr:08x}",
                    rs.join(" ")
                )
            }
            Event::Irq { cycles, pc } => format!("irq {cycles} before {pc:08x}"),
            Event::Halt { cycles } => format!("halt until {cycles}"),
        });
    }
    let regs = nba.regs().map_err(e)?;
    out.push(format!(
        "cycles {} pc {:08x}{}",
        regs.cycles,
        regs.pc,
        if regs.halted { " halted" } else { "" }
    ));
    let named: Vec<String> = regs
        .named
        .iter()
        .map(|(k, v)| format!("{k}={v:08x}"))
        .collect();
    out.push(format!("regs {}", named.join(" ")));
    let f = nba.frame().map_err(e)?;
    out.push(format!("frame {} {:016x}", f.count, f.hash));
    let a = nba.audio().map_err(e)?;
    out.push(format!("audio {} {:016x}", a.count, a.hash));
    for &(addr, len) in &r.dump {
        out.push(format!(
            "mem {addr:08x} {}",
            hexdump(&nba.mem(addr, len).map_err(e)?)
        ));
    }
    if r.io {
        out.push(format!("io {}", hexdump(&nba.io().map_err(e)?)));
    }
    if let Some(p) = &r.pixels {
        let px = nba.pixels().map_err(e)?;
        relift_cache::write_if_changed(p, &ppm(&px))
            .map_err(|err| format!("{}: {err}", p.display()))?;
    }
    if let Some(p) = &r.save {
        nba.save(p).map_err(e)?;
    }
    Ok(out)
}

fn run(sub: &str, rest: &[String]) -> Result<(), String> {
    match sub {
        "rom" => {
            for l in run_rom(&parse_rom(rest)?)? {
                println!("{l}");
            }
            Ok(())
        }
        "game" => {
            let (lines, ok) = super::game::run_game_check(&super::game::parse_game(rest)?)?;
            for l in lines {
                println!("{l}");
            }
            if ok {
                Ok(())
            } else {
                Err("the game and the oracle differ".into())
            }
        }
        _ => Err(format!("unknown command {sub}")),
    }
}

/// Handles `--oracle nba ...`; `None` when the arguments are for another
/// oracle.
pub fn dispatch(args: &[String]) -> Option<ExitCode> {
    let i = args.iter().position(|a| a == "--oracle")?;
    if args.get(i + 1).map(String::as_str) != Some("nba") {
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
            eprintln!("{e}\n{USAGE}\n{}", super::game::USAGE);
            ExitCode::FAILURE
        }
    })
}
