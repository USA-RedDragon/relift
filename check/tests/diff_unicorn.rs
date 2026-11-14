//! The Unicorn engine of `relift diff` speaks the protocol: given a code
//! page, it runs a function and reports a match against the registers a
//! driver says the lifted code left, and a mismatch against wrong ones.

use std::io::{Read, Write};
use std::process::Stdio;

const MOVZ_X1_17: u32 = 0xd280_0221;
const ADD_X0_X0_X1: u32 = 0x8b01_0000;
const RET: u32 = 0xd65f_03c0;

fn drive(after: &str) -> (String, String) {
    let mut page = vec![0u8; 4096];
    for (i, w) in [MOVZ_X1_17, ADD_X0_X0_X1, RET].iter().enumerate() {
        page[i * 4..i * 4 + 4].copy_from_slice(&w.to_le_bytes());
    }
    let mut child = relift_proc::command(env!("CARGO_BIN_EXE_relift-diff-unicorn"))
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("the engine starts");
    let mut input = child.stdin.take().expect("stdin");
    let mut out = Vec::new();
    let script = [
        b"STATE pc=10000 stop=20000 x0=5 x30=20000 sp=40000\n".as_slice(),
        b"OK\n",
        &page,
        format!("{after}\n").as_bytes(),
        b"OK\n",
        &page,
    ]
    .concat();
    input.write_all(&script).expect("the engine reads");
    drop(input);
    child
        .stdout
        .take()
        .expect("stdout")
        .read_to_end(&mut out)
        .expect("stdout reads");
    let mut err = String::new();
    child
        .stderr
        .take()
        .expect("stderr")
        .read_to_string(&mut err)
        .expect("stderr reads");
    child.wait().expect("the engine ends");
    (String::from_utf8_lossy(&out).into_owned(), err)
}

#[test]
fn a_correct_result_matches() {
    let (out, err) = drive("AFTER x0=16 x1=11 sp=40000");
    assert!(out.contains("RESULT match"), "{out}\n{err}");
}

#[test]
fn a_wrong_register_is_a_mismatch() {
    let (out, err) = drive("AFTER x0=16 x1=1 sp=40000");
    assert!(out.contains("RESULT mismatch"), "{out}\n{err}");
    assert!(
        err.contains("register x1: lifted 0x1, engine 0x11"),
        "{err}"
    );
}
