//! A `match` on a value fixed at lift time (a word field) whose arms compute
//! run-time values: every arm must reach the template, chosen by the field,
//! and arms the evaluator cannot join must stop generation once read.

use relift_sail::eval::{Cpu, E, Gen, R};
use relift_sail::model::Model;
use relift_sail::value::{K, V};

#[derive(Default)]
struct Toy;

impl Cpu for Toy {
    fn cut(g: &mut Gen<'_, Self>, name: &str, a: &[V], _hint: Option<u32>) -> R<Option<V>> {
        Ok(match name {
            "rd" => Some(g.dy(K::Bits(8), "st.get(X)".into())),
            "wr" => {
                let v = g.dx(&a[0])?;
                g.effect(format!("st.set(X, {v});"));
                Some(V::Unit)
            }
            "undef" => {
                g.throw("Undefined")?;
                Some(V::Unit)
            }
            _ => None,
        })
    }
}

fn model(body: &str) -> Model {
    let mut m = Model::default();
    m.load_text(&format!(
        "overload operator == = {{eq_int}}\n\
         overload operator > = {{gt_int}}\n\
         val decode_toy : bits(8) -> unit\n\
         function clause decode_toy (0b00 @ r : bits(3) @ 0b000) = {{\n  {}\n}}\n",
        body.replace('\n', "\n  ")
    ));
    m
}

/// The lines of the one decode clause run on an 8-bit word whose bits 3 to
/// 5 (`r`) are open, or the error that stops it.
fn generate(body: &str) -> Result<String, String> {
    let m = model(body);
    let clauses = match m.func("decode_toy") {
        Some(Ok(c)) => c,
        other => panic!("decode_toy does not parse: {:?}", other.map(|r| r.err())),
    };
    let mut g: Gen<'_, Toy> = Gen::new(&m, 0xC7, 0);
    match g.run_decode(&clauses[0], 8) {
        Ok(()) | Err(E::Throw(_)) => Ok(g.take().join("\n")),
        Err(E::Fail(e)) => Err(e),
        Err(e) => Err(format!("{e:?}")),
    }
}

fn code(body: &str) -> String {
    generate(body).unwrap_or_else(|e| panic!("generation fails: {e}"))
}

/// Whether the arm constants `0x1`, `0x2`, `0x3` each reach the template
/// and the first write takes a selection between arms, not one arm.
fn has_every_arm(c: &str) -> bool {
    let consts = ["0x1", "0x2", "0x3"].iter().all(|k| c.contains(k));
    let written = c
        .lines()
        .find_map(|l| l.split_once("st.set(X, "))
        .and_then(|(_, r)| r.split_once(')'))
        .map(|(v, _)| format!("let {v} = "));
    let selected = written.is_some_and(|def| {
        c.lines()
            .filter_map(|l| l.strip_prefix(def.as_str()))
            .any(|rhs| rhs.starts_with("if ") || rhs.starts_with("b.select("))
    });
    consts && selected
}

#[test]
fn literal_arms_on_a_field_keep_every_arm() {
    let c = code(
        "let k : range(0, 7) = unsigned(r);\n\
         let s : bits(8) = rd();\n\
         let x : bits(8) = match k {\n\
           1 => add_bits(s, 0x01),\n\
           2 => add_bits(s, 0x02),\n\
           _ => add_bits(s, 0x03),\n\
         };\n\
         wr(x)",
    );
    assert!(has_every_arm(&c), "{c}");
}

#[test]
fn tuple_arms_on_a_field_keep_every_arm() {
    let c = code(
        "let k : range(0, 7) = unsigned(r);\n\
         let s : bits(8) = rd();\n\
         let (x, y) : (bits(8), bits(8)) = match k {\n\
           1 => (add_bits(s, 0x01), s),\n\
           2 => (add_bits(s, 0x02), s),\n\
           _ => (add_bits(s, 0x03), s),\n\
         };\n\
         wr(x);\n\
         wr(y)",
    );
    assert!(has_every_arm(&c), "{c}");
}

#[test]
fn tuple_bound_field_keeps_every_arm() {
    let c = code(
        "let (k, s) : (range(0, 7), bits(8)) = (unsigned(r), rd());\n\
         let (x, f) : (bits(8), bits(1)) = match k {\n\
           1 => (add_bits(s, 0x01), 0b0),\n\
           2 => (add_bits(s, 0x02), 0b1),\n\
           _ => (add_bits(s, 0x03), 0b0),\n\
         };\n\
         wr(x)",
    );
    assert!(has_every_arm(&c), "{c}");
}

#[test]
fn arms_after_a_guarded_throw_keep_every_arm() {
    let c = code(
        "let k : range(0, 7) = unsigned(r);\n\
         if k == 0 | k > 3 then undef();\n\
         let s : bits(8) = rd();\n\
         let (x, f) : (bits(8), bits(1)) = match k {\n\
           1 => (add_bits(s, 0x01), 0b0),\n\
           2 => (add_bits(s, 0x02), 0b1),\n\
           _ => (add_bits(s, 0x03), 0b0),\n\
         };\n\
         wr(x)",
    );
    assert!(has_every_arm(&c), "{c}");
}

/// Arms that give Sail's `bit` once as a boolean (a comparison) and once as
/// a one-bit vector join to one bit; the third arm is not dropped for the
/// first.
#[test]
fn bool_and_bit_arms_keep_every_arm() {
    let c = code(
        "let k : range(0, 7) = unsigned(r);\n\
         let s : bits(8) = rd();\n\
         let (x, f) : (bits(8), bits(1)) = match k {\n\
           1 => (add_bits(s, 0x01), eq_bits(s, 0x00)),\n\
           2 => (add_bits(s, 0x02), eq_bits(s, 0x00)),\n\
           _ => (add_bits(s, 0x03), 0b1),\n\
         };\n\
         wr(x);\n\
         wr(zero_extend(f, 8))",
    );
    assert!(has_every_arm(&c), "{c}");
}

/// Arms the evaluator cannot join stop generation once the value is used.
#[test]
fn unjoinable_arms_are_an_error_when_used() {
    let r = generate(
        "let k : range(0, 7) = unsigned(r);\n\
         let s : bits(8) = rd();\n\
         let x = match k {\n\
           1 => add_bits(s, 0x01),\n\
           2 => add_bits(s, 0x02),\n\
           _ => 0x3,\n\
         };\n\
         wr(x)",
    );
    match r {
        Err(e) => assert!(e.contains("do not join"), "{e}"),
        Ok(c) => panic!("generates {c}"),
    }
}

/// An unjoinable value nothing reads is no error.
#[test]
fn unjoinable_arms_are_harmless_when_unused() {
    let c = code(
        "let k : range(0, 7) = unsigned(r);\n\
         let s : bits(8) = rd();\n\
         let x = match k {\n\
           1 => add_bits(s, 0x01),\n\
           _ => 0x3,\n\
         };\n\
         wr(s)",
    );
    assert!(c.contains("st.set(X"), "{c}");
}
