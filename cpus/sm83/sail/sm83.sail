/*!
The Sharp SM83 (Game Boy and Game Boy Color CPU) instruction set.

State: the 8-bit registers A, B, C, D, E, H and L, the 16-bit SP and PC, and
the four flags Z, N, H and C stored one bit each (bits 7 to 4 of F; the low
nibble of F reads as zero). PC holds the address of the instruction being
run.

Every bus access is a call with the M-cycle (1 is the opcode fetch) it takes
place in: MemFetch for instruction bytes, MemRead and MemWrite for data.
Cycles gives the total M-cycles of the instruction once it has run; a
branch taken in a conditional instruction gives the taken total instead.
The clock between accesses is not modeled further.

The interrupt master enable, the delay of EI, HALT (and its bug) and STOP
are not modeled here: they are calls into the per-CPU layer (SM83_EI,
SM83_DI, SM83_RETI, SM83_Halt, SM83_Stop). Control transfers are BranchTo
calls; an instruction that makes none goes on to the next one.
*/

default Order dec

$include <prelude.sail>

register A : bits(8)
register B : bits(8)
register C : bits(8)
register D : bits(8)
register E : bits(8)
register H : bits(8)
register L : bits(8)
register SP : bits(16)
register PC : bits(16)
register FZ : bits(1)
register FN : bits(1)
register FH : bits(1)
register FC : bits(1)

enum BranchType = {BranchType_JMP, BranchType_CALL, BranchType_RET}

val MemFetch : (bits(16), int) -> bits(8)

val MemRead : (bits(16), int) -> bits(8)

val MemWrite : (bits(16), bits(8), int) -> unit

val Cycles : int -> unit

val BranchTo : (bits(16), BranchType) -> unit

val SM83_EI : unit -> unit

val SM83_DI : unit -> unit

val SM83_RETI : unit -> unit

val SM83_Halt : unit -> unit

val SM83_Stop : unit -> unit

val SM83_Illegal : bits(8) -> unit

val ZeroExtend : forall 'n 'm, 'm >= 'n. (bits('n), int('m)) -> bits('m)

function ZeroExtend (x, m) = sail_zero_extend(x, m)

val SignExtend : forall 'n 'm, 'm >= 'n. (bits('n), int('m)) -> bits('m)

function SignExtend (x, m) = sail_sign_extend(x, m)

val Bit : bool -> bits(1)

function Bit (b) = if b then 0b1 else 0b0

val IsZero8 : bits(8) -> bits(1)

function IsZero8 (x) = Bit(x == 0x00)

/* Register pairs. */

val BC_read : unit -> bits(16)

function BC_read () = B @ C

val DE_read : unit -> bits(16)

function DE_read () = D @ E

val HL_read : unit -> bits(16)

function HL_read () = H @ L

val HL_write : bits(16) -> unit

function HL_write (v) = {
    H = v[15 .. 8];
    L = v[7 .. 0]
}

val F_read : unit -> bits(8)

function F_read () = FZ @ FN @ FH @ FC @ 0x0

/* rp: BC, DE, HL, SP. */

val RP_read : bits(2) -> bits(16)

function RP_read (p) = match p {
    0b00 => BC_read(),
    0b01 => DE_read(),
    0b10 => HL_read(),
    0b11 => SP
}

val RP_write : (bits(2), bits(16)) -> unit

function RP_write (p, v) = match p {
    0b00 => {
        B = v[15 .. 8];
        C = v[7 .. 0]
    },
    0b01 => {
        D = v[15 .. 8];
        E = v[7 .. 0]
    },
    0b10 => HL_write(v),
    0b11 => SP = v
}

/* rp2: BC, DE, HL, AF. */

val RP2_read : bits(2) -> bits(16)

function RP2_read (p) = match p {
    0b11 => A @ F_read(),
    _ => RP_read(p)
}

val RP2_write : (bits(2), bits(16)) -> unit

function RP2_write (p, v) = match p {
    0b11 => {
        A = v[15 .. 8];
        FZ = [v[7]];
        FN = [v[6]];
        FH = [v[5]];
        FC = [v[4]]
    },
    _ => RP_write(p, v)
}

/* r: B, C, D, E, H, L, (HL), A. The memory operand is read or written in
   M-cycle `at`. */

val R_read : (bits(3), int) -> bits(8)

function R_read (r, at) = match r {
    0b000 => B,
    0b001 => C,
    0b010 => D,
    0b011 => E,
    0b100 => H,
    0b101 => L,
    0b110 => MemRead(HL_read(), at),
    0b111 => A
}

val R_write : (bits(3), bits(8), int) -> unit

function R_write (r, v, at) = match r {
    0b000 => B = v,
    0b001 => C = v,
    0b010 => D = v,
    0b011 => E = v,
    0b100 => H = v,
    0b101 => L = v,
    0b110 => MemWrite(HL_read(), v, at),
    0b111 => A = v
}

/* cc: NZ, Z, NC, C. */

val Condition : bits(2) -> bool

function Condition (cc) = match cc {
    0b00 => FZ == 0b0,
    0b01 => FZ == 0b1,
    0b10 => FC == 0b0,
    0b11 => FC == 0b1
}

/* The immediate operands, fetched after the opcode. */

val Imm8 : unit -> bits(8)

function Imm8 () = MemFetch(PC + 0x0001, 2)

val Imm16 : unit -> bits(16)

function Imm16 () = {
    let lo = MemFetch(PC + 0x0001, 2);
    let hi = MemFetch(PC + 0x0002, 3);
    hi @ lo
}

/* The stack. The pushes and pops write and read the high byte at the
   higher address. */

val Push16 : (bits(16), int) -> unit

function Push16 (v, at) = {
    let hi_at = SP - 0x0001;
    let lo_at = SP - 0x0002;
    MemWrite(hi_at, v[15 .. 8], at);
    MemWrite(lo_at, v[7 .. 0], at + 1);
    SP = lo_at
}

val Pop16 : int -> bits(16)

function Pop16 (at) = {
    let hi_at = SP + 0x0001;
    let lo = MemRead(SP, at);
    let hi = MemRead(hi_at, at + 1);
    SP = SP + 0x0002;
    hi @ lo
}

/* Arithmetic. */

val AddWithCarry8 : (bits(8), bits(8), bits(1)) -> (bits(8), bits(1), bits(1))

function AddWithCarry8 (a, x, cin) = {
    let wide : bits(9) = ZeroExtend(a, 9) + ZeroExtend(x, 9) + ZeroExtend(cin, 9);
    let nib : bits(8) = (a & 0x0F) + (x & 0x0F) + ZeroExtend(cin, 8);
    (wide[7 .. 0], [nib[4]], [wide[8]])
}

val SubWithBorrow8 : (bits(8), bits(8), bits(1)) -> (bits(8), bits(1), bits(1))

function SubWithBorrow8 (a, x, cin) = {
    let wide : bits(9) = ZeroExtend(a, 9) - ZeroExtend(x, 9) - ZeroExtend(cin, 9);
    let nib : bits(8) = (a & 0x0F) - (x & 0x0F) - ZeroExtend(cin, 8);
    (wide[7 .. 0], [nib[4]], [wide[8]])
}

/* SP plus a signed offset, with the half carry and carry of the low byte. */

val SPPlusOffset : bits(8) -> (bits(16), bits(1), bits(1))

function SPPlusOffset (e) = {
    let sum = SP + SignExtend(e, 16);
    let lo = SP[7 .. 0];
    let hs : bits(8) = (lo & 0x0F) + (e & 0x0F);
    let cs : bits(9) = ZeroExtend(lo, 9) + ZeroExtend(e, 9);
    (sum, [hs[4]], [cs[8]])
}

/* The execute functions. Each is named for its group; the decode clauses
   bind the opcode fields. */

function execute_NOP () -> unit = Cycles(1)

function execute_LD_r_r (d : bits(3), s : bits(3)) -> unit = {
    let v = R_read(s, 2);
    R_write(d, v, 2);
    Cycles(if d == 0b110 | s == 0b110 then 2 else 1)
}

function execute_LD_r_n d : bits(3) -> unit = {
    let n = Imm8();
    R_write(d, n, 3);
    Cycles(if d == 0b110 then 3 else 2)
}

function execute_LD_rr_nn p : bits(2) -> unit = {
    let nn = Imm16();
    RP_write(p, nn);
    Cycles(3)
}

/* LD (BC),A; LD (DE),A; LD (HL+),A; LD (HL-),A and the loads back. */

val IndirectAddress : bits(2) -> bits(16)

function IndirectAddress (p) = match p {
    0b00 => BC_read(),
    0b01 => DE_read(),
    _ => HL_read()
}

val AdjustHL : bits(2) -> unit

function AdjustHL (p) = match p {
    0b10 => HL_write(HL_read() + 0x0001),
    0b11 => HL_write(HL_read() + 0xFFFF),
    _ => ()
}

function execute_LD_ind_A p : bits(2) -> unit = {
    MemWrite(IndirectAddress(p), A, 2);
    AdjustHL(p);
    Cycles(2)
}

function execute_LD_A_ind p : bits(2) -> unit = {
    A = MemRead(IndirectAddress(p), 2);
    AdjustHL(p);
    Cycles(2)
}

function execute_LD_nn_SP () -> unit = {
    let nn = Imm16();
    MemWrite(nn, SP[7 .. 0], 4);
    MemWrite(nn + 0x0001, SP[15 .. 8], 5);
    Cycles(5)
}

function execute_LDH_n_A () -> unit = {
    let n = Imm8();
    MemWrite(0xFF @ n, A, 3);
    Cycles(3)
}

function execute_LDH_A_n () -> unit = {
    let n = Imm8();
    A = MemRead(0xFF @ n, 3);
    Cycles(3)
}

function execute_LDH_C_A () -> unit = {
    MemWrite(0xFF @ C, A, 2);
    Cycles(2)
}

function execute_LDH_A_C () -> unit = {
    A = MemRead(0xFF @ C, 2);
    Cycles(2)
}

function execute_LD_nn_A () -> unit = {
    let nn = Imm16();
    MemWrite(nn, A, 4);
    Cycles(4)
}

function execute_LD_A_nn () -> unit = {
    let nn = Imm16();
    A = MemRead(nn, 4);
    Cycles(4)
}

function execute_LD_SP_HL () -> unit = {
    SP = HL_read();
    Cycles(2)
}

function execute_LD_HL_SP_e () -> unit = {
    let e = Imm8();
    let (sum, h, c) = SPPlusOffset(e);
    HL_write(sum);
    FZ = 0b0;
    FN = 0b0;
    FH = h;
    FC = c;
    Cycles(3)
}

function execute_PUSH p : bits(2) -> unit = {
    Push16(RP2_read(p), 3);
    Cycles(4)
}

function execute_POP p : bits(2) -> unit = {
    let v = Pop16(2);
    RP2_write(p, v);
    Cycles(3)
}

/* 8-bit arithmetic and logic: ADD, ADC, SUB, SBC, AND, XOR, OR, CP. */

val ALU8 : (bits(3), bits(8)) -> unit

function ALU8 (op, x) = {
    let a = A;
    result : bits(8) = undefined;
    n : bits(1) = 0b0;
    h : bits(1) = 0b0;
    c : bits(1) = 0b0;
    match op {
        0b000 => (result, h, c) = AddWithCarry8(a, x, 0b0),
        0b001 => (result, h, c) = AddWithCarry8(a, x, FC),
        0b010 => {
            (result, h, c) = SubWithBorrow8(a, x, 0b0);
            n = 0b1
        },
        0b011 => {
            (result, h, c) = SubWithBorrow8(a, x, FC);
            n = 0b1
        },
        0b100 => {
            result = a & x;
            h = 0b1
        },
        0b101 => result = xor_vec(a, x),
        0b110 => result = a | x,
        0b111 => {
            (result, h, c) = SubWithBorrow8(a, x, 0b0);
            n = 0b1
        }
    };
    FZ = IsZero8(result);
    FN = n;
    FH = h;
    FC = c;
    if op != 0b111 then {
        A = result
    }
}

function execute_ALU_r (op : bits(3), s : bits(3)) -> unit = {
    let x = R_read(s, 2);
    ALU8(op, x);
    Cycles(if s == 0b110 then 2 else 1)
}

function execute_ALU_n op : bits(3) -> unit = {
    let x = Imm8();
    ALU8(op, x);
    Cycles(2)
}

function execute_INC_r r : bits(3) -> unit = {
    let x = R_read(r, 2);
    let res = x + 0x01;
    FZ = IsZero8(res);
    FN = 0b0;
    FH = Bit((x & 0x0F) == 0x0F);
    R_write(r, res, 3);
    Cycles(if r == 0b110 then 3 else 1)
}

function execute_DEC_r r : bits(3) -> unit = {
    let x = R_read(r, 2);
    let res = x - 0x01;
    FZ = IsZero8(res);
    FN = 0b1;
    FH = Bit((x & 0x0F) == 0x00);
    R_write(r, res, 3);
    Cycles(if r == 0b110 then 3 else 1)
}

function execute_INC_rr p : bits(2) -> unit = {
    RP_write(p, RP_read(p) + 0x0001);
    Cycles(2)
}

function execute_DEC_rr p : bits(2) -> unit = {
    RP_write(p, RP_read(p) - 0x0001);
    Cycles(2)
}

function execute_ADD_HL_rr p : bits(2) -> unit = {
    let hl = HL_read();
    let rr = RP_read(p);
    let wide : bits(17) = ZeroExtend(hl, 17) + ZeroExtend(rr, 17);
    let hs : bits(16) = (hl & 0x0FFF) + (rr & 0x0FFF);
    HL_write(wide[15 .. 0]);
    FN = 0b0;
    FH = [hs[12]];
    FC = [wide[16]];
    Cycles(2)
}

function execute_ADD_SP_e () -> unit = {
    let e = Imm8();
    let (sum, h, c) = SPPlusOffset(e);
    SP = sum;
    FZ = 0b0;
    FN = 0b0;
    FH = h;
    FC = c;
    Cycles(4)
}

function execute_DAA () -> unit = {
    let a = A;
    let adj6 : bool = FH == 0b1 | (FN == 0b0 & unsigned(a & 0x0F) > 9);
    let adj60 : bool = FC == 0b1 | (FN == 0b0 & unsigned(a) > 153);
    let adjust : bits(8) = (if adj6 then 0x06 else 0x00) | (if adj60 then 0x60 else 0x00);
    let res = if FN == 0b1 then a - adjust else a + adjust;
    A = res;
    FZ = IsZero8(res);
    FH = 0b0;
    FC = Bit(adj60);
    Cycles(1)
}

function execute_CPL () -> unit = {
    A = not_vec(A);
    FN = 0b1;
    FH = 0b1;
    Cycles(1)
}

function execute_SCF () -> unit = {
    FN = 0b0;
    FH = 0b0;
    FC = 0b1;
    Cycles(1)
}

function execute_CCF () -> unit = {
    FN = 0b0;
    FH = 0b0;
    FC = not_vec(FC);
    Cycles(1)
}

/* Rotates and shifts: RLC, RRC, RL, RR, SLA, SRA, SWAP, SRL. Returns the
   result and the carry out. */

val Shift8 : (bits(3), bits(8)) -> (bits(8), bits(1))

function Shift8 (op, x) = match op {
    0b000 => (x[6 .. 0] @ [x[7]], [x[7]]),
    0b001 => ([x[0]] @ x[7 .. 1], [x[0]]),
    0b010 => (x[6 .. 0] @ FC, [x[7]]),
    0b011 => (FC @ x[7 .. 1], [x[0]]),
    0b100 => (x[6 .. 0] @ 0b0, [x[7]]),
    0b101 => ([x[7]] @ x[7 .. 1], [x[0]]),
    0b110 => (x[3 .. 0] @ x[7 .. 4], 0b0),
    0b111 => (0b0 @ x[7 .. 1], [x[0]])
}

/* RLCA, RRCA, RLA, RRA: Z is cleared. */

function execute_ROT_A op : bits(2) -> unit = {
    let (res, c) = Shift8(0b0 @ op, A);
    A = res;
    FZ = 0b0;
    FN = 0b0;
    FH = 0b0;
    FC = c;
    Cycles(1)
}

function execute_CB_SHIFT (op : bits(3), r : bits(3)) -> unit = {
    let x = R_read(r, 3);
    let (res, c) = Shift8(op, x);
    FZ = IsZero8(res);
    FN = 0b0;
    FH = 0b0;
    FC = c;
    R_write(r, res, 4);
    Cycles(if r == 0b110 then 4 else 2)
}

function execute_CB_BIT (b : bits(3), r : bits(3)) -> unit = {
    let x = R_read(r, 3);
    FZ = Bit(x[unsigned(b)] == bitzero);
    FN = 0b0;
    FH = 0b1;
    Cycles(if r == 0b110 then 3 else 2)
}

function execute_CB_RES (b : bits(3), r : bits(3)) -> unit = {
    let x = R_read(r, 3);
    R_write(r, [x with unsigned(b) = bitzero], 4);
    Cycles(if r == 0b110 then 4 else 2)
}

function execute_CB_SET (b : bits(3), r : bits(3)) -> unit = {
    let x = R_read(r, 3);
    R_write(r, [x with unsigned(b) = bitone], 4);
    Cycles(if r == 0b110 then 4 else 2)
}

/* Control transfers. */

function execute_JR () -> unit = {
    let e = Imm8();
    BranchTo(PC + 0x0002 + SignExtend(e, 16), BranchType_JMP);
    Cycles(3)
}

function execute_JR_cc cc : bits(2) -> unit = {
    let e = Imm8();
    if Condition(cc) then {
        BranchTo(PC + 0x0002 + SignExtend(e, 16), BranchType_JMP);
        Cycles(3)
    } else {
        Cycles(2)
    }
}

function execute_JP () -> unit = {
    let nn = Imm16();
    BranchTo(nn, BranchType_JMP);
    Cycles(4)
}

function execute_JP_cc cc : bits(2) -> unit = {
    let nn = Imm16();
    if Condition(cc) then {
        BranchTo(nn, BranchType_JMP);
        Cycles(4)
    } else {
        Cycles(3)
    }
}

function execute_JP_HL () -> unit = {
    BranchTo(HL_read(), BranchType_JMP);
    Cycles(1)
}

function execute_CALL () -> unit = {
    let nn = Imm16();
    Push16(PC + 0x0003, 5);
    BranchTo(nn, BranchType_CALL);
    Cycles(6)
}

function execute_CALL_cc cc : bits(2) -> unit = {
    let nn = Imm16();
    if Condition(cc) then {
        Push16(PC + 0x0003, 5);
        BranchTo(nn, BranchType_CALL);
        Cycles(6)
    } else {
        Cycles(3)
    }
}

function execute_RST y : bits(3) -> unit = {
    Push16(PC + 0x0001, 3);
    BranchTo(ZeroExtend(0b00 @ y @ 0b000, 16), BranchType_CALL);
    Cycles(4)
}

function execute_RET () -> unit = {
    let target = Pop16(2);
    BranchTo(target, BranchType_RET);
    Cycles(4)
}

function execute_RETI () -> unit = {
    let target = Pop16(2);
    SM83_RETI();
    BranchTo(target, BranchType_RET);
    Cycles(4)
}

function execute_RET_cc cc : bits(2) -> unit = {
    if Condition(cc) then {
        let target = Pop16(3);
        BranchTo(target, BranchType_RET);
        Cycles(5)
    } else {
        Cycles(2)
    }
}

/* Calls into the per-CPU layer. HALT and STOP decide in the layer what runs
   next: the HALT bug, and whether STOP takes one byte or two. */

function execute_HALT () -> unit = {
    Cycles(1);
    SM83_Halt()
}

function execute_STOP () -> unit = {
    Cycles(1);
    SM83_Stop()
}

function execute_DI () -> unit = {
    SM83_DI();
    Cycles(1)
}

function execute_EI () -> unit = {
    Cycles(1);
    SM83_EI()
}

/* Decode: the opcode byte, and the byte after 0xCB. A clause pattern names
   the opcode's fields; the first clause that matches (with its guard) is
   taken. */

val __DecodeSM83 : bits(8) -> unit

scattered function __DecodeSM83

val __DecodeSM83_CB : bits(8) -> unit

scattered function __DecodeSM83_CB

function clause __DecodeSM83 (0x00) = execute_NOP()

function clause __DecodeSM83 (0x08) = execute_LD_nn_SP()

function clause __DecodeSM83 (0x10) = execute_STOP()

function clause __DecodeSM83 (0x18) = execute_JR()

function clause __DecodeSM83 (0b001 @ cc : bits(2) @ 0b000) = execute_JR_cc(cc)

function clause __DecodeSM83 (0b00 @ p : bits(2) @ 0b0001) = execute_LD_rr_nn(p)

function clause __DecodeSM83 (0b00 @ p : bits(2) @ 0b1001) = execute_ADD_HL_rr(p)

function clause __DecodeSM83 (0b00 @ p : bits(2) @ 0b0010) = execute_LD_ind_A(p)

function clause __DecodeSM83 (0b00 @ p : bits(2) @ 0b1010) = execute_LD_A_ind(p)

function clause __DecodeSM83 (0b00 @ p : bits(2) @ 0b0011) = execute_INC_rr(p)

function clause __DecodeSM83 (0b00 @ p : bits(2) @ 0b1011) = execute_DEC_rr(p)

function clause __DecodeSM83 (0b00 @ r : bits(3) @ 0b100) = execute_INC_r(r)

function clause __DecodeSM83 (0b00 @ r : bits(3) @ 0b101) = execute_DEC_r(r)

function clause __DecodeSM83 (0b00 @ r : bits(3) @ 0b110) = execute_LD_r_n(r)

function clause __DecodeSM83 (0b000 @ op : bits(2) @ 0b111) = execute_ROT_A(op)

function clause __DecodeSM83 (0x27) = execute_DAA()

function clause __DecodeSM83 (0x2F) = execute_CPL()

function clause __DecodeSM83 (0x37) = execute_SCF()

function clause __DecodeSM83 (0x3F) = execute_CCF()

function clause __DecodeSM83 (0x76) = execute_HALT()

function clause __DecodeSM83 ((0b01 @ d : bits(3) @ s : bits(3)) if d != 0b110 | s != 0b110) = execute_LD_r_r(d, s)

function clause __DecodeSM83 (0b10 @ op : bits(3) @ s : bits(3)) = execute_ALU_r(op, s)

function clause __DecodeSM83 (0b110 @ cc : bits(2) @ 0b000) = execute_RET_cc(cc)

function clause __DecodeSM83 (0xE0) = execute_LDH_n_A()

function clause __DecodeSM83 (0xE8) = execute_ADD_SP_e()

function clause __DecodeSM83 (0xF0) = execute_LDH_A_n()

function clause __DecodeSM83 (0xF8) = execute_LD_HL_SP_e()

function clause __DecodeSM83 (0b11 @ p : bits(2) @ 0b0001) = execute_POP(p)

function clause __DecodeSM83 (0xC9) = execute_RET()

function clause __DecodeSM83 (0xD9) = execute_RETI()

function clause __DecodeSM83 (0xE9) = execute_JP_HL()

function clause __DecodeSM83 (0xF9) = execute_LD_SP_HL()

function clause __DecodeSM83 (0b110 @ cc : bits(2) @ 0b010) = execute_JP_cc(cc)

function clause __DecodeSM83 (0xE2) = execute_LDH_C_A()

function clause __DecodeSM83 (0xEA) = execute_LD_nn_A()

function clause __DecodeSM83 (0xF2) = execute_LDH_A_C()

function clause __DecodeSM83 (0xFA) = execute_LD_A_nn()

function clause __DecodeSM83 (0xC3) = execute_JP()

function clause __DecodeSM83 (0xCB) = __DecodeSM83_CB(MemFetch(PC + 0x0001, 2))

function clause __DecodeSM83 (0xF3) = execute_DI()

function clause __DecodeSM83 (0xFB) = execute_EI()

function clause __DecodeSM83 (0b110 @ cc : bits(2) @ 0b100) = execute_CALL_cc(cc)

function clause __DecodeSM83 (0b11 @ p : bits(2) @ 0b0101) = execute_PUSH(p)

function clause __DecodeSM83 (0xCD) = execute_CALL()

function clause __DecodeSM83 (0b11 @ op : bits(3) @ 0b110) = execute_ALU_n(op)

function clause __DecodeSM83 (0b11 @ y : bits(3) @ 0b111) = execute_RST(y)

function clause __DecodeSM83 (op) = SM83_Illegal(op)

function clause __DecodeSM83_CB (0b00 @ op : bits(3) @ r : bits(3)) = execute_CB_SHIFT(op, r)

function clause __DecodeSM83_CB (0b01 @ b : bits(3) @ r : bits(3)) = execute_CB_BIT(b, r)

function clause __DecodeSM83_CB (0b10 @ b : bits(3) @ r : bits(3)) = execute_CB_RES(b, r)

function clause __DecodeSM83_CB (0b11 @ b : bits(3) @ r : bits(3)) = execute_CB_SET(b, r)

end __DecodeSM83

end __DecodeSM83_CB

/* One instruction: fetch the opcode in M-cycle 1 and run it. */

val Step : unit -> unit

function Step () = __DecodeSM83(MemFetch(PC, 1))
