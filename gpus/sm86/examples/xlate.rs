use relift_gpu_sm86::program::Program;
use relift_gpu_sm86::translate::{HostProfile, Options, translate_with};

fn main() {
    let mut o = Options::default();
    let args: Vec<String> = std::env::args()
        .filter(|a| {
            let flag = a == "--exact-mufu";
            o.exact_mufu |= flag;
            !flag
        })
        .collect();
    let b = std::fs::read(&args[1]).unwrap_or_else(|e| panic!("{}: {e}", args[1]));
    let p = Program::parse(&b);
    let host = match args.get(3) {
        Some(n) => HostProfile::preset(n).unwrap_or_else(|| panic!("no host preset {n}")),
        None => HostProfile::default(),
    };
    o.host = host;
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
