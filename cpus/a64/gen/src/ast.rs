//! The Sail subset the generator reads: literals, patterns, types and
//! expressions, as the parser in [`crate::parse`] builds them.

/// A literal.
#[derive(Clone, Debug, PartialEq)]
pub enum Lit {
    /// `()`.
    Unit,
    /// `true` or `false`.
    Bool(bool),
    /// `bitzero` (0) or `bitone` (1).
    Bit(u8),
    /// A decimal integer.
    Int(i128),
    /// A `0b` or `0x` literal: its width in bits and value.
    Bits {
        /// The width: one per binary digit, four per hex digit.
        width: u32,
        /// The value.
        value: u128,
    },
    /// A decimal real such as `1.5`, kept as written.
    Real(String),
    /// A string, without its quotes.
    Str(String),
}

/// A pattern.
#[derive(Clone, Debug, PartialEq)]
pub enum Pat {
    /// `_`.
    Wild,
    /// A literal.
    Lit(Lit),
    /// A name: binds a variable, or names an enum member.
    Id(String),
    /// A union constructor applied to patterns.
    App(String, Vec<Pat>),
    /// `(p, q, ...)`.
    Tuple(Vec<Pat>),
    /// `[p, q, ...]`, one pattern per bit, most significant first.
    Vec(Vec<Pat>),
    /// `p @ q @ ...`, most significant first.
    Concat(Vec<Pat>),
    /// `p : T`.
    Typed(Box<Pat>, Typ),
    /// `p as name`.
    As(Box<Pat>, String),
}

/// A type argument: a numeric expression or a type.
#[derive(Clone, Debug, PartialEq)]
pub enum TypArg {
    /// A numeric expression such as `'N` or `64`.
    Expr(Expr),
    /// A type.
    Typ(Typ),
}

/// A type.
#[derive(Clone, Debug, PartialEq)]
pub enum Typ {
    /// A plain name: `int`, `bool`, `MemOp`, `'a`.
    Id(String),
    /// An application: `bits('N)`, `range(0, 3)`, `implicit('N)`.
    App(String, Vec<TypArg>),
    /// `(A, B)`.
    Tuple(Vec<Typ>),
    /// `(A, B) -> C`.
    Fun(Vec<Typ>, Box<Typ>),
    /// A braced set or existential, kept opaque: `{1, 2}`, `{'n, ... . int('n)}`.
    Set,
}

/// One arm of a `match` or `try`.
#[derive(Clone, Debug, PartialEq)]
pub struct Arm {
    /// The pattern.
    pub pat: Pat,
    /// The `if` guard, if any.
    pub guard: Option<Expr>,
    /// The body.
    pub body: Expr,
}

/// A loop direction.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Dir {
    /// `to`: counting up.
    Up,
    /// `downto`: counting down.
    Down,
}

/// An expression.
#[derive(Clone, Debug, PartialEq)]
pub enum Expr {
    /// A literal.
    Lit(Lit),
    /// A name.
    Id(String),
    /// `f(a, b)`; unary minus is `negate(x)` and `~x` is `not_bool(x)`.
    Call(String, Vec<Expr>),
    /// `a op b` for the operators in the parser's table (`+`, `@`, `==`,
    /// `&`, `|`, `-->`, ...), with the operator as written.
    Binop(String, Box<Expr>, Box<Expr>),
    /// `e.field`.
    Field(Box<Expr>, String),
    /// `e[i]`.
    Index(Box<Expr>, Box<Expr>),
    /// `e[hi .. lo]`.
    Slice(Box<Expr>, Box<Expr>, Box<Expr>),
    /// `(e : T)` or `e : T` in a position the parser accepts.
    Typed(Box<Expr>, Typ),
    /// `(a, b, ...)`.
    Tuple(Vec<Expr>),
    /// `[a, b, ...]`.
    VecLit(Vec<Expr>),
    /// `[e with i = v, hi .. lo = v, ...]`: (index or hi, lo, value).
    VUpdate(Box<Expr>, Vec<(Expr, Option<Expr>, Expr)>),
    /// `{ e with f = v, ... }`.
    SUpdate(Box<Expr>, Vec<(String, Expr)>),
    /// `struct { f = v, ... }`.
    Struct(Vec<(String, Expr)>),
    /// `{ s; s; ... }`.
    Block(Vec<Stmt>),
    /// `if c then a else b`.
    If(Box<Expr>, Box<Expr>, Option<Box<Expr>>),
    /// `match e { arms }`.
    Match(Box<Expr>, Vec<Arm>),
    /// `foreach (var from a to b by step) body`.
    Foreach {
        /// The loop variable.
        var: String,
        /// The first value.
        from: Box<Expr>,
        /// The direction.
        dir: Dir,
        /// The last value, inclusive.
        to: Box<Expr>,
        /// The step, 1 when not written.
        step: Box<Expr>,
        /// The body.
        body: Box<Expr>,
    },
    /// `while c do body`.
    While(Box<Expr>, Box<Expr>),
    /// `repeat body until c`.
    Repeat(Box<Expr>, Box<Expr>),
    /// `return e`.
    Return(Box<Expr>),
    /// `throw e`.
    Throw(Box<Expr>),
    /// `exit(...)`.
    Exit,
    /// `try e catch { arms }`.
    Try(Box<Expr>, Vec<Arm>),
    /// `undefined`.
    Undefined,
    /// `let p = e in body`.
    LetIn(Pat, Box<Expr>, Box<Expr>),
}

/// A statement of a block.
#[derive(Clone, Debug, PartialEq)]
pub enum Stmt {
    /// `let p = e`, scoped to the rest of the block.
    Let(Pat, Expr),
    /// `lhs = e`, also `var p = e` (with a pattern turned into an expression).
    Assign(Expr, Expr),
    /// `lhs : T = e`, a declaration.
    Decl(Expr, Typ, Expr),
    /// An expression evaluated for its effect or as the block's value.
    Expr(Expr),
}

/// One clause of a function definition.
#[derive(Clone, Debug, PartialEq)]
pub struct Clause {
    /// The parameter pattern; a tuple pattern for several parameters.
    pub params: Pat,
    /// The clause guard, `function f (p) if g = ...`.
    pub guard: Option<Expr>,
    /// The body.
    pub body: Expr,
}
