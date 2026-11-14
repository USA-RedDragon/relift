use relift_input::{
    Bindings, Control, ControlKind, Device, Error, HostInput, KeyBindingSetting, Origin, Port,
    PortSet, declarations_c, host_names, input_section,
};

fn pad() -> PortSet {
    PortSet::native(vec![Port::new(
        "p1",
        "GBA pad",
        vec![
            Control::new("a", ControlKind::Button).bound("gamepad:east,key:x"),
            Control::new("l", ControlKind::Button).labeled("L shoulder"),
            Control::new("stick", ControlKind::Axis),
        ],
    )])
    .unwrap()
}

#[test]
fn the_shared_name_table_is_read() {
    let n = host_names();
    assert!(n.len() > 90);
    assert!(
        n.iter()
            .any(|h| h.device == Device::Key && h.name == "z" && h.value == 29)
    );
    assert!(
        n.iter()
            .any(|h| h.axis && h.name == "right_y" && h.value == 3)
    );
    assert!(
        n.iter()
            .any(|h| h.device == Device::Mouse && h.name == "forward")
    );
}

#[test]
fn host_inputs_round_trip() {
    for t in [
        "gamepad:south",
        "gamepad2:left_x-",
        "gamepad:right_y+",
        "key:left_shift",
        "mouse:middle",
    ] {
        assert_eq!(HostInput::parse(t).unwrap().to_string(), t);
    }
    let l = HostInput::parse_list("key:z, gamepad:south").unwrap();
    assert_eq!(HostInput::list_string(&l), "key:z,gamepad:south");
    assert!(HostInput::parse_list("").unwrap().is_empty());
}

#[test]
fn bad_inputs_are_refused() {
    for t in [
        "south",
        "pad:south",
        "gamepad9:south",
        "gamepad0:south",
        "key:nope",
        "mouse:z",
        "gamepad:z",
    ] {
        assert!(
            matches!(HostInput::parse(t), Err(Error::BadInput { .. })),
            "{t}"
        );
    }
}

#[test]
fn defaults_and_settings_apply_to_known_controls_only() {
    let set = pad();
    let mut b = Bindings::defaults(&set);
    assert_eq!(b.get("p1", "a").unwrap().len(), 2);
    assert!(b.get("p1", "l").unwrap().is_empty());
    assert!(b.apply_setting(&set, "bind.p1.l", "key:q").unwrap());
    assert_eq!(b.get("p1", "l").unwrap()[0].to_string(), "key:q");
    assert!(!b.apply_setting(&set, "fullscreen", "true").unwrap());
    assert!(matches!(
        b.apply_setting(&set, "bind.p2.a", "key:q"),
        Err(Error::UnknownPort(_))
    ));
    assert!(matches!(
        b.apply_setting(&set, "bind.p1.zz", "key:q"),
        Err(Error::UnknownControl { .. })
    ));
    assert!(b.apply_setting(&set, "bind.p1.a", "key:bogus").is_err());
}

#[test]
fn setting_keys_round_trip() {
    let s = KeyBindingSetting {
        target: KeyBindingSetting::target_of_key("bind.p3.stick_l").unwrap(),
        default: vec![],
    };
    assert_eq!(s.key(), "bind.p3.stick_l");
    assert!(KeyBindingSetting::target_of_key("bind.p3").is_none());
}

#[test]
fn duplicates_and_bad_ids_are_refused() {
    let mut set = pad();
    assert!(matches!(
        set.add_port(Port::new("p1", "x", vec![])),
        Err(Error::DuplicatePort(_))
    ));
    assert!(matches!(
        set.add_control("p1", Control::new("a", ControlKind::Button)),
        Err(Error::DuplicateControl { .. })
    ));
    assert!(matches!(
        set.add_control("p1", Control::new("Up", ControlKind::Button)),
        Err(Error::BadId(_))
    ));
}

#[test]
fn a_mod_adds_a_port_and_a_control() {
    let mut set = pad();
    let text = r#"
[input.port.p1.control.turbo]
kind = "button"
label = "Turbo"
default = "key:t"

[input.port.dial]
kind = "Rotary dial"
label = "Dial"

[input.port.dial.control.turn]
kind = "axis"
"#;
    set.extend_from_toml("mod.toml", Origin::Mod("dial".into()), text)
        .unwrap();
    assert_eq!(set.ports().len(), 2);
    let p = set.port("dial").unwrap();
    assert_eq!(p.origin, Origin::Mod("dial".into()));
    assert_eq!(p.control("turn").unwrap().kind, ControlKind::Axis);
    assert_eq!(
        set.port("p1")
            .unwrap()
            .control("turbo")
            .unwrap()
            .label
            .as_deref(),
        Some("Turbo")
    );
    let b = Bindings::defaults(&set);
    assert_eq!(b.get("p1", "turbo").unwrap()[0].to_string(), "key:t");
}

#[test]
fn bad_declarations_are_refused() {
    let mut set = pad();
    let cases = [
        "[input.port.q]\n",
        "[input.port.p1]\nkind = \"other\"\n",
        "[input.port.q]\nkind = 3\n",
        "[input.port.p1.control.a]\n",
        "[input.port.p1.control.z]\nkind = \"wheel\"\n",
        "[input.port.p1.control.z]\ndefault = \"key:nope\"\n",
        "input = 3\n[",
    ];
    for c in cases {
        assert!(
            set.clone()
                .extend_from_toml("game.toml", Origin::Game, c)
                .is_err(),
            "{c}"
        );
    }
    set.extend_from_toml("game.toml", Origin::Game, "[game]\nx = 1\n")
        .unwrap();
    assert_eq!(set, pad());
}

#[test]
fn the_self_description_lists_ports_and_marks_extras() {
    let mut set = pad();
    let i = input_section(&set);
    assert_eq!(i.ports.len(), 1);
    assert_eq!(i.ports[0].controls[1].label, "L shoulder");
    assert_eq!(i.ports[0].controls[0].label, "a");
    assert!(!i.ports[0].extra);
    assert_eq!(i.ports[0].id, "p1");
    assert_eq!(i.ports[0].origin, "native");
    assert_eq!(i.ports[0].controls[0].defaults, ["gamepad:east", "key:x"]);
    assert_eq!(i.ports[0].controls[2].kind, "axis");
    set.extend_from_toml(
        "game.toml",
        Origin::Game,
        "[input.port.p2]\nkind = \"GBA pad\"\n[input.port.p2.control.go]\n",
    )
    .unwrap();
    let i = input_section(&set);
    assert_eq!(i.ports.len(), 2);
    assert_eq!(i.ports[1].port, 1);
    assert!(i.ports[1].extra);
    assert_eq!(i.ports[1].origin, "game");
}

#[test]
fn c_declarations_carry_ids_kinds_and_defaults() {
    let set = pad();
    let c = declarations_c("rl_test_ports", set.ports());
    assert!(c.contains("{ \"a\", 0, \"gamepad:east,key:x\" },"));
    assert!(c.contains("{ \"stick\", 2, NULL },"));
    assert!(c.contains("{ \"p1\", \"GBA pad\", 3, rl_test_ports_0 },"));
    assert!(c.contains("rl_test_ports_count = 1;"));
}

#[test]
fn extras_hold_only_what_was_added() {
    let mut set = pad();
    assert!(set.extras().is_empty());
    set.extend_from_toml(
        "game.toml",
        Origin::Game,
        "[input.port.p1.control.turbo]\n[input.port.dial]\nkind = \"Dial\"\n",
    )
    .unwrap();
    let e = set.extras();
    assert_eq!(e.len(), 2);
    assert_eq!(e[0].id, "p1");
    assert_eq!(e[0].controls.len(), 1);
    assert_eq!(e[1].id, "dial");
    let c = relift_input::extras_c_file(&set);
    assert!(c.contains("rl_ports_declare(rl_game_ports, rl_game_ports_count)"));
}
