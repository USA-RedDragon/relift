//! Unicorn's static library uses 16-byte atomics, which GNU toolchains on
//! Linux provide in libatomic.

fn main() {
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("linux") {
        println!("cargo:rustc-link-lib=atomic");
    }
}
