use relift_gpu_sm86::program::Program;
use relift_gpu_sm86::translate::translate;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let b = std::fs::read(&args[1]).unwrap_or_else(|e| panic!("{}: {e}", args[1]));
    let p = Program::parse(&b);
    match translate(&p) {
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
