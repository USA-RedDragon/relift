//! Synthetic fixtures: small NSOs, a game with a base, two updates and a
//! DLC, written out as the extracted dump layout or packed as a NUP. Tests
//! across the workspace build every input they read from here; no console
//! data is involved.

use std::path::Path;

use crate::nup::{self, BuildPackage, PackageKind};

fn pad(v: &mut Vec<u8>, to: usize) {
    v.resize(v.len().div_ceil(to) * to, 0);
}

/// What one package of a dump holds: its `exefs/` and `romfs/` files.
#[derive(Clone, Debug)]
pub struct ProgramSpec {
    /// The program ID its `main.npdm` carries.
    pub program_id: u64,
    /// The exefs files, by name.
    pub exefs: Vec<(String, Vec<u8>)>,
    /// The romfs files, by `/`-separated path.
    pub romfs: Vec<(String, Vec<u8>)>,
}

/// One add-on content package.
#[derive(Clone, Debug)]
pub struct Dlc {
    /// The add-on title ID.
    pub title_id: u64,
    /// The index directory name, opaque as in the dump layout.
    pub index: String,
    /// The romfs files.
    pub romfs: Vec<(String, Vec<u8>)>,
}

/// A synthetic game: a base, updates in ascending version order and DLC.
#[derive(Clone, Debug)]
pub struct Game {
    /// The base program.
    pub base_spec: ProgramSpec,
    /// The updates as (decimal version, full program), oldest first. Each
    /// romfs is the full patched tree, not a delta.
    pub updates: Vec<(u32, ProgramSpec)>,
    /// The add-on content.
    pub dlc: Vec<Dlc>,
}

fn put(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    relift_cache::write_if_changed(path, bytes).map(|_| ())
}

fn put_files(dir: &Path, files: &[(String, Vec<u8>)]) -> std::io::Result<()> {
    std::fs::create_dir_all(dir)?;
    for (name, bytes) in files {
        put(&dir.join(name), bytes)?;
    }
    Ok(())
}

/// A package to build: title ID, version, kind and (path, content) files.
type Owned<'a> = (u64, u32, PackageKind, Vec<(String, &'a [u8])>);

impl Game {
    /// Write the extracted dump layout under `dir`: `base/{exefs,romfs}`,
    /// `updates/<version>/{exefs,romfs}` and `dlc/<title id>/<index>/romfs`.
    ///
    /// # Errors
    ///
    /// A directory or file cannot be written.
    pub fn write_tree(&self, dir: &Path) -> std::io::Result<()> {
        let program = |at: &Path, p: &ProgramSpec| -> std::io::Result<()> {
            put_files(&at.join("exefs"), &p.exefs)?;
            put_files(&at.join("romfs"), &p.romfs)
        };
        program(&dir.join("base"), &self.base_spec)?;
        for (v, p) in &self.updates {
            program(&dir.join("updates").join(v.to_string()), p)?;
        }
        for d in &self.dlc {
            let at = dir
                .join("dlc")
                .join(format!("{:016x}", d.title_id))
                .join(&d.index);
            put_files(&at.join("romfs"), &d.romfs)?;
        }
        Ok(())
    }

    /// The same game packed as a NUP.
    pub fn nup(&self) -> Vec<u8> {
        fn files(p: &ProgramSpec) -> Vec<(String, &[u8])> {
            let ex = p.exefs.iter().map(|(n, b)| (format!("exefs/{n}"), &b[..]));
            let ro = p.romfs.iter().map(|(n, b)| (format!("romfs/{n}"), &b[..]));
            ex.chain(ro).collect()
        }
        let mut owned: Vec<Owned<'_>> = vec![(
            self.base_spec.program_id,
            0,
            PackageKind::Base,
            files(&self.base_spec),
        )];
        for (v, p) in &self.updates {
            owned.push((p.program_id | 0x800, *v, PackageKind::Update, files(p)));
        }
        for d in &self.dlc {
            let ro = d
                .romfs
                .iter()
                .map(|(n, b)| (format!("romfs/{n}"), &b[..]))
                .collect();
            owned.push((d.title_id, 0, PackageKind::Addon, ro));
        }
        let packages: Vec<BuildPackage<'_>> = owned
            .iter()
            .map(|(title_id, version, kind, files)| BuildPackage {
                title_id: *title_id,
                version: *version,
                kind: *kind,
                standalone: false,
                files: files.iter().map(|(n, b)| (n.as_str(), *b)).collect(),
                empty_dirs: vec![],
                meta: vec![],
            })
            .collect();
        nup::build(&packages)
    }
}

/// The program ID of [`game`].
pub const PROGRAM_ID: u64 = 0x0100_1234_5678_0000;

fn npdm() -> Vec<u8> {
    crate::npdm::build(
        "Application",
        PROGRAM_ID,
        0x10_0000,
        &[("fsp-srv", false), ("sm:", false)],
    )
}

/// A base game and two updates: version 65536 changes `main` and edits a
/// romfs file, version 131072 changes `main` again and adds a file. One DLC
/// holds a single romfs file.
pub fn game() -> Game {
    let big: Vec<u8> = (0..0x5000u32).map(|i| (i * 7 % 251) as u8).collect();
    let main_path = "D:\\work\\game\\out\\game-main.nss";
    let base_spec = ProgramSpec {
        program_id: PROGRAM_ID,
        exefs: vec![
            ("main".into(), nso(main_path, 0xA1)),
            ("main.npdm".into(), npdm()),
            ("sdk".into(), nso("nnSdk", 0xA2)),
        ],
        romfs: vec![
            ("data/big.bin".into(), big.clone()),
            ("data/keep.txt".into(), b"kept".to_vec()),
            ("readme.txt".into(), b"version 0".to_vec()),
        ],
    };
    let mut big1 = big;
    big1[0x2100] ^= 0xFF;
    let mut v1 = base_spec.clone();
    v1.exefs[0].1 = nso(main_path, 0xB1);
    v1.romfs = vec![
        ("data/big.bin".into(), big1.clone()),
        ("data/keep.txt".into(), b"kept".to_vec()),
        ("readme.txt".into(), b"version 1".to_vec()),
    ];
    let mut v2 = v1.clone();
    v2.exefs[0].1 = nso(main_path, 0xC1);
    v2.romfs = vec![
        ("data/big.bin".into(), big1),
        ("data/keep.txt".into(), b"kept".to_vec()),
        ("data/new/added.bin".into(), vec![9u8; 0x1234]),
        ("readme.txt".into(), b"version 2".to_vec()),
    ];
    Game {
        base_spec,
        updates: vec![(0x10000, v1), (0x20000, v2)],
        dlc: vec![Dlc {
            title_id: PROGRAM_ID + 0x1001,
            index: "0".into(),
            romfs: vec![("dlc.txt".into(), b"add-on".to_vec())],
        }],
    }
}

/// A small NSO with the given module path in .rodata, two dynamic symbols
/// and a MOD0, compressed with LZ4.
pub fn nso(module_path: &str, build_id: u8) -> Vec<u8> {
    let mut text = vec![0u8; 0x200];
    text[4..8].copy_from_slice(&0x10u32.to_le_bytes());
    text[0x10..0x14].copy_from_slice(b"MOD0");
    let mut rodata = Vec::new();
    rodata.extend_from_slice(&0u32.to_le_bytes());
    rodata.extend_from_slice(&u32::try_from(module_path.len()).unwrap_or(0).to_le_bytes());
    rodata.extend_from_slice(module_path.as_bytes());
    pad(&mut rodata, 0x10);
    let dynstr_at = rodata.len();
    let dynstr = b"\0nnMain\0nnImported\0";
    rodata.extend_from_slice(dynstr);
    pad(&mut rodata, 0x10);
    let dynsym_at = rodata.len();
    let mut sym = vec![0u8; 24 * 3];
    sym[24..28].copy_from_slice(&1u32.to_le_bytes());
    sym[28] = 0x12;
    sym[30..32].copy_from_slice(&1u16.to_le_bytes());
    sym[32..40].copy_from_slice(&0x100u64.to_le_bytes());
    sym[40..48].copy_from_slice(&8u64.to_le_bytes());
    sym[48..52].copy_from_slice(&8u32.to_le_bytes());
    sym[52] = 0x12;
    rodata.extend_from_slice(&sym);
    rodata.extend_from_slice(&[0x5a; 0x300]);
    let data = vec![0x11u8; 0x180];
    let text_pages = u32::try_from(text.len().div_ceil(0x1000) * 0x1000).unwrap_or(0);
    let ro_pages = u32::try_from(rodata.len().div_ceil(0x1000) * 0x1000).unwrap_or(0);
    let data_at = i64::from(text_pages + ro_pages);
    let dyn_rel = i32::try_from(data_at - 0x10).unwrap_or(0);
    text[0x14..0x18].copy_from_slice(&dyn_rel.to_le_bytes());
    let bss_rel = i32::try_from(data_at + 0x180 - 0x10).unwrap_or(0);
    text[0x18..0x1C].copy_from_slice(&bss_rel.to_le_bytes());
    text[0x1C..0x20].copy_from_slice(&(bss_rel + 0x40).to_le_bytes());
    crate::nso::build(&crate::nso::NsoSpec {
        text,
        rodata,
        data,
        bss: 0x40,
        build_id: [build_id; 32],
        dyn_extents: [
            (
                u32::try_from(dynstr_at).unwrap_or(0),
                u32::try_from(dynstr.len()).unwrap_or(0),
            ),
            (u32::try_from(dynsym_at).unwrap_or(0), 24 * 3),
        ],
        compress: true,
    })
}
