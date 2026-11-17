//! Task and mesh pipelines from a hand-built BNSH: the stage slots, the
//! GLSLC headers, the programs' SHA-256 and the primitive SPH.

use relift_switch_shaders::carve::{BRA_SELF, PROGRAM_MAGIC, carve};
use relift_switch_shaders::mesh::{bahs_pipelines, pipelines};

fn program(stage: u32, seed: u8) -> Vec<u8> {
    let mut v = vec![0u8; 0x80];
    let w0 = 1 | (4 << 5) | (stage << 10);
    v[..4].copy_from_slice(&w0.to_le_bytes());
    v[0x10..0x14].copy_from_slice(&0xff00_0000u32.to_le_bytes());
    for i in 0..3u8 {
        v.extend_from_slice(&[0x24, 0x78, seed, i, 0x11, 0x22, 0x33, 0x44]);
        v.extend_from_slice(&[seed ^ 0x5a, 1, 2, 3, 4, 5, 6, 7]);
    }
    v.extend_from_slice(&BRA_SELF);
    v.extend_from_slice(&[0, 0, 0xc0, 0x0f]);
    v
}

fn header(stage: u8, prim: Option<u32>) -> Vec<u8> {
    let mut h = vec![0u8; 0xe0];
    h[..4].copy_from_slice(&PROGRAM_MAGIC);
    h[0x56] = stage;
    h[0x58..0x5c].copy_from_slice(&0x0080_0020u32.to_le_bytes());
    if let Some(off) = prim {
        h[0x64..0x68].copy_from_slice(&off.to_le_bytes());
        h[0x68..0x6c].copy_from_slice(&0x80u32.to_le_bytes());
    }
    h
}

fn sha(b: &[u8]) -> String {
    relift_switch_formats::hex(&relift_switch_formats::sha256(b))
}

#[test]
fn a_bnsh_program_gives_its_task_and_mesh_pipeline() {
    let task = program(1, 1);
    let mesh = program(3, 2);
    let mut mesh_code = mesh.clone();
    mesh_code.resize(0x200, 0);
    let mut prim = vec![0u8; 0x80];
    prim[..4].copy_from_slice(&0x0100_1081u32.to_le_bytes());
    mesh_code.extend_from_slice(&prim);
    let mut b = vec![0u8; 0x100];
    b[..4].copy_from_slice(b"BNSH");
    let slots = 0x40;
    let th = b.len() as u64;
    b.extend(header(6, None));
    let mh = b.len() as u64;
    b.extend(header(7, Some(0x200)));
    let tc = b.len() as u64;
    b.extend(&task);
    let mc = b.len() as u64;
    b.extend(&mesh_code);
    for (k, (h, c, n)) in [(th, tc, task.len()), (mh, mc, mesh_code.len())]
        .into_iter()
        .enumerate()
    {
        let at = slots + 0x40 * k;
        b[at..at + 8].copy_from_slice(&h.to_le_bytes());
        b[at + 8..at + 16].copy_from_slice(&c.to_le_bytes());
        b[at + 16..at + 20].copy_from_slice(&(n as u32).to_le_bytes());
        b[at + 20..at + 24].copy_from_slice(&0xe0u32.to_le_bytes());
    }
    let len = b.len() as u32;
    b[0x1c..0x20].copy_from_slice(&len.to_le_bytes());
    let p = pipelines(&b);
    assert_eq!(p.len(), 1);
    assert_eq!(p[0]["task"], sha(&task));
    assert_eq!(p[0]["mesh"], sha(&mesh));
    assert!(p[0]["fragment"].is_null());
    assert_eq!(p[0]["mesh_glslc"].as_str().unwrap().len(), 2 * 0xe0);
    assert!(
        p[0]["primitive_sph"]
            .as_str()
            .unwrap()
            .starts_with("81100001")
    );
    let carved: Vec<String> = carve(&b).iter().map(|f| sha(&b[f.start..f.end])).collect();
    assert!(carved.contains(&sha(&task)) && carved.contains(&sha(&mesh)));
}

#[test]
fn a_bahs_archive_gives_its_task_and_mesh_pipeline() {
    let frag = program(5, 3);
    let task = program(1, 4);
    let mesh = program(3, 5);
    let mut b = vec![0u8; 0x600];
    b[..4].copy_from_slice(b"BAHS");
    let mut headers = Vec::new();
    for (k, (st, p)) in [(1u8, &frag), (6, &task), (7, &mesh)]
        .into_iter()
        .enumerate()
    {
        let mut h = header(st, None);
        h[0x38..0x3c].copy_from_slice(&(p.len() as u32).to_le_bytes());
        let at = 0x200 + 0x100 * k;
        b[at..at + 0xe0].copy_from_slice(&h);
        headers.push(at);
    }
    let code = b.len();
    b[0x1c..0x20].copy_from_slice(&(code as u32).to_le_bytes());
    let mut offs = Vec::new();
    for p in [&frag, &task, &mesh] {
        offs.push((b.len() - code) as u64);
        b.extend_from_slice(p);
        b.resize(b.len().next_multiple_of(0x100), 0);
    }
    for k in 0..3 {
        let r = 0x40 + 0x40 * k;
        b[r..r + 8].copy_from_slice(&offs[k].to_le_bytes());
        b[r + 8..r + 12].copy_from_slice(&0xd8u32.to_le_bytes());
        let rel = headers[k] as i32 - r as i32;
        b[r + 12..r + 16].copy_from_slice(&rel.to_le_bytes());
    }
    let p = bahs_pipelines(&b);
    assert_eq!(p.len(), 1);
    assert_eq!(p[0]["fragment"], sha(&frag));
    assert_eq!(p[0]["task"], sha(&task));
    assert_eq!(p[0]["mesh"], sha(&mesh));
}
