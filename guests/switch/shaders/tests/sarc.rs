//! SARC name tables over crafted archives: a name must end inside the name
//! table and within the name limit.

use relift_switch_shaders::sarc::{self, MAX_NAME};

fn archive(count: u16, names: &[u8]) -> Vec<u8> {
    let header = 0x14usize;
    let data = header + 12 + usize::from(count) * 16 + 8 + names.len();
    let mut out = Vec::new();
    out.extend_from_slice(b"SARC");
    out.extend_from_slice(&(header as u16).to_le_bytes());
    out.extend_from_slice(&[0xFF, 0xFE]);
    out.extend_from_slice(&(data as u32 + 4).to_le_bytes());
    out.extend_from_slice(&(data as u32).to_le_bytes());
    out.extend_from_slice(&0x100u32.to_le_bytes());
    out.extend_from_slice(b"SFAT");
    out.extend_from_slice(&12u16.to_le_bytes());
    out.extend_from_slice(&count.to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes());
    for i in 0..u32::from(count) {
        out.extend_from_slice(&i.to_le_bytes());
        out.extend_from_slice(&0x0100_0000u32.to_le_bytes());
        out.extend_from_slice(&0u32.to_le_bytes());
        out.extend_from_slice(&4u32.to_le_bytes());
    }
    out.extend_from_slice(b"SFNT");
    out.extend_from_slice(&8u16.to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(names);
    out.extend_from_slice(&[0, 0, 0, 0]);
    out
}

#[test]
fn shared_names_resolve() {
    let a = archive(1000, b"same\0\0\0\0");
    let m = sarc::members(&a).expect("parses");
    assert_eq!(m.len(), 1000);
    assert!(m.iter().all(|(n, d)| n == "same" && d.len() == 4));
}

#[test]
fn a_name_without_a_terminator_in_the_table_is_an_error() {
    let a = archive(64, &[b'x'; 0x1000]);
    assert!(sarc::members(&a).is_err());
}

#[test]
fn a_name_past_the_limit_is_an_error() {
    let mut names = vec![b'y'; MAX_NAME + 1];
    names.extend_from_slice(&[0, 0, 0]);
    assert!(sarc::members(&archive(4, &names)).is_err());
    let mut names = vec![b'y'; MAX_NAME];
    names.extend_from_slice(&[0, 0, 0, 0]);
    let a = archive(4, &names);
    assert_eq!(sarc::members(&a).expect("parses")[0].0.len(), MAX_NAME);
}
