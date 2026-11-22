use relift_gpu_nv::translate::{HostProfile, Options, translate_with};

fn main() {
    let mut o = Options::default();
    let mut no_fma = false;
    let mut flush_fix = None;
    let mut raw = std::env::args().peekable();
    let mut kept = Vec::new();
    while let Some(a) = raw.next() {
        if a == "--flush-fix" {
            flush_fix = raw.next().and_then(|v| v.parse::<u8>().ok());
        } else {
            kept.push(a);
        }
    }
    let args: Vec<String> = kept
        .into_iter()
        .filter(|a| {
            let flag = a == "--exact-mufu";
            o.exact_mufu |= flag;
            let unfused = a == "--no-fused-fma";
            no_fma |= unfused;
            !flag && !unfused
        })
        .collect();
    let b = std::fs::read(&args[1]).unwrap_or_else(|e| panic!("{}: {e}", args[1]));
    let p = relift_gpu_sm86::program::parse(&b);
    let host = match args.get(3) {
        Some(n) if n.ends_with(".json") => {
            let text = std::fs::read_to_string(n).unwrap_or_else(|e| panic!("{n}: {e}"));
            let caps = relift_gpu_nv::translate::DeviceCaps::from_json(&text)
                .unwrap_or_else(|e| panic!("{n}: {e}"));
            HostProfile::from_device("device", &caps)
        }
        Some(n) => HostProfile::preset(n).unwrap_or_else(|| panic!("no host preset {n}")),
        None => HostProfile::default(),
    };
    o.host = host;
    o.host.fused_fma &= !no_fma;
    if let Some(f) = flush_fix {
        o.host.denorm = relift_gpu_nv::translate::Denorm::Unknown;
        o.host.denorm_preserve_mode = false;
        o.host.flush_fix = Some(f);
    }
    match translate_with(&p, &o) {
        Ok(s) => {
            let bytes: Vec<u8> = s.spirv.iter().flat_map(|w| w.to_le_bytes()).collect();
            if let Some(out) = args.get(2) {
                relift_cache::write_if_changed(std::path::Path::new(out), &bytes)
                    .unwrap_or_else(|e| panic!("{out}: {e}"));
            }
            println!("ok {:?} {:?}", s.stage, s.interface);
        }
        Err(e) => println!("unsupported {e}"),
    }
}
