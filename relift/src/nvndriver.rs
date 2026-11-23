//! The NVN driver facts a Switch or Switch 2 build hands the runtime (see
//! `relift_switch::nvndriver`): the driver's version, which of its procs
//! are pure enough to run as the game's own lifted code, and what to write
//! into device and memory pool objects before the runtime runs the driver's
//! own texture code. The build writes them to `c/nvn_driver.c`, which
//! registers them before `main`.

use std::fmt::Write as _;
use std::path::{Path, PathBuf};

use relift_cpu_arm64::frontend::Arm64;
use relift_switch::nvndriver::{
    DeviceLayout, Field, Fill, Image, Integer, PoolLayout, device_integers, device_layout,
    proc_table, purity,
};
use relift_switch::program::Placed;

use crate::import::BoxError;

/// The NVN API generation of a driver.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Generation {
    /// NVN1: the Switch's API, on the Maxwell GM20B.
    Nvn1,
    /// NVN2: the Switch 2's API, on its Ampere GPU.
    Nvn2,
}

impl Generation {
    /// The generation a driver that answers `major` for
    /// `NVN_DEVICE_INFO_API_MAJOR_VERSION` implements, if relift knows it:
    /// NVN1 drivers of SDK 4.5 to 16.2 answer 53 or 55, NVN2 drivers of SDK
    /// 20 to 22 answer 231.
    pub fn of_major(major: i32) -> Option<Generation> {
        match major {
            53 | 55 => Some(Generation::Nvn1),
            231 => Some(Generation::Nvn2),
            _ => None,
        }
    }

    fn number(self) -> u32 {
        match self {
            Generation::Nvn1 => 1,
            Generation::Nvn2 => 2,
        }
    }
}

/// What the build read from a program's NVN driver.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Driver {
    /// Its API generation.
    pub generation: Generation,
    /// Its `NVN_DEVICE_INFO_API_MAJOR_VERSION` and `_MINOR_VERSION`.
    pub api: (i32, i32),
    /// The NVN SDK version its middleware string names (`16_2_0` in
    /// `SDK MW+...+NintendoSDK_NVN-16_2_0-Release`), if it has one.
    pub sdk: Option<String>,
    /// The device layout, or why it could not be read.
    pub layout: Result<DeviceLayout, String>,
    /// The procs whose own code is not pure (see
    /// [`relift_switch::nvndriver::purity`]) and the first reason.
    pub impure: Vec<(String, String)>,
}

/// The NVN SDK version the middleware string of `img` names.
fn sdk_version(img: &[u8]) -> Option<String> {
    const TAG: &[u8] = b"NintendoSDK_NVN-";
    let at = img.windows(TAG.len()).position(|w| w == TAG)? + TAG.len();
    let rest = &img[at..];
    let end = rest.iter().position(|&b| b == b'-' || b == 0)?;
    let v = std::str::from_utf8(&rest[..end]).ok()?;
    (!v.is_empty() && v.bytes().all(|b| b.is_ascii_digit() || b == b'_'))
        .then(|| v.replace('_', "."))
}

/// Reads the driver in `image` (the module image `img`).
///
/// # Errors
///
/// No proc table, a `nvnDeviceGetInteger` this cannot read, or an API
/// major version relift does not know.
pub fn read(image: &Image, img: &[u8]) -> Result<Driver, String> {
    let procs = proc_table(image)?;
    let layout = device_layout(image);
    let integers = match &layout {
        Ok(l) => l.integers.clone(),
        Err(_) => device_integers(image)?,
    };
    let api = match integers.as_slice() {
        [Integer::Const(major), Integer::Const(minor), ..] => (*major, *minor),
        _ => return Err("nvn driver: nvnDeviceGetInteger answers no constant API version".into()),
    };
    let generation = Generation::of_major(api.0).ok_or_else(|| {
        format!(
            "nvn driver: API version {}.{} is of no NVN generation relift knows (NVN1: 53 and 55, NVN2: 231)",
            api.0, api.1
        )
    })?;
    let layout = match layout {
        Ok(DeviceLayout { pool: Err(e), .. }) if generation == Generation::Nvn2 => Err(e),
        l => l,
    };
    Ok(Driver {
        generation,
        api,
        sdk: sdk_version(img),
        layout,
        impure: purity(image, &procs),
    })
}

/// Reads the driver in the module of `placed` that defines
/// `nvnBootstrapLoaderInternal` and writes `c/nvn_driver.c` in `cdir`; its
/// path and the driver, or none when no module defines it.
///
/// # Errors
///
/// As [`read`], an import `nvnDeviceGetInteger` follows that the runtime
/// does not, or the file cannot be written.
pub fn write(
    placed: &[Placed],
    cpu: Arm64,
    cdir: &Path,
) -> Result<Option<(PathBuf, Driver)>, BoxError> {
    let Some(sdk) = placed.iter().find(|p| {
        p.nso.symbols().is_ok_and(|s| {
            s.iter()
                .any(|s| s.is_defined() && s.name == "nvnBootstrapLoaderInternal")
        })
    }) else {
        return Ok(None);
    };
    let image = Image::new(&sdk.nso, cpu)?;
    let d = read(&image, &sdk.nso.image)?;
    if let Ok(l) = &d.layout
        && let Some(n) = l.integers.iter().find_map(|i| match i {
            Integer::Import { name, .. } if name != "NvRmGpuDeviceGetInfo" => Some(name),
            _ => None,
        })
    {
        return Err(format!(
            "nvn driver: nvnDeviceGetInteger answers what {n} returns; the runtime follows only NvRmGpuDeviceGetInfo"
        )
        .into());
    }
    let path = cdir.join("nvn_driver.c");
    relift_cache::write_if_changed(&path, c_text(&d, sdk.base).as_bytes())?;
    Ok(Some((path, d)))
}

/// `v`, or the runtime's mark for none.
fn or_none(v: Option<u32>) -> String {
    v.map_or_else(|| "0xffffffffu".into(), |v| format!("0x{v:x}"))
}

/// The C file for `d`, whose image offsets are in a module loaded at `base`.
/// It also hands the runtime the translator stamp ([`crate::shaders::STAMP`])
/// the program expects its shader tables to carry.
pub fn c_text(d: &Driver, base: u64) -> String {
    let mut s = String::from(
        "#include <stdint.h>\n\
         struct rl_nvn_field { uint32_t off, len; const uint8_t *bytes; int32_t info; };\n\
         struct rl_nvn_layout {\n    uint64_t storage_class, settings_hook;\n    \
         uint32_t builder_device, flags, gpu_device, nfields, nsettings;\n    \
         const struct rl_nvn_field *fields;\n    const char *const *settings;\n    \
         uint32_t nints;\n    const int32_t *ints;\n    const uint8_t *int_device;\n    \
         uint32_t pool_page_info;\n    \
         uint32_t pool_device, pool_flags, pool_gpu, pool_memory, pool_size, npool_virtual, npool_fields;\n    \
         const struct rl_nvn_field *pool_virtual, *pool_fields;\n    \
         uint32_t npool_ranges, npool_mappings;\n    const uint32_t *pool_ranges, *pool_mappings;\n};\n\
         struct rl_nvn_driver {\n    uint32_t generation;\n    int32_t api_major, api_minor;\n    \
         const char *sdk;\n    const struct rl_nvn_layout *layout;\n    const char *layout_error;\n    \
         uint32_t nimpure;\n    const char *const *impure;\n};\n\
         void rl_nvn_set_driver(const struct rl_nvn_driver *d);\n\
         void rl_nvn_set_table_stamp(const char *stamp);\n",
    );
    if let Ok(l) = &d.layout {
        layout_text(&mut s, l, base);
    }
    s.push_str("static const char *const impure[] = {\n");
    for (n, why) in &d.impure {
        let _ = writeln!(
            s,
            "    {}, {},",
            crate::shaders::c_string(n),
            crate::shaders::c_string(why)
        );
    }
    let _ = write!(
        s,
        "    0,\n}};\n\
         static const struct rl_nvn_driver driver = {{ {}, {}, {}, {}, {}, {}, {}, impure }};\n\
         __attribute__((constructor)) static void rl_nvn_driver_init(void) {{ rl_nvn_set_driver(&driver); rl_nvn_set_table_stamp({}); }}\n",
        d.generation.number(),
        d.api.0,
        d.api.1,
        d.sdk
            .as_deref()
            .map_or_else(|| "0".into(), crate::shaders::c_string),
        if d.layout.is_ok() { "&layout" } else { "0" },
        d.layout
            .as_ref()
            .err()
            .map_or_else(|| "0".into(), |e| crate::shaders::c_string(e)),
        d.impure.len(),
        crate::shaders::c_string(crate::shaders::STAMP),
    );
    s
}

fn layout_text(s: &mut String, l: &DeviceLayout, base: u64) {
    c_fields(s, "fields", &l.fields);
    let empty = PoolLayout::default();
    let pool = l.pool.as_ref().unwrap_or(&empty);
    c_fields(s, "pool_virtual", &pool.virtual_fields);
    c_fields(s, "pool_fields", &pool.fields);
    c_offsets(s, "pool_ranges", &pool.ranges);
    c_offsets(s, "pool_mappings", &pool.mappings);
    s.push_str("static const char *const settings[] = {\n");
    for k in &l.settings {
        let _ = writeln!(s, "    {},", crate::shaders::c_string(k));
    }
    let ints: Vec<String> = l
        .integers
        .iter()
        .map(|i| match i {
            Integer::Const(v) => v.to_string(),
            Integer::Device(_) => "0".into(),
            Integer::Import { off, .. } => off.to_string(),
        })
        .collect();
    let device: Vec<&str> = l
        .integers
        .iter()
        .map(|i| match i {
            Integer::Const(_) => "0",
            Integer::Device(_) => "1",
            Integer::Import { .. } => "2",
        })
        .collect();
    let _ = write!(
        s,
        "    0,\n}};\nstatic const int32_t ints[] = {{ {}, 0 }};\nstatic const uint8_t int_device[] = {{ {}, 0 }};\n\
         static const struct rl_nvn_layout layout = {{ 0x{:x}, 0x{:x}, 0x{:x}, {}, {}, {}, {}, fields, settings, {}, ints, int_device, {}, \
         0x{:x}, 0x{:x}, 0x{:x}, 0x{:x}, 0x{:x}, {}, {}, {}, {}, {}, {}, pool_ranges, pool_mappings }};\n",
        ints.join(", "),
        device.join(", "),
        base + l.storage_class,
        l.settings_hook.map_or(0, |h| base + h),
        l.builder_device,
        or_none(l.flags),
        or_none(l.gpu_device),
        l.fields.len(),
        l.settings.len(),
        l.integers.len(),
        or_none(l.pool_page_info),
        pool.device,
        pool.flags,
        pool.gpu,
        pool.memory,
        pool.size,
        pool.virtual_fields.len(),
        pool.fields.len(),
        if l.pool.is_ok() { "pool_virtual" } else { "0" },
        if l.pool.is_ok() { "pool_fields" } else { "0" },
        pool.ranges.len(),
        pool.mappings.len(),
    );
}

/// Writes the table `name` of the field offsets `offs`, ending in a 0 that
/// keeps it non-empty.
fn c_offsets(s: &mut String, name: &str, offs: &[u32]) {
    let o: Vec<String> = offs.iter().map(|o| format!("0x{o:x}, ")).collect();
    let _ = writeln!(s, "static const uint32_t {name}[] = {{ {}0 }};", o.concat());
}

/// Writes the table `name` of `fields`, ending in an empty entry, and the
/// byte arrays its entries point to.
fn c_fields(s: &mut String, name: &str, fields: &[Field]) {
    for (i, f) in fields.iter().enumerate() {
        if let Fill::Bytes(b) = &f.fill {
            let bytes: Vec<String> = b.iter().map(|b| format!("0x{b:02x}")).collect();
            let _ = writeln!(
                s,
                "static const uint8_t {name}{i}[] = {{ {} }};",
                bytes.join(", ")
            );
        }
    }
    let _ = writeln!(s, "static const struct rl_nvn_field {name}[] = {{");
    for (i, f) in fields.iter().enumerate() {
        let _ = match &f.fill {
            Fill::Bytes(b) => writeln!(s, "    {{ 0x{:x}, {}, {name}{i}, -1 }},", f.off, b.len()),
            Fill::Info(at) => writeln!(s, "    {{ 0x{:x}, 4, 0, {at} }},", f.off),
        };
    }
    s.push_str("    { 0, 0, 0, -1 },\n};\n");
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_sdk_version_is_the_nvn_middleware_strings() {
        let img = b"\0SDK MW+Nvidia+NintendoSDK_NVN-16_2_12-Release\0";
        assert_eq!(sdk_version(img).as_deref(), Some("16.2.12"));
        assert_eq!(sdk_version(b"NintendoSDK_libz-16_2_0-Release"), None);
    }

    #[test]
    fn generations_are_named_by_the_api_major_version() {
        assert_eq!(Generation::of_major(53), Some(Generation::Nvn1));
        assert_eq!(Generation::of_major(55), Some(Generation::Nvn1));
        assert_eq!(Generation::of_major(231), Some(Generation::Nvn2));
        assert_eq!(Generation::of_major(54), None);
    }

    #[test]
    fn the_c_file_registers_the_version_without_a_layout() {
        let d = Driver {
            generation: Generation::Nvn1,
            api: (55, 15),
            sdk: Some("16.2.0".into()),
            layout: Err("no layout".into()),
            impure: vec![("nvnA".into(), "calls memcmp".into())],
        };
        let c = c_text(&d, 0x1000);
        assert!(
            c.contains("driver = { 1, 55, 15, \"16.2.0\", 0, \"no layout\", 1, impure }"),
            "{c}"
        );
        assert!(c.contains("\"nvnA\", \"calls memcmp\","), "{c}");
        assert!(!c.contains("layout = {"), "{c}");
    }

    #[test]
    fn the_c_file_hands_the_runtime_the_translator_stamp() {
        let d = Driver {
            generation: Generation::Nvn2,
            api: (231, 0),
            sdk: None,
            layout: Err("no layout".into()),
            impure: Vec::new(),
        };
        let c = c_text(&d, 0x1000);
        let want = format!("rl_nvn_set_table_stamp(\"{}\");", crate::shaders::STAMP);
        assert!(c.contains(&want), "{c}");
        assert_eq!(crate::shaders::STAMP.len(), 16);
    }
}
