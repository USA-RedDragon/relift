# Site outline

This page lists the pages of the docs site in reading order, by section; the site builds its navigation from it.

- [Overview](../README.md)
- Get started
  - [Install](start/01-install.md)
  - [Recompile your first program](start/02-first-program.md)
  - [Recompile your first game](start/03-first-game.md)
  - [Next steps](start/04-next-steps.md)
- Guides
  - [Recompile a game](guides/workflow.md)
  - [Use relift](guides/usage.md)
  - [Package a game for each host](guides/packaging.md)
  - [Nintendo Switch games](../guests/switch/README.md)
  - [Game Boy and Game Boy Color games](../guests/gb/README.md)
  - [Game Boy Advance games](../guests/gba/README.md)
  - [Nintendo 64 games](../guests/n64/README.md)
  - [Master System, Game Gear and SG-1000 games](../guests/sms/README.md)
- Concepts
  - [Architecture](concepts/architecture.md)
  - [The C runtime](../runtime/README.md)
  - [The core crates](../core/README.md)
- Reference
  - [Settings](reference/configuration.md)
  - [Command line](reference/command-line.md)
  - [Input ports and bindings](reference/input.md)
  - [Runtime ABI contract](abi.md)
  - [The IR](reference/ir.md)
  - [ARM64 CPU](../cpus/arm64/README.md)
  - [ARMv4T CPU](../cpus/arm32/README.md)
  - [SM83 CPU](../cpus/sm83/README.md)
  - [VR4300 CPU](../cpus/mips/README.md)
  - [SM53 (Maxwell) shader code](../gpus/sm53/README.md)
  - [SM53 lowering table](../gpus/sm53/LOWERING.md)
  - [SM86 shader code](../gpus/sm86/README.md)
  - [NVIDIA shader translator](../gpus/nv/README.md)
  - [The differential checker](../check/README.md)
- Project
  - [Contributing](../CONTRIBUTING.md)
  - [Open work](../GAPS.md)
  - [Tooling and checks](project/tooling.md)
  - [License](project/license.md)

Each item is a link to a page, or, for a page that is planned and not written, its title and its path in code. A comment `<!-- old: <path> -->` after a link gives the path of the page before it moved; the site uses that file until the new one exists. A link to a file that is not a page, or to another site, shows in the navigation and opens that file or site.
