//! CRC-32 and the CRC1/CRC2 checksum of the header.

use crate::cic::Cic;

/// The first byte the checksum covers.
pub const START: usize = 0x1000;
/// The number of bytes the checksum covers.
pub const LENGTH: usize = 0x10_0000;

const fn table() -> [u32; 256] {
    let mut t = [0; 256];
    let mut i = 0;
    while i < 256 {
        let mut c = i as u32;
        let mut k = 0;
        while k < 8 {
            c = if c & 1 != 0 {
                (c >> 1) ^ 0xEDB8_8320
            } else {
                c >> 1
            };
            k += 1;
        }
        t[i] = c;
        i += 1;
    }
    t
}

static TABLE: [u32; 256] = table();

/// The IEEE CRC-32 of `bytes`.
#[must_use]
pub fn crc32(bytes: &[u8]) -> u32 {
    !bytes
        .iter()
        .fold(!0u32, |c, &b| (c >> 8) ^ TABLE[usize::from((c as u8) ^ b)])
}

/// The CRC1 and CRC2 of a big-endian image under `cic`'s seed. `None` when
/// the image is shorter than `0x101000` bytes or the variant's checksum
/// cannot be computed from the game's own data.
#[must_use]
pub fn compute(be: &[u8], cic: Cic) -> Option<(u32, u32)> {
    let seed = cic.seed()?;
    let body = be.get(START..START + LENGTH)?;
    let (mut t1, mut t2, mut t3, mut t4, mut t5, mut t6) = (seed, seed, seed, seed, seed, seed);
    for w in body.as_chunks::<4>().0 {
        let d = u32::from_be_bytes(*w);
        let (sum, carry) = t6.overflowing_add(d);
        if carry {
            t4 = t4.wrapping_add(1);
        }
        t6 = sum;
        t3 ^= d;
        let r = d.rotate_left(d & 0x1F);
        t5 = t5.wrapping_add(r);
        if t2 > d {
            t2 ^= r;
        } else {
            t2 ^= t6 ^ d;
        }
        t1 = t1.wrapping_add(t5 ^ d);
    }
    Some(match cic {
        Cic::C6103 => ((t6 ^ t4).wrapping_add(t3), (t5 ^ t2).wrapping_add(t1)),
        Cic::C6106 => (
            t6.wrapping_mul(t4).wrapping_add(t3),
            t5.wrapping_mul(t2).wrapping_add(t1),
        ),
        _ => (t6 ^ t4 ^ t3, t5 ^ t2 ^ t1),
    })
}
