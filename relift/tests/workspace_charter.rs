//! The workspace charter: every crate's band and dependency ceiling, and the
//! folder cut edges, read from one `cargo metadata`.
//!
//! Ceilings are judged on DECLARED dependencies (`packages[].dependencies`),
//! which are feature- and platform-independent, so no feature selection or
//! host target can hide an entry. The cut edges are judged on the RESOLVED
//! graph (`resolve.nodes`) over every target, walked as closures: a check of
//! direct names would pass while an indirect route brings the whole of a
//! forbidden crate back. The first hop out of a member may be of any kind
//! (its tests compile what its dev-dependencies reach); later hops are normal
//! or build, which is what Cargo makes transitive.
//!
//! The cut edges are folder rules read from each member's manifest path, so a
//! new crate under `core/` or `guests/` is covered without editing this file.
//! Every absence rule has a presence control beside it, so an empty walk or a
//! reader that reads nothing cannot pass.

use std::collections::{BTreeMap, BTreeSet, VecDeque};
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

/// The workspace root: this test's manifest dir is `relift/`.
const ROOT: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/..");

/// A dependency kind as `cargo metadata` spells it; `null` is normal.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
enum Kind {
    Normal,
    Dev,
    Build,
}

impl Kind {
    fn parse(value: &serde_json::Value) -> Kind {
        match value.as_str() {
            None => Kind::Normal,
            Some("dev") => Kind::Dev,
            Some("build") => Kind::Build,
            Some(other) => {
                panic!("cargo metadata spells a dependency kind this reader does not know: {other}")
            }
        }
    }

    fn label(self) -> &'static str {
        match self {
            Kind::Normal => "normal",
            Kind::Dev => "dev",
            Kind::Build => "build",
        }
    }
}

/// One crate's charter row. An empty list is a rule with no entries, not a
/// rule that is skipped: every declared dependency must be listed here.
struct Charter {
    member: &'static str,
    /// The graph band. A crate may declare crates in its own band or a lower
    /// one, never a higher one.
    band: u8,
    /// What the crate is, printed when its row breaks.
    why: &'static str,
    normal: &'static [&'static str],
    dev: &'static [&'static str],
    build: &'static [&'static str],
}

const NONE: &[&str] = &[];

/// Members that ship their own binaries or are not wired into `relift` yet
/// (`relift-emit-wasm` until a browser build uses it), so the reachability controls skip them; the
/// absence walks still cover them.
const STANDALONE: &[&str] = &[
    "relift-cpu-arm32",
    "relift-cpu-mips",
    "relift-cpu-z80",
    "relift-cpu-rsp",
    "relift-emit-wasm",
    "relift-gb-analyze",
    "relift-gb-tools",
    "relift-gba-tools",
    "relift-n64-tools",
    "relift-nds-tools",
    "relift-sms-tools",
    "relift-switch-shaders",
    "relift-switch-tools",
    "relift-switch-vkgen",
];

const CHARTERS: &[Charter] = &[
    Charter {
        member: "relift-info",
        band: 0,
        why: "the self-description types shared by the tool and launchers, a leaf",
        normal: &["serde", "serde_json"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-ir",
        band: 0,
        why: "the guest-neutral IR, a leaf",
        normal: NONE,
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-patch",
        band: 0,
        why: "IPS, IPS32, UPS and BPS patches, a leaf",
        normal: NONE,
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-cache",
        band: 0,
        why: "the content-addressed store and the one file writer, a leaf",
        normal: NONE,
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-host",
        band: 0,
        why: "the host target list, a leaf",
        normal: NONE,
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-proc",
        band: 0,
        why: "the one subprocess runner and worker count, a leaf",
        normal: NONE,
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-progress",
        band: 0,
        why: "live and plain progress lines, color rules and throttling; writes to a sink it is given, a leaf",
        normal: NONE,
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-sail",
        band: 0,
        why: "the shared Sail frontend (parser, model tables, staged evaluator) for template generators; a leaf that knows no CPU and no guest",
        normal: NONE,
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-diff",
        band: 0,
        why: "the differential check's wire protocol, page cache and comparison; knows no CPU, a leaf",
        normal: NONE,
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-guest",
        band: 1,
        why: "the guest contract and vocabulary, over the IR only",
        normal: &["relift-ir"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-analyze",
        band: 2,
        why: "analysis generic over a CPU frontend; knows no CPU and no guest",
        normal: &["relift-guest", "relift-ir", "relift-proc"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-emit",
        band: 2,
        why: "IR to C per host target; knows no CPU and no guest",
        normal: &["relift-guest", "relift-host", "relift-ir", "relift-proc"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-emit-wasm",
        band: 2,
        why: "IR to WebAssembly modules; knows no CPU and no guest; wasmtime runs its differential tests only",
        normal: &["relift-guest", "relift-ir", "wasm-encoder"],
        dev: &["wasmtime"],
        build: NONE,
    },
    Charter {
        member: "relift-build",
        band: 2,
        why: "toolchains and Ninja, running compilers through relift-proc and writing through relift-cache",
        normal: &["relift-cache", "relift-host", "relift-proc"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-input",
        band: 2,
        why: "the input model: ports, controls, bindings; reads the runtime's name table and renders the self-description section",
        normal: &["relift-info", "toml"],
        dev: &["relift-cache"],
        build: NONE,
    },
    Charter {
        member: "relift-manifest",
        band: 2,
        why: "game.toml types; the guest owns its own section",
        normal: &["relift-guest"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-cpu-arm64",
        band: 2,
        why: "the ARM64 frontend, over the IR and the guest contract only",
        normal: &["relift-guest", "relift-ir"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-cpu-arm32",
        band: 2,
        why: "the ARMv4T (GBA ARM7TDMI) decoder and lifter, over the IR only; no guest uses it yet",
        normal: &["relift-ir"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-cpu-mips",
        band: 2,
        why: "the VR4300 (N64 CPU) decoder and lifter, over the IR only; no guest uses it yet",
        normal: &["relift-ir"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-cpu-rsp",
        band: 2,
        why: "the RSP (N64 signal processor) decoder and lifter, its vector unit over the IR's lane operations; no guest uses it yet",
        normal: &["relift-ir"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-cpu-arm32-analyze",
        band: 2,
        why: "function discovery for 32-bit Arm code across ARM and Thumb, over the arm32 lifter; the machine comes from a platform the guest gives",
        normal: &["relift-cpu-arm32", "relift-ir"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-cpu-sm83",
        band: 2,
        why: "the SM83 decoder, opcode table and frontend, over the IR and the guest contract only",
        normal: &["relift-guest", "relift-ir"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-gpu-sm86",
        band: 2,
        why: "the SM86 (Ampere) SASS decoder and a SASS to SPIR-V translator; reads instruction words and the target device's capabilities as JSON",
        normal: &["serde_json"],
        dev: &["relift-cache", "relift-proc"],
        build: NONE,
    },
    Charter {
        member: "relift-cpu-z80",
        band: 2,
        why: "the Z80 decoder, lifter and frontend over the templates generated from our Sail model; no guest uses it yet",
        normal: &["relift-guest", "relift-ir"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-gb-formats",
        band: 2,
        why: "the Game Boy cartridge format: header, mapper kinds, size codes, checksums and bank layout; reads bytes only",
        normal: NONE,
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-gb-analyze",
        band: 2,
        why: "Game Boy function discovery across mapper banks, over the SM83 decoder and the cartridge format; decode and control flow only",
        normal: &["relift-cpu-sm83", "relift-gb-formats", "relift-proc"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-gb-tools",
        band: 2,
        why: "single-purpose tools for Game Boy files, thin wrappers over relift-gb-formats and relift-gb-analyze",
        normal: &[
            "clap",
            "relift-gb-analyze",
            "relift-gb-formats",
            "serde_json",
        ],
        dev: &[
            "relift-cache",
            "relift-gb-formats",
            "relift-proc",
            "serde_json",
        ],
        build: NONE,
    },
    Charter {
        member: "relift-gba-analyze",
        band: 2,
        why: "Game Boy Advance function discovery: the GBA memory map, BIOS calls, DMA and roots over the shared arm32 discovery and the cartridge format; analysis only",
        normal: &[
            "relift-cpu-arm32",
            "relift-cpu-arm32-analyze",
            "relift-gba-formats",
        ],
        dev: &["relift-gba-formats"],
        build: NONE,
    },
    Charter {
        member: "relift-gba-formats",
        band: 2,
        why: "the Game Boy Advance cartridge format: header, complement and logo hash, size and mirror rules, save library strings and multiboot detection; reads bytes only",
        normal: &["sha2"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-gba-tools",
        band: 2,
        why: "single-purpose tools for Game Boy Advance files, thin wrappers over relift-gba-formats and relift-gba-analyze",
        normal: &[
            "clap",
            "relift-gba-analyze",
            "relift-gba-formats",
            "serde_json",
        ],
        dev: &[
            "relift-cache",
            "relift-gba-formats",
            "relift-proc",
            "serde_json",
        ],
        build: NONE,
    },
    Charter {
        member: "relift-sms-formats",
        band: 2,
        why: "the Master System, Game Gear and SG-1000 cartridge format: size and bank layout, TMR SEGA and Codemasters headers and checksums, system and mapper detection; reads bytes only",
        normal: NONE,
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-nds-analyze",
        band: 2,
        why: "Nintendo DS function discovery: the DS memory map, BIOS calls, DMA and the unit runs (static binaries, overlays, seeds between them) over the shared arm32 discovery and the cartridge format; analysis only",
        normal: &["relift-cpu-arm32-analyze", "relift-nds-formats"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-nds-formats",
        band: 2,
        why: "the Nintendo DS cartridge format: header with CRC16 and logo hash, module parameters, BLZ, static binaries with autoload blocks, overlays and file tables; reads bytes only",
        normal: &["sha2"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-n64-analyze",
        band: 2,
        why: "Nintendo 64 code discovery: the VR4300 lifter's IR run over abstract values, the PI DMA fixed point, the ROM-wide region scan with the link address vote and the RSP image list, over the cartridge format; analysis only",
        normal: &[
            "relift-cpu-mips",
            "relift-cpu-rsp",
            "relift-ir",
            "relift-n64-formats",
        ],
        dev: &["relift-n64-formats"],
        build: NONE,
    },
    Charter {
        member: "relift-n64-formats",
        band: 2,
        why: "the Nintendo 64 cartridge format: byte orders, header, boot variant by hash, checksum, Yay0, Yaz0 and MIO0 decoders and their scan; reads bytes only",
        normal: NONE,
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-sms-tools",
        band: 2,
        why: "single-purpose tools for Master System, Game Gear and SG-1000 files, thin wrappers over relift-sms-formats and relift-sms",
        normal: &[
            "clap",
            "relift-cache",
            "relift-sms",
            "relift-sms-formats",
            "serde_json",
        ],
        dev: &[
            "relift-cache",
            "relift-proc",
            "relift-sms-formats",
            "serde_json",
        ],
        build: NONE,
    },
    Charter {
        member: "relift-n64-tools",
        band: 2,
        why: "single-purpose tools for Nintendo 64 files, thin wrappers over relift-n64-formats and relift-n64-analyze",
        normal: &[
            "clap",
            "relift-cache",
            "relift-n64-analyze",
            "relift-n64-formats",
            "serde_json",
        ],
        dev: &[
            "relift-cache",
            "relift-n64-formats",
            "relift-proc",
            "serde_json",
        ],
        build: NONE,
    },
    Charter {
        member: "relift-nds-tools",
        band: 2,
        why: "single-purpose tools for decrypted Nintendo DS files, thin wrappers over relift-nds-formats and relift-nds-analyze",
        normal: &[
            "clap",
            "relift-cache",
            "relift-nds-analyze",
            "relift-nds-formats",
            "serde_json",
        ],
        dev: &["relift-proc", "serde_json"],
        build: NONE,
    },
    Charter {
        member: "relift-switch-formats",
        band: 2,
        why: "decrypted Switch formats and the import; extraction writes through relift-cache",
        normal: &["lz4_flex", "relift-cache", "sha2", "zstd"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-switch-tools",
        band: 2,
        why: "single-purpose tools for decrypted Switch files, thin wrappers over relift-switch-formats, and the IR dump over relift-cpu-arm64",
        normal: &[
            "clap",
            "relift-cache",
            "relift-cpu-arm64",
            "relift-ir",
            "relift-switch-formats",
            "serde_json",
        ],
        dev: &[
            "relift-cache",
            "relift-proc",
            "relift-switch-formats",
            "serde_json",
        ],
        build: NONE,
    },
    Charter {
        member: "relift-switch-shaders",
        band: 2,
        why: "finds the SM86 shader programs of a Switch 2 dump (carving, containers, KTGL entries); reads decrypted dumps through relift-switch-formats and writes through relift-cache",
        normal: &[
            "clap",
            "relift-cache",
            "relift-proc",
            "relift-switch-formats",
            "serde_json",
            "zstd",
        ],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-switch-vkgen",
        band: 2,
        why: "generates the Switch runtime's Vulkan forwarding C from vk.xml; writes through relift-cache",
        normal: &["clap", "relift-cache", "roxmltree", "sha2", "toml"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-switch-names",
        band: 2,
        why: "Switch symbols as host names, over the guest contract's naming trait only",
        normal: &["relift-guest"],
        dev: &["cpp_demangle"],
        build: NONE,
    },
    Charter {
        member: "relift-switch",
        band: 2,
        why: "the Switch guest over its formats and the ARM64 frontend",
        normal: &[
            "relift-cpu-arm64",
            "relift-guest",
            "relift-ir",
            "relift-switch-formats",
            "relift-switch-names",
        ],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-gb",
        band: 2,
        why: "the Game Boy guest: the exact-mode SM83 frontend and a ROM lifted and emitted one unit per bank, over discovery and the C emitter",
        normal: &[
            "relift-cpu-sm83",
            "relift-emit",
            "relift-gb-analyze",
            "relift-gb-formats",
            "relift-guest",
            "relift-ir",
        ],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-gba",
        band: 2,
        why: "the Game Boy Advance guest: the ARMv4T frontend for the runtime's GBA layer and a ROM lifted and emitted as one unit, over discovery and the C emitter",
        normal: &[
            "relift-cpu-arm32",
            "relift-emit",
            "relift-gba-analyze",
            "relift-guest",
            "relift-ir",
        ],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-sms",
        band: 2,
        why: "the Master System and Game Gear guest: the Z80 frontend for the runtime's SMS run layer and code lifted and emitted one unit per ROM bank and slot and per RAM group, over the C emitter",
        normal: &[
            "relift-cpu-z80",
            "relift-emit",
            "relift-guest",
            "relift-ir",
            "relift-sms-formats",
        ],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-n64",
        band: 2,
        why: "the Nintendo 64 guest: the VR4300 and RSP frontends for the runtime's N64 and RSP layers and a ROM lifted and emitted as one unit per segment and per RSP image, over discovery and the C emitter",
        normal: &[
            "relift-cpu-mips",
            "relift-cpu-rsp",
            "relift-emit",
            "relift-guest",
            "relift-ir",
            "relift-n64-analyze",
            "relift-n64-formats",
        ],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-nds",
        band: 2,
        why: "the Nintendo DS guest: the frontend of both CPUs for the runtime's NDS layer and a cartridge's units lifted and emitted, over discovery and the C emitter",
        normal: &[
            "relift-cpu-arm32",
            "relift-emit",
            "relift-guest",
            "relift-ir",
            "relift-nds-analyze",
            "relift-nds-formats",
        ],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-testguest",
        band: 2,
        why: "the synthetic guest the core is tested against",
        normal: &["relift-guest", "relift-ir"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-cpu-arm64-gen",
        band: 3,
        why: "the Arm64 template generator over the decoder, the IR and the Sail frontend, run by hand; nothing links it",
        normal: &["relift-cpu-arm64", "relift-ir", "relift-sail"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-cpu-arm32-gen",
        band: 3,
        why: "the ARMv4T decode table and template generator over the lifter's register file and the Sail frontend, run by hand; nothing links it",
        normal: &["relift-cpu-arm32", "relift-sail"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-cpu-mips-gen",
        band: 3,
        why: "the VR4300 decode table and template generator over the lifter's register file and the Sail frontend, run by hand; nothing links it",
        normal: &["relift-cpu-mips", "relift-sail"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-cpu-rsp-gen",
        band: 3,
        why: "the RSP decode table and template generator over the lifter's register file and the Sail frontend, run by hand; nothing links it",
        normal: &["relift-cpu-rsp", "relift-sail"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-cpu-sm83-gen",
        band: 3,
        why: "the SM83 template generator over the opcode table and the Sail frontend, run by hand; nothing links it",
        normal: &["relift-cpu-sm83", "relift-sail"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-cpu-z80-gen",
        band: 3,
        why: "the Z80 template generator over the lifter's template groups and the Sail frontend, run by hand; nothing links it",
        normal: &["relift-cpu-z80", "relift-sail"],
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift",
        band: 3,
        why: "the binary: settings, dispatch and the guest registry",
        normal: &[
            "clap",
            "configulator-rs",
            "jpeg-decoder",
            "relift-analyze",
            "relift-build",
            "relift-cache",
            "relift-cpu-arm64",
            "relift-cpu-sm83",
            "relift-diff",
            "relift-emit",
            "relift-gb",
            "relift-gb-analyze",
            "relift-gb-formats",
            "relift-gba",
            "relift-gba-analyze",
            "relift-gba-formats",
            "relift-gpu-sm86",
            "relift-guest",
            "relift-host",
            "relift-info",
            "relift-input",
            "relift-ir",
            "relift-manifest",
            "relift-n64",
            "relift-n64-analyze",
            "relift-n64-formats",
            "relift-nds",
            "relift-nds-analyze",
            "relift-nds-formats",
            "relift-patch",
            "relift-proc",
            "relift-progress",
            "relift-sms",
            "relift-sms-formats",
            "relift-switch",
            "relift-switch-formats",
            "relift-switch-names",
            "relift-testguest",
            "serde_json",
            "sha2",
            "toml",
            "uds_windows",
            "zstd",
        ],
        dev: &["relift-cpu-z80"],
        build: NONE,
    },
    Charter {
        member: "relift-override-macros",
        band: 0,
        why: "override attribute macros for game repos; nothing from the tool",
        normal: NONE,
        dev: NONE,
        build: NONE,
    },
    Charter {
        member: "relift-override",
        band: 1,
        why: "the Rust override API for game repos; nothing from the tool",
        normal: &["relift-override-macros"],
        dev: NONE,
        build: NONE,
    },
];

/// The folder a member sits in, which is what the cut edges are about.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord)]
enum Folder {
    Core,
    Cpus,
    /// `gpus/<gpu>`, a GPU shader decoder and translator.
    Gpus,
    /// `guests/<name>/*`; each name is its own guest.
    Guest(String),
    /// `testguest/`, a guest of its own.
    TestGuest,
    Override,
    /// `relift/`, the binary.
    Tool,
    /// `cpus/<cpu>/gen`, a generator run by hand to refresh committed code.
    Generator,
}

impl Folder {
    fn of(rel: &str) -> Folder {
        let parts: Vec<&str> = rel.split('/').collect();
        match parts.as_slice() {
            ["core", _] => Folder::Core,
            ["cpus", _] => Folder::Cpus,
            ["gpus", _] => Folder::Gpus,
            ["guests", name, _] => Folder::Guest((*name).to_string()),
            ["testguest"] => Folder::TestGuest,
            ["override", _] => Folder::Override,
            ["relift"] => Folder::Tool,
            ["cpus", _, "gen"] => Folder::Generator,
            ["cpus", _, _] => Folder::Cpus,
            _ => {
                panic!("member at {rel} sits in no folder the charter knows; add it to Folder::of")
            }
        }
    }

    fn is_guest(&self) -> bool {
        matches!(self, Folder::Guest(_) | Folder::TestGuest)
    }
}

/// One member as the metadata lists it.
struct Member {
    name: String,
    /// The manifest directory relative to the root, `/`-separated.
    rel: String,
    folder: Folder,
    /// Declared edges: `(kind, dependency name)`.
    declared: BTreeSet<(Kind, String)>,
}

/// The whole workspace as one `cargo metadata` read.
struct Workspace {
    root: PathBuf,
    /// Members by package id.
    members: BTreeMap<String, Member>,
    /// Every package in the graph, member or not: id to (name, manifest dir).
    packages: BTreeMap<String, (String, PathBuf)>,
    /// Resolved edges by package id: `(dependency id, kinds)`.
    resolved: BTreeMap<String, Vec<(String, BTreeSet<Kind>)>>,
    /// The root manifest's `workspace.exclude`.
    exclude: Vec<String>,
}

fn workspace() -> &'static Workspace {
    static READ: OnceLock<Workspace> = OnceLock::new();
    READ.get_or_init(|| read_workspace(Path::new(ROOT)))
}

fn metadata(dir: &Path, extra: &[&str]) -> serde_json::Value {
    let out = relift_proc::command(env!("CARGO"))
        .args(["metadata", "--format-version", "1"])
        .args(extra)
        .current_dir(dir)
        .output()
        .expect("cargo metadata runs");
    assert!(
        out.status.success(),
        "cargo metadata failed in {}:\n{}",
        dir.display(),
        String::from_utf8_lossy(&out.stderr)
    );
    serde_json::from_slice(&out.stdout).expect("cargo metadata emits JSON")
}

fn manifest_dir(p: &serde_json::Value) -> PathBuf {
    PathBuf::from(
        p["manifest_path"]
            .as_str()
            .expect("a package has a manifest path"),
    )
    .parent()
    .expect("a manifest sits in a directory")
    .to_path_buf()
}

fn relative(root: &Path, dir: &Path) -> Option<String> {
    dir.strip_prefix(root)
        .ok()
        .map(|r| r.to_string_lossy().replace('\\', "/"))
}

fn read_workspace(root: &Path) -> Workspace {
    let root = std::fs::canonicalize(root).expect("the workspace root resolves");
    let meta = metadata(&root, &[]);
    let member_ids: BTreeSet<&str> = meta["workspace_members"]
        .as_array()
        .expect("metadata carries workspace_members")
        .iter()
        .map(|id| id.as_str().expect("a member id is a string"))
        .collect();
    let mut members = BTreeMap::new();
    let mut packages = BTreeMap::new();
    for p in meta["packages"]
        .as_array()
        .expect("metadata carries packages")
    {
        let id = p["id"].as_str().expect("a package has an id").to_string();
        let name = p["name"]
            .as_str()
            .expect("a package has a name")
            .to_string();
        let dir = manifest_dir(p);
        packages.insert(id.clone(), (name.clone(), dir.clone()));
        if !member_ids.contains(id.as_str()) {
            continue;
        }
        let rel = relative(&root, &dir)
            .unwrap_or_else(|| panic!("member {name} sits outside the root at {}", dir.display()));
        let declared = p["dependencies"]
            .as_array()
            .expect("a package carries dependencies")
            .iter()
            .map(|d| {
                (
                    Kind::parse(&d["kind"]),
                    d["name"]
                        .as_str()
                        .expect("a dependency has a name")
                        .to_string(),
                )
            })
            .collect();
        members.insert(
            id,
            Member {
                name,
                folder: Folder::of(&rel),
                rel,
                declared,
            },
        );
    }
    let resolved = meta["resolve"]["nodes"]
        .as_array()
        .expect("metadata carries a resolve")
        .iter()
        .map(|n| {
            let deps = n["deps"]
                .as_array()
                .expect("a node carries deps")
                .iter()
                .map(|d| {
                    let kinds = d["dep_kinds"]
                        .as_array()
                        .expect("a dep carries dep_kinds")
                        .iter()
                        .map(|k| Kind::parse(&k["kind"]))
                        .collect();
                    (
                        d["pkg"].as_str().expect("a dep has a pkg id").to_string(),
                        kinds,
                    )
                })
                .collect();
            (
                n["id"].as_str().expect("a node has an id").to_string(),
                deps,
            )
        })
        .collect();
    let manifest =
        std::fs::read_to_string(root.join("Cargo.toml")).expect("the root manifest reads");
    let exclude = manifest
        .lines()
        .find_map(|l| l.trim().strip_prefix("exclude = "))
        .map(|list| {
            list.trim_matches(|c| c == '[' || c == ']')
                .split(',')
                .map(|s| s.trim().trim_matches('"').to_string())
                .filter(|s| !s.is_empty())
                .collect()
        })
        .unwrap_or_default();
    Workspace {
        root,
        members,
        packages,
        resolved,
        exclude,
    }
}

/// A route for a failure message: `a -> (normal) b -> (dev) c`.
fn render(ws: &Workspace, from: &str, route: &[(Kind, String)]) -> String {
    let mut out = ws.packages[from].0.clone();
    for (kind, id) in route {
        out.push_str(&format!(" -> ({}) {}", kind.label(), ws.packages[id].0));
    }
    out
}

impl Workspace {
    fn id_of(&self, name: &str) -> &str {
        self.members
            .iter()
            .find(|(_, m)| m.name == name)
            .map(|(id, _)| id.as_str())
            .unwrap_or_else(|| {
                panic!("{name} is not a workspace member, so no rule about it reads anything")
            })
    }

    fn member(&self, name: &str) -> &Member {
        &self.members[self.id_of(name)]
    }

    /// Every package the resolved graph reaches from `from`, each mapped to
    /// one shortest route. The first hop may be of any kind; later hops must
    /// be normal or build, since Cargo does not make dev-dependencies
    /// transitive.
    fn closure(&self, from: &str) -> BTreeMap<String, Vec<(Kind, String)>> {
        let mut routes: BTreeMap<String, Vec<(Kind, String)>> = BTreeMap::new();
        let mut queue = VecDeque::from([from.to_string()]);
        while let Some(node) = queue.pop_front() {
            let here = routes.get(&node).cloned().unwrap_or_default();
            for (dep, kinds) in self.resolved.get(&node).into_iter().flatten() {
                let kind = if node == from {
                    kinds.iter().next().copied()
                } else {
                    kinds.iter().copied().find(|k| *k != Kind::Dev)
                };
                let Some(kind) = kind else { continue };
                if dep == from || routes.contains_key(dep) {
                    continue;
                }
                let mut route = here.clone();
                route.push((kind, dep.clone()));
                routes.insert(dep.clone(), route);
                queue.push_back(dep.clone());
            }
        }
        routes
    }

    /// Every package `from` links into a build of its own (normal and build
    /// edges only, at every hop), so what its tests compile is left out.
    fn linked(&self, from: &str) -> BTreeSet<String> {
        let mut seen = BTreeSet::new();
        let mut queue = VecDeque::from([from.to_string()]);
        while let Some(node) = queue.pop_front() {
            for (dep, kinds) in self.resolved.get(&node).into_iter().flatten() {
                if kinds.iter().any(|k| *k != Kind::Dev) && seen.insert(dep.clone()) {
                    queue.push_back(dep.clone());
                }
            }
        }
        seen
    }

    /// The members of `closure` and the routes to them.
    fn members_reached(&self, from: &str) -> Vec<(&Member, Vec<(Kind, String)>)> {
        self.closure(from)
            .into_iter()
            .filter_map(|(id, route)| self.members.get(&id).map(|m| (m, route)))
            .collect()
    }

    fn in_folder(&self, pick: impl Fn(&Folder) -> bool) -> Vec<(&str, &Member)> {
        self.members
            .iter()
            .filter(|(_, m)| pick(&m.folder))
            .map(|(id, m)| (id.as_str(), m))
            .collect()
    }
}

fn charter(member: &str) -> &'static Charter {
    CHARTERS
        .iter()
        .find(|c| c.member == member)
        .unwrap_or_else(|| panic!("no charter row for {member}"))
}

/// Fails with every broken rule at once, not just the first.
fn report(what: &str, broken: Vec<String>) {
    assert!(
        broken.is_empty(),
        "{what}: {} broken\n{}",
        broken.len(),
        broken.join("\n")
    );
}

/// Every member has exactly one row and every row names a member, so a new
/// crate cannot join the workspace without a written ceiling.
#[test]
fn every_member_has_a_charter_row() {
    let ws = workspace();
    let members: BTreeSet<&str> = ws.members.values().map(|m| m.name.as_str()).collect();
    let rows: BTreeSet<&str> = CHARTERS.iter().map(|c| c.member).collect();
    assert_eq!(rows.len(), CHARTERS.len(), "a member has two charter rows");
    assert_eq!(
        members, rows,
        "the workspace members and the charter rows differ; add or strike a row"
    );
}

/// Every member declares exactly what its row lists, per kind.
#[test]
fn every_declared_ceiling_holds() {
    let ws = workspace();
    let mut broken = Vec::new();
    for m in ws.members.values() {
        let row = charter(&m.name);
        for (kind, allowed) in [
            (Kind::Normal, row.normal),
            (Kind::Dev, row.dev),
            (Kind::Build, row.build),
        ] {
            let have: BTreeSet<&str> = m
                .declared
                .iter()
                .filter(|(k, _)| *k == kind)
                .map(|(_, n)| n.as_str())
                .collect();
            let want: BTreeSet<&str> = allowed.iter().copied().collect();
            for extra in have.difference(&want) {
                broken.push(format!(
                    "{} declares {} dependency {extra}, which its row does not list ({})",
                    m.name,
                    kind.label(),
                    row.why
                ));
            }
            for missing in want.difference(&have) {
                broken.push(format!(
                    "{}'s row lists {} dependency {missing}, which it no longer declares; strike it",
                    m.name,
                    kind.label()
                ));
            }
        }
    }
    report("dependency ceilings", broken);
}

/// No edge between members, of any kind, points to a higher band.
#[test]
fn the_band_order_holds() {
    let ws = workspace();
    let names: BTreeSet<&str> = ws.members.values().map(|m| m.name.as_str()).collect();
    let mut broken = Vec::new();
    let mut edges = 0;
    for m in ws.members.values() {
        let band = charter(&m.name).band;
        for (kind, dep) in &m.declared {
            if !names.contains(dep.as_str()) {
                continue;
            }
            edges += 1;
            let dep_band = charter(dep).band;
            if dep_band > band {
                broken.push(format!(
                    "{} (band {band}) declares {} dependency {dep} (band {dep_band}); nothing in a lower band may depend on a higher one",
                    m.name,
                    kind.label()
                ));
            }
        }
    }
    assert!(
        edges >= 20,
        "control: only {edges} edges between members, too few to be this workspace"
    );
    report("band order", broken);
}

/// No member depends on a generator: generated code is committed, so a
/// build never runs one.
#[test]
fn no_member_reaches_a_generator() {
    let ws = workspace();
    let gens = ws.in_folder(|f| *f == Folder::Generator);
    assert!(
        !gens.is_empty(),
        "control: no member under cpus/*/gen, so this rule checks nothing"
    );
    for (id, m) in &gens {
        assert!(
            ws.members_reached(id)
                .iter()
                .any(|(r, _)| r.folder == Folder::Cpus),
            "control: {} reaches no cpus/ member, so the walk cannot be trusted",
            m.name
        );
    }
    let all = ws.in_folder(|_| true);
    let mut broken = Vec::new();
    for (id, m) in &all {
        for (reached, route) in ws.members_reached(id) {
            if reached.folder == Folder::Generator {
                broken.push(format!(
                    "{} reaches {}: {}",
                    m.name,
                    reached.rel,
                    render(ws, id, &route)
                ));
            }
        }
    }
    report("generators", broken);
}

/// Nothing under `core/` reaches anything under `cpus/`, `guests/` or
/// `testguest/` by any route: the core knows no CPU and no guest.
#[test]
fn core_reaches_no_cpu_and_no_guest() {
    let ws = workspace();
    let needles = ws.in_folder(|f| *f == Folder::Cpus || f.is_guest());
    assert!(
        needles.iter().any(|(_, m)| m.folder == Folder::Cpus)
            && needles
                .iter()
                .any(|(_, m)| matches!(m.folder, Folder::Guest(_)))
            && needles.iter().any(|(_, m)| m.folder == Folder::TestGuest),
        "control: cpus/, guests/ and testguest/ must each hold a member, or this rule checks nothing"
    );
    let from_binary = ws.closure(ws.id_of("relift"));
    for (id, m) in needles
        .iter()
        .filter(|(_, m)| !STANDALONE.contains(&m.name.as_str()))
    {
        assert!(
            from_binary.contains_key(*id),
            "control: the walk from relift does not find {}, so it cannot be trusted to find it from core/",
            m.name
        );
    }
    let mut broken = Vec::new();
    let core = ws.in_folder(|f| *f == Folder::Core);
    assert!(
        core.len() >= 8,
        "control: only {} members under core/",
        core.len()
    );
    for (id, m) in &core {
        for (reached, route) in ws.members_reached(id) {
            if reached.folder == Folder::Cpus || reached.folder.is_guest() {
                broken.push(format!(
                    "{} reaches {}: {}",
                    m.name,
                    reached.rel,
                    render(ws, id, &route)
                ));
            }
        }
    }
    report("core/ reaches no CPU and no guest", broken);
}

/// `cpus/` crates reach only `core/ir` and `core/guest` among the members;
/// a crate in a folder under `cpus/<cpu>/` (the generator aside) may also
/// reach its own frontend `cpus/<cpu>`, and no other CPU's.
#[test]
fn cpus_reach_only_ir_and_guest() {
    let ws = workspace();
    let allowed = ["core/ir", "core/guest"];
    let cpus = ws.in_folder(|f| *f == Folder::Cpus);
    assert!(!cpus.is_empty(), "control: no member under cpus/");
    let frontend = |rel: &str| rel.split('/').take(2).collect::<Vec<_>>().join("/");
    let parts: Vec<_> = cpus
        .iter()
        .filter(|(_, m)| m.rel != frontend(&m.rel))
        .collect();
    assert!(
        !parts.is_empty(),
        "control: no member in a folder under cpus/<cpu>/, so the frontend exception is never exercised"
    );
    let mut broken = Vec::new();
    for (id, m) in &cpus {
        let own = frontend(&m.rel);
        let part = m.rel != own;
        let reached: Vec<_> = ws.members_reached(id);
        if part {
            assert!(
                reached.iter().any(|(r, _)| r.rel == own),
                "control: {} does not reach its frontend {own}, so the exception is not needed",
                m.name
            );
        }
        for want in allowed
            .iter()
            .filter(|_| !part && !STANDALONE.contains(&m.name.as_str()))
        {
            assert!(
                reached.iter().any(|(r, _)| r.rel == *want),
                "control: {} does not reach {want}, so the walk from it may be empty",
                m.name
            );
        }
        for (r, route) in reached {
            if !allowed.contains(&r.rel.as_str()) && !(part && r.rel == own) {
                broken.push(format!(
                    "{} reaches {}: {}",
                    m.name,
                    r.rel,
                    render(ws, id, &route)
                ));
            }
        }
    }
    report(
        "cpus/ reach only core/ir, core/guest and their own frontend",
        broken,
    );
}

/// No guest reaches another guest by any route. Each `guests/<name>/` is
/// one guest, and `testguest/` is another.
#[test]
fn no_guest_reaches_another_guest() {
    let ws = workspace();
    let guests = ws.in_folder(Folder::is_guest);
    let distinct: BTreeSet<&Folder> = guests.iter().map(|(_, m)| &m.folder).collect();
    assert!(
        distinct.len() >= 2,
        "control: {} guest(s), so no pair can be checked",
        distinct.len()
    );
    let switch = ws.members_reached(ws.id_of("relift-switch"));
    assert!(
        switch
            .iter()
            .any(|(m, _)| m.name == "relift-switch-formats"),
        "control: relift-switch does not reach its own formats crate, so the walk sees no guest member"
    );
    let mut broken = Vec::new();
    for (id, m) in &guests {
        for (reached, route) in ws.members_reached(id) {
            if reached.folder.is_guest() && reached.folder != m.folder {
                broken.push(format!(
                    "{} reaches {}: {}",
                    m.name,
                    reached.rel,
                    render(ws, id, &route)
                ));
            }
        }
    }
    report("no guest reaches another guest", broken);
}

/// `override/` crates reach nothing in the tool: game repos link them, and
/// they must not drag the recompiler along.
#[test]
fn override_reaches_no_tool_crate() {
    let ws = workspace();
    let overrides = ws.in_folder(|f| *f == Folder::Override);
    assert!(
        overrides.len() >= 2,
        "control: fewer than two members under override/"
    );
    let api = ws.members_reached(ws.id_of("relift-override"));
    assert!(
        api.iter().any(|(m, _)| m.name == "relift-override-macros"),
        "control: relift-override does not reach relift-override-macros, so the walk is empty"
    );
    let mut broken = Vec::new();
    for (id, m) in &overrides {
        for (reached, route) in ws.members_reached(id) {
            if reached.folder != Folder::Override {
                broken.push(format!(
                    "{} reaches {}: {}",
                    m.name,
                    reached.rel,
                    render(ws, id, &route)
                ));
            }
        }
    }
    report("override/ reaches no tool crate", broken);
}

/// Nothing in this workspace reaches `check/`, which links a GPL-2
/// reference emulator and must stay its own workspace.
#[test]
fn nothing_reaches_check() {
    let ws = workspace();
    let check = ws.root.join("check");
    let check_meta = metadata(&check, &["--no-deps"]);
    let check_pkgs: Vec<String> = check_meta["packages"]
        .as_array()
        .expect("check/ metadata carries packages")
        .iter()
        .map(|p| p["name"].as_str().expect("a name").to_string())
        .collect();
    assert_eq!(
        check_pkgs,
        ["relift-check"],
        "control: check/ must be its own workspace holding relift-check"
    );
    assert!(
        relative(
            &ws.root,
            Path::new(
                check_meta["workspace_root"]
                    .as_str()
                    .expect("a workspace root")
            )
        )
        .is_some_and(|r| r == "check"),
        "check/ is not its own workspace root"
    );
    assert!(
        ws.exclude.iter().any(|e| e == "check"),
        "the root manifest no longer excludes check/: {:?}",
        ws.exclude
    );
    let mut broken = Vec::new();
    for (id, (name, dir)) in &ws.packages {
        if name == "relift-check" || dir.starts_with(&check) {
            broken.push(format!(
                "{name} ({id}) is in this workspace's graph from {}",
                dir.display()
            ));
        }
    }
    assert!(
        ws.packages.len() > ws.members.len(),
        "control: the graph lists only members, so registry packages were not read"
    );
    report("nothing reaches check/", broken);
}

/// The separate tool that converts encrypted dumps. relift has no dependency
/// on it in either direction, so none of its crates may be in the graph.
const SEPARATE_TOOL_CRATES: &[&str] = &["switch-formats", "switch-io", "switch-tools"];

/// The decrypted-only policy, in the graph: no crate of the separate
/// conversion tool is a dependency of any kind, by name or by manifest
/// folder. Crypto crates are not banned: a crate such as `aes` may enter
/// where its charter row states why (guest crypto instructions, SDK
/// `nn::crypto` replacements, save data hashing). The source rules are in
/// `decrypted_only.rs`; the refusal rules are in the import tests. Control:
/// the RustCrypto hash crates the formats use (`sha2` and its `digest`) are
/// found, and so is a member whose name ends the same way as a banned one
/// (`relift-switch-formats`), so the list matches whole names only.
#[test]
fn no_crate_of_the_separate_tool_is_in_the_graph() {
    let ws = workspace();
    let names: BTreeSet<&str> = ws.packages.values().map(|(n, _)| n.as_str()).collect();
    for control in ["sha2", "digest", "relift-switch-formats"] {
        assert!(
            names.contains(control),
            "control: {control} is not in the graph, so the reader may not see its packages"
        );
    }
    let mut broken = Vec::new();
    for (id, (name, dir)) in &ws.packages {
        if SEPARATE_TOOL_CRATES.contains(&name.as_str()) {
            broken.push(format!("{name} ({id}) is in the graph"));
        }
        if dir
            .components()
            .any(|c| c.as_os_str().to_string_lossy() == "switch-tools")
        {
            broken.push(format!("{name} ({id}) lives in {}", dir.display()));
        }
    }
    for member in ws.members.values() {
        for (kind, dep) in &member.declared {
            if SEPARATE_TOOL_CRATES.contains(&dep.as_str()) {
                broken.push(format!("{} declares {dep} ({})", member.name, kind.label()));
            }
        }
    }
    report("no crate of the separate tool in the graph", broken);
}

/// The reader reads this tree and the walk recurses: the controls every
/// zero above rests on.
#[test]
fn the_reader_reads_this_workspace() {
    let ws = workspace();
    assert!(
        ws.members.len() >= 16,
        "control: cargo metadata listed {} members, too few to be this workspace",
        ws.members.len()
    );
    let switch = ws.id_of("relift-switch");
    let cache = ws.id_of("relift-cache");
    let route = ws
        .closure(switch)
        .remove(cache)
        .expect("control: relift-switch reaches relift-cache through relift-switch-formats");
    assert!(
        route.len() > 1,
        "control: the walk reached relift-cache from relift-switch in one hop, so it reads declared edges, not the closure: {}",
        render(ws, switch, &route)
    );
    assert!(
        !ws.member("relift-switch")
            .declared
            .iter()
            .any(|(_, n)| n == "relift-cache"),
        "control: relift-switch now declares relift-cache directly; point this control at another indirect edge"
    );
    let ir = ws.id_of("relift-ir");
    assert!(
        ws.members_reached(ws.id_of("relift-analyze"))
            .iter()
            .any(|(m, _)| m.name == "relift-ir")
            && ws.closure(ir).keys().all(|k| !ws.members.contains_key(k)),
        "control: relift-analyze must reach relift-ir, and relift-ir must reach no member"
    );
}

/// The packages only a test may link: a WebAssembly runtime is how
/// `relift-emit-wasm` checks its modules, never part of a shipped build.
const TEST_ONLY: &[&str] = &["wasmtime", "cranelift-codegen"];

/// No member links a [`TEST_ONLY`] package through normal or build edges.
/// Control: `relift-emit-wasm`'s tests do reach `wasmtime`, and its own
/// build does reach `wasm-encoder`, so the walk sees both kinds of edge.
#[test]
fn test_only_packages_stay_out_of_every_build() {
    let ws = workspace();
    let name = |id: &String| ws.packages[id].0.clone();
    let wasm = ws.id_of("relift-emit-wasm");
    let tests: BTreeSet<String> = ws.closure(wasm).keys().map(name).collect();
    assert!(
        TEST_ONLY.iter().all(|t| tests.contains(*t)),
        "control: relift-emit-wasm's tests no longer reach {TEST_ONLY:?}; point this control elsewhere"
    );
    assert!(
        ws.linked(wasm)
            .iter()
            .map(name)
            .any(|n| n == "wasm-encoder"),
        "control: relift-emit-wasm's build does not reach wasm-encoder"
    );
    let mut broken = Vec::new();
    for (id, m) in &ws.members {
        for dep in ws.linked(id) {
            let n = name(&dep);
            if TEST_ONLY.contains(&n.as_str()) {
                broken.push(format!("{} links {n}", m.name));
            }
        }
    }
    report("test-only packages", broken);
}
