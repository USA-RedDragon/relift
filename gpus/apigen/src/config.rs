//! The generator configuration (`vkgen.toml`): the pinned registry, the core
//! versions and extensions to forward, and the commands and structs whose
//! translation is written by hand in the runtime.

use std::collections::BTreeMap;

use crate::Error;

/// A second registry file a target reads (`glx.xml` beside `gl.xml`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Extra {
    /// The file name, matched against the `--xml` paths.
    pub file: String,
    /// Where CI downloads it from.
    pub url: String,
    /// Its sha256, lowercase hex.
    pub sha256: String,
    /// The registry's `api` attribute of the features to read (`glx`).
    pub api: String,
    /// The features to read.
    pub features: Vec<String>,
    /// Extension commands to read too.
    pub commands: Vec<String>,
    /// True when every command it selects is written by hand.
    pub hand: bool,
}

/// The generator configuration. Several files merge: lists concatenate, and
/// the registry pin and hooks of a later file win.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Config {
    /// The guest ABI the output is for (`a64` when no file names one).
    pub abi: String,
    /// The forwarded API: `vulkan` (the default), `gl` or `egl`.
    pub api: String,
    /// The soname the guest links (OpenGL and EGL).
    pub soname: String,
    /// The host lookup function for commands the library does not export (`glXGetProcAddressARB`).
    pub proc_address: String,
    /// The registry file name, matched against the `--xml` paths (`vk.xml` when empty).
    pub registry_file: String,
    /// Further registry files.
    pub extra: Vec<Extra>,
    /// The registry's `api` attribute of the selected features (`gl`, `egl`).
    pub xml_api: String,
    /// Platform types the registry names but does not define, as C types (`EGLint = "int32_t"`).
    pub types: BTreeMap<String, String>,
    /// Pointer types the guest passes as integers (`EGLClientBuffer`).
    pub as_integer: Vec<String>,
    /// Element counts the registry lacks: `command.param` to a parameter name.
    pub lens: BTreeMap<String, String>,
    /// `void *` parameters that point at one pointer-sized integer (an X11 XID): `command.param`.
    pub word_pointers: Vec<String>,
    /// Pointer-sized list types terminated by a value at an even index (`EGLAttrib = "0x3038"`).
    pub terminated: BTreeMap<String, String>,
    /// GL parameters that hold a buffer offset while a buffer is bound: binding query to `command.param` list.
    pub offsets: BTreeMap<String, Vec<String>>,
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
    /// Extension commands of an OpenGL-family registry to forward too.
    pub commands: Vec<String>,
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
    /// Command name to a runtime function called after the host call and
    /// the copy of its results back to the guest.
    pub after: BTreeMap<String, String>,
    /// Command name to a runtime function returning a `VkResult`, called
    /// before the host call: an error result ends the call with it.
    pub check: BTreeMap<String, String>,
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
                .ok_or_else(|| Error(format!("{k} must be a string")))?
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
            if !["target", "registry", "api", "thunks", "hooks", "types"].contains(&key.as_str()) {
                return Err(Error(format!("configuration: unknown section [{key}]")));
            }
        }
        if let Some(r) = table(&t, "target")? {
            for (k, out) in [
                ("abi", &mut self.abi),
                ("api", &mut self.api),
                ("soname", &mut self.soname),
                ("proc_address", &mut self.proc_address),
            ] {
                if let Some(v) = r.get(k) {
                    *out = v
                        .as_str()
                        .ok_or_else(|| Error(format!("target.{k} must be a string")))?
                        .to_string();
                }
            }
        }
        if let Some(ty) = table(&t, "types")? {
            map(ty, &mut self.types)?;
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
            if let Some(v) = get("file") {
                self.registry_file = v;
            }
            if let Some(x) = r.get("extra") {
                let a = x
                    .as_array()
                    .ok_or_else(|| Error("registry.extra must be a list of tables".into()))?;
                for e in a {
                    let e = e
                        .as_table()
                        .ok_or_else(|| Error("registry.extra must be a list of tables".into()))?;
                    let get = |k: &str| e.get(k).and_then(|v| v.as_str()).unwrap_or("").to_string();
                    let mut x = Extra {
                        file: get("file"),
                        url: get("url"),
                        sha256: get("sha256").to_lowercase(),
                        api: get("api"),
                        hand: e
                            .get("hand")
                            .and_then(toml::Value::as_bool)
                            .unwrap_or(false),
                        ..Extra::default()
                    };
                    strings(e, "features", &mut x.features)?;
                    strings(e, "commands", &mut x.commands)?;
                    self.extra.push(x);
                }
            }
        }
        if let Some(a) = table(&t, "api")? {
            strings(a, "features", &mut self.features)?;
            strings(a, "extensions", &mut self.extensions)?;
            strings(a, "host_extensions", &mut self.host_extensions)?;
            strings(a, "commands", &mut self.commands)?;
            if let Some(v) = a.get("name") {
                self.xml_api = v
                    .as_str()
                    .ok_or_else(|| Error("api.name must be a string".into()))?
                    .to_string();
            }
        }
        if let Some(a) = table(&t, "thunks")? {
            strings(a, "hand_commands", &mut self.hand_commands)?;
            strings(a, "hand_structs", &mut self.hand_structs)?;
            strings(a, "drop_types", &mut self.drop_types)?;
            strings(a, "as_integer", &mut self.as_integer)?;
            strings(a, "word_pointers", &mut self.word_pointers)?;
            if let Some(l) = table(a, "lens")? {
                map(l, &mut self.lens)?;
            }
            if let Some(l) = table(a, "terminated")? {
                map(l, &mut self.terminated)?;
            }
            if let Some(o) = table(a, "offsets")? {
                for (k, v) in o {
                    let mut list = Vec::new();
                    let mut one = toml::Table::new();
                    one.insert("v".into(), v.clone());
                    strings(&one, "v", &mut list).map_err(|_| {
                        Error(format!("thunks.offsets.{k} must be a list of strings"))
                    })?;
                    self.offsets.entry(k.clone()).or_default().extend(list);
                }
            }
        }
        if let Some(h) = table(&t, "hooks")? {
            if let Some(b) = table(h, "before")? {
                map(b, &mut self.before)?;
            }
            if let Some(b) = table(h, "after")? {
                map(b, &mut self.after)?;
            }
            if let Some(b) = table(h, "check")? {
                map(b, &mut self.check)?;
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
            "[registry]\ntag = \"v1\"\nsha256 = \"AB\"\n[api]\nfeatures = [\"VK_VERSION_1_0\"]\nextensions = [\"A\"]\n[hooks.before]\nvkQueueSubmit = \"f\"\n[hooks.check]\nvkCreateImage = \"g\"\n",
        )
        .unwrap();
        c.merge("[api]\nextensions = [\"B\", \"A\"]\n").unwrap();
        assert_eq!(c.registry_sha256, "ab");
        assert_eq!(c.extensions, ["A", "B"]);
        assert_eq!(c.before["vkQueueSubmit"], "f");
        assert_eq!(c.check["vkCreateImage"], "g");
        assert!(c.merge("[nope]\n").is_err());
        c.merge("[target]\nabi = \"i386\"\n").unwrap();
        assert_eq!(c.abi, "i386");
    }
}
