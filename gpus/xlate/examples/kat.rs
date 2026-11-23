//! What the C test of the library compares with: `kat <out>` writes the
//! SM86 KAT programs (`gpus/sm86/tests/kat/compute.txt`) as a shader folder
//! in `<out>/programs`, and for each captured device of
//! `gpus/sm86/tests/caps` the tables `relift_gpu_table::translate` makes in
//! `<out>/<device>/rust` and each program's module from
//! `Translator::one` in `<out>/<device>/one/<key>.spv`.

use std::path::Path;

use relift_gpu_table::{BoxError, One, Programs, Translator, hex, sha256, translate};

fn unhex(s: &str) -> Result<Vec<u8>, BoxError> {
    (0..s.len())
        .step_by(2)
        .map(|i| {
            s.get(i..i + 2)
                .and_then(|b| u8::from_str_radix(b, 16).ok())
                .ok_or_else(|| BoxError::from(format!("not hex: {s}")))
        })
        .collect()
}

fn main() -> Result<(), BoxError> {
    let out = std::env::args().nth(1).ok_or("usage: kat <out>")?;
    let out = Path::new(&out);
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
    let set = out.join("programs");
    std::fs::create_dir_all(set.join("programs"))?;
    let text = std::fs::read_to_string(root.join("gpus/sm86/tests/kat/compute.txt"))?;
    let mut n = 0;
    for line in text.lines() {
        if let Some(h) = line.strip_prefix("program ") {
            let code = unhex(h.trim())?;
            let name = format!("{}.bin", hex(&sha256(&code)));
            relift_cache::write_if_changed(&set.join("programs").join(name), &code)?;
            n += 1;
        }
    }
    let programs = Programs::dir(&set)?;
    let mut caps: Vec<_> = std::fs::read_dir(root.join("gpus/sm86/tests/caps"))?
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.extension().is_some_and(|x| x == "json"))
        .collect();
    caps.sort();
    for c in &caps {
        let name = c
            .file_stem()
            .and_then(|s| s.to_str())
            .ok_or("a caps name")?;
        let dir = out.join(name);
        let device = std::fs::read(c)?;
        std::fs::create_dir_all(dir.join("one"))?;
        relift_cache::write_if_changed(&dir.join("device.json"), &device)?;
        translate(&programs, Some(&device), &dir.join("rust"), 2, &|_, _| true)?;
        let tr = Translator::new(&programs, &device)?;
        for k in programs.keys() {
            let code = programs.code(k)?.ok_or("a program")?;
            if let One::Done { words, .. } = tr.one(*k, code) {
                let b: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
                relift_cache::write_if_changed(
                    &dir.join("one").join(format!("{}.spv", hex(k))),
                    &b,
                )?;
            }
        }
    }
    println!("{n} programs, {} devices", caps.len());
    Ok(())
}
