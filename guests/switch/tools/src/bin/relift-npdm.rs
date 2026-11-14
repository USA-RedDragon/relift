//! `relift-npdm`: dump an NPDM (`main.npdm`) as JSON.

use std::path::PathBuf;
use std::process::ExitCode;

use clap::Parser;
use relift_switch_formats::npdm::Npdm;
use relift_switch_formats::storage::read_all;
use relift_switch_tools::{print_json, run};
use serde_json::json;

/// Dump an NPDM as JSON.
#[derive(Parser, Debug)]
#[command(name = "relift-npdm", version)]
struct Cli {
    /// The NPDM file, or - for stdin.
    input: PathBuf,
}

fn main() -> ExitCode {
    run::<Cli>("relift-npdm", |cli| {
        let n = Npdm::parse(&read_all(&*relift_switch_tools::input(&cli.input)?)?)?;
        let services: Vec<_> = n
            .services
            .iter()
            .map(|s| json!({ "name": s.name, "server": s.is_server }))
            .collect();
        print_json(&json!({
            "name": n.name,
            "product_code": n.product_code,
            "program_id": format!("{:016x}", n.program_id),
            "is_64bit": n.is_64bit,
            "address_space": n.address_space.name(),
            "main_thread_priority": n.main_thread_priority,
            "main_thread_core": n.main_thread_core,
            "main_thread_stack_size": n.main_thread_stack_size,
            "system_resource_size": n.system_resource_size,
            "version": n.version,
            "services": services,
        }))
    })
}
