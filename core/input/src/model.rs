use crate::bind::HostInput;
use crate::error::Error;

/// How a control reads: a button is 0 or 1, a trigger runs 0 to 1, an axis
/// runs -1 to 1.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ControlKind {
    /// Pressed or not.
    Button,
    /// An analog value from 0 to 1.
    Trigger,
    /// An analog value from -1 to 1.
    Axis,
}

impl ControlKind {
    /// The name used in `game.toml` and the self-description.
    pub fn as_str(self) -> &'static str {
        match self {
            ControlKind::Button => "button",
            ControlKind::Trigger => "trigger",
            ControlKind::Axis => "axis",
        }
    }

    /// Parses `button`, `trigger` or `axis`.
    pub fn parse(s: &str) -> Result<ControlKind, Error> {
        match s {
            "button" => Ok(ControlKind::Button),
            "trigger" => Ok(ControlKind::Trigger),
            "axis" => Ok(ControlKind::Axis),
            other => Err(Error::BadKind(other.to_string())),
        }
    }

    /// The number the C declarations use.
    pub fn code(self) -> u8 {
        match self {
            ControlKind::Button => 0,
            ControlKind::Trigger => 1,
            ControlKind::Axis => 2,
        }
    }
}

/// Who declared a port or control.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Origin {
    /// The platform.
    Native,
    /// The game repo's `game.toml`.
    Game,
    /// A mod, by name.
    Mod(String),
}

impl Origin {
    /// `native`, `game` or `mod:<name>`.
    pub fn label(&self) -> String {
        match self {
            Origin::Native => "native".to_string(),
            Origin::Game => "game".to_string(),
            Origin::Mod(m) => format!("mod:{m}"),
        }
    }
}

/// One control of a port.
#[derive(Clone, Debug, PartialEq)]
pub struct Control {
    /// Id, unique on its port.
    pub id: String,
    /// How it reads.
    pub kind: ControlKind,
    /// A human label, when the declaration gives one.
    pub label: Option<String>,
    /// Who declared it.
    pub origin: Origin,
    /// The inputs bound when the player changes nothing.
    pub defaults: Vec<HostInput>,
}

impl Control {
    /// A native control with no label and no default.
    pub fn new(id: &str, kind: ControlKind) -> Control {
        Control {
            id: id.to_string(),
            kind,
            label: None,
            origin: Origin::Native,
            defaults: Vec::new(),
        }
    }

    /// Sets the label.
    pub fn labeled(mut self, label: &str) -> Control {
        self.label = Some(label.to_string());
        self
    }

    /// Sets the default bindings; panics on text that does not parse, so use it for fixed tables only.
    pub fn bound(mut self, inputs: &str) -> Control {
        self.defaults = HostInput::parse_list(inputs).expect("a fixed default binding");
        self
    }
}

/// One input port: a controller kind and its controls.
#[derive(Clone, Debug, PartialEq)]
pub struct Port {
    /// Id, unique in the set.
    pub id: String,
    /// The controller kind, such as `gba-pad`.
    pub kind: String,
    /// A human label for the port.
    pub label: Option<String>,
    /// Who declared it.
    pub origin: Origin,
    /// Its controls, in display order.
    pub controls: Vec<Control>,
}

impl Port {
    /// A native port.
    pub fn new(id: &str, kind: &str, controls: Vec<Control>) -> Port {
        Port {
            id: id.to_string(),
            kind: kind.to_string(),
            label: None,
            origin: Origin::Native,
            controls,
        }
    }

    /// Sets the label.
    pub fn labeled(mut self, label: &str) -> Port {
        self.label = Some(label.to_string());
        self
    }

    /// The control with this id.
    pub fn control(&self, id: &str) -> Option<&Control> {
        self.controls.iter().find(|c| c.id == id)
    }
}

pub(crate) fn check_id(id: &str) -> Result<(), Error> {
    let ok = !id.is_empty()
        && id
            .bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_');
    if ok {
        Ok(())
    } else {
        Err(Error::BadId(id.to_string()))
    }
}

/// The ports of one build: the platform's native ports, then the extra ones.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct PortSet {
    ports: Vec<Port>,
}

impl PortSet {
    /// A set of native ports.
    pub fn native(ports: Vec<Port>) -> Result<PortSet, Error> {
        let mut set = PortSet::default();
        for p in ports {
            set.add_port(p)?;
        }
        Ok(set)
    }

    /// The ports, in declaration order.
    pub fn ports(&self) -> &[Port] {
        &self.ports
    }

    /// The port with this id.
    pub fn port(&self, id: &str) -> Option<&Port> {
        self.ports.iter().find(|p| p.id == id)
    }

    /// Adds a port; ids must be valid and new.
    pub fn add_port(&mut self, port: Port) -> Result<(), Error> {
        check_id(&port.id)?;
        if self.port(&port.id).is_some() {
            return Err(Error::DuplicatePort(port.id));
        }
        let mut seen: Vec<&str> = Vec::new();
        for c in &port.controls {
            check_id(&c.id)?;
            if seen.contains(&c.id.as_str()) {
                return Err(Error::DuplicateControl {
                    port: port.id.clone(),
                    control: c.id.clone(),
                });
            }
            seen.push(&c.id);
        }
        self.ports.push(port);
        Ok(())
    }

    /// Adds a control to an existing port; the id must be new on that port.
    pub fn add_control(&mut self, port: &str, control: Control) -> Result<(), Error> {
        check_id(&control.id)?;
        let p = self
            .ports
            .iter_mut()
            .find(|p| p.id == port)
            .ok_or_else(|| Error::UnknownPort(port.to_string()))?;
        if p.control(&control.id).is_some() {
            return Err(Error::DuplicateControl {
                port: port.to_string(),
                control: control.id,
            });
        }
        p.controls.push(control);
        Ok(())
    }

    /// What the game repo or a mod added: new ports whole, and for a native
    /// port only its added controls.
    pub fn extras(&self) -> Vec<Port> {
        let mut out = Vec::new();
        for p in &self.ports {
            let controls: Vec<Control> = p
                .controls
                .iter()
                .filter(|c| p.origin != Origin::Native || c.origin != Origin::Native)
                .cloned()
                .collect();
            if p.origin != Origin::Native || !controls.is_empty() {
                out.push(Port {
                    controls,
                    ..p.clone()
                });
            }
        }
        out
    }

    /// Checks that a port and control exist.
    pub fn check(&self, port: &str, control: &str) -> Result<&Control, Error> {
        let p = self
            .port(port)
            .ok_or_else(|| Error::UnknownPort(port.to_string()))?;
        p.control(control).ok_or_else(|| Error::UnknownControl {
            port: port.to_string(),
            control: control.to_string(),
        })
    }
}
