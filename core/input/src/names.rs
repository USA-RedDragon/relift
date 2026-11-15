use std::sync::OnceLock;

use crate::bind::Device;

const HEADER: &str = include_str!("../../../runtime/core/include/relift/input_names.h");

/// One host input name from the runtime's shared table.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HostName {
    /// Which device the name belongs to.
    pub device: Device,
    /// True for a stick axis name (gamepad device only).
    pub axis: bool,
    /// The name as written in a binding.
    pub name: String,
    /// The number the runtime uses for it.
    pub value: u32,
}

fn parse() -> Vec<HostName> {
    let mut out = Vec::new();
    for line in HEADER.lines() {
        let Some(rest) = line.trim().strip_prefix("X(") else {
            continue;
        };
        let Some(rest) = rest.strip_suffix(')').or_else(|| rest.strip_suffix(") \\")) else {
            continue;
        };
        let parts: Vec<&str> = rest.split(',').map(str::trim).collect();
        if parts.len() != 3 {
            continue;
        }
        let name = parts[1].trim_matches('"').to_string();
        let Ok(value) = parts[2].parse() else {
            continue;
        };
        let (device, axis) = match parts[0] {
            "GAMEPAD" => (Device::Gamepad, false),
            "AXIS" => (Device::Gamepad, true),
            "KEY" => (Device::Key, false),
            "MOUSE" => (Device::Mouse, false),
            "TOUCH" => (Device::Touch, false),
            _ => continue,
        };
        out.push(HostName {
            device,
            axis,
            name,
            value,
        });
    }
    out
}

/// Every host input name the runtime knows, read from `input_names.h`.
pub fn host_names() -> &'static [HostName] {
    static TABLE: OnceLock<Vec<HostName>> = OnceLock::new();
    TABLE.get_or_init(parse)
}

pub(crate) fn find(device: Device, name: &str) -> Option<&'static HostName> {
    host_names()
        .iter()
        .find(|n| n.device == device && n.name == name)
}
