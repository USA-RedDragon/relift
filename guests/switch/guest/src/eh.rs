//! Landing pads from the unwind tables: the `.eh_frame` entries the
//! `.eh_frame_hdr` search table lists, and the call-site tables of the
//! `.gcc_except_table` data they point at (the Itanium C++ ABI's LSDA).

use relift_guest::Pad;

struct Reader<'a> {
    img: &'a [u8],
    at: usize,
}

impl Reader<'_> {
    fn u8(&mut self) -> Option<u8> {
        let v = *self.img.get(self.at)?;
        self.at += 1;
        Some(v)
    }

    fn uleb(&mut self) -> Option<u64> {
        let (mut v, mut shift) = (0u64, 0u32);
        loop {
            let b = self.u8()?;
            v |= u64::from(b & 0x7f).checked_shl(shift)?;
            if b & 0x80 == 0 {
                return Some(v);
            }
            shift += 7;
        }
    }

    fn sleb(&mut self) -> Option<i64> {
        let (mut v, mut shift) = (0i64, 0u32);
        loop {
            let b = self.u8()?;
            v |= i64::from(b & 0x7f).checked_shl(shift)?;
            shift += 7;
            if b & 0x80 == 0 {
                if shift < 64 && b & 0x40 != 0 {
                    v |= -1i64 << shift;
                }
                return Some(v);
            }
        }
    }

    fn fixed(&mut self, n: usize) -> Option<u64> {
        let b = self.img.get(self.at..self.at.checked_add(n)?)?;
        self.at += n;
        let mut w = [0u8; 8];
        w[..n].copy_from_slice(b);
        Some(u64::from_le_bytes(w))
    }

    /// A value in DWARF pointer encoding `enc`, as an image offset; `None`
    /// for the omitted encoding and for encodings no toolchain emits here.
    fn encoded(&mut self, enc: u8, data_base: usize) -> Option<u64> {
        if enc == 0xff {
            return None;
        }
        let field = self.at as u64;
        let raw = match enc & 0x0f {
            0x00 | 0x04 | 0x0c => self.fixed(8)?,
            0x01 => self.uleb()?,
            0x02 => self.fixed(2)?,
            0x03 => self.fixed(4)?,
            0x09 => self.sleb()? as u64,
            0x0a => self.fixed(2)? as u16 as i16 as i64 as u64,
            0x0b => self.fixed(4)? as u32 as i32 as i64 as u64,
            _ => return None,
        };
        match enc & 0x70 {
            0x00 => Some(raw),
            0x10 => Some(field.wrapping_add(raw)),
            0x30 => Some((data_base as u64).wrapping_add(raw)),
            _ => None,
        }
    }
}

struct Cie {
    fde_enc: u8,
    lsda_enc: u8,
    has_lsda: bool,
    augmented: bool,
}

fn parse_cie(img: &[u8], at: usize, data_base: usize) -> Option<Cie> {
    let mut r = Reader { img, at };
    let len = r.fixed(4)? as usize;
    if len == 0 || len == 0xffff_ffff {
        return None;
    }
    let end = at.checked_add(4)?.checked_add(len)?;
    if r.fixed(4)? != 0 {
        return None;
    }
    let version = r.u8()?;
    let mut aug = Vec::new();
    loop {
        match r.u8()? {
            0 => break,
            b => aug.push(b),
        }
    }
    r.uleb()?;
    r.sleb()?;
    if version == 1 {
        r.u8()?;
    } else {
        r.uleb()?;
    }
    let mut cie = Cie {
        fde_enc: 0,
        lsda_enc: 0xff,
        has_lsda: false,
        augmented: aug.first() == Some(&b'z'),
    };
    if !cie.augmented {
        return Some(cie);
    }
    let _ = r.uleb()?;
    for &c in &aug[1..] {
        match c {
            b'R' => cie.fde_enc = r.u8()?,
            b'L' => {
                cie.lsda_enc = r.u8()?;
                cie.has_lsda = true;
            }
            b'P' => {
                let e = r.u8()?;
                r.encoded(e, data_base)?;
            }
            b'S' | b'B' => {}
            _ => return None,
        }
    }
    (r.at <= end).then_some(cie)
}

/// The landing pads of the module whose unwind table is at image offset
/// `start`, with call-site addresses and pads as guest addresses from
/// `base`. The table is either the `.eh_frame` records themselves (the
/// SDK's linker script puts the start symbol there) or an `.eh_frame_hdr`
/// (version 1, with an `.eh_frame` pointer of `DW_EH_PE_pcrel |
/// DW_EH_PE_sdata4`); the records are walked to their zero terminator. A
/// malformed entry gives no pads.
pub(crate) fn landing_pads(img: &[u8], start: usize, base: u64) -> Vec<Pad> {
    let mut out: Vec<Pad> = fdes(img, start)
        .into_iter()
        .flat_map(|at| fde_pads(img, at, start, base))
        .collect();
    out.sort();
    out.dedup();
    out
}

/// The function starts of the FDEs of the unwind table at image offset
/// `start` (laid out as for [`landing_pads`]), as image offsets.
pub(crate) fn fde_starts(img: &[u8], start: usize) -> Vec<u64> {
    fdes(img, start)
        .into_iter()
        .filter_map(|at| fde_start(img, at, start))
        .collect()
}

fn fdes(img: &[u8], start: usize) -> Vec<usize> {
    let word = |o: usize| -> Option<u32> {
        Some(u32::from_le_bytes(
            img.get(o..o.checked_add(4)?)?.try_into().ok()?,
        ))
    };
    let mut at = start;
    if img.get(start) == Some(&1) && img.get(start + 1) == Some(&0x1b) {
        let Some(rel) = word(start + 4) else {
            return Vec::new();
        };
        let Ok(frames) = usize::try_from((start + 4) as i64 + i64::from(rel as i32)) else {
            return Vec::new();
        };
        at = frames;
    }
    let mut out = Vec::new();
    while let Some(len) = word(at) {
        if len == 0 || len == 0xffff_ffff {
            break;
        }
        if word(at + 4).is_some_and(|id| id != 0) {
            out.push(at);
        }
        at = match at.checked_add(4 + len as usize) {
            Some(n) => n,
            None => break,
        };
    }
    out
}

fn fde_start(img: &[u8], at: usize, data_base: usize) -> Option<u64> {
    let mut r = Reader { img, at };
    r.fixed(4)?;
    let id_at = r.at;
    let id = r.fixed(4)?;
    let cie = parse_cie(img, id_at.checked_sub(id as usize)?, data_base)?;
    if !cie.augmented {
        return None;
    }
    r.encoded(cie.fde_enc, data_base)
}

fn fde_pads(img: &[u8], at: usize, data_base: usize, base: u64) -> Vec<Pad> {
    let mut r = Reader { img, at };
    let Some(len) = r.fixed(4) else {
        return Vec::new();
    };
    if len == 0 || len == 0xffff_ffff {
        return Vec::new();
    }
    let id_at = r.at;
    let Some(id) = r.fixed(4) else {
        return Vec::new();
    };
    let Some(cie_at) = id_at.checked_sub(id as usize) else {
        return Vec::new();
    };
    let Some(cie) = parse_cie(img, cie_at, data_base) else {
        return Vec::new();
    };
    if !cie.augmented || !cie.has_lsda {
        return Vec::new();
    }
    let (Some(start), Some(_range), Some(_alen)) = (
        r.encoded(cie.fde_enc, data_base),
        r.encoded(cie.fde_enc & 0x0f, data_base),
        r.uleb(),
    ) else {
        return Vec::new();
    };
    let field = r.at as u64;
    let Some(lsda) = r.encoded(cie.lsda_enc, data_base) else {
        return Vec::new();
    };
    let absent = lsda == 0 || (cie.lsda_enc & 0x70 == 0x10 && lsda == field);
    if absent {
        return Vec::new();
    }
    call_sites(img, lsda as usize, start, data_base)
        .unwrap_or_default()
        .into_iter()
        .map(|(site, pad)| Pad {
            site: base.wrapping_add(site),
            addr: base.wrapping_add(pad),
        })
        .collect()
}

fn call_sites(img: &[u8], lsda: usize, start: u64, data_base: usize) -> Option<Vec<(u64, u64)>> {
    let mut r = Reader { img, at: lsda };
    let lp_enc = r.u8()?;
    let lp_start = if lp_enc == 0xff {
        start
    } else {
        r.encoded(lp_enc, data_base)?
    };
    let tt_enc = r.u8()?;
    if tt_enc != 0xff {
        r.uleb()?;
    }
    let cs_enc = r.u8()?;
    let cs_len = r.uleb()? as usize;
    let end = r.at.checked_add(cs_len)?;
    let mut out = Vec::new();
    while r.at < end {
        let site = r.encoded(cs_enc & 0x0f, data_base)?;
        let _len = r.encoded(cs_enc & 0x0f, data_base)?;
        let pad = r.encoded(cs_enc & 0x0f, data_base)?;
        r.uleb()?;
        if pad != 0 {
            out.push((start.wrapping_add(site), lp_start.wrapping_add(pad)));
        }
    }
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn put32(v: &mut Vec<u8>, x: u32) {
        v.extend_from_slice(&x.to_le_bytes());
    }

    fn lsda(call_sites: &[(u8, u8, u8)]) -> Vec<u8> {
        let mut cs = Vec::new();
        for &(site, len, pad) in call_sites {
            cs.extend_from_slice(&[site, len, pad, 0]);
        }
        let mut l = vec![0xff, 0xff, 0x01, cs.len() as u8];
        l.extend(cs);
        l
    }

    /// A `zPLR` CIE at 0 and one FDE for a function at 0x1000 whose LSDA
    /// follows the records, with 1-byte uleb call-site fields.
    fn table(call_sites: &[(u8, u8, u8)]) -> Vec<u8> {
        let mut t = Vec::new();
        let mut cie = Vec::new();
        put32(&mut cie, 0);
        cie.extend_from_slice(&[
            1, b'z', b'P', b'L', b'R', 0, 4, 0x78, 30, 7, 0x9b, 0, 0, 0, 0, 0x1b, 0x1b,
        ]);
        while cie.len() % 4 != 0 {
            cie.push(0);
        }
        put32(&mut t, cie.len() as u32);
        t.extend(cie);
        let fde_at = t.len();
        let lsda_field = fde_at + 4 + 4 + 4 + 4 + 1;
        let lsda_at = fde_at + 4 + 20;
        let mut fde = Vec::new();
        put32(&mut fde, (fde_at + 4) as u32);
        put32(&mut fde, 0x1000u32.wrapping_sub((fde_at + 8) as u32));
        put32(&mut fde, 0x40);
        fde.push(4);
        put32(&mut fde, (lsda_at - lsda_field) as u32);
        fde.resize(20, 0);
        put32(&mut t, fde.len() as u32);
        t.extend(fde);
        t.extend(lsda(call_sites));
        put32(&mut t, 0);
        t
    }

    #[test]
    fn the_landing_pads_of_an_lsda_are_read() {
        let t = table(&[(0x10, 4, 0x30), (0x20, 8, 0), (0x24, 4, 0x34)]);
        let pads = landing_pads(&t, 0, 0x8000_0000);
        assert_eq!(
            pads,
            [
                Pad {
                    site: 0x8000_1010,
                    addr: 0x8000_1030
                },
                Pad {
                    site: 0x8000_1024,
                    addr: 0x8000_1034
                },
            ]
        );
    }

    #[test]
    fn a_zero_lsda_field_means_no_lsda() {
        let mut t = table(&[(0x10, 4, 0x30)]);
        let fde_at = 4 + u32::from_le_bytes(t[..4].try_into().unwrap()) as usize;
        t[fde_at + 17..fde_at + 21].fill(0);
        assert!(landing_pads(&t, 0, 0x8000_0000).is_empty());
        assert_eq!(fde_starts(&t, 0), [0x1000]);
    }

    #[test]
    fn the_function_starts_of_the_records_are_read() {
        let t = table(&[(0x10, 4, 0x30)]);
        assert_eq!(fde_starts(&t, 0), [0x1000]);
        assert!(fde_starts(&[0, 0, 0, 0], 0).is_empty());
        assert!(fde_starts(&[], 0).is_empty());
    }

    #[test]
    fn a_table_with_no_lsda_gives_no_pads() {
        assert!(landing_pads(&[0, 0, 0, 0], 0, 0).is_empty());
        assert!(landing_pads(&[], 0, 0).is_empty());
    }
}
