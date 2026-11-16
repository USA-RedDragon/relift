//! Synthetic cartridge images for tests: a valid header, ARM9 and ARM7
//! static binaries with module parameters and autoload blocks (optionally
//! compressed), overlays, and a file system, built from a [`Spec`].

use crate::blz;
use crate::checksum::crc16;
use crate::params;

/// Where the module parameters sit in a synthetic static binary.
pub const PARAMS_AT: usize = 0x200;
/// Where the literal-pool pointer sits when the layout is
/// [`Layout::LiteralPool`].
pub const POOL_AT: usize = 0x100;
const CODE_MIN: usize = 0x240;

/// How a synthetic static binary announces its module parameters.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Layout {
    /// A 12-byte footer after the binary in the image (ARM9 only).
    Footer,
    /// The marker words, `0xDEC00621` first.
    Markers,
    /// The marker words, `0x2106C0DE` first.
    MarkersSwapped,
    /// No markers: a pointer in a literal pool near the start.
    LiteralPool,
}

/// One autoload block.
#[derive(Clone, Debug)]
pub struct AutoloadSpec {
    /// The load address.
    pub addr: u32,
    /// The data.
    pub data: Vec<u8>,
    /// The bss size.
    pub bss: u32,
}

/// A static binary.
#[derive(Clone, Debug)]
pub struct StaticSpec {
    /// The load address.
    pub ram: u32,
    /// The entry address.
    pub entry: u32,
    /// Size of the code before the autoload data, at least `0x240`, a
    /// multiple of 4.
    pub code_len: usize,
    /// The autoload blocks.
    pub autoloads: Vec<AutoloadSpec>,
    /// How the parameters are announced.
    pub layout: Layout,
    /// Whether to BLZ-compress the static.
    pub compress: bool,
    /// The SDK version word.
    pub sdk_version: u32,
    /// The bss range.
    pub bss: (u32, u32),
}

/// An overlay.
#[derive(Clone, Debug)]
pub struct OverlaySpec {
    /// The overlay id.
    pub id: u32,
    /// The load address.
    pub ram: u32,
    /// The loaded data.
    pub data: Vec<u8>,
    /// The bss size.
    pub bss: u32,
    /// Whether the file is BLZ-compressed.
    pub compress: bool,
    /// The authenticated flag.
    pub authenticated: bool,
    /// The static initializer table, start and end addresses.
    pub static_init: (u32, u32),
}

/// What a synthetic cartridge looks like.
#[derive(Clone, Debug)]
pub struct Spec {
    /// The title, at most 12 bytes.
    pub title: String,
    /// The game code, 4 bytes.
    pub game_code: String,
    /// The maker code, 2 bytes.
    pub maker: String,
    /// The software version.
    pub version: u8,
    /// The 156 logo bytes; synthetic and unrelated to the real logo.
    pub logo: [u8; 156],
    /// The ARM9 binary.
    pub arm9: StaticSpec,
    /// The ARM7 binary.
    pub arm7: StaticSpec,
    /// The ARM9 overlays.
    pub overlays9: Vec<OverlaySpec>,
    /// The ARM7 overlays.
    pub overlays7: Vec<OverlaySpec>,
    /// Named files, with `/` between path parts.
    pub files: Vec<(String, Vec<u8>)>,
}

/// A byte pattern of `len` bytes that compresses well, varied by `seed`.
#[must_use]
pub fn pattern(len: usize, seed: u8) -> Vec<u8> {
    (0..len)
        .map(|i| {
            let k = i / 7;
            seed.wrapping_add(u8::try_from((k * k + i % 5) % 13).unwrap_or(0))
        })
        .collect()
}

/// A logo-sized pattern that is not the real logo.
#[must_use]
pub fn synthetic_logo() -> [u8; 156] {
    let mut logo = [0u8; 156];
    for (i, b) in logo.iter_mut().enumerate() {
        *b = u8::try_from((i * 11 + 5) % 256).unwrap_or(0);
    }
    logo
}

impl Default for StaticSpec {
    fn default() -> StaticSpec {
        StaticSpec {
            ram: 0x0200_0000,
            entry: 0x0200_0800,
            code_len: 0x400,
            autoloads: vec![
                AutoloadSpec {
                    addr: 0x01FF_8000,
                    data: pattern(0x80, 1),
                    bss: 0x40,
                },
                AutoloadSpec {
                    addr: 0x027E_0000,
                    data: pattern(0x100, 2),
                    bss: 0,
                },
            ],
            layout: Layout::Footer,
            compress: false,
            sdk_version: 0x0003_0000,
            bss: (0x0210_0000, 0x0210_4000),
        }
    }
}

impl Default for Spec {
    fn default() -> Spec {
        Spec {
            title: "SYNTH".to_owned(),
            game_code: "ASYN".to_owned(),
            maker: "01".to_owned(),
            version: 0,
            logo: synthetic_logo(),
            arm9: StaticSpec::default(),
            arm7: StaticSpec {
                ram: 0x0238_0000,
                entry: 0x0238_0400,
                layout: Layout::LiteralPool,
                autoloads: vec![AutoloadSpec {
                    addr: 0x0380_0000,
                    data: pattern(0x60, 3),
                    bss: 0x20,
                }],
                ..StaticSpec::default()
            },
            overlays9: Vec::new(),
            overlays7: Vec::new(),
            files: Vec::new(),
        }
    }
}

fn put32(b: &mut [u8], at: usize, v: u32) {
    b[at..at + 4].copy_from_slice(&v.to_le_bytes());
}

fn size32(n: usize) -> u32 {
    u32::try_from(n).expect("a synthetic image stays under 4 GiB")
}

/// The static binary of `s` as the program sees it (compressed static end
/// zero), and the offset of its autoload list.
///
/// # Panics
///
/// `s.code_len` is below `0x240` or not a multiple of 4.
#[must_use]
pub fn static_image(s: &StaticSpec) -> (Vec<u8>, usize) {
    assert!(
        s.code_len >= CODE_MIN && s.code_len.is_multiple_of(4),
        "code length"
    );
    let mut b: Vec<u8> = (0..s.code_len)
        .map(|i| u8::try_from(i % 241).unwrap_or(0))
        .collect();
    let data_start = s.code_len;
    let data_len: usize = s.autoloads.iter().map(|a| a.data.len()).sum();
    let list = data_start + data_len;
    for a in &s.autoloads {
        b.extend_from_slice(&a.data);
    }
    for a in &s.autoloads {
        b.extend_from_slice(&a.addr.to_le_bytes());
        b.extend_from_slice(&size32(a.data.len()).to_le_bytes());
        b.extend_from_slice(&a.bss.to_le_bytes());
    }
    let at = |off: usize| s.ram + size32(off);
    let p = PARAMS_AT;
    put32(&mut b, p, at(list));
    put32(&mut b, p + 4, at(list + 12 * s.autoloads.len()));
    put32(&mut b, p + 8, at(data_start));
    put32(&mut b, p + 12, s.bss.0);
    put32(&mut b, p + 16, s.bss.1);
    put32(&mut b, p + 0x14, 0);
    put32(&mut b, p + params::SDK_VERSION, s.sdk_version);
    match s.layout {
        Layout::Footer | Layout::Markers => {
            put32(&mut b, p + params::MARKERS, 0xDEC0_0621);
            put32(&mut b, p + params::MARKERS + 4, 0x2106_C0DE);
        }
        Layout::MarkersSwapped => {
            put32(&mut b, p + params::MARKERS, 0x2106_C0DE);
            put32(&mut b, p + params::MARKERS + 4, 0xDEC0_0621);
        }
        Layout::LiteralPool => {
            put32(&mut b, p + params::MARKERS, 0);
            put32(&mut b, p + params::MARKERS + 4, 0);
            put32(&mut b, POOL_AT, at(p));
        }
    }
    (b, list)
}

/// The static binary as stored in the image: compressed when asked, with
/// the compressed static end set.
///
/// # Panics
///
/// The static cannot be compressed.
#[must_use]
pub fn static_stored(s: &StaticSpec) -> Vec<u8> {
    let (image, _) = static_image(s);
    if !s.compress {
        return image;
    }
    let mut blob = blz::compress(&image, PARAMS_AT + params::SIZE).expect("compressible static");
    let end = s.ram + size32(blob.len());
    put32(&mut blob, PARAMS_AT + params::COMPRESSED_END, end);
    blob
}

struct Layouter {
    rom: Vec<u8>,
}

impl Layouter {
    fn put(&mut self, bytes: &[u8]) -> u32 {
        while !self.rom.len().is_multiple_of(0x200) {
            self.rom.push(0xFF);
        }
        let at = size32(self.rom.len());
        self.rom.extend_from_slice(bytes);
        at
    }
}

struct Dir {
    path: String,
    files: Vec<usize>,
}

fn fnt(files: &[(String, Vec<u8>)], first_id: u32) -> (Vec<u8>, Vec<usize>) {
    let mut paths: Vec<String> = vec![String::new()];
    for (p, _) in files {
        let mut at = 0;
        while let Some(i) = p[at..].find('/') {
            paths.push(p[..at + i].to_owned());
            at += i + 1;
        }
    }
    paths.sort();
    paths.dedup();
    let mut dirs: Vec<Dir> = paths
        .into_iter()
        .map(|path| Dir {
            path,
            files: Vec::new(),
        })
        .collect();
    for (n, (p, _)) in files.iter().enumerate() {
        let parent = p.rfind('/').map_or("", |i| &p[..i]);
        let d = dirs
            .iter_mut()
            .find(|d| d.path == parent)
            .expect("the parent directory exists");
        d.files.push(n);
    }
    let mut ids = vec![0usize; files.len()];
    let mut next = first_id as usize;
    let mut firsts = Vec::new();
    for d in &dirs {
        firsts.push(next);
        for n in &d.files {
            ids[*n] = next;
            next += 1;
        }
    }
    let mut main = vec![0u8; 8 * dirs.len()];
    let mut subs = Vec::new();
    for (i, d) in dirs.iter().enumerate() {
        let off = 8 * dirs.len() + subs.len();
        put32(&mut main, 8 * i, size32(off));
        let parent = if i == 0 {
            dirs.len()
        } else {
            let pp = d.path.rfind('/').map_or("", |k| &d.path[..k]);
            0xF000 + dirs.iter().position(|x| x.path == pp).unwrap_or(0)
        };
        main[8 * i + 4..8 * i + 6]
            .copy_from_slice(&u16::try_from(firsts[i]).unwrap_or(0).to_le_bytes());
        main[8 * i + 6..8 * i + 8]
            .copy_from_slice(&u16::try_from(parent).unwrap_or(0).to_le_bytes());
        for n in &d.files {
            let name = files[*n].0.rsplit('/').next().unwrap_or("");
            subs.push(u8::try_from(name.len()).unwrap_or(0));
            subs.extend_from_slice(name.as_bytes());
        }
        for (k, c) in dirs.iter().enumerate().skip(1) {
            let pp = c.path.rfind('/').map_or("", |j| &c.path[..j]);
            if pp == d.path {
                let name = c.path.rsplit('/').next().unwrap_or("");
                subs.push(0x80 | u8::try_from(name.len()).unwrap_or(0));
                subs.extend_from_slice(name.as_bytes());
                subs.extend_from_slice(&u16::try_from(0xF000 + k).unwrap_or(0).to_le_bytes());
            }
        }
        subs.push(0);
    }
    main.extend(subs);
    (main, ids)
}

fn overlay_file(o: &OverlaySpec) -> Vec<u8> {
    if o.compress {
        blz::compress(&o.data, 0).expect("compressible overlay")
    } else {
        o.data.clone()
    }
}

fn overlay_table(list: &[OverlaySpec], file_id0: u32, files: &[Vec<u8>]) -> Vec<u8> {
    let mut t = Vec::new();
    for (i, o) in list.iter().enumerate() {
        let mut flags = size32(files[i].len()) & 0x00FF_FFFF;
        if o.compress {
            flags |= 1 << 24;
        }
        if o.authenticated {
            flags |= 1 << 25;
        }
        for w in [
            o.id,
            o.ram,
            size32(o.data.len()),
            o.bss,
            o.static_init.0,
            o.static_init.1,
            file_id0 + size32(i),
            flags,
        ] {
            t.extend_from_slice(&w.to_le_bytes());
        }
    }
    t
}

/// Build the image of `spec`: the header at `0x000`, padded to `0x4000`,
/// the ARM9 binary there (then its footer for [`Layout::Footer`]), the ARM7
/// binary, the name table, the allocation table, the overlay tables, the
/// overlay files and the named files, each at a `0x200` boundary. The
/// header, logo and secure area CRCs are correct.
///
/// # Panics
///
/// A field is longer than its slot or a static or overlay cannot be
/// compressed.
#[must_use]
pub fn rom(spec: &Spec) -> Vec<u8> {
    let mut l = Layouter {
        rom: vec![0u8; 0x4000],
    };
    let arm9 = static_stored(&spec.arm9);
    let arm9_off = l.put(&arm9);
    if spec.arm9.layout == Layout::Footer {
        let mut f = Vec::new();
        for w in [params::FOOTER_MARK, size32(PARAMS_AT), 0] {
            f.extend_from_slice(&w.to_le_bytes());
        }
        l.rom.extend_from_slice(&f);
    }
    let arm7 = static_stored(&spec.arm7);
    let arm7_off = l.put(&arm7);

    let n9 = spec.overlays9.len();
    let n7 = spec.overlays7.len();
    let files9: Vec<Vec<u8>> = spec.overlays9.iter().map(overlay_file).collect();
    let files7: Vec<Vec<u8>> = spec.overlays7.iter().map(overlay_file).collect();
    let (fnt_bytes, ids) = fnt(&spec.files, size32(n9 + n7));
    let fnt_off = l.put(&fnt_bytes);
    let total_files = n9 + n7 + spec.files.len();
    let fat_off = l.put(&vec![0u8; 8 * total_files]);
    let ovt9 = overlay_table(&spec.overlays9, 0, &files9);
    let ovt7 = overlay_table(&spec.overlays7, size32(n9), &files7);
    let ovt9_off = l.put(&ovt9);
    let ovt7_off = l.put(&ovt7);
    let mut fat = vec![0u8; 8 * total_files];
    let mut place = |l: &mut Layouter, id: usize, bytes: &[u8]| {
        let start = l.put(bytes);
        put32(&mut fat, 8 * id, start);
        put32(&mut fat, 8 * id + 4, start + size32(bytes.len()));
    };
    for (i, f) in files9.iter().enumerate() {
        place(&mut l, i, f);
    }
    for (i, f) in files7.iter().enumerate() {
        place(&mut l, n9 + i, f);
    }
    for (n, (_, data)) in spec.files.iter().enumerate() {
        place(&mut l, ids[n], data);
    }
    l.rom[fat_off as usize..fat_off as usize + fat.len()].copy_from_slice(&fat);
    let mut rom = l.rom;
    if rom.len() < 0x8000 {
        rom.resize(0x8000, 0xFF);
    }
    let total = size32(rom.len());

    let put = |rom: &mut [u8], at: usize, max: usize, bytes: &[u8]| {
        assert!(bytes.len() <= max, "field longer than its slot");
        rom[at..at + bytes.len()].copy_from_slice(bytes);
    };
    put(&mut rom, 0, 12, spec.title.as_bytes());
    put(&mut rom, 0x0C, 4, spec.game_code.as_bytes());
    put(&mut rom, 0x10, 2, spec.maker.as_bytes());
    rom[0x1E] = spec.version;
    for (at, off, s, bytes) in [
        (0x20, arm9_off, &spec.arm9, &arm9),
        (0x30, arm7_off, &spec.arm7, &arm7),
    ] {
        put32(&mut rom, at, off);
        put32(&mut rom, at + 4, s.entry);
        put32(&mut rom, at + 8, s.ram);
        put32(&mut rom, at + 12, size32(bytes.len()));
    }
    for (at, off, size) in [
        (0x40, fnt_off, size32(fnt_bytes.len())),
        (0x48, fat_off, size32(fat.len())),
        (0x50, ovt9_off, size32(ovt9.len())),
        (0x58, ovt7_off, size32(ovt7.len())),
    ] {
        put32(&mut rom, at, off);
        put32(&mut rom, at + 4, size);
    }
    put32(&mut rom, 0x80, total);
    put32(&mut rom, 0x84, 0x4000);
    rom[0xC0..0xC0 + 156].copy_from_slice(&spec.logo);
    let secure = crc16(&rom[arm9_off as usize..0x8000]);
    rom[0x6C..0x6E].copy_from_slice(&secure.to_le_bytes());
    fix_crcs(&mut rom);
    rom
}

/// Recompute and store the logo and header CRCs of `rom`.
pub fn fix_crcs(rom: &mut [u8]) {
    let logo = crc16(&rom[0xC0..0xC0 + 156]);
    rom[0x15C..0x15E].copy_from_slice(&logo.to_le_bytes());
    let header = crc16(&rom[..0x15E]);
    rom[0x15E..0x160].copy_from_slice(&header.to_le_bytes());
}
