//! Tests that need no external data.

use super::ares;
use super::cpu::Cpu;
use super::json;
use super::mutate::Mutation;
use super::sst::run_case;
use super::zex;

/// `ADD A,B` with A = 0x7F, B = 1, in the shape of a SingleStepTests case.
const ADD_A_B: &str = r#"{
  "name": "80 0000",
  "initial": {"pc": 4096, "sp": 0, "a": 127, "b": 1, "c": 0, "d": 0, "e": 0, "f": 0,
              "h": 0, "l": 0, "i": 0, "r": 0, "ei": 0, "wz": 0, "ix": 0, "iy": 0,
              "af_": 0, "bc_": 0, "de_": 0, "hl_": 0, "im": 0, "p": 0, "q": 0,
              "iff1": 0, "iff2": 0, "ram": [[4096, 128]]},
  "final": {"pc": 4097, "sp": 0, "a": 128, "b": 1, "f": 148, "r": 1, "q": 148,
            "ram": [[4096, 128]]},
  "cycles": [[4096, 128, "----"], [4096, 128, "r-m-"], [4097, null, "----"], [4097, null, "----"]],
  "ports": []
}"#;

#[test]
fn a_correct_case_passes() {
    let t = json::parse(ADD_A_B.as_bytes());
    let (bad, detail) = run_case(&mut Cpu::new(), &t);
    assert!(bad.is_empty(), "{bad:?}{detail}");
}

#[test]
fn mutations_are_caught() {
    let t = json::parse(ADD_A_B.as_bytes());
    for m in [Mutation::FlipConst, Mutation::DropSet] {
        let mut cpu = Cpu::new();
        cpu.mutation = Some(m);
        let (bad, _) = run_case(&mut cpu, &t);
        assert!(!bad.is_empty(), "{m:?} was not caught");
    }
}

#[test]
fn a_wrong_expectation_fails() {
    let wrong = ADD_A_B.replace("\"a\": 128", "\"a\": 129");
    let t = json::parse(wrong.as_bytes());
    let (bad, _) = run_case(&mut Cpu::new(), &t);
    assert_eq!(bad, ["a"]);
}

#[test]
fn random_states_cover_every_template() {
    let lines = ares::states(1, 31);
    assert_eq!(lines.len(), 252 * 3 + 256 * 4);
    for l in lines.iter().step_by(37) {
        let t = json::parse(l.as_bytes());
        assert!(t.get("initial").is_some(), "{l}");
    }
    assert_eq!(
        lines,
        ares::states(1, 31),
        "the same seed gives the same states"
    );
    assert_eq!(ares::irq_states(1, 31).len(), 40);
}

#[test]
fn a_state_compared_with_its_own_result_is_equal() {
    let ins = ares::states(1, 7);
    let mut outs = Vec::new();
    let mut cpu = Cpu::new();
    for l in &ins {
        let t = json::parse(l.as_bytes());
        cpu.load(t.get("initial").expect("initial"));
        cpu.reset_bus(&[]);
        cpu.step().expect("steps");
        let mut o = String::from("{\"final\":{");
        for (i, (k, v)) in cpu.dump().iter().enumerate() {
            o += &format!("{}\"{k}\":{v}", if i > 0 { "," } else { "" });
        }
        o += &format!("}},\"tstates\":{},\"cycles\":[", cpu.bus.borrow().clock);
        let mut cycles = Vec::new();
        for a in &cpu.bus.borrow().log {
            let (d, k) = match a.kind {
                b'f' if a.m1 => (4, 'f'),
                b'f' | b'r' => (3, 'r'),
                b'w' => (3, 'w'),
                k => (4, k as char),
            };
            cycles.push(format!("[{},{},\"{k}\",{}]", a.addr, a.val, a.t + d));
        }
        o += &cycles.join(",");
        o += "]}";
        outs.push(o);
    }
    let rep = ares::compare(&ins.join("\n"), &outs.join("\n"));
    assert_eq!(rep.unexplained, 0, "{:?}", rep.first);
    assert_eq!(rep.states.iter().sum::<u64>(), ins.len() as u64);
}

#[test]
fn a_tiny_exerciser_runs_to_the_end() {
    let com = [
        0x0E, 0x02, 0x1E, 0x20, 0xCD, 0x05, 0x00, 0x1E, 0x4F, 0xCD, 0x05, 0x00, 0x1E, 0x4B, 0xCD,
        0x05, 0x00, 0xC3, 0x00, 0x00,
    ];
    let s = zex::run_com(&com, false).expect("runs");
    assert_eq!((s.ok, s.error, s.instructions), (1, 0, 8));
}
