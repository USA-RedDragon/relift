//! C source that holds a binary file as a constant array.

use std::fmt::Write;

/// The C file that defines `const uint8_t <symbol>[]` with the contents of
/// `file` (the file `bytes` is written to beside it).
///
/// A compiler with `#embed` reads the file as data. Any other compiler gets
/// the same array from an assembler `.incbin`, which needs the directory of
/// `file` as an include directory and which dependency files do not track,
/// so the text carries a hash of `bytes` to change with them.
#[must_use]
pub fn blob_c(symbol: &str, file: &str, bytes: &[u8]) -> String {
    let mut hash: u64 = 0xcbf2_9ce4_8422_2325;
    for b in bytes {
        hash = (hash ^ u64::from(*b)).wrapping_mul(0x0100_0000_01b3);
    }
    let mut t = String::new();
    let _ = write!(
        t,
        "#include <stdint.h>\n\
#if defined(__has_embed)\n\
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
#endif\n"
    );
    t
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
    fn the_source_changes_with_the_bytes() {
        assert_ne!(blob_c("a", "a.bin", &[1]), blob_c("a", "a.bin", &[2]));
        assert_eq!(blob_c("a", "a.bin", &[1]), blob_c("a", "a.bin", &[1]));
    }
}
