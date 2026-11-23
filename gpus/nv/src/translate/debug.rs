//! SPIR-V debug info (`Options::debug_info`): NonSemantic.Shader.DebugInfo.100
//! whose source is the guest program's disassembly, one line per
//! instruction (`/*pc*/ text ;`, line n for the instruction at byte
//! 16 (n - 1)). Each guest instruction's code follows a `DebugLine` of its
//! line, and each register write a `DebugValue` of the guest register as a
//! local variable named `R0`..`R255`, `UR0`..`UR63`, `P0`..`P6` or
//! `UP0`..`UP6`. RenderDoc's shader debugger then steps the guest program
//! with the values of a capture. Programs on the software warp path get
//! lines but no register values (each lane's registers are an array).

use super::Ctx;
use crate::spirv::{inst, op, str_words};
use std::collections::BTreeMap;

const EXT_INST: u16 = 12;
const STRING: u16 = 7;
const COMPILATION_UNIT: u32 = 1;
const TYPE_BASIC: u32 = 2;
const TYPE_FUNCTION: u32 = 8;
const FUNCTION: u32 = 20;
const SCOPE: u32 = 23;
const LOCAL_VARIABLE: u32 = 26;
const VALUE: u32 = 29;
const EXPRESSION: u32 = 31;
const SOURCE: u32 = 35;
const FUNCTION_DEFINITION: u32 = 101;
const LINE: u32 = 103;
const ENCODING_BOOLEAN: u32 = 2;
const ENCODING_UNSIGNED: u32 = 6;
const FLAG_IS_LOCAL: u32 = 1 << 2;
const FLAG_IS_DEFINITION: u32 = 1 << 3;

/// What a variable of the debug info holds.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub(crate) enum Reg {
    R(u8),
    Ur(u8),
    P(u8),
    Up(u8),
}

impl Reg {
    fn name(self) -> String {
        match self {
            Reg::R(n) => format!("R{n}"),
            Reg::Ur(n) => format!("UR{n}"),
            Reg::P(n) => format!("P{n}"),
            Reg::Up(n) => format!("UP{n}"),
        }
    }

    fn boolean(self) -> bool {
        matches!(self, Reg::P(_) | Reg::Up(_))
    }
}

/// Debug info ids, made when the translation starts.
#[derive(Default)]
pub(crate) struct State {
    pub on: bool,
    import: u32,
    source: u32,
    function: u32,
    expression: u32,
    text: String,
    file: String,
    vars: BTreeMap<Reg, u32>,
}

impl Ctx {
    /// Starts debug info for a program whose disassembly is `lines` (one per
    /// instruction), named `file`.
    pub(crate) fn debug_begin(&mut self, file: &str, lines: &[String]) {
        let d = &mut self.debug;
        d.on = true;
        d.import = self.m.id();
        d.source = self.m.id();
        d.function = self.m.id();
        d.expression = self.m.id();
        d.file = file.to_string();
        d.text = lines.join("\n");
        d.text.push('\n');
        self.exts.insert("SPV_KHR_non_semantic_info");
    }

    fn debug_ext(&mut self, ty: u32, what: u32, args: &[u32]) -> u32 {
        let id = self.m.id();
        let mut a = vec![ty, id, self.debug.import, what];
        a.extend_from_slice(args);
        inst(&mut self.m.body, EXT_INST, &a);
        id
    }

    /// The line of the instruction at `pc`, before its code.
    pub(crate) fn debug_line(&mut self, pc: u64) {
        if !self.debug.on {
            return;
        }
        let void = self.m.t_void();
        let line = self.m.c_u32((pc / 16) as u32 + 1);
        let zero = self.m.c_u32(0);
        let (source, function) = (self.debug.source, self.debug.function);
        self.debug_ext(void, SCOPE, &[function]);
        self.debug_ext(void, LINE, &[source, line, line, zero, zero]);
    }

    /// A write of `val` to guest register `r`.
    pub(crate) fn debug_value(&mut self, r: Reg, val: u32) {
        if !self.debug.on || self.simt.is_some() || self.collecting() {
            return;
        }
        let next = self.m.id();
        let var = *self.debug.vars.entry(r).or_insert(next);
        let void = self.m.t_void();
        let e = self.debug.expression;
        self.debug_ext(void, VALUE, &[var, val, e]);
    }

    /// The debug info's global part, in the types section, the strings
    /// before the annotations, and the function's definition at `at` in the
    /// body (after the entry block's variables), for function `main`.
    pub(crate) fn debug_finish(&mut self, main: u32, at: usize) {
        if !self.debug.on {
            return;
        }
        let void = self.m.t_void();
        let c = |m: &mut crate::spirv::Module, v: u32| m.c_u32(v);
        let (v100, dwarf, zero, one, size32) = (
            c(&mut self.m, 100),
            c(&mut self.m, 4),
            c(&mut self.m, 0),
            c(&mut self.m, 1),
            c(&mut self.m, 32),
        );
        let (unsigned, boolean) = (
            c(&mut self.m, ENCODING_UNSIGNED),
            c(&mut self.m, ENCODING_BOOLEAN),
        );
        let (local, definition) = (
            c(&mut self.m, FLAG_IS_LOCAL),
            c(&mut self.m, FLAG_IS_DEFINITION),
        );
        let mut strings = Vec::new();
        let mut string = |m: &mut crate::spirv::Module, s: &str| {
            let id = m.id();
            let mut a = vec![id];
            a.extend(str_words(s));
            inst(&mut strings, STRING, &a);
            id
        };
        let file = self.debug.file.clone();
        let text = std::mem::take(&mut self.debug.text);
        let file_s = string(&mut self.m, &file);
        let text_s = string(&mut self.m, &text);
        let main_s = string(&mut self.m, "main");
        let uint_s = string(&mut self.m, "uint");
        let bool_s = string(&mut self.m, "bool");
        let regs: Vec<(Reg, u32)> = self.debug.vars.iter().map(|(&r, &id)| (r, id)).collect();
        let vars: Vec<(Reg, u32, u32)> = regs
            .into_iter()
            .map(|(r, id)| (r, id, string(&mut self.m, &r.name())))
            .collect();
        let d = &self.debug;
        let (import, source, function, expression) = (d.import, d.source, d.function, d.expression);
        let mut g = Vec::new();
        let ext = |g: &mut Vec<u32>, id: u32, what: u32, args: &[u32]| {
            let mut a = vec![void, id, import, what];
            a.extend_from_slice(args);
            inst(g, EXT_INST, &a);
        };
        ext(&mut g, source, SOURCE, &[file_s, text_s]);
        let cu = self.m.id();
        ext(&mut g, cu, COMPILATION_UNIT, &[v100, dwarf, source, zero]);
        let tu = self.m.id();
        ext(&mut g, tu, TYPE_BASIC, &[uint_s, size32, unsigned, zero]);
        let tb = self.m.id();
        ext(&mut g, tb, TYPE_BASIC, &[bool_s, size32, boolean, zero]);
        let tf = self.m.id();
        ext(&mut g, tf, TYPE_FUNCTION, &[zero, void]);
        ext(
            &mut g,
            function,
            FUNCTION,
            &[main_s, tf, source, one, zero, cu, main_s, definition, one],
        );
        ext(&mut g, expression, EXPRESSION, &[]);
        for (r, id, name) in vars {
            let ty = if r.boolean() { tb } else { tu };
            ext(
                &mut g,
                id,
                LOCAL_VARIABLE,
                &[name, ty, source, one, zero, function, local],
            );
        }
        self.m.types.extend(g);
        let mut decos = strings;
        decos.append(&mut self.m.decos);
        self.m.decos = decos;
        let mut def = Vec::new();
        let id = self.m.id();
        inst(
            &mut def,
            EXT_INST,
            &[void, id, import, FUNCTION_DEFINITION, function, main],
        );
        self.m.body.splice(at..at, def);
        let mut imp = vec![import];
        imp.extend(str_words("NonSemantic.Shader.DebugInfo.100"));
        inst(&mut self.m.ext, op::EXT_INST_IMPORT, &imp);
    }
}
