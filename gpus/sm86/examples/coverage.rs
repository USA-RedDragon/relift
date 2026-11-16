use relift_gpu_sm86::program::Program;
use relift_gpu_sm86::translate::translate;
use std::collections::BTreeMap;

fn main() {
    let dir = std::env::args().nth(1).unwrap_or_default();
    let mut ok = 0u64;
    let mut total = 0u64;
    let mut why: BTreeMap<String, u64> = BTreeMap::new();
    let mut entries: Vec<_> = std::fs::read_dir(&dir)
        .unwrap_or_else(|e| panic!("{dir}: {e}"))
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.extension().is_some_and(|x| x == "bin"))
        .collect();
    entries.sort();
    for p in entries {
        let Ok(b) = std::fs::read(&p) else { continue };
        total += 1;
        match translate(&Program::parse(&b)) {
            Ok(_) => ok += 1,
            Err(e) => {
                let k = e.0.split(' ').take(2).collect::<Vec<_>>().join(" ");
                *why.entry(k).or_default() += 1;
            }
        }
    }
    let mut v: Vec<_> = why.into_iter().collect();
    v.sort_by_key(|x| std::cmp::Reverse(x.1));
    for (k, n) in v.iter().take(40) {
        println!("{n:8} {k}");
    }
    println!("translated {ok} of {total}");
}
