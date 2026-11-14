//! The generator configuration (`vkgen.toml`): the pinned registry, the core
//! versions and extensions to forward, and the commands and structs whose
//! translation is written by hand in the runtime.

use std::collections::BTreeMap;

use crate::Error;

/// The generator configuration. Several files merge: lists concatenate, and
/// the registry pin and hooks of a later file win.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Config {
    /// The registry tag the output was generated from (`v1.4.363`).
    pub registry_tag: String,
    /// Where CI downloads vk.xml from.
    pub registry_url: String,
    /// sha256 of vk.xml, lowercase hex; the generator refuses any other file.
    pub registry_sha256: String,
    /// Core versions (`VK_VERSION_1_0`); the features they depend on follow.
    pub features: Vec<String>,
    /// Extensions the guest can see.
    pub extensions: Vec<String>,
    /// Extensions only the runtime uses on the host: their commands are loaded, never given to the guest.
    pub host_extensions: Vec<String>,
    /// Commands whose thunk is written by hand (`vkh_<name>`).
    pub hand_commands: Vec<String>,
    /// Structs whose conversion is written by hand (`vkh_in_<name>`).
    pub hand_structs: Vec<String>,
    /// Types always passed to the host as NULL (`VkAllocationCallbacks`).
    pub drop_types: Vec<String>,
    /// Command name to a runtime function called before the host call.
    pub before: BTreeMap<String, String>,
    /// Command name to a runtime function called after the host call.
    pub after: BTreeMap<String, String>,
}

fn strings(t: &toml::Table, key: &str, out: &mut Vec<String>) -> Result<(), Error> {
    let Some(v) = t.get(key) else { return Ok(()) };
    let a = v
        .as_array()
        .ok_or_else(|| Error(format!("{key} must be a list of strings")))?;
    for s in a {
        out.push(
            s.as_str()
                .ok_or_else(|| Error(format!("{key} must be a list of strings")))?
                .to_string(),
        );
    }
    Ok(())
}

fn table<'a>(t: &'a toml::Table, key: &str) -> Result<Option<&'a toml::Table>, Error> {
    match t.get(key) {
        None => Ok(None),
        Some(v) => v
            .as_table()
            .map(Some)
            .ok_or_else(|| Error(format!("[{key}] must be a table"))),
    }
}

fn map(t: &toml::Table, out: &mut BTreeMap<String, String>) -> Result<(), Error> {
    for (k, v) in t {
        out.insert(
            k.clone(),
            v.as_str()
                .ok_or_else(|| Error(format!("hook {k} must name a function")))?
                .to_string(),
        );
    }
    Ok(())
}

impl Config {
    /// Merges one TOML file's text into the configuration.
    pub fn merge(&mut self, text: &str) -> Result<(), Error> {
        let t: toml::Table = text
            .parse()
            .map_err(|e| Error(format!("configuration: {e}")))?;
        for key in t.keys() {
            if !["registry", "api", "thunks", "hooks"].contains(&key.as_str()) {
                return Err(Error(format!("configuration: unknown section [{key}]")));
            }
        }
        if let Some(r) = table(&t, "registry")? {
            let get = |k: &str| r.get(k).and_then(|v| v.as_str()).map(str::to_string);
            if let Some(v) = get("tag") {
                self.registry_tag = v;
            }
            if let Some(v) = get("url") {
                self.registry_url = v;
            }
            if let Some(v) = get("sha256") {
                self.registry_sha256 = v.to_lowercase();
            }
        }
        if let Some(a) = table(&t, "api")? {
            strings(a, "features", &mut self.features)?;
            strings(a, "extensions", &mut self.extensions)?;
            strings(a, "host_extensions", &mut self.host_extensions)?;
        }
        if let Some(a) = table(&t, "thunks")? {
            strings(a, "hand_commands", &mut self.hand_commands)?;
            strings(a, "hand_structs", &mut self.hand_structs)?;
            strings(a, "drop_types", &mut self.drop_types)?;
        }
        if let Some(h) = table(&t, "hooks")? {
            if let Some(b) = table(h, "before")? {
                map(b, &mut self.before)?;
            }
            if let Some(b) = table(h, "after")? {
                map(b, &mut self.after)?;
            }
        }
        for l in [
            &mut self.features,
            &mut self.extensions,
            &mut self.host_extensions,
            &mut self.hand_commands,
            &mut self.hand_structs,
            &mut self.drop_types,
        ] {
            let mut seen = std::collections::BTreeSet::new();
            l.retain(|x| seen.insert(x.clone()));
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::Config;

    #[test]
    fn files_merge() {
        let mut c = Config::default();
        c.merge(
            "[registry]\ntag = \"v1\"\nsha256 = \"AB\"\n[api]\nfeatures = [\"VK_VERSION_1_0\"]\nextensions = [\"A\"]\n[hooks.before]\nvkQueueSubmit = \"f\"\n",
        )
        .unwrap();
        c.merge("[api]\nextensions = [\"B\", \"A\"]\n").unwrap();
        assert_eq!(c.registry_sha256, "ab");
        assert_eq!(c.extensions, ["A", "B"]);
        assert_eq!(c.before["vkQueueSubmit"], "f");
        assert!(c.merge("[nope]\n").is_err());
    }
}
