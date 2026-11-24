//! `<api>.c`: the OpenGL or EGL forwarding of an i386 guest, from the
//! commands [`crate::glreg`] read.
//!
//! The thunks run on the i386 core of the runtime (`tg.h`). The C types
//! are written from each parameter's classification, so no host GL or EGL
//! header is needed: integers and floats of the same size on both sides
//! pass as they are, pointer-sized integers widen, opaque pointers are
//! tokens, and data pointers point into the guest window in place. A GL
//! parameter that holds a buffer offset while a buffer is bound goes
//! through `tg_gl_offset` (gl_hand.c), which tells the two apart.

use std::fmt::Write;

use crate::Error;
use crate::config::Config;
use crate::glreg::{Api, Cmd, Param, Ty};

/// The host C type of a classification.
fn c_type(t: Ty) -> Result<&'static str, Error> {
    Ok(match t {
        Ty::Int(1, true) => "int8_t",
        Ty::Int(1, false) => "uint8_t",
        Ty::Int(2, true) => "int16_t",
        Ty::Int(2, false) => "uint16_t",
        Ty::Int(4, true) => "int32_t",
        Ty::Int(4, false) => "uint32_t",
        Ty::Int(8, true) => "int64_t",
        Ty::Int(8, false) => "uint64_t",
        Ty::Float(4) => "float",
        Ty::Float(8) => "double",
        Ty::Word(true) => "intptr_t",
        Ty::Word(false) => "uintptr_t",
        Ty::Handle | Ty::FuncPtr => "void *",
        Ty::Void => "void",
        _ => return Err(Error(format!("no C type for {t:?}"))),
    })
}

fn host_param_type(api: &Api, p: &Param) -> Result<String, Error> {
    if p.ptr > 0 {
        return Ok(if p.pointee_const && p.ptr == 1 {
            "const void *".into()
        } else {
            "void *".into()
        });
    }
    Ok(c_type(api.ty(&p.ty))?.to_string())
}

struct Ctx<'a> {
    api: &'a Api,
    cfg: &'a Config,
    cmd: &'a Cmd,
    offs: Vec<u32>,
}

impl Ctx<'_> {
    fn key(&self, p: &Param) -> String {
        format!("{}.{}", self.cmd.name, p.name)
    }

    fn binding(&self, p: &Param) -> Result<Option<String>, Error> {
        let key = self.key(p);
        for (b, list) in &self.cfg.offsets {
            if list.contains(&key) {
                let v =
                    self.api.enums.get(b).ok_or_else(|| {
                        Error(format!("offset binding {b} is not in the registry"))
                    })?;
                return Ok(Some(v.clone()));
            }
        }
        Ok(None)
    }

    /// The element count of a pointer parameter, read from the guest argument it names.
    fn count(&self, p: &Param) -> Result<Option<String>, Error> {
        let len = self
            .cfg
            .lens
            .get(&self.key(p))
            .cloned()
            .or_else(|| p.len.clone());
        let Some(len) = len else { return Ok(None) };
        let j = self
            .cmd
            .params
            .iter()
            .position(|q| q.name == len && q.ptr == 0)
            .ok_or_else(|| {
                Error(format!(
                    "{}: count {len} of {} is not a parameter",
                    self.cmd.name, p.name
                ))
            })?;
        Ok(Some(format!(
            "(uint64_t)tg_count(tg_arg32(k, {}))",
            self.offs[j]
        )))
    }
}

/// Emits one thunk.
fn emit_thunk(
    api: &Api,
    cfg: &Config,
    cmd: &Cmd,
    idx: usize,
    pfx: &str,
    o: &mut String,
) -> Result<(), Error> {
    let mut offs = Vec::new();
    let mut off = 0u32;
    for p in &cmd.params {
        offs.push(off);
        off += if p.ptr == 0 && api.ty(&p.ty).i386_size() == 8 {
            8
        } else {
            4
        };
    }
    let cx = Ctx {
        api,
        cfg,
        cmd,
        offs,
    };
    let name = &cmd.name;
    let fail = |what: &str| Error(format!("{name}: {what}; list it in hand_commands"));
    writeln!(
        o,
        "static uint64_t tgt_{name}(rl_ctx *c, uint8_t *mem)\n{{\n    tg_call kk, *k = &kk;\n    tg_begin(k, c, mem);"
    )
    .ok();
    let mut after = Vec::new();
    for (i, p) in cmd.params.iter().enumerate() {
        let at = cx.offs[i];
        let t = api.ty(&p.ty);
        if p.ptr == 0 {
            match t {
                Ty::Int(8, _) => writeln!(o, "    {} a{i} = ({})tg_arg64(k, {at});", c_type(t)?, c_type(t)?),
                Ty::Int(..) => writeln!(o, "    {} a{i} = ({})tg_arg32(k, {at});", c_type(t)?, c_type(t)?),
                Ty::Word(true) => writeln!(o, "    intptr_t a{i} = (intptr_t)(int32_t)tg_arg32(k, {at});"),
                Ty::Word(false) => writeln!(o, "    uintptr_t a{i} = (uintptr_t)tg_arg32(k, {at});"),
                Ty::Float(4) => writeln!(
                    o,
                    "    float a{i};\n    {{\n        uint32_t b = tg_arg32(k, {at});\n        memcpy(&a{i}, &b, 4);\n    }}"
                ),
                Ty::Float(_) => writeln!(
                    o,
                    "    double a{i};\n    {{\n        uint64_t b = tg_arg64(k, {at});\n        memcpy(&a{i}, &b, 8);\n    }}"
                ),
                Ty::Handle if cfg.as_integer.contains(&p.ty) => {
                    writeln!(o, "    void *a{i} = (void *)(uintptr_t)tg_arg32(k, {at});")
                }
                Ty::Handle => writeln!(
                    o,
                    "    void *a{i} = tg_unhandle(tg_arg32(k, {at}), \"{name}\");"
                ),
                _ => return Err(fail(&format!("parameter {} by value", p.name))),
            }
            .ok();
            continue;
        }
        writeln!(o, "    uint32_t g{i} = tg_arg32(k, {at});").ok();
        let key = cx.key(p);
        let binding = cx.binding(p)?;
        if cfg.word_pointers.contains(&key) {
            writeln!(
                o,
                "    uintptr_t w{i} = g{i} ? (uintptr_t)tg_ld(mem + g{i}, 4) : 0;\n    void *a{i} = g{i} ? &w{i} : NULL;"
            )
            .ok();
        } else if p.ptr == 1
            && let Some(b) = &binding
        {
            if p.pointee_const {
                writeln!(o, "    const void *a{i} = tg_gl_offset(k, g{i}, {b});").ok();
            } else {
                writeln!(
                    o,
                    "    void *a{i} = (void *)(uintptr_t)tg_gl_offset(k, g{i}, {b});"
                )
                .ok();
            }
        } else if p.ptr == 1 && matches!(t, Ty::Int(..) | Ty::Float(_) | Ty::Void) {
            let q = if p.pointee_const {
                "const void *"
            } else {
                "void *"
            };
            writeln!(o, "    {q}a{i} = tg_ptr(k, g{i});").ok();
        } else if p.ptr == 1 && matches!(t, Ty::Word(_)) {
            if p.pointee_const {
                let end = cfg
                    .terminated
                    .get(&p.ty)
                    .ok_or_else(|| fail(&format!("{} list without a terminator", p.name)))?;
                writeln!(o, "    const intptr_t *a{i} = {pfx}_list(k, g{i}, {end});").ok();
            } else {
                writeln!(
                    o,
                    "    intptr_t w{i} = g{i} ? (intptr_t)(int32_t)tg_ld(mem + g{i}, 4) : 0;\n    intptr_t *a{i} = g{i} ? &w{i} : NULL;"
                )
                .ok();
                after.push(format!(
                    "    if (g{i})\n        tg_st(mem + g{i}, 4, (uint32_t)w{i});"
                ));
            }
        } else if p.ptr == 1 && t == Ty::Handle {
            let n = cx
                .count(p)?
                .ok_or_else(|| fail(&format!("handle array {} without a count", p.name)))?;
            writeln!(o, "    uint64_t n{i} = g{i} ? {n} : 0;").ok();
            writeln!(
                o,
                "    void **a{i} = g{i} ? tg_alloc(k, (n{i} ? n{i} : 1) * sizeof *a{i}) : NULL;"
            )
            .ok();
            if p.pointee_const {
                writeln!(
                    o,
                    "    for (uint64_t j = 0; j < n{i}; j++)\n        a{i}[j] = tg_unhandle((uint32_t)tg_ld(mem + g{i} + 4 * j, 4), \"{name}\");"
                )
                .ok();
            } else {
                after.push(format!(
                    "    for (uint64_t j = 0; a{i} && j < n{i}; j++)\n        tg_st(mem + g{i} + 4 * j, 4, tg_handle(a{i}[j]));"
                ));
            }
        } else if p.ptr == 2 && p.pointee_const && matches!(t, Ty::Int(1, _) | Ty::Void) {
            let n = cx
                .count(p)?
                .ok_or_else(|| fail(&format!("pointer array {} without a count", p.name)))?;
            let each = match &binding {
                Some(b) => format!("tg_gl_offset(k, e, {b})"),
                None => "tg_ptr(k, e)".to_string(),
            };
            writeln!(
                o,
                "    uint64_t n{i} = g{i} ? {n} : 0;\n    const void **a{i} = g{i} ? tg_alloc(k, (n{i} ? n{i} : 1) * sizeof *a{i}) : NULL;\n    for (uint64_t j = 0; j < n{i}; j++) {{\n        uint32_t e = (uint32_t)tg_ld(mem + g{i} + 4 * j, 4);\n        a{i}[j] = {each};\n    }}"
            )
            .ok();
        } else if p.ptr == 2
            && !p.pointee_const
            && t == Ty::Void
            && matches!(p.len.as_deref(), None | Some("1"))
        {
            writeln!(
                o,
                "    void *p{i} = NULL;\n    void **a{i} = g{i} ? &p{i} : NULL;"
            )
            .ok();
            after.push(format!(
                "    if (g{i})\n        tg_st(mem + g{i}, 4, tg_gl_unptr(k, p{i}, \"{name}\"));"
            ));
        } else {
            return Err(fail(&format!(
                "parameter {} has no i386 conversion",
                p.name
            )));
        }
    }
    let rt = api.ty(&cmd.ret.ty);
    let ret = if cmd.ret.ptr > 0 {
        if cmd.ret.ptr == 1 && cmd.ret.pointee_const && matches!(rt, Ty::Int(1, _)) {
            "const char *".to_string()
        } else {
            return Err(fail("pointer result"));
        }
    } else {
        match rt {
            Ty::Int(..) | Ty::Word(_) | Ty::Handle | Ty::Void => c_type(rt)?.to_string(),
            _ => return Err(fail("result type")),
        }
    };
    let ps: Vec<String> = cmd
        .params
        .iter()
        .map(|p| host_param_type(api, p))
        .collect::<Result<_, _>>()?;
    writeln!(
        o,
        "    typedef {ret} (*fn_t)({});\n    fn_t fn = (fn_t){pfx}_host({idx}, \"{name}\");",
        if ps.is_empty() {
            "void".into()
        } else {
            ps.join(", ")
        }
    )
    .ok();
    let args: Vec<String> = (0..cmd.params.len()).map(|i| format!("a{i}")).collect();
    let call = format!("fn({})", args.join(", "));
    if ret == "void" {
        writeln!(o, "    {call};").ok();
    } else {
        writeln!(o, "    {ret} r = {call};").ok();
    }
    for a in &after {
        writeln!(o, "{a}").ok();
    }
    if cmd.ret.ptr > 0 {
        writeln!(o, "    tg_i386_set_eax(c, tg_str_out(k, r));").ok();
    } else {
        match rt {
            Ty::Int(8, _) => writeln!(
                o,
                "    tg_i386_set_eax(c, (uint32_t)r);\n    tg_i386_set_edx(c, (uint32_t)((uint64_t)r >> 32));"
            ),
            Ty::Handle if cfg.as_integer.contains(&cmd.ret.ty) => {
                writeln!(o, "    tg_i386_set_eax(c, (uint32_t)(uintptr_t)r);")
            }
            Ty::Handle => writeln!(o, "    tg_i386_set_eax(c, tg_handle(r));"),
            Ty::Void => Ok(()),
            _ => writeln!(o, "    tg_i386_set_eax(c, (uint32_t)r);"),
        }
        .ok();
    }
    writeln!(o, "    return tg_end(k, 0);\n}}\n").ok();
    Ok(())
}

/// Emits the forwarding of one OpenGL-family library for an i386 guest.
pub fn emit(api: &Api, cfg: &Config, tag: &str) -> Result<String, Error> {
    let pfx = format!("tg_{}", cfg.api);
    let soname = &cfg.soname;
    if soname.is_empty() {
        return Err(Error(
            "target.soname is required for an OpenGL-family API".into(),
        ));
    }
    for h in &cfg.hand_commands {
        if !api.commands.iter().any(|c| &c.name == h) {
            return Err(Error(format!("hand command {h} is not a selected command")));
        }
    }
    let keys: Vec<String> = api
        .commands
        .iter()
        .flat_map(|c| {
            c.params
                .iter()
                .map(move |p| format!("{}.{}", c.name, p.name))
        })
        .collect();
    for k in cfg
        .offsets
        .values()
        .flatten()
        .chain(&cfg.word_pointers)
        .chain(cfg.lens.keys())
    {
        if !keys.contains(k) {
            return Err(Error(format!(
                "{k} is not a parameter of a selected command"
            )));
        }
    }
    let n = api.commands.len();
    let lookup = if cfg.proc_address.is_empty() {
        "NULL".to_string()
    } else {
        format!("\"{}\"", cfg.proc_address)
    };
    let mut o = String::new();
    writeln!(
        o,
        "/* Generated by relift-vkgen from the Khronos registry ({tag}) for an i386 guest. Do not edit. */\n#include \"../../../i386/tg.h\"\n\n#include <pthread.h>\n"
    )
    .ok();
    if !cfg.offsets.is_empty() {
        writeln!(o, "/** @brief A GL pointer argument: a buffer offset while a buffer is bound to binding, else a guest pointer (gl_hand.c). */\nconst void *tg_gl_offset(tg_call *k, uint32_t g, uint32_t binding);").ok();
    }
    if api
        .commands
        .iter()
        .any(|c| c.params.iter().any(|p| p.ptr == 2 && !p.pointee_const))
    {
        writeln!(o, "/** @brief A pointer GL gives back: a guest address, or a buffer offset as it is (gl_hand.c). */\nuint32_t tg_gl_unptr(tg_call *k, const void *h, const char *fn);").ok();
    }
    writeln!(
        o,
        "\nstatic void *{pfx}_fns[{n}];\nstatic const char *const {pfx}_names[{n}] = {{"
    )
    .ok();
    for c in &api.commands {
        writeln!(o, "    \"{}\",", c.name).ok();
    }
    writeln!(
        o,
        "}};\nstatic pthread_once_t {pfx}_once = PTHREAD_ONCE_INIT;\n\nstatic void {pfx}_bind(void)\n{{\n    for (uint32_t i = 0; i < {n}; i++)\n        {pfx}_fns[i] = tg_dlsym(\"{soname}\", {pfx}_names[i], {lookup});\n}}\n\n/** @brief The host function of command i; a missing one stops the program. */\nstatic void *{pfx}_host(uint32_t i, const char *name)\n{{\n    pthread_once(&{pfx}_once, {pfx}_bind);\n    if (!{pfx}_fns[i])\n        tg_fail(name, \"missing in the host library {soname}\");\n    return {pfx}_fns[i];\n}}\n"
    )
    .ok();
    if !cfg.terminated.is_empty() {
        writeln!(
            o,
            "/** @brief A guest list of pointer-sized integers ending with end at an even index, widened for the host. */\nstatic const intptr_t *{pfx}_list(tg_call *k, uint32_t g, int32_t end)\n{{\n    if (!g)\n        return NULL;\n    uint32_t n = 0;\n    while ((int32_t)tg_ld(k->mem + g + 4 * n, 4) != end)\n        n += 2;\n    intptr_t *h = tg_alloc(k, 8 * ((size_t)n + 1));\n    for (uint32_t i = 0; i <= n; i++)\n        h[i] = (int32_t)tg_ld(k->mem + g + 4 * i, 4);\n    return h;\n}}\n"
        )
        .ok();
    }
    let hand = |c: &Cmd| c.hand || cfg.hand_commands.contains(&c.name);
    for c in api.commands.iter().filter(|c| hand(c)) {
        writeln!(o, "uint64_t tgh_{}(rl_ctx *c, uint8_t *mem);", c.name).ok();
    }
    writeln!(o).ok();
    for (i, c) in api.commands.iter().enumerate() {
        if !hand(c) {
            emit_thunk(api, cfg, c, i, &pfx, &mut o)?;
        }
    }
    writeln!(o, "static const tg_fn {pfx}_table[{n}] = {{").ok();
    for c in &api.commands {
        let f = if hand(c) { "tgh" } else { "tgt" };
        writeln!(o, "    {{\"{}\", {f}_{}}},", c.name, c.name).ok();
    }
    writeln!(
        o,
        "}};\n\nstatic void {pfx}_load(void)\n{{\n    pthread_once(&{pfx}_once, {pfx}_bind);\n}}\n\nconst tg_lib tg_lib_{} = {{\"{soname}\", {n}, {pfx}_table, {pfx}_load}};",
        cfg.api
    )
    .ok();
    Ok(o)
}
