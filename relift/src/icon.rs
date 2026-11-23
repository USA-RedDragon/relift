//! The icon of a game as PNG, made once at build time from the guest's own
//! image (a Switch control icon is JPEG). The converted PNG is what the
//! self-description embeds; the window, the Windows `.ico`, the Android
//! launcher sizes and the AppImage icon all come from those bytes. A game
//! without an icon gets a generated one only for launcher and desktop
//! entries, never for a window.

use relift_info::Icon;
use sha2::{Digest, Sha256};

/// A decoded image: RGB, row by row.
struct Rgb {
    width: usize,
    height: usize,
    pixels: Vec<u8>,
}

fn crc32(data: &[u8]) -> u32 {
    let mut crc = 0xffff_ffffu32;
    for &b in data {
        crc ^= u32::from(b);
        for _ in 0..8 {
            crc = if crc & 1 == 1 {
                (crc >> 1) ^ 0xedb8_8320
            } else {
                crc >> 1
            };
        }
    }
    !crc
}

fn adler32(data: &[u8]) -> u32 {
    let (mut a, mut b) = (1u32, 0u32);
    for &x in data {
        a = (a + u32::from(x)) % 65521;
        b = (b + a) % 65521;
    }
    (b << 16) | a
}

fn chunk(out: &mut Vec<u8>, kind: &[u8; 4], body: &[u8]) {
    out.extend((body.len() as u32).to_be_bytes());
    let start = out.len();
    out.extend(kind);
    out.extend(body);
    let crc = crc32(&out[start..]);
    out.extend(crc.to_be_bytes());
}

/// A PNG of `img`: 8-bit RGB, no filter, zlib stored blocks.
fn encode(img: &Rgb) -> Vec<u8> {
    let mut raw = Vec::with_capacity((img.width * 3 + 1) * img.height);
    for row in img.pixels.chunks(img.width * 3) {
        raw.push(0);
        raw.extend(row);
    }
    let mut z = vec![0x78, 0x01];
    let mut blocks = raw.chunks(65535).peekable();
    while let Some(b) = blocks.next() {
        z.push(u8::from(blocks.peek().is_none()));
        z.extend((b.len() as u16).to_le_bytes());
        z.extend((!(b.len() as u16)).to_le_bytes());
        z.extend(b);
    }
    z.extend(adler32(&raw).to_be_bytes());
    let mut out = b"\x89PNG\r\n\x1a\n".to_vec();
    let mut head = Vec::new();
    head.extend((img.width as u32).to_be_bytes());
    head.extend((img.height as u32).to_be_bytes());
    head.extend([8, 2, 0, 0, 0]);
    chunk(&mut out, b"IHDR", &head);
    chunk(&mut out, b"IDAT", &z);
    chunk(&mut out, b"IEND", &[]);
    out
}

fn unbase64(s: &str) -> Option<Vec<u8>> {
    let mut out = Vec::with_capacity(s.len() / 4 * 3);
    let (mut acc, mut bits) = (0u32, 0);
    for c in s.bytes().filter(|c| !c.is_ascii_whitespace() && *c != b'=') {
        let v = match c {
            b'A'..=b'Z' => c - b'A',
            b'a'..=b'z' => c - b'a' + 26,
            b'0'..=b'9' => c - b'0' + 52,
            b'+' => 62,
            b'/' => 63,
            _ => return None,
        };
        acc = (acc << 6) | u32::from(v);
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((acc >> bits) as u8);
            acc &= (1 << bits) - 1;
        }
    }
    Some(out)
}

fn decode_jpeg(bytes: &[u8]) -> Option<Rgb> {
    let mut d = jpeg_decoder::Decoder::new(bytes);
    let px = d.decode().ok()?;
    let info = d.info()?;
    let pixels = match info.pixel_format {
        jpeg_decoder::PixelFormat::RGB24 => px,
        jpeg_decoder::PixelFormat::L8 => px.iter().flat_map(|&g| [g, g, g]).collect(),
        _ => return None,
    };
    Some(Rgb {
        width: usize::from(info.width),
        height: usize::from(info.height),
        pixels,
    })
}

/// A 5 by 5 mirrored pattern in a color taken from `seed`, 128 pixels
/// wide.
fn generated(seed: &str) -> Rgb {
    let h = Sha256::digest(seed.as_bytes());
    let color = [64 + h[0] % 128, 64 + h[1] % 128, 64 + h[2] % 128];
    let cell = 20;
    let size = 128;
    let margin = (size - 5 * cell) / 2;
    let mut pixels = vec![240u8; size * size * 3];
    for y in 0..5 {
        for x in 0..5 {
            let mirrored = if x < 3 { x } else { 4 - x };
            let bit = y * 3 + mirrored;
            if h[3 + bit / 8] >> (bit % 8) & 1 == 0 {
                continue;
            }
            for py in 0..cell {
                for px in 0..cell {
                    let at = ((margin + y * cell + py) * size + margin + x * cell + px) * 3;
                    pixels[at..at + 3].copy_from_slice(&color);
                }
            }
        }
    }
    Rgb {
        width: size,
        height: size,
        pixels,
    }
}

/// Decodes a WebP image (lossy or lossless) to RGB, alpha dropped over
/// black as the console's icon frame shows it.
fn decode_webp(bytes: &[u8]) -> Option<Rgb> {
    let mut d = image_webp::WebPDecoder::new(std::io::Cursor::new(bytes)).ok()?;
    let (w, h) = d.dimensions();
    let mut buf = vec![0u8; d.output_buffer_size()?];
    d.read_image(&mut buf).ok()?;
    let pixels = if d.has_alpha() {
        buf.chunks_exact(4)
            .flat_map(|p| {
                let a = u16::from(p[3]);
                [0, 1, 2].map(|i| u8::try_from(u16::from(p[i]) * a / 255).unwrap_or(255))
            })
            .collect()
    } else {
        buf
    };
    Some(Rgb {
        width: usize::try_from(w).ok()?,
        height: usize::try_from(h).ok()?,
        pixels,
    })
}

/// The icon of a game from its control data's image, JPEG (the Switch)
/// or WebP (the Switch 2), told apart by content, not by file name:
/// converted to PNG, with its size. `None` when the bytes do not decode.
#[must_use]
pub fn from_image(bytes: &[u8]) -> Option<Icon> {
    let webp = bytes.len() >= 12 && &bytes[..4] == b"RIFF" && &bytes[8..12] == b"WEBP";
    let img = if webp {
        decode_webp(bytes)?
    } else {
        decode_jpeg(bytes)?
    };
    Some(Icon {
        format: "png".into(),
        width: u32::try_from(img.width).ok()?,
        height: u32::try_from(img.height).ok()?,
        data: crate::identity::base64(&encode(&img)),
    })
}

fn inflate_stored(z: &[u8]) -> Option<Vec<u8>> {
    let mut at = 2;
    let mut out = Vec::new();
    loop {
        let head = *z.get(at)?;
        let len = usize::from(u16::from_le_bytes(z.get(at + 1..at + 3)?.try_into().ok()?));
        let nlen = u16::from_le_bytes(z.get(at + 3..at + 5)?.try_into().ok()?);
        if head & 6 != 0 || nlen != !(len as u16) {
            return None;
        }
        out.extend(z.get(at + 5..at + 5 + len)?);
        at += 5 + len;
        if head & 1 == 1 {
            return Some(out);
        }
    }
}

/// Decodes a PNG of the form [`encode`] writes (8-bit RGB, no filter,
/// stored blocks); any other PNG is `None`.
fn decode_own(png: &[u8]) -> Option<Rgb> {
    let rest = png.strip_prefix(b"\x89PNG\r\n\x1a\n")?;
    let mut at = 0;
    let mut dims = None;
    let mut z = Vec::new();
    while let Some(len) = rest.get(at..at + 4) {
        let len = u32::from_be_bytes(len.try_into().ok()?) as usize;
        let kind = rest.get(at + 4..at + 8)?;
        let body = rest.get(at + 8..at + 8 + len)?;
        match kind {
            b"IHDR" => {
                if body.get(8..13)? != [8, 2, 0, 0, 0] {
                    return None;
                }
                let w = u32::from_be_bytes(body.get(..4)?.try_into().ok()?) as usize;
                let h = u32::from_be_bytes(body.get(4..8)?.try_into().ok()?) as usize;
                dims = Some((w, h));
            }
            b"IDAT" => z.extend(body),
            _ => {}
        }
        at += 12 + len;
    }
    let (width, height) = dims?;
    let raw = inflate_stored(&z)?;
    if width == 0 || raw.len() != (width * 3 + 1) * height {
        return None;
    }
    let mut pixels = Vec::with_capacity(width * height * 3);
    for row in raw.chunks(width * 3 + 1) {
        if row[0] != 0 {
            return None;
        }
        pixels.extend(&row[1..]);
    }
    Some(Rgb {
        width,
        height,
        pixels,
    })
}

/// `img` scaled to `size` by `size` pixels, averaging the source pixels
/// each target pixel covers (a nearest pixel when scaling up).
fn scaled(img: &Rgb, size: usize) -> Rgb {
    let mut pixels = Vec::with_capacity(size * size * 3);
    for y in 0..size {
        let (y0, y1) = span(y, size, img.height);
        for x in 0..size {
            let (x0, x1) = span(x, size, img.width);
            let mut sum = [0usize; 3];
            for sy in y0..y1 {
                for sx in x0..x1 {
                    let at = (sy * img.width + sx) * 3;
                    for (c, v) in sum.iter_mut().enumerate() {
                        *v += usize::from(img.pixels[at + c]);
                    }
                }
            }
            let n = (y1 - y0) * (x1 - x0);
            pixels.extend(sum.map(|v| (v / n) as u8));
        }
    }
    Rgb {
        width: size,
        height: size,
        pixels,
    }
}

fn span(i: usize, to: usize, from: usize) -> (usize, usize) {
    let a = i * from / to;
    let b = ((i + 1) * from).div_ceil(to).max(a + 1);
    (a, b.min(from))
}

/// The game's image: its own icon (PNG made by this module), or with
/// `generate` the generated one. `None` for a game without an icon when
/// `generate` is off.
fn image_of(icon: Option<&Icon>, seed: &str, generate: bool) -> Option<Rgb> {
    icon.and_then(|i| decode_own(&unbase64(&i.data)?))
        .or_else(|| generate.then(|| generated(seed)))
}

/// The Android launcher icons: `(resource directory, PNG)` for each density.
/// A game without an icon gets the generated one (the launcher needs some
/// icon).
#[must_use]
pub fn launcher_pngs(icon: Option<&Icon>, seed: &str) -> Vec<(&'static str, Vec<u8>)> {
    const SIZES: [(&str, usize); 5] = [
        ("mipmap-mdpi", 48),
        ("mipmap-hdpi", 72),
        ("mipmap-xhdpi", 96),
        ("mipmap-xxhdpi", 144),
        ("mipmap-xxxhdpi", 192),
    ];
    let Some(img) = image_of(icon, seed, true) else {
        return Vec::new();
    };
    SIZES
        .iter()
        .map(|(dir, n)| (*dir, encode(&scaled(&img, *n))))
        .collect()
}

/// The iOS home screen icons: `(file name in the bundle, PNG)`. Files named
/// `AppIcon60x60@2x.png` and the like are what `CFBundleIconFiles` lists
/// without suffix; iOS picks the size and scale it needs. A game without an
/// icon gets the generated one.
#[must_use]
pub fn ios_pngs(icon: Option<&Icon>, seed: &str) -> Vec<(&'static str, Vec<u8>)> {
    const SIZES: [(&str, usize); 4] = [
        ("AppIcon60x60@2x.png", 120),
        ("AppIcon60x60@3x.png", 180),
        ("AppIcon76x76@2x~ipad.png", 152),
        ("AppIcon83.5x83.5@2x~ipad.png", 167),
    ];
    let Some(img) = image_of(icon, seed, true) else {
        return Vec::new();
    };
    SIZES
        .iter()
        .map(|(name, n)| (*name, encode(&scaled(&img, *n))))
        .collect()
}

/// A Windows `.ico` of the game's own icon, PNG entries at 16, 32, 48 and
/// 256 pixels; `None` for a game without one (the executable keeps the
/// system's icon).
#[must_use]
pub fn ico(icon: Option<&Icon>) -> Option<Vec<u8>> {
    let img = image_of(icon, "", false)?;
    let sizes = [16usize, 32, 48, 256];
    let pngs: Vec<Vec<u8>> = sizes.iter().map(|n| encode(&scaled(&img, *n))).collect();
    let mut out = Vec::new();
    out.extend([0u8, 0, 1, 0]);
    out.extend((sizes.len() as u16).to_le_bytes());
    let mut offset = 6 + 16 * sizes.len();
    for (n, png) in sizes.iter().zip(&pngs) {
        out.push(if *n >= 256 { 0 } else { *n as u8 });
        out.push(if *n >= 256 { 0 } else { *n as u8 });
        out.extend([0, 0, 1, 0, 32, 0]);
        out.extend((png.len() as u32).to_le_bytes());
        out.extend((offset as u32).to_le_bytes());
        offset += png.len();
    }
    for png in pngs {
        out.extend(png);
    }
    Some(out)
}

/// A macOS `.icns` of the game's icon, PNG entries from 16 to 256 pixels
/// (with the Retina variants). A game without an icon gets the generated
/// one for `seed` (Finder and the Dock need some icon).
#[must_use]
pub fn icns(icon: Option<&Icon>, seed: &str) -> Vec<u8> {
    const SIZES: [(&[u8; 4], usize); 7] = [
        (b"icp4", 16),
        (b"ic11", 32),
        (b"icp5", 32),
        (b"ic12", 64),
        (b"ic07", 128),
        (b"ic13", 256),
        (b"ic08", 256),
    ];
    let Some(img) = image_of(icon, seed, true) else {
        return Vec::new();
    };
    let mut body = Vec::new();
    for (kind, n) in SIZES {
        let png = encode(&scaled(&img, n));
        body.extend(kind);
        body.extend(((png.len() + 8) as u32).to_be_bytes());
        body.extend(png);
    }
    let mut out = b"icns".to_vec();
    out.extend(((body.len() + 8) as u32).to_be_bytes());
    out.extend(body);
    out
}

/// The PNG bytes of the game's icon. `seed` (the game's name) decides the
/// generated icon of a game without one.
#[must_use]
pub fn png(icon: Option<&Icon>, seed: &str) -> Vec<u8> {
    let from_icon = icon.and_then(|i| {
        let bytes = unbase64(&i.data)?;
        match i.format.as_str() {
            "png" if bytes.starts_with(b"\x89PNG") => Some(bytes),
            "jpeg" => decode_jpeg(&bytes).map(|r| encode(&r)),
            _ => None,
        }
    });
    from_icon.unwrap_or_else(|| encode(&generated(seed)))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc_and_adler_match_their_check_values() {
        assert_eq!(crc32(b"123456789"), 0xcbf4_3926);
        assert_eq!(adler32(b"Wikipedia"), 0x11e6_0398);
    }

    #[test]
    fn base64_decodes_with_and_without_padding() {
        assert_eq!(unbase64("Zm9v").unwrap(), b"foo");
        assert_eq!(unbase64("Zm8=").unwrap(), b"fo");
        assert_eq!(unbase64("Zg").unwrap(), b"f");
        assert!(unbase64("Z*").is_none());
    }

    #[test]
    fn a_png_has_its_signature_and_a_header_of_the_image_size() {
        let p = png(None, "seed");
        assert!(p.starts_with(b"\x89PNG\r\n\x1a\n"));
        assert_eq!(&p[12..16], b"IHDR");
        assert_eq!(u32::from_be_bytes(p[16..20].try_into().unwrap()), 128);
        assert_eq!(&p[p.len() - 8..p.len() - 4], b"IEND");
    }

    #[test]
    fn the_generated_icon_depends_on_the_name_only() {
        assert_eq!(png(None, "a"), png(None, "a"));
        assert_ne!(png(None, "a"), png(None, "b"));
    }

    #[test]
    fn a_png_icon_passes_through_and_a_broken_jpeg_falls_back() {
        let own = png(None, "x");
        let icon = Icon {
            format: "png".into(),
            width: 128,
            height: 128,
            data: crate::identity::base64(&own),
        };
        assert_eq!(png(Some(&icon), "y"), own);
        let broken = Icon {
            format: "jpeg".into(),
            width: 0,
            height: 0,
            data: crate::identity::base64(b"not a jpeg"),
        };
        assert_eq!(png(Some(&broken), "y"), png(None, "y"));
    }

    #[test]
    fn a_webp_icon_is_told_by_content_and_decoded() {
        let mut rgba = Vec::new();
        for y in 0..8u8 {
            for x in 0..8u8 {
                rgba.extend([x * 30, y * 30, 200, if x == 0 { 0 } else { 255 }]);
            }
        }
        let mut webp = Vec::new();
        image_webp::WebPEncoder::new(&mut webp)
            .encode(&rgba, 8, 8, image_webp::ColorType::Rgba8)
            .unwrap();
        let icon = from_image(&webp).expect("a WebP icon decodes");
        assert_eq!(
            (icon.format.as_str(), icon.width, icon.height),
            ("png", 8, 8)
        );
        let rgb = decode_own(&unbase64(&icon.data).unwrap()).unwrap();
        assert_eq!(
            &rgb.pixels[..3],
            &[0, 0, 0],
            "a clear pixel shows over black"
        );
        assert_eq!(&rgb.pixels[3..6], &[30, 0, 200]);
        assert!(from_image(b"RIFF\0\0\0\0WEBPbroken").is_none());
    }

    fn gradient() -> Icon {
        let mut pixels = Vec::new();
        for y in 0..64u8 {
            for x in 0..64u8 {
                pixels.extend([x * 4, y * 4, 128]);
            }
        }
        let img = Rgb {
            width: 64,
            height: 64,
            pixels,
        };
        Icon {
            format: "png".into(),
            width: 64,
            height: 64,
            data: crate::identity::base64(&encode(&img)),
        }
    }

    #[test]
    fn our_png_decodes_back_to_the_same_pixels() {
        let icon = gradient();
        let img = decode_own(&unbase64(&icon.data).unwrap()).unwrap();
        assert_eq!((img.width, img.height), (64, 64));
        assert_eq!(&img.pixels[..3], &[0, 0, 128]);
        assert_eq!(&img.pixels[63 * 3..64 * 3], &[252, 0, 128]);
        assert!(decode_own(b"\x89PNG\r\n\x1a\nnope").is_none());
    }

    #[test]
    fn a_jpeg_becomes_a_png_with_its_size() {
        let jpeg: &[u8] = include_bytes!("../tests/icon.jpg");
        let icon = from_image(jpeg).unwrap();
        assert_eq!(
            (icon.format.as_str(), icon.width, icon.height),
            ("png", 16, 8)
        );
        let img = decode_own(&unbase64(&icon.data).unwrap()).unwrap();
        assert_eq!((img.width, img.height), (16, 8));
        assert!(from_image(b"not a jpeg").is_none());
    }

    #[test]
    fn the_ios_icons_have_the_sizes_ios_asks_for() {
        for icon in [Some(gradient()), None] {
            let l = ios_pngs(icon.as_ref(), "g");
            assert_eq!(l.len(), 4);
            for ((name, png), n) in l.iter().zip([120u32, 180, 152, 167]) {
                assert!(name.starts_with("AppIcon"), "{name}");
                assert_eq!(u32::from_be_bytes(png[16..20].try_into().unwrap()), n);
            }
        }
    }

    #[test]
    fn the_launcher_gets_five_densities_and_a_game_without_icon_a_generated_one() {
        for icon in [Some(gradient()), None] {
            let l = launcher_pngs(icon.as_ref(), "g");
            let dirs: Vec<_> = l.iter().map(|(d, _)| *d).collect();
            assert_eq!(
                dirs,
                [
                    "mipmap-mdpi",
                    "mipmap-hdpi",
                    "mipmap-xhdpi",
                    "mipmap-xxhdpi",
                    "mipmap-xxxhdpi"
                ]
            );
            for ((_, png), n) in l.iter().zip([48u32, 72, 96, 144, 192]) {
                assert_eq!(u32::from_be_bytes(png[16..20].try_into().unwrap()), n);
            }
        }
    }

    #[test]
    fn the_ico_lists_its_entries_and_a_game_without_icon_has_none() {
        assert!(ico(None).is_none());
        let ico = ico(Some(&gradient())).unwrap();
        assert_eq!(&ico[..6], &[0, 0, 1, 0, 4, 0]);
        let entry = |i: usize| &ico[6 + 16 * i..22 + 16 * i];
        assert_eq!((entry(0)[0], entry(3)[0]), (16, 0));
        for i in 0..4 {
            let len = u32::from_le_bytes(entry(i)[8..12].try_into().unwrap()) as usize;
            let at = u32::from_le_bytes(entry(i)[12..16].try_into().unwrap()) as usize;
            assert_eq!(&ico[at..at + 8], b"\x89PNG\r\n\x1a\n");
            assert!(at + len <= ico.len());
        }
    }

    #[test]
    fn the_icns_holds_a_png_for_each_size() {
        for icon in [Some(gradient()), None] {
            let icns = icns(icon.as_ref(), "g");
            assert_eq!(&icns[..4], b"icns");
            assert_eq!(
                u32::from_be_bytes(icns[4..8].try_into().unwrap()) as usize,
                icns.len()
            );
            let mut at = 8;
            let mut seen = Vec::new();
            while at < icns.len() {
                let len = u32::from_be_bytes(icns[at + 4..at + 8].try_into().unwrap()) as usize;
                let png = &icns[at + 8..at + len];
                assert_eq!(&png[..8], b"\x89PNG\r\n\x1a\n");
                seen.push((
                    std::str::from_utf8(&icns[at..at + 4]).unwrap().to_string(),
                    u32::from_be_bytes(png[16..20].try_into().unwrap()),
                ));
                at += len;
            }
            assert_eq!(at, icns.len());
            let want: Vec<(String, u32)> = [
                ("icp4", 16),
                ("ic11", 32),
                ("icp5", 32),
                ("ic12", 64),
                ("ic07", 128),
                ("ic13", 256),
                ("ic08", 256),
            ]
            .iter()
            .map(|(k, n)| ((*k).to_string(), *n))
            .collect();
            assert_eq!(seen, want);
        }
    }

    #[test]
    fn scaling_down_averages_and_up_repeats() {
        let img = Rgb {
            width: 2,
            height: 2,
            pixels: vec![0, 0, 0, 100, 100, 100, 100, 100, 100, 200, 200, 200],
        };
        assert_eq!(scaled(&img, 1).pixels, [100, 100, 100]);
        let up = scaled(&img, 4);
        assert_eq!(&up.pixels[..3], &[0, 0, 0]);
        assert_eq!(&up.pixels[(3 * 4 + 3) * 3..], &[200, 200, 200]);
    }
}
