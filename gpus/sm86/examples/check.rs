use relift_gpu_sm86::{Word, decode};
use std::collections::BTreeMap;
use std::io::BufRead;

fn norm(s: &str) -> String {
    s.split_whitespace().collect::<Vec<_>>().join(" ")
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let path = &args[1];
    let filter = args.get(2).and_then(|s| u32::from_str_radix(s, 16).ok());
    let show: usize = args.get(3).and_then(|s| s.parse().ok()).unwrap_or(3);
    let f = std::io::BufReader::new(
        std::fs::File::open(path).unwrap_or_else(|e| panic!("{path}: {e}")),
    );
    let mut per: BTreeMap<u32, (u64, u64, u64, Vec<String>)> = BTreeMap::new();
    let mut total = (0u64, 0u64);
    for line in f.lines() {
        let line = line.unwrap_or_default();
        let mut it = line.splitn(3, '\t');
        let (Some(h), Some(pc), Some(text)) = (it.next(), it.next(), it.next()) else {
            continue;
        };
        let Ok(bytes) = (0..16)
            .map(|i| u8::from_str_radix(&h[2 * i..2 * i + 2], 16))
            .collect::<Result<Vec<u8>, _>>()
        else {
            continue;
        };
        let mut a = [0u8; 16];
        a.copy_from_slice(&bytes);
        let w = Word::from_bytes(&a);
        let op = w.get(0, 9);
        if filter.is_some_and(|f| f != op) {
            continue;
        }
        let pc: u64 = pc.parse().unwrap_or(0);
        let e = per.entry(op).or_default();
        e.0 += 1;
        total.0 += 1;
        match decode(w, pc) {
            Ok(i) => {
                let got = norm(&i.to_string());
                if got == norm(text) {
                    e.1 += 1;
                    total.1 += 1;
                } else {
                    e.2 += 1;
                    if e.3.len() < show {
                        e.3.push(format!("{h}  want `{text}`\n{:34} got  `{got}`", ""));
                    }
                }
            }
            Err(_) => {
                if e.3.len() < show {
                    e.3.push(format!("{h}  undecoded `{text}`"));
                }
            }
        }
    }
    for (op, (n, ok, bad, ex)) in &per {
        if ok == n {
            continue;
        }
        println!(
            "{op:03x} {n:8} ok {ok:8} wrong {bad:8} undecoded {}",
            n - ok - bad
        );
        for x in ex {
            println!("   {x}");
        }
    }
    println!(
        "TOTAL {} ok {} ({:.3}%)",
        total.0,
        total.1,
        100.0 * total.1 as f64 / total.0.max(1) as f64
    );
}
