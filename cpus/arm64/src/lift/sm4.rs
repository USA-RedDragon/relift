//! The SM4 S-box (GB/T 32907) as the intrinsic the generated templates of
//! `SM4E` and `SM4EKEY` call, once per byte. The table is computed from the
//! field arithmetic the standard defines, so the generator can check it
//! against the Sail model's table.
//!
//! The runtime's `rl_sm4_sbox` is the C side (docs/abi.md section 10).

/// Intrinsic: the SM4 S-box of the low byte of the operand, 8 bits.
pub const SBOX: &str = "arm64.sm4_sbox";

/// The SM4 field polynomial `x^8 + x^7 + x^6 + x^5 + x^4 + x^2 + 1`.
const POLY: u16 = 0x1f5;

/// `a * b` in GF(2^8) modulo [`POLY`].
const fn gf_mul(mut a: u16, mut b: u16) -> u8 {
    let mut r = 0u16;
    while b != 0 {
        if b & 1 == 1 {
            r ^= a;
        }
        b >>= 1;
        a <<= 1;
        if a & 0x100 != 0 {
            a ^= POLY;
        }
    }
    r as u8
}

/// The multiplicative inverse in GF(2^8) modulo [`POLY`], with 0 for 0.
const fn gf_inv(a: u8) -> u8 {
    let mut r = 1u8;
    let mut i = 0;
    if a == 0 {
        return 0;
    }
    while i < 254 {
        r = gf_mul(r as u16, a as u16);
        i += 1;
    }
    r
}

/// The SM4 affine map: output bit `7 - i` is the parity of `x` and `0xd3`
/// rotated right by `i`, plus the constant `0xd3`.
const fn affine(x: u8) -> u8 {
    let mut o = 0u8;
    let mut i = 0;
    while i < 8 {
        let row = 0xd3u8.rotate_right(i);
        o |= (((row & x).count_ones() & 1) as u8) << (7 - i);
        i += 1;
    }
    o ^ 0xd3
}

const fn table() -> [u8; 256] {
    let mut t = [0u8; 256];
    let mut x = 0;
    while x < 256 {
        t[x] = affine(gf_inv(affine(x as u8)));
        x += 1;
    }
    t
}

static TABLE: [u8; 256] = table();

/// The SM4 S-box of `x`.
pub fn sbox(x: u8) -> u8 {
    TABLE[usize::from(x)]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn known_values() {
        assert_eq!(TABLE[..4], [0xd6, 0x90, 0xe9, 0xfe]);
        assert_eq!(sbox(0xff), 0x48);
    }
}
