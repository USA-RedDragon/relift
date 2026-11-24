//! The OpenGL-family registries (`gl.xml`, `glx.xml`, `egl.xml`): the
//! commands the selected features require, with each parameter's type
//! classified by its C typedef in the registry (or the configuration, for
//! the platform types the registry leaves to a header).

use std::collections::{BTreeMap, BTreeSet};

use roxmltree::{Document, Node};

use crate::Error;

/// What a registry type is, as far as forwarding is concerned.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Ty {
    /// A fixed-size integer: bytes and signedness.
    Int(u8, bool),
    /// `float` (4) or `double` (8).
    Float(u8),
    /// A pointer-sized integer (`long`, `intptr_t`): 4 bytes on i386, 8 on the host.
    Word(bool),
    /// An opaque pointer the guest holds as a token (`EGLDisplay`, `GLsync`).
    Handle,
    /// `void`.
    Void,
    /// A function pointer.
    FuncPtr,
    /// A type the forwarding cannot pass (`Display`, `XVisualInfo`).
    Opaque,
}

impl Ty {
    /// Guest bytes of the type used by value on i386.
    pub fn i386_size(self) -> u8 {
        match self {
            Ty::Int(n, _) | Ty::Float(n) => n,
            Ty::Void => 0,
            _ => 4,
        }
    }
}

/// A parameter or return type.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Param {
    /// Name; empty for a return type.
    pub name: String,
    /// The base type name (`GLenum`, `void`, `char`).
    pub ty: String,
    /// Number of `*`.
    pub ptr: u8,
    /// Whether the outermost pointee is const.
    pub pointee_const: bool,
    /// The `len` attribute.
    pub len: Option<String>,
}

/// One command.
#[derive(Clone, Debug)]
pub struct Cmd {
    /// The name.
    pub name: String,
    /// The return type.
    pub ret: Param,
    /// The parameters.
    pub params: Vec<Param>,
    /// True when it is written by hand.
    pub hand: bool,
}

/// The selected commands of one or more registries.
#[derive(Clone, Debug, Default)]
pub struct Api {
    /// The commands, in name order.
    pub commands: Vec<Cmd>,
    /// The types they use.
    pub types: BTreeMap<String, Ty>,
    /// Enum values by name.
    pub enums: BTreeMap<String, String>,
}

/// The C base types an LP64 host and an i386 guest agree on by name.
fn c_base(t: &str) -> Option<Ty> {
    Some(match t {
        "char" | "signed char" | "khronos_int8_t" | "int8_t" => Ty::Int(1, true),
        "unsigned char" | "khronos_uint8_t" | "uint8_t" => Ty::Int(1, false),
        "short" | "khronos_int16_t" | "int16_t" => Ty::Int(2, true),
        "unsigned short" | "khronos_uint16_t" | "uint16_t" => Ty::Int(2, false),
        "int" | "khronos_int32_t" | "int32_t" | "Bool" => Ty::Int(4, true),
        "unsigned int" | "khronos_uint32_t" | "uint32_t" => Ty::Int(4, false),
        "khronos_int64_t" | "int64_t" | "khronos_stime_nanoseconds_t" => Ty::Int(8, true),
        "khronos_uint64_t" | "uint64_t" | "khronos_utime_nanoseconds_t" => Ty::Int(8, false),
        "float" | "khronos_float_t" => Ty::Float(4),
        "double" => Ty::Float(8),
        "long" | "khronos_intptr_t" | "khronos_ssize_t" | "intptr_t" | "ssize_t" => Ty::Word(true),
        "unsigned long" | "khronos_uintptr_t" | "khronos_usize_t" | "uintptr_t" | "size_t" => {
            Ty::Word(false)
        }
        "void" => Ty::Void,
        _ => return None,
    })
}

fn text(n: Node<'_, '_>) -> String {
    let mut s = String::new();
    for c in n.descendants() {
        if c.is_text() {
            s.push_str(c.text().unwrap_or(""));
        }
    }
    s
}

fn child<'a, 'i>(n: Node<'a, 'i>, tag: &str) -> Option<Node<'a, 'i>> {
    n.children().find(|c| c.has_tag_name(tag))
}

/// Parses a `<proto>` or `<param>`: the type is the `<ptype>` when there
/// is one, else the C words before the name.
fn param(n: Node<'_, '_>) -> Param {
    let mut pre = String::new();
    let mut post = String::new();
    let mut ty = None;
    let mut name = String::new();
    for c in n.children() {
        if c.has_tag_name("ptype") {
            ty = Some(text(c));
        } else if c.has_tag_name("name") {
            name = text(c);
            break;
        } else if c.is_text() {
            let t = c.text().unwrap_or("");
            if ty.is_some() {
                post.push_str(t);
            } else {
                pre.push_str(t);
            }
        }
    }
    let (ty, pre, post) = match ty {
        Some(t) => (t, pre, post),
        None => {
            let words: Vec<&str> = pre
                .split(|c: char| c.is_whitespace() || c == '*')
                .filter(|w| !w.is_empty() && *w != "const")
                .collect();
            let base = words.join(" ");
            let first_star = pre.find('*').unwrap_or(pre.len());
            (
                base,
                pre[..first_star].to_string(),
                pre[first_star..].to_string(),
            )
        }
    };
    let ptr = post.matches('*').count() as u8;
    let pointee_const = match ptr {
        0 => false,
        1 => pre.contains("const") || post.split('*').next().unwrap_or("").contains("const"),
        _ => {
            let p: String = post.split_whitespace().collect();
            let last = p.rfind('*').unwrap_or(0);
            p[..last].ends_with("const")
        }
    };
    Param {
        name,
        ty,
        ptr,
        pointee_const,
        len: n.attribute("len").map(str::to_string),
    }
}

/// Classifies the typedef text of a `<type>` element.
fn classify(t: Node<'_, '_>, defs: &BTreeMap<String, String>) -> Option<Ty> {
    let s = text(t);
    let s = s.trim();
    let body = s.strip_prefix("typedef ")?.trim_end_matches(';').trim();
    if body.contains('(') {
        return Some(Ty::FuncPtr);
    }
    let name = child(t, "name").map(text)?;
    let base = body.strip_suffix(name.as_str())?.trim();
    if base.ends_with('*') {
        return Some(Ty::Handle);
    }
    c_base(base).or_else(|| defs.get(base).and_then(|b| c_base(b)))
}

/// Reads the commands of `features` (with API attribute `api`) and the
/// extension commands `more` from one registry into `out`; `hand` marks
/// every one of them hand-written.
pub fn read(
    xml: &str,
    api: &str,
    features: &[String],
    more: &[String],
    hand: bool,
    platform: &BTreeMap<String, String>,
    out: &mut Api,
) -> Result<(), Error> {
    let doc = Document::parse(xml).map_err(|e| Error(format!("registry: {e}")))?;
    let root = doc.root_element();
    let mut wanted: BTreeSet<String> = more.iter().cloned().collect();
    for f in features {
        let node = root
            .children()
            .find(|n| n.has_tag_name("feature") && n.attribute("name") == Some(f.as_str()))
            .ok_or_else(|| Error(format!("the registry has no feature {f}")))?;
        if node.attribute("api") != Some(api) {
            return Err(Error(format!("feature {f} is not of API {api}")));
        }
        for r in node.children().filter(|n| n.has_tag_name("require")) {
            for c in r.children().filter(|n| n.has_tag_name("command")) {
                if let Some(n) = c.attribute("name") {
                    wanted.insert(n.to_string());
                }
            }
        }
    }
    let mut defs: BTreeMap<String, String> = BTreeMap::new();
    for (k, v) in platform {
        defs.insert(k.clone(), v.clone());
    }
    let mut typed: Vec<Node<'_, '_>> = Vec::new();
    for block in root.children().filter(|n| n.has_tag_name("types")) {
        for t in block.children().filter(|n| n.has_tag_name("type")) {
            if t.attribute("api").is_some_and(|a| a != api) {
                continue;
            }
            typed.push(t);
        }
    }
    for t in &typed {
        let Some(name) = child(*t, "name").map(text) else {
            continue;
        };
        if let Some(ty) = classify(*t, &defs) {
            out.types.entry(name).or_insert(ty);
        } else if let Some(body) = text(*t).trim().strip_prefix("typedef ") {
            let base = body
                .trim_end_matches(';')
                .trim()
                .trim_end_matches(name.as_str())
                .trim()
                .to_string();
            defs.entry(name).or_insert(base);
        }
    }
    for (k, v) in platform {
        let ty = if v == "handle" {
            Some(Ty::Handle)
        } else {
            c_base(v)
        };
        let ty = ty.ok_or_else(|| Error(format!("platform type {k} = {v} is not a C type")))?;
        out.types.insert(k.clone(), ty);
    }
    for e in root.descendants().filter(|n| n.has_tag_name("enum")) {
        if let (Some(n), Some(v)) = (e.attribute("name"), e.attribute("value")) {
            out.enums.entry(n.to_string()).or_insert(v.to_string());
        }
    }
    let mut seen = BTreeSet::new();
    for cmds in root.children().filter(|n| n.has_tag_name("commands")) {
        for c in cmds.children().filter(|n| n.has_tag_name("command")) {
            let Some(proto) = child(c, "proto") else {
                continue;
            };
            let ret = param(proto);
            let name = ret.name.clone();
            if !wanted.contains(&name) || !seen.insert(name.clone()) {
                continue;
            }
            let params = c
                .children()
                .filter(|n| n.has_tag_name("param"))
                .map(param)
                .collect();
            out.commands.push(Cmd {
                name,
                ret: Param {
                    name: String::new(),
                    ..ret
                },
                params,
                hand,
            });
        }
    }
    let missing: Vec<&String> = wanted.iter().filter(|w| !seen.contains(*w)).collect();
    if !missing.is_empty() {
        return Err(Error(format!("the registry lacks commands {missing:?}")));
    }
    out.commands.sort_by(|a, b| a.name.cmp(&b.name));
    Ok(())
}

impl Api {
    /// The type of a base type name: a registry type, or a C base type.
    pub fn ty(&self, name: &str) -> Ty {
        self.types
            .get(name)
            .copied()
            .or_else(|| c_base(name))
            .unwrap_or(Ty::Opaque)
    }
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;

    use super::{Api, Ty, read};

    const XML: &str = r#"<registry>
<types>
  <type>typedef unsigned int <name>GLenum</name>;</type>
  <type requires="khrplatform">typedef khronos_ssize_t <name>GLsizeiptr</name>;</type>
  <type>typedef char <name>GLchar</name>;</type>
  <type>typedef void *<name>EGLDisplay</name>;</type>
  <type>typedef intptr_t <name>EGLAttrib</name>;</type>
  <type>typedef void (*<name>PROC</name>)(void);</type>
</types>
<enums><enum value="0x3038" name="EGL_NONE"/></enums>
<commands>
  <command><proto>const <ptype>GLubyte</ptype> *<name>glGetString</name></proto><param><ptype>GLenum</ptype> <name>name</name></param></command>
  <command><proto>void <name>glShaderSource</name></proto><param><ptype>GLsizei</ptype> <name>count</name></param><param len="count">const <ptype>GLchar</ptype> *const*<name>string</name></param></command>
  <command><proto>void *<name>glMapBuffer</name></proto><param><ptype>GLenum</ptype> <name>target</name></param></command>
  <command><proto>void <name>glUnused</name></proto></command>
</commands>
<feature api="gl" name="GL_VERSION_1_0"><require><command name="glGetString"/><command name="glShaderSource"/><command name="glMapBuffer"/></require></feature>
</registry>"#;

    #[test]
    fn reads_types_and_commands() {
        let mut api = Api::default();
        let mut platform = BTreeMap::new();
        platform.insert("GLubyte".to_string(), "uint8_t".to_string());
        platform.insert("GLsizei".to_string(), "int".to_string());
        read(
            XML,
            "gl",
            &["GL_VERSION_1_0".into()],
            &[],
            false,
            &platform,
            &mut api,
        )
        .unwrap();
        assert_eq!(api.ty("GLenum"), Ty::Int(4, false));
        assert_eq!(api.ty("GLsizeiptr"), Ty::Word(true));
        assert_eq!(api.ty("EGLDisplay"), Ty::Handle);
        assert_eq!(api.ty("EGLAttrib"), Ty::Word(true));
        assert_eq!(api.ty("PROC"), Ty::FuncPtr);
        assert_eq!(api.enums["EGL_NONE"], "0x3038");
        let names: Vec<&str> = api.commands.iter().map(|c| c.name.as_str()).collect();
        assert_eq!(names, ["glGetString", "glMapBuffer", "glShaderSource"]);
        let gs = &api.commands[0];
        assert_eq!(
            (gs.ret.ty.as_str(), gs.ret.ptr, gs.ret.pointee_const),
            ("GLubyte", 1, true)
        );
        let mb = &api.commands[1];
        assert_eq!((mb.ret.ty.as_str(), mb.ret.ptr), ("void", 1));
        let ss = &api.commands[2].params[1];
        assert_eq!(
            (ss.ty.as_str(), ss.ptr, ss.pointee_const),
            ("GLchar", 2, true)
        );
        assert_eq!(ss.len.as_deref(), Some("count"));
        assert!(
            read(
                XML,
                "gl",
                &["GL_VERSION_9".into()],
                &[],
                false,
                &platform,
                &mut api
            )
            .is_err()
        );
    }
}
