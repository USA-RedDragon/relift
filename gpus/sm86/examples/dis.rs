fn main() {
    for path in std::env::args().skip(1) {
        let b = std::fs::read(&path).unwrap_or_else(|e| panic!("{path}: {e}"));
        let p = relift_gpu_sm86::program::parse(&b);
        println!("// {path} {:?}", p.stage());
        if let Some(s) = &p.sph {
            println!("// sph {:08x?}", s.words);
        }
        for (n, d) in p.code.clone().into_iter().enumerate() {
            match d {
                Ok(i) => println!("/*{:04x}*/ {i} ;", 16 * n),
                Err(e) => println!("/*{:04x}*/ ?? {e}", 16 * n),
            }
        }
    }
}
