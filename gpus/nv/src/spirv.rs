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
    pub const CONSTANT_NULL: u16 = 46;
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
    pub const TYPE_COOPERATIVE_MATRIX: u16 = 4456;
    pub const COOPERATIVE_MATRIX_MUL_ADD: u16 = 4459;
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
    pub const TANH: u32 = 21;
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
    pub const FIND_I_LSB: u32 = 73;
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
    /// Instructions emitted that a driver compiles to work (not types, labels, loads, stores, bitcasts or access chains), for
    /// RELIFT_OP_ATTR's per-guest-mnemonic count.
    pub work: u64,
    /// Blocks begun (labels emitted), for RELIFT_OP_ATTR.
    pub blocks: u64,
    pub caps: Vec<u32>,
    pub ext: Vec<u32>,
    pub head: Vec<u32>,
    pub modes: Vec<u32>,
    pub decos: Vec<u32>,
    pub types: Vec<u32>,
    pub body: Vec<u32>,
    cache: HashMap<Vec<u32>, u32>,
    /// The key [`Module::ty`] and [`Module::konst`] look up, kept so a
    /// lookup that hits allocates nothing.
    key: Vec<u32>,
    vars: Option<Vars>,
    /// The label of the block being emitted (see [`Module::block`]).
    block: u32,
    /// The value of each 32-bit `OpConstant` by id (see [`Module::const32`]).
    consts32: HashMap<u32, u32>,
}

/// The values of function variables known in the current block, so loads
/// and stores of them become plain SSA values within a block (see
/// [`Module::load_var`]).
#[derive(Default)]
pub struct Vars {
    vals: HashMap<u32, (u32, bool)>,
    order: Vec<u32>,
    memo: HashMap<Vec<u32>, u32>,
    /// The block being emitted.
    block: u32,
    /// The blocks that branch to each label so far.
    preds: HashMap<u32, Vec<u32>>,
    /// The current block took its values from its one predecessor and
    /// nothing has been emitted in it yet: if it turns out to be a loop
    /// header (an `OpLoopMerge` follows), its values are dropped.
    carried: bool,
}

pub fn inst(out: &mut Vec<u32>, opcode: u16, args: &[u32]) {
    out.push(((args.len() as u32 + 1) << 16) | u32::from(opcode));
    out.extend_from_slice(args);
}

/// [`inst`] with the operands `head` then `args`.
fn inst2(out: &mut Vec<u32>, opcode: u16, head: &[u32], args: &[u32]) {
    out.push((((head.len() + args.len()) as u32 + 1) << 16) | u32::from(opcode));
    out.extend_from_slice(head);
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
            work: 0,
            blocks: 0,
            ..Module::default()
        }
    }

    pub fn id(&mut self) -> u32 {
        let i = self.next;
        self.next += 1;
        i
    }

    /// The id cached for the declaration `key` then `args`, or a new id
    /// that [`Module::cached`] gives with the declaration's operands.
    fn cached(&mut self, key: &[u32], args: &[u32]) -> Result<u32, u32> {
        let mut k = std::mem::take(&mut self.key);
        k.clear();
        k.extend_from_slice(key);
        k.extend_from_slice(args);
        let hit = self.cache.get(k.as_slice()).copied();
        let r = match hit {
            Some(i) => Ok(i),
            None => {
                let i = self.id();
                self.cache.insert(k.clone(), i);
                Err(i)
            }
        };
        self.key = k;
        r
    }

    fn ty(&mut self, opcode: u16, args: &[u32]) -> u32 {
        match self.cached(&[u32::from(opcode)], args) {
            Ok(i) => i,
            Err(i) => {
                inst2(&mut self.types, opcode, &[i], args);
                i
            }
        }
    }

    fn konst(&mut self, opcode: u16, ty: u32, args: &[u32]) -> u32 {
        match self.cached(&[u32::from(opcode), ty], args) {
            Ok(i) => i,
            Err(i) => {
                inst2(&mut self.types, opcode, &[ty, i], args);
                if opcode == op::CONSTANT && args.len() == 1 {
                    self.consts32.insert(i, args[0]);
                }
                i
            }
        }
    }

    /// The bits of a 32-bit `OpConstant` made by this module, or none for
    /// any other id.
    pub fn const32(&self, id: u32) -> Option<u32> {
        self.consts32.get(&id).copied()
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
    /// `OpTypeCooperativeMatrixKHR` of `component`, subgroup scope, with
    /// `usage` 0 for A, 1 for B and 2 for the accumulator.
    pub fn t_coop_matrix(&mut self, component: u32, rows: u32, cols: u32, usage: u32) -> u32 {
        let scope = self.c_u32(3);
        let r = self.c_u32(rows);
        let c = self.c_u32(cols);
        let u = self.c_u32(usage);
        self.ty(op::TYPE_COOPERATIVE_MATRIX, &[component, scope, r, c, u])
    }
    pub fn t_f16(&mut self) -> u32 {
        self.ty(op::TYPE_FLOAT, &[16])
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
    pub fn t_fn_args(&mut self, ret: u32, params: &[u32]) -> u32 {
        let mut a = vec![ret];
        a.extend_from_slice(params);
        self.ty(op::TYPE_FUNCTION, &a)
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
    /// `OpConstantNull` of `ty`.
    pub fn c_null(&mut self, ty: u32) -> u32 {
        self.konst(op::CONSTANT_NULL, ty, &[])
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
    pub fn c_f16(&mut self, bits: u16) -> u32 {
        let t = self.t_f16();
        self.konst(op::CONSTANT, t, &[u32::from(bits)])
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
        self.work += u64::from(!matches!(opcode, op::BITCAST | op::LOAD | op::ACCESS_CHAIN));
        if let Some(v) = &mut self.vars {
            v.carried = false;
        }
        let i = self.id();
        inst2(&mut self.body, opcode, &[ty, i], args);
        i
    }

    pub fn emit_void(&mut self, opcode: u16, args: &[u32]) {
        if opcode == op::LABEL {
            self.blocks += 1;
            self.block = args[0];
        }
        if let Some(v) = &mut self.vars {
            let carried = std::mem::take(&mut v.carried);
            match opcode {
                op::LABEL => {
                    let only = v.preds.get(&args[0]).is_some_and(|p| *p == [v.block]);
                    if only {
                        v.carried = true;
                    } else {
                        v.vals.clear();
                        v.memo.clear();
                    }
                    v.order.clear();
                    v.block = args[0];
                }
                op::LOOP_MERGE if carried => {
                    v.vals.clear();
                    v.memo.clear();
                }
                _ => {}
            }
            if matches!(
                opcode,
                op::SELECTION_MERGE
                    | op::LOOP_MERGE
                    | op::BRANCH
                    | op::BRANCH_CONDITIONAL
                    | op::SWITCH
            ) {
                for var in std::mem::take(&mut v.order) {
                    if let Some((val, true)) = v.vals.get(&var).copied() {
                        inst(&mut self.body, op::STORE, &[var, val]);
                        v.vals.insert(var, (val, false));
                    }
                }
            }
            let targets: Vec<u32> = match opcode {
                op::BRANCH => vec![args[0]],
                op::BRANCH_CONDITIONAL => vec![args[1], args[2]],
                op::SWITCH => std::iter::once(args[1])
                    .chain(args[2..].iter().skip(1).step_by(2).copied())
                    .collect(),
                _ => Vec::new(),
            };
            for t in targets {
                v.preds.entry(t).or_default().push(v.block);
            }
        }
        inst(&mut self.body, opcode, args);
    }

    /// Starts keeping variable values per block: from here, a load of a
    /// variable that [`Module::store_var`] wrote or [`Module::load_var`]
    /// read in the same block, or in the block's one predecessor when that
    /// was the block emitted just before and the block is no loop header,
    /// reuses the value, and stores are written once,
    /// before the block's merge or branch (none before a return or a
    /// discard: function variables end there). Only for variables nothing
    /// else reads or writes, in code emitted block by block in order.
    pub fn keep_vars(&mut self) {
        self.vars = Some(Vars::default());
    }

    /// Takes the per-block state away while code for another function is
    /// emitted; [`Module::restore_vars`] puts it back.
    pub fn suspend_vars(&mut self) -> Option<Vars> {
        self.vars.take()
    }

    /// The label of the block code is being emitted into: a value made in
    /// it since that label dominates what follows in the same block.
    pub fn block(&self) -> u32 {
        self.block
    }

    /// Sets the block [`Module::block`] reports, when code for another
    /// function was emitted in between.
    pub fn set_block(&mut self, b: u32) {
        self.block = b;
    }

    /// Whether [`Module::keep_vars`] is on.
    pub fn keeps_vars(&self) -> bool {
        self.vars.is_some()
    }

    pub fn restore_vars(&mut self, v: Option<Vars>) {
        self.vars = v;
    }

    /// The value of function variable `var` of type `ty`.
    pub fn load_var(&mut self, ty: u32, var: u32) -> u32 {
        if let Some(v) = &mut self.vars
            && let Some((val, _)) = v.vals.get(&var).copied()
        {
            v.carried = false;
            return val;
        }
        let val = self.emit(op::LOAD, ty, &[var]);
        if let Some(v) = &mut self.vars {
            v.vals.insert(var, (val, false));
        }
        val
    }

    /// The value an earlier [`Module::remember`] in the current block gave
    /// `key`.
    pub fn recall(&mut self, key: &[u32]) -> Option<u32> {
        let v = self.vars.as_mut()?;
        let r = v.memo.get(key).copied();
        if r.is_some() {
            v.carried = false;
        }
        r
    }

    /// Notes `val` for `key` until the block ends (only while
    /// [`Module::keep_vars`] is on): for values that cannot change within
    /// a block, such as constant bank words.
    pub fn remember(&mut self, key: Vec<u32>, val: u32) {
        if let Some(v) = &mut self.vars {
            v.memo.insert(key, val);
        }
    }

    /// Writes `val` to function variable `var`.
    pub fn store_var(&mut self, var: u32, val: u32) {
        match &mut self.vars {
            Some(v) => {
                if v.vals.insert(var, (val, true)).is_none_or(|(_, d)| !d) {
                    v.order.push(var);
                }
            }
            None => inst(&mut self.body, op::STORE, &[var, val]),
        }
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
