//! The PNG icon of a game: the identity's icon converted to PNG, or a
//! generated one when the game has none (a cartridge header holds no
//! image). AppImage and desktop menus want PNG.

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
        let p = png(None, "game");
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
}
