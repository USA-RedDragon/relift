//! The units of a ROM: static binaries, autoload blocks and overlays, as
//! listed, hashed and extracted by `relift-ndsrom`.

use std::path::{Path, PathBuf};

use relift_nds_formats::checksum::{hex, sha256};
use relift_nds_formats::{Cpu, Rom, StaticUnit};
use serde_json::{Value, json};

use crate::BoxError;

/// One unit with its decompressed bytes.
pub struct Unit {
    /// The file name `extract` writes, such as `arm9_static.bin`.
    pub name: String,
    /// What the unit is: `static`, `autoload` or `overlay`.
    pub kind: &'static str,
    /// The processor.
    pub cpu: Cpu,
    /// The load address.
    pub addr: u32,
    /// The bss size after the data, when the unit has one.
    pub bss: u32,
    /// The overlay id.
    pub id: Option<u32>,
    /// Whether the unit was compressed in the image.
    pub compressed: bool,
    /// The decompressed bytes.
    pub bytes: Vec<u8>,
}

impl Unit {
    /// The SHA-256 of the decompressed bytes, in hex.
    #[must_use]
    pub fn sha256(&self) -> String {
        hex(&sha256(&self.bytes))
    }
}

/// The static binary of `cpu`, its autoload blocks and then its overlays.
///
/// # Errors
///
/// Any unit lies outside the image or does not decode.
pub fn collect(rom: &Rom<'_>) -> Result<Vec<Unit>, BoxError> {
    let mut out = Vec::new();
    for cpu in [Cpu::Arm9, Cpu::Arm7] {
        let unit = match cpu {
            Cpu::Arm9 => rom.arm9()?,
            Cpu::Arm7 => rom.arm7()?,
        };
        let n = cpu.name();
        out.push(Unit {
            name: format!("{n}_static.bin"),
            kind: "static",
            cpu,
            addr: unit.base,
            bss: 0,
            id: None,
            compressed: unit.compressed,
            bytes: unit.image.clone(),
        });
        for a in &unit.autoloads {
            out.push(Unit {
                name: format!("{n}_auto_{:08x}.bin", a.addr),
                kind: "autoload",
                cpu,
                addr: a.addr,
                bss: a.bss,
                id: None,
                compressed: false,
                bytes: unit.autoload_data(a).to_vec(),
            });
        }
    }
    for (cpu, list) in [(Cpu::Arm9, &rom.overlays9), (Cpu::Arm7, &rom.overlays7)] {
        for o in list {
            out.push(Unit {
                name: format!("overlays{}_{}.bin", &cpu.name()[3..], o.id),
                kind: "overlay",
                cpu,
                addr: o.ram,
                bss: o.bss,
                id: Some(o.id),
                compressed: o.compressed(),
                bytes: rom.overlay_data(o)?,
            });
        }
    }
    Ok(out)
}

fn static_json(unit: &StaticUnit<'_>, dir: &Path) -> Value {
    let n = unit.cpu.name();
    let file = |name: String| dir.join(name).display().to_string();
    let mut v = json!({
        "cpu": n,
        "base": unit.base,
        "entry": unit.entry,
        "rom_size": unit.raw.len(),
        "raw_file": file(format!("{n}_raw.bin")),
        "module_params": unit.params.map(|p| unit.base + p.offset as u32),
        "file": file(format!("{n}_static.bin")),
        "size": unit.image.len(),
    });
    if let Some(p) = unit.params {
        v["compressed"] = json!(unit.compressed);
        v["autoload_src"] = json!(p.autoload_data);
        v["bss"] = json!([p.bss_start, p.bss_end]);
        v["autoload"] = Value::Array(
            unit.autoloads
                .iter()
                .map(|a| {
                    json!({
                        "addr": a.addr,
                        "size": a.size,
                        "bss": a.bss,
                        "file": file(format!("{n}_auto_{:08x}.bin", a.addr)),
                    })
                })
                .collect(),
        );
    }
    v
}

/// Write every unit of `rom` into `dir`, with `units.json`, and return
/// the number of units.
///
/// The names and keys are those of the spike loader: `arm9_raw.bin` (the
/// static as stored), `arm9_static.bin`, `arm9_auto_<addr>.bin`,
/// `overlays9_<id>.bin` and the same for ARM7.
///
/// # Errors
///
/// Any unit does not decode or a file cannot be written.
pub fn extract(rom: &Rom<'_>, dir: &Path) -> Result<usize, BoxError> {
    std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    let write = |p: PathBuf, bytes: &[u8]| -> Result<(), BoxError> {
        relift_cache::write_if_changed(&p, bytes).map_err(|e| format!("{}: {e}", p.display()))?;
        Ok(())
    };
    let units = collect(rom)?;
    for u in &units {
        write(dir.join(&u.name), &u.bytes)?;
    }
    let mut res = json!({ "title": rom.header.title });
    for cpu in [Cpu::Arm9, Cpu::Arm7] {
        let unit = match cpu {
            Cpu::Arm9 => rom.arm9()?,
            Cpu::Arm7 => rom.arm7()?,
        };
        write(dir.join(format!("{}_raw.bin", cpu.name())), unit.raw)?;
        res[cpu.name()] = static_json(&unit, dir);
    }
    for (key, list) in [("overlays9", &rom.overlays9), ("overlays7", &rom.overlays7)] {
        let mut arr = Vec::new();
        for o in list {
            let body = rom.overlay_data(o)?;
            let file = rom.file_bytes(o.file_id)?;
            let name = format!("{key}_{}.bin", o.id);
            let ptrs = o.static_init_pointers(&body);
            arr.push(json!({
                "id": o.id,
                "ram": o.ram,
                "size": o.size,
                "bss": o.bss,
                "sinit": [o.static_init_start, o.static_init_end],
                "sinit_ptrs": ptrs,
                "file_id": o.file_id,
                "compressed": o.compressed(),
                "rom_size": file.len(),
                "file": dir.join(name).display().to_string(),
            }));
        }
        res[key] = Value::Array(arr);
    }
    let mut text = serde_json::to_string_pretty(&res)?;
    text.push('\n');
    write(dir.join("units.json"), text.as_bytes())?;
    Ok(units.len())
}
