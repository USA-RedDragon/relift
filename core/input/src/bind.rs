use std::collections::BTreeMap;
use std::fmt;

use crate::error::Error;
use crate::model::PortSet;
use crate::names;

/// Which kind of host device an input comes from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Device {
    /// An SDL3 gamepad.
    Gamepad,
    /// The keyboard.
    Key,
    /// The mouse.
    Mouse,
}

impl Device {
    fn prefix(self) -> &'static str {
        match self {
            Device::Gamepad => "gamepad",
            Device::Key => "key",
            Device::Mouse => "mouse",
        }
    }
}

/// Which part of an input's range a binding uses.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Half {
    /// The whole range.
    Full,
    /// Only the positive part (suffix `+`).
    Positive,
    /// Only the negative part, and a pressed button reads -1 (suffix `-`).
    Negative,
}

/// One host input in a binding: `gamepad:south`, `gamepad2:left_x-`, `key:z`, `mouse:left`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HostInput {
    /// The device.
    pub device: Device,
    /// A 1-based gamepad number; `None` means the gamepad of the control's port.
    pub pad: Option<u8>,
    /// The name from the runtime's table.
    pub name: String,
    /// The part of the range used.
    pub half: Half,
}

fn bad(text: &str, why: &str) -> Error {
    Error::BadInput {
        text: text.to_string(),
        why: why.to_string(),
    }
}

impl HostInput {
    /// Parses one input.
    pub fn parse(text: &str) -> Result<HostInput, Error> {
        let text = text.trim();
        let (dev, rest) = text
            .split_once(':')
            .ok_or_else(|| bad(text, "expected device:name"))?;
        let (device, pad) = if let Some(n) = dev.strip_prefix("gamepad") {
            if n.is_empty() {
                (Device::Gamepad, None)
            } else {
                let n: u8 = n
                    .parse()
                    .map_err(|_| bad(text, "the gamepad number is not a number"))?;
                if n == 0 || n > 8 {
                    return Err(bad(text, "the gamepad number runs from 1 to 8"));
                }
                (Device::Gamepad, Some(n))
            }
        } else if dev == "key" {
            (Device::Key, None)
        } else if dev == "mouse" {
            (Device::Mouse, None)
        } else {
            return Err(bad(text, "the device is gamepad, key or mouse"));
        };
        let (name, half) = if let Some(n) = rest.strip_suffix('+') {
            (n, Half::Positive)
        } else if let Some(n) = rest.strip_suffix('-') {
            (n, Half::Negative)
        } else {
            (rest, Half::Full)
        };
        names::find(device, name).ok_or_else(|| bad(text, "unknown name"))?;
        Ok(HostInput {
            device,
            pad,
            name: name.to_string(),
            half,
        })
    }

    /// Parses a comma separated list; an empty text is an empty list.
    pub fn parse_list(text: &str) -> Result<Vec<HostInput>, Error> {
        let text = text.trim();
        if text.is_empty() {
            return Ok(Vec::new());
        }
        text.split(',').map(HostInput::parse).collect()
    }

    /// The list as comma separated text.
    pub fn list_string(list: &[HostInput]) -> String {
        let parts: Vec<String> = list.iter().map(ToString::to_string).collect();
        parts.join(",")
    }
}

impl fmt::Display for HostInput {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.device.prefix())?;
        if let Some(n) = self.pad {
            write!(f, "{n}")?;
        }
        write!(f, ":{}", self.name)?;
        match self.half {
            Half::Full => Ok(()),
            Half::Positive => write!(f, "+"),
            Half::Negative => write!(f, "-"),
        }
    }
}

/// A port and control a binding applies to.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct BindingTarget {
    /// The port id.
    pub port: String,
    /// The control id.
    pub control: String,
}

/// The `key binding` setting type: its value is the list of host inputs bound to one port and control.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct KeyBindingSetting {
    /// What the binding drives.
    pub target: BindingTarget,
    /// The inputs bound by default.
    pub default: Vec<HostInput>,
}

impl KeyBindingSetting {
    /// The setting key a launcher passes: `bind.<port>.<control>`.
    pub fn key(&self) -> String {
        format!("bind.{}.{}", self.target.port, self.target.control)
    }

    /// Splits a setting key back into its target; `None` when it is not a binding key.
    pub fn target_of_key(key: &str) -> Option<BindingTarget> {
        let rest = key.strip_prefix("bind.")?;
        let (port, control) = rest.split_once('.')?;
        if port.is_empty() || control.is_empty() {
            return None;
        }
        Some(BindingTarget {
            port: port.to_string(),
            control: control.to_string(),
        })
    }

    /// Parses a setting value (comma separated inputs).
    pub fn parse_value(text: &str) -> Result<Vec<HostInput>, Error> {
        HostInput::parse_list(text)
    }
}

/// The bindings in force: the defaults of a port set with the player's changes on top.
#[derive(Clone, Debug, PartialEq)]
pub struct Bindings {
    map: BTreeMap<BindingTarget, Vec<HostInput>>,
}

impl Bindings {
    /// The default bindings of every control.
    pub fn defaults(set: &PortSet) -> Bindings {
        let mut map = BTreeMap::new();
        for p in set.ports() {
            for c in &p.controls {
                map.insert(
                    BindingTarget {
                        port: p.id.clone(),
                        control: c.id.clone(),
                    },
                    c.defaults.clone(),
                );
            }
        }
        Bindings { map }
    }

    /// The inputs bound to a control.
    pub fn get(&self, port: &str, control: &str) -> Option<&[HostInput]> {
        self.map
            .get(&BindingTarget {
                port: port.to_string(),
                control: control.to_string(),
            })
            .map(Vec::as_slice)
    }

    /// Replaces the binding of a control; the port and control must exist in the set.
    pub fn set(
        &mut self,
        set: &PortSet,
        port: &str,
        control: &str,
        inputs: Vec<HostInput>,
    ) -> Result<(), Error> {
        set.check(port, control)?;
        self.map.insert(
            BindingTarget {
                port: port.to_string(),
                control: control.to_string(),
            },
            inputs,
        );
        Ok(())
    }

    /// Applies one `bind.<port>.<control>` setting; other keys return `Ok(false)`.
    pub fn apply_setting(&mut self, set: &PortSet, key: &str, value: &str) -> Result<bool, Error> {
        let Some(t) = KeyBindingSetting::target_of_key(key) else {
            return Ok(false);
        };
        let inputs = KeyBindingSetting::parse_value(value)?;
        self.set(set, &t.port, &t.control, inputs)?;
        Ok(true)
    }

    /// Every binding as `(port, control, inputs)`.
    pub fn entries(&self) -> impl Iterator<Item = (&BindingTarget, &[HostInput])> {
        self.map.iter().map(|(t, v)| (t, v.as_slice()))
    }
}
