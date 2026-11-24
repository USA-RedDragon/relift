//! The C source that stores the document in a game binary and prints it.

use std::fmt::Write;

/// The ELF section, or the Mach-O section name in `__DATA`, that holds the
/// document.
pub const ELF_SECTION: &str = ".relift_info";
/// The Mach-O section (in segment `__DATA`).
pub const MACHO_SECTION: &str = "__relift_info";
/// The PE section.
pub const PE_SECTION: &str = ".rlinfo";

fn bytes_rows(t: &mut String, bytes: &[u8]) {
    for line in bytes.chunks(24) {
        t.push_str("   ");
        for b in line {
            let _ = write!(t, " {b},");
        }
        t.push('\n');
    }
    if bytes.is_empty() {
        t.push_str("    0\n");
    }
}

fn c_string(s: &str) -> String {
    let mut t = String::from("\"");
    for b in s.bytes() {
        if b.is_ascii_alphanumeric() || b == b' ' || b".,:;-_+()[]!'/".contains(&b) {
            t.push(char::from(b));
        } else {
            let _ = write!(t, "\\{b:03o}");
        }
    }
    t.push('"');
    t
}

/// Where the base64 of the icon sits in the document, when it has one.
fn icon_span(doc: &[u8], info: &crate::Info) -> Option<(usize, usize)> {
    let data = info.identity.icon.as_ref()?.data.as_bytes();
    if data.is_empty() {
        return None;
    }
    let at = doc.windows(data.len()).position(|w| w == data)?;
    Some((at, at + data.len()))
}

/// The titles of the document in fallback order: [`crate::LANGUAGES`], then
/// `default`, then any other key.
fn titles(info: &crate::Info) -> Vec<(&str, &str)> {
    let n = &info.identity.names;
    let mut out: Vec<(&str, &str)> = crate::LANGUAGES
        .iter()
        .chain(["default"].iter())
        .filter_map(|k| n.get_key_value(*k))
        .map(|(k, v)| (k.as_str(), v.as_str()))
        .collect();
    for (k, v) in n {
        if !out.iter().any(|(o, _)| o == k) {
            out.push((k, v));
        }
    }
    out
}

/// The `build:` line of a binary's stop reports: a 64-bit FNV-1a digest of
/// its embedded info document, as 16 hex digits. The document holds the
/// relift version, the game's identity and module build ids, the code mods
/// and the settings schema, so two builds of one game with another relift
/// or other mods differ here; the guest images themselves are the report's
/// `image:` line.
#[must_use]
pub fn build_digest(doc: &[u8]) -> String {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    for b in doc {
        h = (h ^ u64::from(*b)).wrapping_mul(0x0100_0000_01b3);
    }
    format!("{h:016x}")
}

/// The header lines the binary gives every stop report (`rl_report_info`):
/// the relift that built it, the game, the code mods built in and a digest
/// of the whole document, which tells two builds apart.
fn report_lines(info: Option<&crate::Info>, doc: &[u8]) -> Vec<(String, String)> {
    let mut out = Vec::new();
    if let Some(i) = info {
        out.push(("relift".to_string(), i.relift.version.clone()));
        let id = &i.identity;
        let mut game = id.platform.clone();
        if let Some(t) = &id.title_id {
            game.push(' ');
            game.push_str(t);
        }
        if let Some(v) = &id.version {
            game.push_str(" version ");
            game.push_str(v);
        }
        out.push(("game".to_string(), game));
        let code: Vec<String> = i
            .mods
            .code
            .iter()
            .map(|m| format!("{} {}", m.name, m.hash))
            .collect();
        out.push((
            "code mods".to_string(),
            if code.is_empty() {
                "none".to_string()
            } else {
                code.join(", ")
            },
        ));
    }
    out.push(("build".to_string(), build_digest(doc)));
    out
}

/// The C file of a binary: the document in its own section, the same
/// document's title table and icon registered with the runtime at start-up
/// (the icon's base64 is stored once, inside the document) with the header
/// lines of stop reports, and
/// `int relift_info_main(void)`, which writes the document to stdout.
#[must_use]
pub fn c_source(doc: &[u8]) -> String {
    let info = crate::Info::from_bytes(doc).ok();
    let span = info.as_ref().and_then(|i| icon_span(doc, i));
    let mut t = String::new();
    t.push_str("#include <stdio.h>\n#include <relift/appinfo.h>\n#ifdef _WIN32\n#include <fcntl.h>\n#include <io.h>\n#endif\n");
    let _ = write!(
        t,
        "#if defined(__APPLE__)\n#define RL_INFO_SECTION __attribute__((section(\"__DATA,{MACHO_SECTION}\")))\n#elif defined(_WIN32)\n#define RL_INFO_SECTION __attribute__((section(\"{PE_SECTION}\")))\n#else\n#define RL_INFO_SECTION __attribute__((section(\"{ELF_SECTION}\")))\n#endif\n"
    );
    let (head, icon, tail) = match span {
        Some((a, b)) => (&doc[..a], &doc[a..b], &doc[b..]),
        None => (doc, &doc[..0], &doc[..0]),
    };
    let parts: Vec<(&str, &[u8])> = if icon.is_empty() {
        vec![("head", head)]
    } else {
        vec![("head", head), ("icon", icon), ("tail", tail)]
    };
    t.push_str("RL_INFO_SECTION __attribute__((used)) static const struct {\n");
    for (name, part) in &parts {
        let _ = writeln!(t, "    unsigned char {name}[{}];", part.len().max(1));
    }
    t.push_str("} relift_info_doc = {\n");
    for (name, part) in &parts {
        let _ = writeln!(t, "  {{ /* {name} */");
        bytes_rows(&mut t, part);
        t.push_str("  },\n");
    }
    t.push_str("};\n");
    let titles = info.as_ref().map(titles).unwrap_or_default();
    if titles.is_empty() {
        t.push_str("static const char *const *const relift_langs = 0;\nstatic const char *const *const relift_names = 0;\n");
    } else {
        t.push_str("static const char *const relift_langs[] = {\n");
        for (k, _) in &titles {
            let _ = writeln!(t, "    {},", c_string(k));
        }
        t.push_str("};\nstatic const char *const relift_names[] = {\n");
        for (_, v) in &titles {
            let _ = writeln!(t, "    {},", c_string(v));
        }
        t.push_str("};\n");
    }
    let default = info
        .as_ref()
        .and_then(|i| i.identity.default_language.as_deref())
        .map_or_else(|| "0".to_string(), c_string);
    let icon_ref = if icon.is_empty() {
        "0, 0".to_string()
    } else {
        "(const char *)relift_info_doc.icon, sizeof relift_info_doc.icon".to_string()
    };
    let report: String = report_lines(info.as_ref(), doc)
        .iter()
        .map(|(k, v)| format!("    rl_report_info({}, {});\n", c_string(k), c_string(v)))
        .collect();
    let game = info.as_ref().is_none_or(|i| i.identity.is_game());
    let _ = write!(
        t,
        "__attribute__((constructor)) static void relift_register_identity(void)\n{{\n    rl_app_set_identity(relift_langs, relift_names, {}, {default}, {icon_ref});\n    rl_app_set_game({});\n{report}}}\n",
        titles.len(),
        i32::from(game)
    );
    t.push_str(
        "int relift_info_main(void)\n{\n#ifdef _WIN32\n    _setmode(_fileno(stdout), _O_BINARY);\n#endif\n    size_t n = fwrite(&relift_info_doc, 1, sizeof relift_info_doc, stdout);\n    return (n == sizeof relift_info_doc && fflush(stdout) == 0) ? 0 : 1;\n}\n",
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
        assert!(c.contains("head[3]"));
        assert!(c.contains(" 123, 125, 10,"));
    }

    #[test]
    fn the_source_includes_what_the_windows_binary_mode_call_needs() {
        let c = c_source(b"{}\n");
        let win = c.find("#ifdef _WIN32").unwrap();
        let setmode = c.find("_setmode").unwrap();
        assert!(win < setmode);
        for h in ["<io.h>", "<fcntl.h>"] {
            let at = c.find(h).unwrap();
            assert!(at < setmode, "{h}");
        }
    }

    fn doc_with_icon() -> Vec<u8> {
        let mut info = crate::Info::new("0");
        info.identity
            .names
            .insert("French".into(), "L\u{e9}o \"x\"".into());
        info.identity
            .names
            .insert("AmericanEnglish".into(), "Leo".into());
        info.identity.default_language = Some("AmericanEnglish".into());
        info.identity.icon = Some(crate::Icon {
            format: "png".into(),
            width: 1,
            height: 1,
            data: "QUJDRA==".into(),
        });
        info.to_bytes()
    }

    #[test]
    fn the_icon_is_stored_once_inside_the_document() {
        let doc = doc_with_icon();
        let c = c_source(&doc);
        assert!(c.contains("icon[8]"), "{c}");
        let b64: Vec<String> = b"QUJDRA==".iter().map(|b| format!(" {b},")).collect();
        assert_eq!(c.matches(&b64.join("")).count(), 1);
        assert!(c.contains("(const char *)relift_info_doc.icon, sizeof relift_info_doc.icon"));
        let (a, b) = icon_span(&doc, &crate::Info::from_bytes(&doc).unwrap()).unwrap();
        assert_eq!(&doc[a..b], b"QUJDRA==");
    }

    #[test]
    fn the_title_table_follows_the_fallback_order_and_escapes() {
        let c = c_source(&doc_with_icon());
        let en = c.find("\"AmericanEnglish\"").unwrap();
        let fr = c.find("\"French\"").unwrap();
        assert!(en < fr);
        assert!(c.contains("\"L\\303\\251o \\042x\\042\""), "{c}");
        assert!(
            c.contains("{ rl_app_set_identity")
                || c.contains(
                    "rl_app_set_identity(relift_langs, relift_names, 2, \"AmericanEnglish\""
                )
        );
    }

    #[test]
    fn a_document_without_titles_or_icon_registers_nothing_to_show() {
        let c = c_source(b"{}\n");
        assert!(c.contains("relift_langs = 0"));
        assert!(!c.contains("icon["));
        assert!(c.contains("rl_app_set_identity(relift_langs, relift_names, 0, 0, 0, 0)"));
    }

    #[test]
    fn the_source_registers_whether_the_program_is_a_game() {
        let mut i = crate::Info::new("0");
        assert!(c_source(&i.to_bytes()).contains("rl_app_set_game(1)"));
        i.identity.game = Some(false);
        assert!(c_source(&i.to_bytes()).contains("rl_app_set_game(0)"));
        i.identity.game = Some(true);
        assert!(c_source(&i.to_bytes()).contains("rl_app_set_game(1)"));
    }
}
