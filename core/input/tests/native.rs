use std::path::PathBuf;

use relift_input::{Bindings, SwitchLayout, native_c_file, native_ports, switch_player};

fn runtime(path: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../runtime/guests")
        .join(path)
}

fn check_file(path: &str, want: String) {
    let p = runtime(path);
    if std::env::var_os("RELIFT_BLESS").is_some() {
        relift_cache::write_if_changed(&p, want.as_bytes()).unwrap();
    }
    let have = std::fs::read_to_string(&p).unwrap_or_default();
    assert_eq!(
        have, want,
        "{path} is stale; run RELIFT_BLESS=1 cargo test -p relift-input"
    );
}

#[test]
fn switch_has_eight_players_and_touch() {
    let s = native_ports("switch").unwrap();
    let ids: Vec<&str> = s.ports().iter().map(|p| p.id.as_str()).collect();
    assert_eq!(
        ids,
        ["p1", "p2", "p3", "p4", "p5", "p6", "p7", "p8", "touch"]
    );
    let p = s.port("p3").unwrap();
    assert_eq!(p.kind, "Switch Pro Controller");
    assert_eq!(p.controls.len(), 20);
    assert_eq!(
        Bindings::defaults(&s).get("p8", "a").unwrap()[0].to_string(),
        "gamepad:east"
    );
    assert_eq!(s.port("touch").unwrap().controls[0].id, "press");
}

#[test]
fn joy_con_layouts_are_subsets() {
    let pair = switch_player(1, SwitchLayout::JoyConPair);
    let left = switch_player(1, SwitchLayout::JoyConLeft);
    let right = switch_player(2, SwitchLayout::JoyConRight);
    assert_eq!(pair.controls.len(), 20);
    assert_eq!(left.controls.len(), 10);
    assert_eq!(right.controls.len(), 10);
    assert!(left.control("dpad_up").is_some() && left.control("a").is_none());
    assert!(right.control("a").is_some() && right.control("dpad_up").is_none());
    assert_eq!(right.id, "p2");
}

#[test]
fn handhelds_have_one_pad() {
    for (platform, controls, ports) in [("gb", 8, 1), ("gbc", 8, 1), ("gba", 10, 1), ("nds", 12, 2)]
    {
        let s = native_ports(platform).unwrap();
        assert_eq!(s.ports().len(), ports, "{platform}");
        assert_eq!(s.ports()[0].controls.len(), controls, "{platform}");
    }
    assert_eq!(native_ports("nds").unwrap().ports()[1].id, "touch");
    assert!(native_ports("n64").is_err());
}

#[test]
fn generated_c_files_are_current() {
    check_file(
        "switch/ports_gen.c",
        native_c_file("switch", "rl_switch_ports").unwrap(),
    );
    let mut gb = String::new();
    for (platform, symbol) in [("gb", "rl_gb_ports"), ("gbc", "rl_gbc_ports")] {
        let f = native_c_file(platform, symbol).unwrap();
        if gb.is_empty() {
            gb.push_str(&f);
        } else {
            gb.push('\n');
            gb.push_str(f.split_once("#include <relift/ports.h>\n\n").unwrap().1);
        }
    }
    check_file("gb/ports_gen.c", gb);
    check_file(
        "gba/ports_gen.c",
        native_c_file("gba", "rl_gba_ports").unwrap(),
    );
}
