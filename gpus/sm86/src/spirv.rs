use std::collections::HashMap;

pub mod op {
    pub const EXT_INST_IMPORT: u16 = 11;
    pub const MEMORY_MODEL: u16 = 14;
    pub const ENTRY_POINT: u16 = 15;
    pub const EXECUTION_MODE: u16 = 16;
    pub const CAPABILITY: u16 = 17;
    pub const TYPE_VOID: u16 = 19;
    pub const TYPE_BOOL: u16 = 20;
    pub const TYPE_INT: u16 = 21;
    pub const TYPE_FLOAT: u16 = 22;
    pub const TYPE_VECTOR: u16 = 23;
    pub const TYPE_ARRAY: u16 = 28;
    pub const TYPE_STRUCT: u16 = 30;
    pub const TYPE_POINTER: u16 = 32;
    pub const TYPE_FUNCTION: u16 = 33;
    pub const CONSTANT: u16 = 43;
    pub const FUNCTION: u16 = 54;
    pub const FUNCTION_END: u16 = 56;
    pub const VARIABLE: u16 = 59;
    pub const LOAD: u16 = 61;
    pub const STORE: u16 = 62;
    pub const ACCESS_CHAIN: u16 = 65;
    pub const DECORATE: u16 = 71;
    pub const MEMBER_DECORATE: u16 = 72;
    pub const COMPOSITE_EXTRACT: u16 = 81;
    pub const CONVERT_F_TO_U: u16 = 109;
    pub const CONVERT_F_TO_S: u16 = 110;
    pub const CONVERT_S_TO_F: u16 = 111;
    pub const CONVERT_U_TO_F: u16 = 112;
    pub const BITCAST: u16 = 124;
    pub const F_NEGATE: u16 = 127;
    pub const I_ADD: u16 = 128;
    pub const F_ADD: u16 = 129;
    pub const I_MUL: u16 = 132;
    pub const F_MUL: u16 = 133;
    pub const F_DIV: u16 = 136;
    pub const SELECT: u16 = 169;
    pub const LOGICAL_NOT: u16 = 168;
    pub const LOGICAL_AND: u16 = 167;
    pub const LOGICAL_OR: u16 = 166;
    pub const LOGICAL_NOT_EQUAL: u16 = 165;
    pub const I_EQUAL: u16 = 170;
    pub const I_NOT_EQUAL: u16 = 171;
    pub const U_GREATER_THAN: u16 = 172;
    pub const S_GREATER_THAN: u16 = 173;
    pub const U_GREATER_THAN_EQUAL: u16 = 174;
    pub const S_GREATER_THAN_EQUAL: u16 = 175;
    pub const U_LESS_THAN: u16 = 176;
    pub const S_LESS_THAN: u16 = 177;
    pub const U_LESS_THAN_EQUAL: u16 = 178;
    pub const S_LESS_THAN_EQUAL: u16 = 179;
    pub const F_ORD_EQUAL: u16 = 180;
    pub const F_UNORD_EQUAL: u16 = 181;
    pub const F_ORD_NOT_EQUAL: u16 = 182;
    pub const F_UNORD_NOT_EQUAL: u16 = 183;
    pub const F_ORD_LESS_THAN: u16 = 184;
    pub const F_UNORD_LESS_THAN: u16 = 185;
    pub const F_ORD_GREATER_THAN: u16 = 186;
    pub const F_UNORD_GREATER_THAN: u16 = 187;
    pub const F_ORD_LESS_THAN_EQUAL: u16 = 188;
    pub const F_UNORD_LESS_THAN_EQUAL: u16 = 189;
    pub const F_ORD_GREATER_THAN_EQUAL: u16 = 190;
    pub const F_UNORD_GREATER_THAN_EQUAL: u16 = 191;
    pub const SHIFT_RIGHT_LOGICAL: u16 = 194;
    pub const SHIFT_RIGHT_ARITHMETIC: u16 = 195;
    pub const SHIFT_LEFT_LOGICAL: u16 = 196;
    pub const BITWISE_OR: u16 = 197;
    pub const BITWISE_XOR: u16 = 198;
    pub const BITWISE_AND: u16 = 199;
    pub const NOT: u16 = 200;
    pub const EXT_INST: u16 = 12;
    pub const SELECTION_MERGE: u16 = 247;
    pub const LABEL: u16 = 248;
    pub const BRANCH: u16 = 249;
    pub const BRANCH_CONDITIONAL: u16 = 250;
    pub const RETURN: u16 = 253;
    pub const KILL: u16 = 252;
    pub const IS_NAN: u16 = 156;
}

pub mod glsl {
    pub const F_ABS: u32 = 4;
    pub const FLOOR: u32 = 8;
    pub const CEIL: u32 = 9;
    pub const TRUNC: u32 = 3;
    pub const ROUND_EVEN: u32 = 2;
    pub const SIN: u32 = 13;
    pub const COS: u32 = 14;
    pub const EXP2: u32 = 29;
    pub const LOG2: u32 = 30;
    pub const SQRT: u32 = 31;
    pub const INVERSE_SQRT: u32 = 32;
    pub const FMA: u32 = 50;
    pub const N_MIN: u32 = 79;
    pub const N_MAX: u32 = 80;
    pub const N_CLAMP: u32 = 81;
}

#[derive(Default)]
pub struct Module {
    next: u32,
    pub caps: Vec<u32>,
    pub ext: Vec<u32>,
    pub head: Vec<u32>,
    pub modes: Vec<u32>,
    pub decos: Vec<u32>,
    pub types: Vec<u32>,
    pub body: Vec<u32>,
    cache: HashMap<Vec<u32>, u32>,
}

pub fn inst(out: &mut Vec<u32>, opcode: u16, args: &[u32]) {
    out.push(((args.len() as u32 + 1) << 16) | u32::from(opcode));
    out.extend_from_slice(args);
}

pub fn str_words(s: &str) -> Vec<u32> {
    let mut b = s.as_bytes().to_vec();
    b.push(0);
    while !b.len().is_multiple_of(4) {
        b.push(0);
    }
    b.chunks(4)
        .map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))
        .collect()
}

impl Module {
    pub fn new() -> Module {
        Module {
            next: 1,
            ..Module::default()
        }
    }

    pub fn id(&mut self) -> u32 {
        let i = self.next;
        self.next += 1;
        i
    }

    fn ty(&mut self, opcode: u16, args: &[u32]) -> u32 {
        let mut key = vec![u32::from(opcode)];
        key.extend_from_slice(args);
        if let Some(&i) = self.cache.get(&key) {
            return i;
        }
        let i = self.id();
        let mut a = vec![i];
        a.extend_from_slice(args);
        inst(&mut self.types, opcode, &a);
        self.cache.insert(key, i);
        i
    }

    fn konst(&mut self, opcode: u16, ty: u32, args: &[u32]) -> u32 {
        let mut key = vec![u32::from(opcode), ty];
        key.extend_from_slice(args);
        if let Some(&i) = self.cache.get(&key) {
            return i;
        }
        let i = self.id();
        let mut a = vec![ty, i];
        a.extend_from_slice(args);
        inst(&mut self.types, opcode, &a);
        self.cache.insert(key, i);
        i
    }

    pub fn t_void(&mut self) -> u32 {
        self.ty(op::TYPE_VOID, &[])
    }
    pub fn t_bool(&mut self) -> u32 {
        self.ty(op::TYPE_BOOL, &[])
    }
    pub fn t_u32(&mut self) -> u32 {
        self.ty(op::TYPE_INT, &[32, 0])
    }
    pub fn t_i32(&mut self) -> u32 {
        self.ty(op::TYPE_INT, &[32, 1])
    }
    pub fn t_f32(&mut self) -> u32 {
        self.ty(op::TYPE_FLOAT, &[32])
    }
    pub fn t_vec(&mut self, c: u32, n: u32) -> u32 {
        self.ty(op::TYPE_VECTOR, &[c, n])
    }
    pub fn t_ptr(&mut self, class: u32, t: u32) -> u32 {
        self.ty(op::TYPE_POINTER, &[class, t])
    }
    pub fn t_fn(&mut self, ret: u32) -> u32 {
        self.ty(op::TYPE_FUNCTION, &[ret])
    }
    pub fn t_array(&mut self, t: u32, n: u32) -> u32 {
        let len = self.c_u32(n);
        self.ty(op::TYPE_ARRAY, &[t, len])
    }
    pub fn t_struct_unique(&mut self, members: &[u32]) -> u32 {
        let i = self.id();
        let mut a = vec![i];
        a.extend_from_slice(members);
        inst(&mut self.types, op::TYPE_STRUCT, &a);
        i
    }
    pub fn c_u32(&mut self, v: u32) -> u32 {
        let t = self.t_u32();
        self.konst(op::CONSTANT, t, &[v])
    }
    pub fn c_f32(&mut self, v: u32) -> u32 {
        let t = self.t_f32();
        self.konst(op::CONSTANT, t, &[v])
    }
    pub fn c_bool(&mut self, v: bool) -> u32 {
        let t = self.t_bool();
        self.konst(if v { 41 } else { 42 }, t, &[])
    }

    pub fn global(&mut self, ptr: u32, class: u32) -> u32 {
        let i = self.id();
        inst(&mut self.types, op::VARIABLE, &[ptr, i, class]);
        i
    }

    pub fn deco(&mut self, target: u32, d: u32, extra: &[u32]) {
        let mut a = vec![target, d];
        a.extend_from_slice(extra);
        inst(&mut self.decos, op::DECORATE, &a);
    }

    pub fn member_deco(&mut self, target: u32, m: u32, d: u32, extra: &[u32]) {
        let mut a = vec![target, m, d];
        a.extend_from_slice(extra);
        inst(&mut self.decos, op::MEMBER_DECORATE, &a);
    }

    pub fn emit(&mut self, opcode: u16, ty: u32, args: &[u32]) -> u32 {
        let i = self.id();
        let mut a = vec![ty, i];
        a.extend_from_slice(args);
        inst(&mut self.body, opcode, &a);
        i
    }

    pub fn emit_void(&mut self, opcode: u16, args: &[u32]) {
        inst(&mut self.body, opcode, args);
    }

    pub fn finish(self) -> Vec<u32> {
        let mut w = vec![0x0723_0203, 0x0001_0300, 0, self.next, 0];
        w.extend(self.caps);
        w.extend(self.ext);
        w.extend(self.head);
        w.extend(self.modes);
        w.extend(self.decos);
        w.extend(self.types);
        w.extend(self.body);
        w
    }
}
