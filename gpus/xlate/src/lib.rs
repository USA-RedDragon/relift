//! The NVIDIA GPU program translator behind a C ABI (`include/rlx.h`), so
//! the runtime makes a game's shader tables on the player's device: at first
//! launch, and whenever the device description or the translator no longer
//! matches the tables.
//!
//! It runs `relift_gpu_table::translate`, the same code `relift build` and
//! `relift shaders` run, so a table made here is byte for byte the one the
//! build machine makes for the same programs and device description. It
//! reads and writes only the paths the caller passes, opens no network
//! connection and keeps no state between calls beyond an [`Rlx`] handle.
//!
//! Every call catches a panic of the translator and reports it as an
//! error; none unwinds into C.

use std::ffi::{CStr, c_char, c_int, c_void};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::Path;

use relift_gpu_table::{BoxError, One, Programs, Translator};

/// The version of this ABI; `rlx.h`'s `RLX_ABI` must equal it.
pub const ABI: u32 = 1;

/// The translator stamp as a C string (16 hex digits and a NUL).
static STAMP: [u8; 17] = {
    let s = relift_gpu_table::STAMP.as_bytes();
    assert!(s.len() == 16, "a stamp is 16 hex digits");
    let mut b = [0u8; 17];
    let mut i = 0;
    while i < 16 {
        b[i] = s[i];
        i += 1;
    }
    b
};

/// A device's translator and the programs it translates.
pub struct Rlx {
    programs: Programs,
    device: Vec<u8>,
    translator: Translator,
}

/// What a table holds (`rlx_built` in `rlx.h`).
#[repr(C)]
#[derive(Default)]
pub struct RlxBuilt {
    pub programs: u32,
    pub translated: u32,
    pub refused: u32,
}

/// The progress callback: the programs done and in all; returning 0 stops
/// the work.
pub type RlxProgress = Option<unsafe extern "C" fn(*mut c_void, u32, u32) -> c_int>;

/// Writes `msg` into the caller's buffer `why` of `n` bytes, cut and NUL
/// terminated.
///
/// # Safety
///
/// `why` is NULL or points to `n` writable bytes.
unsafe fn tell(why: *mut c_char, n: usize, msg: &str) {
    if why.is_null() || n == 0 {
        return;
    }
    let b = msg.as_bytes();
    let k = b.len().min(n - 1);
    // SAFETY: the caller gives `n` writable bytes at `why`, and k < n.
    unsafe {
        std::ptr::copy_nonoverlapping(b.as_ptr(), why.cast::<u8>(), k);
        *why.add(k) = 0;
    }
}

/// The message of a caught panic.
fn panic_message(p: &(dyn std::any::Any + Send)) -> String {
    let m = p
        .downcast_ref::<&str>()
        .map(|s| (*s).to_string())
        .or_else(|| p.downcast_ref::<String>().cloned())
        .unwrap_or_default();
    format!("the translator panicked: {m}")
}

/// Runs `f`, turning an error or a panic into `why` and -1.
///
/// # Safety
///
/// As [`tell`].
unsafe fn guard(why: *mut c_char, n: usize, f: impl FnOnce() -> Result<(), BoxError>) -> c_int {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => 0,
        Ok(Err(e)) => {
            // SAFETY: as this function's contract.
            unsafe { tell(why, n, &e.to_string()) };
            -1
        }
        Err(p) => {
            // SAFETY: as this function's contract.
            unsafe { tell(why, n, &panic_message(p.as_ref())) };
            -1
        }
    }
}

/// `p` as a path, or none for NULL.
///
/// # Safety
///
/// `p` is NULL or a NUL-terminated string.
unsafe fn path<'a>(p: *const c_char) -> Result<Option<&'a Path>, BoxError> {
    if p.is_null() {
        return Ok(None);
    }
    // SAFETY: the caller gives a NUL-terminated string.
    let s = unsafe { CStr::from_ptr(p) }.to_str()?;
    Ok(Some(Path::new(s)))
}

/// `len` bytes at `p` (none for NULL or 0).
///
/// # Safety
///
/// `p` is NULL or points to `len` readable bytes.
unsafe fn bytes<'a>(p: *const u8, len: usize) -> &'a [u8] {
    if p.is_null() || len == 0 {
        return &[];
    }
    // SAFETY: the caller gives `len` readable bytes at `p`.
    unsafe { std::slice::from_raw_parts(p, len) }
}

/// The ABI version ([`ABI`]).
#[unsafe(no_mangle)]
pub extern "C" fn rlx_abi() -> u32 {
    ABI
}

/// The translator stamp: the one every table this library makes carries.
#[unsafe(no_mangle)]
pub extern "C" fn rlx_stamp() -> *const c_char {
    STAMP.as_ptr().cast()
}

/// A translator for the device `device` describes (the bytes of the
/// runtime's `device.json`), over the programs at `programs` (a pack file
/// or a build's shader folder; NULL for none). NULL with `why` set on an
/// error.
///
/// # Safety
///
/// `programs` is NULL or a NUL-terminated string, `device` points to
/// `device_len` bytes, `why` is NULL or points to `why_len` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlx_new(
    programs: *const c_char,
    device: *const u8,
    device_len: usize,
    why: *mut c_char,
    why_len: usize,
) -> *mut Rlx {
    let mut out = std::ptr::null_mut();
    let run = || -> Result<(), BoxError> {
        // SAFETY: the caller gives a NUL-terminated string or NULL.
        let set = match unsafe { path(programs) }? {
            Some(p) => Programs::open(p)?,
            None => Programs::none(),
        };
        // SAFETY: the caller gives `device_len` bytes at `device`.
        let device = unsafe { bytes(device, device_len) }.to_vec();
        let translator = Translator::new(&set, &device)?;
        out = Box::into_raw(Box::new(Rlx {
            programs: set,
            device,
            translator,
        }));
        Ok(())
    };
    // SAFETY: the caller gives `why_len` bytes at `why`.
    unsafe { guard(why, why_len, run) };
    out
}

/// Frees a translator; NULL is allowed.
///
/// # Safety
///
/// `x` is NULL or from [`rlx_new`], and not used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlx_free(x: *mut Rlx) {
    if !x.is_null() {
        // SAFETY: `x` came from Box::into_raw in rlx_new.
        drop(unsafe { Box::from_raw(x) });
    }
}

/// The number of programs the translator has.
///
/// # Safety
///
/// `x` is from [`rlx_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlx_programs(x: *const Rlx) -> u32 {
    // SAFETY: the caller gives a live handle.
    unsafe { &*x }.programs.keys().len() as u32
}

/// Translates one program (its SPH, if any, then its code: `len` bytes at
/// `code`) as a table holds it. On success `*words` is the SPIR-V module of
/// `*n_words` words, which [`rlx_words_free`] frees; on a refusal or an
/// error -1 with `why` set. Safe to call from several threads at once.
///
/// # Safety
///
/// `x` is from [`rlx_new`], `code` points to `len` bytes, `words` and
/// `n_words` are writable, `why` is NULL or points to `why_len` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlx_translate_one(
    x: *const Rlx,
    code: *const u8,
    len: usize,
    words: *mut *mut u32,
    n_words: *mut usize,
    why: *mut c_char,
    why_len: usize,
) -> c_int {
    // SAFETY: the caller gives writable `words` and `n_words` and a live handle.
    let x = unsafe {
        *words = std::ptr::null_mut();
        *n_words = 0;
        &*x
    };
    let run = || -> Result<(), BoxError> {
        // SAFETY: the caller gives `len` bytes at `code`.
        let code = unsafe { bytes(code, len) }.to_vec();
        let key = relift_gpu_table::sha256(&code);
        match x.translator.one(key, code) {
            One::Failed(e) => Err(e.into()),
            One::Done { words: w, .. } => {
                let b = w.into_boxed_slice();
                // SAFETY: the caller gives writable `words` and `n_words`.
                unsafe {
                    *n_words = b.len();
                    *words = Box::into_raw(b).cast();
                }
                Ok(())
            }
        }
    };
    // SAFETY: the caller gives `why_len` bytes at `why`.
    unsafe { guard(why, why_len, run) }
}

/// Frees a module from [`rlx_translate_one`]; NULL is allowed.
///
/// # Safety
///
/// `words` and `n` are what one call of [`rlx_translate_one`] gave, and
/// `words` is not used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlx_words_free(words: *mut u32, n: usize) {
    if !words.is_null() {
        // SAFETY: the slice came from Box::into_raw of a boxed slice of n words.
        drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(words, n)) });
    }
}

/// Translates every program of `x` on `threads` threads (0 for one per
/// CPU) and writes the tables, their stamp and their device description in
/// the folder `dir`, which is made. `progress` (may be NULL) hears the
/// programs done and in all, on the calling thread, after each batch, and
/// stops the work by returning 0; a stopped run leaves the old tables.
/// What the tables hold goes to `out` (may be NULL). 0 on success, else -1
/// with `why` set.
///
/// # Safety
///
/// `x` is from [`rlx_new`], `dir` is a NUL-terminated string, `out` is NULL
/// or writable, `why` is NULL or points to `why_len` bytes; `progress` is
/// safe to call with `user`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlx_write_tables(
    x: *const Rlx,
    dir: *const c_char,
    threads: u32,
    progress: RlxProgress,
    user: *mut c_void,
    out: *mut RlxBuilt,
    why: *mut c_char,
    why_len: usize,
) -> c_int {
    let user = user as usize;
    // SAFETY: the caller gives a live handle.
    let x = unsafe { &*x };
    let run = || -> Result<(), BoxError> {
        // SAFETY: the caller gives a NUL-terminated string.
        let dir = unsafe { path(dir) }?.ok_or("no table folder")?;
        let jobs = match threads {
            0 => std::thread::available_parallelism().map_or(1, |n| n.get()),
            n => n as usize,
        };
        let tell = |done: usize, all: usize| match progress {
            None => true,
            // SAFETY: the caller's callback, with the caller's user pointer.
            Some(f) => unsafe { f(user as *mut c_void, done as u32, all as u32) != 0 },
        };
        let b = relift_gpu_table::translate(&x.programs, Some(&x.device), dir, jobs, &tell)?;
        if !out.is_null() {
            // SAFETY: the caller gives a writable `out` when it is not NULL.
            unsafe {
                *out = RlxBuilt {
                    programs: b.programs as u32,
                    translated: b.translated as u32,
                    refused: b.refused.values().sum::<usize>() as u32,
                };
            }
        }
        Ok(())
    };
    // SAFETY: the caller gives `why_len` bytes at `why`.
    unsafe { guard(why, why_len, run) }
}
