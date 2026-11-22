# Recompile your first game

This step, after [the first program](02-first-program.md), turns one dump into a native program: a project folder, a shipped build and a development build that runs on this computer.

## Import the dump

Run `relift` in an empty project folder. `import` reads the dump and writes a `game.toml` for the project:

```sh
mkdir mygame && cd mygame
relift import ~/dumps/game.gbc
```

The argument is a Switch `.nup` or an extracted tree, or a `.gb`, `.gbc` or `.gba` ROM. The guest pages say what `import` finds in each: [Switch](../../guests/switch/README.md), [Game Boy](../../guests/gb/README.md) and [Game Boy Advance](../../guests/gba/README.md).

## Build and run

```sh
relift build
relift run
```

`relift build` makes a shipped build: the runtime is linked in, no code is loaded at run time, and the program is packaged for this host ([Package a game for each host](../guides/packaging.md)). `relift run` builds a development build when it needs one and runs it.

Both commands keep their artifacts in the cache directory. The cache can be copied to another path, and the next build reuses it ([Use relift](../guides/usage.md#projects-and-the-cache)).

## If the program stops

Discovery cannot see every code address. A shipped build that reaches code that discovery missed writes a stop report and exits with status <!-- docgen:const file=runtime/core/include/relift/abi_core.h name=RL_EXIT_SOFTWARE -->`70`<!-- /docgen:const -->. `relift report <file>` explains the report and adds the missed target for the next build. What the report fields mean, what a development build does with the miss, and how to report it upstream are in [Recompile a game](../guides/workflow.md#4-what-usually-happens-next-it-stops); the short form of the same facts is in [missed code and stop reports](../guides/usage.md#missed-code-and-stop-reports).

Next: [Next steps](04-next-steps.md).
