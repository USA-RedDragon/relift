//! Embeds the shader translator stamp: a hash of every source that decides
//! what a program translates to (the `gpus` crates, and the translator
//! options and table layout in `src/shaders.rs`). `scripts/shader-stamp.sh`
//! computes the same value for a source tree, and a test keeps the two equal.

use std::path::{Path, PathBuf};

use sha2::{Digest, Sha256};

/// Every file below `dir`, by path.
fn walk(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(rd) = std::fs::read_dir(dir) else {
        return;
    };
    for e in rd.flatten() {
        let p = e.path();
        if p.is_dir() {
            walk(&p, out);
        } else {
            out.push(p);
        }
    }
}

fn main() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let mut files: Vec<PathBuf> = Vec::new();
    for c in std::fs::read_dir(root.join("gpus"))
        .expect("the gpus folder")
        .flatten()
    {
        walk(&c.path().join("src"), &mut files);
        files.push(c.path().join("Cargo.toml"));
        println!("cargo:rerun-if-changed={}", c.path().join("src").display());
        println!(
            "cargo:rerun-if-changed={}",
            c.path().join("Cargo.toml").display()
        );
    }
    files.push(root.join("relift/src/shaders.rs"));
    walk(&root.join("relift/src/shaders"), &mut files);
    println!("cargo:rerun-if-changed=src/shaders.rs");
    println!("cargo:rerun-if-changed=src/shaders");
    let mut named: Vec<(String, PathBuf)> = files
        .into_iter()
        .filter(|f| f.is_file())
        .map(|f| {
            let rel = f.strip_prefix(&root).expect("under the root");
            let rel = rel
                .components()
                .map(|c| c.as_os_str().to_string_lossy().into_owned())
                .collect::<Vec<_>>()
                .join("/");
            (rel, f)
        })
        .collect();
    named.sort();
    let mut h = Sha256::new();
    for (rel, f) in &named {
        h.update(rel.as_bytes());
        h.update(b"\n");
        h.update(std::fs::read(f).expect("a translator source"));
    }
    let digest = h.finalize();
    let stamp: String = digest[..8].iter().map(|b| format!("{b:02x}")).collect();
    println!("cargo:rustc-env=RELIFT_SHADER_STAMP={stamp}");
}
