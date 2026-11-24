//! Guest layout and translation classes of the selected types.
//!
//! The guest's data model comes from its [`Abi`]: on AArch64 LP64 pointers,
//! handles and size_t are 8 bytes and every scalar is aligned to its size;
//! on i386 they are 4 bytes and 8-byte scalars align to 4 in structs. The
//! generator computes every struct's size, alignment and member offsets
//! under those rules and emits them as static asserts, so a compiler whose
//! layout differs fails instead of corrupting guest memory. The host layout
//! is the [`A64`] one (every host is LP64).

use std::collections::{BTreeMap, BTreeSet};

use crate::Error;
use crate::abi::{A64, Abi};
use crate::config::Config;
use crate::registry::{Decl, Kind, Registry};
use crate::select::Selection;

/// Size and alignment in bytes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Layout {
    /// Size.
    pub size: u64,
    /// Alignment.
    pub align: u64,
}

/// A struct's guest layout.
#[derive(Clone, Debug)]
pub struct StructLayout {
    /// Size and alignment of the whole struct.
    pub layout: Layout,
    /// Offset of each member, in member order.
    pub offsets: Vec<u64>,
}

/// The selection with layouts and classes.
pub struct Model<'r> {
    /// The registry.
    pub reg: &'r Registry,
    /// The selection.
    pub sel: &'r Selection,
    /// The configuration.
    pub cfg: &'r Config,
    /// The guest ABI.
    pub abi: Abi,
    /// Guest layouts of the selected structs and unions.
    pub structs: BTreeMap<String, StructLayout>,
    /// Host (LP64) layouts, filled only for a guest whose layout differs.
    pub host_structs: BTreeMap<String, StructLayout>,
    /// Structs and unions that cannot be passed to the host as guest bytes.
    pub converted: BTreeSet<String>,
    /// The dispatchable handle types, in kind order (kind = index + 1).
    pub dispatchable: Vec<String>,
}

fn prim(abi: Abi, name: &str) -> Option<Layout> {
    let s = match name {
        "char" | "int8_t" | "uint8_t" => 1,
        "int16_t" | "uint16_t" => 2,
        "int32_t" | "uint32_t" | "float" | "int" => 4,
        "int64_t" | "uint64_t" | "double" => 8,
        "size_t" => abi.word,
        _ => return None,
    };
    let (size, align) = abi.scalar(s);
    Some(Layout { size, align })
}

impl<'r> Model<'r> {
    /// Builds the model for the AArch64 guest.
    pub fn new(reg: &'r Registry, sel: &'r Selection, cfg: &'r Config) -> Result<Model<'r>, Error> {
        Model::for_abi(reg, sel, cfg, A64)
    }

    /// Builds the model for one guest ABI, failing on anything the
    /// forwarding cannot express.
    pub fn for_abi(
        reg: &'r Registry,
        sel: &'r Selection,
        cfg: &'r Config,
        abi: Abi,
    ) -> Result<Model<'r>, Error> {
        let mut m = Model {
            reg,
            sel,
            cfg,
            abi,
            structs: BTreeMap::new(),
            host_structs: BTreeMap::new(),
            converted: BTreeSet::new(),
            dispatchable: Vec::new(),
        };
        for t in &sel.types {
            if let Some(ty) = reg.types.get(t) {
                match &ty.kind {
                    Kind::Handle { dispatchable: true } => m.dispatchable.push(t.clone()),
                    Kind::Struct { .. } => {
                        m.struct_layout(t)?;
                        if !abi.is_host() {
                            m.layout_in(A64, t)?;
                        }
                    }
                    _ => {}
                }
            }
        }
        let names: Vec<String> = m.structs.keys().cloned().collect();
        let mut memo = BTreeMap::new();
        for n in &names {
            if m.needs_conversion(n, &mut memo)? {
                m.converted.insert(n.clone());
            }
        }
        Ok(m)
    }

    /// The kind number of a dispatchable handle type, 0 for any other type.
    pub fn kind_of(&self, ty: &str) -> usize {
        let t = self.reg.resolve(ty);
        self.dispatchable
            .iter()
            .position(|d| d == t)
            .map_or(0, |i| i + 1)
    }

    /// The registry kind of a type, aliases followed.
    pub fn kind(&self, ty: &str) -> Option<&'r Kind> {
        self.reg.types.get(self.reg.resolve(ty)).map(|t| &t.kind)
    }

    /// True for a struct or union.
    pub fn is_struct(&self, ty: &str) -> bool {
        matches!(self.kind(ty), Some(Kind::Struct { .. }))
    }

    /// The canonical name of a type.
    pub fn canon<'a>(&'a self, ty: &'a str) -> &'a str {
        self.reg.resolve(ty)
    }

    /// The guest layout of a type used by value.
    pub fn type_layout(&mut self, ty: &str) -> Result<Layout, Error> {
        self.type_layout_in(self.abi, ty)
    }

    /// The layout of a type used by value under one ABI.
    pub fn type_layout_in(&mut self, abi: Abi, ty: &str) -> Result<Layout, Error> {
        if let Some(l) = prim(abi, ty) {
            return Ok(l);
        }
        let t = self.reg.resolve(ty).to_string();
        let kind = self
            .reg
            .types
            .get(&t)
            .map(|x| x.kind.clone())
            .ok_or_else(|| Error(format!("unknown type {ty}")))?;
        match kind {
            Kind::Base(Some(b)) => self.type_layout_in(abi, &b),
            Kind::Base(None) => Err(Error(format!("opaque type {ty} used by value"))),
            Kind::Bitmask { over, .. } => self.type_layout_in(abi, &over),
            Kind::Handle {
                dispatchable: false,
            } => {
                let (size, align) = abi.scalar(8);
                Ok(Layout { size, align })
            }
            Kind::Handle { .. } | Kind::FuncPtr { .. } => Ok(Layout {
                size: abi.word,
                align: abi.word,
            }),
            Kind::Enum => {
                let w = self.reg.enum_width.get(&t).copied().unwrap_or(32);
                let s = u64::from(w / 8);
                Ok(Layout { size: s, align: s })
            }
            Kind::Struct { .. } => Ok(self.layout_in(abi, &t)?.layout),
            Kind::Alias(_) | Kind::Other => Err(Error(format!("type {ty} has no layout"))),
        }
    }

    fn dim(&self, d: &str) -> Result<u64, Error> {
        if let Ok(n) = d.parse() {
            return Ok(n);
        }
        match self.reg.enum_values.get(d).map(|e| &e.value) {
            Some(crate::registry::Value::Literal(v)) => v
                .trim_end_matches(['U', 'u'])
                .parse()
                .map_err(|_| Error(format!("array size {d} = {v} is not a number"))),
            _ => Err(Error(format!("unknown array size {d}"))),
        }
    }

    /// Number of elements of a declaration's fixed arrays (1 when none).
    pub fn elems(&self, d: &Decl) -> Result<u64, Error> {
        let mut n = 1;
        for a in &d.array {
            n *= self.dim(a)?;
        }
        Ok(n)
    }

    /// The guest layout of one declaration.
    pub fn decl_layout(&mut self, d: &Decl) -> Result<Layout, Error> {
        self.decl_layout_in(self.abi, d)
    }

    /// The layout of one declaration under one ABI.
    pub fn decl_layout_in(&mut self, abi: Abi, d: &Decl) -> Result<Layout, Error> {
        if d.bitfield.is_some() {
            return Err(Error(format!("bitfield {} is not supported", d.text)));
        }
        let one = if d.ptr > 0 {
            Layout {
                size: abi.word,
                align: abi.word,
            }
        } else {
            self.type_layout_in(abi, &d.ty)?
        };
        let n = self.elems(d)?;
        Ok(Layout {
            size: one.size * n,
            align: one.align,
        })
    }

    /// The guest layout of a struct or union.
    pub fn struct_layout(&mut self, name: &str) -> Result<StructLayout, Error> {
        self.layout_in(self.abi, name)
    }

    /// The host layout of a struct or union.
    pub fn host_layout(&mut self, name: &str) -> Result<StructLayout, Error> {
        self.layout_in(A64, name)
    }

    fn cache(&mut self, abi: Abi) -> &mut BTreeMap<String, StructLayout> {
        if abi == self.abi {
            &mut self.structs
        } else {
            &mut self.host_structs
        }
    }

    /// The layout of a struct or union under one ABI (the guest's or the host's).
    pub fn layout_in(&mut self, abi: Abi, name: &str) -> Result<StructLayout, Error> {
        let name = self.reg.resolve(name).to_string();
        if let Some(l) = self.cache(abi).get(&name) {
            return Ok(l.clone());
        }
        let Some(Kind::Struct { members, union, .. }) =
            self.reg.types.get(&name).map(|t| t.kind.clone())
        else {
            return Err(Error(format!("{name} is not a struct")));
        };
        let mut off: u64 = 0;
        let mut size: u64 = 0;
        let mut align: u64 = 1;
        let mut offsets = Vec::new();
        for m in &members {
            let l = self.decl_layout_in(abi, m)?;
            align = align.max(l.align);
            if union {
                offsets.push(0);
                size = size.max(l.size);
            } else {
                off = off.div_ceil(l.align) * l.align;
                offsets.push(off);
                off += l.size;
                size = off;
            }
        }
        size = size.div_ceil(align) * align;
        let l = StructLayout {
            layout: Layout { size, align },
            offsets,
        };
        self.cache(abi).insert(name, l.clone());
        Ok(l)
    }

    /// The members of a struct.
    pub fn members(&self, name: &str) -> &'r [Decl] {
        match self.kind(name) {
            Some(Kind::Struct { members, .. }) => members,
            _ => &[],
        }
    }

    /// True for a type whose guest bytes the host can use in place: no
    /// pointer, size_t, dispatchable handle or function pointer anywhere.
    pub fn plain(&self, ty: &str) -> bool {
        let t = self.reg.resolve(ty);
        if self.is_struct(t) {
            return !self.converted.contains(t);
        }
        self.kind_of(t) == 0 && !matches!(self.kind(t), Some(Kind::FuncPtr { .. })) && t != "size_t"
    }

    fn needs_conversion(
        &self,
        name: &str,
        memo: &mut BTreeMap<String, bool>,
    ) -> Result<bool, Error> {
        if let Some(v) = memo.get(name) {
            return Ok(*v);
        }
        memo.insert(name.to_string(), false);
        let mut need = self.cfg.hand_structs.iter().any(|h| h == name)
            || (!self.abi.is_host()
                && self.structs.get(name).map(|l| (&l.layout, &l.offsets))
                    != self.host_structs.get(name).map(|l| (&l.layout, &l.offsets)));
        for m in self.members(name) {
            let t = self.reg.resolve(&m.ty);
            let direct = m.ptr > 0
                || t == "size_t"
                || self.kind_of(t) != 0
                || matches!(self.kind(t), Some(Kind::FuncPtr { .. }))
                || matches!(self.kind(t), Some(Kind::Base(None)));
            if direct || (self.is_struct(t) && self.needs_conversion(t, memo)?) {
                need = true;
            }
        }
        memo.insert(name.to_string(), need);
        Ok(need)
    }
}
