//! C source that holds a binary file as a constant array.

use std::fmt::Write;

/// The C file that defines `const uint8_t <symbol>[]` with the contents of
/// `file` (the file `bytes` is written to beside it).
///
/// A compiler with `#embed` reads the file as data. Any other compiler gets
/// the same array from an assembler `.incbin`, which dependency files do not
/// track, so the text carries a hash of `bytes` to change with them. `file`
/// is relative to the build directory, which the compile has as an include
/// directory (a path relative to the including file would be recorded in
/// the dependency file as an absolute one).
///
/// Empty data, and data that begins with a byte order mark (clang reads such
/// an `#embed` file as text and refuses a UTF-16 or UTF-32 one, which
/// little-endian ARM code that starts `fe ff` is) always goes through
/// `.incbin`.
#[must_use]
pub fn blob_c(symbol: &str, file: &str, bytes: &[u8]) -> String {
    let embed = !bytes.is_empty() && !starts_with_byte_order_mark(bytes);
    let mut hash: u64 = 0xcbf2_9ce4_8422_2325;
    for b in bytes {
        hash = (hash ^ u64::from(*b)).wrapping_mul(0x0100_0000_01b3);
    }
    let mut t = String::new();
    let _ = write!(
        t,
        "#include <stdint.h>\n\
#undef RL_BLOB_EMBED\n\
#undef RL_BLOB_SYMBOL\n\
#undef RL_BLOB_SECTION\n\
#if defined(__has_embed) && {}\n\
#if __has_embed(\"{file}\")\n\
#define RL_BLOB_EMBED 1\n\
#endif\n\
#endif\n\
#ifdef RL_BLOB_EMBED\n\
#pragma clang diagnostic ignored \"-Wc23-extensions\"\n\
extern const uint8_t {symbol}[];\n\
const uint8_t {symbol}[] = {{\n\
#embed \"{file}\"\n\
}};\n\
#else\n\
#if defined(__APPLE__)\n\
#define RL_BLOB_SYMBOL \"_{symbol}\"\n\
#define RL_BLOB_SECTION \".pushsection __DATA,__const\\n\"\n\
#elif defined(_WIN32)\n\
#define RL_BLOB_SYMBOL \"{symbol}\"\n\
#define RL_BLOB_SECTION \".pushsection .rdata,\\\"dr\\\"\\n\"\n\
#else\n\
#define RL_BLOB_SYMBOL \"{symbol}\"\n\
#define RL_BLOB_SECTION \".pushsection .rodata\\n\"\n\
#endif\n\
/* fnv1a64 {hash:016x} */\n\
extern const uint8_t {symbol}[];\n\
__asm__(RL_BLOB_SECTION \".balign 16\\n.globl \" RL_BLOB_SYMBOL \"\\n\" RL_BLOB_SYMBOL \":\\n.incbin \\\"{file}\\\"\\n.popsection\\n\");\n\
#endif\n",
        u8::from(embed)
    );
    t
}

fn starts_with_byte_order_mark(bytes: &[u8]) -> bool {
    [
        &[0xEF, 0xBB, 0xBF][..],
        &[0xFE, 0xFF],
        &[0xFF, 0xFE],
        &[0x00, 0x00, 0xFE, 0xFF],
    ]
    .iter()
    .any(|m| bytes.starts_with(m))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_source_embeds_when_it_can_and_includes_the_binary_otherwise() {
        let c = blob_c("rl_x", "x.bin", &[1, 2, 3]);
        assert!(c.contains("#embed \"x.bin\""));
        assert!(c.contains("const uint8_t rl_x[] = {"));
        assert!(c.contains(".incbin \\\"x.bin\\\""));
        assert!(c.contains("\"_rl_x\""));
    }

    #[test]
    fn data_that_starts_like_a_byte_order_mark_is_never_embedded_as_text() {
        assert!(blob_c("rl_x", "x.bin", &[]).contains("#if defined(__has_embed) && 0"));
        for head in [&[0xFE, 0xFF][..], &[0xFF, 0xFE], &[0xEF, 0xBB, 0xBF]] {
            let c = blob_c("rl_x", "x.bin", head);
            assert!(c.contains("#if defined(__has_embed) && 0"), "{c}");
        }
        assert!(blob_c("rl_x", "x.bin", &[1, 2]).contains("#if defined(__has_embed) && 1"));
    }

    #[test]
    fn blobs_in_one_file_do_not_share_the_embed_decision() {
        let c = blob_c("rl_a", "a.bin", &[1]) + &blob_c("rl_b", "b.bin", &[0xFE, 0xFF]);
        let second = c.rfind("#undef RL_BLOB_EMBED").unwrap();
        assert!(second > c.find("#define RL_BLOB_EMBED 1").unwrap());
    }

    #[test]
    fn the_source_changes_with_the_bytes() {
        assert_ne!(blob_c("a", "a.bin", &[1]), blob_c("a", "a.bin", &[2]));
        assert_eq!(blob_c("a", "a.bin", &[1]), blob_c("a", "a.bin", &[1]));
    }
}
