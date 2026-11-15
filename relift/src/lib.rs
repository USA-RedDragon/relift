//! The `relift` command's settings and subcommand tree, as a library so the
//! integration tests can name them.

pub mod appimage;
pub mod appimage_runtime;
pub mod build;
pub mod cli;
pub mod coverage;
pub mod dev;
pub mod devsock;
pub mod diff;
pub mod diff_ir;
pub mod elfdeps;
pub mod emitstore;
pub mod fpflags;
pub mod game_settings;
pub mod gb;
pub mod hints;
pub mod icon;
pub mod identity;
pub mod import;
pub mod info;
pub mod mods;
pub mod paths;
pub mod runtime_link;
pub mod settings;
pub mod ui;
