use super::{D, Num, Result, Slot};
use crate::inst::{Operand, RegMods};

fn rel_target(d: &D) -> i64 {
    d.pc as i64 + 16 + raw_target(d)
}

fn raw_target(d: &D) -> i64 {
    let off = d.w.get64(34, 48) as i64;
    ((off << 16) >> 16) * 4
}

fn abs_target(d: &D) -> i64 {
    if d.bit(91) {
        raw_target(d)
    } else {
        d.w.get64(34, 47) as i64 * 4
    }
}

fn sym(s: impl Into<String>) -> Operand {
    Operand::Sym(s.into())
}

fn guard_pred(d: &D) -> Option<Operand> {
    if d.get(87, 3) != 7 || d.bit(90) {
        Some(d.pred_src(87, 90))
    } else {
        None
    }
}

fn push_guard_pred(d: &mut D) {
    if let Some(p) = guard_pred(d) {
        d.op(p);
    }
}

fn pred_dst_at(d: &D, lo: u32) -> Operand {
    d.pred_at(lo)
}

fn state_name(n: u32) -> String {
    match n {
        0..=15 => format!("B{n}"),
        16..=20 => format!("THREAD_STATE_ENUM.{}", n - 16),
        21 => "TRAP_RETURN_PC.LO".into(),
        22 => "TRAP_RETURN_PC.HI".into(),
        23 => "TRAP_RETURN_MASK".into(),
        24 => "MEXITED".into(),
        25 => "MKILL".into(),
        26 => "MACTIVE".into(),
        27 => "MATEXIT".into(),
        28 => "OPT_STACK".into(),
        29 => "API_CALL_DEPTH".into(),
        30 => "ATEXIT_PC.LO".into(),
        31 => "ATEXIT_PC.HI".into(),
        _ => format!("???{n}"),
    }
}

fn bar_only_name(n: u32) -> String {
    if n < 16 {
        format!("B{n}")
    } else {
        format!("???{n}")
    }
}

const SR_NAMES: &[(u32, &str)] = &[
    (0, "SR_LANEID"),
    (1, "SR_CLOCK"),
    (2, "SR_VIRTCFG"),
    (3, "SR_VIRTID"),
    (15, "SR_ORDERING_TICKET"),
    (16, "SR_PRIM_TYPE"),
    (17, "SR_INVOCATION_ID"),
    (18, "SR_Y_DIRECTION"),
    (19, "SR_THREAD_KILL"),
    (20, "SM_SHADER_TYPE"),
    (21, "SR_DIRECTCBEWRITEADDRESSLOW"),
    (22, "SR_DIRECTCBEWRITEADDRESSHIGH"),
    (23, "SR_DIRECTCBEWRITEENABLED"),
    (24, "SR_SW_SCRATCH"),
    (25, "SR_MACHINE_ID_1"),
    (26, "SR_MACHINE_ID_2"),
    (27, "SR_MACHINE_ID_3"),
    (28, "SR_AFFINITY"),
    (29, "SR_INVOCATION_INFO"),
    (30, "SR_WSCALEFACTOR_XY"),
    (31, "SR_WSCALEFACTOR_Z"),
    (32, "SR_TID"),
    (33, "SR_TID.X"),
    (34, "SR_TID.Y"),
    (35, "SR_TID.Z"),
    (37, "SR_CTAID.X"),
    (38, "SR_CTAID.Y"),
    (39, "SR_CTAID.Z"),
    (40, "SR_NTID"),
    (41, "SR_CirQueueIncrMinusOne"),
    (42, "SR_NLATC"),
    (44, "SR_SM_SPA_VERSION"),
    (45, "SR_MULTIPASSSHADERINFO"),
    (46, "SR_LWINHI"),
    (47, "SR_SWINHI"),
    (48, "SR_SWINLO"),
    (49, "SR_SWINSZ"),
    (50, "SR_SMEMSZ"),
    (51, "SR_SMEMBANKS"),
    (52, "SR_LWINLO"),
    (53, "SR_LWINSZ"),
    (54, "SR_LMEMLOSZ"),
    (55, "SR_LMEMHIOFF"),
    (56, "SR_EQMASK"),
    (57, "SR_LTMASK"),
    (58, "SR_LEMASK"),
    (59, "SR_GTMASK"),
    (60, "SR_GEMASK"),
    (61, "SR_REGALLOC"),
    (62, "SR_BARRIERALLOC"),
    (64, "SR_GLOBALERRORSTATUS"),
    (66, "SR_WARPERRORSTATUS"),
    (67, "SR_VIRTUALSMID"),
    (68, "SR_VIRTUALENGINEID"),
    (80, "SR_CLOCKLO"),
    (81, "SR_CLOCKHI"),
    (82, "SR_GLOBALTIMERLO"),
    (83, "SR_GLOBALTIMERHI"),
    (84, "SR_ESR_PC"),
    (85, "SR_ESR_PC_HI"),
    (96, "SR_HWTASKID"),
    (97, "SR_CIRCULARQUEUEENTRYINDEX"),
    (98, "SR_CIRCULARQUEUEENTRYADDRESSLOW"),
    (99, "SR_CIRCULARQUEUEENTRYADDRESSHIGH"),
    (100, "SR_PM0"),
    (101, "SR_PM_HI0"),
    (102, "SR_PM1"),
    (103, "SR_PM_HI1"),
    (104, "SR_PM2"),
    (105, "SR_PM_HI2"),
    (106, "SR_PM3"),
    (107, "SR_PM_HI3"),
    (108, "SR_PM4"),
    (109, "SR_PM_HI4"),
    (110, "SR_PM5"),
    (111, "SR_PM_HI5"),
    (112, "SR_PM6"),
    (113, "SR_PM_HI6"),
    (114, "SR_PM7"),
    (115, "SR_PM_HI7"),
    (116, "SR_SNAP_PM0"),
    (117, "SR_SNAP_PM_HI0"),
    (118, "SR_SNAP_PM1"),
    (119, "SR_SNAP_PM_HI1"),
    (120, "SR_SNAP_PM2"),
    (121, "SR_SNAP_PM_HI2"),
    (122, "SR_SNAP_PM3"),
    (123, "SR_SNAP_PM_HI3"),
    (124, "SR_SNAP_PM4"),
    (125, "SR_SNAP_PM_HI4"),
    (126, "SR_SNAP_PM5"),
    (127, "SR_SNAP_PM_HI5"),
    (128, "SR_SNAP_PM6"),
    (129, "SR_SNAP_PM_HI6"),
    (130, "SR_SNAP_PM7"),
    (131, "SR_SNAP_PM_HI7"),
    (132, "SR_VARIABLE_RATE"),
    (133, "__HIR0X000"),
    (255, "SRZ"),
];

fn sr_name(n: u32) -> String {
    if n == 255 {
        return "SRZ".into();
    }
    match SR_NAMES.iter().find(|(i, _)| *i == n) {
        Some((_, s)) => (*s).to_string(),
        None => format!("SR{n}"),
    }
}

fn reg_or_ureg_then_target(d: &D, t: i64) -> Operand {
    let r = if d.bit(91) {
        d.ureg_at(24)
    } else {
        d.reg_plain(24)
    };
    sym(format!("{r} {}", crate::inst::fmt_hex(t)))
}

fn call_like(d: &mut D, name: &'static str) {
    d.i.mnemonic = name;
}

fn inc_dec(d: &mut D) {
    match d.get(85, 2) {
        1 => d.m("INC"),
        2 => d.m("DEC"),
        3 => d.m("INVALID3"),
        _ => {}
    }
}

pub(crate) fn decode(d: &mut D, op: u32) -> Result<bool> {
    match op {
        0x147 => {
            d.i.mnemonic = "BRA";
            match d.get(32, 2) {
                1 => d.m("U"),
                2 => d.m("DIV"),
                3 => d.m("CONV"),
                _ => {}
            }
            push_guard_pred(d);
            if d.bit(91) {
                let n = d.get(24, 6) as u8;
                let m = RegMods {
                    not: d.bit(30),
                    ..RegMods::default()
                };
                d.op(Operand::UReg(n, m));
            }
            d.op(Operand::Target(rel_target(d)));
        }
        0x14d => {
            d.i.mnemonic = "EXIT";
            d.m_if(d.bit(85), "PREEMPTED");
            d.m_if(d.bit(86), "NO_ATEXIT");
            push_guard_pred(d);
        }
        0x145 => {
            d.i.mnemonic = "BSSY";
            d.op(Operand::Barrier(d.get(16, 4) as u8));
            d.op(Operand::Target(rel_target(d)));
        }
        0x141 => {
            d.i.mnemonic = "BSYNC";
            push_guard_pred(d);
            d.op(Operand::Barrier(d.get(16, 4) as u8));
        }
        0x118 => {
            d.i.mnemonic = "NOP";
        }
        0x144 => {
            if !matches!(d.form(), 1 | 4) {
                return d.err("call form");
            }
            call_like(d, "CALL");
            d.m("REL");
            d.m_if(d.bit(86), "NOINC");
            push_guard_pred(d);
            let t = rel_target(d);
            if d.form() == 1 || d.bit(91) {
                let o = reg_or_ureg_then_target(d, t);
                d.op(o);
            } else {
                d.op(Operand::Target(t));
            }
        }
        0x150 => {
            if d.form() != 4 {
                return d.err("ret form");
            }
            d.i.mnemonic = "RET";
            if d.bit(85) {
                d.m("ABS");
            } else {
                d.m("REL");
            }
            d.m_if(d.bit(86), "NODEC");
            push_guard_pred(d);
            let t = if d.bit(85) {
                raw_target(d)
            } else {
                rel_target(d)
            };
            let o = reg_or_ureg_then_target(d, t);
            d.op(o);
        }
        0x149 => {
            if d.form() != 4 {
                return d.err("brx form");
            }
            d.i.mnemonic = "BRX";
            inc_dec(d);
            push_guard_pred(d);
            let t = raw_target(d);
            if t == 0 {
                let r = if d.bit(91) {
                    d.ureg_at(24)
                } else {
                    d.reg_plain(24)
                };
                d.op(r);
            } else {
                let o = reg_or_ureg_then_target(d, t);
                d.op(o);
            }
        }
        0x158 => {
            if d.form() != 4 {
                return d.err("brxu form");
            }
            d.i.mnemonic = "BRXU";
            inc_dec(d);
            match d.get(32, 2) {
                1 => d.m("U"),
                2 => d.m("DIV"),
                3 => d.m("CONV"),
                _ => {}
            }
            push_guard_pred(d);
            let r = d.ureg_at(24);
            d.op(sym(format!("{r} {}", crate::inst::fmt_hex(raw_target(d)))));
        }
        0x142 => {
            d.i.mnemonic = "BREAK";
            push_guard_pred(d);
            d.op(Operand::Barrier(d.get(16, 4) as u8));
        }
        0x15b => {
            d.i.mnemonic = "KILL";
            push_guard_pred(d);
        }
        0x11a => {
            if d.form() != 4 {
                return d.err("depbar form");
            }
            d.i.mnemonic = "DEPBAR";
            if d.bit(47) {
                d.m("LE");
                let sb = d.get(44, 3);
                if sb < 6 {
                    d.op(Operand::ScoreBoard(sb as u8));
                } else {
                    d.op(sym(format!("INVALID{sb}")));
                }
                d.op(Operand::Hex(d.get(38, 6) as u64));
                let mask = d.get(32, 6);
                if mask != 0 {
                    let items: Vec<String> = (0..6)
                        .rev()
                        .filter(|b| mask >> b & 1 == 1)
                        .map(|b| b.to_string())
                        .collect();
                    d.op(sym(format!("{{{}}}", items.join(","))));
                }
            }
        }
        0x119 => {
            d.i.mnemonic = "S2R";
            d.op(d.dst());
            d.op(sym(sr_name(d.get(72, 8))));
        }
        0x1c3 => {
            d.i.mnemonic = "S2UR";
            d.op(d.udst());
            d.op(sym(sr_name(d.get(72, 8))));
        }
        0x1c2 => {
            d.i.mnemonic = "R2UR";
            d.m_if(d.bit(84), "OR");
            if d.get(81, 3) != 7 || d.bit(84) {
                d.op(pred_dst_at(d, 81));
            }
            d.op(d.udst());
            d.op(d.reg_plain(24));
        }
        0x1c4 => {
            d.i.mnemonic = "REDUX";
            match d.get(78, 3) {
                0 => d.m("AND"),
                1 => d.m("OR"),
                2 => d.m("XOR"),
                3 => d.m("SUM"),
                4 => d.m("MIN"),
                5 => d.m("MAX"),
                7 => d.m("INVALID7"),
                _ => {}
            }
            d.m_if(d.bit(73), "S32");
            d.op(d.udst());
            d.op(d.reg_plain(24));
        }
        0x006 | 0x086 => {
            let u = op == 0x086;
            d.i.mnemonic = if u { "VOTEU" } else { "VOTE" };
            match d.get(72, 2) {
                0 => d.m("ALL"),
                1 => d.m("ANY"),
                2 => d.m("EQ"),
                _ => d.m("INVALID3"),
            }
            if u {
                if d.get(16, 6) != 63 {
                    d.op(d.udst());
                }
                d.op(d.upred_at(81));
            } else {
                if d.get(16, 8) != 255 {
                    d.op(d.dst());
                }
                d.op(d.pred_at(81));
            }
            d.op(d.pred_src(87, 90));
        }
        0x1a1 => {
            d.i.mnemonic = "MATCH";
            let any = d.bit(79);
            d.m(if any { "ANY" } else { "ALL" });
            d.m_if(d.bit(73), "U64");
            if !any {
                d.op(d.pred_at(81));
            }
            d.op(d.dst());
            d.op(d.reg_plain(24));
        }
        0x155 => {
            d.i.mnemonic = "BMOV";
            d.m("32");
            let clear = d.bit(84);
            d.m_if(clear, "CLEAR");
            match d.form() {
                1 => d.op(d.dst()),
                7 => d.op(sym(format!("B{}", d.get(16, 4)))),
                _ => return d.err("bmov form"),
            }
            let n = d.get(24, 6);
            d.op(sym(if clear {
                bar_only_name(n)
            } else {
                state_name(n)
            }));
        }
        0x156 => {
            d.i.mnemonic = "BMOV";
            d.m("32");
            d.m_if(d.bit(84), "PQUAD");
            d.op(sym(state_name(d.get(24, 6))));
            let s = d.src(Slot::B, Num::UInt, false)?;
            d.op(s);
        }
        0x005 => {
            d.i.mnemonic = "CS2R";
            d.m_if(!d.bit(80), "32");
            d.op(d.dst());
            d.op(sym(sr_name(d.get(72, 8))));
        }
        0x15d | 0x148 => {
            if !matches!(d.form(), 1 | 4 | 5) {
                return d.err("form");
            }
            if op == 0x15d {
                d.i.mnemonic = "NANOSLEEP";
                d.m_if(d.bit(86), "RAND");
                d.m_if(d.bit(85), "WARP");
            } else {
                d.i.mnemonic = "WARPSYNC";
                d.m_if(d.form() == 1 && d.bit(86), "EXCLUSIVE");
            }
            push_guard_pred(d);
            let o = d.src(Slot::B, Num::UInt, false)?;
            d.op(o);
        }
        0x146 => {
            if d.form() != 4 {
                return d.err("yield form");
            }
            d.i.mnemonic = "YIELD";
            push_guard_pred(d);
        }
        0x143 => {
            if d.form() != 4 {
                return d.err("call form");
            }
            d.i.mnemonic = "CALL";
            d.m("ABS");
            d.m_if(d.bit(86), "NOINC");
            push_guard_pred(d);
            let t = abs_target(d);
            if d.bit(91) {
                let r = d.ureg_at(24);
                d.op(sym(format!("{r} {}", crate::inst::fmt_hex(t))));
            } else {
                d.op(Operand::Target(t));
            }
        }
        0x14a => {
            if d.form() != 4 {
                return d.err("jmp form");
            }
            d.i.mnemonic = "JMP";
            inc_dec(d);
            let ur = d.bit(91);
            match (d.get(32, 2), ur) {
                (0, true) => d.m("INVALID0"),
                (1, true) => d.m("INVALID1"),
                (1, false) => d.m("U"),
                (2, _) => d.m("DIV"),
                (3, _) => d.m("CONV"),
                _ => {}
            }
            push_guard_pred(d);
            if ur {
                d.op(d.ureg_at(24));
            }
            d.op(Operand::Target(d.w.get64(34, 47) as i64 * 4));
        }
        0x03c => hmma(d)?,
        0x037 => imma(d)?,
        0x03b => ldsm(d)?,
        0x03a => movm(d)?,
        _ => return Ok(false),
    }
    Ok(true)
}

fn tensor_upred(d: &mut D) {
    let v = d.get(87, 3);
    if v != 0 || d.bit(90) {
        d.op(Operand::UPred(7 - v as u8, d.bit(90)));
    }
}

fn signed_reg(d: &D, lo: u32, neg: bool, reuse: u32) -> Operand {
    let o = d.reg_plain(lo);
    match o {
        Operand::Reg(n, mut m) => {
            m.neg = neg;
            m.reuse = d.bit(reuse) && n != 255;
            Operand::Reg(n, m)
        }
        o => o,
    }
}

fn sparse_tail(d: &mut D) {
    let m = RegMods {
        reuse: d.bit(50),
        ..RegMods::default()
    };
    d.op(Operand::Reg(d.get(40, 8) as u8, m));
    d.op(Operand::Hex(d.get(48, 2) as u64));
}

fn reuse_str(d: &D, lo: u32, bit: u32) -> String {
    let r = signed_reg(d, lo, false, bit);
    r.to_string()
}

fn hmma(d: &mut D) -> Result<()> {
    if d.form() != 1 {
        return d.err("hmma form");
    }
    d.i.mnemonic = "HMMA";
    let sp = d.bit(73);
    d.m_if(sp, "SP");
    match (sp, d.get(75, 1) | d.get(78, 1) << 1) {
        (_, 0) => d.m("1688"),
        (_, 1) => d.m("16816"),
        (false, 2) => d.m("1684"),
        (true, 2) => d.m("INVALID2"),
        (true, _) => d.m("16832"),
        (false, _) => d.m("INVALID3"),
    }
    d.m(if d.bit(76) { "F32" } else { "F16" });
    match d.get(82, 2) {
        1 => d.m("BF16"),
        2 => d.m("TF32"),
        3 => d.m("INVALID3"),
        _ => {}
    }
    d.op(d.dst());
    d.op(signed_reg(d, 24, d.bit(72), 122));
    d.op(signed_reg(d, 32, d.bit(63), 123));
    d.op(d.reg_plain(64));
    tensor_upred(d);
    if sp {
        sparse_tail(d);
    }
    Ok(())
}

fn imma(d: &mut D) -> Result<()> {
    if d.form() != 1 {
        return d.err("imma form");
    }
    d.i.mnemonic = "IMMA";
    let sp = d.bit(72);
    d.m_if(sp, "SP");
    let sz = d.get(75, 1) | d.get(85, 2) << 1;
    d.m(match (sp, sz) {
        (false, 0) => "8816",
        (false, 2) => "8832",
        (false, 4) => "16816",
        (false, 5) => "16832",
        (false, 6) => "16864",
        (true, 2) => "8832",
        (true, 3) => "8864",
        (true, 5) => "16832",
        (true, 6) => "16864",
        (true, 7) => "168128",
        (_, 0) => "INVALID0",
        (_, 1) => "INVALID1",
        (_, 3) => "INVALID3",
        (_, 4) => "INVALID4",
        _ => "INVALID7",
    });
    let ty = |d: &D, lo: u32, wide: u32| -> &'static str {
        match (d.get(lo, 2), d.bit(wide)) {
            (0, false) => "U8",
            (1, false) => "S8",
            (0, true) => "U4",
            (1, true) => "S4",
            (2, false) => "INVALID2",
            (_, false) => "INVALID3",
            (2, true) => "INVALID6",
            (_, true) => "INVALID7",
        }
    };
    let a = ty(d, 76, 83);
    let b = ty(d, 78, 84);
    d.m(a);
    d.m(b);
    d.m_if(d.bit(82), "SAT");
    d.op(d.dst());
    let la = if d.bit(73) { "???1" } else { "ROW" };
    let lb = if d.bit(74) { "COL" } else { "???0" };
    d.op(sym(format!("{}.{la}", reuse_str(d, 24, 122))));
    d.op(sym(format!("{}.{lb}", reuse_str(d, 32, 123))));
    d.op(d.reg_plain(64));
    tensor_upred(d);
    if sp {
        sparse_tail(d);
    }
    Ok(())
}

fn ldsm(d: &mut D) -> Result<()> {
    if d.form() != 4 {
        return d.err("ldsm form");
    }
    d.i.mnemonic = "LDSM";
    d.m(match d.get(75, 3) {
        0 => "16",
        5 => "INVALID5",
        6 => "INVALID6",
        7 => "INVALID7",
        _ => return d.err("size"),
    });
    match d.get(78, 2) {
        0 => d.m("M88"),
        1 => d.m("MT88"),
        2 => d.m("M816"),
        _ => d.m("M832"),
    }
    match d.get(72, 2) {
        1 => d.m("2"),
        2 => d.m("4"),
        3 => d.m("INVALID3"),
        _ => {}
    }
    d.op(d.dst());
    let off = (((d.get(40, 24) << 8) as i32) >> 8) as i64;
    let mut s = format!("[{}", d.reg_plain(24));
    if d.bit(91) {
        s.push_str(&format!("+{}", d.ureg_at(32)));
    }
    if off != 0 {
        s.push_str(&format!("+{}", crate::inst::fmt_hex(off)));
    }
    s.push(']');
    d.op(sym(s));
    Ok(())
}

fn movm(d: &mut D) -> Result<()> {
    if d.form() != 1 {
        return d.err("movm form");
    }
    d.i.mnemonic = "MOVM";
    d.m(match d.get(75, 3) {
        0 => "16",
        5 => "INVALID5",
        6 => "INVALID6",
        7 => "INVALID7",
        _ => return d.err("size"),
    });
    match d.get(78, 2) {
        0 => d.m("MT88"),
        1 => d.m("M832"),
        2 => d.m("M864"),
        _ => d.m("INVALID3"),
    }
    d.op(d.dst());
    d.op(d.reg_plain(24));
    Ok(())
}
