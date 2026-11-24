//! Prints the NVN driver's proc table of an SDK module (name and image
//! offset), for reading a driver proc's code:
//! `cargo run -p relift-switch --example nvnprocs -- <sdk.nso> [substring]`.

use relift_cpu_arm64::frontend::Arm64;
use relift_switch::cpu::{Console, cpu};
use relift_switch::nvndriver::{Image, proc_table};
use relift_switch_formats::nso::Nso;

fn main() -> Result<(), String> {
    let mut args = std::env::args().skip(1);
    let path = args.next().ok_or("usage: nvnprocs <sdk.nso> [substring]")?;
    let filter = args.next().unwrap_or_default();
    let bytes = std::fs::read(&path).map_err(|e| format!("{path}: {e}"))?;
    let nso = Nso::parse(&bytes).map_err(|e| e.to_string())?;
    let cpu = cpu(None, Some(Console::Switch2)).map_err(|e| e.to_string())?;
    let image = Image::new(&nso, Arm64 { cpu })?;
    for (name, at) in proc_table(&image)? {
        if name.contains(&filter) {
            println!("{name}\t{at:#x}");
        }
    }
    Ok(())
}
