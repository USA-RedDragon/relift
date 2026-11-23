//! Prints the Maxwell text of each program file given, one instruction per
//! line with its instruction index; with `--stops` first, only the forms
//! of each program that do not lower, one line each.
fn main() {
    let mut args: Vec<String> = std::env::args().skip(1).collect();
    let stops = args.first().is_some_and(|a| a == "--stops");
    if stops {
        args.remove(0);
    }
    for path in args {
        let b = std::fs::read(&path).unwrap_or_else(|e| panic!("{path}: {e}"));
        // a code header, or (scan files) the SPH and code alone
        let mut hdr = vec![0u8; relift_gpu_sm53::program::CODE_HEADER_LEN];
        hdr[..4].copy_from_slice(&relift_gpu_sm53::program::CODE_MAGIC.to_le_bytes());
        hdr.extend_from_slice(&b);
        let p = match relift_gpu_sm53::Program::parse(&b)
            .or_else(|_| relift_gpu_sm53::Program::parse(&hdr))
        {
            Ok(p) => p,
            Err(e) => {
                println!("// {path} unparsed: {e}");
                continue;
            }
        };
        if stops {
            let s = relift_gpu_sm53::lower::stops(&p);
            if s.is_empty() {
                println!("lowers {path}");
            }
            for x in s {
                println!("stop {} {:?} {}", x.op.name(), x.src, x.reason);
            }
            continue;
        }
        println!("// {path} {:?}", p.stage());
        for (k, s) in relift_gpu_sm53::bundle::slots(&p.code).enumerate() {
            match relift_gpu_sm53::Inst::decode(s.word, s.at) {
                Some(i) => println!("/*{k:04x}*/ {i} ;"),
                None => println!("/*{k:04x}*/ ?? {:016x}", s.word),
            }
        }
    }
}
