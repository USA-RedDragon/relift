//! The scan over synthetic games: hand-built SARC, zstd (with and without
//! a dictionary), BNSH-style blobs, sead `.bin`, KTGL indexes with zlib
//! chunks, an NSO and a NUP. No game data is involved.

use std::collections::BTreeSet;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use relift_switch_formats::nso::{self, NsoSpec};
use relift_switch_formats::nup::{self, BuildPackage, PackageKind};
use relift_switch_formats::storage::MemStorage;
use relift_switch_shaders::carve::{BRA_SELF, Kind, carve};
use relift_switch_shaders::ktgl::{self, Location};
use relift_switch_shaders::{DirSource, NupSource, Options, Report, Source, sarc, scan};

const DICT: &[u8] = &[
    0x37, 0xa4, 0x30, 0xec, 0xd8, 0xd8, 0x3f, 0x07, 0x17, 0x10, 0xe8, 0xa9, 0xd0, 0x01, 0x00, 0x00,
    0xc0, 0x89, 0x89, 0x71, 0x80, 0x18, 0x6b, 0x97, 0x96, 0x03, 0x34, 0x65, 0x23, 0x03, 0x80, 0x14,
    0xc3, 0x00, 0x58, 0x58, 0x50, 0x44, 0x36, 0xa5, 0x0d, 0x00, 0x00, 0x00, 0x04, 0x20, 0x20, 0x14,
    0x30, 0x82, 0x39, 0x05, 0x01, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x84, 0xf3, 0x0a, 0xe5, 0xa1, 0x53,
    0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x60, 0x00, 0x19, 0x79, 0x20, 0x00, 0x88,
    0x99, 0x20, 0x00, 0x88, 0x99, 0x40, 0x00, 0x19, 0x79, 0x60, 0x00, 0x18, 0x79, 0x40, 0x00, 0x18,
    0x79, 0x40, 0x00, 0x70, 0x47, 0x60, 0x00, 0x19, 0x79, 0x50, 0x00, 0x19, 0x79, 0x60, 0x00, 0x34,
    0x12, 0x70, 0x00, 0x70, 0x47, 0x50, 0x00, 0x88, 0x99, 0x60, 0x00, 0x30, 0x00, 0x88, 0x99, 0x20,
    0x00, 0x34, 0x12, 0x50, 0x00, 0x18, 0x79, 0x70, 0x00, 0x18, 0x79, 0x30, 0x00, 0x34, 0x12, 0x50,
    0x00, 0x70, 0x47, 0x50, 0x00, 0x70, 0x47, 0x70, 0x00, 0x34, 0x12, 0x50, 0x00, 0x19, 0x79, 0x70,
    0x00, 0x18, 0x79, 0x50, 0x00, 0x19, 0x79, 0x50, 0x00, 0x70, 0x47, 0x10, 0x00, 0x70, 0x00, 0x88,
    0x99, 0x00, 0x00, 0x70, 0x47, 0x20, 0x00, 0x34, 0x12, 0x10, 0x00, 0x18, 0x79, 0x60, 0x00, 0x19,
    0x79, 0x70, 0x00, 0x88, 0x99, 0x70, 0x00, 0x34, 0x12, 0x00, 0x00, 0x88, 0x99, 0x10, 0x00, 0x34,
    0x12, 0x70, 0x00, 0x19, 0x79, 0x30, 0x00, 0x70, 0x47, 0x10, 0x00, 0x70, 0x47, 0x00, 0x00, 0x40,
];

const DICT_ID: u32 = 0x073f_d8d8;

fn terminator() -> Vec<u8> {
    let mut t = BRA_SELF.to_vec();
    t.extend_from_slice(&[0x00, 0x00, 0xc0, 0x0f]);
    t
}

fn code(seed: u8, lines: usize) -> Vec<u8> {
    let mut v = Vec::new();
    for i in 0..lines {
        let mut l = vec![0x24, 0x78, seed, i as u8, 0x11, 0x22, 0x33, 0x44];
        l.extend_from_slice(&[seed ^ 0x5a, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07]);
        v.extend_from_slice(&l);
    }
    v.extend(terminator());
    v
}

fn graphics(stage: u32, seed: u8) -> Vec<u8> {
    let mut sph = vec![0u8; 0x80];
    let w0 = 1 | (4 << 5) | (stage << 10);
    sph[..4].copy_from_slice(&w0.to_le_bytes());
    sph[0x10..0x14].copy_from_slice(&[seed, 0x77, 0x66, 0x55]);
    sph.extend(code(seed, 3 + usize::from(seed % 4)));
    sph
}

fn compute(seed: u8) -> Vec<u8> {
    code(seed, 5)
}

fn blob(parts: &[&[u8]]) -> Vec<u8> {
    let mut v = vec![0u8; 0x100];
    for p in parts {
        v.extend_from_slice(p);
        v.extend(vec![0u8; 0x100]);
    }
    v
}

fn zstd(b: &[u8]) -> Vec<u8> {
    zstd::bulk::compress(b, 3).expect("zstd compresses")
}

fn zstd_dict(b: &[u8]) -> Vec<u8> {
    zstd::bulk::Compressor::with_dictionary(3, DICT)
        .expect("the dictionary loads")
        .compress(b)
        .expect("zstd compresses")
}

fn sha(b: &[u8]) -> String {
    relift_switch_formats::hex(&relift_switch_formats::sha256(b))
}

fn shas(parts: &[&[u8]]) -> BTreeSet<String> {
    parts.iter().map(|p| sha(p)).collect()
}

fn found(r: &Report) -> BTreeSet<String> {
    r.programs.keys().cloned().collect()
}

fn adler32(b: &[u8]) -> u32 {
    let (mut a, mut s) = (1u32, 0u32);
    for &x in b {
        a = (a + u32::from(x)) % 65521;
        s = (s + a) % 65521;
    }
    (s << 16) | a
}

fn zlib_stored(b: &[u8]) -> Vec<u8> {
    let mut v = vec![0x78, 0x01];
    let n = u16::try_from(b.len()).expect("a small chunk");
    v.push(1);
    v.extend_from_slice(&n.to_le_bytes());
    v.extend_from_slice(&(!n).to_le_bytes());
    v.extend_from_slice(b);
    v.extend_from_slice(&adler32(b).to_be_bytes());
    v
}

fn scratch(name: &str) -> PathBuf {
    let dir = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(format!("shaders-{name}"));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).expect("the scratch dir is creatable");
    dir
}

fn put(root: &Path, files: &[(String, Vec<u8>)]) {
    for (p, b) in files {
        let path = root.join(p);
        std::fs::create_dir_all(path.parent().expect("a parent")).expect("a dir");
        relift_cache::write_if_changed(&path, b).expect("the fixture is writable");
    }
}

fn nso_with(text: &[u8]) -> Vec<u8> {
    nso::build(&NsoSpec {
        text: text.to_vec(),
        rodata: vec![0x5a; 0x40],
        data: vec![0x11; 0x40],
        bss: 0x40,
        build_id: [7; 32],
        dyn_extents: [(0, 0), (0, 0)],
        compress: true,
    })
}

struct Game {
    files: Vec<(String, Vec<u8>)>,
    expected: BTreeSet<String>,
}

fn game() -> Game {
    let (g1, g5, g2, g4, g3, g1b) = (
        graphics(1, 1),
        graphics(5, 2),
        graphics(2, 3),
        graphics(4, 4),
        graphics(3, 6),
        graphics(1, 8),
    );
    let (c1, c2) = (compute(9), compute(10));
    let mut fsha = b"FSHA    ".to_vec();
    fsha.extend(blob(&[&g1, &g5]));
    let inner = zstd(&blob(&[&g2]));
    let agl = sarc::build(&[
        ("a/x.bnsh", &blob(&[&c1])),
        ("a/y.zs", &inner),
        ("a/readme.txt", b"hello"),
    ]);
    let mut effect = b"BNSH".to_vec();
    effect.extend(blob(&[&g1]));
    let mut sead = vec![0u8; 0x10];
    sead.extend_from_slice(&[0x34, 0x12, 0x76, 0x98]);
    sead.extend(blob(&[&g4]));
    let junk = blob(&[&graphics(1, 99)]);
    let files = vec![
        ("exefs/main".to_string(), nso_with(&blob(&[&g3]))),
        ("exefs/main.npdm".to_string(), junk.clone()),
        ("romfs/Shader/a.bfsha.zs".to_string(), zstd(&fsha)),
        ("romfs/Lib/agl.sarc".to_string(), agl),
        ("romfs/Effect/e.byml.zs".to_string(), zstd(&effect)),
        ("romfs/Lib/f.bin".to_string(), sead),
        ("romfs/Other/junk.bin".to_string(), junk.clone()),
        ("romfs/Tex/t.bntx.zs".to_string(), zstd(&junk)),
        (
            "romfs/Shader/b.ptcl.zst".to_string(),
            zstd(&blob(&[&g1b, &c2])),
        ),
    ];
    Game {
        files,
        expected: shas(&[&g1, &g5, &g2, &g4, &g3, &g1b, &c1, &c2]),
    }
}

fn opts(cache: &Path) -> Options {
    Options {
        jobs: 2,
        cache: Some(cache.to_path_buf()),
        package: None,
    }
}

#[test]
fn carve_kinds_stages_and_exact_bytes() {
    let (g, c, p) = (graphics(1, 1), compute(2), graphics(5, 3));
    let b = blob(&[&g, &c, &p]);
    let f = carve(&b);
    assert_eq!(f.len(), 3);
    assert_eq!(
        f.iter().map(|x| x.kind).collect::<Vec<_>>(),
        [Kind::Graphics, Kind::Compute, Kind::Graphics]
    );
    assert_eq!(f.iter().map(|x| x.stage(&b)).collect::<Vec<_>>(), [1, 0, 5]);
    for (x, want) in f.iter().zip([&g, &c, &p]) {
        assert_eq!(&b[x.start..x.end], &want[..]);
    }
    assert_eq!(f[0].code - f[0].start, 0x80);
}

fn nops(lines: usize) -> Vec<u8> {
    let mut l = vec![0x18, 0x79];
    l.resize(16, 0);
    l.repeat(lines)
}

#[test]
fn carve_walks_over_nops_inside_a_program() {
    let mut sph = graphics(2, 1);
    sph.truncate(0x80);
    let mut g = sph.clone();
    g.extend(code(1, 2).split_at(32).0);
    g.extend(nops(3));
    g.extend(code(1, 3));
    let mut c = code(3, 2).split_at(32).0.to_vec();
    c.extend(nops(2));
    c.extend(code(3, 2));
    let b = blob(&[&g, &c]);
    let f = carve(&b);
    assert_eq!(f.len(), 2);
    assert_eq!(f[0].kind, Kind::Graphics);
    assert_eq!(&b[f[0].start..f[0].end], &g[..]);
    assert_eq!(f[1].kind, Kind::Compute);
    assert_eq!(&b[f[1].start..f[1].end], &c[..]);
}

#[test]
fn carve_stops_at_nops_after_an_sph() {
    let mut g = graphics(1, 1)[..0x80].to_vec();
    g.extend(nops(2));
    g.extend(code(1, 3));
    let b = blob(&[&g]);
    let f = carve(&b);
    assert_eq!(f[0].kind, Kind::Graphics);
    assert_eq!(f[0].start, 0x100);
}

#[test]
fn carve_ignores_a_header_that_is_not_one() {
    let mut g = graphics(1, 1);
    g[0] = 3;
    let b = blob(&[&g]);
    assert_eq!(carve(&b)[0].kind, Kind::Compute);
}

#[test]
fn sarc_round_trip() {
    let a = sarc::build(&[("one", b"1111"), ("dir/two", b"22")]);
    let m = sarc::members(&a).expect("the archive parses");
    assert_eq!(
        m,
        vec![
            ("one".to_string(), &b"1111"[..]),
            ("dir/two".to_string(), &b"22"[..])
        ]
    );
}

#[test]
fn directory_scan_finds_every_container_and_nothing_else() {
    let g = game();
    let root = scratch("dir");
    put(&root.join("tree"), &g.files);
    let out = root.join("out");
    let r = scan(
        &DirSource::new(&root.join("tree")),
        &out,
        &opts(&root.join("cache")),
    )
    .expect("the scan runs");
    assert_eq!(found(&r), g.expected);
    assert!(r.errors.is_empty(), "{:?}", r.errors);
    assert_eq!(
        r.programs
            .values()
            .filter(|p| p.kind == Kind::Compute)
            .count(),
        2
    );
    let dup = &r.programs[&sha(&graphics(1, 1))];
    assert_eq!(
        dup.n, 2,
        "the program is in a shader archive and an effect file"
    );
    assert_eq!(dup.stage, 1);
    for s in r.programs.keys() {
        assert!(out.join(format!("{s}.bin")).is_file());
    }
    for (c, n) in [
        ("zstd", 4),
        ("sarc", 1),
        ("bfsha", 1),
        ("sead-bin", 1),
        ("nso", 1),
    ] {
        assert_eq!(r.containers.get(c), Some(&n), "{c}: {:?}", r.containers);
    }
}

#[test]
fn rerun_with_no_change_reads_from_the_cache() {
    let g = game();
    let root = scratch("cache");
    let tree = root.join("tree");
    put(&tree, &g.files);
    let (out, cache) = (root.join("out"), root.join("cache"));
    let first = scan(&DirSource::new(&tree), &out, &opts(&cache)).expect("runs");
    assert_eq!(first.stats.hits, 0);
    let again = scan(&DirSource::new(&tree), &out, &opts(&cache)).expect("runs");
    assert_eq!(again.stats.hits, again.stats.jobs);
    assert_eq!(again.programs, first.programs);
    let changed = vec![(
        "romfs/Effect/e.byml.zs".to_string(),
        zstd(&blob(&[&graphics(1, 40)])),
    )];
    put(&tree, &changed);
    let third = scan(&DirSource::new(&tree), &out, &opts(&cache)).expect("runs");
    assert_eq!(third.stats.hits, third.stats.jobs - 1);
    assert!(third.programs.contains_key(&sha(&graphics(1, 40))));
    std::fs::remove_file(out.join(format!("{}.bin", sha(&compute(9))))).expect("removes");
    let fourth = scan(&DirSource::new(&tree), &out, &opts(&cache)).expect("runs");
    assert!(out.join(format!("{}.bin", sha(&compute(9)))).is_file());
    assert_eq!(fourth.stats.hits, fourth.stats.jobs - 1);
}

#[test]
fn nup_scan_matches_the_directory_scan() {
    let g = game();
    let files: Vec<(&str, &[u8])> = g
        .files
        .iter()
        .map(|(n, b)| (n.as_str(), b.as_slice()))
        .collect();
    let image = nup::build(&[BuildPackage {
        title_id: 0x0100_1234_5678_0000,
        version: 0,
        kind: PackageKind::Base,
        standalone: false,
        files,
        empty_dirs: vec![],
        meta: vec![],
    }]);
    let source = NupSource::new(Arc::new(MemStorage(image))).expect("the NUP opens");
    assert!(source.has_packages());
    let root = scratch("nup");
    let r = scan(&source, &root.join("out"), &opts(&root.join("cache"))).expect("scans");
    assert_eq!(found(&r), g.expected);
    assert!(
        r.programs
            .values()
            .all(|p| p.srcs.iter().all(|s| s.starts_with("pkg0:")))
    );
    let none = scan(
        &source,
        &root.join("out2"),
        &Options {
            package: Some(3),
            ..opts(&root.join("cache2"))
        },
    )
    .expect_err("a missing package is an error");
    assert!(none.to_string().contains("package 3"), "{none}");
}

#[test]
fn the_cache_key_covers_package_labels_and_the_package_selection() {
    let g = game();
    let root = scratch("key");
    let tree = root.join("tree");
    put(&tree, &g.files);
    let cache = root.join("cache");
    let dir = scan(&DirSource::new(&tree), &root.join("out"), &opts(&cache)).expect("scans");
    assert!(
        dir.programs
            .values()
            .all(|p| p.srcs.iter().all(|s| !s.starts_with("pkg")))
    );
    let files: Vec<(&str, &[u8])> = g
        .files
        .iter()
        .map(|(n, b)| (n.as_str(), b.as_slice()))
        .collect();
    let image = nup::build(&[BuildPackage {
        title_id: 0x0100_1234_5678_0000,
        version: 0,
        kind: PackageKind::Base,
        standalone: false,
        files,
        empty_dirs: vec![],
        meta: vec![],
    }]);
    let source = NupSource::new(Arc::new(MemStorage(image))).expect("the NUP opens");
    let nup = scan(&source, &root.join("out"), &opts(&cache)).expect("scans");
    assert_eq!(
        nup.stats.hits, 0,
        "a NUP labels by package: no directory result fits"
    );
    assert!(
        nup.programs
            .values()
            .all(|p| p.srcs.iter().all(|s| s.starts_with("pkg0:")))
    );
    let one = Options {
        package: Some(0),
        ..opts(&cache)
    };
    let picked = scan(&source, &root.join("out"), &one).expect("scans");
    assert_eq!(picked.stats.hits, 0, "the package selection is in the key");
    let again = scan(&source, &root.join("out"), &one).expect("scans");
    assert_eq!(again.stats.hits, again.stats.jobs);
}

#[test]
fn zstd_dictionaries_come_from_the_games_pack() {
    let p = graphics(2, 21);
    let frame = zstd_dict(&blob(&[&p]));
    assert_eq!(
        relift_switch_shaders::zst::frame_dict_id(&frame).expect("the header parses"),
        DICT_ID,
        "the frame names the dictionary"
    );
    let root = scratch("dict");
    let tree = root.join("tree");
    put(&tree, &[("romfs/Effect/d.byml.zs".to_string(), frame)]);
    let r = scan(
        &DirSource::new(&tree),
        &root.join("out"),
        &opts(&root.join("cache")),
    )
    .expect("scans");
    assert!(r.programs.is_empty());
    let e = r
        .errors
        .get(&format!("missing zstd dictionary {DICT_ID}"))
        .expect("the missing dictionary is reported");
    assert_eq!(e.count, 1);
    let pack = zstd(&sarc::build(&[("t.zsdic", DICT), ("note.txt", b"x")]));
    put(&tree, &[("romfs/Pack/ZsDic.pack.zs".to_string(), pack)]);
    let r = scan(
        &DirSource::new(&tree),
        &root.join("out"),
        &opts(&root.join("cache")),
    )
    .expect("scans");
    assert!(r.errors.is_empty(), "{:?}", r.errors);
    assert_eq!(found(&r), shas(&[&p]));
}

fn ktgl_files() -> (Vec<(String, Vec<u8>)>, BTreeSet<String>) {
    let (a, b, c, d) = (
        graphics(1, 31),
        graphics(5, 32),
        compute(33),
        graphics(3, 34),
    );
    let blob_a = blob(&[&a]);
    let blob_b = blob(&[&b, &c]);
    let whole_d = blob(&[&d]);
    let (d1, d2) = whole_d.split_at(whole_d.len() / 2);
    let mut u32_chunks = Vec::new();
    for ch in [zlib_stored(d1), zlib_stored(d2)] {
        u32_chunks.extend_from_slice(&(ch.len() as u32).to_le_bytes());
        u32_chunks.extend(ch);
    }
    u32_chunks.extend_from_slice(&0u32.to_le_bytes());
    let mut u16_chunks = Vec::new();
    for ch in [zlib_stored(&blob_b[..0x180]), zlib_stored(&blob_b[0x180..])] {
        u16_chunks.extend_from_slice(&1u16.to_le_bytes());
        u16_chunks.extend_from_slice(&[0u8; 8]);
        u16_chunks.extend(ch);
    }
    u16_chunks.extend_from_slice(&0u16.to_le_bytes());
    let entries = [
        ktgl::build_entry(ktgl::TYPE_S2G, 0, &blob_a),
        ktgl::build_entry(ktgl::TYPE_DOK, 0x40_0000, &u16_chunks),
        ktgl::build_entry(ktgl::TYPE_S2G, 0x10_0000, &u32_chunks),
        ktgl::build_entry(ktgl::TYPE_S2G, 0x50_0000, &[1, 2, 3, 4]),
    ];
    let mut near = vec![0u8; 0x40];
    let mut locs = Vec::new();
    for e in &entries {
        locs.push(Location {
            index: 0,
            offset: near.len() as u32,
            size: e.len() as u32,
        });
        near.extend_from_slice(e);
        near.extend(vec![0u8; 0x20]);
    }
    locs.push(Location {
        index: 1,
        offset: 0,
        size: 8,
    });
    locs.push(Location {
        index: 2,
        offset: 0,
        size: 8,
    });
    locs.push(Location {
        index: 9,
        offset: 0,
        size: 8,
    });
    let mut rdb = ktgl::build_rdb(ktgl::TYPE_S2G, &locs);
    rdb.extend(ktgl::build_entry(0x1234_5678, 0, &[0u8; 12]));
    let rdx = ktgl::build_rdx(&[(0xFE, 0x1111), (1, 0xabcd), (1, 0xdead)]);
    let files = vec![
        ("romfs/kt/root.rdb".to_string(), rdb),
        ("romfs/kt/root.rdx".to_string(), rdx),
        ("romfs/kt/0x00001111.fdata".to_string(), near),
        (
            "romfs/kt/MountPoint_1/0x0000abcd.fdata".to_string(),
            vec![0u8; 0x40],
        ),
    ];
    (files, shas(&[&a, &b, &c, &d]))
}

fn check_ktgl(r: &Report, expected: &BTreeSet<String>) {
    assert_eq!(&found(r), expected);
    assert_eq!(r.errors["malformed: KTGL: LZ4 chunks"].count, 1);
    assert_eq!(
        r.errors["malformed: KTGL: no IDRK at the entry offset"].count,
        1
    );
    assert_eq!(r.errors["KTGL: the .fdata is not in the input"].count, 1);
    assert_eq!(
        r.errors["KTGL: an entry names an index the .rdx lacks"].count,
        1
    );
    assert_eq!(r.errors.len(), 4, "{:?}", r.errors);
    assert_eq!(r.containers.get("ktgl-entry"), Some(&5));
}

#[test]
fn ktgl_entries_whole_and_in_zlib_chunks() {
    let (files, expected) = ktgl_files();
    let root = scratch("ktgl");
    put(&root.join("tree"), &files);
    let r = scan(
        &DirSource::new(&root.join("tree")),
        &root.join("out"),
        &opts(&root.join("cache")),
    )
    .expect("scans");
    check_ktgl(&r, &expected);
    let again = scan(
        &DirSource::new(&root.join("tree")),
        &root.join("out"),
        &opts(&root.join("cache")),
    )
    .expect("scans");
    assert_eq!(again.stats.hits, again.stats.jobs);
    assert_eq!(found(&again), expected);
}

#[test]
fn ktgl_names_match_in_any_case() {
    let (mut files, expected) = ktgl_files();
    for (p, _) in &mut files {
        *p = match p.as_str() {
            "romfs/kt/root.rdb" => "romfs/kt/ROOT.RDB".into(),
            "romfs/kt/root.rdx" => "romfs/kt/Root.Rdx".into(),
            "romfs/kt/0x00001111.fdata" => "romfs/kt/0x00001111.FDATA".into(),
            other => other.to_string(),
        };
    }
    files.push((
        "romfs/kt/alone.rdb".into(),
        ktgl::build_rdb(ktgl::TYPE_S2G, &[]),
    ));
    let root = scratch("ktgl-case");
    put(&root.join("tree"), &files);
    let r = scan(
        &DirSource::new(&root.join("tree")),
        &root.join("out"),
        &opts(&root.join("cache")),
    )
    .expect("scans");
    assert_eq!(found(&r), expected);
    assert_eq!(r.errors["KTGL: an .rdb has no .rdx beside it"].count, 1);
}

#[test]
fn a_ktgl_entry_length_that_wraps_is_reported() {
    let (mut files, expected) = ktgl_files();
    let mut bad = ktgl::build_entry(ktgl::TYPE_S2G, 0, &[0u8; 12]);
    let at = files[0].1.len() as u64;
    bad[8..16].copy_from_slice(&(u64::MAX - at + 1).to_le_bytes());
    files[0].1.extend(bad);
    let root = scratch("ktgl-wrap");
    put(&root.join("tree"), &files);
    let r = scan(
        &DirSource::new(&root.join("tree")),
        &root.join("out"),
        &opts(&root.join("cache")),
    )
    .expect("scans");
    assert_eq!(found(&r), expected);
    let e = r
        .errors
        .iter()
        .find(|(m, _)| m.contains(".rdb entry length"))
        .expect("the bad entry is reported");
    assert_eq!(e.1.count, 1);
}

#[test]
fn ktgl_through_a_nup_reads_the_same_entries() {
    let (files, expected) = ktgl_files();
    let list: Vec<(&str, &[u8])> = files
        .iter()
        .map(|(n, b)| (n.as_str(), b.as_slice()))
        .collect();
    let image = nup::build(&[BuildPackage {
        title_id: 0x0100_1234_5678_0000,
        version: 0,
        kind: PackageKind::Base,
        standalone: false,
        files: list,
        empty_dirs: vec![],
        meta: vec![],
    }]);
    let source = NupSource::new(Arc::new(MemStorage(image))).expect("the NUP opens");
    let root = scratch("ktgl-nup");
    let r = scan(&source, &root.join("out"), &opts(&root.join("cache"))).expect("scans");
    check_ktgl(&r, &expected);
}
