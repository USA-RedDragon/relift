/*!
The Zilog Z80 (NMOS) instruction set, every prefix page: the base page, CB,
ED, DD and FD (IX and IY in place of HL), and DDCB and FDCB. Part of
relift, MIT licensed.

The flag rules, MEMPTR (WZ) and the timings follow the Zilog Z80 CPU User
Manual (UM0080), "The Undocumented Z80 Documented" (Sean Young), the
MEMPTR notes of boo_boo and Vladimir Kladov, and the Z80 core of ares
(component/processor/z80, ISC licence, Copyright (c) 2004-2025 ares team,
Near et al; see the notice at the end of this file), from which the flag
algorithms of the block I/O instructions and the X/Y rules are ported.

State: A and the eight flags of F one bit each (S Z Y H X P/V N C, bits 7
to 0 of F), B C D E H L, IX, IY, SP, the shadow pairs AF' BC' DE' HL', I, R,
WZ (MEMPTR), IFF1, IFF2, IM, Q (the last instruction set the flags: SCF and
CCF read it for X and Y), EIDelay (the last instruction was EI) and LDAIR
(the last instruction was LD A,I or LD A,R; NMOS clears P/V when an
interrupt is taken after one). PC holds the address of the instruction.

An instruction is Begin() and then one clause of its page's decode function;
the generator runs both, as Step does. Page says which page the template is
for (0 base, 1 CB, 2 ED, 3 DD, 4 FD, 5 DDCB, 6 FDCB); it and FetchAddr (the
address of the next instruction byte) and QIn (Q as the instruction found
it) are lift-time values of the model, not guest state.

Every bus access is a cut call with the T-state (from 0, the start of the
instruction) at which its machine cycle begins: MemFetch for instruction
bytes (the opcode fetches are M1 cycles of 4 T-states, the others 3),
MemRead and MemWrite for data (3), PortRead and PortWrite for I/O (4).
Cycles gives the instruction's total; a taken branch gives the taken total.
BranchTo is a control transfer; an instruction without one goes on to the
next. Decide marks the condition of a conditional CALL or RET, whose taken
path has bus accesses. Z80_Halt is HALT, for the per-CPU layer.
*/

default Order dec

$include <prelude.sail>

register A : bits(8)
register FS : bits(1)
register FZ : bits(1)
register FY : bits(1)
register FH : bits(1)
register FX : bits(1)
register FP : bits(1)
register FN : bits(1)
register FC : bits(1)
register B : bits(8)
register C : bits(8)
register D : bits(8)
register E : bits(8)
register H : bits(8)
register L : bits(8)
register IX : bits(16)
register IY : bits(16)
register SP : bits(16)
register PC : bits(16)
register WZ : bits(16)
register I : bits(8)
register R : bits(8)
register AF_ : bits(16)
register BC_ : bits(16)
register DE_ : bits(16)
register HL_ : bits(16)
register IFF1 : bits(1)
register IFF2 : bits(1)
register IM : bits(2)
register Q : bits(1)
register EIDelay : bits(1)
register LDAIR : bits(1)

/* Lift-time values of the model. */
register Page : bits(3)
register FetchAddr : bits(16)
register QIn : bits(1)
register XYAddr : bits(16)

enum BranchType = {BranchType_JMP, BranchType_CALL, BranchType_RET}

val MemFetch : (bits(16), int) -> bits(8)

val MemRead : (bits(16), int) -> bits(8)

val MemWrite : (bits(16), bits(8), int) -> unit

val PortRead : (bits(16), int) -> bits(8)

val PortWrite : (bits(16), bits(8), int) -> unit

val Cycles : int -> unit

val BranchTo : (bits(16), BranchType) -> unit

val Decide : bool -> bool

val Z80_Halt : unit -> unit

val ZeroExtend : forall 'n 'm, 'm >= 'n. (bits('n), int('m)) -> bits('m)

function ZeroExtend (x, m) = sail_zero_extend(x, m)

val SignExtend : forall 'n 'm, 'm >= 'n. (bits('n), int('m)) -> bits('m)

function SignExtend (x, m) = sail_sign_extend(x, m)

val Bit : bool -> bits(1)

function Bit (b) = if b then 0b1 else 0b0

val IsZero8 : bits(8) -> bits(1)

function IsZero8 (x) = Bit(x == 0x00)

/* 1 when x has an even number of set bits. */

val Parity8 : bits(8) -> bits(1)

function Parity8 (x) = {
    let a : bits(4) = xor_vec(x[7 .. 4], x[3 .. 0]);
    let b : bits(2) = xor_vec(a[3 .. 2], a[1 .. 0]);
    not_vec(xor_vec([b[1]], [b[0]]))
}

/* The page and its timing. */

val XYPage : unit -> bool

function XYPage () = Page == 0b011 | Page == 0b100

val XYCBPage : unit -> bool

function XYCBPage () = Page == 0b101 | Page == 0b110

/* The T-state at which the instruction's own work begins: after the opcode
   fetch, the prefix and opcode fetches, or on DDCB and FDCB after the
   displacement and opcode reads and two more T-states. */

val T0 : unit -> int

function T0 () = match Page {
    0b000 => 4,
    0b101 => 16,
    0b110 => 16,
    _ => 8
}

/* R counts opcode fetches in its low seven bits. */

val IncR : unit -> unit

function IncR () = {
    R = [R[7]] @ (R[6 .. 0] + 0b0000001)
}

/* The start of every instruction: the opcode fetches, the R increments,
   and for DDCB and FDCB the displacement (which sets WZ) and the opcode
   read. */

val Begin : unit -> unit

function Begin () = {
    /* A DD or FD prefix is an instruction of its own for Q: it sets no
       flags, so SCF and CCF after one see Q clear. */
    QIn = if Page == 0b011 | Page == 0b100 then 0b0 else Q;
    Q = 0b0;
    EIDelay = 0b0;
    LDAIR = 0b0;
    let op0 = MemFetch(PC, 0);
    IncR();
    match Page {
        0b000 => FetchAddr = PC + 0x0001,
        0b101 => {
            let op1 = MemFetch(PC + 0x0001, 4);
            IncR();
            let d = MemFetch(PC + 0x0002, 8);
            let op3 = MemFetch(PC + 0x0003, 11);
            XYAddr = IX + SignExtend(d, 16);
            WZ = XYAddr;
            FetchAddr = PC + 0x0004
        },
        0b110 => {
            let op1 = MemFetch(PC + 0x0001, 4);
            IncR();
            let d = MemFetch(PC + 0x0002, 8);
            let op3 = MemFetch(PC + 0x0003, 11);
            XYAddr = IY + SignExtend(d, 16);
            WZ = XYAddr;
            FetchAddr = PC + 0x0004
        },
        _ => {
            let op1 = MemFetch(PC + 0x0001, 4);
            IncR();
            FetchAddr = PC + 0x0002
        }
    }
}

/* Bus accesses at T-state k of the instruction's own work. */

val Rd : (bits(16), int) -> bits(8)

function Rd (a, k) = MemRead(a, T0() + k)

val Wr : (bits(16), bits(8), int) -> unit

function Wr (a, v, k) = MemWrite(a, v, T0() + k)

val In : (bits(16), int) -> bits(8)

function In (a, k) = PortRead(a, T0() + k)

val Out : (bits(16), bits(8), int) -> unit

function Out (a, v, k) = PortWrite(a, v, T0() + k)

val Done : int -> unit

function Done (k) = Cycles(T0() + k)

/* The next instruction byte, read at T-state k of the instruction's work. */

val Imm8 : int -> bits(8)

function Imm8 (k) = {
    let v = MemFetch(FetchAddr, T0() + k);
    FetchAddr = FetchAddr + 0x0001;
    v
}

val Imm16 : int -> bits(16)

function Imm16 (k) = {
    let lo = Imm8(k);
    let hi = Imm8(k + 3);
    hi @ lo
}

/* The address of the next instruction. */

val NextPC : unit -> bits(16)

function NextPC () = FetchAddr

/* Register pairs. HLX is HL, or IX or IY on their pages. */

val BC_read : unit -> bits(16)

function BC_read () = B @ C

val DE_read : unit -> bits(16)

function DE_read () = D @ E

val HL_read : unit -> bits(16)

function HL_read () = H @ L

val BC_write : bits(16) -> unit

function BC_write (v) = {
    B = v[15 .. 8];
    C = v[7 .. 0]
}

val DE_write : bits(16) -> unit

function DE_write (v) = {
    D = v[15 .. 8];
    E = v[7 .. 0]
}

val HL_write : bits(16) -> unit

function HL_write (v) = {
    H = v[15 .. 8];
    L = v[7 .. 0]
}

val HLX_read : unit -> bits(16)

function HLX_read () = match Page {
    0b011 => IX,
    0b100 => IY,
    _ => H @ L
}

val HLX_write : bits(16) -> unit

function HLX_write (v) = match Page {
    0b011 => IX = v,
    0b100 => IY = v,
    _ => HL_write(v)
}

val F_read : unit -> bits(8)

function F_read () = FS @ FZ @ FY @ FH @ FX @ FP @ FN @ FC

val F_write : bits(8) -> unit

function F_write (v) = {
    FS = [v[7]];
    FZ = [v[6]];
    FY = [v[5]];
    FH = [v[4]];
    FX = [v[3]];
    FP = [v[2]];
    FN = [v[1]];
    FC = [v[0]]
}

/* rp: BC, DE, HL (IX, IY), SP. */

val RP_read : bits(2) -> bits(16)

function RP_read (p) = match p {
    0b00 => BC_read(),
    0b01 => DE_read(),
    0b10 => HLX_read(),
    0b11 => SP
}

val RP_write : (bits(2), bits(16)) -> unit

function RP_write (p, v) = match p {
    0b00 => BC_write(v),
    0b01 => DE_write(v),
    0b10 => HLX_write(v),
    0b11 => SP = v
}

/* rp2: BC, DE, HL (IX, IY), AF. */

val RP2_read : bits(2) -> bits(16)

function RP2_read (p) = match p {
    0b11 => A @ F_read(),
    _ => RP_read(p)
}

val RP2_write : (bits(2), bits(16)) -> unit

function RP2_write (p, v) = match p {
    0b11 => {
        A = v[15 .. 8];
        F_write(v[7 .. 0])
    },
    _ => RP_write(p, v)
}

/* r without (HL): B, C, D, E, H, L, A. Reg8 gives IXH and IXL (IYH, IYL)
   for H and L on the DD and FD pages; Reg8True always H and L, for the
   instructions that also have a memory operand. */

val Reg8 : bits(3) -> bits(8)

function Reg8 (r) = match r {
    0b000 => B,
    0b001 => C,
    0b010 => D,
    0b011 => E,
    0b100 => HLX_read()[15 .. 8],
    0b101 => HLX_read()[7 .. 0],
    _ => A
}

val Reg8_write : (bits(3), bits(8)) -> unit

function Reg8_write (r, v) = match r {
    0b000 => B = v,
    0b001 => C = v,
    0b010 => D = v,
    0b011 => E = v,
    0b100 => HLX_write(v @ HLX_read()[7 .. 0]),
    0b101 => HLX_write(HLX_read()[15 .. 8] @ v),
    _ => A = v
}

val Reg8True : bits(3) -> bits(8)

function Reg8True (r) = match r {
    0b000 => B,
    0b001 => C,
    0b010 => D,
    0b011 => E,
    0b100 => H,
    0b101 => L,
    _ => A
}

val Reg8True_write : (bits(3), bits(8)) -> unit

function Reg8True_write (r, v) = match r {
    0b000 => B = v,
    0b001 => C = v,
    0b010 => D = v,
    0b011 => E = v,
    0b100 => H = v,
    0b101 => L = v,
    _ => A = v
}

/* The memory operand (HL), or (IX+d) and (IY+d) on their pages: its
   address and the offset its access cycles move by (the displacement read
   and five more T-states). The displacement is read at k. */

val EA : int -> (bits(16), int)

function EA (k) = {
    if XYPage() then {
        let d = Imm8(k);
        let ea = HLX_read() + SignExtend(d, 16);
        WZ = ea;
        (ea, k + 8)
    } else {
        (H @ L, k)
    }
}

/* cc: NZ, Z, NC, C, PO, PE, P, M. */

val CondValue : bits(3) -> bool

function CondValue (cc) = match cc {
    0b000 => FZ == 0b0,
    0b001 => FZ == 0b1,
    0b010 => FC == 0b0,
    0b011 => FC == 0b1,
    0b100 => FP == 0b0,
    0b101 => FP == 0b1,
    0b110 => FS == 0b0,
    0b111 => FS == 0b1
}

/* The stack: the high byte goes to the higher address and is pushed
   first. */

val Push16 : (bits(16), int) -> unit

function Push16 (v, k) = {
    let hi_at = SP - 0x0001;
    let lo_at = SP - 0x0002;
    Wr(hi_at, v[15 .. 8], k);
    Wr(lo_at, v[7 .. 0], k + 3);
    SP = lo_at
}

val Pop16 : int -> bits(16)

function Pop16 (k) = {
    let lo = Rd(SP, k);
    let hi = Rd(SP + 0x0001, k + 3);
    SP = SP + 0x0002;
    hi @ lo
}

/* Flags. */

val SetSZXY : bits(8) -> unit

function SetSZXY (v) = {
    FS = [v[7]];
    FZ = IsZero8(v);
    FY = [v[5]];
    FX = [v[3]]
}

/* x + y + c and x - y - c with the flags of ADD, ADC, SUB, SBC, NEG; CP
   takes X and Y from the operand instead. */

val Add8 : (bits(8), bits(8), bits(1)) -> bits(8)

function Add8 (x, y, c) = {
    let z : bits(9) = ZeroExtend(x, 9) + ZeroExtend(y, 9) + ZeroExtend(c, 9);
    let r = z[7 .. 0];
    FC = [z[8]];
    FN = 0b0;
    FP = [(not_vec(xor_vec(x, y)) & xor_vec(x, r))[7]];
    FH = [xor_vec(xor_vec(x, y), r)[4]];
    SetSZXY(r);
    r
}

val Sub8 : (bits(8), bits(8), bits(1)) -> bits(8)

function Sub8 (x, y, c) = {
    let z : bits(9) = ZeroExtend(x, 9) - ZeroExtend(y, 9) - ZeroExtend(c, 9);
    let r = z[7 .. 0];
    FC = [z[8]];
    FN = 0b1;
    FP = [(xor_vec(x, y) & xor_vec(x, r))[7]];
    FH = [xor_vec(xor_vec(x, y), r)[4]];
    SetSZXY(r);
    r
}

val Logic : (bits(8), bits(1)) -> unit

function Logic (r, h) = {
    FC = 0b0;
    FN = 0b0;
    FP = Parity8(r);
    FH = h;
    SetSZXY(r)
}

/* ADD ADC SUB SBC AND XOR OR CP. */

val ALU8 : (bits(3), bits(8)) -> unit

function ALU8 (op, y) = {
    Q = 0b1;
    match op {
        0b000 => A = Add8(A, y, 0b0),
        0b001 => A = Add8(A, y, FC),
        0b010 => A = Sub8(A, y, 0b0),
        0b011 => A = Sub8(A, y, FC),
        0b100 => {
            let r = A & y;
            A = r;
            Logic(r, 0b1)
        },
        0b101 => {
            let r = xor_vec(A, y);
            A = r;
            Logic(r, 0b0)
        },
        0b110 => {
            let r = A | y;
            A = r;
            Logic(r, 0b0)
        },
        0b111 => {
            let r = Sub8(A, y, 0b0);
            FY = [y[5]];
            FX = [y[3]]
        }
    }
}

val Inc8 : bits(8) -> bits(8)

function Inc8 (x) = {
    let z = x + 0x01;
    Q = 0b1;
    FN = 0b0;
    FP = Bit(z == 0x80);
    FH = Bit(z[3 .. 0] == 0x0);
    SetSZXY(z);
    z
}

val Dec8 : bits(8) -> bits(8)

function Dec8 (x) = {
    let z = x - 0x01;
    Q = 0b1;
    FN = 0b1;
    FP = Bit(z == 0x7F);
    FH = Bit(z[3 .. 0] == 0xF);
    SetSZXY(z);
    z
}

/* RLC RRC RL RR SLA SRA SLL SRL, with every flag of the CB page. */

val Shift8 : (bits(3), bits(8)) -> bits(8)

function Shift8 (op, x) = {
    let (r, c) : (bits(8), bits(1)) = match op {
        0b000 => (x[6 .. 0] @ [x[7]], [x[7]]),
        0b001 => ([x[0]] @ x[7 .. 1], [x[0]]),
        0b010 => (x[6 .. 0] @ FC, [x[7]]),
        0b011 => (FC @ x[7 .. 1], [x[0]]),
        0b100 => (x[6 .. 0] @ 0b0, [x[7]]),
        0b101 => ([x[7]] @ x[7 .. 1], [x[0]]),
        0b110 => (x[6 .. 0] @ 0b1, [x[7]]),
        0b111 => (0b0 @ x[7 .. 1], [x[0]])
    };
    Q = 0b1;
    FC = c;
    FN = 0b0;
    FH = 0b0;
    FP = Parity8(r);
    SetSZXY(r);
    r
}

/* BIT b: X and Y come from `xy` (the register, or the high byte of WZ for
   a memory operand). */

val BitTest : (bits(3), bits(8), bits(8)) -> unit

function BitTest (b, x, xy) = {
    let z = [x[unsigned(b)]];
    Q = 0b1;
    FZ = not_vec(z);
    FP = not_vec(z);
    FS = Bit(b == 0b111 & z == 0b1);
    FH = 0b1;
    FN = 0b0;
    FY = [xy[5]];
    FX = [xy[3]]
}

/* The execute functions of the base page (and the DD and FD pages). */

function execute_NOP () -> unit = Done(0)

function execute_EX_AF () -> unit = {
    let t = A @ F_read();
    A = AF_[15 .. 8];
    F_write(AF_[7 .. 0]);
    AF_ = t;
    Done(0)
}

function execute_DJNZ () -> unit = {
    let e = Imm8(1);
    let b = B - 0x01;
    B = b;
    if b != 0x00 then {
        let t = NextPC() + SignExtend(e, 16);
        WZ = t;
        BranchTo(t, BranchType_JMP);
        Done(9)
    } else {
        Done(4)
    }
}

function execute_JR () -> unit = {
    let e = Imm8(0);
    let t = NextPC() + SignExtend(e, 16);
    WZ = t;
    BranchTo(t, BranchType_JMP);
    Done(8)
}

function execute_JR_cc cc : bits(2) -> unit = {
    let e = Imm8(0);
    if CondValue(0b0 @ cc) then {
        let t = NextPC() + SignExtend(e, 16);
        WZ = t;
        BranchTo(t, BranchType_JMP);
        Done(8)
    } else {
        Done(3)
    }
}

function execute_LD_rr_nn p : bits(2) -> unit = {
    let nn = Imm16(0);
    RP_write(p, nn);
    Done(6)
}

function execute_ADD_HL_rr p : bits(2) -> unit = {
    let x = HLX_read();
    let y = RP_read(p);
    WZ = x + 0x0001;
    let z : bits(17) = ZeroExtend(x, 17) + ZeroExtend(y, 17);
    let r = z[15 .. 0];
    HLX_write(r);
    Q = 0b1;
    FC = [z[16]];
    FN = 0b0;
    FH = [xor_vec(xor_vec(x, y), r)[12]];
    FY = [r[13]];
    FX = [r[11]];
    Done(7)
}

function execute_LD_ind_A p : bits(1) -> unit = {
    let a = if p == 0b0 then BC_read() else DE_read();
    Wr(a, A, 0);
    WZ = A @ (a[7 .. 0] + 0x01);
    Done(3)
}

function execute_LD_A_ind p : bits(1) -> unit = {
    let a = if p == 0b0 then BC_read() else DE_read();
    A = Rd(a, 0);
    WZ = a + 0x0001;
    Done(3)
}

function execute_LD_nn_HL () -> unit = {
    let nn = Imm16(0);
    let v = HLX_read();
    Wr(nn, v[7 .. 0], 6);
    Wr(nn + 0x0001, v[15 .. 8], 9);
    WZ = nn + 0x0001;
    Done(12)
}

function execute_LD_HL_nn () -> unit = {
    let nn = Imm16(0);
    let lo = Rd(nn, 6);
    let hi = Rd(nn + 0x0001, 9);
    HLX_write(hi @ lo);
    WZ = nn + 0x0001;
    Done(12)
}

function execute_LD_nn_A () -> unit = {
    let nn = Imm16(0);
    Wr(nn, A, 6);
    WZ = A @ (nn[7 .. 0] + 0x01);
    Done(9)
}

function execute_LD_A_nn () -> unit = {
    let nn = Imm16(0);
    A = Rd(nn, 6);
    WZ = nn + 0x0001;
    Done(9)
}

function execute_INC_rr p : bits(2) -> unit = {
    RP_write(p, RP_read(p) + 0x0001);
    Done(2)
}

function execute_DEC_rr p : bits(2) -> unit = {
    RP_write(p, RP_read(p) - 0x0001);
    Done(2)
}

function execute_INC_r r : bits(3) -> unit = {
    if r == 0b110 then {
        let (ea, k) = EA(0);
        let v = Rd(ea, k);
        Wr(ea, Inc8(v), k + 4);
        Done(k + 7)
    } else {
        Reg8_write(r, Inc8(Reg8(r)));
        Done(0)
    }
}

function execute_DEC_r r : bits(3) -> unit = {
    if r == 0b110 then {
        let (ea, k) = EA(0);
        let v = Rd(ea, k);
        Wr(ea, Dec8(v), k + 4);
        Done(k + 7)
    } else {
        Reg8_write(r, Dec8(Reg8(r)));
        Done(0)
    }
}

function execute_LD_r_n r : bits(3) -> unit = {
    if r == 0b110 then {
        if XYPage() then {
            let d = Imm8(0);
            let n = Imm8(3);
            let ea = HLX_read() + SignExtend(d, 16);
            WZ = ea;
            Wr(ea, n, 8);
            Done(11)
        } else {
            let n = Imm8(0);
            Wr(H @ L, n, 3);
            Done(6)
        }
    } else {
        Reg8_write(r, Imm8(0));
        Done(3)
    }
}

/* RLCA RRCA RLA RRA: S, Z and P/V are kept. */

function execute_ROT_A op : bits(2) -> unit = {
    let x = A;
    let (r, c) : (bits(8), bits(1)) = match op {
        0b00 => (x[6 .. 0] @ [x[7]], [x[7]]),
        0b01 => ([x[0]] @ x[7 .. 1], [x[0]]),
        0b10 => (x[6 .. 0] @ FC, [x[7]]),
        0b11 => (FC @ x[7 .. 1], [x[0]])
    };
    A = r;
    Q = 0b1;
    FC = c;
    FN = 0b0;
    FH = 0b0;
    FY = [r[5]];
    FX = [r[3]];
    Done(0)
}

function execute_DAA () -> unit = {
    let a = A;
    let hi : bool = FC == 0b1 | unsigned(a) > 153;
    let lo : bool = FH == 0b1 | unsigned(a[3 .. 0]) > 9;
    let adj : bits(8) = (if hi then 0x60 else 0x00) | (if lo then 0x06 else 0x00);
    let r = if FN == 0b1 then a - adj else a + adj;
    A = r;
    Q = 0b1;
    if hi then {
        FC = 0b1
    };
    FP = Parity8(r);
    FH = [xor_vec(a, r)[4]];
    SetSZXY(r);
    Done(0)
}

function execute_CPL () -> unit = {
    let r = not_vec(A);
    A = r;
    Q = 0b1;
    FN = 0b1;
    FH = 0b1;
    FY = [r[5]];
    FX = [r[3]];
    Done(0)
}

/* SCF and CCF: X and Y are A's bits ORed with the flags' own, unless the
   instruction before set the flags (Q). */

val SCF_CCF_XY : unit -> unit

function SCF_CCF_XY () = {
    let keep = not_vec(QIn);
    FY = (FY & keep) | [A[5]];
    FX = (FX & keep) | [A[3]]
}

function execute_SCF () -> unit = {
    SCF_CCF_XY();
    Q = 0b1;
    FC = 0b1;
    FN = 0b0;
    FH = 0b0;
    Done(0)
}

function execute_CCF () -> unit = {
    SCF_CCF_XY();
    let c = FC;
    Q = 0b1;
    FH = c;
    FC = not_vec(c);
    FN = 0b0;
    Done(0)
}

function execute_HALT () -> unit = {
    Done(0);
    Z80_Halt()
}

/* LD r,r'. With a memory operand the other register is H or L itself. */

function execute_LD_r_r (d : bits(3), s : bits(3)) -> unit = {
    if s == 0b110 then {
        let (ea, k) = EA(0);
        Reg8True_write(d, Rd(ea, k));
        Done(k + 3)
    } else if d == 0b110 then {
        let (ea, k) = EA(0);
        Wr(ea, Reg8True(s), k);
        Done(k + 3)
    } else {
        Reg8_write(d, Reg8(s));
        Done(0)
    }
}

function execute_ALU_r (op : bits(3), s : bits(3)) -> unit = {
    if s == 0b110 then {
        let (ea, k) = EA(0);
        ALU8(op, Rd(ea, k));
        Done(k + 3)
    } else {
        ALU8(op, Reg8(s));
        Done(0)
    }
}

function execute_ALU_n op : bits(3) -> unit = {
    ALU8(op, Imm8(0));
    Done(3)
}

function execute_RET_cc cc : bits(3) -> unit = {
    if Decide(CondValue(cc)) then {
        let t = Pop16(1);
        WZ = t;
        BranchTo(t, BranchType_RET);
        Done(7)
    } else {
        Done(1)
    }
}

function execute_POP p : bits(2) -> unit = {
    RP2_write(p, Pop16(0));
    Done(6)
}

function execute_RET () -> unit = {
    let t = Pop16(0);
    WZ = t;
    BranchTo(t, BranchType_RET);
    Done(6)
}

function execute_EXX () -> unit = {
    let bc = BC_read();
    let de = DE_read();
    let hl = HL_read();
    BC_write(BC_);
    DE_write(DE_);
    HL_write(HL_);
    BC_ = bc;
    DE_ = de;
    HL_ = hl;
    Done(0)
}

function execute_JP_HL () -> unit = {
    BranchTo(HLX_read(), BranchType_JMP);
    Done(0)
}

function execute_LD_SP_HL () -> unit = {
    SP = HLX_read();
    Done(2)
}

function execute_JP_cc cc : bits(3) -> unit = {
    let nn = Imm16(0);
    WZ = nn;
    if CondValue(cc) then {
        BranchTo(nn, BranchType_JMP)
    };
    Done(6)
}

function execute_JP () -> unit = {
    let nn = Imm16(0);
    WZ = nn;
    BranchTo(nn, BranchType_JMP);
    Done(6)
}

function execute_OUT_n_A () -> unit = {
    let n = Imm8(0);
    let a = A;
    Out(a @ n, a, 3);
    WZ = a @ (n + 0x01);
    Done(7)
}

function execute_IN_A_n () -> unit = {
    let n = Imm8(0);
    let port = A @ n;
    A = In(port, 3);
    WZ = port + 0x0001;
    Done(7)
}

function execute_EX_SP_HL () -> unit = {
    let lo = Rd(SP, 0);
    let hi = Rd(SP + 0x0001, 3);
    let v = HLX_read();
    Wr(SP + 0x0001, v[15 .. 8], 7);
    Wr(SP, v[7 .. 0], 10);
    HLX_write(hi @ lo);
    WZ = hi @ lo;
    Done(15)
}

function execute_EX_DE_HL () -> unit = {
    let de = DE_read();
    DE_write(HL_read());
    HL_write(de);
    Done(0)
}

function execute_DI () -> unit = {
    IFF1 = 0b0;
    IFF2 = 0b0;
    Done(0)
}

function execute_EI () -> unit = {
    IFF1 = 0b1;
    IFF2 = 0b1;
    EIDelay = 0b1;
    Done(0)
}

function execute_CALL_cc cc : bits(3) -> unit = {
    let nn = Imm16(0);
    WZ = nn;
    if Decide(CondValue(cc)) then {
        Push16(NextPC(), 7);
        BranchTo(nn, BranchType_CALL);
        Done(13)
    } else {
        Done(6)
    }
}

function execute_PUSH p : bits(2) -> unit = {
    Push16(RP2_read(p), 1);
    Done(7)
}

function execute_CALL () -> unit = {
    let nn = Imm16(0);
    WZ = nn;
    Push16(NextPC(), 7);
    BranchTo(nn, BranchType_CALL);
    Done(13)
}

function execute_RST y : bits(3) -> unit = {
    Push16(NextPC(), 1);
    let t = 0x00 @ 0b00 @ y @ 0b000;
    WZ = t;
    BranchTo(t, BranchType_CALL);
    Done(7)
}

/* The CB page, and DDCB and FDCB: there the operand is always (IX+d) or
   (IY+d), and a register field other than 6 also receives the result
   (BIT writes nothing). */

function execute_CB_SHIFT (op : bits(3), r : bits(3)) -> unit = {
    if XYCBPage() then {
        let v = Shift8(op, Rd(XYAddr, 0));
        Wr(XYAddr, v, 4);
        if r != 0b110 then {
            Reg8True_write(r, v)
        };
        Done(7)
    } else if r == 0b110 then {
        let v = Shift8(op, Rd(H @ L, 0));
        Wr(H @ L, v, 4);
        Done(7)
    } else {
        Reg8True_write(r, Shift8(op, Reg8True(r)));
        Done(0)
    }
}

function execute_CB_BIT (b : bits(3), r : bits(3)) -> unit = {
    if XYCBPage() then {
        BitTest(b, Rd(XYAddr, 0), WZ[15 .. 8]);
        Done(4)
    } else if r == 0b110 then {
        BitTest(b, Rd(H @ L, 0), WZ[15 .. 8]);
        Done(4)
    } else {
        let x = Reg8True(r);
        BitTest(b, x, x);
        Done(0)
    }
}

val CB_RESSET : (bits(1), bits(3), bits(3)) -> unit

function CB_RESSET (s, b, r) = {
    if XYCBPage() then {
        let x = Rd(XYAddr, 0);
        let v = [x with unsigned(b) = s[0]];
        Wr(XYAddr, v, 4);
        if r != 0b110 then {
            Reg8True_write(r, v)
        };
        Done(7)
    } else if r == 0b110 then {
        let x = Rd(H @ L, 0);
        Wr(H @ L, [x with unsigned(b) = s[0]], 4);
        Done(7)
    } else {
        let x = Reg8True(r);
        Reg8True_write(r, [x with unsigned(b) = s[0]]);
        Done(0)
    }
}

function execute_CB_RES (b : bits(3), r : bits(3)) -> unit = CB_RESSET(0b0, b, r)

function execute_CB_SET (b : bits(3), r : bits(3)) -> unit = CB_RESSET(0b1, b, r)

/* The ED page. */

/* IN r,(C) and IN (C) (r = 6: flags only). */

function execute_IN_r_C r : bits(3) -> unit = {
    let bc = BC_read();
    let v = In(bc, 0);
    if r != 0b110 then {
        Reg8True_write(r, v)
    };
    Q = 0b1;
    FN = 0b0;
    FP = Parity8(v);
    FH = 0b0;
    SetSZXY(v);
    WZ = bc + 0x0001;
    Done(4)
}

/* OUT (C),r and OUT (C),0 (r = 6, NMOS). */

function execute_OUT_C_r r : bits(3) -> unit = {
    let bc = BC_read();
    Out(bc, if r == 0b110 then 0x00 else Reg8True(r), 0);
    WZ = bc + 0x0001;
    Done(4)
}

function execute_SBC_HL_rr p : bits(2) -> unit = {
    let x = H @ L;
    let y = RP_read(p);
    WZ = x + 0x0001;
    let lo = Sub8(x[7 .. 0], y[7 .. 0], FC);
    let hi = Sub8(x[15 .. 8], y[15 .. 8], FC);
    HL_write(hi @ lo);
    Q = 0b1;
    FZ = Bit((hi @ lo) == 0x0000);
    Done(7)
}

function execute_ADC_HL_rr p : bits(2) -> unit = {
    let x = H @ L;
    let y = RP_read(p);
    WZ = x + 0x0001;
    let lo = Add8(x[7 .. 0], y[7 .. 0], FC);
    let hi = Add8(x[15 .. 8], y[15 .. 8], FC);
    HL_write(hi @ lo);
    Q = 0b1;
    FZ = Bit((hi @ lo) == 0x0000);
    Done(7)
}

function execute_LD_nn_rr p : bits(2) -> unit = {
    let nn = Imm16(0);
    let v = RP_read(p);
    Wr(nn, v[7 .. 0], 6);
    Wr(nn + 0x0001, v[15 .. 8], 9);
    WZ = nn + 0x0001;
    Done(12)
}

function execute_LD_rr_nn_ind p : bits(2) -> unit = {
    let nn = Imm16(0);
    let lo = Rd(nn, 6);
    let hi = Rd(nn + 0x0001, 9);
    RP_write(p, hi @ lo);
    WZ = nn + 0x0001;
    Done(12)
}

function execute_NEG () -> unit = {
    Q = 0b1;
    A = Sub8(0x00, A, 0b0);
    Done(0)
}

/* RETN and RETI: both copy IFF2 to IFF1. */

function execute_RETN () -> unit = {
    let t = Pop16(0);
    WZ = t;
    IFF1 = IFF2;
    BranchTo(t, BranchType_RET);
    Done(6)
}

function execute_IM y : bits(3) -> unit = {
    IM = match y[1 .. 0] {
        0b10 => 0b01,
        0b11 => 0b10,
        _ => 0b00
    };
    Done(0)
}

function execute_LD_I_A () -> unit = {
    I = A;
    Done(1)
}

function execute_LD_R_A () -> unit = {
    R = A;
    Done(1)
}

val LD_A_IR : bits(8) -> unit

function LD_A_IR (v) = {
    A = v;
    Q = 0b1;
    FN = 0b0;
    FH = 0b0;
    FP = IFF2;
    SetSZXY(v);
    LDAIR = 0b1;
    Done(1)
}

function execute_LD_A_I () -> unit = LD_A_IR(I)

function execute_LD_A_R () -> unit = LD_A_IR(R)

function execute_RRD () -> unit = {
    let hl = H @ L;
    let m = Rd(hl, 0);
    let a = A;
    Wr(hl, a[3 .. 0] @ m[7 .. 4], 7);
    let r = a[7 .. 4] @ m[3 .. 0];
    A = r;
    WZ = hl + 0x0001;
    Q = 0b1;
    FN = 0b0;
    FH = 0b0;
    FP = Parity8(r);
    SetSZXY(r);
    Done(10)
}

function execute_RLD () -> unit = {
    let hl = H @ L;
    let m = Rd(hl, 0);
    let a = A;
    Wr(hl, m[3 .. 0] @ a[3 .. 0], 7);
    let r = a[7 .. 4] @ m[7 .. 4];
    A = r;
    WZ = hl + 0x0001;
    Q = 0b1;
    FN = 0b0;
    FH = 0b0;
    FP = Parity8(r);
    SetSZXY(r);
    Done(10)
}

function execute_ED_NOP () -> unit = Done(0)

/* The block instructions. `dn` is 1 for the decrementing forms, `rep` for
   the repeating ones; a repeat goes back to the instruction itself, takes
   five more T-states, sets WZ to its address plus one, and takes X and Y
   from bits 11 and 13 of that address. */

val Step16 : (bits(16), bits(1)) -> bits(16)

function Step16 (x, dn) = if dn == 0b1 then x - 0x0001 else x + 0x0001

val Repeat : unit -> unit

function Repeat () = {
    WZ = PC + 0x0001;
    FY = [PC[13]];
    FX = [PC[11]];
    BranchTo(PC, BranchType_JMP);
    Done(13)
}

function execute_LDI (dn : bits(1), rep : bits(1)) -> unit = {
    let hl = H @ L;
    let de = DE_read();
    let v = Rd(hl, 0);
    Wr(de, v, 3);
    HL_write(Step16(hl, dn));
    DE_write(Step16(de, dn));
    let bc = BC_read() - 0x0001;
    BC_write(bc);
    let n = A + v;
    Q = 0b1;
    FN = 0b0;
    FH = 0b0;
    FP = Bit(bc != 0x0000);
    FY = [n[1]];
    FX = [n[3]];
    if rep == 0b1 & bc != 0x0000 then {
        Repeat()
    } else {
        Done(8)
    }
}

function execute_CPI (dn : bits(1), rep : bits(1)) -> unit = {
    let hl = H @ L;
    let v = Rd(hl, 0);
    HL_write(Step16(hl, dn));
    WZ = Step16(WZ, dn);
    let bc = BC_read() - 0x0001;
    BC_write(bc);
    let a = A;
    let n = a - v;
    let h = [xor_vec(xor_vec(a, v), n)[4]];
    let m = n - ZeroExtend(h, 8);
    Q = 0b1;
    FN = 0b1;
    FP = Bit(bc != 0x0000);
    FH = h;
    FY = [m[1]];
    FX = [m[3]];
    FZ = IsZero8(n);
    FS = [n[7]];
    if rep == 0b1 & bc != 0x0000 & n != 0x00 then {
        Repeat()
    } else {
        Done(8)
    }
}

/* The flags after INI, IND, OUTI and OUTD: `k` is the byte sum that gives
   C and H, `b` is B after the decrement. A repeat changes P/V and H
   further (David Banks' findings, as in ares). */

val BlockIOFlags : (bits(8), bits(9), bits(8), bits(1)) -> unit

function BlockIOFlags (v, k, b, rep) = {
    let c = [k[8]];
    let p = Parity8(xor_vec(k[7 .. 0] & 0x07, b));
    Q = 0b1;
    FC = c;
    FN = [v[7]];
    FH = c;
    SetSZXY(b);
    if rep == 0b1 & b != 0x00 then {
        if c == 0b1 then {
            if v[7] == bitone then {
                FP = not_vec(xor_vec(p, Parity8((b - 0x01) & 0x07)));
                FH = Bit(b[3 .. 0] == 0x0)
            } else {
                FP = not_vec(xor_vec(p, Parity8((b + 0x01) & 0x07)));
                FH = Bit(b[3 .. 0] == 0xF)
            }
        } else {
            FP = not_vec(xor_vec(p, Parity8(b & 0x07)))
        };
        Repeat()
    } else {
        FP = p;
        Done(8)
    }
}

function execute_INI (dn : bits(1), rep : bits(1)) -> unit = {
    let bc = BC_read();
    let hl = H @ L;
    WZ = Step16(bc, dn);
    let v = In(bc, 1);
    let b = B - 0x01;
    B = b;
    Wr(hl, v, 5);
    HL_write(Step16(hl, dn));
    let c1 = if dn == 0b1 then C - 0x01 else C + 0x01;
    let k : bits(9) = ZeroExtend(c1, 9) + ZeroExtend(v, 9);
    BlockIOFlags(v, k, b, rep)
}

function execute_OUTI (dn : bits(1), rep : bits(1)) -> unit = {
    let hl = H @ L;
    let v = Rd(hl, 1);
    let b = B - 0x01;
    B = b;
    let bc = b @ C;
    Out(bc, v, 4);
    let hl2 = Step16(hl, dn);
    HL_write(hl2);
    WZ = Step16(bc, dn);
    let k : bits(9) = ZeroExtend(hl2[7 .. 0], 9) + ZeroExtend(v, 9);
    BlockIOFlags(v, k, b, rep)
}

/* Decode. */

val __DecodeZ80 : bits(8) -> unit

scattered function __DecodeZ80

val __DecodeZ80_CB : bits(8) -> unit

scattered function __DecodeZ80_CB

val __DecodeZ80_ED : bits(8) -> unit

scattered function __DecodeZ80_ED

function clause __DecodeZ80 (0x00) = execute_NOP()

function clause __DecodeZ80 (0x08) = execute_EX_AF()

function clause __DecodeZ80 (0x10) = execute_DJNZ()

function clause __DecodeZ80 (0x18) = execute_JR()

function clause __DecodeZ80 (0b001 @ cc : bits(2) @ 0b000) = execute_JR_cc(cc)

function clause __DecodeZ80 (0b00 @ p : bits(2) @ 0b0001) = execute_LD_rr_nn(p)

function clause __DecodeZ80 (0b00 @ p : bits(2) @ 0b1001) = execute_ADD_HL_rr(p)

function clause __DecodeZ80 (0b000 @ p : bits(1) @ 0b0010) = execute_LD_ind_A(p)

function clause __DecodeZ80 (0b000 @ p : bits(1) @ 0b1010) = execute_LD_A_ind(p)

function clause __DecodeZ80 (0x22) = execute_LD_nn_HL()

function clause __DecodeZ80 (0x2A) = execute_LD_HL_nn()

function clause __DecodeZ80 (0x32) = execute_LD_nn_A()

function clause __DecodeZ80 (0x3A) = execute_LD_A_nn()

function clause __DecodeZ80 (0b00 @ p : bits(2) @ 0b0011) = execute_INC_rr(p)

function clause __DecodeZ80 (0b00 @ p : bits(2) @ 0b1011) = execute_DEC_rr(p)

function clause __DecodeZ80 (0b00 @ r : bits(3) @ 0b100) = execute_INC_r(r)

function clause __DecodeZ80 (0b00 @ r : bits(3) @ 0b101) = execute_DEC_r(r)

function clause __DecodeZ80 (0b00 @ r : bits(3) @ 0b110) = execute_LD_r_n(r)

function clause __DecodeZ80 (0b000 @ op : bits(2) @ 0b111) = execute_ROT_A(op)

function clause __DecodeZ80 (0x27) = execute_DAA()

function clause __DecodeZ80 (0x2F) = execute_CPL()

function clause __DecodeZ80 (0x37) = execute_SCF()

function clause __DecodeZ80 (0x3F) = execute_CCF()

function clause __DecodeZ80 (0x76) = execute_HALT()

function clause __DecodeZ80 (0b01 @ d : bits(3) @ s : bits(3)) = execute_LD_r_r(d, s)

function clause __DecodeZ80 (0b10 @ op : bits(3) @ s : bits(3)) = execute_ALU_r(op, s)

function clause __DecodeZ80 (0b11 @ cc : bits(3) @ 0b000) = execute_RET_cc(cc)

function clause __DecodeZ80 (0b11 @ p : bits(2) @ 0b0001) = execute_POP(p)

function clause __DecodeZ80 (0xC9) = execute_RET()

function clause __DecodeZ80 (0xD9) = execute_EXX()

function clause __DecodeZ80 (0xE9) = execute_JP_HL()

function clause __DecodeZ80 (0xF9) = execute_LD_SP_HL()

function clause __DecodeZ80 (0b11 @ cc : bits(3) @ 0b010) = execute_JP_cc(cc)

function clause __DecodeZ80 (0xC3) = execute_JP()

function clause __DecodeZ80 (0xD3) = execute_OUT_n_A()

function clause __DecodeZ80 (0xDB) = execute_IN_A_n()

function clause __DecodeZ80 (0xE3) = execute_EX_SP_HL()

function clause __DecodeZ80 (0xEB) = execute_EX_DE_HL()

function clause __DecodeZ80 (0xF3) = execute_DI()

function clause __DecodeZ80 (0xFB) = execute_EI()

function clause __DecodeZ80 (0b11 @ cc : bits(3) @ 0b100) = execute_CALL_cc(cc)

function clause __DecodeZ80 (0b11 @ p : bits(2) @ 0b0101) = execute_PUSH(p)

function clause __DecodeZ80 (0xCD) = execute_CALL()

function clause __DecodeZ80 (0b11 @ op : bits(3) @ 0b110) = execute_ALU_n(op)

function clause __DecodeZ80 (0b11 @ y : bits(3) @ 0b111) = execute_RST(y)

function clause __DecodeZ80_CB (0b00 @ op : bits(3) @ r : bits(3)) = execute_CB_SHIFT(op, r)

function clause __DecodeZ80_CB (0b01 @ b : bits(3) @ r : bits(3)) = execute_CB_BIT(b, r)

function clause __DecodeZ80_CB (0b10 @ b : bits(3) @ r : bits(3)) = execute_CB_RES(b, r)

function clause __DecodeZ80_CB (0b11 @ b : bits(3) @ r : bits(3)) = execute_CB_SET(b, r)

function clause __DecodeZ80_ED (0b01 @ r : bits(3) @ 0b000) = execute_IN_r_C(r)

function clause __DecodeZ80_ED (0b01 @ r : bits(3) @ 0b001) = execute_OUT_C_r(r)

function clause __DecodeZ80_ED (0b01 @ p : bits(2) @ 0b0010) = execute_SBC_HL_rr(p)

function clause __DecodeZ80_ED (0b01 @ p : bits(2) @ 0b1010) = execute_ADC_HL_rr(p)

function clause __DecodeZ80_ED (0b01 @ p : bits(2) @ 0b0011) = execute_LD_nn_rr(p)

function clause __DecodeZ80_ED (0b01 @ p : bits(2) @ 0b1011) = execute_LD_rr_nn_ind(p)

function clause __DecodeZ80_ED (0b01 @ y : bits(3) @ 0b100) = execute_NEG()

function clause __DecodeZ80_ED (0b01 @ y : bits(3) @ 0b101) = execute_RETN()

function clause __DecodeZ80_ED (0b01 @ y : bits(3) @ 0b110) = execute_IM(y)

function clause __DecodeZ80_ED (0x47) = execute_LD_I_A()

function clause __DecodeZ80_ED (0x4F) = execute_LD_R_A()

function clause __DecodeZ80_ED (0x57) = execute_LD_A_I()

function clause __DecodeZ80_ED (0x5F) = execute_LD_A_R()

function clause __DecodeZ80_ED (0x67) = execute_RRD()

function clause __DecodeZ80_ED (0x6F) = execute_RLD()

function clause __DecodeZ80_ED (0b101 @ rep : bits(1) @ dn : bits(1) @ 0b000) = execute_LDI(dn, rep)

function clause __DecodeZ80_ED (0b101 @ rep : bits(1) @ dn : bits(1) @ 0b001) = execute_CPI(dn, rep)

function clause __DecodeZ80_ED (0b101 @ rep : bits(1) @ dn : bits(1) @ 0b010) = execute_INI(dn, rep)

function clause __DecodeZ80_ED (0b101 @ rep : bits(1) @ dn : bits(1) @ 0b011) = execute_OUTI(dn, rep)

/* Every other ED opcode is an 8-T-state NOP. (core/sail takes only clauses
   that fix a bit, so the catch-all is written as two.) */

function clause __DecodeZ80_ED (0b0 @ op : bits(7)) = execute_ED_NOP()

function clause __DecodeZ80_ED (0b1 @ op : bits(7)) = execute_ED_NOP()

end __DecodeZ80

end __DecodeZ80_CB

end __DecodeZ80_ED

/* Interrupt entry, after an instruction: NMI, and the maskable interrupt
   in each mode. `bus` is the byte the device puts on the data bus (0xFF on
   the Master System and the Mega Drive, which makes IM 0 a RST 38H). The
   entry is one acknowledge cycle (M1, R incremented) and the pushes. IM 0
   runs the RST on the bus with two more T-states; another bus byte is an
   instruction of the base page, left to the per-CPU layer. */

val TakeNMI : unit -> unit

function TakeNMI () = {
    IncR();
    IFF1 = 0b0;
    if LDAIR == 0b1 then {
        FP = 0b0
    };
    LDAIR = 0b0;
    EIDelay = 0b0;
    Q = 0b0;
    let hi_at = SP - 0x0001;
    let lo_at = SP - 0x0002;
    MemWrite(hi_at, PC[15 .. 8], 5);
    MemWrite(lo_at, PC[7 .. 0], 8);
    SP = lo_at;
    WZ = 0x0066;
    BranchTo(0x0066, BranchType_CALL);
    Cycles(11)
}

val TakeIRQ : (bits(2), bits(8)) -> unit

function TakeIRQ (mode, bus) = {
    IncR();
    IFF1 = 0b0;
    IFF2 = 0b0;
    if LDAIR == 0b1 then {
        FP = 0b0
    };
    LDAIR = 0b0;
    EIDelay = 0b0;
    Q = 0b0;
    let hi_at = SP - 0x0001;
    let lo_at = SP - 0x0002;
    if mode == 0b10 then {
        let v = I @ bus;
        MemWrite(hi_at, PC[15 .. 8], 7);
        MemWrite(lo_at, PC[7 .. 0], 10);
        SP = lo_at;
        let lo = MemRead(v, 13);
        let hi = MemRead(v + 0x0001, 16);
        WZ = hi @ lo;
        BranchTo(hi @ lo, BranchType_CALL);
        Cycles(19)
    } else {
        let t = 0x00 @ (bus & 0x38);
        MemWrite(hi_at, PC[15 .. 8], 7);
        MemWrite(lo_at, PC[7 .. 0], 10);
        SP = lo_at;
        WZ = t;
        BranchTo(t, BranchType_CALL);
        Cycles(13)
    }
}

/* The interrupt-entry forms the generator makes: 0 NMI, 1 IM 0 (RST from
   a bus of 0xFF), 2 IM 1, 3 IM 2 (vector from a bus of 0xFF). */

val __Z80_Accept : bits(2) -> unit

scattered function __Z80_Accept

function clause __Z80_Accept (0b00) = TakeNMI()

function clause __Z80_Accept (0b01) = TakeIRQ(0b00, 0xFF)

function clause __Z80_Accept (0b10) = TakeIRQ(0b01, 0xFF)

function clause __Z80_Accept (0b11) = TakeIRQ(0b10, 0xFF)

end __Z80_Accept

/* A DD or FD prefix followed by another prefix: the CPU fetches it as an
   opcode (one M1 cycle and an R increment) and does nothing else, and the
   next byte starts a new instruction. Begin does the fetch as on the base
   page. */

val execute_LONE_PREFIX : unit -> unit

function execute_LONE_PREFIX () = Done(0)

val __Z80_Lone : bits(8) -> unit

scattered function __Z80_Lone

function clause __Z80_Lone (0xDD) = execute_LONE_PREFIX()

function clause __Z80_Lone (0xFD) = execute_LONE_PREFIX()

end __Z80_Lone

/* One instruction: the page is chosen by the prefix bytes (DD or FD then
   CB is the DDCB or FDCB page; DD or FD then DD, FD or ED runs as a lone
   4-T-state prefix, __Z80_Lone). The generator runs Begin
   and one clause of the page's decode function in the same way. */

val Step : unit -> unit

function Step () = {
    Begin();
    match Page {
        0b000 => __DecodeZ80(MemFetch(PC, 0)),
        0b001 => __DecodeZ80_CB(MemFetch(PC + 0x0001, 4)),
        0b010 => __DecodeZ80_ED(MemFetch(PC + 0x0001, 4)),
        0b011 => __DecodeZ80(MemFetch(PC + 0x0001, 4)),
        0b100 => __DecodeZ80(MemFetch(PC + 0x0001, 4)),
        _ => __DecodeZ80_CB(MemFetch(PC + 0x0003, 11))
    }
}

/*
Notice for the parts ported from ares (component/processor/z80):

Copyright (c) 2004-2025 ares team, Near et al

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
