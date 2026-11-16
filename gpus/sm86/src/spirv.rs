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
    pub const SWITCH: u16 = 251;
    pub const RETURN: u16 = 253;
    pub const KILL: u16 = 252;
    pub const IS_NAN: u16 = 156;
    pub const IS_INF: u16 = 157;
    pub const EXTENSION: u16 = 10;
    pub const TYPE_RUNTIME_ARRAY: u16 = 29;
    pub const CONSTANT_COMPOSITE: u16 = 44;
    pub const SPEC_CONSTANT: u16 = 50;
    pub const SPEC_CONSTANT_COMPOSITE: u16 = 51;
    pub const VECTOR_SHUFFLE: u16 = 79;
    pub const COMPOSITE_CONSTRUCT: u16 = 80;
    pub const U_CONVERT: u16 = 113;
    pub const S_CONVERT: u16 = 114;
    pub const F_CONVERT: u16 = 115;
    pub const CONVERT_U_TO_PTR: u16 = 120;
    pub const S_NEGATE: u16 = 126;
    pub const I_SUB: u16 = 130;
    pub const F_SUB: u16 = 131;
    pub const U_DIV: u16 = 134;
    pub const U_MOD: u16 = 137;
    pub const I_ADD_CARRY: u16 = 149;
    pub const U_MUL_EXTENDED: u16 = 151;
    pub const S_MUL_EXTENDED: u16 = 152;
    pub const BIT_FIELD_INSERT: u16 = 201;
    pub const BIT_FIELD_S_EXTRACT: u16 = 202;
    pub const BIT_FIELD_U_EXTRACT: u16 = 203;
    pub const BIT_REVERSE: u16 = 204;
    pub const BIT_COUNT: u16 = 205;
    pub const CONTROL_BARRIER: u16 = 224;
    pub const MEMORY_BARRIER: u16 = 225;
    pub const LOOP_MERGE: u16 = 246;
    pub const UNREACHABLE: u16 = 255;
    pub const TYPE_IMAGE: u16 = 25;
    pub const TYPE_SAMPLER: u16 = 26;
    pub const TYPE_SAMPLED_IMAGE: u16 = 27;
    pub const SPEC_CONSTANT_OP: u16 = 52;
    pub const IMAGE_TEXEL_POINTER: u16 = 60;
    pub const SAMPLED_IMAGE: u16 = 86;
    pub const IMAGE_SAMPLE_IMPLICIT_LOD: u16 = 87;
    pub const IMAGE_SAMPLE_EXPLICIT_LOD: u16 = 88;
    pub const IMAGE_SAMPLE_DREF_IMPLICIT_LOD: u16 = 89;
    pub const IMAGE_SAMPLE_DREF_EXPLICIT_LOD: u16 = 90;
    pub const IMAGE_FETCH: u16 = 95;
    pub const IMAGE_GATHER: u16 = 96;
    pub const IMAGE_DREF_GATHER: u16 = 97;
    pub const IMAGE_READ: u16 = 98;
    pub const IMAGE_WRITE: u16 = 99;
    pub const IMAGE_QUERY_SIZE_LOD: u16 = 103;
    pub const IMAGE_QUERY_SIZE: u16 = 104;
    pub const IMAGE_QUERY_LOD: u16 = 105;
    pub const IMAGE_QUERY_LEVELS: u16 = 106;
    pub const IMAGE_QUERY_SAMPLES: u16 = 107;
    pub const ATOMIC_EXCHANGE: u16 = 229;
    pub const ATOMIC_COMPARE_EXCHANGE: u16 = 230;
    pub const ATOMIC_I_INCREMENT: u16 = 232;
    pub const ATOMIC_I_DECREMENT: u16 = 233;
    pub const ATOMIC_I_ADD: u16 = 234;
    pub const ATOMIC_S_MIN: u16 = 236;
    pub const ATOMIC_U_MIN: u16 = 237;
    pub const ATOMIC_S_MAX: u16 = 238;
    pub const ATOMIC_U_MAX: u16 = 239;
    pub const ATOMIC_AND: u16 = 240;
    pub const ATOMIC_OR: u16 = 241;
    pub const ATOMIC_XOR: u16 = 242;
    pub const PHI: u16 = 245;
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
    pub const S_ABS: u32 = 5;
    pub const U_MIN: u32 = 38;
    pub const S_MIN: u32 = 39;
    pub const U_MAX: u32 = 41;
    pub const S_MAX: u32 = 42;
    pub const PACK_HALF_2X16: u32 = 58;
    pub const UNPACK_HALF_2X16: u32 = 62;
    pub const FIND_S_MSB: u32 = 74;
    pub const FIND_U_MSB: u32 = 75;
    pub const S_CLAMP: u32 = 45;
    pub const U_CLAMP: u32 = 44;
    pub const LDEXP: u32 = 53;
    pub const F_SIGN: u32 = 6;
    pub const INTERPOLATE_AT_CENTROID: u32 = 76;
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
    /// An array type whose length is the constant or specialization
    /// constant `len`.
    pub fn t_array_of(&mut self, t: u32, len: u32) -> u32 {
        self.ty(op::TYPE_ARRAY, &[t, len])
    }
    pub fn t_struct_unique(&mut self, members: &[u32]) -> u32 {
        let i = self.id();
        let mut a = vec![i];
        a.extend_from_slice(members);
        inst(&mut self.types, op::TYPE_STRUCT, &a);
        i
    }
    /// A struct type with no decorations, shared by every use.
    pub fn t_struct_cached(&mut self, members: &[u32]) -> u32 {
        self.ty(op::TYPE_STRUCT, members)
    }
    pub fn t_u64(&mut self) -> u32 {
        self.ty(op::TYPE_INT, &[64, 0])
    }
    pub fn t_runtime_array(&mut self, t: u32) -> u32 {
        self.ty(op::TYPE_RUNTIME_ARRAY, &[t])
    }
    /// An `OpTypeImage`: sampled type, `Dim`, depth, arrayed, multisampled,
    /// sampled (1) or storage (2), image format.
    pub fn t_image(&mut self, args: [u32; 7]) -> u32 {
        self.ty(op::TYPE_IMAGE, &args)
    }
    pub fn t_sampler(&mut self) -> u32 {
        self.ty(op::TYPE_SAMPLER, &[])
    }
    pub fn t_sampled_image(&mut self, image: u32) -> u32 {
        self.ty(op::TYPE_SAMPLED_IMAGE, &[image])
    }
    /// An `OpSpecConstantOp` of `ty` computing `opcode` over `args`.
    pub fn spec_op(&mut self, ty: u32, opcode: u16, args: &[u32]) -> u32 {
        let mut a = vec![u32::from(opcode)];
        a.extend_from_slice(args);
        self.konst(op::SPEC_CONSTANT_OP, ty, &a)
    }
    pub fn c_u32(&mut self, v: u32) -> u32 {
        let t = self.t_u32();
        self.konst(op::CONSTANT, t, &[v])
    }
    pub fn c_u64(&mut self, v: u64) -> u32 {
        let t = self.t_u64();
        self.konst(op::CONSTANT, t, &[v as u32, (v >> 32) as u32])
    }
    pub fn c_i32(&mut self, v: i32) -> u32 {
        let t = self.t_i32();
        self.konst(op::CONSTANT, t, &[v as u32])
    }
    pub fn c_f32(&mut self, v: u32) -> u32 {
        let t = self.t_f32();
        self.konst(op::CONSTANT, t, &[v])
    }
    pub fn c_bool(&mut self, v: bool) -> u32 {
        let t = self.t_bool();
        self.konst(if v { 41 } else { 42 }, t, &[])
    }

    /// A 32-bit unsigned specialization constant with `SpecId` `spec`.
    pub fn spec_u32(&mut self, default: u32, spec: u32) -> u32 {
        let key = vec![u32::from(op::SPEC_CONSTANT), spec, default];
        if let Some(&i) = self.cache.get(&key) {
            return i;
        }
        let t = self.t_u32();
        let i = self.id();
        inst(&mut self.types, op::SPEC_CONSTANT, &[t, i, default]);
        self.deco(i, 1, &[spec]);
        self.cache.insert(key, i);
        i
    }

    /// A constant composite of `ty` from constants `parts`.
    pub fn c_composite(&mut self, ty: u32, parts: &[u32]) -> u32 {
        self.konst(op::CONSTANT_COMPOSITE, ty, parts)
    }

    /// A specialization constant composite of `ty` from `parts`.
    pub fn spec_composite(&mut self, ty: u32, parts: &[u32]) -> u32 {
        self.konst(op::SPEC_CONSTANT_COMPOSITE, ty, parts)
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
        let mut w = vec![0x0723_0203, 0x0001_0500, 0, self.next, 0];
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
