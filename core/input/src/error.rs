use std::fmt;

/// What went wrong declaring ports or reading a binding.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Error {
    /// An id is empty or has a character outside `a-z`, `0-9`, `_` and `-`.
    BadId(String),
    /// A port id is declared twice.
    DuplicatePort(String),
    /// A control id is declared twice on one port.
    DuplicateControl { port: String, control: String },
    /// A declaration names a port that does not exist.
    UnknownPort(String),
    /// A binding names a control that does not exist on its port.
    UnknownControl { port: String, control: String },
    /// A control kind is not `button`, `trigger` or `axis`.
    BadKind(String),
    /// A host input does not parse.
    BadInput { text: String, why: String },
    /// A `relift.toml` or mod declaration is malformed.
    BadDeclaration { source: String, why: String },
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::BadId(id) => write!(
                f,
                "`{id}` is not a valid id (use lowercase letters, digits and `_`)"
            ),
            Error::DuplicatePort(p) => write!(f, "port `{p}` is declared twice"),
            Error::DuplicateControl { port, control } => {
                write!(f, "control `{control}` is declared twice on port `{port}`")
            }
            Error::UnknownPort(p) => write!(f, "there is no port `{p}`"),
            Error::UnknownControl { port, control } => {
                write!(f, "port `{port}` has no control `{control}`")
            }
            Error::BadKind(k) => write!(
                f,
                "`{k}` is not a control kind (use button, trigger or axis)"
            ),
            Error::BadInput { text, why } => write!(f, "host input `{text}`: {why}"),
            Error::BadDeclaration { source, why } => write!(f, "{source}: {why}"),
        }
    }
}

impl std::error::Error for Error {}
