//! SM53 (Switch 1) scans over crafted files: programs found by their NVN
//! code header in raw files, Yaz0 streams, SARC members, zstd frames and
//! UnityFS bundles, stored as SPH and code.

use std::collections::BTreeSet;
use std::path::{Path, PathBuf};

use relift_switch_shaders::carve::Kind;
use relift_switch_shaders::{Arch, DirSource, Options, maxwell, sarc, scan, unity, yaz0};

const EXIT: u64 = 0xe300_0000_0007_000f;
const NOP: u64 = 0x50b0_0000_0007_0f00;
const MOV32I: u64 = 0x0100_0000_0007_0000;

/// A program: code header, SPH (stage 1 to 5, or none for 0), then
/// `bundles` bundles of MOV32I with `seed` as the immediate, the last
/// ending in EXIT.
fn program(stage: u32, seed: u32, bundles: usize) -> Vec<u8> {
    let mut b = 0x1234_5678u32.to_le_bytes().to_vec();
    b.resize(0x30, 0);
    if stage != 0 {
        let kind = if stage == 5 { 2 } else { 1 };
        b.extend((kind | 3 << 5 | stage << 10).to_le_bytes());
        b.resize(0x30 + 0x50, 0);
    }
    for k in 0..bundles {
        b.extend(0u64.to_le_bytes());
        let mov = MOV32I | (u64::from(seed) << 20);
        let last = if k + 1 == bundles { EXIT } else { mov };
        for w in [mov, NOP, last] {
            b.extend(w.to_le_bytes());
        }
    }
    b
}

/// The bytes a scan stores for `p`: everything after the code header.
fn stored(p: &[u8]) -> Vec<u8> {
    p[0x30..].to_vec()
}

fn sha(b: &[u8]) -> String {
    relift_switch_formats::hex(&relift_switch_formats::sha256(b))
}

/// A Yaz0 stream of `b`: literals only, then one back reference that
/// repeats the last `len` bytes once (when `b` is long enough).
fn yaz0_of(b: &[u8]) -> Vec<u8> {
    let mut out = b"Yaz0".to_vec();
    out.extend((b.len() as u32).to_be_bytes());
    out.resize(16, 0);
    for chunk in b.chunks(8) {
        out.push(0xffu8 << (8 - chunk.len()));
        out.extend(chunk);
    }
    out
}

/// A UnityFS bundle (format 6, block table after the header) of one LZ4
/// block holding `data`.
fn unityfs(data: &[u8]) -> Vec<u8> {
    let block = lz4_flex::block::compress(data);
    let mut table = vec![0u8; 16];
    table.extend(1u32.to_be_bytes());
    table.extend((data.len() as u32).to_be_bytes());
    table.extend((block.len() as u32).to_be_bytes());
    table.extend(2u16.to_be_bytes());
    let mut b = b"UnityFS\0".to_vec();
    b.extend(6u32.to_be_bytes());
    b.extend(b"5.x.x\0");
    b.extend(b"2019.4\0");
    b.extend(0u64.to_be_bytes());
    b.extend((table.len() as u32).to_be_bytes());
    b.extend((table.len() as u32).to_be_bytes());
    b.extend(0u32.to_be_bytes());
    b.extend(&table);
    b.extend(block);
    b
}

fn zstd(b: &[u8]) -> Vec<u8> {
    zstd::bulk::compress(b, 3).expect("compresses")
}

struct Scratch(PathBuf);

impl Drop for Scratch {
    fn drop(&mut self) {
        if !std::thread::panicking() {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }
}

fn scratch(name: &str) -> Scratch {
    let dir = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(format!("maxwell-{name}"));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).expect("the scratch dir is creatable");
    Scratch(dir)
}

fn put(root: &Path, files: &[(&str, Vec<u8>)]) {
    for (p, b) in files {
        let path = root.join(p);
        std::fs::create_dir_all(path.parent().expect("a parent")).expect("a dir");
        relift_cache::write_if_changed(&path, b).expect("the fixture is writable");
    }
}

#[test]
fn yaz0_decodes_literals_and_back_references() {
    let mut s = b"Yaz0".to_vec();
    s.extend(10u32.to_be_bytes());
    s.resize(16, 0);
    s.push(0b1110_0000);
    s.extend(b"abc");
    s.extend([0x50, 0x02]);
    assert_eq!(yaz0::decode(&s).expect("decodes"), b"abcabcabca");
    let mut long = b"Yaz0".to_vec();
    long.extend(21u32.to_be_bytes());
    long.resize(16, 0);
    long.push(0b1000_0000);
    long.push(b'z');
    long.extend([0x00, 0x00, 0x02]);
    assert_eq!(yaz0::decode(&long).expect("decodes"), vec![b'z'; 21]);
    assert!(yaz0::decode(&s[..s.len() - 1]).is_err());
    let mut before = b"Yaz0".to_vec();
    before.extend(4u32.to_be_bytes());
    before.resize(16, 0);
    before.extend([0x00, 0x20, 0x05]);
    assert!(yaz0::decode(&before).is_err());
}

#[test]
fn unityfs_joins_its_blocks() {
    let data: Vec<u8> = (0..5000u32).map(|i| (i % 7) as u8).collect();
    assert_eq!(unity::data(&unityfs(&data)).expect("reads"), data);
    let mut lzma = unityfs(&data);
    let at = lzma.len() - lz4_flex::block::compress(&data).len() - 1;
    lzma[at] = 1;
    assert!(
        unity::data(&lzma)
            .expect_err("LZMA is not read")
            .to_string()
            .contains("LZMA")
    );
}

#[test]
fn carve_takes_sph_and_reachable_code() {
    let vs = program(1, 1, 2);
    let cs = program(0, 2, 1);
    let mut b = vec![0xaa; 40];
    b.extend(&vs);
    b.extend([0xbb; 24]);
    b.extend(&cs);
    let (found, errors) = maxwell::carve(&b);
    assert!(errors.is_empty(), "{errors:?}");
    assert_eq!(found.len(), 2);
    assert_eq!(
        (found[0].at, found[0].kind, found[0].stage, found[0].sph),
        (40, Kind::Graphics, 1, 0x50)
    );
    assert_eq!(found[0].bytes, stored(&vs));
    assert_eq!(
        (found[1].kind, found[1].stage, found[1].sph),
        (Kind::Compute, 0, 0)
    );
    assert_eq!(found[1].bytes, stored(&cs));
}

#[test]
fn a_header_that_starts_no_program_is_reported() {
    let mut b = 0x1234_5678u32.to_le_bytes().to_vec();
    b.resize(0x30 + 0x50 + 32, 0);
    let (found, errors) = maxwell::carve(&b);
    assert!(found.is_empty());
    assert_eq!(errors.len(), 1);
}

#[test]
fn an_sm53_scan_finds_programs_in_every_container() {
    let root = scratch("containers");
    let (p1, p2, p3, p4, p5, p6) = (
        program(1, 1, 1),
        program(5, 2, 3),
        program(1, 3, 2),
        program(5, 4, 1),
        program(4, 5, 1),
        program(0, 6, 2),
    );
    let p7 = program(1, 7, 4);
    let mut raw = vec![0x11; 100];
    raw.extend(&p1);
    let mut bea = b"SCNE".to_vec();
    bea.resize(0x40, 0);
    bea.extend(zstd(&p7));
    bea.extend([0x28, 0xb5, 0x2f, 0xfd, 0xff, 0xff]);
    let archive = sarc::build(&[("a/b.bin", &yaz0_of(&p3)), ("a/c.txt", b"hello")]);
    put(
        &root.0.join("tree"),
        &[
            ("romfs/Shader/raw.dat", raw),
            ("romfs/Model/m.szs", yaz0_of(&[&p2[..], &p6[..]].concat())),
            ("romfs/Pack/x.pack", yaz0_of(&archive)),
            ("romfs/Data/z.zs", zstd(&p4)),
            ("romfs/Pack/e.bea", bea),
            (
                "romfs/Data/bundle",
                unityfs(&[vec![0; 33], p5.clone()].concat()),
            ),
        ],
    );
    let opts = Options {
        arch: Arch::Sm53,
        jobs: 2,
        cache: Some(root.0.join("cache")),
        package: None,
    };
    let r = scan(
        &DirSource::new(&root.0.join("tree")),
        &root.0.join("out"),
        &opts,
    )
    .expect("scans");
    let want: BTreeSet<String> = [&p1, &p2, &p3, &p4, &p5, &p6, &p7]
        .iter()
        .map(|p| sha(&stored(p)))
        .collect();
    let got: BTreeSet<String> = r.programs.keys().cloned().collect();
    assert_eq!(got, want, "errors: {:?}", r.errors);
    assert!(r.errors.is_empty(), "{:?}", r.errors);
    for (k, n) in [
        ("yaz0", 3),
        ("sarc", 1),
        ("zstd", 1),
        ("unityfs", 1),
        ("zstd-embedded", 1),
        ("zstd-embedded-undecodable", 1),
    ] {
        assert_eq!(r.containers.get(k), Some(&n), "{k}: {:?}", r.containers);
    }
    for p in r.programs.values() {
        let bytes = std::fs::read(root.0.join("out").join(format!(
            "{}.bin",
            r.programs.iter().find(|(_, q)| *q == p).expect("listed").0
        )))
        .expect("written");
        assert_eq!(bytes.len(), p.size);
    }
    let again = scan(
        &DirSource::new(&root.0.join("tree")),
        &root.0.join("out"),
        &opts,
    )
    .expect("scans");
    assert_eq!(again.stats.hits, again.stats.jobs);
    let sm86 = scan(
        &DirSource::new(&root.0.join("tree")),
        &root.0.join("out86"),
        &Options {
            arch: Arch::Sm86,
            ..opts
        },
    )
    .expect("scans");
    assert!(
        sm86.programs.is_empty(),
        "the SM86 scan takes no SM53 program"
    );
}
