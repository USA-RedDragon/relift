//! The Vulkan memory model, for modules that need it.
//!
//! SPIR-V requires `OpMemoryModel Vulkan` of a shader that declares
//! `CooperativeMatrixKHR`. Everything else the translator emits is written
//! for the GLSL450 model, so a module that uses cooperative matrices is
//! rewritten once it is complete, keeping the GLSL450 meaning:
//!
//! * shared memory is coherent within the workgroup under GLSL450, so every
//!   load and store through a `Workgroup` pointer gets `NonPrivatePointer`;
//!   plain global and local accesses stay private (under GLSL450 they were
//!   not coherent either, and the translator turns every global access that
//!   may meet another invocation into an atomic, see [`super::mem::order`]);
//! * every barrier and atomic that orders memory performs the availability
//!   and visibility operations GLSL450 implies: `MakeAvailable` with release
//!   semantics, `MakeVisible` with acquire semantics, and
//!   `SequentiallyConsistent`, which the Vulkan model does not allow,
//!   becomes `AcquireRelease` (Vulkan gives the two the same meaning);
//! * a `Coherent` storage image loses the decoration, banned under the
//!   Vulkan model, and each read or write of it becomes non-private and
//!   made visible or available at `Device` scope;
//! * Device scope needs `VulkanMemoryModelDeviceScope`, which the runtime's
//!   probe requires before it lets the translator use cooperative matrices.

use std::collections::{HashMap, HashSet};

use crate::spirv::op;
use crate::translate::Ctx;

const CAP_VULKAN_MEMORY_MODEL: u32 = 5345;
const CAP_VULKAN_MEMORY_MODEL_DEVICE_SCOPE: u32 = 5346;
const STORAGE_WORKGROUP: u32 = 4;
const DECORATION_COHERENT: u32 = 23;
const SCOPE_DEVICE: u32 = 1;
const NON_PRIVATE_POINTER: u32 = 0x20;
const ACQUIRE: u32 = 0x2;
const RELEASE: u32 = 0x4;
const ACQUIRE_RELEASE: u32 = 0x8;
const SEQUENTIALLY_CONSISTENT: u32 = 0x10;
const MAKE_AVAILABLE: u32 = 0x2000;
const MAKE_VISIBLE: u32 = 0x4000;
const MAKE_TEXEL_AVAILABLE: u32 = 0x100;
const MAKE_TEXEL_VISIBLE: u32 = 0x200;
const NON_PRIVATE_TEXEL: u32 = 0x400;

const FUNCTION_PARAMETER: u16 = 55;
const IN_BOUNDS_ACCESS_CHAIN: u16 = 66;
const PTR_ACCESS_CHAIN: u16 = 67;
const COPY_OBJECT: u16 = 83;
const CONVERT_U_TO_PTR: u16 = 120;
const SELECT: u16 = 169;
const ATOMIC_LOAD: u16 = 227;
const ATOMIC_STORE: u16 = 228;
const ATOMIC_COMPARE_EXCHANGE_WEAK: u16 = 231;
const ATOMIC_FLAG_TEST_AND_SET: u16 = 318;
const ATOMIC_FLAG_CLEAR: u16 = 319;
const ATOMIC_F_MIN: u16 = 5614;
const ATOMIC_F_MAX: u16 = 5615;
const ATOMIC_F_ADD: u16 = 6035;

/// Whether the module must use the Vulkan memory model.
pub(crate) fn needed(c: &Ctx) -> bool {
    c.exts.contains("SPV_KHR_cooperative_matrix")
}

/// Rewrites the finished module for the Vulkan memory model (see the
/// module docs) and declares its capabilities.
pub(crate) fn lower(c: &mut Ctx) {
    c.cap(CAP_VULKAN_MEMORY_MODEL);
    c.cap(CAP_VULKAN_MEMORY_MODEL_DEVICE_SCOPE);
    let mut ptr_class = HashMap::new();
    let mut consts = HashMap::new();
    let mut class_of = HashMap::new();
    for (opc, a) in insts(&c.m.types) {
        match opc {
            op::TYPE_POINTER => {
                ptr_class.insert(a[0], a[1]);
            }
            op::CONSTANT if a.len() == 3 => {
                consts.insert(a[1], a[2]);
            }
            op::VARIABLE => {
                if let Some(&k) = ptr_class.get(&a[0]) {
                    class_of.insert(a[1], k);
                }
            }
            _ => {}
        }
    }
    let coherent = drop_coherent(&mut c.m.decos);
    let mut derived: HashSet<u32> = coherent;
    for (opc, a) in insts(&c.m.body) {
        let result_ptr = matches!(
            opc,
            op::VARIABLE
                | op::ACCESS_CHAIN
                | IN_BOUNDS_ACCESS_CHAIN
                | PTR_ACCESS_CHAIN
                | CONVERT_U_TO_PTR
                | op::BITCAST
                | COPY_OBJECT
                | FUNCTION_PARAMETER
                | SELECT
                | op::PHI
        );
        if result_ptr
            && a.len() >= 2
            && let Some(&k) = ptr_class.get(&a[0])
        {
            class_of.insert(a[1], k);
        }
        let from = match opc {
            op::ACCESS_CHAIN
            | IN_BOUNDS_ACCESS_CHAIN
            | PTR_ACCESS_CHAIN
            | op::LOAD
            | COPY_OBJECT => a.get(2),
            _ => None,
        };
        if from.is_some_and(|f| derived.contains(f)) {
            derived.insert(a[1]);
        }
    }
    let device = c.m.c_u32(SCOPE_DEVICE);
    let body = std::mem::take(&mut c.m.body);
    let mut out = Vec::with_capacity(body.len() + body.len() / 16);
    for (opc, a) in insts(&body) {
        let mut a = a.to_vec();
        match opc {
            op::LOAD if class_of.get(&a[2]) == Some(&STORAGE_WORKGROUP) => {
                add_mask(&mut a, 3, NON_PRIVATE_POINTER);
            }
            op::STORE if class_of.get(&a[0]) == Some(&STORAGE_WORKGROUP) => {
                add_mask(&mut a, 2, NON_PRIVATE_POINTER);
            }
            op::CONTROL_BARRIER => semantics(c, &consts, &mut a, 2),
            op::MEMORY_BARRIER => semantics(c, &consts, &mut a, 1),
            ATOMIC_STORE => semantics(c, &consts, &mut a, 2),
            ATOMIC_FLAG_CLEAR => semantics(c, &consts, &mut a, 2),
            op::ATOMIC_COMPARE_EXCHANGE | ATOMIC_COMPARE_EXCHANGE_WEAK => {
                semantics(c, &consts, &mut a, 4);
                semantics(c, &consts, &mut a, 5);
            }
            ATOMIC_LOAD
            | op::ATOMIC_EXCHANGE
            | op::ATOMIC_I_INCREMENT..=op::ATOMIC_XOR
            | ATOMIC_FLAG_TEST_AND_SET
            | ATOMIC_F_MIN
            | ATOMIC_F_MAX
            | ATOMIC_F_ADD => semantics(c, &consts, &mut a, 4),
            op::IMAGE_READ if derived.contains(&a[2]) => {
                image_operands(&mut a, 4, MAKE_TEXEL_VISIBLE, device);
            }
            op::IMAGE_WRITE if derived.contains(&a[0]) => {
                image_operands(&mut a, 3, MAKE_TEXEL_AVAILABLE, device);
            }
            _ => {}
        }
        crate::spirv::inst(&mut out, opc, &a);
    }
    c.m.body = out;
}

/// The instructions of a section: opcode and operands.
fn insts(words: &[u32]) -> Vec<(u16, &[u32])> {
    let mut out = Vec::new();
    let mut i = 0;
    while i < words.len() {
        let n = (words[i] >> 16) as usize;
        out.push(((words[i] & 0xffff) as u16, &words[i + 1..i + n]));
        i += n.max(1);
    }
    out
}

/// Removes every `Coherent` decoration and gives the decorated ids.
fn drop_coherent(decos: &mut Vec<u32>) -> HashSet<u32> {
    let mut ids = HashSet::new();
    let mut kept = Vec::with_capacity(decos.len());
    for (opc, a) in insts(decos) {
        if opc == op::DECORATE && a.get(1) == Some(&DECORATION_COHERENT) {
            ids.insert(a[0]);
        } else {
            crate::spirv::inst(&mut kept, opc, a);
        }
    }
    *decos = kept;
    ids
}

/// Ors `bit` into the memory operands at `at`, adding them if absent; the
/// bit must take no extra operand.
fn add_mask(a: &mut Vec<u32>, at: usize, bit: u32) {
    if a.len() > at {
        a[at] |= bit;
    } else {
        a.push(bit);
    }
}

/// The memory semantics operand at `at` (a constant id) with the
/// availability and visibility its order implies under GLSL450.
fn semantics(c: &mut Ctx, consts: &HashMap<u32, u32>, a: &mut [u32], at: usize) {
    let Some(&v) = a.get(at).and_then(|id| consts.get(id)) else {
        return;
    };
    let mut s = v;
    if s & SEQUENTIALLY_CONSISTENT != 0 {
        s = s & !SEQUENTIALLY_CONSISTENT | ACQUIRE_RELEASE;
    }
    let storage = s & 0x1fc0 != 0;
    if storage && s & (RELEASE | ACQUIRE_RELEASE) != 0 {
        s |= MAKE_AVAILABLE;
    }
    if storage && s & (ACQUIRE | ACQUIRE_RELEASE) != 0 {
        s |= MAKE_VISIBLE;
    }
    if s != v {
        a[at] = c.m.c_u32(s);
    }
}

/// Operands that each image operand bit below `0x100` takes.
const IMAGE_OPERAND_ARGS: [(u32, usize); 8] = [
    (0x1, 1),
    (0x2, 1),
    (0x4, 2),
    (0x8, 1),
    (0x10, 1),
    (0x20, 1),
    (0x40, 1),
    (0x80, 1),
];

/// Adds `NonPrivateTexel` and `make` (with its scope operand `scope`) to
/// the image operands that start at `at`.
fn image_operands(a: &mut Vec<u32>, at: usize, make: u32, scope: u32) {
    if a.len() <= at {
        a.extend([make | NON_PRIVATE_TEXEL, scope]);
        return;
    }
    let mask = a[at];
    let before: usize = IMAGE_OPERAND_ARGS
        .iter()
        .filter(|(b, _)| mask & b != 0)
        .map(|(_, n)| n)
        .sum();
    a[at] = mask | make | NON_PRIVATE_TEXEL;
    a.insert(at + 1 + before, scope);
}
