//! Task and mesh pipelines in BNSH and agl BAHS shader archives.
//!
//! The pipeline state of a task or mesh program (workgroup size, payload
//! size, output topology, vertex and primitive limits, the primitive SPH)
//! is not in its SPH but in the GLSLC program header NVN takes beside the
//! code, and which programs run together is in the archive. A BNSH
//! program holds a table with one 0x40-byte slot per stage it uses, in
//! stage order (vertex 0, fragment 1, task 6, mesh 7 by the header's stage
//! byte); a slot starts with the header's offset and the code's offset
//! (both 64-bit, from the BNSH start), the code size and the header size
//! (0xe0 or more; measured on two games' archives; the slots are found by that
//! form, inside the BNSH's size at 0x1c). The code starts with the SPH, so the program the carve finds there
//! has the same SHA-256 as the slot's code through its last `BRA self`.
//!
//! A BAHS archive keeps its code from the offset at 0x1c on. Before that,
//! each stage of a program has a record: the code's offset (64-bit, from
//! the code start), a header size (0xd8 or 0xe0 seen), and the header's
//! offset from the record (signed
//! 32-bit; programs that differ only in code share a header). A program's
//! records come in stage order and the next program's records follow;
//! a mesh record ends a program, with the task record before it if there
//! is one and the fragment record before that. The code size is the
//! header's at 0x38.

use crate::carve::{BRA_SELF, PROGRAM_MAGIC};
use crate::sha256_hex;
use serde_json::{Value, json};

/// The GLSLC header is 0xe0 bytes, or more when it lists the program's
/// functions after them.
const HEADER_LEN: usize = 0xe0;
const HEADER_MAX: usize = 0x1000;
const SLOT: usize = 0x40;
const STAGE_AT: usize = 0x56;
const STAGE_FRAGMENT: u8 = 1;
const STAGE_TASK: u8 = 6;
const STAGE_MESH: u8 = 7;
/// The primitive SPH's offset in the code and its size, in the header.
const PRIMITIVE_SPH_AT: usize = 0x64;
const SPH_LEN: usize = 0x80;
/// Where a BNSH holds its size.
const BNSH_SIZE_AT: usize = 0x1c;
/// Where a BAHS holds the offset of its code.
const BAHS_CODE_AT: usize = 0x1c;
/// The smallest header size a BAHS stage record gives.
const BAHS_HEADER_MIN: usize = 0xd8;
/// The code size in a GLSLC header.
const CODE_SIZE_AT: usize = 0x38;

struct Slot {
    at: usize,
    stage: u8,
    header: Vec<u8>,
    code: Vec<u8>,
}

fn u32_at(b: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(at..at + 4)?.try_into().ok()?))
}

fn u64_at(b: &[u8], at: usize) -> Option<usize> {
    usize::try_from(u64::from_le_bytes(b.get(at..at + 8)?.try_into().ok()?)).ok()
}

/// The program at the start of `code`: through its last `BRA self`.
fn program(code: &[u8]) -> &[u8] {
    let n = code
        .as_chunks::<16>()
        .0
        .iter()
        .rposition(|l| l[..12] == BRA_SELF)
        .map_or(code.len(), |k| 16 * (k + 1));
    &code[..n]
}

fn slot(b: &[u8], base: usize, span: usize, p: usize) -> Option<Slot> {
    let ho = u64_at(b, p)?;
    let co = u64_at(b, p + 8)?;
    let size = u32_at(b, p + 16)? as usize;
    let len = u32_at(b, p + 20)? as usize;
    if !(HEADER_LEN..=HEADER_MAX).contains(&len) || size == 0 || ho >= span || co >= span {
        return None;
    }
    let header = b.get(base + ho..base + ho + len)?;
    if header[..4] != PROGRAM_MAGIC {
        return None;
    }
    Some(Slot {
        at: p,
        stage: header[STAGE_AT],
        header: header[..HEADER_LEN].to_vec(),
        code: b.get(base + co..base + co + size)?.to_vec(),
    })
}

/// Every BNSH program in `b` that has a task or mesh stage: per program the
/// SHA-256 of its fragment, task and mesh programs, the task and mesh
/// GLSLC headers and the mesh stage's primitive SPH, as JSON.
pub fn pipelines(b: &[u8]) -> Vec<Value> {
    let mut out = Vec::new();
    let mut seen = std::collections::HashSet::new();
    for base in b
        .windows(4)
        .enumerate()
        .filter_map(|(i, w)| (w == b"BNSH").then_some(i))
    {
        let span = u32_at(b, base + BNSH_SIZE_AT).map_or(0, |n| n as usize);
        let end = b.len().min(base + span);
        let mut groups: Vec<Vec<Slot>> = Vec::new();
        let mut p = base;
        while p + 24 <= end {
            match slot(b, base, span, p) {
                Some(s) if seen.insert(p) => {
                    match groups.last_mut() {
                        Some(g) if g.last().is_some_and(|l| l.at + SLOT == s.at) => g.push(s),
                        _ => groups.push(vec![s]),
                    }
                    p += SLOT;
                }
                _ => p += 8,
            }
        }
        for g in groups {
            if let Some(v) = group(&g) {
                out.push(v);
            }
        }
    }
    out
}

/// Every task and mesh pipeline of the BAHS archives in `b`, as
/// [`pipelines`] gives them.
pub fn bahs_pipelines(b: &[u8]) -> Vec<Value> {
    let mut out = Vec::new();
    for base in b
        .windows(4)
        .enumerate()
        .filter_map(|(i, w)| (w == b"BAHS").then_some(i))
    {
        let Some(code) = u32_at(b, base + BAHS_CODE_AT).map(|c| base + c as usize) else {
            continue;
        };
        let mut run: Vec<Slot> = Vec::new();
        for r in (base..code.min(b.len())).step_by(8) {
            let Some(s) = bahs_record(b, code, r) else {
                continue;
            };
            let stage = s.stage;
            if stage == STAGE_FRAGMENT {
                run.clear();
            }
            run.push(s);
            if stage == STAGE_MESH {
                if let Some(v) = group(&run) {
                    out.push(v);
                }
                run.clear();
            }
        }
    }
    out
}

fn bahs_record(b: &[u8], code: usize, r: usize) -> Option<Slot> {
    let tag = u32_at(b, r + 8)? as usize;
    if !(BAHS_HEADER_MIN..=HEADER_MAX).contains(&tag) || !tag.is_multiple_of(8) {
        return None;
    }
    let rel = i32::from_le_bytes(b.get(r + 12..r + 16)?.try_into().ok()?);
    let h = r.checked_add_signed(isize::try_from(rel).ok()?)?;
    let header = b.get(h..h + HEADER_LEN)?;
    if header[..4] != PROGRAM_MAGIC {
        return None;
    }
    let at = code.checked_add(u64_at(b, r)?)?;
    let size = u32_at(header, CODE_SIZE_AT)? as usize;
    let off = u32_at(header, PRIMITIVE_SPH_AT)? as usize;
    let len = u32_at(header, PRIMITIVE_SPH_AT + 4)? as usize;
    let end = at.checked_add(size.max(off + len))?;
    Some(Slot {
        at: r,
        stage: header[STAGE_AT],
        header: header.to_vec(),
        code: b.get(at..end.min(b.len()))?.to_vec(),
    })
}

fn group(g: &[Slot]) -> Option<Value> {
    let find = |st: u8| g.iter().find(|s| s.stage == st);
    let mesh = find(STAGE_MESH);
    let task = find(STAGE_TASK);
    if mesh.is_none() && task.is_none() {
        return None;
    }
    let sha = |s: Option<&Slot>| {
        s.map(|s| {
            let size = match u32_at(&s.header, CODE_SIZE_AT) {
                Some(n) if n > 0 => n as usize,
                _ => s.code.len(),
            };
            sha256_hex(program(&s.code[..size.min(s.code.len())]))
        })
    };
    let hex = |b: &[u8]| relift_switch_formats::hex(b);
    let mut v = json!({
        "fragment": sha(find(STAGE_FRAGMENT)),
        "task": sha(task),
        "mesh": sha(mesh),
        "task_glslc": task.map(|s| hex(&s.header)),
        "mesh_glslc": mesh.map(|s| hex(&s.header)),
    });
    if let Some(m) = mesh {
        let off = u32_at(&m.header, PRIMITIVE_SPH_AT)? as usize;
        let len = u32_at(&m.header, PRIMITIVE_SPH_AT + 4)? as usize;
        if len == SPH_LEN {
            v["primitive_sph"] = json!(hex(m.code.get(off..off + len)?));
        }
    }
    Some(v)
}
