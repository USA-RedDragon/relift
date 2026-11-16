//! What the target Vulkan device guarantees, and which fallback the
//! translator took for each guarantee a program needs and the device lacks.
//!
//! A [`HostProfile`] describes one device class: the presets are the hosts
//! relift targets, from the drivers' reported limits. Fields describe what a
//! translated shader may rely on, so a feature that is present but not
//! guaranteed by the API (a subgroup size that cannot be required, a denormal
//! mode that cannot be requested) counts as absent. Every gap has a fallback
//! that keeps the guest's results bit-exact, or the program is reported as
//! inexact on that host in [`HostReport::inexact`].

/// Behavior of 32-bit float denormals that a shader can count on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Denorm {
    /// Inputs and results below the normal range read and write as signed
    /// zero, with the result rounded to 24 bits first, as SM86 `.FTZ` does.
    Flush,
    /// Denormal inputs and results are kept (IEEE 754).
    Preserve,
    /// Either, or something in between: the translator flushes and rounds
    /// tiny values itself.
    Unknown,
}

/// The subgroup size a stage can count on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Warp {
    /// 32, fixed or required through `requiredSubgroupSize` with full
    /// subgroups.
    Fixed32,
    /// 32 in practice, but the size cannot be required: the runtime must
    /// check it at start-up.
    Unchecked32,
    /// A multiple of 32 (64 or 128), fixed or required: a guest warp is a
    /// 32-lane cluster of a subgroup.
    Wide,
    /// A fixed size below 32.
    Narrow(u32),
    /// Not controllable and possibly below 32.
    Varies,
    /// No subgroup operations in the stage.
    Absent,
}

/// The device clock a shader can read.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Clock {
    /// `shaderDeviceClock`.
    Device,
    /// Only `shaderSubgroupClock`.
    Subgroup,
    /// Neither: SR_CLOCK reads a counter the shader keeps.
    None,
}

/// What a device guarantees to a translated shader.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HostProfile {
    pub name: &'static str,
    /// Subgroups in compute shaders.
    pub warp_compute: Warp,
    /// Subgroups in fragment shaders.
    pub warp_fragment: Warp,
    /// Subgroups in the other graphics stages.
    pub warp_graphics: Warp,
    /// `VK_EXT_mesh_shader` with task and mesh shaders, and its limits: a
    /// task or mesh pipeline that fits them takes the native back end,
    /// every other one the compute back end.
    pub mesh_shader: Option<MeshLimits>,
    /// Subgroups in task and mesh shaders.
    pub warp_mesh: Warp,
    /// Subgroup quad operations in the fragment stage.
    pub quad_fragment: bool,
    /// `shaderMaximalReconvergence` (VK_KHR_shader_maximal_reconvergence):
    /// invocations that diverge come together again at the merge of the
    /// construct, as guest lanes do at their reconvergence point.
    pub maximal_reconvergence: bool,
    /// `OpDemoteToHelperInvocation` keeps a demoted invocation running as a
    /// helper (false where it maps to a terminating discard).
    pub demote: bool,
    /// `bufferDeviceAddress` with `shaderInt64`.
    pub bda: bool,
    /// 64-bit integer atomics on storage buffers.
    pub int64_atomics_buffer: bool,
    /// 64-bit integer atomics on workgroup memory.
    pub int64_atomics_shared: bool,
    /// 64-bit integer atomics on storage images.
    pub int64_atomics_image: bool,
    /// `shaderBufferFloat32AtomicAdd`.
    pub float32_atomic_add: bool,
    /// Denormal behavior without an execution mode.
    pub denorm: Denorm,
    /// `shaderDenormPreserveFloat32`: the DenormPreserve execution mode can
    /// be requested.
    pub denorm_preserve_mode: bool,
    /// `shaderDenormFlushToZeroFloat32`.
    pub denorm_flush_mode: bool,
    /// `shaderSignedZeroInfNanPreserveFloat32`: the SignedZeroInfNanPreserve
    /// execution mode can be requested. Without it a compiler may treat
    /// signed zeros, infinities and NaNs loosely, and a program with float
    /// arithmetic is reported inexact.
    pub signed_zero_inf_nan_preserve: bool,
    /// `shaderFmaFloat32` (VK_KHR_shader_fma): `OpFmaKHR` is available and
    /// fused. GLSL `Fma` only has to be as precise as a multiply and an add,
    /// so without this FFMA is built from integer operations. Set in the
    /// presets only where a device was seen to report it.
    pub fused_fma: bool,
    pub clock: Clock,
    /// `maxComputeSharedMemorySize`.
    pub max_shared_bytes: u32,
    /// `maxDescriptorSetUpdateAfterBindSamplers`.
    pub max_samplers: u32,
    /// `maxDescriptorSetUpdateAfterBindSampledImages`.
    pub max_sampled_images: u32,
    /// `maxPerStageDescriptorUpdateAfterBindStorageBuffers`.
    pub max_storage_buffers: u32,
    /// `maxStorageBufferRange`.
    pub max_storage_buffer_range: u64,
}

impl Default for HostProfile {
    fn default() -> HostProfile {
        HostProfile::nvidia()
    }
}

impl HostProfile {
    /// NVIDIA proprietary driver, Ampere and later. The driver reports
    /// neither `shaderDenormFlushToZeroFloat32` nor
    /// `shaderDenormPreserveFloat32` (with independence `ALL`), so no
    /// denormal mode can be requested; without one it flushes as SM86 `.FTZ`
    /// does, rounding to 24 bits before the tininess check (an exact
    /// `2^-126 (1 - 2^-46)` from FMUL.FTZ gives `2^-126`, not zero). That was
    /// measured on an SM86 device for FMUL, FFMA and FADD `.FTZ` in all four
    /// roundings on 4096 operand pairs around `2^-126`, bit-exact against
    /// CUDA; the runtime checks it at start-up (`HostReport::check_flush`).
    /// It reports `shaderFmaFloat32` and `shaderSignedZeroInfNanPreserveFloat32`.
    pub fn nvidia() -> HostProfile {
        HostProfile {
            name: "nvidia",
            warp_compute: Warp::Fixed32,
            warp_fragment: Warp::Fixed32,
            warp_graphics: Warp::Fixed32,
            mesh_shader: Some(MeshLimits::NVIDIA),
            warp_mesh: Warp::Fixed32,
            quad_fragment: true,
            maximal_reconvergence: true,
            demote: true,
            bda: true,
            int64_atomics_buffer: true,
            int64_atomics_shared: true,
            int64_atomics_image: true,
            float32_atomic_add: true,
            denorm: Denorm::Flush,
            denorm_preserve_mode: false,
            denorm_flush_mode: false,
            signed_zero_inf_nan_preserve: true,
            fused_fma: true,
            clock: Clock::Device,
            max_shared_bytes: 49152,
            max_samplers: 1 << 20,
            max_sampled_images: 1 << 20,
            max_storage_buffers: 1 << 20,
            max_storage_buffer_range: u64::from(u32::MAX),
        }
    }

    /// RADV on RDNA2/3 (ACO): a required subgroup size of 32 in compute and
    /// fragment, both denormal modes.
    pub fn radv() -> HostProfile {
        HostProfile {
            name: "radv",
            warp_graphics: Warp::Varies,
            warp_mesh: Warp::Varies,
            fused_fma: false,
            denorm: Denorm::Unknown,
            denorm_preserve_mode: true,
            denorm_flush_mode: true,
            float32_atomic_add: false,
            max_shared_bytes: 65536,
            max_samplers: u32::MAX,
            max_sampled_images: u32::MAX,
            max_storage_buffers: u32::MAX,
            ..HostProfile::nvidia()
        }
    }

    /// AMD proprietary and AMDVLK: size 32 only in compute, 32 KiB shared,
    /// no float atomic add, and no shader clock assumed (AMDVLK lacks it).
    pub fn amd_pro() -> HostProfile {
        HostProfile {
            name: "amd_pro",
            warp_fragment: Warp::Varies,
            max_shared_bytes: 32768,
            clock: Clock::None,
            ..HostProfile::radv()
        }
    }

    /// Intel ANV (Xe, Arc): size 32 only in compute, no shared int64
    /// atomics, only the subgroup clock.
    pub fn anv() -> HostProfile {
        HostProfile {
            name: "anv",
            warp_fragment: Warp::Varies,
            int64_atomics_shared: false,
            clock: Clock::Subgroup,
            max_shared_bytes: 49152,
            max_samplers: 1 << 26,
            max_sampled_images: 1 << 25,
            max_storage_buffers: 1 << 25,
            ..HostProfile::radv()
        }
    }

    /// MoltenVK on Apple GPUs (M2, A15): subgroups are natively 32 but
    /// the size cannot be required, demote is a discard, no float controls,
    /// no 64-bit atomics, no shader clock, 32 KiB shared, 1024 samplers.
    pub fn moltenvk() -> HostProfile {
        HostProfile {
            name: "moltenvk",
            warp_compute: Warp::Unchecked32,
            warp_fragment: Warp::Unchecked32,
            warp_graphics: Warp::Absent,
            mesh_shader: None,
            warp_mesh: Warp::Absent,
            quad_fragment: true,
            maximal_reconvergence: true,
            demote: false,
            bda: true,
            int64_atomics_buffer: false,
            int64_atomics_shared: false,
            int64_atomics_image: false,
            float32_atomic_add: true,
            denorm: Denorm::Unknown,
            denorm_preserve_mode: false,
            denorm_flush_mode: false,
            signed_zero_inf_nan_preserve: true,
            fused_fma: false,
            clock: Clock::None,
            max_shared_bytes: 32768,
            max_samplers: 1024,
            max_sampled_images: 1_000_000,
            max_storage_buffers: 1_000_000,
            max_storage_buffer_range: 1 << 31,
        }
    }

    /// Qualcomm's Adreno 740 driver: subgroups of 64 or 128, flush-to-zero
    /// only, no shared int64 or float atomics, 32 KiB shared.
    pub fn adreno() -> HostProfile {
        HostProfile {
            name: "adreno",
            warp_compute: Warp::Wide,
            warp_fragment: Warp::Wide,
            warp_graphics: Warp::Absent,
            mesh_shader: None,
            warp_mesh: Warp::Absent,
            quad_fragment: true,
            maximal_reconvergence: true,
            demote: true,
            bda: true,
            int64_atomics_buffer: true,
            int64_atomics_shared: false,
            int64_atomics_image: true,
            float32_atomic_add: false,
            denorm: Denorm::Unknown,
            denorm_preserve_mode: false,
            denorm_flush_mode: true,
            signed_zero_inf_nan_preserve: true,
            fused_fma: false,
            clock: Clock::Device,
            max_shared_bytes: 32768,
            max_samplers: 1 << 24,
            max_sampled_images: 1 << 24,
            max_storage_buffers: 1 << 24,
            max_storage_buffer_range: 1 << 30,
        }
    }

    /// Turnip (Mesa) on Adreno 7xx. Values from the driver source, not a
    /// device: buffer int64 atomics are not assumed.
    pub fn turnip() -> HostProfile {
        HostProfile {
            name: "turnip",
            warp_graphics: Warp::Wide,
            int64_atomics_buffer: false,
            int64_atomics_image: false,
            max_samplers: 1 << 20,
            max_sampled_images: 1 << 20,
            max_storage_buffers: 1 << 20,
            ..HostProfile::adreno()
        }
    }

    /// Arm Mali Valhall (G710, G715): subgroups of 16, both denormal modes,
    /// no shader clock or float atomic add, 32 KiB shared.
    pub fn mali() -> HostProfile {
        HostProfile {
            name: "mali",
            warp_compute: Warp::Narrow(16),
            warp_fragment: Warp::Narrow(16),
            warp_graphics: Warp::Narrow(16),
            int64_atomics_shared: true,
            denorm_preserve_mode: true,
            clock: Clock::None,
            max_samplers: 1 << 23,
            max_sampled_images: 1 << 23,
            max_storage_buffers: 1 << 23,
            max_storage_buffer_range: 1 << 31,
            ..HostProfile::adreno()
        }
    }

    /// lavapipe (llvmpipe) as measured here: subgroups of 8, no float
    /// controls (it flushes, but the API does not promise it), 32 KiB
    /// shared, 128 MiB storage buffers.
    pub fn lavapipe() -> HostProfile {
        HostProfile {
            name: "lavapipe",
            warp_compute: Warp::Narrow(8),
            warp_fragment: Warp::Narrow(8),
            warp_graphics: Warp::Absent,
            mesh_shader: Some(MeshLimits {
                max_task_work_group_invocations: 1024,
                max_mesh_work_group_invocations: 1024,
                ..MeshLimits::NVIDIA
            }),
            warp_mesh: Warp::Narrow(8),
            quad_fragment: true,
            maximal_reconvergence: true,
            demote: true,
            bda: true,
            int64_atomics_buffer: true,
            int64_atomics_shared: true,
            int64_atomics_image: true,
            float32_atomic_add: true,
            denorm: Denorm::Unknown,
            denorm_preserve_mode: false,
            denorm_flush_mode: false,
            signed_zero_inf_nan_preserve: true,
            fused_fma: false,
            clock: Clock::Device,
            max_shared_bytes: 32768,
            max_samplers: 1_015_808,
            max_sampled_images: 1_015_808,
            max_storage_buffers: 1_015_808,
            max_storage_buffer_range: 1 << 27,
        }
    }

    /// Every preset, by name.
    pub fn presets() -> Vec<HostProfile> {
        vec![
            HostProfile::nvidia(),
            HostProfile::radv(),
            HostProfile::amd_pro(),
            HostProfile::anv(),
            HostProfile::moltenvk(),
            HostProfile::adreno(),
            HostProfile::turnip(),
            HostProfile::mali(),
            HostProfile::lavapipe(),
        ]
    }

    /// The host's own transcendental operations give the SM86 MUFU results
    /// (the NVIDIA driver lowers them to the same unit; see the `mufu`
    /// module for how that was proven).
    pub fn native_mufu(&self) -> bool {
        self.name == "nvidia"
    }

    /// The preset called `name`, [`HostProfile::minimal`] included.
    pub fn preset(name: &str) -> Option<HostProfile> {
        HostProfile::presets()
            .into_iter()
            .chain([HostProfile::minimal()])
            .find(|p| p.name == name)
    }

    /// The device without any optional feature: every fallback on.
    pub fn minimal() -> HostProfile {
        HostProfile {
            name: "minimal",
            warp_compute: Warp::Varies,
            warp_fragment: Warp::Absent,
            warp_graphics: Warp::Absent,
            mesh_shader: None,
            warp_mesh: Warp::Absent,
            quad_fragment: false,
            maximal_reconvergence: false,
            demote: false,
            bda: false,
            int64_atomics_buffer: false,
            int64_atomics_shared: false,
            int64_atomics_image: false,
            float32_atomic_add: false,
            denorm: Denorm::Unknown,
            denorm_preserve_mode: false,
            denorm_flush_mode: false,
            signed_zero_inf_nan_preserve: false,
            fused_fma: false,
            clock: Clock::None,
            max_shared_bytes: 16384,
            max_samplers: 1024,
            max_sampled_images: 1 << 16,
            max_storage_buffers: 1 << 16,
            max_storage_buffer_range: 1 << 27,
        }
    }
}

/// How warp-wide operations are carried out.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum WarpPath {
    /// The program has none.
    #[default]
    None,
    /// Subgroup operations: the runtime must run the stage with full
    /// 32-wide subgroups.
    Subgroup,
    /// Quad-local operations only, through quads of the host's
    /// subgroups: exact on any device with quad operations.
    Quad,
    /// 32-lane clusters of a wider subgroup: the runtime must run the
    /// stage with full subgroups of a multiple of 32.
    Cluster,
    /// Workgroup memory lane arrays and barriers: a guest warp is 32
    /// consecutive local invocations. Needs every warp operation in
    /// workgroup-uniform control flow.
    Shared,
    /// The host's own subgroups, which are not 32 wide, for a program
    /// whose warp-wide operations all give the same result however the
    /// guest groups lanes into warps: exact on any subgroup size. A guest
    /// warp is the aligned 32-invocation group of the subgroup that holds
    /// the invocation (the whole subgroup when it is smaller).
    Invariant,
    /// The host's own subgroups, which are not 32 wide: not exact (listed
    /// in [`HostReport::inexact`]).
    Host,
    /// Compute only: one invocation runs a whole guest warp, with each
    /// register an array of 32 lanes and the divergence in software.
    /// Exact on any device and needs no subgroup operations. The runtime
    /// sets the workgroup size specialization constants to the guest's
    /// size as on every path; the shader's `WorkgroupSize` is that size
    /// divided by 32, rounded up, so the dispatch is unchanged. Local
    /// memory is 32 lanes' worth per invocation.
    Simt,
}

/// How global memory is addressed.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum GlobalPath {
    #[default]
    None,
    /// Buffer device addresses.
    Bda,
    /// An array of storage buffers, each a segment of `1 << shift` bytes of
    /// the guest address space (see [`GUEST_MEMORY_SET`]).
    Segments { shift: u32 },
}

/// How 64-bit atomics are carried out.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Atomic64Path {
    #[default]
    None,
    Native,
    /// A spin lock per address in a lock table, around plain accesses.
    /// A subgroup that waits for a lock another subgroup holds relies on
    /// the device to keep running the holder, which Vulkan does not promise.
    Locked,
}

/// How SR_CLOCK is read.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ClockPath {
    #[default]
    None,
    Device,
    Subgroup,
    /// A per-invocation counter that rises at every read, seeded from a
    /// word of the driver constant bank (see [`CLOCK_SEED_OFFSET`]).
    Counter,
}

/// The `VkPhysicalDeviceMeshShaderPropertiesEXT` limits the native back
/// end depends on (the field names are Vulkan's).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MeshLimits {
    pub max_mesh_output_vertices: u32,
    pub max_mesh_output_primitives: u32,
    pub max_task_payload_size: u32,
    pub max_task_work_group_invocations: u32,
    pub max_mesh_work_group_invocations: u32,
    pub max_mesh_shared_memory_size: u32,
}

impl MeshLimits {
    /// An RTX 3090 (and the `VK_EXT_mesh_shader` minimums for the counts).
    pub const NVIDIA: MeshLimits = MeshLimits {
        max_mesh_output_vertices: 256,
        max_mesh_output_primitives: 256,
        max_task_payload_size: 16384,
        max_task_work_group_invocations: 128,
        max_mesh_work_group_invocations: 128,
        max_mesh_shared_memory_size: 28672,
    };

    /// Whether a pipeline whose mesh workgroup keeps `shared` bytes of
    /// stage buffer fits.
    pub fn fits(&self, p: &super::MeshPipeline, shared: u32) -> bool {
        p.max_vertices <= self.max_mesh_output_vertices
            && p.max_primitives <= self.max_mesh_output_primitives
            && p.payload_bytes <= self.max_task_payload_size
            && p.task_lanes <= self.max_task_work_group_invocations
            && p.mesh_lanes <= self.max_mesh_work_group_invocations
            && shared <= self.max_mesh_shared_memory_size
    }
}

/// Descriptor set of the guest memory segment array (binding 0, a
/// runtime array of storage buffers) used when the device lacks buffer
/// device addresses, and of the 64-bit atomic lock table (binding 1).
pub const GUEST_MEMORY_SET: u32 = 3;
/// Binding of the lock table: a storage buffer of [`LOCK_WORDS`] words
/// the runtime zeroes once.
pub const LOCK_BINDING: u32 = 1;
/// Number of lock words.
pub const LOCK_WORDS: u32 = 4096;
/// Specialization constant id of the guest segment shift.
pub const SEGMENT_SHIFT_SPEC: u32 = 5;
/// Default segment size: 128 MiB, the smallest `maxStorageBufferRange`
/// among the presets.
pub const SEGMENT_SHIFT_DEFAULT: u32 = 27;
/// Specialization constant id of the number of guest segments the runtime
/// binds; binding 0 holds one more descriptor, a trap segment that every
/// address past them reaches.
pub const SEGMENT_COUNT_SPEC: u32 = 6;
/// Default segment count: 256 segments of the default size, 32 GiB.
pub const SEGMENT_COUNT_DEFAULT: u32 = 256;
/// Byte offset in driver constant bank 0 where the runtime puts a 32-bit
/// clock seed for [`ClockPath::Counter`].
pub const CLOCK_SEED_OFFSET: u32 = 0x1f0;

/// The fallbacks a translated program uses and what the runtime must
/// provide for it.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct HostReport {
    /// The preset the program was translated for.
    pub host: &'static str,
    /// The back end of a task or mesh program.
    pub mesh: super::MeshPath,
    pub warp: WarpPath,
    /// Warp operations (SHFL, VOTE, REDUX, MATCH, lane masks, SR_LANEID)
    /// translated through the warp path.
    pub warp_ops: u32,
    /// Votes on a draw-uniform predicate: the same result in any warp the
    /// guest could form, so translated per lane on every host.
    pub warp_invariant: u32,
    /// The warp operations depend on 2x2 quads holding the same pixels as
    /// on the guest, helper invocations included: the runtime must run
    /// the fragment stage with whole quads (`FullQuadsKHR` where it has
    /// `VK_KHR_shader_quad_control`, which keeps helper invocations for
    /// uncovered pixels of a quad as the guest does).
    pub whole_quads: bool,
    /// Workgroup memory the warp emulation adds, in bytes per guest warp.
    pub warp_shared_bytes_per_warp: u32,
    /// Denormal execution mode requested (`None`: the device default).
    pub denorm_mode: Option<Denorm>,
    /// Float instructions that check for tiny values and round them
    /// themselves.
    pub denorm_fixups: u32,
    /// The program uses `OpFmaKHR`: the device must enable
    /// `shaderFmaFloat32` (VK_KHR_shader_fma).
    pub fma_khr: bool,
    pub global: GlobalPath,
    pub atomics64: Atomic64Path,
    pub clock: ClockPath,
    /// The runtime must check at start-up that the stage really runs with
    /// 32-wide subgroups.
    pub check_warp32: bool,
    /// The program asks for maximal reconvergence (the MaximallyReconvergesKHR
    /// execution mode): the runtime must enable `shaderMaximalReconvergence`.
    pub max_reconvergence: bool,
    /// The runtime must check at start-up that the device flushes
    /// denormals as SM86 does (see [`Denorm::Flush`]).
    pub check_flush: bool,
    /// Workgroup memory the program declares beyond the device's limit.
    pub shared_overflow_bytes: u32,
    /// Constructs that cannot be exact on this host, one line each.
    pub inexact: Vec<String>,
}
