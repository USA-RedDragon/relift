//! The in-process test-case harness: grades a run by the protocols of
//! rustyboi-test-runner, observed at instruction boundaries.
//!
//! The lifted CPU owns stepping, so a case is a state machine that the
//! runtime polls at every instruction boundary ([`Case::poll`]). Each poll
//! first accounts for the instruction that ran since the previous one (its
//! cycles, the frame it may have completed), which is what the runner does
//! after `step_instruction`, and then does what the runner does before the
//! next step (scripted input, the done marker, the serial capture).
//!
//! A case is described by newline-separated `key=value` lines:
//!
//! - `protocol`: `run`, `mem`, `mooneye`, `mooneye_ed`, `serial`,
//!   `serial_text`, `blargg_mem`, `png`, `png_fixed`, `png_layout`,
//!   `png_shootout`, `sram`, `region_dump`, `gambatte_hex`, `gambatte_audio`
//!   or `gambatte_png`.
//! - `frames`: the budget in frames (default 15); `cycles`: the budget in
//!   cycles (the cap of the mooneye protocols, default 250000000; the budget
//!   of `run` when `frames` is absent).
//! - `marker` (0x40 or 0xED), `addr`, `expect`, `capture` (`sc_edge` or
//!   `sb_change`), `pass_text`, `fail_text` (repeatable), `expect_hex`,
//!   `audible`, `ref` (repeatable, absolute paths), `skip` (repeatable,
//!   `start-end` as a half-open byte range), `region` (`oam` or `vram`),
//!   `input` (`frame[@ly]:buttons,...`, buttons joined by `+`, `-` releases
//!   all), `cart` (`lazy_sram_cs`) and `mode` (`dmg`, `cgb` or `agb`).
//!
//! `protocol=run` grades nothing: it runs `frames` LCD frames (or `cycles`)
//! and passes, reporting `frame <16 hex digits> serial <escaped text>`.

use crate::imaging::{self, FrameMismatch};
use crate::{Rlgbd, RlgbdRegs};
use rustyboi_core_lib::ppu::{ColorCorrection, LY};
use std::ops::Range;
use std::path::PathBuf;

const CYCLES_PER_FRAME: u64 = 70224;
const MAX_CYCLES_UNTIL_LCD_FRAME: u32 = 70224 * 64;
const DUMP_MIN_FRAMES: u64 = 64;
const DEFAULT_FRAMES: u64 = 15;
const MOONEYE_CAP: u64 = 250_000_000;
const STOPPED_CYCLES: u32 = 4;

/// The answer of a poll.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Verdict {
    /// The case runs on.
    Continue = 0,
    /// The case passed.
    Pass = 1,
    /// The case failed.
    Fail = 2,
    /// The case cannot run in this mode.
    Unsupported = 3,
    /// The verdict needs the CPU registers; supply them and poll again.
    NeedRegs = 4,
}

enum Kind {
    Run,
    Mem {
        addr: u16,
        expect: u8,
    },
    Mooneye {
        marker: u8,
    },
    Serial {
        pass: String,
        fails: Vec<String>,
        sc_edge: bool,
    },
    BlarggMem,
    Csp {
        path: PathBuf,
        fixed: bool,
        recolor: bool,
    },
    Shootout {
        refs: Vec<PathBuf>,
    },
    Sram {
        path: PathBuf,
        skip: Vec<Range<usize>>,
    },
    Region {
        path: PathBuf,
        base: u16,
        name: &'static str,
    },
    Hex {
        expected: String,
    },
    GambattePng {
        path: PathBuf,
    },
    Audio,
}

#[derive(Clone, Copy)]
struct InputEvent {
    frame: u32,
    ly: Option<u8>,
    buttons: u8,
}

/// An open case.
pub(crate) struct Case {
    kind: Kind,
    frames: u64,
    frames_given: bool,
    cycle_key: Option<u64>,
    input: Vec<InputEvent>,
    input_next: usize,
    cycles: u64,
    last_now: Option<u64>,
    verdict: Option<(Verdict, String)>,
    pub(crate) regs: Option<RlgbdRegs>,
    frame_cycles: u32,
    frame_index: u64,
    budget: i64,
    total_budget: i64,
    last_norm: Option<Vec<u32>>,
    out: Vec<u8>,
    prev_start: bool,
    prev_sb: u8,
    saw_running: bool,
    worst: Option<FrameMismatch>,
    refs: Vec<Vec<u32>>,
    began: bool,
    awaiting_regs: bool,
    frames_at_open: u32,
}

fn num(text: &str) -> Result<u64, String> {
    let t = text.trim();
    match t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")) {
        Some(h) => u64::from_str_radix(h, 16),
        None => t.parse(),
    }
    .map_err(|_| format!("bad number {text:?}"))
}

fn parse_input(script: &str) -> Result<Vec<InputEvent>, String> {
    let mut events = Vec::new();
    let mut last_frame = 0u32;
    for tok in script.split(',') {
        let tok = tok.trim();
        if tok.is_empty() {
            continue;
        }
        let (when, buttons_str) = tok
            .split_once(':')
            .ok_or_else(|| format!("bad input event {tok}: missing ':'"))?;
        let (frame_str, ly) = match when.split_once('@') {
            Some((f, ly)) => (
                f,
                Some(
                    ly.parse::<u8>()
                        .ok()
                        .filter(|ly| *ly <= 153)
                        .ok_or_else(|| format!("bad input event {tok}: LY must be 0-153"))?,
                ),
            ),
            None => (when, None),
        };
        let frame = frame_str
            .parse::<u32>()
            .map_err(|_| format!("bad input event {tok}: bad frame number"))?;
        if frame < last_frame {
            return Err(format!(
                "input events must be in non-decreasing frame order (event {tok})"
            ));
        }
        last_frame = frame;
        let mut buttons = 0u8;
        if buttons_str != "-" {
            for name in buttons_str.split('+') {
                buttons |= match name.trim().to_ascii_lowercase().as_str() {
                    "a" => crate::BTN_A,
                    "b" => crate::BTN_B,
                    "select" => crate::BTN_SELECT,
                    "start" => crate::BTN_START,
                    "right" => crate::BTN_RIGHT,
                    "left" => crate::BTN_LEFT,
                    "up" => crate::BTN_UP,
                    "down" => crate::BTN_DOWN,
                    other => return Err(format!("unknown button {other}")),
                };
            }
        }
        events.push(InputEvent { frame, ly, buttons });
    }
    Ok(events)
}

fn escape(bytes: &[u8]) -> String {
    let mut s = String::new();
    for &b in bytes {
        match b {
            b'\\' => s.push_str("\\\\"),
            b'\n' => s.push_str("\\n"),
            b'\r' => s.push_str("\\r"),
            b'\t' => s.push_str("\\t"),
            0x20..=0x7E => s.push(char::from(b)),
            _ => s.push_str(&format!("\\x{b:02x}")),
        }
    }
    s
}

fn verdict_tail(s: &str) -> String {
    let tail: String = s
        .chars()
        .rev()
        .take(60)
        .collect::<String>()
        .chars()
        .rev()
        .collect();
    tail.replace('\n', " ").trim().to_string()
}

fn compare_dump(
    label: &str,
    expected: &[u8],
    actual: &[u8],
    skip: &[Range<usize>],
) -> Result<(), String> {
    if actual.len() < expected.len() {
        return Err(format!(
            "{label}: captured region too small ({} bytes available, {} expected)",
            actual.len(),
            expected.len()
        ));
    }
    for (offset, (&want, &got)) in expected.iter().zip(actual.iter()).enumerate() {
        if skip.iter().any(|range| range.contains(&offset)) {
            continue;
        }
        if want != got {
            return Err(format!(
                "{label}: first mismatch at offset {offset:#06X}: expected {want:#04X}, got {got:#04X}",
            ));
        }
    }
    Ok(())
}

impl Case {
    /// Parses `spec` and applies the per-case machine setup.
    pub(crate) fn open(m: &mut Rlgbd, spec: &str) -> Result<Self, String> {
        let mut protocol = String::new();
        let mut frames = None;
        let mut cycle_key = None;
        let mut marker = None;
        let mut addr = None;
        let mut expect = None;
        let mut capture = None;
        let mut pass_text = None;
        let mut fails = Vec::new();
        let mut refs: Vec<PathBuf> = Vec::new();
        let mut skip = Vec::new();
        let mut region = None;
        let mut input = String::new();
        let mut expect_hex = None;
        let mut lazy = false;
        let mut mode = String::from("dmg");
        for line in spec.lines() {
            let line = line.trim();
            if line.is_empty() {
                continue;
            }
            let (k, v) = line
                .split_once('=')
                .ok_or_else(|| format!("bad spec line {line:?}"))?;
            let v = v.trim();
            match k.trim() {
                "protocol" => protocol = v.to_string(),
                "frames" => frames = Some(num(v)?),
                "cycles" => cycle_key = Some(num(v)?),
                "marker" => marker = Some(num(v)? as u8),
                "addr" => addr = Some(num(v)? as u16),
                "expect" => expect = Some(num(v)? as u8),
                "capture" => capture = Some(v.to_string()),
                "pass_text" => pass_text = Some(v.to_string()),
                "fail_text" => fails.push(v.to_string()),
                "ref" => refs.push(PathBuf::from(v)),
                "skip" => {
                    let (a, b) = v
                        .split_once('-')
                        .ok_or_else(|| format!("bad skip range {v:?}"))?;
                    skip.push(num(a)? as usize..num(b)? as usize);
                }
                "region" => region = Some(v.to_string()),
                "input" => input = v.to_string(),
                "expect_hex" => expect_hex = Some(v.to_string()),
                "audible" => {}
                "cart" => lazy = v == "lazy_sram_cs",
                "mode" => mode = v.to_string(),
                other => return Err(format!("unknown spec key {other:?}")),
            }
        }
        let first_ref = |refs: &[PathBuf]| {
            refs.first()
                .cloned()
                .ok_or_else(|| format!("protocol {protocol} needs a ref"))
        };
        let kind = match protocol.as_str() {
            "run" => Kind::Run,
            "mem" => Kind::Mem {
                addr: addr.ok_or("mem needs addr")?,
                expect: expect.ok_or("mem needs expect")?,
            },
            "mooneye" => Kind::Mooneye {
                marker: marker.unwrap_or(0x40),
            },
            "mooneye_ed" => Kind::Mooneye {
                marker: marker.unwrap_or(0xED),
            },
            "serial" => Kind::Serial {
                pass: pass_text.unwrap_or_else(|| "Passed".to_string()),
                fails: if fails.is_empty() {
                    vec!["Failed".to_string(), "Error".to_string()]
                } else {
                    fails
                },
                sc_edge: capture.as_deref() != Some("sb_change"),
            },
            "serial_text" => Kind::Serial {
                pass: pass_text.ok_or("serial_text needs pass_text")?,
                fails,
                sc_edge: capture.as_deref() == Some("sc_edge"),
            },
            "blargg_mem" => Kind::BlarggMem,
            "png" | "png_fixed" | "png_layout" => Kind::Csp {
                path: first_ref(&refs)?,
                fixed: protocol == "png_fixed",
                recolor: protocol == "png_layout",
            },
            "png_shootout" => {
                first_ref(&refs)?;
                Kind::Shootout { refs }
            }
            "sram" => Kind::Sram {
                path: first_ref(&refs)?,
                skip,
            },
            "region_dump" => {
                let (base, name) = match region.as_deref() {
                    Some("oam") => (0xFE00, "Oam"),
                    Some("vram") => (0x8000, "Vram"),
                    other => return Err(format!("bad region {other:?}")),
                };
                Kind::Region {
                    path: first_ref(&refs)?,
                    base,
                    name,
                }
            }
            "gambatte_hex" => Kind::Hex {
                expected: expect_hex.ok_or("gambatte_hex needs expect_hex")?,
            },
            "gambatte_png" => Kind::GambattePng {
                path: first_ref(&refs)?,
            },
            "gambatte_audio" => Kind::Audio,
            other => return Err(format!("unknown protocol {other:?}")),
        };
        let cgb_mode = matches!(mode.as_str(), "cgb" | "agb");
        if cgb_mode {
            let csp = matches!(kind, Kind::Csp { .. });
            m.gb.set_cgb_color_conversion(if csp {
                ColorCorrection::Linear
            } else {
                ColorCorrection::Lcd
            });
        }
        if lazy && m.flags & crate::NEW_LAZY_SRAM_CS == 0 {
            m.gb.set_cart_sram_cs_lazy(true);
        }
        if matches!(kind, Kind::Sram { .. }) && m.flags & crate::NEW_BOOT_RESIDUE == 0 {
            m.gb.skip_bios_with_boot_residue();
        }
        let uses_input = matches!(
            kind,
            Kind::Csp { .. } | Kind::Shootout { .. } | Kind::Sram { .. } | Kind::Region { .. }
        );
        let frames_given = frames.is_some();
        let frames = frames.unwrap_or(DEFAULT_FRAMES);
        Ok(Self {
            prev_sb: m.gb.read_memory(0xFF01),
            frames_at_open: m.frames,
            kind,
            frames,
            frames_given,
            cycle_key,
            input: if uses_input {
                parse_input(&input)?
            } else {
                Vec::new()
            },
            input_next: 0,
            cycles: 0,
            last_now: None,
            verdict: None,
            regs: None,
            frame_cycles: 0,
            frame_index: 0,
            budget: 0,
            total_budget: 0,
            last_norm: None,
            out: Vec::new(),
            prev_start: false,
            saw_running: false,
            worst: None,
            refs: Vec::new(),
            began: false,
            awaiting_regs: false,
        })
    }

    /// The detail of the verdict.
    pub(crate) fn detail(&self) -> &str {
        self.verdict.as_ref().map_or("", |v| v.1.as_str())
    }

    fn done(&mut self, v: Verdict, detail: impl Into<String>) -> Verdict {
        self.verdict = Some((v, detail.into()));
        v
    }

    fn pass(&mut self) -> Verdict {
        self.done(Verdict::Pass, "ok")
    }

    fn fail(&mut self, detail: impl Into<String>) -> Verdict {
        self.done(Verdict::Fail, detail)
    }

    fn poll_input(&mut self, m: &mut Rlgbd) {
        while let Some(event) = self.input.get(self.input_next).copied() {
            let due = u64::from(event.frame) * CYCLES_PER_FRAME;
            if self.cycles < due {
                break;
            }
            if let Some(ly) = event.ly
                && m.gb.read_memory(LY) != ly
                && self.cycles < due + 2 * CYCLES_PER_FRAME
            {
                break;
            }
            m.set_buttons(event.buttons);
            self.input_next += 1;
        }
    }

    fn poll_input_frame(&mut self, m: &mut Rlgbd, frame_index: u64) {
        while let Some(event) = self.input.get(self.input_next).copied() {
            if u64::from(event.frame) > frame_index {
                break;
            }
            m.set_buttons(event.buttons);
            self.input_next += 1;
        }
    }

    fn frame_ready(m: &Rlgbd) -> bool {
        m.gb.get_ppu_debug_info().0.frame_ready()
    }

    fn take(&mut self, m: &mut Rlgbd) {
        m.take_frame();
        self.last_norm = Some(if m.gb.frame_renders_color() {
            imaging::normalize_color(&m.frame)
        } else {
            imaging::normalize_mono(&m.gb.presented_shade_frame()[..])
        });
    }

    fn read_ram(m: &Rlgbd, off: usize) -> u8 {
        m.gb.cartridge()
            .map_or(0xFF, |c| c.save_ram().get(off).copied().unwrap_or(0xFF))
    }

    fn csp_verdict(&mut self, path: &std::path::Path, recolor: bool) -> Verdict {
        let Some(actual) = self.last_norm.take() else {
            return self.fail("no frame produced");
        };
        let expected = match imaging::read_png_rgb(path) {
            Ok(e) => e,
            Err(e) => return self.fail(format!("reference PNG {}: {e}", path.display())),
        };
        let mismatch = if recolor {
            imaging::frame_buffer_mismatch_recolor(&actual, &expected)
        } else {
            imaging::frame_buffer_mismatch(&actual, &expected)
        };
        match mismatch {
            None => self.pass(),
            Some(mm) => {
                let kind = if recolor {
                    "layout (recolor-invariant)"
                } else {
                    "PNG"
                };
                self.fail(format!(
                    "screen did not match {kind} {}: {}",
                    path.display(),
                    mm.describe()
                ))
            }
        }
    }

    fn dump_verdict(&mut self, m: &Rlgbd) -> Verdict {
        let result = match &self.kind {
            Kind::Sram { path, skip } => std::fs::read(path)
                .map_err(|e| format!("failed to read SRAM dump {}: {e}", path.display()))
                .and_then(|expected| {
                    let cart = m
                        .gb
                        .cartridge()
                        .ok_or_else(|| "no cartridge present when reading SRAM dump".to_string())?;
                    compare_dump(
                        &format!("SRAM dump {}", path.display()),
                        &expected,
                        cart.save_ram(),
                        skip,
                    )
                }),
            Kind::Region { path, base, name } => std::fs::read(path)
                .map_err(|e| format!("failed to read region dump {}: {e}", path.display()))
                .and_then(|expected| {
                    let actual: Vec<u8> = (0..expected.len())
                        .map(|offset| m.gb.read_memory(base.wrapping_add(offset as u16)))
                        .collect();
                    compare_dump(
                        &format!("region dump {} ({name} @ {base:#06X})", path.display()),
                        &expected,
                        &actual,
                        &[],
                    )
                }),
            _ => Err("not a dump case".to_string()),
        };
        match result {
            Ok(()) => self.pass(),
            Err(e) => self.fail(e),
        }
    }

    /// Polls at an instruction boundary; `pc` is the address of the next instruction.
    pub(crate) fn poll(&mut self, m: &mut Rlgbd, pc: u16) -> Verdict {
        if let Some((v, _)) = &self.verdict {
            return *v;
        }
        let now = m.gb.ext_now();
        let regs_poll = std::mem::take(&mut self.awaiting_regs);
        let post = if regs_poll {
            None
        } else {
            self.last_now.map(|last| match now.saturating_sub(last) {
                0 => STOPPED_CYCLES,
                dots => dots as u32,
            })
        };
        self.last_now = Some(now);
        if let Some(c) = post {
            self.cycles += u64::from(c);
        }
        match &self.kind {
            Kind::Audio => self.done(
                Verdict::Unsupported,
                "the external-CPU path generates no audio",
            ),
            Kind::Run => self.poll_run(m),
            Kind::Mem { addr, expect } => {
                let (addr, expect) = (*addr, *expect);
                self.poll_mem(m, addr, expect)
            }
            Kind::Mooneye { marker } => {
                let marker = *marker;
                self.poll_mooneye(m, pc, marker)
            }
            Kind::Serial { .. } => self.poll_serial(m),
            Kind::BlarggMem => self.poll_blargg(m),
            Kind::Csp { .. } => self.poll_csp(m, pc, post),
            Kind::Shootout { .. } => self.poll_shootout(m, post),
            Kind::Sram { .. } | Kind::Region { .. } => {
                let budget = self.frames.max(DUMP_MIN_FRAMES) * CYCLES_PER_FRAME;
                if self.cycles >= budget {
                    self.dump_verdict(m)
                } else {
                    self.poll_input(m);
                    Verdict::Continue
                }
            }
            Kind::Hex { .. } | Kind::GambattePng { .. } => self.poll_gambatte(m, post),
        }
    }

    fn capture_serial(&mut self, m: &Rlgbd, sc_edge: bool) -> Option<u8> {
        if sc_edge {
            let sc = m.gb.read_memory(0xFF02);
            let start = (sc & 0x80) != 0 && (sc & 0x01) != 0;
            let edge = start && !self.prev_start;
            self.prev_start = start;
            edge.then(|| m.gb.read_memory(0xFF01))
        } else {
            let sb = m.gb.read_memory(0xFF01);
            let changed = sb != self.prev_sb;
            self.prev_sb = sb;
            changed.then_some(sb)
        }
    }

    fn poll_run(&mut self, m: &mut Rlgbd) -> Verdict {
        m.frame_poll();
        if let Some(b) = self.capture_serial(m, true) {
            self.out.push(b);
        }
        let taken = u64::from(m.frames.wrapping_sub(self.frames_at_open));
        let by_cycles = self.cycle_key.filter(|_| !self.frames_given);
        let finished = match by_cycles {
            Some(c) => self.cycles >= c,
            None => taken >= self.frames,
        };
        let cap = self.frames.max(1) * u64::from(MAX_CYCLES_UNTIL_LCD_FRAME);
        if finished || self.cycles >= cap {
            let detail = format!("frame {:016x} serial {}", m.frame_hash(), escape(&self.out));
            return self.done(Verdict::Pass, detail);
        }
        Verdict::Continue
    }

    fn poll_mem(&mut self, m: &mut Rlgbd, addr: u16, expected: u8) -> Verdict {
        if self.cycles < self.frames * CYCLES_PER_FRAME {
            return Verdict::Continue;
        }
        m.gb.sync_lazy_peripherals();
        let got = m.gb.read_memory(addr);
        if got == expected {
            self.pass()
        } else if addr == 0xFF82 {
            let actual = m.gb.read_memory(0xFF80);
            let want = m.gb.read_memory(0xFF81);
            self.fail(format!(
                "FF82={got:02X} (want {expected:02X}); FF80(actual)={actual:02X} FF81(expected)={want:02X}"
            ))
        } else {
            self.fail(format!("[{addr:04X}]={got:02X} want {expected:02X}"))
        }
    }

    fn poll_mooneye(&mut self, m: &mut Rlgbd, pc: u16, marker: u8) -> Verdict {
        let cap = self.cycle_key.unwrap_or(MOONEYE_CAP);
        if self.cycles >= cap {
            return self.fail("no done-marker (timeout)");
        }
        if m.gb.read_memory(pc) != marker {
            return Verdict::Continue;
        }
        let Some(r) = self.regs else {
            self.awaiting_regs = true;
            return Verdict::NeedRegs;
        };
        if (r.b, r.c, r.d, r.e, r.h, r.l) == (3, 5, 8, 13, 21, 34) {
            self.pass()
        } else {
            self.fail(format!(
                "regs B={:02X} C={:02X} D={:02X} E={:02X} H={:02X} L={:02X} (want 03 05 08 0D 15 22)",
                r.b, r.c, r.d, r.e, r.h, r.l
            ))
        }
    }

    fn poll_serial(&mut self, m: &mut Rlgbd) -> Verdict {
        let Kind::Serial {
            pass,
            fails,
            sc_edge,
        } = &self.kind
        else {
            return Verdict::Fail;
        };
        let (pass, fails, sc_edge) = (pass.clone(), fails.clone(), *sc_edge);
        let budget = self.frames * CYCLES_PER_FRAME;
        if self.cycles >= budget {
            let s = String::from_utf8_lossy(&self.out).into_owned();
            return if s.contains(&pass) {
                self.pass()
            } else if s.is_empty() {
                self.fail("no serial output (timeout)")
            } else {
                self.fail(format!("no {pass:?}; tail: {}", verdict_tail(&s)))
            };
        }
        let min_len = fails
            .iter()
            .map(String::len)
            .chain(std::iter::once(pass.len()))
            .min()
            .unwrap_or(1);
        if let Some(byte) = self.capture_serial(m, sc_edge) {
            self.out.push(byte);
            if self.out.len() >= min_len {
                let s = String::from_utf8_lossy(&self.out).into_owned();
                if s.contains(&pass) {
                    return self.pass();
                }
                if fails.iter().any(|f| s.contains(f.as_str())) {
                    return self.fail(format!("serial verdict: {}", verdict_tail(&s)));
                }
            }
        }
        Verdict::Continue
    }

    fn poll_blargg(&mut self, m: &mut Rlgbd) -> Verdict {
        let sig = [
            Self::read_ram(m, 1),
            Self::read_ram(m, 2),
            Self::read_ram(m, 3),
        ];
        let status = Self::read_ram(m, 0);
        if sig == [0xDE, 0xB0, 0x61] && status == 0x80 {
            self.saw_running = true;
        }
        if self.saw_running && sig == [0xDE, 0xB0, 0x61] && status != 0x80 {
            let mut txt = Vec::new();
            for off in 4usize..0x200 {
                let b = Self::read_ram(m, off);
                if b == 0 {
                    break;
                }
                txt.push(b);
            }
            let s = String::from_utf8_lossy(&txt).replace('\n', " ");
            if status == 0x00 {
                return self.pass();
            }
            return self.fail(format!("code={status:02X}: {}", s.trim()));
        }
        if self.cycles >= self.frames * CYCLES_PER_FRAME {
            return self.fail("no result signature (timeout)");
        }
        Verdict::Continue
    }

    fn poll_csp(&mut self, m: &mut Rlgbd, pc: u16, post: Option<u32>) -> Verdict {
        let Kind::Csp {
            path,
            fixed,
            recolor,
        } = &self.kind
        else {
            return Verdict::Fail;
        };
        let (path, fixed, recolor) = (path.clone(), *fixed, *recolor);
        if fixed {
            if self.cycles >= self.frames * CYCLES_PER_FRAME {
                self.take(m);
                return self.csp_verdict(&path, recolor);
            }
            self.poll_input(m);
            return Verdict::Continue;
        }
        if self.frames == 0 {
            return self.fail("no frame produced");
        }
        if let Some(c) = post {
            self.frame_cycles = self.frame_cycles.wrapping_add(c);
            if Self::frame_ready(m) {
                self.take(m);
                self.frame_index += 1;
                self.frame_cycles = 0;
                if self.frame_index >= self.frames {
                    return self.csp_verdict(&path, recolor);
                }
            } else if self.frame_cycles >= MAX_CYCLES_UNTIL_LCD_FRAME {
                return self.fail(format!("frame {} timeout", self.frame_index));
            }
        }
        self.poll_input(m);
        if m.gb.read_memory(pc) == 0x40 {
            self.take(m);
            return self.csp_verdict(&path, recolor);
        }
        Verdict::Continue
    }

    fn shootout_end(&mut self, refs0: &std::path::Path) -> Verdict {
        match self.worst.take() {
            None => self.fail("no LCD frame within the case budget"),
            Some(mm) => self.fail(format!(
                "screen did not match shootout PNG {}: {}",
                refs0.display(),
                mm.describe()
            )),
        }
    }

    fn poll_shootout(&mut self, m: &mut Rlgbd, post: Option<u32>) -> Verdict {
        let Kind::Shootout { refs } = &self.kind else {
            return Verdict::Fail;
        };
        let paths = refs.clone();
        if !self.began {
            self.began = true;
            for r in &paths {
                match imaging::read_png_rgb(r) {
                    Ok(p) => self.refs.push(p),
                    Err(e) => return self.fail(format!("reference PNG {}: {e}", r.display())),
                }
            }
            self.total_budget = self.frames.max(1) as i64;
            self.budget = self.total_budget;
            self.poll_input_frame(m, 0);
            return Verdict::Continue;
        }
        let Some(c) = post else {
            return Verdict::Continue;
        };
        self.frame_cycles = self.frame_cycles.saturating_add(c);
        let mut advanced = false;
        if Self::frame_ready(m) {
            self.take(m);
            self.budget -= 1;
            advanced = true;
            let actual = self.last_norm.take().unwrap_or_default();
            let mut matched = false;
            for expected in &self.refs {
                match imaging::shootout_mismatch(&actual, expected) {
                    None => {
                        matched = true;
                        break;
                    }
                    Some(mm) => self.worst = Some(mm),
                }
            }
            if matched {
                return self.pass();
            }
        } else if self.frame_cycles >= MAX_CYCLES_UNTIL_LCD_FRAME {
            self.budget -= i64::from(MAX_CYCLES_UNTIL_LCD_FRAME / CYCLES_PER_FRAME as u32);
            advanced = true;
        }
        if advanced {
            self.frame_cycles = 0;
            if self.budget <= 0 {
                return self.shootout_end(&paths[0]);
            }
            let index = (self.total_budget - self.budget) as u64;
            self.poll_input_frame(m, index);
        }
        Verdict::Continue
    }

    fn poll_gambatte(&mut self, m: &mut Rlgbd, post: Option<u32>) -> Verdict {
        if self.frames == 0 {
            return self.fail("frame count must be greater than zero");
        }
        let Some(c) = post else {
            return Verdict::Continue;
        };
        self.frame_cycles = self.frame_cycles.saturating_add(c);
        if Self::frame_ready(m) {
            self.take(m);
            self.frame_index += 1;
            self.frame_cycles = 0;
            if self.frame_index >= self.frames {
                return self.gambatte_verdict();
            }
        } else if self.frame_cycles >= MAX_CYCLES_UNTIL_LCD_FRAME {
            return self.fail(format!(
                "timed out waiting for LCD frame while running frame {}",
                self.frame_index
            ));
        }
        Verdict::Continue
    }

    fn gambatte_verdict(&mut self) -> Verdict {
        let Some(frame) = self.last_norm.take() else {
            return self.fail("no frame was produced");
        };
        match &self.kind {
            Kind::Hex { expected } => {
                let expected = expected.clone();
                match imaging::hex_output_mismatch(&frame, &expected) {
                    None => self.pass(),
                    Some(mm) => {
                        self.fail(format!("screen did not match hex output {expected}: {mm}"))
                    }
                }
            }
            Kind::GambattePng { path } => {
                let path = path.clone();
                match imaging::read_png_rgb(&path) {
                    Err(e) => self.fail(e),
                    Ok(expected) => match imaging::frame_buffer_mismatch(&frame, &expected) {
                        None => self.pass(),
                        Some(mm) => self.fail(format!(
                            "screen did not match PNG {}: {}",
                            path.display(),
                            mm.describe()
                        )),
                    },
                }
            }
            _ => self.fail("not a gambatte case"),
        }
    }
}
