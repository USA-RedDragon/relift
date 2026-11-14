//! The remangler against fixed names, a generated corpus and an independent
//! demangler.

mod corpus;
mod oracle;

use super::{GuestName, fallback_name, guest_name, host_name};

/// Input, module, exact output. These strings are the stable contract: a
/// change here changes every host name a build writes.
const GOLDEN: &[(&str, &str, &str)] = &[
    ("_ZN3foo3barEv", "sdk", "_ZN5guest3sdk3foo3barEv"),
    ("_ZN3foo3barERKS_", "sdk", "_ZN5guest3sdk3foo3barERKS1_"),
    ("_Z3fooi", "sdk", "_ZN5guest3sdk3fooEi"),
    ("_Z3fooIiEvT_", "sdk", "_ZN5guest3sdk3fooIiEEvT_"),
    ("_ZL3foov", "sdk", "_ZN5guest3sdkL3fooEv"),
    (
        "_ZNSt6vectorIiE4sizeEv",
        "sdk",
        "_ZN5guest3sdk3std6vectorIiE4sizeEv",
    ),
    (
        "_ZSt4swapIiEvRT_S1_",
        "sdk",
        "_ZN5guest3sdk3std4swapIiEEvRT_S4_",
    ),
    (
        "_ZNSs6appendEPKcm",
        "sdk",
        "_ZN5guest3sdk3std12basic_stringIcSt11char_traitsIcESaIcEE6appendEPKcm",
    ),
    (
        "_ZNSaIcEC1ERKS_",
        "sdk",
        "_ZN5guest3sdk3std9allocatorIcEC1ERKS3_",
    ),
    (
        "_ZZN3foo3barEvE3baz",
        "sdk",
        "_ZZN5guest3sdk3foo3barEvE3baz",
    ),
    ("_ZTV3foo", "sdk", "_ZTVN5guest3sdk3fooE"),
    ("_ZTISt9exception", "sdk", "_ZTIN5guest3sdk3std9exceptionE"),
    (
        "_ZThn16_N3foo3barEv",
        "sdk",
        "_ZThn16_N5guest3sdk3foo3barEv",
    ),
    (
        "_ZGVZN3foo3barEvE1x",
        "sdk",
        "_ZGVZN5guest3sdk3foo3barEvE1x",
    ),
    (
        "_ZN3foo3barEv.llvm.12",
        "sdk",
        "_ZN5guest3sdk3foo3barB13sfx_dllvm_d12Ev",
    ),
    (
        "_ZN3foo3barEv.cold",
        "sdk",
        "_ZN5guest3sdk3foo3barB9sfx_dcoldEv",
    ),
    (
        "_Z3fooRi.isra.0",
        "main",
        "_ZN5guest4main3fooB12sfx_disra_d0ERi",
    ),
    ("memcpy", "sdk", "_ZN5guest3sdk6memcpyE"),
    ("nnMain", "main", "_ZN5guest4main6nnMainE"),
];

/// Inputs that must get no name.
const UNNAMED: &[(&str, &str)] = &[
    ("_ZTIPKc", "sdk"),
    ("_ZN3fooE", "sdk"),
    ("_ZNT_3fooEv", "sdk"),
    ("_ZN3foo3barEv", "1bad"),
    ("_ZN3foo3barEv", "a.b"),
    ("_ZN3fooC1Ev.cold", "sdk"),
    ("_ZTV3foo.cold", "sdk"),
    ("_ZN3foo3barEv.a-b", "sdk"),
    ("_ZN3std6vectorIiE4sizeEv", "sdk"),
    ("_ZNSt9allocatorIcE8allocateEm", "sdk"),
    ("_ZNSbIcSt11char_traitsIcESaIcEE6appendEv", "sdk"),
    ("fn_00123450", "main"),
    ("_Z3foo", "sdk"),
    ("a.b", "sdk"),
    ("", "sdk"),
    ("_ZN3foo3barB9sfx_dcoldEv", "sdk"),
];

#[test]
fn golden_names_are_stable() {
    for (input, module, want) in GOLDEN {
        assert_eq!(
            host_name(module, input).as_deref(),
            Some(*want),
            "{module}:{input}"
        );
        assert_eq!(
            guest_name(want),
            Some((module.to_string(), GuestName::Symbol(input.to_string()))),
            "{want}"
        );
        oracle::check(module, input, want).unwrap_or_else(|e| panic!("{e}"));
    }
}

#[test]
fn names_without_a_canonical_host_name_fall_back() {
    for (input, module) in UNNAMED {
        assert_eq!(host_name(module, input), None, "{module}:{input}");
    }
}

#[test]
fn fallback_names_read_back_as_addresses() {
    let h = fallback_name("main", 0x12_3450);
    assert_eq!(h, "_ZN5guest4main11fn_00123450E");
    assert_eq!(
        guest_name(&h),
        Some(("main".to_string(), GuestName::Address(0x12_3450)))
    );
    let wide = fallback_name("sdk", 0x1_2345_6789);
    assert_eq!(
        guest_name(&wide),
        Some(("sdk".to_string(), GuestName::Address(0x1_2345_6789)))
    );
    assert_eq!(
        oracle::demangle(&h).as_deref(),
        Ok("guest::main::fn_00123450")
    );
    assert_eq!(
        guest_name("_ZN5guest4main12fn_000123450E"),
        Some((
            "main".to_string(),
            GuestName::Symbol("fn_000123450".to_string())
        ))
    );
}

#[test]
fn foreign_names_do_not_read_back() {
    for h in [
        "_ZN3foo3barEv",
        "_ZN5guest3barEv",
        "_ZN5guestEv",
        "memcpy",
        "_ZN5guest3sdk3foo3barEv.cold",
        "_ZN5guest3sdkE",
    ] {
        assert_eq!(guest_name(h), None, "{h}");
    }
}

/// Counts for one module of an outside corpus.
#[derive(Debug, Default)]
struct Counts {
    total: usize,
    named: usize,
    fallback: usize,
    round_trip_failures: usize,
    oracle_mismatches: usize,
    oracle_unknown: usize,
}

/// Runs the scheme over the names in the file `RELIFT_NAMES_CORPUS`, one
/// `module<TAB>symbol` per line, and prints counts only, never names, so a
/// corpus taken from a real program stays out of logs. Run it with
/// `cargo test -p relift-switch-names -- --ignored --nocapture`.
#[test]
#[ignore = "needs RELIFT_NAMES_CORPUS"]
fn an_outside_corpus_names_and_round_trips() {
    let Some(path) = std::env::var_os("RELIFT_NAMES_CORPUS") else {
        println!("RELIFT_NAMES_CORPUS is not set; nothing to check");
        return;
    };
    let text = std::fs::read_to_string(&path).expect("read RELIFT_NAMES_CORPUS");
    let mut by_module: std::collections::BTreeMap<String, Counts> = Default::default();
    let mut seen = std::collections::BTreeMap::new();
    let mut collisions = 0;
    for line in text.lines() {
        let Some((module, symbol)) = line.split_once('\t') else {
            continue;
        };
        let c = by_module.entry(module.to_string()).or_default();
        c.total += 1;
        let Some(h) = host_name(module, symbol) else {
            c.fallback += 1;
            continue;
        };
        c.named += 1;
        let back = guest_name(&h);
        if back != Some((module.to_string(), GuestName::Symbol(symbol.to_string()))) {
            c.round_trip_failures += 1;
        }
        if symbol.starts_with("_Z") && !oracle::knows(symbol) {
            c.oracle_unknown += 1;
        } else if oracle::check(module, symbol, &h).is_err() {
            c.oracle_mismatches += 1;
        }
        if let Some(prev) = seen.insert(h, (module.to_string(), symbol.to_string()))
            && prev != (module.to_string(), symbol.to_string())
        {
            collisions += 1;
        }
    }
    for (module, c) in &by_module {
        println!("{module}: {c:?}");
    }
    println!("host name collisions: {collisions}");
    assert_eq!(collisions, 0);
    assert!(by_module.values().all(|c| c.round_trip_failures == 0));
}

#[test]
fn the_naming_trait_uses_the_scheme() {
    use relift_guest::Naming;
    assert_eq!(
        super::ItaniumNaming.host_name("sdk", "_ZN3foo3barEv"),
        host_name("sdk", "_ZN3foo3barEv")
    );
}
