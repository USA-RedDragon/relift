//! The hand-written pieces every MIPS profile shares: the lift-time block
//! state ([`rt::Core`]) and the block driver that pairs a branch with its
//! delay slot ([`lift::body`]).

pub(crate) mod lift;
pub(crate) mod rt;
