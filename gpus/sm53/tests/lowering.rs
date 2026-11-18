use relift_gpu_sm53::form::ENCODINGS;
use relift_gpu_sm53::lower::table::{Row, forms, markdown, row};

const PATH: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/LOWERING.md");

#[test]
fn lowering_md_is_current() {
    let want = markdown();
    if std::env::var_os("RELIFT_BLESS").is_some() {
        relift_cache::write_if_changed(std::path::Path::new(PATH), want.as_bytes())
            .expect("LOWERING.md is writable");
    }
    let have = std::fs::read_to_string(PATH).unwrap_or_default();
    assert!(
        have == want,
        "gpus/sm53/LOWERING.md is stale; run RELIFT_BLESS=1 cargo test -p relift-gpu-sm53 --test lowering"
    );
}

#[test]
fn every_encoding_has_a_row_with_its_reason() {
    for e in &ENCODINGS {
        assert!(forms().contains(&(e.op, e.src)));
        match row(e.op, e.src) {
            Row::Exact { to, evidence } => assert!(!to.is_empty() && !evidence.is_empty()),
            Row::Pseudo { name, evidence } => assert!(!name.is_empty() && !evidence.is_empty()),
            Row::Stop { reason } => assert!(!reason.is_empty()),
        }
    }
}

#[test]
fn a_program_names_each_form_that_stops_once() {
    let mut b = 0x1234_5678u32.to_le_bytes().to_vec();
    b.resize(0x30, 0);
    let mov = 0x0100_0000_0007_0000u64;
    let exit = 0xe300_0000_0007_000fu64;
    for w in [0u64, mov, mov, exit] {
        b.extend(w.to_le_bytes());
    }
    let p = relift_gpu_sm53::Program::parse(&b).expect("parses");
    let s = relift_gpu_sm53::lower::stops(&p);
    let names: Vec<(&str, u64)> = s.iter().map(|x| (x.op.name(), x.at)).collect();
    assert_eq!(names, [("MOV", 8), ("EXIT", 24)]);
    assert!(s.iter().all(|x| x.reason == "no semantic check yet"));
}
