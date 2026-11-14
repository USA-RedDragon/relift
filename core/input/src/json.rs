use crate::bind::HostInput;
use crate::model::{Origin, PortSet};

/// The `input` section of the self-description for a port set. A port's
/// number is its place in the set; native ports come first.
pub fn input_section(set: &PortSet) -> relift_info::Input {
    let ports = set
        .ports()
        .iter()
        .enumerate()
        .map(|(i, p)| relift_info::Port {
            id: p.id.clone(),
            port: u32::try_from(i).expect("few ports"),
            label: p.label.clone(),
            origin: p.origin.label(),
            kind: p.kind.clone(),
            controls: p
                .controls
                .iter()
                .map(|c| relift_info::Control {
                    id: c.id.clone(),
                    label: c.label.clone().unwrap_or_else(|| c.id.clone()),
                    kind: c.kind.as_str().to_string(),
                    origin: c.origin.label(),
                    defaults: c.defaults.iter().map(HostInput::to_string).collect(),
                })
                .collect(),
            extra: p.origin != Origin::Native
                || p.controls.iter().any(|c| c.origin != Origin::Native),
        })
        .collect();
    relift_info::Input { ports }
}
