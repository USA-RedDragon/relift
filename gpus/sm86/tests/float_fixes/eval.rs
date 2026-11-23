//! A small SPIR-V interpreter for the translator's compute output: enough
//! of the core and GLSL.std.450 instruction sets to run a straight-line
//! guest program on the CPU, with the float behavior of a host as knobs.

use std::collections::HashMap;

/// How the simulated device does 32-bit float arithmetic.
#[derive(Clone, Copy, Debug)]
pub struct Device {
    /// Denormal inputs and results of float operations read and write as
    /// signed zero, results rounded to 24 bits first as SM86 `.FTZ` does.
    pub flush: bool,
    /// With `flush`, `Ldexp` flushes a denormal result too (an
    /// implementation by multiplication) rather than building it from bits.
    pub flush_ldexp: bool,
    /// With `flush`, comparisons read denormals as zero too.
    pub flush_cmp: bool,
    /// GLSL `Fma` is fused; otherwise a multiply and an add, each rounded
    /// (`OpFmaKHR` is always fused).
    pub fused: bool,
    /// A NaN result is `0x7fc00000` whatever the inputs; otherwise the CPU's
    /// own (a quieted input NaN, or the negative default NaN).
    pub canonical_nan: bool,
    /// A NaN result is `0x7fffffff` whatever the inputs: a host with
    /// `HostProfile::canonical_nan`.
    pub sm86_nan: bool,
}

pub const PRESERVE: Device = Device {
    flush: false,
    flush_ldexp: false,
    flush_cmp: false,
    fused: true,
    canonical_nan: false,
    sm86_nan: false,
};

pub const FLUSH: Device = Device {
    flush: true,
    flush_ldexp: false,
    flush_cmp: true,
    fused: true,
    canonical_nan: false,
    sm86_nan: false,
};

#[derive(Clone, Debug)]
enum V {
    S(u64),
    C(Vec<V>),
    P(Ptr),
}

#[derive(Clone, Debug)]
enum Ptr {
    Slot(usize, Vec<usize>),
    Mem(u64),
}

#[derive(Clone, Debug)]
enum Ty {
    Void,
    Bool,
    Int(u32),
    Float(u32),
    Vec(u32, u32),
    Array(u32, u32),
    Struct(Vec<u32>),
    Ptr(u32),
    Other,
}

struct Ins<'a> {
    op: u16,
    w: &'a [u32],
}

pub struct Run {
    pub mem: HashMap<u64, u32>,
    pub steps: u64,
}

struct Ex<'a> {
    ins: Vec<Ins<'a>>,
    types: HashMap<u32, Ty>,
    consts: HashMap<u32, V>,
    globals: HashMap<u32, V>,
    funcs: HashMap<u32, usize>,
    labels: HashMap<u32, usize>,
    slots: Vec<V>,
    glsl: u32,
    dev: Device,
    run: Run,
    result_types: HashMap<u32, u32>,
}

fn f(x: u64) -> f32 {
    f32::from_bits(x as u32)
}

/// The minimum, a non-NaN operand over a NaN, and the first operand when
/// they compare equal (so `-0` may come out of a clamp at `+0`).
fn nmin(x: f32, y: f32) -> f32 {
    if y.is_nan() || y >= x { x } else { y }
}

fn nmax(x: f32, y: f32) -> f32 {
    if y.is_nan() || y <= x { x } else { y }
}

fn fb(x: f32) -> u64 {
    u64::from(x.to_bits())
}

impl Ex<'_> {
    fn fin(&self, x: f32) -> f32 {
        if self.dev.flush && x.is_subnormal() {
            0.0f32.copysign(x)
        } else {
            x
        }
    }

    fn fout(&self, x: f32) -> u64 {
        let x = self.fin(x);
        if x.is_nan() && self.dev.sm86_nan {
            0x7fff_ffff
        } else if x.is_nan() && self.dev.canonical_nan {
            0x7fc0_0000
        } else {
            fb(x)
        }
    }

    /// With `flush`, the result `r` of an operation as a flushing device
    /// gives it: rounded to 24 bits with an unbounded exponent, then zero if
    /// below `2^-126`. `scaled` is the same operation with its result scaled
    /// by `2^64`.
    fn tiny(&self, r: f32, scaled: impl FnOnce() -> f32) -> f32 {
        if !self.dev.flush || r == 0.0 || r.is_nan() || r.abs() > f32::MIN_POSITIVE {
            return r;
        }
        let s = scaled();
        if s == 0.0 {
            r
        } else if s.abs() < f32::MIN_POSITIVE * 2f32.powi(64) {
            0.0f32.copysign(s)
        } else {
            s * 2f32.powi(-64)
        }
    }

    fn width(&self, t: u32) -> u32 {
        match self.types.get(&t) {
            Some(Ty::Int(w) | Ty::Float(w)) => *w,
            Some(Ty::Bool) => 1,
            _ => 32,
        }
    }

    fn zero_of(&self, t: u32) -> V {
        match self.types.get(&t).cloned().unwrap_or(Ty::Other) {
            Ty::Vec(e, n) | Ty::Array(e, n) => V::C((0..n).map(|_| self.zero_of(e)).collect()),
            Ty::Struct(m) => V::C(m.iter().map(|&e| self.zero_of(e)).collect()),
            _ => V::S(0),
        }
    }

    fn val(&self, locals: &HashMap<u32, V>, id: u32) -> V {
        if let Some(v) = locals.get(&id) {
            return v.clone();
        }
        if let Some(v) = self.consts.get(&id) {
            return v.clone();
        }
        if let Some(v) = self.globals.get(&id) {
            return v.clone();
        }
        panic!("no value for %{id}")
    }

    fn s(&self, locals: &HashMap<u32, V>, id: u32) -> u64 {
        match self.val(locals, id) {
            V::S(x) => x,
            v => panic!("%{id} not scalar: {v:?}"),
        }
    }

    fn load(&mut self, p: &Ptr, t: u32) -> V {
        match p {
            Ptr::Slot(s, path) => {
                let mut v = &self.slots[*s];
                for &i in path {
                    match v {
                        V::C(c) if i < c.len() => v = &c[i],
                        // a buffer the interpreter does not lay out (a runtime array, such as the empty pool
                        // table): it reads zeros
                        _ => return self.zero_of(t),
                    }
                }
                v.clone()
            }
            Ptr::Mem(a) => {
                let w = self.width(t);
                let lo = u64::from(*self.run.mem.get(a).unwrap_or(&0));
                if w == 64 {
                    let hi = u64::from(*self.run.mem.get(&(a + 4)).unwrap_or(&0));
                    V::S(lo | (hi << 32))
                } else {
                    V::S(lo)
                }
            }
        }
    }

    fn store(&mut self, p: &Ptr, v: V) {
        match p {
            Ptr::Slot(s, path) => {
                let mut t = &mut self.slots[*s];
                for &i in path {
                    match t {
                        V::C(c) => t = &mut c[i],
                        _ => panic!("bad access path"),
                    }
                }
                *t = v;
            }
            Ptr::Mem(a) => match v {
                V::S(x) => {
                    self.run.mem.insert(*a, x as u32);
                }
                V::C(c) => {
                    for (k, e) in c.into_iter().enumerate() {
                        self.store(&Ptr::Mem(a + 4 * k as u64), e);
                    }
                }
                V::P(_) => panic!("pointer store"),
            },
        }
    }

    fn ext(&self, t: u32, k: u32, a: &[u64]) -> u64 {
        let w = self.width(t);
        let m = if w == 64 { u64::MAX } else { (1u64 << w) - 1 };
        let si = |x: u64| x as u32 as i32;
        let fi = |x: u64| self.fin(f(x));
        match k {
            2 => self.fout(fi(a[0]).round_ties_even()),
            3 => self.fout(fi(a[0]).trunc()),
            4 => fb(f(a[0]).abs()),
            8 => self.fout(fi(a[0]).floor()),
            9 => self.fout(fi(a[0]).ceil()),
            21 => self.fout(fi(a[0]).tanh()),
            29 => self.fout(fi(a[0]).exp2()),
            30 => self.fout(fi(a[0]).log2()),
            31 => self.fout(fi(a[0]).sqrt()),
            32 => self.fout(1.0 / fi(a[0]).sqrt()),
            37 | 79 => self.fout(nmin(fi(a[0]), fi(a[1]))),
            40 | 80 => self.fout(nmax(fi(a[0]), fi(a[1]))),
            43 | 81 => self.fout(nmin(nmax(fi(a[0]), fi(a[1])), fi(a[2]))),
            38 => (a[0] as u32).min(a[1] as u32).into(),
            41 => (a[0] as u32).max(a[1] as u32).into(),
            44 => (a[0] as u32).max(a[1] as u32).min(a[2] as u32).into(),
            39 => u64::from(si(a[0]).min(si(a[1])) as u32),
            42 => u64::from(si(a[0]).max(si(a[1])) as u32),
            45 => u64::from(si(a[0]).max(si(a[1])).min(si(a[2])) as u32),
            50 => {
                let (x, y, z) = (fi(a[0]), fi(a[1]), fi(a[2]));
                let k = 2f32.powi(64);
                if self.dev.fused {
                    self.fout(self.tiny(x.mul_add(y, z), || (x * k).mul_add(y, z * k)))
                } else {
                    let p = self.tiny(x * y, || (x * k) * y);
                    let p = self.fin(p);
                    self.fout(self.tiny(p + z, || p * k + z * k))
                }
            }
            53 => {
                let x = f64::from(fi(a[0]));
                let e = si(a[1]).clamp(-400, 400);
                let r = (x * 2f64.powi(e)) as f32;
                if self.dev.flush_ldexp {
                    self.fout(r)
                } else {
                    fb(r)
                }
            }
            73 => u64::from(if a[0] as u32 == 0 {
                u32::MAX
            } else {
                (a[0] as u32).trailing_zeros()
            }),
            74 => {
                let x = a[0] as u32;
                let x = if (x as i32) < 0 { !x } else { x };
                u64::from(if x == 0 {
                    u32::MAX
                } else {
                    31 - x.leading_zeros()
                })
            }
            75 => u64::from(if a[0] as u32 == 0 {
                u32::MAX
            } else {
                31 - (a[0] as u32).leading_zeros()
            }),
            _ => panic!("GLSL.std.450 {k}"),
        }
        .min(m)
    }

    fn call(&mut self, fid: u32, args: Vec<V>) -> Option<V> {
        let mut pc = self.funcs[&fid] + 1;
        let mut locals: HashMap<u32, V> = HashMap::new();
        let mut args = args.into_iter();
        let mut prev = 0;
        let mut cur = 0;
        loop {
            self.run.steps += 1;
            assert!(self.run.steps < 50_000_000, "runaway");
            let i = &self.ins[pc];
            let w = i.w;
            let op = i.op;
            pc += 1;
            let rt = |k: usize| w[k];
            match op {
                55 => {
                    locals.insert(w[1], args.next().expect("argument"));
                }
                248 => {
                    prev = cur;
                    cur = w[0];
                }
                249 => {
                    pc = self.labels[&w[0]];
                }
                250 => {
                    let c = self.s(&locals, w[0]);
                    pc = self.labels[&if c != 0 { w[1] } else { w[2] }];
                }
                251 => {
                    let v = self.s(&locals, w[0]);
                    let mut t = w[1];
                    for p in w[2..].chunks(2) {
                        if u64::from(p[0]) == v {
                            t = p[1];
                        }
                    }
                    pc = self.labels[&t];
                }
                245 => {
                    let mut v = None;
                    for p in w[2..].chunks(2) {
                        if p[1] == prev {
                            v = Some(self.val(&locals, p[0]));
                        }
                    }
                    locals.insert(w[1], v.expect("phi"));
                }
                246 | 247 | 8 | 1 => {}
                253 => return None,
                254 => return Some(self.val(&locals, w[0])),
                56 => return None,
                57 => {
                    let a: Vec<V> = w[3..].iter().map(|&x| self.val(&locals, x)).collect();
                    let r = self.call(w[2], a);
                    if let Some(r) = r {
                        locals.insert(w[1], r);
                    }
                }
                59 => {
                    let Ty::Ptr(pt) = self.types[&w[0]] else {
                        panic!("variable type")
                    };
                    let init = match w.get(3) {
                        Some(&x) => self.val(&locals, x),
                        None => self.zero_of(pt),
                    };
                    self.slots.push(init);
                    locals.insert(w[1], V::P(Ptr::Slot(self.slots.len() - 1, vec![])));
                }
                61 => {
                    let V::P(p) = self.val(&locals, w[2]) else {
                        panic!("load from non-pointer")
                    };
                    let v = self.load(&p, w[0]);
                    locals.insert(w[1], v);
                }
                62 => {
                    let V::P(p) = self.val(&locals, w[0]) else {
                        panic!("store to non-pointer")
                    };
                    let v = self.val(&locals, w[1]);
                    self.store(&p, v);
                }
                65 | 66 => {
                    let V::P(p) = self.val(&locals, w[2]) else {
                        panic!("access chain base")
                    };
                    let idx: Vec<usize> = w[3..]
                        .iter()
                        .map(|&x| self.s(&locals, x) as usize)
                        .collect();
                    let p = match p {
                        Ptr::Slot(s, mut path) => {
                            path.extend(idx);
                            Ptr::Slot(s, path)
                        }
                        Ptr::Mem(_) => panic!("access chain on memory"),
                    };
                    locals.insert(w[1], V::P(p));
                }
                120 => {
                    let a = self.s(&locals, w[2]);
                    locals.insert(w[1], V::P(Ptr::Mem(a)));
                }
                80 => {
                    let c: Vec<V> = w[2..].iter().map(|&x| self.val(&locals, x)).collect();
                    locals.insert(w[1], V::C(c));
                }
                81 => {
                    let mut v = self.val(&locals, w[2]);
                    for &k in &w[3..] {
                        let V::C(c) = v else { panic!("extract") };
                        v = c[k as usize].clone();
                    }
                    locals.insert(w[1], v);
                }
                82 => {
                    let mut c = self.val(&locals, w[3]);
                    let e = self.val(&locals, w[2]);
                    let mut t = &mut c;
                    for &k in &w[4..] {
                        let V::C(cc) = t else { panic!("insert") };
                        t = &mut cc[k as usize];
                    }
                    *t = e;
                    locals.insert(w[1], c);
                }
                169 => {
                    let c = self.val(&locals, w[2]);
                    let a = self.val(&locals, w[3]);
                    let b = self.val(&locals, w[4]);
                    let r = match (c, a, b) {
                        (V::S(c), a, b) => {
                            if c != 0 {
                                a
                            } else {
                                b
                            }
                        }
                        (V::C(c), V::C(a), V::C(b)) => V::C(
                            c.iter()
                                .zip(a.iter().zip(b.iter()))
                                .map(|(c, (a, b))| match c {
                                    V::S(0) => b.clone(),
                                    _ => a.clone(),
                                })
                                .collect(),
                        ),
                        _ => panic!("select"),
                    };
                    locals.insert(w[1], r);
                }
                124 => {
                    let v = self.val(&locals, w[2]);
                    let r = match (&self.types[&w[0]], v) {
                        (Ty::Ptr(_), V::C(c)) => match (&c[0], &c[1]) {
                            (V::S(a), V::S(b)) => V::P(Ptr::Mem(a | (b << 32))),
                            _ => panic!("bitcast to a pointer"),
                        },
                        (Ty::Vec(_, 2), V::S(x)) => {
                            V::C(vec![V::S(x & 0xffff_ffff), V::S(x >> 32)])
                        }
                        (_, V::C(c)) => match (&c[0], &c[1]) {
                            (V::S(a), V::S(b)) => V::S(a | (b << 32)),
                            _ => panic!("bitcast"),
                        },
                        (_, v) => v,
                    };
                    locals.insert(w[1], r);
                }
                12 => {
                    assert_eq!(w[2], self.glsl, "extended instruction set");
                    let a: Vec<u64> = w[4..].iter().map(|&x| self.s(&locals, x)).collect();
                    let r = self.ext(w[0], w[3], &a);
                    locals.insert(w[1], V::S(r));
                }
                _ => {
                    let r = self.scalar(op, rt(0), &w[2..], &locals);
                    locals.insert(w[1], r);
                }
            }
        }
    }

    fn scalar(&self, op: u16, t: u32, args: &[u32], locals: &HashMap<u32, V>) -> V {
        let w = self.width(t);
        let m = if w >= 64 { u64::MAX } else { (1u64 << w) - 1 };
        let a: Vec<u64> = args.iter().map(|&x| self.s(locals, x)).collect();
        let sx = |x: u64, bits: u32| -> i64 {
            if bits >= 64 {
                x as i64
            } else {
                ((x << (64 - bits)) as i64) >> (64 - bits)
            }
        };
        let aw = args
            .first()
            .map_or(32, |&x| self.width(self.type_of(x, locals)));
        let fi = |x: u64| self.fin(f(x));
        let b = |c: bool| V::S(u64::from(c));
        let r = match op {
            109 => {
                let x = fi(a[0]);
                if w == 64 {
                    x as u64
                } else {
                    u64::from(x as u32)
                }
            }
            110 => {
                let x = fi(a[0]);
                if w == 64 {
                    x as i64 as u64
                } else {
                    u64::from(x as i32 as u32)
                }
            }
            111 => self.fout(sx(a[0], aw) as f32),
            112 => self.fout(a[0] as f32),
            113 => a[0] & m,
            114 => (sx(a[0], aw) as u64) & m,
            127 => fb(-f(a[0])),
            126 => (a[0].wrapping_neg()) & m,
            128 => a[0].wrapping_add(a[1]) & m,
            130 => a[0].wrapping_sub(a[1]) & m,
            132 => a[0].wrapping_mul(a[1]) & m,
            134 => a[0].checked_div(a[1]).unwrap_or(m),
            137 => a[0].checked_rem(a[1]).unwrap_or(0),
            4427 => {
                let (x, y, z) = (fi(a[0]), fi(a[1]), fi(a[2]));
                let k = 2f32.powi(64);
                self.fout(self.tiny(x.mul_add(y, z), || (x * k).mul_add(y, z * k)))
            }
            129 => {
                let (x, y) = (fi(a[0]), fi(a[1]));
                let k = 2f32.powi(64);
                self.fout(self.tiny(x + y, || x * k + y * k))
            }
            131 => {
                let (x, y) = (fi(a[0]), fi(a[1]));
                let k = 2f32.powi(64);
                self.fout(self.tiny(x - y, || x * k - y * k))
            }
            133 => {
                let (x, y) = (fi(a[0]), fi(a[1]));
                let k = 2f32.powi(64);
                self.fout(self.tiny(x * y, || (x * k) * y))
            }
            136 => self.fout(fi(a[0]) / fi(a[1])),
            149 | 150 => {
                let (x, y) = (a[0] as u32, a[1] as u32);
                let (s, c) = if op == 149 {
                    x.overflowing_add(y)
                } else {
                    x.overflowing_sub(y)
                };
                return V::C(vec![V::S(s.into()), V::S(u64::from(c))]);
            }
            151 | 152 => {
                let p = if op == 151 {
                    u64::from(a[0] as u32) * u64::from(a[1] as u32)
                } else {
                    (i64::from(a[0] as u32 as i32) * i64::from(a[1] as u32 as i32)) as u64
                };
                return V::C(vec![V::S(p & 0xffff_ffff), V::S(p >> 32)]);
            }
            156 => return b(f(a[0]).is_nan()),
            157 => return b(f(a[0]).is_infinite()),
            164 => return b(a[0] == a[1]),
            165 => return b(a[0] != a[1]),
            166 => return b(a[0] != 0 || a[1] != 0),
            167 => return b(a[0] != 0 && a[1] != 0),
            168 => return b(a[0] == 0),
            170 => return b(a[0] == a[1]),
            171 => return b(a[0] != a[1]),
            172 => return b(a[0] > a[1]),
            173 => return b(sx(a[0], aw) > sx(a[1], aw)),
            174 => return b(a[0] >= a[1]),
            175 => return b(sx(a[0], aw) >= sx(a[1], aw)),
            176 => return b(a[0] < a[1]),
            177 => return b(sx(a[0], aw) < sx(a[1], aw)),
            178 => return b(a[0] <= a[1]),
            179 => return b(sx(a[0], aw) <= sx(a[1], aw)),
            180..=191 => {
                let (x, y) = if self.dev.flush_cmp {
                    (fi(a[0]), fi(a[1]))
                } else {
                    (f(a[0]), f(a[1]))
                };
                let un = x.is_nan() || y.is_nan();
                let c = match (op - 180) / 2 {
                    0 => x == y,
                    1 => x != y,
                    2 => x < y,
                    3 => x > y,
                    4 => x <= y,
                    _ => x >= y,
                };
                return b(if (op - 180).is_multiple_of(2) {
                    !un && c
                } else {
                    un || c
                });
            }
            194 => (a[0] >> (a[1] & u64::from(w - 1))) & m,
            195 => (sx(a[0], w) >> (a[1] & u64::from(w - 1))) as u64 & m,
            196 => (a[0] << (a[1] & u64::from(w - 1))) & m,
            197 => a[0] | a[1],
            198 => a[0] ^ a[1],
            199 => a[0] & a[1],
            200 => !a[0] & m,
            201 => {
                let (o, c) = (a[2] as u32, a[3] as u32);
                if c == 0 {
                    a[0]
                } else {
                    let mk = ((u64::MAX >> (64 - c)) << o) & m;
                    (a[0] & !mk) | ((a[1] << o) & mk)
                }
            }
            202 | 203 => {
                let (o, c) = (a[1] as u32, a[2] as u32);
                if c == 0 {
                    0
                } else {
                    let x = (a[0] >> o) & (u64::MAX >> (64 - c));
                    if op == 202 { (sx(x, c) as u64) & m } else { x }
                }
            }
            204 => u64::from((a[0] as u32).reverse_bits()),
            205 => u64::from(a[0].count_ones()),
            _ => panic!("opcode {op}"),
        };
        V::S(r)
    }

    fn type_of(&self, id: u32, _locals: &HashMap<u32, V>) -> u32 {
        *self.result_types.get(&id).unwrap_or(&0)
    }
}

/// Runs the module's entry point once and returns the memory it wrote.
pub fn run(spirv: &[u32], dev: Device) -> Run {
    let mut ins = Vec::new();
    let mut k = 5;
    while k < spirv.len() {
        let n = (spirv[k] >> 16) as usize;
        ins.push(Ins {
            op: spirv[k] as u16,
            w: &spirv[k + 1..k + n],
        });
        k += n.max(1);
    }
    let mut ex = Ex {
        ins: Vec::new(),
        types: HashMap::new(),
        consts: HashMap::new(),
        globals: HashMap::new(),
        funcs: HashMap::new(),
        labels: HashMap::new(),
        slots: Vec::new(),
        glsl: 0,
        dev,
        run: Run {
            mem: HashMap::new(),
            steps: 0,
        },
        result_types: HashMap::new(),
    };
    let mut entry = 0;
    let mut in_fn = false;
    for (pc, i) in ins.iter().enumerate() {
        let w = i.w;
        match i.op {
            11 => ex.glsl = w[0],
            15 => entry = w[1],
            19 => {
                ex.types.insert(w[0], Ty::Void);
            }
            20 => {
                ex.types.insert(w[0], Ty::Bool);
            }
            21 => {
                ex.types.insert(w[0], Ty::Int(w[1]));
            }
            22 => {
                ex.types.insert(w[0], Ty::Float(w[1]));
            }
            23 => {
                ex.types.insert(w[0], Ty::Vec(w[1], w[2]));
            }
            28 => {
                let n = ex.consts.get(&w[2]).map_or(0, |v| match v {
                    V::S(x) => *x as u32,
                    _ => 0,
                });
                ex.types.insert(w[0], Ty::Array(w[1], n));
            }
            30 => {
                ex.types.insert(w[0], Ty::Struct(w[1..].to_vec()));
            }
            32 => {
                ex.types.insert(w[0], Ty::Ptr(w[2]));
            }
            33 => {
                ex.types.insert(w[0], Ty::Other);
            }
            41 | 48 => {
                ex.consts.insert(w[1], V::S(1));
            }
            42 | 49 => {
                ex.consts.insert(w[1], V::S(0));
            }
            43 | 50 => {
                let v = u64::from(w[2]) | w.get(3).map_or(0, |&h| u64::from(h) << 32);
                ex.consts.insert(w[1], V::S(v));
            }
            44 | 51 => {
                let c = w[2..].iter().map(|x| ex.consts[x].clone()).collect();
                ex.consts.insert(w[1], V::C(c));
            }
            46 | 1 => {
                let z = ex.zero_of(w[0]);
                ex.consts.insert(w[1], z);
            }
            59 if !in_fn => {
                let Ty::Ptr(pt) = ex.types[&w[0]] else {
                    panic!("variable type")
                };
                let init = match w.get(3) {
                    Some(x) => ex.consts[x].clone(),
                    None => ex.zero_of(pt),
                };
                ex.slots.push(init);
                ex.globals
                    .insert(w[1], V::P(Ptr::Slot(ex.slots.len() - 1, vec![])));
            }
            54 => {
                in_fn = true;
                ex.funcs.insert(w[1], pc);
            }
            56 => in_fn = false,
            248 => {
                ex.labels.insert(w[0], pc);
            }
            _ => {}
        }
        if (in_fn && has_result(i.op) || (41..=51).contains(&i.op)) && w.len() >= 2 {
            ex.result_types.insert(w[1], w[0]);
        }
    }
    ex.ins = ins;
    ex.call(entry, Vec::new());
    ex.run
}

fn has_result(op: u16) -> bool {
    !matches!(
        op,
        62 | 248 | 249 | 250 | 251 | 246 | 247 | 253 | 254 | 56 | 8 | 1
    )
}
