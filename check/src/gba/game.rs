//! `relift-check --oracle nba game GAME ROM ...`: a built game against the
//! oracle at frame ends.
//!
//! The game is a program `relift build` made from the ROM. For each frame
//! count it runs headless to the end of that frame (`--frames N --dump
//! PREFIX`) and the oracle runs to the end of the same frame, from the
//! cartridge entry when it boots the real BIOS (the high-level BIOS skips the
//! intro); `--lead K` lets the oracle run K frames more, for a game whose
//! entry falls in another frame of the oracle's boot. Both report the frame
//! hash, r0 to r14 and the CPSR, and work RAM, palette, VRAM and OAM, which
//! are compared byte for byte.

use std::path::{Path, PathBuf};

use super::{Nba, Options, binary};

pub const USAGE: &str = "\
usage: relift-check --oracle nba game GAME ROM (--bios FILE | --skip-bios) --frames N[,N...]
                    [--lead K] [--save TYPE] [--bin FILE]
GAME is the program relift build made from ROM.";

#[derive(Debug, Default)]
pub struct GameRun {
    pub options: Options,
    pub game: PathBuf,
    pub bin: Option<PathBuf>,
    pub frames: Vec<u64>,
    pub lead: u64,
}

pub fn parse_game(args: &[String]) -> Result<GameRun, String> {
    let [game, rom, rest @ ..] = args else {
        return Err("game needs a program and a ROM".into());
    };
    let mut r = GameRun {
        options: Options {
            rom: PathBuf::from(rom),
            bios: std::env::var_os("RELIFT_GBA_BIOS").map(PathBuf::from),
            ..Options::default()
        },
        game: PathBuf::from(game),
        ..GameRun::default()
    };
    let mut i = 0;
    while i < rest.len() {
        let k = rest[i].as_str();
        let v = || {
            rest.get(i + 1)
                .cloned()
                .ok_or_else(|| format!("{k} needs a value"))
        };
        match k {
            "--skip-bios" => {
                r.options.skip_bios = true;
                i += 1;
                continue;
            }
            "--bios" => r.options.bios = Some(PathBuf::from(v()?)),
            "--save" => r.options.save = Some(v()?),
            "--bin" => r.bin = Some(PathBuf::from(v()?)),
            "--lead" => r.lead = v()?.parse().map_err(|_| "--lead needs a number")?,
            "--frames" => {
                r.frames = v()?
                    .split(',')
                    .map(|f| f.parse().map_err(|_| format!("bad frame count {f}")))
                    .collect::<Result<_, _>>()?;
            }
            _ => return Err(format!("unknown option {k}")),
        }
        i += 2;
    }
    if r.frames.is_empty() {
        return Err("game needs --frames".into());
    }
    r.frames.sort_unstable();
    Ok(r)
}

/// What the game reports at its frame stop.
#[derive(Debug, Default)]
struct Mine {
    hash: u64,
    regs: Vec<(String, u32)>,
}

fn parse_mine(out: &str) -> Result<Mine, String> {
    let mut m = Mine::default();
    for l in out.lines() {
        if let Some(r) = l.strip_prefix("frames ") {
            let h = r.split_whitespace().nth(2).ok_or("no frame hash")?;
            m.hash = u64::from_str_radix(h, 16).map_err(|_| format!("bad hash {h}"))?;
        } else if let Some(r) = l.strip_prefix("regs ") {
            let cpsr = r
                .strip_prefix("cpsr ")
                .and_then(|c| c.split_whitespace().next())
                .ok_or("no cpsr")?;
            m.regs.push((
                "cpsr".into(),
                u32::from_str_radix(cpsr, 16).map_err(|_| "bad cpsr")?,
            ));
            for part in r.split(", ").skip(1) {
                let mut w = part.split_whitespace();
                if let (Some(k), Some(v)) = (w.next(), w.next())
                    && k.starts_with('r')
                    && let Ok(v) = u32::from_str_radix(v, 16)
                {
                    m.regs.push((k.into(), v));
                }
            }
        }
    }
    if m.regs.is_empty() {
        return Err(format!("the game printed no report:\n{out}"));
    }
    Ok(m)
}

const REGIONS: [(&str, u32, u32); 5] = [
    ("ewram", 0x0200_0000, 0x4_0000),
    ("iwram", 0x0300_0000, 0x8000),
    ("palette", 0x0500_0000, 0x400),
    ("vram", 0x0600_0000, 0x1_8000),
    ("oam", 0x0700_0000, 0x400),
];

/// The address ranges where `a` and `b` differ, nearby ranges joined.
fn ranges(base: u32, a: &[u8], b: &[u8]) -> Vec<(u32, u32)> {
    let mut out: Vec<(u32, u32)> = Vec::new();
    for (i, (x, y)) in a.iter().zip(b).enumerate() {
        if x == y {
            continue;
        }
        let at = base + i as u32;
        match out.last_mut() {
            Some((_, end)) if at <= *end + 8 => *end = at,
            _ => out.push((at, at)),
        }
    }
    out
}

fn run_game(game: &Path, frames: u64, prefix: &Path) -> Result<String, String> {
    let out = relift_proc::command(game)
        .args([
            "--headless",
            "--no-save",
            "--frames",
            &frames.to_string(),
            "--dump",
        ])
        .arg(prefix)
        .output()
        .map_err(|e| format!("{}: {e}", game.display()))?;
    Ok(String::from_utf8_lossy(&out.stdout).into_owned())
}

/// Runs the comparison; the report lines and whether everything matched.
pub fn run_game_check(r: &GameRun) -> Result<(Vec<String>, bool), String> {
    let e = |x: super::Error| x.0;
    let bin = r
        .bin
        .clone()
        .or_else(binary)
        .ok_or("no nba-oracle (build it with check/oracle/nba/build.sh)")?;
    let mut nba = Nba::spawn(&bin, &r.options).map_err(e)?;
    if !r.options.skip_bios {
        nba.run_until(0x0800_0000, 1 << 30).map_err(e)?;
    }
    if r.lead > 0 {
        nba.run_frames(r.lead).map_err(e)?;
    }
    let dir = std::env::temp_dir().join(format!("relift-check-gba-{}", std::process::id()));
    std::fs::create_dir_all(&dir).map_err(|x| format!("{}: {x}", dir.display()))?;
    let prefix = dir.join("dump");
    let mut lines = Vec::new();
    let mut all = true;
    let mut done = 0;
    for &k in &r.frames {
        nba.run_frames(k - done).map_err(e)?;
        done = k;
        let theirs = nba.regs().map_err(e)?;
        let frame = nba.frame().map_err(e)?;
        let mine = parse_mine(&run_game(&r.game, k, &prefix)?)?;
        let mut diffs = Vec::new();
        if frame.hash != mine.hash {
            diffs.push(format!(
                "frame hash {:016x} (oracle {:016x})",
                mine.hash, frame.hash
            ));
        }
        for (name, v) in &mine.regs {
            if let Some(o) = theirs.named.get(name)
                && o != v
            {
                diffs.push(format!("{name} {v:08x} (oracle {o:08x})"));
            }
        }
        for (name, base, len) in REGIONS {
            let path = dir.join(format!("dump.{name}"));
            let m = std::fs::read(&path).map_err(|x| format!("{}: {x}", path.display()))?;
            let o = nba.mem(base, len).map_err(e)?;
            let rs = ranges(base, &o, &m);
            if !rs.is_empty() {
                let shown: Vec<String> = rs
                    .iter()
                    .take(6)
                    .map(|(a, b)| format!("{a:08x}+{}", b - a + 1))
                    .collect();
                diffs.push(format!("{name} {} ranges: {}", rs.len(), shown.join(" ")));
            }
        }
        all &= diffs.is_empty();
        lines.push(if diffs.is_empty() {
            format!("frame {k}: ok, hash {:016x}", mine.hash)
        } else {
            format!("frame {k}: {}", diffs.join("; "))
        });
    }
    let _ = std::fs::remove_dir_all(&dir);
    Ok((lines, all))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_report_parses() {
        let m = parse_mine(
            "frames 60 hash ffb537b2caeb46f1\nregs cpsr 6000001f (-ZC- --- sys), r0 030048c0, r1 08001d24, r14 08001f50, pc 08001e08, frames 1\n",
        )
        .unwrap();
        assert_eq!(m.hash, 0xffb5_37b2_caeb_46f1);
        assert_eq!(m.regs[0], ("cpsr".into(), 0x6000_001f));
        assert_eq!(m.regs[1], ("r0".into(), 0x0300_48c0));
        assert_eq!(m.regs.len(), 4);
    }

    #[test]
    fn nearby_differences_join() {
        let a = [0u8; 32];
        let mut b = [0u8; 32];
        b[2] = 1;
        b[5] = 1;
        b[30] = 1;
        assert_eq!(ranges(0x100, &a, &b), [(0x102, 0x105), (0x11e, 0x11e)]);
    }

    #[test]
    fn options_parse() {
        let a: Vec<String> = [
            "g",
            "r.gba",
            "--skip-bios",
            "--frames",
            "60,10",
            "--lead",
            "1",
        ]
        .iter()
        .map(|s| s.to_string())
        .collect();
        let r = parse_game(&a).unwrap();
        assert_eq!(r.frames, [10, 60]);
        assert_eq!(r.lead, 1);
        assert!(r.options.skip_bios);
    }
}
