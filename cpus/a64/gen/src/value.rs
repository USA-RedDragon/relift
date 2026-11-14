//! Values of the staged evaluator.
//!
//! A value is known at one of three times. Concrete values are known while
//! generating. Static values (`V::S`) are known when lifting: they depend on
//! instruction bits the template leaves open, and the template computes them
//! as Rust expressions. Dynamic values (`V::D`) are known only when the guest
//! runs: they are `relift_ir::Value`s the template builds.

use std::rc::Rc;

/// The type of a static or dynamic value.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum K {
    /// A boolean; dynamic booleans are one-bit IR values.
    Bool,
    /// A Sail integer; static as `i128`, dynamic as a 128-bit IR value read
    /// as signed.
    Int,
    /// A bit vector of this width; static as `u128`.
    Bits(u32),
    /// A member of this enum; static as its index, an `i128`.
    Enum(&'static str),
}

/// A value.
#[derive(Clone, Debug, PartialEq)]
pub enum V {
    /// `()`.
    Unit,
    /// A concrete boolean.
    Bool(bool),
    /// A concrete integer.
    Int(i128),
    /// A concrete bit vector: width and value.
    Bits(u32, u128),
    /// A string, used only in messages.
    Str(Rc<str>),
    /// A member of an enum: the enum and the member index.
    Enum(Rc<str>, usize),
    /// A struct value: its type name and fields.
    Struct(Rc<str>, Rc<Vec<(String, V)>>),
    /// A tuple.
    Tuple(Rc<Vec<V>>),
    /// A bitfield value: its type name and its bits.
    Bf(Rc<str>, Box<V>),
    /// A union value: the constructor and its argument.
    Union(Rc<str>, Box<V>),
    /// `undefined`, with its width when known.
    Undef(Option<u32>),
    /// A static value: its type, the Rust expression (a local name or a
    /// literal) and the instruction bits it depends on.
    S(K, Rc<str>, u32),
    /// A dynamic value: its type and the Rust local holding the IR value.
    D(K, Rc<str>),
    /// A bit vector wider than 128 bits, as its parts, least significant
    /// first; each part is at most 128 bits.
    Wide(Rc<Vec<V>>),
}

/// The type class of a value, for signature matching.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Kind {
    /// A boolean at any time.
    Bool,
    /// An integer at any time.
    Int,
    /// A bit vector at any time.
    Bits,
    /// An enum.
    Enum(Rc<str>),
    /// A struct.
    Struct(Rc<str>),
    /// A bitfield.
    Bf(Rc<str>),
    /// A string.
    Str,
    /// `()`.
    Unit,
    /// A tuple.
    Tuple,
    /// A union.
    Union,
    /// `undefined`.
    Undef,
}

impl V {
    /// The type class.
    pub fn kind(&self) -> Kind {
        match self {
            V::Unit => Kind::Unit,
            V::Bool(_) | V::S(K::Bool, ..) | V::D(K::Bool, _) => Kind::Bool,
            V::Int(_) | V::S(K::Int, ..) | V::D(K::Int, _) => Kind::Int,
            V::Bits(..) | V::S(K::Bits(_), ..) | V::D(K::Bits(_), _) | V::Wide(_) => Kind::Bits,
            V::Str(_) => Kind::Str,
            V::Enum(t, _) => Kind::Enum(t.clone()),
            V::S(K::Enum(t), ..) | V::D(K::Enum(t), _) => Kind::Enum((*t).into()),
            V::Struct(t, _) => Kind::Struct(t.clone()),
            V::Tuple(_) => Kind::Tuple,
            V::Bf(t, _) => Kind::Bf(t.clone()),
            V::Union(..) => Kind::Union,
            V::Undef(_) => Kind::Undef,
        }
    }

    /// The width of a bit vector value.
    pub fn width(&self) -> Option<u32> {
        match self {
            V::Bits(w, _) | V::S(K::Bits(w), ..) | V::D(K::Bits(w), _) => Some(*w),
            V::Undef(w) => *w,
            V::Wide(ps) => ps.iter().map(V::width).sum(),
            _ => None,
        }
    }

    /// The instruction bits a static value depends on.
    pub fn deps(&self) -> u32 {
        match self {
            V::S(_, _, d) => *d,
            V::Tuple(vs) | V::Wide(vs) => vs.iter().fold(0, |a, v| a | v.deps()),
            V::Struct(_, fs) => fs.iter().fold(0, |a, (_, v)| a | v.deps()),
            V::Bf(_, v) | V::Union(_, v) => v.deps(),
            _ => 0,
        }
    }

    /// Whether the value is known while generating.
    pub fn is_concrete(&self) -> bool {
        match self {
            V::S(..) | V::D(..) => false,
            V::Wide(ps) => ps.iter().all(V::is_concrete),
            _ => true,
        }
    }

    /// Whether the value is dynamic.
    pub fn is_dyn(&self) -> bool {
        matches!(self, V::D(..))
    }

    /// A tuple of `vs`.
    pub fn tuple(vs: Vec<V>) -> V {
        V::Tuple(Rc::new(vs))
    }
}

/// The all-ones value of `w` bits.
pub fn mask(w: u32) -> u128 {
    if w >= 128 {
        u128::MAX
    } else {
        (1u128 << w) - 1
    }
}
