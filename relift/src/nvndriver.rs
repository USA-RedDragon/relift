//! The NVN2 driver facts a Switch 2 build hands the runtime (see
//! `relift_switch::nvndriver`): what to write into a device object before
//! the runtime runs the driver's own `nvnTextureBuilderGetStorageClass`.
//! The build writes them to `c/nvn_driver.c`, which registers them before
//! `main`.

use std::fmt::Write as _;
use std::path::{Path, PathBuf};

use relift_cpu_arm64::frontend::Arm64;
use relift_switch::nvndriver::{DeviceLayout, Image, Integer, device_layout};
use relift_switch::program::Placed;

use crate::import::BoxError;

/// Reads the driver in the module of `placed` that defines
/// `nvnBootstrapLoaderInternal` and writes `c/nvn_driver.c` in `cdir`; its
/// path, or none when no module defines it.
///
/// # Errors
///
/// The driver's code has a shape the reading does not know (the message
/// names it), or the file cannot be written.
pub fn write(placed: &[Placed], cpu: Arm64, cdir: &Path) -> Result<Option<PathBuf>, BoxError> {
    let Some(sdk) = placed.iter().find(|p| {
        p.nso.symbols().is_ok_and(|s| {
            s.iter()
                .any(|s| s.is_defined() && s.name == "nvnBootstrapLoaderInternal")
        })
    }) else {
        return Ok(None);
    };
    let image = Image::new(&sdk.nso, cpu)?;
    let layout = device_layout(&image)?;
    if let Some(n) = layout.integers.iter().find_map(|i| match i {
        Integer::Import { name, .. } if name != "NvRmGpuDeviceGetInfo" => Some(name),
        _ => None,
    }) {
        return Err(format!(
            "nvn driver: nvnDeviceGetInteger answers what {n} returns; the runtime follows only NvRmGpuDeviceGetInfo"
        )
        .into());
    }
    let path = cdir.join("nvn_driver.c");
    relift_cache::write_if_changed(&path, c_text(&layout, sdk.base).as_bytes())?;
    Ok(Some(path))
}

/// The C file for `l`, whose image offsets are in a module loaded at `base`.
pub fn c_text(l: &DeviceLayout, base: u64) -> String {
    let mut s = String::from(
        "#include <stdint.h>\n\
         struct rl_nvn_field { uint32_t off, len; const uint8_t *bytes; };\n\
         struct rl_nvn_layout {\n    uint64_t storage_class, settings_hook;\n    \
         uint32_t builder_device, flags, gpu_device, nfields, nsettings;\n    \
         const struct rl_nvn_field *fields;\n    const char *const *settings;\n    \
         uint32_t nints;\n    const int32_t *ints;\n    const uint8_t *int_device;\n    \
         uint32_t pool_page_info;\n};\n\
         void rl_nvn_set_layout(const struct rl_nvn_layout *l);\n",
    );
    for (i, f) in l.fields.iter().enumerate() {
        let bytes: Vec<String> = f.bytes.iter().map(|b| format!("0x{b:02x}")).collect();
        let _ = writeln!(
            s,
            "static const uint8_t field{i}[] = {{ {} }};",
            bytes.join(", ")
        );
    }
    s.push_str("static const struct rl_nvn_field fields[] = {\n");
    for (i, f) in l.fields.iter().enumerate() {
        let _ = writeln!(s, "    {{ 0x{:x}, {}, field{i} }},", f.off, f.bytes.len());
    }
    s.push_str("    { 0, 0, 0 },\n};\nstatic const char *const settings[] = {\n");
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
         static const struct rl_nvn_layout layout = {{ 0x{:x}, 0x{:x}, 0x{:x}, 0x{:x}, 0x{:x}, {}, {}, fields, settings, {}, ints, int_device, {} }};\n\
         __attribute__((constructor)) static void rl_nvn_layout_init(void) {{ rl_nvn_set_layout(&layout); }}\n",
        ints.join(", "),
        device.join(", "),
        base + l.storage_class,
        l.settings_hook.map_or(0, |h| base + h),
        l.builder_device,
        l.flags,
        l.gpu_device,
        l.fields.len(),
        l.settings.len(),
        l.integers.len(),
        l.pool_page_info,
    );
    s
}
