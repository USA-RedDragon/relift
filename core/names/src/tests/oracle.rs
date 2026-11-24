//! The independent check: `cpp_demangle` must print a host name as the
//! guest name inside `guest::<module>::`.

/// The written-out forms of the abbreviations, with the short spelling a
/// demangler may print instead.
const SHORT: [(&str, &str, &str); 4] = [
    (
        "std::basic_string<char, std::char_traits<char>, std::allocator<char> >",
        "std::string",
        "string",
    ),
    (
        "std::basic_iostream<char, std::char_traits<char> >",
        "std::iostream",
        "iostream",
    ),
    (
        "std::basic_istream<char, std::char_traits<char> >",
        "std::istream",
        "istream",
    ),
    (
        "std::basic_ostream<char, std::char_traits<char> >",
        "std::ostream",
        "ostream",
    ),
];

/// `cpp_demangle`'s text for `s`.
pub fn demangle(s: &str) -> Result<String, String> {
    let sym = cpp_demangle::Symbol::new(s.as_bytes()).map_err(|e| format!("{s}: {e}"))?;
    sym.demangle().map_err(|e| format!("{s}: {e}"))
}

/// One spelling for the names a demangler may print two ways.
fn normalize(s: &str) -> String {
    let mut s = s.to_string();
    for (long, short, leaf) in SHORT {
        s = s.replace(long, short);
        let base = &long[5..long.find('<').unwrap_or(long.len())];
        s = s.replace(&format!("{short}::~{base}("), &format!("{short}::~{leaf}("));
        s = s.replace(&format!("{short}::{base}("), &format!("{short}::{leaf}("));
    }
    s
}

fn cut_clone(s: &str) -> &str {
    s.find(" [clone ").map_or(s, |i| &s[..i])
}

fn cut_sfx_tag(s: &str) -> String {
    match s.find("[abi:sfx") {
        Some(i) => {
            let end = s[i..].find(']').map_or(s.len(), |j| i + j + 1);
            let gap = if s[..i].ends_with('<') && s[end..].starts_with('<') {
                " "
            } else {
                ""
            };
            format!("{}{gap}{}", &s[..i], &s[end..])
        }
        None => s.to_string(),
    }
}

/// Whether `guest` demangles at all, so a mismatch can be told from a name
/// the oracle does not know.
pub fn knows(guest: &str) -> bool {
    demangle(guest).is_ok()
}

/// Checks that `host` prints as `guest` in `guest::<module>::`. A plain C
/// name must print as exactly `guest::<module>::<name>`.
pub fn check(module: &str, guest: &str, host: &str) -> Result<(), String> {
    let h = demangle(host)?;
    if !guest.starts_with("_Z") {
        let want = format!("guest::{module}::{guest}");
        return if h == want {
            Ok(())
        } else {
            Err(format!("{host}: {h:?}, want {want:?}"))
        };
    }
    let g = demangle(guest)?;
    let inner = h.replace(&format!("guest::{module}::"), "");
    let got = normalize(&cut_sfx_tag(&inner));
    let want = normalize(cut_clone(&g));
    if got == want {
        Ok(())
    } else {
        Err(format!(
            "{module}:{guest} -> {host}\n  host:  {got}\n  guest: {want}"
        ))
    }
}
