//! The parts of the Khronos registry (`vk.xml`) the generator reads: types,
//! enum values, API constants, commands, features and extensions, with the
//! elements of other APIs (Vulkan SC) dropped.

use std::collections::BTreeMap;

use crate::Error;

/// A declaration: a struct member, a command parameter or a return type.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Decl {
    /// Member or parameter name; empty for a return type.
    pub name: String,
    /// The base type name (`uint32_t`, `VkBuffer`, `char`).
    pub ty: String,
    /// Number of `*` in the declaration.
    pub ptr: u8,
    /// Whether the object the outermost pointer points at is const.
    pub pointee_const: bool,
    /// Fixed array dimensions, literal numbers or API constant names.
    pub array: Vec<String>,
    /// The `len` attribute.
    pub len: Option<String>,
    /// The `values` attribute (the sType of a struct).
    pub values: Option<String>,
    /// A bitfield width, when the member is one.
    pub bitfield: Option<u32>,
    /// The C text of the declaration, comments removed.
    pub text: String,
}

/// What kind of type a registry type is.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Kind {
    /// A typedef of a C type (`typedef uint32_t VkBool32;`), or an opaque platform type when None.
    Base(Option<String>),
    /// A flags type over VkFlags or VkFlags64, with its bits enum.
    Bitmask {
        /// The underlying type, `VkFlags` or `VkFlags64`.
        over: String,
        /// The bits enum, when there is one.
        bits: Option<String>,
    },
    /// A handle.
    Handle {
        /// Dispatchable (a pointer) or not (a 64-bit value).
        dispatchable: bool,
    },
    /// An enum type; its values are in `Registry::enum_values`.
    Enum,
    /// A struct or union.
    Struct {
        /// The members in order.
        members: Vec<Decl>,
        /// True for a union.
        union: bool,
        /// The structs this one extends through pNext.
        extends: Vec<String>,
        /// True when the implementation fills it and the application only reads it.
        returned_only: bool,
    },
    /// A function pointer type.
    FuncPtr {
        /// The return type.
        ret: Decl,
        /// The parameters.
        params: Vec<Decl>,
    },
    /// Another name for a type.
    Alias(String),
    /// A C type that needs no declaration (`uint32_t`) or something the generator never emits.
    Other,
}

/// One registry type.
#[derive(Clone, Debug)]
pub struct Type {
    /// The name.
    pub name: String,
    /// What it is.
    pub kind: Kind,
    /// The `requires` attribute (a header or the bits enum of a bitmask).
    pub requires: Option<String>,
}

/// How an enum value is given.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Value {
    /// A literal C expression.
    Literal(String),
    /// `1 << n`.
    BitPos(u32),
    /// Another enum name.
    Alias(String),
}

/// One enum value definition.
#[derive(Clone, Debug)]
pub struct EnumDef {
    /// The value name.
    pub name: String,
    /// The enum type it belongs to, or None for a free constant.
    pub group: Option<String>,
    /// Its value.
    pub value: Value,
    /// True when it comes from the type's own enums block, not from a feature or extension.
    pub base: bool,
}

/// One command.
#[derive(Clone, Debug)]
pub struct Command {
    /// The name.
    pub name: String,
    /// The return type.
    pub ret: Decl,
    /// The parameters.
    pub params: Vec<Decl>,
}

/// One `<require>` block of a feature or extension.
#[derive(Clone, Debug, Default)]
pub struct Require {
    /// The `depends` expression of the block.
    pub depends: Option<String>,
    /// Type names.
    pub types: Vec<String>,
    /// Command names.
    pub commands: Vec<String>,
    /// Enum value names.
    pub enums: Vec<String>,
}

/// A core version or an extension.
#[derive(Clone, Debug)]
pub struct Unit {
    /// `VK_VERSION_1_0` or `VK_KHR_surface`.
    pub name: String,
    /// The `depends` expression.
    pub depends: Option<String>,
    /// The extension number (0 for features).
    pub number: u32,
    /// `instance` or `device` for extensions.
    pub ext_type: Option<String>,
    /// The `platform` attribute of an extension.
    pub platform: Option<String>,
    /// Its require blocks.
    pub requires: Vec<Require>,
}

/// The registry.
#[derive(Debug, Default)]
pub struct Registry {
    /// Types by name.
    pub types: BTreeMap<String, Type>,
    /// Every enum value definition by name, from the enums blocks and every require block.
    pub enum_values: BTreeMap<String, EnumDef>,
    /// Commands by name; an alias maps to its target in `command_alias`.
    pub commands: BTreeMap<String, Command>,
    /// Command aliases: alias name to target name.
    pub command_alias: BTreeMap<String, String>,
    /// Bit widths of enum groups (32 or 64).
    pub enum_width: BTreeMap<String, u32>,
    /// Names of the API constants, in registry order.
    pub constants: Vec<String>,
    /// Core versions and internal features by name.
    pub features: BTreeMap<String, Unit>,
    /// Extensions by name.
    pub extensions: BTreeMap<String, Unit>,
}

type Node<'a, 'i> = roxmltree::Node<'a, 'i>;

fn for_vulkan(n: Node) -> bool {
    match n.attribute("api") {
        None => true,
        Some(a) => a.split(',').any(|x| x == "vulkan"),
    }
}

fn elements<'a, 'i>(n: Node<'a, 'i>, tag: &'static str) -> impl Iterator<Item = Node<'a, 'i>> {
    n.children()
        .filter(move |c| c.is_element() && c.has_tag_name(tag) && for_vulkan(*c))
}

fn child_text(n: Node, tag: &str) -> Option<String> {
    n.children()
        .find(|c| c.is_element() && c.has_tag_name(tag))
        .map(|c| c.text().unwrap_or("").to_string())
}

/// Parses a member, parameter or proto element into a Decl.
fn decl(n: Node) -> Result<Decl, Error> {
    let mut pre = String::new();
    let mut post = String::new();
    let mut after = String::new();
    let mut ty = None;
    let mut name = None;
    let mut text = String::new();
    for c in n.children() {
        if c.is_text() {
            let t = c.text().unwrap_or("");
            text.push_str(t);
            if name.is_some() {
                after.push_str(t);
            } else if ty.is_some() {
                post.push_str(t);
            } else {
                pre.push_str(t);
            }
        } else if c.is_element() {
            let t = c.text().unwrap_or("");
            match c.tag_name().name() {
                "comment" => continue,
                "type" => ty = Some(t.to_string()),
                "name" => name = Some(t.to_string()),
                "enum" => after.push_str(t),
                _ => {}
            }
            text.push_str(t);
        }
    }
    let ty = ty.ok_or_else(|| Error(format!("declaration without a type: {text}")))?;
    let name = name.unwrap_or_default();
    let ptr = post.matches('*').count() as u8;
    let pointee_const = match ptr {
        0 => false,
        1 => pre.contains("const"),
        _ => {
            let p: String = post.split_whitespace().collect();
            let last = p.rfind('*').unwrap_or(0);
            p[..last].ends_with("const")
        }
    };
    let mut array = Vec::new();
    let mut bitfield = None;
    let a = after.trim();
    if let Some(bits) = a.strip_prefix(':') {
        bitfield = Some(
            bits.trim()
                .parse()
                .map_err(|_| Error(format!("bad bitfield in {text}")))?,
        );
    } else {
        let mut rest = a;
        while let Some(open) = rest.find('[') {
            let close = rest[open..]
                .find(']')
                .ok_or_else(|| Error(format!("bad array in {text}")))?;
            array.push(rest[open + 1..open + close].trim().to_string());
            rest = &rest[open + close + 1..];
        }
    }
    let text = text.split_whitespace().collect::<Vec<_>>().join(" ");
    Ok(Decl {
        name,
        ty,
        ptr,
        pointee_const,
        array,
        len: n.attribute("len").map(str::to_string),
        values: n.attribute("values").map(str::to_string),
        bitfield,
        text,
    })
}

fn split_list(s: Option<&str>) -> Vec<String> {
    s.map(|s| s.split(',').map(str::to_string).collect())
        .unwrap_or_default()
}

fn parse_type(n: Node) -> Result<Option<Type>, Error> {
    let cat = n.attribute("category").unwrap_or("");
    let name = n
        .attribute("name")
        .map(str::to_string)
        .or_else(|| child_text(n, "name"))
        .or_else(|| {
            n.children()
                .find(|c| c.is_element() && c.has_tag_name("proto"))
                .and_then(|p| child_text(p, "name"))
        });
    let Some(name) = name else { return Ok(None) };
    let requires = n.attribute("requires").map(str::to_string);
    if let Some(alias) = n.attribute("alias") {
        return Ok(Some(Type {
            name,
            kind: Kind::Alias(alias.to_string()),
            requires,
        }));
    }
    let kind = match cat {
        "basetype" => Kind::Base(child_text(n, "type")),
        "bitmask" => Kind::Bitmask {
            over: child_text(n, "type").unwrap_or_else(|| "VkFlags".into()),
            bits: n
                .attribute("bitvalues")
                .or(n.attribute("requires"))
                .map(str::to_string),
        },
        "handle" => Kind::Handle {
            dispatchable: child_text(n, "type").as_deref() == Some("VK_DEFINE_HANDLE"),
        },
        "enum" => Kind::Enum,
        "struct" | "union" => {
            let members = elements(n, "member").map(decl).collect::<Result<_, _>>()?;
            Kind::Struct {
                members,
                union: cat == "union",
                extends: split_list(n.attribute("structextends")),
                returned_only: n.attribute("returnedonly") == Some("true"),
            }
        }
        "funcpointer" => {
            let proto = n
                .children()
                .find(|c| c.is_element() && c.has_tag_name("proto"))
                .ok_or_else(|| Error(format!("funcpointer {name} without proto")))?;
            Kind::FuncPtr {
                ret: decl(proto)?,
                params: elements(n, "param").map(decl).collect::<Result<_, _>>()?,
            }
        }
        _ => Kind::Other,
    };
    Ok(Some(Type {
        name,
        kind,
        requires,
    }))
}

fn enum_def(n: Node, group: Option<&str>, extnumber: u32) -> Result<Option<EnumDef>, Error> {
    let Some(name) = n.attribute("name") else {
        return Ok(None);
    };
    let group = n.attribute("extends").or(group).map(str::to_string);
    let value = if let Some(a) = n.attribute("alias") {
        Value::Alias(a.to_string())
    } else if let Some(v) = n.attribute("value") {
        Value::Literal(v.to_string())
    } else if let Some(b) = n.attribute("bitpos") {
        Value::BitPos(
            b.parse()
                .map_err(|_| Error(format!("bad bitpos for {name}")))?,
        )
    } else if let Some(o) = n.attribute("offset") {
        let off: i64 = o
            .parse()
            .map_err(|_| Error(format!("bad offset for {name}")))?;
        let ext: i64 = match n.attribute("extnumber") {
            Some(e) => e
                .parse()
                .map_err(|_| Error(format!("bad extnumber for {name}")))?,
            None => i64::from(extnumber),
        };
        let mut v = 1_000_000_000 + (ext - 1) * 1000 + off;
        if n.attribute("dir") == Some("-") {
            v = -v;
        }
        Value::Literal(v.to_string())
    } else {
        return Ok(None);
    };
    Ok(Some(EnumDef {
        name: name.to_string(),
        group,
        value,
        base: false,
    }))
}

fn parse_unit(n: Node, reg: &mut Registry) -> Result<Unit, Error> {
    let number: u32 = n
        .attribute("number")
        .and_then(|s| s.parse().ok())
        .unwrap_or(0);
    let is_ext = n.has_tag_name("extension");
    let mut requires = Vec::new();
    for r in elements(n, "require") {
        let mut req = Require {
            depends: r.attribute("depends").map(str::to_string),
            ..Require::default()
        };
        for c in r.children().filter(|c| c.is_element() && for_vulkan(*c)) {
            let Some(name) = c.attribute("name") else {
                continue;
            };
            match c.tag_name().name() {
                "type" => req.types.push(name.to_string()),
                "command" => req.commands.push(name.to_string()),
                "enum" => {
                    if let Some(d) = enum_def(c, None, if is_ext { number } else { 0 })? {
                        reg.enum_values.entry(d.name.clone()).or_insert(d);
                    }
                    req.enums.push(name.to_string());
                }
                _ => {}
            }
        }
        requires.push(req);
    }
    Ok(Unit {
        name: n.attribute("name").unwrap_or("").to_string(),
        depends: n.attribute("depends").map(str::to_string),
        number: if is_ext { number } else { 0 },
        ext_type: n.attribute("type").map(str::to_string),
        platform: n.attribute("platform").map(str::to_string),
        requires,
    })
}

impl Registry {
    /// Parses vk.xml text.
    pub fn parse(xml: &str) -> Result<Registry, Error> {
        let doc = roxmltree::Document::parse(xml).map_err(|e| Error(format!("vk.xml: {e}")))?;
        let root = doc.root_element();
        let mut reg = Registry::default();
        for block in root.children().filter(|c| c.is_element()) {
            match block.tag_name().name() {
                "types" => {
                    for t in elements(block, "type") {
                        if let Some(t) = parse_type(t)? {
                            reg.types.entry(t.name.clone()).or_insert(t);
                        }
                    }
                }
                "enums" => {
                    let group = block.attribute("name").unwrap_or("");
                    let constants = block.attribute("type") == Some("constants");
                    if let Some(w) = block.attribute("bitwidth") {
                        reg.enum_width
                            .insert(group.to_string(), w.parse().unwrap_or(32));
                    }
                    for e in elements(block, "enum") {
                        let g = if constants { None } else { Some(group) };
                        if let Some(mut d) = enum_def(e, g, 0)? {
                            d.base = !constants;
                            if constants {
                                reg.constants.push(d.name.clone());
                            }
                            reg.enum_values.insert(d.name.clone(), d);
                        }
                    }
                }
                "commands" => {
                    for c in elements(block, "command") {
                        if let (Some(name), Some(alias)) =
                            (c.attribute("name"), c.attribute("alias"))
                        {
                            reg.command_alias
                                .insert(name.to_string(), alias.to_string());
                            continue;
                        }
                        let proto = c
                            .children()
                            .find(|x| x.is_element() && x.has_tag_name("proto"))
                            .ok_or_else(|| Error("command without proto".into()))?;
                        let ret = decl(proto)?;
                        let params = elements(c, "param").map(decl).collect::<Result<_, _>>()?;
                        reg.commands.insert(
                            ret.name.clone(),
                            Command {
                                name: ret.name.clone(),
                                ret,
                                params,
                            },
                        );
                    }
                }
                "feature" if for_vulkan(block) => {
                    let u = parse_unit(block, &mut reg)?;
                    reg.features.insert(u.name.clone(), u);
                }
                "extensions" => {
                    for e in block.children().filter(|c| c.is_element()) {
                        let supported = e.attribute("supported").unwrap_or("");
                        if !supported.split(',').any(|s| s == "vulkan") {
                            continue;
                        }
                        let u = parse_unit(e, &mut reg)?;
                        reg.extensions.insert(u.name.clone(), u);
                    }
                }
                _ => {}
            }
        }
        Ok(reg)
    }

    /// Follows type aliases to the defining type.
    pub fn resolve<'a>(&'a self, name: &'a str) -> &'a str {
        let mut n = name;
        for _ in 0..8 {
            match self.types.get(n).map(|t| &t.kind) {
                Some(Kind::Alias(a)) => n = a,
                _ => break,
            }
        }
        n
    }

    /// Follows command aliases to the defining command.
    pub fn command<'a>(&'a self, name: &'a str) -> Option<&'a Command> {
        let mut n = name;
        for _ in 0..8 {
            match self.command_alias.get(n) {
                Some(a) => n = a,
                None => break,
            }
        }
        self.commands.get(n)
    }
}
