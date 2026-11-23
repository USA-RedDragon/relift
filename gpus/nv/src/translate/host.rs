//! What the target Vulkan device guarantees, and which fallback the
//! translator took for each guarantee a program needs and the device lacks.
//!
//! A [`HostProfile`] describes one device: at run time it comes from what
//! the device reports ([`HostProfile::from_device`] on a [`DeviceCaps`]);
//! the named presets are the same derivation from a capture of one device
//! of each class relift targets (`tests/caps`), for offline coverage runs.
//! Fields describe what a translated shader may rely on, so a feature that
//! is present but not guaranteed by the API (a subgroup size that cannot be
//! required, a denormal mode that cannot be requested) counts as absent.
//! Every gap has a fallback that keeps the guest's results bit-exact, or the
//! program is reported as inexact on that host in [`HostReport::inexact`].

use std::collections::{BTreeMap, BTreeSet};

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
    pub name: String,
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
    /// `bufferDeviceAddress`.
    pub bda: bool,
    /// `shaderInt64`: 64-bit integer types in shaders. Without it guest
    /// addresses are pairs of 32-bit words.
    pub int64: bool,
    /// 64-bit integer atomics on storage buffers (`shaderBufferInt64Atomics`
    /// with `shaderInt64`).
    pub int64_atomics_buffer: bool,
    /// 64-bit integer atomics on workgroup memory.
    pub int64_atomics_shared: bool,
    /// 64-bit integer atomics on storage images.
    pub int64_atomics_image: bool,
    /// `shaderBufferFloat32AtomicAdd`.
    pub float32_atomic_add: bool,
    /// Denormal behavior without an execution mode.
    pub denorm: Denorm,
    /// A device that flushes as SM86 `.FTZ` does except for what a probe
    /// found in its multiplication, which the translator corrects:
    /// [`FIX_ZERO_SIGN`] and [`FIX_ROUND_AFTER`]. Its addition follows the
    /// flushing model exactly. `None` for a device without such a probe
    /// result.
    pub flush_fix: Option<u8>,
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
    /// so without this FFMA is built from integer operations.
    pub fused_fma: bool,
    /// The runtime keeps guest memory pools as device mirrors (the device has
    /// no `VK_EXT_external_memory_host`; the runtime's `guestMemoryMirrored`
    /// probe) and copies back what the GPU wrote: stores and atomics through
    /// global addresses mark their pages in the written-page bitmap (see
    /// [`super::PUSH_WRITTEN_PAGES`]).
    pub mirror_writes: bool,
    /// A native mesh shader writes `CullPrimitiveEXT` only with constant
    /// stores under a branch, never from a computed bool (see
    /// [`HostProfile::from_device`]).
    pub constant_cull_stores: bool,
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
    /// The host's transcendental operations give the SM86 MUFU results (see
    /// [`HostProfile::from_device`]).
    pub native_mufu: bool,
    /// Half-precision arithmetic as IEEE 754 binary16 with round to nearest
    /// even and denormals kept (`shaderFloat16`, `shaderDenormPreserveFloat16`,
    /// `shaderRoundingModeRTEFloat16`, `shaderSignedZeroInfNanPreserveFloat16`
    /// and a fused `OpFmaKHR` on halves): the SM86 HADD2, HMUL2, HFMA2 and
    /// half conversions without `.FTZ` give the same bits, NaNs aside, so
    /// they run natively instead of being built from integer operations.
    /// Since a driver may report all of these and still flush or round
    /// otherwise, the runtime's probe must also have measured it
    /// (`exactFloat16` in `ReliftDeviceProbes`).
    pub native_f16: bool,
    /// Every single-precision NaN the host's add, multiply, fused
    /// multiply-add, min and max produce is `0x7fffffff`, the SM86 result,
    /// whatever the input payloads, so results need no NaN check. Measured
    /// on the device by the runtime's probe (`canonicalNan32` in the
    /// `ReliftDeviceProbes` of its capability dump); no Vulkan property
    /// promises it.
    pub canonical_nan: bool,
    /// HMMA.F16 runs on `VK_KHR_cooperative_matrix` multiply-adds whose
    /// matrices are built from and read into each lane's registers: the
    /// runtime's probe measured that the device's 16x8x16 and 16x8x8 half
    /// multiply-adds give the SM86 results in that layout
    /// (`hmmaCooperativeMatrix` in `ReliftDeviceProbes`; it asks only when
    /// the device also has the Vulkan memory model at device scope, which
    /// such modules need, and 32-wide compute subgroups). Used only where
    /// warps are 32-wide subgroups.
    pub coop_hmma: bool,
    /// An HMMA that is not a cooperative matrix multiply-add gathers the
    /// A rows and B columns each lane needs through workgroup memory, each
    /// lane's fragment words written once and read back as four-word
    /// vectors, instead of one subgroup shuffle (or one published word with
    /// two barriers on the workgroup path) per word and lane read. The
    /// arithmetic is the same.
    pub hmma_staged: bool,
    /// `maxComputeWorkGroupInvocations`: Vulkan guarantees 128.
    pub max_workgroup_invocations: u32,
    /// `maxComputeWorkGroupSize`: Vulkan guarantees 128, 128 and 64.
    pub max_workgroup_size: [u32; 3],
    /// `minSubgroupSize` and `maxSubgroupSize` (both `subgroupSize` without
    /// `subgroupSizeControl`).
    pub subgroup_sizes: (u32, u32),
    /// The subgroup size the runtime requires of compute pipelines whose
    /// warp operations run on subgroups (`requiredSubgroupSize`, with
    /// `VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT`): the
    /// smallest multiple of 32 the device can require in compute, `None`
    /// where it can require none.
    pub compute_subgroup: Option<u32>,
    /// Guest task and mesh workgroups packed into one compute workgroup of
    /// the mesh fallback (`Options::mesh_slots`): one per 32-lane cluster
    /// of a required subgroup wider than 32 ([`Warp::Wide`]), so that the
    /// subgroup is full, and 1 elsewhere.
    pub mesh_slots: u32,
}

impl Default for HostProfile {
    fn default() -> HostProfile {
        HostProfile::nvidia()
    }
}

impl HostProfile {
    /// The RTX 3090 on NVIDIA's proprietary driver (`tests/caps/nvidia.json`).
    pub fn nvidia() -> HostProfile {
        HostProfile {
            name: "nvidia".into(),
            warp_compute: Warp::Fixed32,
            warp_fragment: Warp::Fixed32,
            warp_graphics: Warp::Fixed32,
            mesh_shader: Some(MeshLimits::NVIDIA),
            warp_mesh: Warp::Fixed32,
            quad_fragment: true,
            maximal_reconvergence: true,
            demote: true,
            bda: true,
            int64: true,
            int64_atomics_buffer: true,
            int64_atomics_shared: true,
            int64_atomics_image: true,
            float32_atomic_add: true,
            denorm: Denorm::Flush,
            flush_fix: None,
            denorm_preserve_mode: false,
            denorm_flush_mode: false,
            signed_zero_inf_nan_preserve: true,
            fused_fma: true,
            mirror_writes: false,
            constant_cull_stores: false,
            clock: Clock::Device,
            max_shared_bytes: 49152,
            max_samplers: 1 << 20,
            max_sampled_images: 1 << 20,
            max_storage_buffers: 1 << 20,
            max_storage_buffer_range: u64::from(u32::MAX),
            native_mufu: true,
            native_f16: true,
            canonical_nan: true,
            coop_hmma: false,
            hmma_staged: false,
            max_workgroup_invocations: 1024,
            max_workgroup_size: [1024, 1024, 64],
            subgroup_sizes: (32, 32),
            compute_subgroup: Some(32),
            mesh_slots: 1,
        }
    }

    /// RADV on RDNA3 (`tests/caps/radv.json`).
    pub fn radv() -> HostProfile {
        HostProfile {
            name: "radv".into(),
            warp_graphics: Warp::Varies,
            mesh_shader: Some(MeshLimits {
                max_task_work_group_invocations: 1024,
                max_mesh_work_group_invocations: 256,
                ..MeshLimits::NVIDIA
            }),
            denorm: Denorm::Unknown,
            flush_fix: None,
            denorm_preserve_mode: true,
            denorm_flush_mode: true,
            max_shared_bytes: 1 << 16,
            max_samplers: 8_388_606,
            max_sampled_images: 8_388_606,
            max_storage_buffers: 8_388_606,
            native_mufu: false,
            native_f16: false,
            canonical_nan: false,
            coop_hmma: false,
            max_workgroup_size: [1024, 1024, 1024],
            subgroup_sizes: (32, 64),
            ..HostProfile::nvidia()
        }
    }

    /// AMD's proprietary Windows driver on RDNA3 (`tests/caps/amd_pro.json`).
    pub fn amd_pro() -> HostProfile {
        HostProfile {
            name: "amd_pro".into(),
            warp_graphics: Warp::Varies,
            mesh_shader: Some(MeshLimits {
                max_task_work_group_invocations: 1024,
                max_mesh_work_group_invocations: 256,
                max_mesh_shared_memory_size: 32_768,
                ..MeshLimits::NVIDIA
            }),
            denorm: Denorm::Unknown,
            flush_fix: None,
            denorm_preserve_mode: true,
            denorm_flush_mode: true,
            max_shared_bytes: 32_768,
            max_samplers: u32::MAX,
            max_sampled_images: u32::MAX,
            max_storage_buffers: u32::MAX,
            native_mufu: false,
            native_f16: false,
            canonical_nan: false,
            coop_hmma: false,
            max_workgroup_size: [1024, 1024, 1024],
            subgroup_sizes: (32, 64),
            ..HostProfile::nvidia()
        }
    }

    /// Mesa ANV on Arc (DG2) (`tests/caps/anv.json`).
    pub fn anv() -> HostProfile {
        HostProfile {
            name: "anv".into(),
            warp_fragment: Warp::Varies,
            warp_graphics: Warp::Varies,
            mesh_shader: Some(MeshLimits {
                max_task_payload_size: 65_504,
                max_task_work_group_invocations: 1024,
                max_mesh_work_group_invocations: 1024,
                max_mesh_shared_memory_size: 65_536,
                ..MeshLimits::NVIDIA
            }),
            int64_atomics_shared: false,
            denorm: Denorm::Unknown,
            flush_fix: None,
            denorm_preserve_mode: true,
            denorm_flush_mode: true,
            clock: Clock::Subgroup,
            max_samplers: 1 << 26,
            max_sampled_images: 1 << 25,
            max_storage_buffers: 1 << 25,
            native_mufu: false,
            native_f16: false,
            canonical_nan: false,
            coop_hmma: false,
            max_workgroup_size: [1024, 1024, 1024],
            subgroup_sizes: (8, 32),
            ..HostProfile::nvidia()
        }
    }

    /// MoltenVK on an Apple M2 (`tests/caps/moltenvk.json`).
    pub fn moltenvk() -> HostProfile {
        HostProfile {
            name: "moltenvk".into(),
            mirror_writes: true,
            warp_compute: Warp::Unchecked32,
            warp_fragment: Warp::Unchecked32,
            warp_graphics: Warp::Absent,
            mesh_shader: None,
            warp_mesh: Warp::Absent,
            demote: false,
            int64_atomics_buffer: false,
            int64_atomics_shared: false,
            int64_atomics_image: false,
            denorm: Denorm::Unknown,
            flush_fix: Some(FIX_ZERO_SIGN),
            clock: Clock::None,
            max_shared_bytes: 32_768,
            max_samplers: 1024,
            max_sampled_images: 1_000_000,
            max_storage_buffers: 1_000_000,
            native_mufu: false,
            native_f16: false,
            canonical_nan: false,
            coop_hmma: false,
            max_workgroup_size: [1024, 1024, 1024],
            subgroup_sizes: (4, 32),
            compute_subgroup: None,
            ..HostProfile::nvidia()
        }
    }

    /// Qualcomm's driver on an Adreno 740 (`tests/caps/adreno.json`).
    pub fn adreno() -> HostProfile {
        HostProfile {
            name: "adreno".into(),
            mirror_writes: true,
            warp_compute: Warp::Wide,
            warp_fragment: Warp::Wide,
            warp_graphics: Warp::Absent,
            mesh_shader: None,
            warp_mesh: Warp::Absent,
            maximal_reconvergence: false,
            int64: false,
            int64_atomics_buffer: false,
            int64_atomics_shared: false,
            int64_atomics_image: false,
            float32_atomic_add: false,
            denorm: Denorm::Unknown,
            flush_fix: Some(FIX_ROUND_AFTER),
            denorm_flush_mode: true,
            fused_fma: false,
            clock: Clock::None,
            max_shared_bytes: 32_768,
            max_samplers: 1 << 24,
            max_sampled_images: 1 << 24,
            max_storage_buffers: 1 << 24,
            max_storage_buffer_range: 1 << 27,
            native_mufu: false,
            native_f16: false,
            canonical_nan: false,
            coop_hmma: false,
            max_workgroup_size: [1024, 1024, 1024],
            subgroup_sizes: (64, 128),
            compute_subgroup: Some(64),
            mesh_slots: 2,
            ..HostProfile::nvidia()
        }
    }

    /// Mesa Turnip on an Adreno 740 (`tests/caps/turnip.json`).
    pub fn turnip() -> HostProfile {
        HostProfile {
            name: "turnip".into(),
            mirror_writes: true,
            warp_compute: Warp::Wide,
            warp_fragment: Warp::Wide,
            warp_graphics: Warp::Wide,
            mesh_shader: None,
            warp_mesh: Warp::Absent,
            maximal_reconvergence: false,
            int64_atomics_shared: false,
            int64_atomics_image: false,
            float32_atomic_add: false,
            denorm: Denorm::Unknown,
            flush_fix: None,
            denorm_preserve_mode: true,
            denorm_flush_mode: true,
            fused_fma: false,
            max_shared_bytes: 32_768,
            max_samplers: 1 << 24,
            max_sampled_images: 1 << 24,
            max_storage_buffers: 1 << 24,
            max_storage_buffer_range: 1 << 27,
            native_mufu: false,
            native_f16: false,
            canonical_nan: false,
            coop_hmma: false,
            max_workgroup_invocations: 2048,
            max_workgroup_size: [1024, 1024, 1024],
            subgroup_sizes: (64, 128),
            compute_subgroup: Some(64),
            mesh_slots: 2,
            ..HostProfile::nvidia()
        }
    }

    /// Arm's driver on a Mali-G715 (`tests/caps/mali.json`).
    pub fn mali() -> HostProfile {
        HostProfile {
            name: "mali".into(),
            mirror_writes: true,
            warp_compute: Warp::Narrow(16),
            warp_fragment: Warp::Narrow(16),
            warp_graphics: Warp::Absent,
            mesh_shader: None,
            warp_mesh: Warp::Absent,
            float32_atomic_add: false,
            denorm: Denorm::Unknown,
            flush_fix: None,
            denorm_preserve_mode: true,
            denorm_flush_mode: true,
            fused_fma: false,
            max_shared_bytes: 32_768,
            max_samplers: 1 << 23,
            max_sampled_images: 1 << 23,
            max_storage_buffers: 1 << 23,
            max_storage_buffer_range: 1 << 31,
            native_mufu: false,
            native_f16: false,
            canonical_nan: false,
            coop_hmma: false,
            max_workgroup_size: [1024, 1024, 1024],
            subgroup_sizes: (16, 16),
            compute_subgroup: None,
            ..HostProfile::nvidia()
        }
    }

    /// lavapipe (llvmpipe) (`tests/caps/lavapipe.json`).
    ///
    /// Before 26.2 (seen with 25.2.8 and 26.0.8) lavapipe draws nothing for
    /// the translated native mesh pipelines, without an error; CI builds a
    /// pinned newer release (`scripts/ci/lib.sh`).
    pub fn lavapipe() -> HostProfile {
        HostProfile {
            name: "lavapipe".into(),
            warp_compute: Warp::Narrow(8),
            warp_fragment: Warp::Narrow(8),
            warp_graphics: Warp::Absent,
            mesh_shader: Some(MeshLimits {
                max_task_work_group_invocations: 1024,
                max_mesh_work_group_invocations: 1024,
                ..MeshLimits::NVIDIA
            }),
            warp_mesh: Warp::Narrow(8),
            denorm: Denorm::Flush,
            flush_fix: None,
            fused_fma: false,
            constant_cull_stores: true,
            max_shared_bytes: 32_768,
            max_samplers: 1_015_808,
            max_sampled_images: 1_015_808,
            max_storage_buffers: 1_015_808,
            max_storage_buffer_range: 1 << 27,
            native_mufu: false,
            native_f16: false,
            canonical_nan: false,
            coop_hmma: false,
            max_workgroup_size: [1024, 1024, 1024],
            subgroup_sizes: (8, 8),
            compute_subgroup: None,
            ..HostProfile::nvidia()
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
            name: "minimal".into(),
            warp_compute: Warp::Varies,
            warp_fragment: Warp::Absent,
            warp_graphics: Warp::Absent,
            mesh_shader: None,
            warp_mesh: Warp::Absent,
            quad_fragment: false,
            maximal_reconvergence: false,
            demote: false,
            bda: false,
            int64: false,
            int64_atomics_buffer: false,
            int64_atomics_shared: false,
            int64_atomics_image: false,
            float32_atomic_add: false,
            denorm: Denorm::Unknown,
            flush_fix: None,
            denorm_preserve_mode: false,
            denorm_flush_mode: false,
            signed_zero_inf_nan_preserve: false,
            fused_fma: false,
            mirror_writes: false,
            constant_cull_stores: false,
            clock: Clock::None,
            max_shared_bytes: 16384,
            max_samplers: 1024,
            max_sampled_images: 1 << 16,
            max_storage_buffers: 1 << 16,
            max_storage_buffer_range: 1 << 27,
            native_mufu: false,
            native_f16: false,
            canonical_nan: false,
            coop_hmma: false,
            hmma_staged: false,
            max_workgroup_invocations: 128,
            max_workgroup_size: [128, 128, 64],
            subgroup_sizes: (1, 128),
            compute_subgroup: None,
            mesh_slots: 1,
        }
    }

    /// The profile of the device that reports `caps`, called `name`.
    ///
    /// Each field is the Vulkan feature, property or extension its
    /// documentation names, with these readings:
    ///
    /// * subgroups ([`Warp`]): a stage outside `supportedStages` has none;
    ///   32 is [`Warp::Fixed32`] where it is the only size or can be
    ///   required in the stage (`requiredSubgroupSizeStages`, and in compute
    ///   `computeFullSubgroups`), a size range of multiples of 32 from 64 is
    ///   [`Warp::Wide`], a single size below 32 is [`Warp::Narrow`],
    ///   anything else [`Warp::Varies`], except that MoltenVK's default
    ///   size of 32 is [`Warp::Unchecked32`] (Apple GPUs run 32-wide SIMD
    ///   groups; Metal cannot require it, so the runtime checks it);
    /// * denormals ([`Denorm`]) come from the runtime's probe of what the
    ///   device does with no denormal mode (`denormDefault32`: 1 flushes as
    ///   SM86 `.FTZ` does, 2 keeps them, anything else or no probe is
    ///   [`Denorm::Unknown`]); the DenormPreserve mode counts only where the
    ///   device lists it and the probe (`denormPreserveMode32`, when
    ///   present) saw it keep denormals;
    /// * NVIDIA's proprietary driver lowers transcendentals to the MUFU unit
    ///   from Ampere (PCI device ids from 0x2200) on;
    /// * MoltenVK reports `shaderDemoteToHelperInvocation` but carries a
    ///   demote out as a terminating discard;
    /// * task and mesh shaders need `VK_EXT_mesh_shader` with both
    ///   `taskShader` and `meshShader`;
    /// * lavapipe (Mesa 26.2.4 and main up to at least 60d7516520,
    ///   2026-11-17) aborts in LLVM ("Cannot emit physreg copy
    ///   instruction") on a mesh shader that stores a computed bool to
    ///   `CullPrimitiveEXT`: gallivm bitcasts the `i1` vector to float.
    ///   Hence [`HostProfile::constant_cull_stores`]; drop it once a Mesa
    ///   release carries the fix that sign-extends 1-bit outputs in
    ///   `emit_store_var` (`lp_bld_nir_soa.c`).
    pub fn from_device(name: &str, caps: &DeviceCaps) -> HostProfile {
        let nvidia = caps.get("driverID") == DRIVER_NVIDIA_PROPRIETARY;
        let int64 = caps.on("shaderInt64");
        HostProfile {
            name: name.into(),
            warp_compute: caps.warp(STAGE_COMPUTE),
            warp_fragment: caps.warp(STAGE_FRAGMENT),
            warp_graphics: caps.warp(STAGE_VERTEX),
            mesh_shader: caps.mesh_limits(),
            warp_mesh: if caps.mesh_limits().is_some() {
                caps.warp(STAGE_MESH)
            } else {
                Warp::Absent
            },
            quad_fragment: caps.stages() & STAGE_FRAGMENT != 0
                && caps.operations() & SUBGROUP_QUAD != 0,
            maximal_reconvergence: caps.extension_on(
                "VK_KHR_shader_maximal_reconvergence",
                "shaderMaximalReconvergence",
            ),
            demote: caps.on("shaderDemoteToHelperInvocation")
                && caps.get("driverID") != DRIVER_MOLTENVK,
            bda: caps.on("bufferDeviceAddress"),
            int64,
            int64_atomics_buffer: int64 && caps.on("shaderBufferInt64Atomics"),
            int64_atomics_shared: int64 && caps.on("shaderSharedInt64Atomics"),
            int64_atomics_image: int64
                && caps.extension_on(
                    "VK_EXT_shader_image_atomic_int64",
                    "shaderImageInt64Atomics",
                ),
            float32_atomic_add: caps
                .extension_on("VK_EXT_shader_atomic_float", "shaderBufferFloat32AtomicAdd"),
            denorm: match caps.get("denormDefault32") {
                1 => Denorm::Flush,
                2 => Denorm::Preserve,
                _ => Denorm::Unknown,
            },
            flush_fix: match (
                caps.get("denormDefault32"),
                caps.on("denormAddFlush32"),
                caps.values.get("denormMulFix32"),
            ) {
                (0, true, Some(&f)) if f <= 3 => Some(f as u8),
                _ => None,
            },
            denorm_preserve_mode: caps.on("shaderDenormPreserveFloat32")
                && (!caps.values.contains_key("denormPreserveMode32")
                    || caps.on("denormPreserveMode32")),
            denorm_flush_mode: caps.on("shaderDenormFlushToZeroFloat32"),
            signed_zero_inf_nan_preserve: caps.on("shaderSignedZeroInfNanPreserveFloat32"),
            fused_fma: caps.extension_on("VK_KHR_shader_fma", "shaderFmaFloat32"),
            mirror_writes: caps.on("guestMemoryMirrored"),
            constant_cull_stores: caps.get("driverID") == DRIVER_MESA_LLVMPIPE,
            clock: if caps.extension_on("VK_KHR_shader_clock", "shaderDeviceClock") {
                Clock::Device
            } else if caps.extension_on("VK_KHR_shader_clock", "shaderSubgroupClock") {
                Clock::Subgroup
            } else {
                Clock::None
            },
            max_shared_bytes: caps.get32("maxComputeSharedMemorySize"),
            max_samplers: caps.get32("maxDescriptorSetUpdateAfterBindSamplers"),
            max_sampled_images: caps.get32("maxDescriptorSetUpdateAfterBindSampledImages"),
            max_storage_buffers: caps.get32("maxPerStageDescriptorUpdateAfterBindStorageBuffers"),
            max_storage_buffer_range: caps.get("maxStorageBufferRange"),
            native_mufu: nvidia && caps.get("deviceID") >= 0x2200,
            native_f16: caps.on("shaderFloat16")
                && caps.on("shaderDenormPreserveFloat16")
                && caps.on("shaderRoundingModeRTEFloat16")
                && caps.on("shaderSignedZeroInfNanPreserveFloat16")
                && caps.extension_on("VK_KHR_shader_fma", "shaderFmaFloat16")
                && caps.on("exactFloat16"),
            canonical_nan: caps.on("canonicalNan32"),
            coop_hmma: caps.on("hmmaCooperativeMatrix"),
            hmma_staged: false,
            max_workgroup_invocations: caps.get32("maxComputeWorkGroupInvocations"),
            max_workgroup_size: [0, 1, 2]
                .map(|k| caps.get32(&format!("maxComputeWorkGroupSize[{k}]"))),
            subgroup_sizes: caps.subgroup_sizes(),
            compute_subgroup: caps.compute_subgroup(),
            mesh_slots: caps.mesh_slots(),
        }
    }
}

/// [`HostProfile::flush_fix`]: a multiplication that gives a zero result
/// the wrong sign (Apple GPUs) gets the sign of the operands' product.
pub const FIX_ZERO_SIGN: u8 = 1;
/// [`HostProfile::flush_fix`]: a multiplication rounds to the denormal grid
/// before it flushes (Adreno), so a product just below the smallest normal
/// number reads as that number; the translator turns it back into zero.
pub const FIX_ROUND_AFTER: u8 = 2;

/// `VK_DRIVER_ID_NVIDIA_PROPRIETARY`.
pub const DRIVER_NVIDIA_PROPRIETARY: u64 = 4;
/// `VK_DRIVER_ID_MESA_LLVMPIPE`.
pub const DRIVER_MESA_LLVMPIPE: u64 = 13;
/// `VK_DRIVER_ID_MOLTENVK`.
pub const DRIVER_MOLTENVK: u64 = 14;
/// `VK_SHADER_STAGE_VERTEX_BIT`.
pub const STAGE_VERTEX: u64 = 0x1;
/// `VK_SHADER_STAGE_FRAGMENT_BIT`.
pub const STAGE_FRAGMENT: u64 = 0x10;
/// `VK_SHADER_STAGE_COMPUTE_BIT`.
pub const STAGE_COMPUTE: u64 = 0x20;
/// `VK_SHADER_STAGE_MESH_BIT_EXT`.
pub const STAGE_MESH: u64 = 0x80;
/// `VK_SUBGROUP_FEATURE_QUAD_BIT`.
pub const SUBGROUP_QUAD: u64 = 0x80;

/// What a Vulkan device reports, for [`HostProfile::from_device`]: its
/// extensions, and its feature and property members by their Vulkan names
/// (`shaderInt64`, `maxSubgroupSize`, `driverID`), booleans as 0 or 1 and
/// flag sets as their bits. The subgroup stages and operations may come
/// under either of their names (`supportedStages` of
/// `VkPhysicalDeviceSubgroupProperties` or `subgroupSupportedStages` of
/// `VkPhysicalDeviceVulkan11Properties`). An array member is one value
/// per element, `name[k]` (`maxComputeWorkGroupSize[2]`). A feature reads
/// as enabled only where its extension, if it has one, is listed too.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct DeviceCaps {
    pub extensions: BTreeSet<String>,
    pub values: BTreeMap<String, u64>,
}

impl DeviceCaps {
    /// The member called `name`, 0 when the device does not report it.
    pub fn get(&self, name: &str) -> u64 {
        self.values.get(name).copied().unwrap_or(0)
    }

    fn get32(&self, name: &str) -> u32 {
        u32::try_from(self.get(name)).unwrap_or(u32::MAX)
    }

    fn on(&self, name: &str) -> bool {
        self.get(name) != 0
    }

    fn extension_on(&self, extension: &str, feature: &str) -> bool {
        self.extensions.contains(extension) && self.on(feature)
    }

    fn mesh_limits(&self) -> Option<MeshLimits> {
        let on = self.extension_on("VK_EXT_mesh_shader", "taskShader")
            && self.extension_on("VK_EXT_mesh_shader", "meshShader");
        on.then(|| MeshLimits {
            max_mesh_output_vertices: self.get32("maxMeshOutputVertices"),
            max_mesh_output_primitives: self.get32("maxMeshOutputPrimitives"),
            max_task_payload_size: self.get32("maxTaskPayloadSize"),
            max_task_work_group_invocations: self.get32("maxTaskWorkGroupInvocations"),
            max_mesh_work_group_invocations: self.get32("maxMeshWorkGroupInvocations"),
            max_mesh_shared_memory_size: self.get32("maxMeshSharedMemorySize"),
        })
    }

    fn stages(&self) -> u64 {
        self.get("supportedStages") | self.get("subgroupSupportedStages")
    }

    fn operations(&self) -> u64 {
        self.get("supportedOperations") | self.get("subgroupSupportedOperations")
    }

    fn subgroup_sizes(&self) -> (u32, u32) {
        let (min, max) = if self.on("subgroupSizeControl") {
            (self.get32("minSubgroupSize"), self.get32("maxSubgroupSize"))
        } else {
            (self.get32("subgroupSize"), self.get32("subgroupSize"))
        };
        (min, max)
    }

    /// Whether a compute pipeline can require a subgroup size with full
    /// subgroups.
    fn compute_size_control(&self) -> bool {
        self.on("subgroupSizeControl")
            && self.get("requiredSubgroupSizeStages") & STAGE_COMPUTE != 0
            && self.on("computeFullSubgroups")
    }

    fn compute_subgroup(&self) -> Option<u32> {
        if self.stages() & STAGE_COMPUTE == 0 || !self.compute_size_control() {
            return None;
        }
        let (min, max) = self.subgroup_sizes();
        (1..=4).map(|k| 32 * k).find(|n| min <= *n && *n <= max)
    }

    fn mesh_slots(&self) -> u32 {
        match (self.warp(STAGE_COMPUTE), self.compute_subgroup()) {
            (Warp::Wide, Some(n)) if n <= self.get32("maxComputeWorkGroupInvocations") => n / 32,
            _ => 1,
        }
    }

    fn warp(&self, stage: u64) -> Warp {
        if self.stages() & stage == 0 {
            return Warp::Absent;
        }
        let size = self.get("subgroupSize");
        let control = self.on("subgroupSizeControl")
            && self.get("requiredSubgroupSizeStages") & stage != 0
            && (stage != STAGE_COMPUTE || self.on("computeFullSubgroups"));
        let (min, max) = if self.on("subgroupSizeControl") {
            (self.get("minSubgroupSize"), self.get("maxSubgroupSize"))
        } else {
            (size, size)
        };
        if (min == 32 && max == 32) || (control && min <= 32 && 32 <= max) {
            Warp::Fixed32
        } else if min >= 64 || (control && min <= 64 && 64 <= max) {
            Warp::Wide
        } else if max < 32 && min == max {
            Warp::Narrow(min as u32)
        } else if size == 32 && self.get("driverID") == DRIVER_MOLTENVK {
            Warp::Unchecked32
        } else {
            Warp::Varies
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
    /// The profile the program was translated for.
    pub host: String,
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
