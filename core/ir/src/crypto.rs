//! The meaning of each [`CryptoOp`], computed from the field arithmetic the
//! standards define (no copied tables).

use crate::CryptoOp;

/// `a * b` in GF(2^8) modulo the polynomial `p` (with its `x^8` bit).
const fn gf_mul(mut a: u16, mut b: u16, p: u16) -> u8 {
    let mut r = 0u16;
    while b != 0 {
        if b & 1 == 1 {
            r ^= a;
        }
        b >>= 1;
        a <<= 1;
        if a & 0x100 != 0 {
            a ^= p;
        }
    }
    r as u8
}

/// The multiplicative inverse in GF(2^8) modulo `p`, with 0 for 0.
const fn gf_inv(a: u8, p: u16) -> u8 {
    let mut r = 1u8;
    let mut i = 0;
    if a == 0 {
        return 0;
    }
    while i < 254 {
        r = gf_mul(r as u16, a as u16, p);
        i += 1;
    }
    r
}

const AES_POLY: u16 = 0x11b;
const SM4_POLY: u16 = 0x1f5;

const fn aes_sbox() -> [u8; 256] {
    let mut t = [0u8; 256];
    let mut x = 0;
    while x < 256 {
        let b = gf_inv(x as u8, AES_POLY);
        t[x] = b ^ b.rotate_left(1) ^ b.rotate_left(2) ^ b.rotate_left(3) ^ b.rotate_left(4) ^ 0x63;
        x += 1;
    }
    t
}

const fn invert(t: &[u8; 256]) -> [u8; 256] {
    let mut out = [0u8; 256];
    let mut x = 0;
    while x < 256 {
        out[t[x] as usize] = x as u8;
        x += 1;
    }
    out
}

/// The SM4 affine map: output bit `7 - i` is the parity of `x` and `0xd3`
/// rotated right by `i`, plus the constant `0xd3`.
const fn sm4_affine(x: u8) -> u8 {
    let mut o = 0u8;
    let mut i = 0;
    while i < 8 {
        let row = 0xd3u8.rotate_right(i);
        o |= (((row & x).count_ones() & 1) as u8) << (7 - i);
        i += 1;
    }
    o ^ 0xd3
}

const fn sm4_sbox() -> [u8; 256] {
    let mut t = [0u8; 256];
    let mut x = 0;
    while x < 256 {
        t[x] = sm4_affine(gf_inv(sm4_affine(x as u8), SM4_POLY));
        x += 1;
    }
    t
}

static AES_SBOX: [u8; 256] = aes_sbox();
static AES_INV_SBOX: [u8; 256] = invert(&AES_SBOX);
static SM4_SBOX: [u8; 256] = sm4_sbox();

fn bytes(x: u128, t: &[u8; 256]) -> u128 {
    let mut b = x.to_le_bytes();
    for v in &mut b {
        *v = t[usize::from(*v)];
    }
    u128::from_le_bytes(b)
}

fn mix(x: u128, m: [u16; 4]) -> u128 {
    let b = x.to_le_bytes();
    let mut o = [0u8; 16];
    for c in 0..4 {
        for r in 0..4 {
            let mut v = 0u8;
            for k in 0..4 {
                v ^= gf_mul(u16::from(b[4 * c + k]), m[(k + 4 - r) % 4], AES_POLY);
            }
            o[4 * c + r] = v;
        }
    }
    u128::from_le_bytes(o)
}

/// Evaluates `op` on `x`.
pub fn eval(op: CryptoOp, x: u128) -> u128 {
    match op {
        CryptoOp::AesSubBytes => bytes(x, &AES_SBOX),
        CryptoOp::AesInvSubBytes => bytes(x, &AES_INV_SBOX),
        CryptoOp::AesMixColumns => mix(x, [2, 3, 1, 1]),
        CryptoOp::AesInvMixColumns => mix(x, [14, 11, 13, 9]),
        CryptoOp::Sm4Sbox => u128::from(SM4_SBOX[(x & 0xff) as usize]),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn known_values() {
        assert_eq!(AES_SBOX[0x00], 0x63);
        assert_eq!(AES_SBOX[0x53], 0xed);
        assert_eq!(AES_INV_SBOX[0x63], 0x00);
        assert_eq!(SM4_SBOX[..4], [0xd6, 0x90, 0xe9, 0xfe]);
        assert_eq!(SM4_SBOX[0xff], 0x48);
        let col = u128::from(u32::from_le_bytes([0xdb, 0x13, 0x53, 0x45]));
        let out = eval(CryptoOp::AesMixColumns, col) as u32;
        assert_eq!(out.to_le_bytes(), [0x8e, 0x4d, 0xa1, 0xbc]);
        let back = eval(CryptoOp::AesInvMixColumns, u128::from(out));
        assert_eq!(back, col);
    }
}
