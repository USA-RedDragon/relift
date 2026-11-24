//! relift-gpu-apigen: generates the GPU-API forwarding of the runtime from
//! the pinned Khronos registries, for each guest ABI ([`abi`]): Vulkan for
//! the AArch64 guest (`runtime/gpuapi/vk/gen/`) and Vulkan for an
//! i386 guest.
//!
//! A guest that calls Vulkan through `vkGetInstanceProcAddr` gets guest
//! addresses of thunks. Each thunk has the lifted-code signature, reads its
//! arguments from the guest context, translates guest structs, pointers,
//! handles and pNext chains to host ones and calls the host driver. The
//! generator emits the types (from vk.xml, so no host Vulkan headers are
//! needed), the layout asserts, the converters, the thunks and the command
//! table; commands and structs that need judgment are written by hand in
//! the runtime and listed in the configuration.

pub mod abi;
pub mod config;
pub mod emit_c;
pub mod emit_i386;
pub mod emit_types;
pub mod model;
pub mod registry;
pub mod select;

use std::fmt;

use sha2::{Digest, Sha256};

pub use config::Config;

/// A generator error: a message for the user.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error(pub String);

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

/// One output file: its name inside the output directory and its text.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct File {
    /// File name.
    pub name: &'static str,
    /// Contents.
    pub text: String,
}

/// Lowercase hex sha256 of bytes.
pub fn sha256_hex(bytes: &[u8]) -> String {
    Sha256::digest(bytes)
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect()
}

/// Generates every output file from vk.xml text. Refuses a registry whose
/// sha256 differs from the configuration's pin, unless the pin is empty.
pub fn generate(xml: &str, cfg: &Config) -> Result<Vec<File>, Error> {
    if !cfg.registry_sha256.is_empty() {
        let got = sha256_hex(xml.as_bytes());
        if got != cfg.registry_sha256 {
            return Err(Error(format!(
                "vk.xml sha256 is {got}, the configuration pins {} ({})",
                cfg.registry_sha256, cfg.registry_tag
            )));
        }
    }
    let abi = if cfg.abi.is_empty() {
        abi::A64
    } else {
        abi::Abi::by_name(&cfg.abi)
            .ok_or_else(|| Error(format!("unknown target ABI {}", cfg.abi)))?
    };
    let reg = registry::Registry::parse(xml)?;
    let sel = select::Selection::new(&reg, cfg)?;
    let mut m = model::Model::for_abi(&reg, &sel, cfg, abi)?;
    let types = emit_types::emit(&mut m, &cfg.registry_tag)?;
    if !abi.is_host() {
        let f = emit_i386::emit(&mut m)?;
        return Ok(vec![
            File {
                name: "vkg_types.h",
                text: types,
            },
            File {
                name: "vki.c",
                text: f.thunks,
            },
            File {
                name: "vki_layout.c",
                text: f.layout,
            },
        ]);
    }
    let c = emit_c::emit(&mut m)?;
    Ok(vec![
        File {
            name: "vkg_types.h",
            text: types,
        },
        File {
            name: "vkg.h",
            text: c.header,
        },
        File {
            name: "vkg_convert.c",
            text: c.convert,
        },
        File {
            name: "vkg_thunks.c",
            text: c.thunks,
        },
    ])
}
