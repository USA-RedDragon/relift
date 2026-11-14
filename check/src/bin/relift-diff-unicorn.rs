//! The Unicorn engine of `relift diff`: `relift diff --engine
//! relift-diff-unicorn` compares a lifted function with what Unicorn does
//! from the same registers and live guest memory (docs/abi.md, "Differential
//! checks"). It lives here because Unicorn is GPL: nothing in the main
//! workspace links it.

use std::cell::Cell;
use std::process::ExitCode;
use std::rc::Rc;

use relift_diff::{Engine, Outcome, Pager, Reg, State, Status, Wire, serve};
use unicorn_engine::RegisterARM64 as R;
use unicorn_engine::unicorn_const::{Arch, HookType, MemType, Mode, Prot, uc_error};
use unicorn_engine::{RegisterARM64, Unicorn};

const PAGE: u64 = 4096;
const CPU_ARM64_MAX: i32 = 3;
const FETCH_UNMAPPED: [uc_error; 3] = [
    uc_error::FETCH_UNMAPPED,
    uc_error::READ_UNMAPPED,
    uc_error::WRITE_UNMAPPED,
];

const XR: [RegisterARM64; 31] = [
    R::X0,
    R::X1,
    R::X2,
    R::X3,
    R::X4,
    R::X5,
    R::X6,
    R::X7,
    R::X8,
    R::X9,
    R::X10,
    R::X11,
    R::X12,
    R::X13,
    R::X14,
    R::X15,
    R::X16,
    R::X17,
    R::X18,
    R::X19,
    R::X20,
    R::X21,
    R::X22,
    R::X23,
    R::X24,
    R::X25,
    R::X26,
    R::X27,
    R::X28,
    R::X29,
    R::X30,
];

fn xreg(n: i32) -> Result<RegisterARM64, uc_error> {
    usize::try_from(n)
        .ok()
        .and_then(|i| XR.get(i).copied())
        .ok_or(uc_error::ARG)
}

fn scalar(name: &str) -> Option<RegisterARM64> {
    Some(match name {
        "sp" => R::SP,
        "nzcv" => R::NZCV,
        "fpcr" => R::FPCR,
        "tpidr_el0" => R::TPIDR_EL0,
        "tpidrro_el0" => R::TPIDRRO_EL0,
        _ => return None,
    })
}

fn write_reg(uc: &mut Unicorn<'_, ()>, name: &str, value: u128) -> Result<(), uc_error> {
    if let Some(r) = scalar(name) {
        return uc.reg_write(r, value as u64);
    }
    let (kind, n) = name.split_at(1);
    let n: i32 = n.parse().map_err(|_| uc_error::ARG)?;
    match kind {
        "x" => {
            let r = xreg(n)?;
            uc.reg_write(r, value as u64)
        }
        "v" => uc.reg_write_long(R::Q0 as i32 + n, &value.to_le_bytes()),
        _ => Err(uc_error::ARG),
    }
}

fn read_reg(uc: &Unicorn<'_, ()>, name: &str) -> Result<u128, uc_error> {
    if let Some(r) = scalar(name) {
        return uc.reg_read(r).map(u128::from);
    }
    let (kind, n) = name.split_at(1);
    let n: i32 = n.parse().map_err(|_| uc_error::ARG)?;
    match kind {
        "x" => {
            let r = xreg(n)?;
            uc.reg_read(r).map(u128::from)
        }
        _ => {
            let b = uc.reg_read_long(R::Q0 as i32 + n)?;
            let mut a = [0u8; 16];
            a.copy_from_slice(&b[..16]);
            Ok(u128::from_le_bytes(a))
        }
    }
}

fn names() -> Vec<String> {
    let mut n: Vec<String> = (0..31).map(|i| format!("x{i}")).collect();
    n.extend(["sp", "nzcv", "fpcr", "tpidr_el0", "tpidrro_el0"].map(String::from));
    n.extend((0..32).map(|i| format!("v{i}")));
    n
}

struct UnicornEngine;

impl UnicornEngine {
    fn run_in(state: &State, pager: &mut Pager<'_>, limit: u64) -> Result<Outcome, uc_error> {
        let mut uc = Unicorn::new(Arch::ARM64, Mode::ARM)?;
        uc.ctl_set_cpu_model(CPU_ARM64_MAX)?;
        uc.reg_write(R::CPACR_EL1, 3 << 20)?;
        let fault = Rc::new(Cell::new(0u64));
        let f = fault.clone();
        uc.add_mem_hook(
            HookType::MEM_UNMAPPED,
            1,
            0,
            move |_: &mut Unicorn<'_, ()>, _: MemType, addr: u64, _: usize, _: i64| {
                f.set(addr);
                false
            },
        )?;
        let low = Rc::new(Cell::new(u64::MAX));
        let l = low.clone();
        uc.add_code_hook(1, 0, move |uc: &mut Unicorn<'_, ()>, _: u64, _: u32| {
            if let Ok(sp) = uc.reg_read(R::SP) {
                l.set(l.get().min(sp));
            }
        })?;
        for r in &state.regs {
            write_reg(&mut uc, &r.name, r.value)?;
        }
        let sp0 = uc.reg_read(R::SP)?;
        let mut pc = state.pc;
        let mut left = limit;
        let status = loop {
            let res = uc.emu_start(
                pc,
                state.stop,
                0,
                usize::try_from(left).unwrap_or(usize::MAX),
            );
            pc = uc.reg_read(R::PC)?;
            match res {
                Ok(()) if pc == state.stop => break Status::Returned,
                Ok(()) => break Status::Failed(format!("step limit {limit} reached at {pc:#x}")),
                Err(e) if FETCH_UNMAPPED.contains(&e) => {
                    let page = fault.get() & !(PAGE - 1);
                    match pager.original(page) {
                        Some(data) => {
                            uc.mem_map(page, PAGE, Prot::ALL)?;
                            uc.mem_write(page, &data)?;
                            left = left.saturating_sub(1).max(1);
                        }
                        None if e == uc_error::FETCH_UNMAPPED => {
                            break Status::Left(format!(
                                "pc {:#x} is not guest memory: a runtime-provided function",
                                fault.get()
                            ));
                        }
                        None => {
                            break Status::Failed(format!(
                                "access to unmapped {:#x} at {pc:#x}",
                                fault.get()
                            ));
                        }
                    }
                }
                Err(uc_error::EXCEPTION) => {
                    break Status::Left(format!("exception (a system call or trap) at {pc:#x}"));
                }
                Err(e) => break Status::Failed(format!("{e:?} at {pc:#x}")),
            }
        };
        let regs = names()
            .into_iter()
            .map(|name| {
                let value = read_reg(&uc, &name)?;
                Ok(Reg { name, value })
            })
            .collect::<Result<Vec<_>, uc_error>>()?;
        for region in uc.mem_regions()? {
            let mut a = region.begin;
            while a < region.end {
                let mut buf = vec![0u8; PAGE as usize];
                uc.mem_read(a, &mut buf)?;
                pager.set_page(a, buf);
                a += PAGE;
            }
        }
        let low = low.get();
        Ok(Outcome {
            status,
            steps: 0,
            regs,
            scratch: (low < sp0).then_some((low, sp0)),
        })
    }
}

impl Engine for UnicornEngine {
    fn run(&mut self, state: &State, pager: &mut Pager<'_>, limit: u64) -> Outcome {
        match UnicornEngine::run_in(state, pager, limit) {
            Ok(outcome) => outcome,
            Err(e) => Outcome {
                status: Status::Failed(format!("unicorn: {e:?}")),
                steps: 0,
                regs: Vec::new(),
                scratch: None,
            },
        }
    }
}

fn main() -> ExitCode {
    let mut wire = Wire::stdio();
    match serve(&mut wire, &mut UnicornEngine, &mut std::io::stderr()) {
        Ok(_) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("relift-diff-unicorn: {e}");
            ExitCode::FAILURE
        }
    }
}
