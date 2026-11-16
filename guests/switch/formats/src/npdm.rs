//! NPDM (`main.npdm`): the program's process parameters and the services it
//! may use.

use crate::bytes::{slice, u8_at, u32_at, u64_at};
use crate::error::{Result, malformed};

/// The process address space an NPDM asks for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AddressSpace {
    /// 32-bit.
    As32Bit,
    /// 36-bit (old 64-bit layout).
    As64BitOld,
    /// 32-bit without the reserved region.
    As32BitNoReserved,
    /// 39-bit.
    As64Bit,
}

impl AddressSpace {
    /// A lowercase name.
    pub fn name(self) -> &'static str {
        match self {
            AddressSpace::As32Bit => "32-bit",
            AddressSpace::As64BitOld => "36-bit",
            AddressSpace::As32BitNoReserved => "32-bit-no-reserved",
            AddressSpace::As64Bit => "39-bit",
        }
    }
}

/// One service-access entry.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Service {
    /// The service name (may end in `*` as a wildcard).
    pub name: String,
    /// Whether the program hosts it rather than uses it.
    pub is_server: bool,
}

/// A parsed NPDM.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Npdm {
    /// The program name in the header.
    pub name: String,
    /// The product code.
    pub product_code: String,
    /// Whether the program is 64-bit code.
    pub is_64bit: bool,
    /// The address space type.
    pub address_space: AddressSpace,
    /// The main thread priority.
    pub main_thread_priority: u8,
    /// The main thread's default core.
    pub main_thread_core: u8,
    /// The main thread stack size.
    pub main_thread_stack_size: u32,
    /// The system resource size.
    pub system_resource_size: u32,
    /// The NPDM version.
    pub version: u32,
    /// The program ID in ACI0.
    pub program_id: u64,
    /// The services the program may use or host, in file order.
    pub services: Vec<Service>,
    /// The supervisor calls the ACI0 kernel capabilities allow.
    pub svcs: SvcMask,
    /// What the ACID, the signed bounds of the ACI0, allows, when the
    /// NPDM has one.
    pub acid: Option<Acid>,
}

/// The ACID's bounds on the ACI0.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Acid {
    /// The services it allows.
    pub services: Vec<Service>,
    /// The supervisor calls it allows.
    pub svcs: SvcMask,
}

/// A set of supervisor call numbers, from the kernel capabilities' svc
/// masks.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SvcMask([u64; 3]);

impl SvcMask {
    /// Whether `svc` is in the set.
    pub fn contains(self, svc: u32) -> bool {
        let i = svc as usize;
        i < 192 && self.0[i / 64] >> (i % 64) & 1 != 0
    }

    /// The calls in both sets.
    pub fn and(self, o: SvcMask) -> SvcMask {
        SvcMask([self.0[0] & o.0[0], self.0[1] & o.0[1], self.0[2] & o.0[2]])
    }

    /// The calls in the set, in order.
    pub fn iter(self) -> impl Iterator<Item = u32> {
        (0..192u32).filter(move |&s| self.contains(s))
    }

    fn insert(&mut self, svc: u32) {
        let i = svc as usize;
        if i < 192 {
            self.0[i / 64] |= 1 << (i % 64);
        }
    }

    /// The svc masks of the kernel capability descriptors `kac`.
    fn of(kac: &[u8]) -> SvcMask {
        let mut m = SvcMask::default();
        for d in kac.as_chunks::<4>().0 {
            let d = u32::from_le_bytes(*d);
            if d.trailing_ones() != 4 {
                continue;
            }
            let base = (d >> 29) * 24;
            for bit in 0..24 {
                if d >> (5 + bit) & 1 != 0 {
                    m.insert(base + bit);
                }
            }
        }
        m
    }
}

fn text(b: &[u8]) -> String {
    let end = b.iter().position(|&c| c == 0).unwrap_or(b.len());
    String::from_utf8_lossy(&b[..end]).into_owned()
}

impl Npdm {
    /// Parse an NPDM.
    ///
    /// # Errors
    ///
    /// Bad META or ACI0 magic, or a region past the end.
    pub fn parse(b: &[u8]) -> Result<Npdm> {
        if b.get(..4) != Some(b"META") {
            return malformed("NPDM: bad META magic");
        }
        let flags = u8_at(b, 0xC, "META")?;
        let address_space = match (flags >> 1) & 7 {
            0 => AddressSpace::As32Bit,
            1 => AddressSpace::As64BitOld,
            2 => AddressSpace::As32BitNoReserved,
            3 => AddressSpace::As64Bit,
            t => return malformed(format!("NPDM: address space type {t}")),
        };
        let aci_off = u32_at(b, 0x70, "META")? as usize;
        let aci_size = u32_at(b, 0x74, "META")? as usize;
        let aci = slice(b, aci_off, aci_size, "ACI0")?;
        if aci.get(..4) != Some(b"ACI0") {
            return malformed("NPDM: bad ACI0 magic");
        }
        let services = access_list(aci, 0x28, "ACI0")?;
        let svcs = kernel_caps(aci, 0x30, "ACI0")?;
        let acid_off = u32_at(b, 0x78, "META")? as usize;
        let acid_size = u32_at(b, 0x7C, "META")? as usize;
        let acid = if acid_size == 0 {
            None
        } else {
            let acid = slice(b, acid_off, acid_size, "ACID")?;
            let Some(head) = [0x200, 0x300]
                .into_iter()
                .find(|&h| acid.get(h..h + 4) == Some(b"ACID"))
            else {
                return malformed("NPDM: bad ACID magic");
            };
            Some(Acid {
                services: access_list(acid, head + 0x28, "ACID")?,
                svcs: kernel_caps(acid, head + 0x30, "ACID")?,
            })
        };
        Ok(Npdm {
            name: text(slice(b, 0x20, 0x10, "META")?),
            product_code: text(slice(b, 0x30, 0x10, "META")?),
            is_64bit: flags & 1 != 0,
            address_space,
            main_thread_priority: u8_at(b, 0xE, "META")?,
            main_thread_core: u8_at(b, 0xF, "META")?,
            main_thread_stack_size: u32_at(b, 0x1C, "META")?,
            system_resource_size: u32_at(b, 0x14, "META")?,
            version: u32_at(b, 0x18, "META")?,
            program_id: u64_at(aci, 0x10, "ACI0")?,
            services,
            svcs,
            acid,
        })
    }

    /// The supervisor calls the program may make: those both the ACI0 and
    /// the ACID allow (the ACI0 alone without an ACID).
    pub fn effective_svcs(&self) -> SvcMask {
        self.acid
            .as_ref()
            .map_or(self.svcs, |a| self.svcs.and(a.svcs))
    }

    /// The ACI0 entry that lets the program use service `name`, when the
    /// ACID (if any) allows it too.
    pub fn uses_service(&self, name: &str) -> Option<&Service> {
        let hit = |list: &[Service]| {
            list.iter()
                .position(|s| !s.is_server && service_matches(&s.name, name))
        };
        let i = hit(&self.services)?;
        if let Some(a) = &self.acid
            && hit(&a.services).is_none()
        {
            return None;
        }
        self.services.get(i)
    }
}

/// Whether the access entry `pattern` (a name, or a prefix ending in `*`)
/// covers service `name`.
fn service_matches(pattern: &str, name: &str) -> bool {
    match pattern.strip_suffix('*') {
        Some(prefix) => name.starts_with(prefix),
        None => pattern == name,
    }
}

/// The service access list whose offset and size sit at `at` in `region`.
fn access_list(region: &[u8], at: usize, what: &str) -> Result<Vec<Service>> {
    let off = u32_at(region, at, what)? as usize;
    let size = u32_at(region, at + 4, what)? as usize;
    let sac = slice(region, off, size, "service access control")?;
    let mut out = Vec::new();
    let mut i = 0;
    while i < sac.len() {
        let control = sac[i];
        if control == 0 {
            break;
        }
        let len = usize::from(control & 7) + 1;
        let name = slice(sac, i + 1, len, "service name")?;
        out.push(Service {
            name: String::from_utf8_lossy(name).into_owned(),
            is_server: control & 0x80 != 0,
        });
        i += 1 + len;
    }
    Ok(out)
}

/// The svcs of the kernel capabilities whose offset and size sit at `at`
/// in `region`.
fn kernel_caps(region: &[u8], at: usize, what: &str) -> Result<SvcMask> {
    let off = u32_at(region, at, what)? as usize;
    let size = u32_at(region, at + 4, what)? as usize;
    Ok(SvcMask::of(slice(
        region,
        off,
        size,
        "kernel capabilities",
    )?))
}

/// Build an NPDM (fixtures).
pub fn build(name: &str, program_id: u64, stack: u32, services: &[(&str, bool)]) -> Vec<u8> {
    build_with(name, program_id, stack, services, None)
}

/// As [`build`], with ACI0 kernel capabilities allowing the supervisor
/// calls `svcs` and an ACID that allows the same services and the calls
/// `acid_svcs`, when given.
pub fn build_with(
    name: &str,
    program_id: u64,
    stack: u32,
    services: &[(&str, bool)],
    caps: Option<(&[u32], &[u32])>,
) -> Vec<u8> {
    let mut sac = Vec::new();
    for (s, server) in services {
        let len = s.len().clamp(1, 8);
        sac.push(u8::try_from(len - 1).unwrap_or(0) | if *server { 0x80 } else { 0 });
        sac.extend_from_slice(&s.as_bytes()[..len]);
    }
    let len32 = |n: usize| u32::try_from(n).unwrap_or(0).to_le_bytes();
    let (aci_svcs, acid_svcs) = caps.unwrap_or((&[], &[]));
    let kac = svc_caps(aci_svcs);
    let mut aci = vec![0u8; 0x40];
    aci[..4].copy_from_slice(b"ACI0");
    aci[0x10..0x18].copy_from_slice(&program_id.to_le_bytes());
    aci[0x28..0x2C].copy_from_slice(&0x40u32.to_le_bytes());
    aci[0x2C..0x30].copy_from_slice(&len32(sac.len()));
    aci[0x30..0x34].copy_from_slice(&len32(0x40 + sac.len()));
    aci[0x34..0x38].copy_from_slice(&len32(kac.len()));
    aci.extend_from_slice(&sac);
    aci.extend_from_slice(&kac);
    let mut acid = Vec::new();
    if caps.is_some() {
        let kac = svc_caps(acid_svcs);
        acid = vec![0u8; 0x240];
        acid[0x200..0x204].copy_from_slice(b"ACID");
        acid[0x228..0x22C].copy_from_slice(&0x240u32.to_le_bytes());
        acid[0x22C..0x230].copy_from_slice(&len32(sac.len()));
        acid[0x230..0x234].copy_from_slice(&len32(0x240 + sac.len()));
        acid[0x234..0x238].copy_from_slice(&len32(kac.len()));
        acid.extend_from_slice(&sac);
        acid.extend_from_slice(&kac);
    }
    let mut m = vec![0u8; 0x80];
    m[..4].copy_from_slice(b"META");
    m[0xC] = 1 | (3 << 1);
    m[0xE] = 44;
    m[0x1C..0x20].copy_from_slice(&stack.to_le_bytes());
    let n = name.len().min(0x10);
    m[0x20..0x20 + n].copy_from_slice(&name.as_bytes()[..n]);
    m[0x70..0x74].copy_from_slice(&0x80u32.to_le_bytes());
    m[0x74..0x78].copy_from_slice(&len32(aci.len()));
    if !acid.is_empty() {
        m[0x78..0x7C].copy_from_slice(&len32(0x80 + aci.len()));
        m[0x7C..0x80].copy_from_slice(&len32(acid.len()));
    }
    m.extend_from_slice(&aci);
    m.extend_from_slice(&acid);
    m
}

/// Kernel capability descriptors whose svc masks allow `svcs`.
fn svc_caps(svcs: &[u32]) -> Vec<u8> {
    let mut masks = [0u32; 8];
    for &s in svcs.iter().filter(|&&s| s < 192) {
        masks[(s / 24) as usize] |= 1 << (s % 24);
    }
    let mut out = Vec::new();
    for (i, m) in (0u32..).zip(masks) {
        if m != 0 {
            out.extend_from_slice(&((i << 29) | (m << 5) | 0xF).to_le_bytes());
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn effective_svcs_are_those_both_aci0_and_acid_allow() {
        let b = build_with(
            "g",
            1,
            0x1000,
            &[("ldr:ro", false), ("fsp-srv", false)],
            Some((&[0x01, 0x4B, 0x4C, 0x5F], &[0x01, 0x4B, 0x5F, 0x77])),
        );
        let n = Npdm::parse(&b).expect("parses");
        assert!(n.svcs.contains(0x4C));
        let e: Vec<u32> = n.effective_svcs().iter().collect();
        assert_eq!(e, vec![0x01, 0x4B, 0x5F]);
        assert!(n.uses_service("ldr:ro").is_some());
        assert!(n.uses_service("jit:u").is_none());
    }

    #[test]
    fn a_wildcard_entry_covers_the_services_it_prefixes() {
        let b = build_with("g", 1, 0x1000, &[("ldr:*", false)], Some((&[], &[])));
        let n = Npdm::parse(&b).expect("parses");
        assert_eq!(
            n.uses_service("ldr:ro").map(|s| s.name.as_str()),
            Some("ldr:*")
        );
        assert!(n.uses_service("jit:u").is_none());
    }

    #[test]
    fn a_switch2_acid_has_its_header_after_three_signature_blocks() {
        let b = build_with(
            "g",
            1,
            0x1000,
            &[("fsp-srv", false)],
            Some((&[0x01, 0x80], &[0x01, 0x80])),
        );
        let word = |b: &[u8], at: usize| u32::from_le_bytes(b[at..at + 4].try_into().unwrap());
        let (off, size) = (word(&b, 0x78) as usize, word(&b, 0x7C) as usize);
        let mut acid = b[off..off + size].to_vec();
        acid.splice(0x200..0x200, [0u8; 0x100]);
        for at in [0x320, 0x328, 0x330] {
            let v = word(&acid, at) + 0x100;
            acid[at..at + 4].copy_from_slice(&v.to_le_bytes());
        }
        let mut s2 = b.clone();
        s2[0x78..0x7C].copy_from_slice(&(b.len() as u32).to_le_bytes());
        s2[0x7C..0x80].copy_from_slice(&(acid.len() as u32).to_le_bytes());
        s2.extend_from_slice(&acid);
        let n = Npdm::parse(&s2).expect("parses");
        let e: Vec<u32> = n.effective_svcs().iter().collect();
        assert_eq!(e, vec![0x01, 0x80]);
        assert!(n.uses_service("fsp-srv").is_some());
    }

    #[test]
    fn without_an_acid_the_aci0_decides() {
        let n = Npdm::parse(&build("g", 1, 0x1000, &[("jit:u", false)])).expect("parses");
        assert!(n.acid.is_none());
        assert!(n.uses_service("jit:u").is_some());
        assert_eq!(n.effective_svcs().iter().count(), 0);
    }
}
