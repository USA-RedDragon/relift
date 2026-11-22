use toml::{Table, Value};

use crate::bind::HostInput;
use crate::error::Error;
use crate::model::{Control, ControlKind, Origin, Port, PortSet};

fn fail(source: &str, why: impl Into<String>) -> Error {
    Error::BadDeclaration {
        source: source.to_string(),
        why: why.into(),
    }
}

fn text<'a>(source: &str, t: &'a Table, key: &str, at: &str) -> Result<Option<&'a str>, Error> {
    match t.get(key) {
        None => Ok(None),
        Some(Value::String(s)) => Ok(Some(s)),
        Some(_) => Err(fail(source, format!("{at}.{key} must be a string"))),
    }
}

fn table<'a>(source: &str, v: &'a Value, at: &str) -> Result<&'a Table, Error> {
    v.as_table()
        .ok_or_else(|| fail(source, format!("{at} must be a table")))
}

fn read_control(
    source: &str,
    origin: &Origin,
    id: &str,
    v: &Value,
    at: &str,
) -> Result<Control, Error> {
    let t = table(source, v, at)?;
    let kind = match text(source, t, "kind", at)? {
        Some(k) => ControlKind::parse(k)?,
        None => ControlKind::Button,
    };
    let defaults = match text(source, t, "default", at)? {
        Some(d) => HostInput::parse_list(d)?,
        None => Vec::new(),
    };
    Ok(Control {
        id: id.to_string(),
        kind,
        label: text(source, t, "label", at)?.map(str::to_string),
        origin: origin.clone(),
        defaults,
    })
}

impl PortSet {
    /// Applies the `[input.port.<id>]` tables of a `relift.toml` or a mod
    /// manifest. A table for a new port needs a `kind`; a table for an
    /// existing port may only add controls. `source` names the file in errors.
    pub fn extend_from_toml(
        &mut self,
        source: &str,
        origin: Origin,
        toml_text: &str,
    ) -> Result<(), Error> {
        let doc: Table = toml_text
            .parse()
            .map_err(|e: toml::de::Error| fail(source, e.message()))?;
        let Some(input) = doc.get("input") else {
            return Ok(());
        };
        let input = table(source, input, "input")?;
        let Some(ports) = input.get("port") else {
            return Ok(());
        };
        for (id, v) in table(source, ports, "input.port")? {
            let at = format!("input.port.{id}");
            let t = table(source, v, &at)?;
            let controls = match t.get("control") {
                Some(c) => table(source, c, &format!("{at}.control"))?.clone(),
                None => Table::new(),
            };
            let mut read = Vec::new();
            for (cid, cv) in &controls {
                read.push(read_control(
                    source,
                    &origin,
                    cid,
                    cv,
                    &format!("{at}.control.{cid}"),
                )?);
            }
            if let Some(existing) = self.port(id) {
                if let Some(k) = text(source, t, "kind", &at)?
                    && k != existing.kind
                {
                    return Err(fail(
                        source,
                        format!(
                            "{at}.kind `{k}` differs from the port's kind `{}`",
                            existing.kind
                        ),
                    ));
                }
                for c in read {
                    self.add_control(id, c)?;
                }
            } else {
                let kind = text(source, t, "kind", &at)?
                    .ok_or_else(|| fail(source, format!("{at} is a new port and needs a kind")))?;
                let mut p = Port::new(id, kind, read);
                p.label = text(source, t, "label", &at)?.map(str::to_string);
                p.origin = origin.clone();
                self.add_port(p)?;
            }
        }
        Ok(())
    }
}
