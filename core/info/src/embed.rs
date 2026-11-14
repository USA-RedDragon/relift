//! The C source that stores the document in a game binary and prints it.

use std::fmt::Write;

/// The ELF section, or the Mach-O section name in `__DATA`, that holds the
/// document.
pub const ELF_SECTION: &str = ".relift_info";
/// The Mach-O section (in segment `__DATA`).
pub const MACHO_SECTION: &str = "__relift_info";
/// The PE section.
pub const PE_SECTION: &str = ".rlinfo";

/// The C file of a binary: the document in its own section and
/// `int relift_info_main(void)`, which writes the document to stdout.
#[must_use]
pub fn c_source(doc: &[u8]) -> String {
    let mut t = String::new();
    t.push_str("#include <stdio.h>\n");
    let _ = write!(
        t,
        "#if defined(__APPLE__)\n#define RL_INFO_SECTION __attribute__((section(\"__DATA,{MACHO_SECTION}\")))\n#elif defined(_WIN32)\n#define RL_INFO_SECTION __attribute__((section(\"{PE_SECTION}\")))\n#else\n#define RL_INFO_SECTION __attribute__((section(\"{ELF_SECTION}\")))\n#endif\n"
    );
    let _ = writeln!(
        t,
        "RL_INFO_SECTION __attribute__((used)) static const unsigned char relift_info_bytes[{}] = {{",
        doc.len().max(1)
    );
    for line in doc.chunks(24) {
        t.push_str("   ");
        for b in line {
            let _ = write!(t, " {b},");
        }
        t.push('\n');
    }
    if doc.is_empty() {
        t.push_str("    0\n");
    }
    t.push_str("};\n");
    t.push_str(
        "int relift_info_main(void)\n{\n#ifdef _WIN32\n    _setmode(_fileno(stdout), 0x8000);\n#endif\n    size_t n = fwrite(relift_info_bytes, 1, sizeof relift_info_bytes, stdout);\n    return (n == sizeof relift_info_bytes && fflush(stdout) == 0) ? 0 : 1;\n}\n",
    );
    t
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_source_names_every_section_and_holds_the_bytes() {
        let c = c_source(b"{}\n");
        for s in [ELF_SECTION, MACHO_SECTION, PE_SECTION] {
            assert!(c.contains(s), "{s}");
        }
        assert!(c.contains("relift_info_bytes[3]"));
        assert!(c.contains(" 123, 125, 10,"));
    }
}
