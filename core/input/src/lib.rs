//! The input model: ports with a controller kind and controls, bindings from
//! host inputs to those controls, and the two renderings a build needs (the
//! `input` section of the self-description and the C declarations the runtime
//! reads). The model knows no platform: a guest crate supplies its native
//! ports, and `game.toml` or a mod adds extra ports and controls.

mod bind;
mod cdecl;
mod error;
mod extras;
mod json;
mod model;
mod names;
mod native;

pub use bind::{BindingTarget, Bindings, Device, Half, HostInput, KeyBindingSetting};
pub use cdecl::{declarations_c, extras_c_file};
pub use error::Error;
pub use json::input_section;
pub use model::{Control, ControlKind, Origin, Port, PortSet};
pub use names::{HostName, host_names};
pub use native::{
    SwitchLayout, TOUCHPAD_PORT, native_c_file, native_ports, switch_player, touchpad_port,
};
