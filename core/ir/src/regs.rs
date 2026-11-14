use crate::Width;

/// One state slot a frontend declares: a register, a flag, a system value.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RegDecl {
    /// The name the frontend gives the slot, unique within its file.
    pub name: &'static str,
    /// The slot width in bits.
    pub width: Width,
}

/// An index into a [`RegFile`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Reg(pub u16);

impl Reg {
    /// The slot index as a `usize`.
    pub fn index(self) -> usize {
        usize::from(self.0)
    }
}

/// A frontend's register file: the ordered list of its state slots.
///
/// The IR attaches no meaning to a slot. Which one is a general register, a
/// flag or a thread pointer is the frontend's business.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RegFile {
    decls: &'static [RegDecl],
}

impl RegFile {
    /// A register file over `decls`.
    ///
    /// # Panics
    ///
    /// Panics if two slots share a name, or there are more than 65,536 slots.
    pub fn new(decls: &'static [RegDecl]) -> RegFile {
        assert!(decls.len() <= usize::from(u16::MAX) + 1, "too many slots");
        for (i, a) in decls.iter().enumerate() {
            assert!(
                decls[..i].iter().all(|b| b.name != a.name),
                "slot name {} declared twice",
                a.name
            );
        }
        RegFile { decls }
    }

    /// Every slot in index order.
    pub fn decls(&self) -> &'static [RegDecl] {
        self.decls
    }

    /// The number of slots.
    pub fn len(&self) -> usize {
        self.decls.len()
    }

    /// Whether the file has no slots.
    pub fn is_empty(&self) -> bool {
        self.decls.is_empty()
    }

    /// The declaration of `reg`.
    ///
    /// # Panics
    ///
    /// Panics if `reg` is not in this file.
    pub fn decl(&self, reg: Reg) -> RegDecl {
        self.decls[reg.index()]
    }

    /// The width of `reg`.
    pub fn width(&self, reg: Reg) -> Width {
        self.decl(reg).width
    }

    /// Looks a slot up by name.
    pub fn find(&self, name: &str) -> Option<Reg> {
        self.decls
            .iter()
            .position(|d| d.name == name)
            .map(|i| Reg(i as u16))
    }
}
