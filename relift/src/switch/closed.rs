//! The closed-world check of a Switch build: whether the program's NPDM
//! lets it make executable memory at run time that no build could lift
//! ahead of time (code memory, process code mapping, `jit:u`, or `ldr:ro`
//! with an NRO or NRR in its data).

use std::path::Path;

use relift_switch_formats::npdm::Npdm;

/// The supervisor calls that make or map executable memory, with their
/// names.
const CODE_SVCS: [(u32, &str); 5] = [
    (0x4B, "CreateCodeMemory"),
    (0x4C, "ControlCodeMemory"),
    (0x73, "SetProcessMemoryPermission"),
    (0x77, "MapProcessCodeMemory"),
    (0x78, "UnmapProcessCodeMemory"),
];

/// The services that load code into the process from images it hands
/// them.
const LOADER_SERVICES: [&str; 2] = ["ldr:ro", "ro:1"];

/// How many offending files a failure names.
const SHOWN: usize = 5;

/// Checks that every executable byte of the program is in its modules:
/// `npdm` grants no way to make code, or grants only `ldr:ro` (`ro:1`) and
/// no file of the data of `import` (its romfs and add-on content, in
/// whatever form crate::gamedata finds them) is an NRO or NRR image for it
/// to load. Gives a note for the build log when the program
/// may use a code loader.
///
/// # Errors
///
/// What opens executable memory, as a message for the user.
pub(crate) fn check(npdm: Option<&Npdm>, import: &Path) -> Result<Option<String>, String> {
    let Some(npdm) = npdm else {
        return Err(
            "executable memory is not closed: the import has no main.npdm, so relift \
             cannot tell whether the program maps code at run time"
                .into(),
        );
    };
    let svcs = npdm.effective_svcs();
    let open: Vec<String> = CODE_SVCS
        .iter()
        .filter(|(s, _)| svcs.contains(*s))
        .map(|(s, n)| format!("svc {s:#04x} {n}"))
        .collect();
    if !open.is_empty() {
        return Err(format!(
            "executable memory is not closed: the NPDM (ACI0 and ACID) allows {}, so the program \
             can make code at run time that no build can lift",
            open.join(", ")
        ));
    }
    if let Some(s) = npdm.uses_service("jit:u") {
        return Err(format!(
            "executable memory is not closed: the NPDM grants the JIT service (entry {}), so \
             the program can make code at run time that no build can lift",
            s.name
        ));
    }
    let Some(ro) = LOADER_SERVICES
        .iter()
        .find_map(|n| npdm.uses_service(n).map(|s| (n, s)))
    else {
        return Ok(None);
    };
    let mut images = Vec::new();
    let mut files = 0usize;
    let units = crate::gamedata::units(import).map_err(|e| format!("{}: {e}", import.display()))?;
    for u in &units {
        let heads = crate::gamedata::heads(u, 0x14).map_err(|e| e.to_string())?;
        files += heads.len();
        let base = u.found.path();
        for (path, head) in heads {
            if let Some(k) = image_kind(&head) {
                images.push((base.join(path), k));
            }
        }
    }
    if images.is_empty() {
        return Ok(Some(format!(
            "closed world: the NPDM grants {} (entry {}) but the program's data ({files} files) \
             holds no NRO or NRR for it to load",
            ro.0, ro.1.name
        )));
    }
    images.sort();
    let shown: Vec<String> = images
        .iter()
        .take(SHOWN)
        .map(|(p, k)| format!("{} ({k})", p.display()))
        .collect();
    Err(format!(
        "executable memory is not closed: the NPDM grants {} and the program's data holds {} \
         code image{} it can load at run time: {}{}",
        ro.0,
        images.len(),
        if images.len() == 1 { "" } else { "s" },
        shown.join(", "),
        if images.len() > SHOWN { ", ..." } else { "" }
    ))
}

/// `NRO` or `NRR` when a file starting with `head` is one.
fn image_kind(head: &[u8]) -> Option<&'static str> {
    if head.starts_with(b"NRR0") {
        Some("NRR")
    } else if head.get(0x10..0x14) == Some(b"NRO0") {
        Some("NRO")
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use relift_switch_formats::npdm::{build, build_with};
    use std::path::PathBuf;

    fn npdm(services: &[(&str, bool)], svcs: &[u32]) -> Npdm {
        Npdm::parse(&build_with("g", 1, 0x1000, services, Some((svcs, svcs)))).expect("parses")
    }

    fn dir(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("relift-closed-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(d.join("romfs/sub")).expect("mkdir");
        d
    }

    fn none() -> PathBuf {
        PathBuf::from("/nonexistent-relift-import")
    }

    #[test]
    fn plain_heap_and_memory_svcs_are_closed() {
        let n = npdm(&[("fsp-srv", false)], &[0x01, 0x02, 0x04, 0x13, 0x2C]);
        assert!(check(Some(&n), &none()).is_ok());
    }

    #[test]
    fn code_memory_svcs_open_the_world() {
        let n = npdm(&[], &[0x01, 0x4B, 0x4C]);
        let e = check(Some(&n), &none()).expect_err("open");
        assert!(
            e.contains("svc 0x4b CreateCodeMemory, svc 0x4c ControlCodeMemory"),
            "{e}"
        );
    }

    #[test]
    fn an_svc_only_the_aci0_allows_stays_closed() {
        let b = build_with("g", 1, 0x1000, &[], Some((&[0x4B], &[])));
        let n = Npdm::parse(&b).expect("parses");
        assert!(check(Some(&n), &none()).is_ok());
    }

    #[test]
    fn jit_opens_the_world() {
        let n = npdm(&[("jit:u", false)], &[]);
        assert!(check(Some(&n), &none()).expect_err("open").contains("JIT"));
    }

    #[test]
    fn the_loader_is_closed_without_images_and_open_with_one() {
        let n = npdm(&[("ldr:ro", false)], &[]);
        let d = dir("ro");
        relift_cache::write_if_changed(&d.join("romfs/sub/a.bin"), b"plain data").expect("write");
        let note = check(Some(&n), &d).expect("closed").expect("a note");
        assert!(note.contains("holds no NRO or NRR"), "{note}");
        let mut nro = vec![0u8; 0x20];
        nro[0x10..0x14].copy_from_slice(b"NRO0");
        relift_cache::write_if_changed(&d.join("romfs/sub/plugin.nro"), &nro).expect("write");
        std::fs::create_dir_all(d.join("dlc/0100000000001001/0/romfs")).expect("mkdir");
        relift_cache::write_if_changed(&d.join("dlc/0100000000001001/0/romfs/x.nrr"), b"NRR0....")
            .expect("write");
        let e = check(Some(&n), &d).expect_err("open");
        assert!(e.contains("2 code images"), "{e}");
        assert!(
            e.contains("plugin.nro (NRO)") && e.contains("x.nrr (NRR)"),
            "{e}"
        );
        let img = d.with_extension("img");
        std::fs::create_dir_all(&img).expect("mkdir");
        crate::gamedata::write_images(&d, &img, false).expect("images");
        let e = check(Some(&n), &img).expect_err("open in an image");
        assert!(e.contains("2 code images"), "{e}");
        std::fs::remove_dir_all(&d).expect("clean");
        std::fs::remove_dir_all(&img).expect("clean");
    }

    #[test]
    fn no_npdm_is_not_closed() {
        assert!(
            check(None, &none())
                .expect_err("open")
                .contains("no main.npdm")
        );
        let n = Npdm::parse(&build("g", 1, 0x1000, &[])).expect("parses");
        assert!(check(Some(&n), &none()).is_ok());
    }
}
