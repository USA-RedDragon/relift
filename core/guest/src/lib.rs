//! The guest contract and vocabulary: the traits a guest implements and the ids
//! every stage shares.

/// How a guest turns its own symbol names into host names.
///
/// The symbol scheme belongs to the guest (Itanium remangling for the Switch,
/// another scheme or none for a later guest), so the core only sees this
/// trait and never a demangler.
pub trait Naming {
    /// The host identifier for `symbol` in `module`, or `None` when the guest
    /// cannot name it and the caller falls back to an address-based name.
    fn host_name(&self, module: &str, symbol: &str) -> Option<String>;
}
