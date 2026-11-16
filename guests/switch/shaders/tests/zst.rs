//! zstd frames over the output cap are errors, never cut short, whether
//! the frame header declares its size or not.

use std::io::Write;

use relift_switch_shaders::zst::{Dicts, decode_limited};

fn with_size(b: &[u8]) -> Vec<u8> {
    zstd::bulk::compress(b, 3).expect("zstd compresses")
}

fn without_size(b: &[u8]) -> Vec<u8> {
    let mut e = zstd::stream::write::Encoder::new(Vec::new(), 3).expect("an encoder");
    e.include_contentsize(false).expect("the option sets");
    e.write_all(b).expect("writes");
    let v = e.finish().expect("finishes");
    assert_eq!(
        zstd::zstd_safe::get_frame_content_size(&v).ok(),
        Some(None),
        "the frame declares no size"
    );
    v
}

#[test]
fn a_frame_at_the_cap_decodes_whole() {
    let b = vec![7u8; 4096];
    for f in [with_size(&b), without_size(&b)] {
        assert_eq!(decode_limited(&f, &Dicts::new(), 4096).expect("decodes"), b);
    }
}

#[test]
fn a_frame_past_the_cap_is_an_error() {
    let b: Vec<u8> = (0..4097u32).map(|i| (i * 31 % 251) as u8).collect();
    for f in [with_size(&b), without_size(&b)] {
        let e = decode_limited(&f, &Dicts::new(), 4096).expect_err("over the cap");
        assert!(e.to_string().contains("more than 4096 bytes"), "{e}");
    }
}
