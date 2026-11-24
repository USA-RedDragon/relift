//! The Switch's part of [`crate::build`]'s pipeline: the runtime layers a
//! Switch program compiles, copies into its build directory and includes.

use crate::layers::Layers;

/// The guest address space a Switch program reserves at start: the
/// compact layout's, about 37 GiB.
pub const RESERVATION: u64 = 37 << 30;

/// The Switch runtime: the directories compiled into the runtime library,
/// the ones copied into the project's build directory and the include
/// directories of lifted code and of the runtime.
pub(crate) const SWITCH_LAYERS: Layers = Layers {
    dirs: &["core", "cpus/arm64", "guests/switch"],
    mirror: &[
        "core",
        "cpus/arm64",
        "gpuapi",
        "guests/switch",
        "host",
        "third_party",
    ],
    lifted_inc: &["core/include", "cpus/arm64/include"],
    rt_inc: &[
        "core/include",
        "core",
        "cpus/arm64/include",
        "cpus/arm64",
        "guests/switch",
        "gpuapi/vk",
    ],
    ..Layers::BASE
};
