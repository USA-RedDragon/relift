use std::fmt::Write;

use crate::bind::HostInput;
use crate::model::Port;

fn quote(s: &str) -> String {
    let mut out = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            c if c.is_ascii_graphic() || c == ' ' => out.push(c),
            _ => {
                let _ = write!(out, "?");
            }
        }
    }
    out.push('"');
    out
}

/// C source declaring `ports` for the runtime: the array `<name>` of
/// `rl_port_decl` and the count `<name>_count`. Labels are not included; the
/// runtime reads ids, kinds and default bindings only.
pub fn declarations_c(name: &str, ports: &[Port]) -> String {
    let mut s = String::new();
    for (i, p) in ports.iter().enumerate() {
        let _ = writeln!(s, "static const rl_control_decl {name}_{i}[] = {{");
        for c in &p.controls {
            let d = if c.defaults.is_empty() {
                "NULL".to_string()
            } else {
                quote(&HostInput::list_string(&c.defaults))
            };
            let _ = writeln!(s, "    {{ {}, {}, {} }},", quote(&c.id), c.kind.code(), d);
        }
        if p.controls.is_empty() {
            let _ = writeln!(s, "    {{ NULL, 0, NULL }},");
        }
        let _ = writeln!(s, "}};\n");
    }
    let _ = writeln!(s, "const rl_port_decl {name}[] = {{");
    for (i, p) in ports.iter().enumerate() {
        let _ = writeln!(
            s,
            "    {{ {}, {}, {}, {name}_{i} }},",
            quote(&p.id),
            quote(&p.kind),
            p.controls.len()
        );
    }
    if ports.is_empty() {
        let _ = writeln!(s, "    {{ NULL, NULL, 0, NULL }},");
    }
    let _ = writeln!(s, "}};\nconst uint32_t {name}_count = {};", ports.len());
    s
}
