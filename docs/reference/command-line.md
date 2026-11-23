# Command line

The usage of `relift` and of the tools that come with it, as each program prints it. Each block is generated from the program, so it is as current as the code. The settings that `relift` also reads from `relift.toml` and the environment are in [Settings](configuration.md); how the commands fit together is in [Use relift](../guides/usage.md).

## relift

<!-- docgen:usage crate=relift bin=relift args="--help" -->
```text
relift: recompile console game code to portable C11

Usage: relift [OPTIONS] <COMMAND>

Commands:
  import    Import a decrypted game dump into a project
  build     Lift, emit and compile the project
  push      Copy the project's game data (romfs, DLC) onto a phone for the app `relift build --target` made, and check every file's size against the data manifest; files that differ are copied again, and the push fails naming the first file that still differs
  run       Build if needed, then run the project
  check     Compare lifted functions against the reference interpreter
  diff      Compare one lifted function with the reference interpreter in a live run
  where     Map between a host address, a guest address and a symbol
  info      Print the self-description of a built game binary, without running it: the JSON document its `--relift-info` flag prints
  coverage  List what discovery could not resolve: per module, functions, instructions not lifted and every unresolved indirect call or jump, with hints from earlier hot-load runs. Switch projects only; for a Game Boy ROM use `relift-gbrom functions`
  shaders   Translate the GPU programs a build recorded again, for the device of its `shaders/device.json`, and rewrite its tables, without building the game. Switch 2 builds only
  report    Explain a stop report a shipped build wrote (missed code, an instruction it could not run) and add the missed target as a root for the next build
  help      Print this message or the help of the given subcommand(s)

Options:
  -q, --quiet
          Print only the final summary and errors.
          
          A flag of its own, not a setting: configulator's boolean flags take an optional value and would swallow the subcommand in `relift -q build`.

  -v, --verbose
          Also print each file as it compiles and every instruction that does not lift

  -c, --config <config>
          config file
          
          [default: relift.toml]

      --cache-dir <cache-dir>
          where lifted, emitted and built artifacts are cached (empty: the user cache dir)

  -j, --jobs <jobs>
          worker threads for every parallel stage of a build: analysis, emit and compile (0: one per CPU)
          
          [default: 0]

      --runtime-dir <runtime-dir>
          the C runtime sources to build against (empty: the ones beside this relift's source)

      --color <color>
          color in output: auto (a terminal that shows it, and NO_COLOR unset), always or never
          
          [default: auto]

      --shader-math <shader-math>
          the GPU float math of the shader tables a build makes for this machine: exact (the console's results), or native or both (also the table of the host GPU's own math, which a run uses with gpu.math=native). Packages and phone targets always carry both
          
          [default: exact]

      --toolchain.clang <toolchain.clang>
          the clang driver

      --toolchain.zig <toolchain.zig>
          zig, for `zig cc` player builds

      --toolchain.ninja <toolchain.ninja>
          the ninja build tool

      --toolchain.appimagetool <toolchain.appimagetool>
          appimagetool, which packs the Linux release output

      --toolchain.android-sdk <toolchain.android-sdk>
          the Android SDK with build-tools and platforms (empty: ANDROID_HOME, then common places)

      --toolchain.android-ndk <toolchain.android-ndk>
          the Android NDK (empty: ANDROID_NDK_HOME, then the SDK's newest)

      --toolchain.codesign-identity <toolchain.codesign-identity>
          the identity that signs macOS game builds, such as a Developer ID Application certificate name or its SHA-1 (empty: an ad-hoc signature)

      --toolchain.codesign-keychain <toolchain.codesign-keychain>
          the keychain that holds the signing identity (empty: the default keychain search list)

      --emit.shard-kib <emit.shard-kib>
          the most KiB of C per emitted file; clang needs about 120 bytes of memory per byte of C
          
          [default: 4096]

      --emit.piece-kib <emit.piece-kib>
          the most KiB of C per C function; a larger guest function is split into several
          
          [default: 1024]

      --emit.cold [<emit.cold>]
          also emit a cold copy of every function that dispatch enters at any instruction, so no guest code is missed
          
          [default: true]

      --emit.cold-piece-kib <emit.cold-piece-kib>
          the most KiB of C per C function of the cold copy
          
          [default: 4]

      --emit.cold-first [<emit.cold-first>]
          for tests: run every function from the cold copy, which then calls only itself
          
          [default: false]

      --emit.fpcr-guards [<emit.fpcr-guards>]
          for debugging: keep the FPCR guards of lifted code even when the build proves the FP mode constant (RELIFT_FP_EXACT=1 needs them)
          
          [default: false]

      --emit.trampoline [<emit.trampoline>]
          make every guest return and indirect branch go through dispatch, so the host stack never mirrors the guest stack (a Linux program that the build proves safe otherwise returns directly)
          
          [default: false]

      --emit.line-tables [<emit.line-tables>]
          give the lifted C of a host build line tables, so a crash names the guest function; false compiles it about 40% faster
          
          [default: true]

      --package.appimage <package.appimage>
          the desktop release output: an AppImage on Linux, a signed .app on macOS. auto (an AppImage when appimagetool is found), always (a Linux build without appimagetool fails) or never (no desktop package; the Android APK is always built)
          
          [default: auto]

      --package.android-id <package.android-id>
          the application id of the Android APK (empty: dev.relift.game.<project folder>)

      --package.android-keystore <package.android-keystore>
          the keystore that signs the APK, with the key alias relift (empty: a debug key in the cache)

      --package.android-keystore-pass <package.android-keystore-pass>
          the password of the keystore and its key

      --package.notarize [<package.notarize>]
          notarize a macOS game build with Apple (needs toolchain.codesign-identity and the notarize-* keys)
          
          [default: false]

      --package.notarize-key <package.notarize-key>
          the App Store Connect API key file (.p8) that notarization signs in with

      --package.notarize-key-id <package.notarize-key-id>
          the key ID of the notarization API key

      --package.notarize-issuer <package.notarize-issuer>
          the issuer ID of the notarization API key

      --package.ios-id <package.ios-id>
          the bundle identifier of the iOS app (empty: dev.relift.game.<project folder>)

      --package.ios-identity <package.ios-identity>
          the keychain identity that signs the iOS app for a device (empty: the first Apple Development identity)

      --package.ios-data <package.ios-data>
          where an iOS app finds the game data: bundle (copied into the app, sealed by its signature) or documents (added later to the app's Documents, for example with relift push)
          
          [default: bundle]

      --package.ios-profile <package.ios-profile>
          the provisioning profile of the iOS app (empty: one from Xcode's profile folders that covers the bundle identifier)

  -h, --help
          Print help (see a summary with '-h')

  -V, --version
          Print version
```
<!-- /docgen:usage -->

### relift import

<!-- docgen:usage crate=relift bin=relift args="help import" -->
```text
Import a decrypted game dump into a project.

The dump is a .nup file, or a directory holding base/{exefs,romfs}, updates/<version>/{exefs,romfs} and dlc/<title id>/<index>/romfs (or exefs/ and romfs/ directly). The romfs is not extracted: a .nup's is read in place (keep the .nup where it is), a tree's becomes one image file. Encrypted dumps are refused: convert them with a separate tool first. A Game Boy or Game Boy Color cartridge ROM (.gb, .gbc) is imported as it is; its battery save stays next to it as a .sav file.

Usage: relift import [OPTIONS] <DUMP>

Arguments:
  <DUMP>
          The .nup file, dump directory or cartridge ROM

Options:
  -q, --quiet
          Print only the final summary and errors.
          
          A flag of its own, not a setting: configulator's boolean flags take an optional value and would swallow the subcommand in `relift -q build`.

      --update <VERSION|base>
          The update version to apply, or `base` for the base alone (default: the newest update)

      --program <TITLE_ID>
          Which program to import from a container holding several (its title id in hex); the import lists them when it must choose

  -v, --verbose
          Also print each file as it compiles and every instruction that does not lift

      --system-data <DIR>
          A folder of the player's own decrypted system data, exported with switch-tools from their console: the shared font titles (0100000000000810 to 0100000000000814), one folder per title id. Its fonts are decoded into the import for pl:u

      --sysroot <DIR>
          For a dynamically linked Linux program: the arm64 Linux root file system, or a cross toolchain's sysroot, that holds its program interpreter and libraries. They are found the way the interpreter searches (run paths, the sysroot's ld.so.conf, the default directories) and copied into the import

  -c, --config <config>
          config file
          
          [default: relift.toml]

      --cache-dir <cache-dir>
          where lifted, emitted and built artifacts are cached (empty: the user cache dir)

  -j, --jobs <jobs>
          worker threads for every parallel stage of a build: analysis, emit and compile (0: one per CPU)
          
          [default: 0]

      --runtime-dir <runtime-dir>
          the C runtime sources to build against (empty: the ones beside this relift's source)

      --color <color>
          color in output: auto (a terminal that shows it, and NO_COLOR unset), always or never
          
          [default: auto]

      --shader-math <shader-math>
          the GPU float math of the shader tables a build makes for this machine: exact (the console's results), or native or both (also the table of the host GPU's own math, which a run uses with gpu.math=native). Packages and phone targets always carry both
          
          [default: exact]

      --toolchain.clang <toolchain.clang>
          the clang driver

      --toolchain.zig <toolchain.zig>
          zig, for `zig cc` player builds

      --toolchain.ninja <toolchain.ninja>
          the ninja build tool

      --toolchain.appimagetool <toolchain.appimagetool>
          appimagetool, which packs the Linux release output

      --toolchain.android-sdk <toolchain.android-sdk>
          the Android SDK with build-tools and platforms (empty: ANDROID_HOME, then common places)

      --toolchain.android-ndk <toolchain.android-ndk>
          the Android NDK (empty: ANDROID_NDK_HOME, then the SDK's newest)

      --toolchain.codesign-identity <toolchain.codesign-identity>
          the identity that signs macOS game builds, such as a Developer ID Application certificate name or its SHA-1 (empty: an ad-hoc signature)

      --toolchain.codesign-keychain <toolchain.codesign-keychain>
          the keychain that holds the signing identity (empty: the default keychain search list)

      --emit.shard-kib <emit.shard-kib>
          the most KiB of C per emitted file; clang needs about 120 bytes of memory per byte of C
          
          [default: 4096]

      --emit.piece-kib <emit.piece-kib>
          the most KiB of C per C function; a larger guest function is split into several
          
          [default: 1024]

      --emit.cold [<emit.cold>]
          also emit a cold copy of every function that dispatch enters at any instruction, so no guest code is missed
          
          [default: true]

      --emit.cold-piece-kib <emit.cold-piece-kib>
          the most KiB of C per C function of the cold copy
          
          [default: 4]

      --emit.cold-first [<emit.cold-first>]
          for tests: run every function from the cold copy, which then calls only itself
          
          [default: false]

      --emit.fpcr-guards [<emit.fpcr-guards>]
          for debugging: keep the FPCR guards of lifted code even when the build proves the FP mode constant (RELIFT_FP_EXACT=1 needs them)
          
          [default: false]

      --emit.trampoline [<emit.trampoline>]
          make every guest return and indirect branch go through dispatch, so the host stack never mirrors the guest stack (a Linux program that the build proves safe otherwise returns directly)
          
          [default: false]

      --emit.line-tables [<emit.line-tables>]
          give the lifted C of a host build line tables, so a crash names the guest function; false compiles it about 40% faster
          
          [default: true]

      --package.appimage <package.appimage>
          the desktop release output: an AppImage on Linux, a signed .app on macOS. auto (an AppImage when appimagetool is found), always (a Linux build without appimagetool fails) or never (no desktop package; the Android APK is always built)
          
          [default: auto]

      --package.android-id <package.android-id>
          the application id of the Android APK (empty: dev.relift.game.<project folder>)

      --package.android-keystore <package.android-keystore>
          the keystore that signs the APK, with the key alias relift (empty: a debug key in the cache)

      --package.android-keystore-pass <package.android-keystore-pass>
          the password of the keystore and its key

      --package.notarize [<package.notarize>]
          notarize a macOS game build with Apple (needs toolchain.codesign-identity and the notarize-* keys)
          
          [default: false]

      --package.notarize-key <package.notarize-key>
          the App Store Connect API key file (.p8) that notarization signs in with

      --package.notarize-key-id <package.notarize-key-id>
          the key ID of the notarization API key

      --package.notarize-issuer <package.notarize-issuer>
          the issuer ID of the notarization API key

      --package.ios-id <package.ios-id>
          the bundle identifier of the iOS app (empty: dev.relift.game.<project folder>)

      --package.ios-identity <package.ios-identity>
          the keychain identity that signs the iOS app for a device (empty: the first Apple Development identity)

      --package.ios-data <package.ios-data>
          where an iOS app finds the game data: bundle (copied into the app, sealed by its signature) or documents (added later to the app's Documents, for example with relift push)
          
          [default: bundle]

      --package.ios-profile <package.ios-profile>
          the provisioning profile of the iOS app (empty: one from Xcode's profile folders that covers the bundle identifier)

  -h, --help
          Print help (see a summary with '-h')

  -V, --version
          Print version
```
<!-- /docgen:usage -->

### relift build

<!-- docgen:usage crate=relift bin=relift args="help build" -->
```text
Lift, emit and compile the project

Usage: relift build [OPTIONS]

Options:
  -q, --quiet
          Print only the final summary and errors.
          
          A flag of its own, not a setting: configulator's boolean flags take an optional value and would swallow the subcommand in `relift -q build`.

      --target <TARGET>
          A host target to build for; repeat for several. Default: this host

      --mods <MOD>
          Code mods to build in, in order: mod directories (Atmosphere or Ryujinx layout) or .ips and .pchtxt files, comma-separated or repeated. Later mods apply over earlier ones; two mods that write different bytes at one address are refused. For a Game Boy ROM, IPS, UPS or BPS ROM-hack patches, applied in order

  -v, --verbose
          Also print each file as it compiles and every instruction that does not lift

      --dev
          A development build of a cartridge ROM project, as `relift run` makes: lifted C compiles at the guest's dev optimization, for compile time. For a Game Boy project, code without a lifted body also runs on the device library's CPU and `--check` works, where a release build stops at it

  -c, --config <config>
          config file
          
          [default: relift.toml]

      --cache-dir <cache-dir>
          where lifted, emitted and built artifacts are cached (empty: the user cache dir)

  -j, --jobs <jobs>
          worker threads for every parallel stage of a build: analysis, emit and compile (0: one per CPU)
          
          [default: 0]

      --runtime-dir <runtime-dir>
          the C runtime sources to build against (empty: the ones beside this relift's source)

      --color <color>
          color in output: auto (a terminal that shows it, and NO_COLOR unset), always or never
          
          [default: auto]

      --shader-math <shader-math>
          the GPU float math of the shader tables a build makes for this machine: exact (the console's results), or native or both (also the table of the host GPU's own math, which a run uses with gpu.math=native). Packages and phone targets always carry both
          
          [default: exact]

      --toolchain.clang <toolchain.clang>
          the clang driver

      --toolchain.zig <toolchain.zig>
          zig, for `zig cc` player builds

      --toolchain.ninja <toolchain.ninja>
          the ninja build tool

      --toolchain.appimagetool <toolchain.appimagetool>
          appimagetool, which packs the Linux release output

      --toolchain.android-sdk <toolchain.android-sdk>
          the Android SDK with build-tools and platforms (empty: ANDROID_HOME, then common places)

      --toolchain.android-ndk <toolchain.android-ndk>
          the Android NDK (empty: ANDROID_NDK_HOME, then the SDK's newest)

      --toolchain.codesign-identity <toolchain.codesign-identity>
          the identity that signs macOS game builds, such as a Developer ID Application certificate name or its SHA-1 (empty: an ad-hoc signature)

      --toolchain.codesign-keychain <toolchain.codesign-keychain>
          the keychain that holds the signing identity (empty: the default keychain search list)

      --emit.shard-kib <emit.shard-kib>
          the most KiB of C per emitted file; clang needs about 120 bytes of memory per byte of C
          
          [default: 4096]

      --emit.piece-kib <emit.piece-kib>
          the most KiB of C per C function; a larger guest function is split into several
          
          [default: 1024]

      --emit.cold [<emit.cold>]
          also emit a cold copy of every function that dispatch enters at any instruction, so no guest code is missed
          
          [default: true]

      --emit.cold-piece-kib <emit.cold-piece-kib>
          the most KiB of C per C function of the cold copy
          
          [default: 4]

      --emit.cold-first [<emit.cold-first>]
          for tests: run every function from the cold copy, which then calls only itself
          
          [default: false]

      --emit.fpcr-guards [<emit.fpcr-guards>]
          for debugging: keep the FPCR guards of lifted code even when the build proves the FP mode constant (RELIFT_FP_EXACT=1 needs them)
          
          [default: false]

      --emit.trampoline [<emit.trampoline>]
          make every guest return and indirect branch go through dispatch, so the host stack never mirrors the guest stack (a Linux program that the build proves safe otherwise returns directly)
          
          [default: false]

      --emit.line-tables [<emit.line-tables>]
          give the lifted C of a host build line tables, so a crash names the guest function; false compiles it about 40% faster
          
          [default: true]

      --package.appimage <package.appimage>
          the desktop release output: an AppImage on Linux, a signed .app on macOS. auto (an AppImage when appimagetool is found), always (a Linux build without appimagetool fails) or never (no desktop package; the Android APK is always built)
          
          [default: auto]

      --package.android-id <package.android-id>
          the application id of the Android APK (empty: dev.relift.game.<project folder>)

      --package.android-keystore <package.android-keystore>
          the keystore that signs the APK, with the key alias relift (empty: a debug key in the cache)

      --package.android-keystore-pass <package.android-keystore-pass>
          the password of the keystore and its key

      --package.notarize [<package.notarize>]
          notarize a macOS game build with Apple (needs toolchain.codesign-identity and the notarize-* keys)
          
          [default: false]

      --package.notarize-key <package.notarize-key>
          the App Store Connect API key file (.p8) that notarization signs in with

      --package.notarize-key-id <package.notarize-key-id>
          the key ID of the notarization API key

      --package.notarize-issuer <package.notarize-issuer>
          the issuer ID of the notarization API key

      --package.ios-id <package.ios-id>
          the bundle identifier of the iOS app (empty: dev.relift.game.<project folder>)

      --package.ios-identity <package.ios-identity>
          the keychain identity that signs the iOS app for a device (empty: the first Apple Development identity)

      --package.ios-data <package.ios-data>
          where an iOS app finds the game data: bundle (copied into the app, sealed by its signature) or documents (added later to the app's Documents, for example with relift push)
          
          [default: bundle]

      --package.ios-profile <package.ios-profile>
          the provisioning profile of the iOS app (empty: one from Xcode's profile folders that covers the bundle identifier)

  -h, --help
          Print help (see a summary with '-h')

  -V, --version
          Print version
```
<!-- /docgen:usage -->

### relift push

<!-- docgen:usage crate=relift bin=relift args="help push" -->
```text
Copy the project's game data (romfs, DLC) onto a phone for the app `relift build --target` made, and check every file's size against the data manifest; files that differ are copied again, and the push fails naming the first file that still differs

Usage: relift push [OPTIONS] --target <TARGET>

Options:
  -q, --quiet
          Print only the final summary and errors.
          
          A flag of its own, not a setting: configulator's boolean flags take an optional value and would swallow the subcommand in `relift -q build`.

      --target <TARGET>
          The app's target: ios-arm64 or android-arm64

      --device <DEVICE>
          The device: an iOS identifier or name, or an adb serial. Default: the one connected device

  -v, --verbose
          Also print each file as it compiles and every instruction that does not lift

  -c, --config <config>
          config file
          
          [default: relift.toml]

      --cache-dir <cache-dir>
          where lifted, emitted and built artifacts are cached (empty: the user cache dir)

  -j, --jobs <jobs>
          worker threads for every parallel stage of a build: analysis, emit and compile (0: one per CPU)
          
          [default: 0]

      --runtime-dir <runtime-dir>
          the C runtime sources to build against (empty: the ones beside this relift's source)

      --color <color>
          color in output: auto (a terminal that shows it, and NO_COLOR unset), always or never
          
          [default: auto]

      --shader-math <shader-math>
          the GPU float math of the shader tables a build makes for this machine: exact (the console's results), or native or both (also the table of the host GPU's own math, which a run uses with gpu.math=native). Packages and phone targets always carry both
          
          [default: exact]

      --toolchain.clang <toolchain.clang>
          the clang driver

      --toolchain.zig <toolchain.zig>
          zig, for `zig cc` player builds

      --toolchain.ninja <toolchain.ninja>
          the ninja build tool

      --toolchain.appimagetool <toolchain.appimagetool>
          appimagetool, which packs the Linux release output

      --toolchain.android-sdk <toolchain.android-sdk>
          the Android SDK with build-tools and platforms (empty: ANDROID_HOME, then common places)

      --toolchain.android-ndk <toolchain.android-ndk>
          the Android NDK (empty: ANDROID_NDK_HOME, then the SDK's newest)

      --toolchain.codesign-identity <toolchain.codesign-identity>
          the identity that signs macOS game builds, such as a Developer ID Application certificate name or its SHA-1 (empty: an ad-hoc signature)

      --toolchain.codesign-keychain <toolchain.codesign-keychain>
          the keychain that holds the signing identity (empty: the default keychain search list)

      --emit.shard-kib <emit.shard-kib>
          the most KiB of C per emitted file; clang needs about 120 bytes of memory per byte of C
          
          [default: 4096]

      --emit.piece-kib <emit.piece-kib>
          the most KiB of C per C function; a larger guest function is split into several
          
          [default: 1024]

      --emit.cold [<emit.cold>]
          also emit a cold copy of every function that dispatch enters at any instruction, so no guest code is missed
          
          [default: true]

      --emit.cold-piece-kib <emit.cold-piece-kib>
          the most KiB of C per C function of the cold copy
          
          [default: 4]

      --emit.cold-first [<emit.cold-first>]
          for tests: run every function from the cold copy, which then calls only itself
          
          [default: false]

      --emit.fpcr-guards [<emit.fpcr-guards>]
          for debugging: keep the FPCR guards of lifted code even when the build proves the FP mode constant (RELIFT_FP_EXACT=1 needs them)
          
          [default: false]

      --emit.trampoline [<emit.trampoline>]
          make every guest return and indirect branch go through dispatch, so the host stack never mirrors the guest stack (a Linux program that the build proves safe otherwise returns directly)
          
          [default: false]

      --emit.line-tables [<emit.line-tables>]
          give the lifted C of a host build line tables, so a crash names the guest function; false compiles it about 40% faster
          
          [default: true]

      --package.appimage <package.appimage>
          the desktop release output: an AppImage on Linux, a signed .app on macOS. auto (an AppImage when appimagetool is found), always (a Linux build without appimagetool fails) or never (no desktop package; the Android APK is always built)
          
          [default: auto]

      --package.android-id <package.android-id>
          the application id of the Android APK (empty: dev.relift.game.<project folder>)

      --package.android-keystore <package.android-keystore>
          the keystore that signs the APK, with the key alias relift (empty: a debug key in the cache)

      --package.android-keystore-pass <package.android-keystore-pass>
          the password of the keystore and its key

      --package.notarize [<package.notarize>]
          notarize a macOS game build with Apple (needs toolchain.codesign-identity and the notarize-* keys)
          
          [default: false]

      --package.notarize-key <package.notarize-key>
          the App Store Connect API key file (.p8) that notarization signs in with

      --package.notarize-key-id <package.notarize-key-id>
          the key ID of the notarization API key

      --package.notarize-issuer <package.notarize-issuer>
          the issuer ID of the notarization API key

      --package.ios-id <package.ios-id>
          the bundle identifier of the iOS app (empty: dev.relift.game.<project folder>)

      --package.ios-identity <package.ios-identity>
          the keychain identity that signs the iOS app for a device (empty: the first Apple Development identity)

      --package.ios-data <package.ios-data>
          where an iOS app finds the game data: bundle (copied into the app, sealed by its signature) or documents (added later to the app's Documents, for example with relift push)
          
          [default: bundle]

      --package.ios-profile <package.ios-profile>
          the provisioning profile of the iOS app (empty: one from Xcode's profile folders that covers the bundle identifier)

  -h, --help
          Print help (see a summary with '-h')

  -V, --version
          Print version
```
<!-- /docgen:usage -->

### relift run

<!-- docgen:usage crate=relift bin=relift args="help run" -->
```text
Build if needed, then run the project

Usage: relift run [OPTIONS] [-- <ARGS>...]

Arguments:
  [ARGS]...
          The arguments a Linux program runs with, after `--`

Options:
  -q, --quiet
          Print only the final summary and errors.
          
          A flag of its own, not a setting: configulator's boolean flags take an optional value and would swallow the subcommand in `relift -q build`.

      --target <TARGET>
          The host target to run on. Default: this host

      --trace-os
          Log every guest OS call and service request (RELIFT_TRACE_OS)

  -v, --verbose
          Also print each file as it compiles and every instruction that does not lift

      --trace <TRACE>
          Log entry and exit of guest functions matching this glob

      --strict
          Stop on the first unknown service command or system call

  -c, --config <config>
          config file
          
          [default: relift.toml]

      --cache-dir <cache-dir>
          where lifted, emitted and built artifacts are cached (empty: the user cache dir)

  -j, --jobs <jobs>
          worker threads for every parallel stage of a build: analysis, emit and compile (0: one per CPU)
          
          [default: 0]

      --runtime-dir <runtime-dir>
          the C runtime sources to build against (empty: the ones beside this relift's source)

      --color <color>
          color in output: auto (a terminal that shows it, and NO_COLOR unset), always or never
          
          [default: auto]

      --shader-math <shader-math>
          the GPU float math of the shader tables a build makes for this machine: exact (the console's results), or native or both (also the table of the host GPU's own math, which a run uses with gpu.math=native). Packages and phone targets always carry both
          
          [default: exact]

      --toolchain.clang <toolchain.clang>
          the clang driver

      --toolchain.zig <toolchain.zig>
          zig, for `zig cc` player builds

      --toolchain.ninja <toolchain.ninja>
          the ninja build tool

      --toolchain.appimagetool <toolchain.appimagetool>
          appimagetool, which packs the Linux release output

      --toolchain.android-sdk <toolchain.android-sdk>
          the Android SDK with build-tools and platforms (empty: ANDROID_HOME, then common places)

      --toolchain.android-ndk <toolchain.android-ndk>
          the Android NDK (empty: ANDROID_NDK_HOME, then the SDK's newest)

      --toolchain.codesign-identity <toolchain.codesign-identity>
          the identity that signs macOS game builds, such as a Developer ID Application certificate name or its SHA-1 (empty: an ad-hoc signature)

      --toolchain.codesign-keychain <toolchain.codesign-keychain>
          the keychain that holds the signing identity (empty: the default keychain search list)

      --emit.shard-kib <emit.shard-kib>
          the most KiB of C per emitted file; clang needs about 120 bytes of memory per byte of C
          
          [default: 4096]

      --emit.piece-kib <emit.piece-kib>
          the most KiB of C per C function; a larger guest function is split into several
          
          [default: 1024]

      --emit.cold [<emit.cold>]
          also emit a cold copy of every function that dispatch enters at any instruction, so no guest code is missed
          
          [default: true]

      --emit.cold-piece-kib <emit.cold-piece-kib>
          the most KiB of C per C function of the cold copy
          
          [default: 4]

      --emit.cold-first [<emit.cold-first>]
          for tests: run every function from the cold copy, which then calls only itself
          
          [default: false]

      --emit.fpcr-guards [<emit.fpcr-guards>]
          for debugging: keep the FPCR guards of lifted code even when the build proves the FP mode constant (RELIFT_FP_EXACT=1 needs them)
          
          [default: false]

      --emit.trampoline [<emit.trampoline>]
          make every guest return and indirect branch go through dispatch, so the host stack never mirrors the guest stack (a Linux program that the build proves safe otherwise returns directly)
          
          [default: false]

      --emit.line-tables [<emit.line-tables>]
          give the lifted C of a host build line tables, so a crash names the guest function; false compiles it about 40% faster
          
          [default: true]

      --package.appimage <package.appimage>
          the desktop release output: an AppImage on Linux, a signed .app on macOS. auto (an AppImage when appimagetool is found), always (a Linux build without appimagetool fails) or never (no desktop package; the Android APK is always built)
          
          [default: auto]

      --package.android-id <package.android-id>
          the application id of the Android APK (empty: dev.relift.game.<project folder>)

      --package.android-keystore <package.android-keystore>
          the keystore that signs the APK, with the key alias relift (empty: a debug key in the cache)

      --package.android-keystore-pass <package.android-keystore-pass>
          the password of the keystore and its key

      --package.notarize [<package.notarize>]
          notarize a macOS game build with Apple (needs toolchain.codesign-identity and the notarize-* keys)
          
          [default: false]

      --package.notarize-key <package.notarize-key>
          the App Store Connect API key file (.p8) that notarization signs in with

      --package.notarize-key-id <package.notarize-key-id>
          the key ID of the notarization API key

      --package.notarize-issuer <package.notarize-issuer>
          the issuer ID of the notarization API key

      --package.ios-id <package.ios-id>
          the bundle identifier of the iOS app (empty: dev.relift.game.<project folder>)

      --package.ios-identity <package.ios-identity>
          the keychain identity that signs the iOS app for a device (empty: the first Apple Development identity)

      --package.ios-data <package.ios-data>
          where an iOS app finds the game data: bundle (copied into the app, sealed by its signature) or documents (added later to the app's Documents, for example with relift push)
          
          [default: bundle]

      --package.ios-profile <package.ios-profile>
          the provisioning profile of the iOS app (empty: one from Xcode's profile folders that covers the bundle identifier)

  -h, --help
          Print help (see a summary with '-h')

  -V, --version
          Print version
```
<!-- /docgen:usage -->

### relift check

<!-- docgen:usage crate=relift bin=relift args="help check" -->
```text
Compare lifted functions against the reference interpreter

Usage: relift check [OPTIONS] <WHAT>

Arguments:
  <WHAT>
          A guest symbol or address

Options:
  -q, --quiet
          Print only the final summary and errors.
          
          A flag of its own, not a setting: configulator's boolean flags take an optional value and would swallow the subcommand in `relift -q build`.

  -v, --verbose
          Also print each file as it compiles and every instruction that does not lift

  -c, --config <config>
          config file
          
          [default: relift.toml]

      --cache-dir <cache-dir>
          where lifted, emitted and built artifacts are cached (empty: the user cache dir)

  -j, --jobs <jobs>
          worker threads for every parallel stage of a build: analysis, emit and compile (0: one per CPU)
          
          [default: 0]

      --runtime-dir <runtime-dir>
          the C runtime sources to build against (empty: the ones beside this relift's source)

      --color <color>
          color in output: auto (a terminal that shows it, and NO_COLOR unset), always or never
          
          [default: auto]

      --shader-math <shader-math>
          the GPU float math of the shader tables a build makes for this machine: exact (the console's results), or native or both (also the table of the host GPU's own math, which a run uses with gpu.math=native). Packages and phone targets always carry both
          
          [default: exact]

      --toolchain.clang <toolchain.clang>
          the clang driver

      --toolchain.zig <toolchain.zig>
          zig, for `zig cc` player builds

      --toolchain.ninja <toolchain.ninja>
          the ninja build tool

      --toolchain.appimagetool <toolchain.appimagetool>
          appimagetool, which packs the Linux release output

      --toolchain.android-sdk <toolchain.android-sdk>
          the Android SDK with build-tools and platforms (empty: ANDROID_HOME, then common places)

      --toolchain.android-ndk <toolchain.android-ndk>
          the Android NDK (empty: ANDROID_NDK_HOME, then the SDK's newest)

      --toolchain.codesign-identity <toolchain.codesign-identity>
          the identity that signs macOS game builds, such as a Developer ID Application certificate name or its SHA-1 (empty: an ad-hoc signature)

      --toolchain.codesign-keychain <toolchain.codesign-keychain>
          the keychain that holds the signing identity (empty: the default keychain search list)

      --emit.shard-kib <emit.shard-kib>
          the most KiB of C per emitted file; clang needs about 120 bytes of memory per byte of C
          
          [default: 4096]

      --emit.piece-kib <emit.piece-kib>
          the most KiB of C per C function; a larger guest function is split into several
          
          [default: 1024]

      --emit.cold [<emit.cold>]
          also emit a cold copy of every function that dispatch enters at any instruction, so no guest code is missed
          
          [default: true]

      --emit.cold-piece-kib <emit.cold-piece-kib>
          the most KiB of C per C function of the cold copy
          
          [default: 4]

      --emit.cold-first [<emit.cold-first>]
          for tests: run every function from the cold copy, which then calls only itself
          
          [default: false]

      --emit.fpcr-guards [<emit.fpcr-guards>]
          for debugging: keep the FPCR guards of lifted code even when the build proves the FP mode constant (RELIFT_FP_EXACT=1 needs them)
          
          [default: false]

      --emit.trampoline [<emit.trampoline>]
          make every guest return and indirect branch go through dispatch, so the host stack never mirrors the guest stack (a Linux program that the build proves safe otherwise returns directly)
          
          [default: false]

      --emit.line-tables [<emit.line-tables>]
          give the lifted C of a host build line tables, so a crash names the guest function; false compiles it about 40% faster
          
          [default: true]

      --package.appimage <package.appimage>
          the desktop release output: an AppImage on Linux, a signed .app on macOS. auto (an AppImage when appimagetool is found), always (a Linux build without appimagetool fails) or never (no desktop package; the Android APK is always built)
          
          [default: auto]

      --package.android-id <package.android-id>
          the application id of the Android APK (empty: dev.relift.game.<project folder>)

      --package.android-keystore <package.android-keystore>
          the keystore that signs the APK, with the key alias relift (empty: a debug key in the cache)

      --package.android-keystore-pass <package.android-keystore-pass>
          the password of the keystore and its key

      --package.notarize [<package.notarize>]
          notarize a macOS game build with Apple (needs toolchain.codesign-identity and the notarize-* keys)
          
          [default: false]

      --package.notarize-key <package.notarize-key>
          the App Store Connect API key file (.p8) that notarization signs in with

      --package.notarize-key-id <package.notarize-key-id>
          the key ID of the notarization API key

      --package.notarize-issuer <package.notarize-issuer>
          the issuer ID of the notarization API key

      --package.ios-id <package.ios-id>
          the bundle identifier of the iOS app (empty: dev.relift.game.<project folder>)

      --package.ios-identity <package.ios-identity>
          the keychain identity that signs the iOS app for a device (empty: the first Apple Development identity)

      --package.ios-data <package.ios-data>
          where an iOS app finds the game data: bundle (copied into the app, sealed by its signature) or documents (added later to the app's Documents, for example with relift push)
          
          [default: bundle]

      --package.ios-profile <package.ios-profile>
          the provisioning profile of the iOS app (empty: one from Xcode's profile folders that covers the bundle identifier)

  -h, --help
          Print help (see a summary with '-h')

  -V, --version
          Print version
```
<!-- /docgen:usage -->

### relift diff

<!-- docgen:usage crate=relift bin=relift args="help diff" -->
```text
Compare one lifted function with the reference interpreter in a live run.

Switch projects only.

Builds a dev build and runs it under gdb. At each call of the function the same guest function runs in the interpreter from the same registers and live guest memory, the lifted code runs with the other threads frozen, and the registers and every page the interpreter touched are compared. A run that reaches a system call or a runtime-provided function is reported and not compared. Exits 1 after a mismatch and 2 when no call was compared.

Usage: relift diff [OPTIONS] [FUNCTION]

Arguments:
  [FUNCTION]
          The function: a host name (`_ZN5guest...`), `module:symbol` (the guest's mangled symbol) or `module+0xoffset` for an unnamed one. With `--serve`, nothing

Options:
      --count <COUNT>
          How many calls to compare
          
          [default: 1]

  -q, --quiet
          Print only the final summary and errors.
          
          A flag of its own, not a setting: configulator's boolean flags take an optional value and would swallow the subcommand in `relift -q build`.

      --skip <SKIP>
          How many calls to let pass first
          
          [default: 0]

  -v, --verbose
          Also print each file as it compiles and every instruction that does not lift

      --engine <COMMAND>
          Another engine to compare against, a program that speaks the protocol of `--serve` (docs/abi.md, "Differential checks"), such as the Unicorn one in check/

      --serve
          Speak the protocol on standard input and output as the interpreter engine, for a driver that runs the program

      --flip-read <N>
          Flip a bit of the Nth memory read of the interpreter run, so a correct build must report a mismatch (a control for the tool)
          
          [default: 0]

  -c, --config <config>
          config file
          
          [default: relift.toml]

      --cache-dir <cache-dir>
          where lifted, emitted and built artifacts are cached (empty: the user cache dir)

  -j, --jobs <jobs>
          worker threads for every parallel stage of a build: analysis, emit and compile (0: one per CPU)
          
          [default: 0]

      --runtime-dir <runtime-dir>
          the C runtime sources to build against (empty: the ones beside this relift's source)

      --color <color>
          color in output: auto (a terminal that shows it, and NO_COLOR unset), always or never
          
          [default: auto]

      --shader-math <shader-math>
          the GPU float math of the shader tables a build makes for this machine: exact (the console's results), or native or both (also the table of the host GPU's own math, which a run uses with gpu.math=native). Packages and phone targets always carry both
          
          [default: exact]

      --toolchain.clang <toolchain.clang>
          the clang driver

      --toolchain.zig <toolchain.zig>
          zig, for `zig cc` player builds

      --toolchain.ninja <toolchain.ninja>
          the ninja build tool

      --toolchain.appimagetool <toolchain.appimagetool>
          appimagetool, which packs the Linux release output

      --toolchain.android-sdk <toolchain.android-sdk>
          the Android SDK with build-tools and platforms (empty: ANDROID_HOME, then common places)

      --toolchain.android-ndk <toolchain.android-ndk>
          the Android NDK (empty: ANDROID_NDK_HOME, then the SDK's newest)

      --toolchain.codesign-identity <toolchain.codesign-identity>
          the identity that signs macOS game builds, such as a Developer ID Application certificate name or its SHA-1 (empty: an ad-hoc signature)

      --toolchain.codesign-keychain <toolchain.codesign-keychain>
          the keychain that holds the signing identity (empty: the default keychain search list)

      --emit.shard-kib <emit.shard-kib>
          the most KiB of C per emitted file; clang needs about 120 bytes of memory per byte of C
          
          [default: 4096]

      --emit.piece-kib <emit.piece-kib>
          the most KiB of C per C function; a larger guest function is split into several
          
          [default: 1024]

      --emit.cold [<emit.cold>]
          also emit a cold copy of every function that dispatch enters at any instruction, so no guest code is missed
          
          [default: true]

      --emit.cold-piece-kib <emit.cold-piece-kib>
          the most KiB of C per C function of the cold copy
          
          [default: 4]

      --emit.cold-first [<emit.cold-first>]
          for tests: run every function from the cold copy, which then calls only itself
          
          [default: false]

      --emit.fpcr-guards [<emit.fpcr-guards>]
          for debugging: keep the FPCR guards of lifted code even when the build proves the FP mode constant (RELIFT_FP_EXACT=1 needs them)
          
          [default: false]

      --emit.trampoline [<emit.trampoline>]
          make every guest return and indirect branch go through dispatch, so the host stack never mirrors the guest stack (a Linux program that the build proves safe otherwise returns directly)
          
          [default: false]

      --emit.line-tables [<emit.line-tables>]
          give the lifted C of a host build line tables, so a crash names the guest function; false compiles it about 40% faster
          
          [default: true]

      --package.appimage <package.appimage>
          the desktop release output: an AppImage on Linux, a signed .app on macOS. auto (an AppImage when appimagetool is found), always (a Linux build without appimagetool fails) or never (no desktop package; the Android APK is always built)
          
          [default: auto]

      --package.android-id <package.android-id>
          the application id of the Android APK (empty: dev.relift.game.<project folder>)

      --package.android-keystore <package.android-keystore>
          the keystore that signs the APK, with the key alias relift (empty: a debug key in the cache)

      --package.android-keystore-pass <package.android-keystore-pass>
          the password of the keystore and its key

      --package.notarize [<package.notarize>]
          notarize a macOS game build with Apple (needs toolchain.codesign-identity and the notarize-* keys)
          
          [default: false]

      --package.notarize-key <package.notarize-key>
          the App Store Connect API key file (.p8) that notarization signs in with

      --package.notarize-key-id <package.notarize-key-id>
          the key ID of the notarization API key

      --package.notarize-issuer <package.notarize-issuer>
          the issuer ID of the notarization API key

      --package.ios-id <package.ios-id>
          the bundle identifier of the iOS app (empty: dev.relift.game.<project folder>)

      --package.ios-identity <package.ios-identity>
          the keychain identity that signs the iOS app for a device (empty: the first Apple Development identity)

      --package.ios-data <package.ios-data>
          where an iOS app finds the game data: bundle (copied into the app, sealed by its signature) or documents (added later to the app's Documents, for example with relift push)
          
          [default: bundle]

      --package.ios-profile <package.ios-profile>
          the provisioning profile of the iOS app (empty: one from Xcode's profile folders that covers the bundle identifier)

  -h, --help
          Print help (see a summary with '-h')

  -V, --version
          Print version
```
<!-- /docgen:usage -->

### relift where

<!-- docgen:usage crate=relift bin=relift args="help where" -->
```text
Map between a host address, a guest address and a symbol

Usage: relift where [OPTIONS] <WHAT>

Arguments:
  <WHAT>
          A host address, guest address or symbol

Options:
  -q, --quiet
          Print only the final summary and errors.
          
          A flag of its own, not a setting: configulator's boolean flags take an optional value and would swallow the subcommand in `relift -q build`.

  -v, --verbose
          Also print each file as it compiles and every instruction that does not lift

  -c, --config <config>
          config file
          
          [default: relift.toml]

      --cache-dir <cache-dir>
          where lifted, emitted and built artifacts are cached (empty: the user cache dir)

  -j, --jobs <jobs>
          worker threads for every parallel stage of a build: analysis, emit and compile (0: one per CPU)
          
          [default: 0]

      --runtime-dir <runtime-dir>
          the C runtime sources to build against (empty: the ones beside this relift's source)

      --color <color>
          color in output: auto (a terminal that shows it, and NO_COLOR unset), always or never
          
          [default: auto]

      --shader-math <shader-math>
          the GPU float math of the shader tables a build makes for this machine: exact (the console's results), or native or both (also the table of the host GPU's own math, which a run uses with gpu.math=native). Packages and phone targets always carry both
          
          [default: exact]

      --toolchain.clang <toolchain.clang>
          the clang driver

      --toolchain.zig <toolchain.zig>
          zig, for `zig cc` player builds

      --toolchain.ninja <toolchain.ninja>
          the ninja build tool

      --toolchain.appimagetool <toolchain.appimagetool>
          appimagetool, which packs the Linux release output

      --toolchain.android-sdk <toolchain.android-sdk>
          the Android SDK with build-tools and platforms (empty: ANDROID_HOME, then common places)

      --toolchain.android-ndk <toolchain.android-ndk>
          the Android NDK (empty: ANDROID_NDK_HOME, then the SDK's newest)

      --toolchain.codesign-identity <toolchain.codesign-identity>
          the identity that signs macOS game builds, such as a Developer ID Application certificate name or its SHA-1 (empty: an ad-hoc signature)

      --toolchain.codesign-keychain <toolchain.codesign-keychain>
          the keychain that holds the signing identity (empty: the default keychain search list)

      --emit.shard-kib <emit.shard-kib>
          the most KiB of C per emitted file; clang needs about 120 bytes of memory per byte of C
          
          [default: 4096]

      --emit.piece-kib <emit.piece-kib>
          the most KiB of C per C function; a larger guest function is split into several
          
          [default: 1024]

      --emit.cold [<emit.cold>]
          also emit a cold copy of every function that dispatch enters at any instruction, so no guest code is missed
          
          [default: true]

      --emit.cold-piece-kib <emit.cold-piece-kib>
          the most KiB of C per C function of the cold copy
          
          [default: 4]

      --emit.cold-first [<emit.cold-first>]
          for tests: run every function from the cold copy, which then calls only itself
          
          [default: false]

      --emit.fpcr-guards [<emit.fpcr-guards>]
          for debugging: keep the FPCR guards of lifted code even when the build proves the FP mode constant (RELIFT_FP_EXACT=1 needs them)
          
          [default: false]

      --emit.trampoline [<emit.trampoline>]
          make every guest return and indirect branch go through dispatch, so the host stack never mirrors the guest stack (a Linux program that the build proves safe otherwise returns directly)
          
          [default: false]

      --emit.line-tables [<emit.line-tables>]
          give the lifted C of a host build line tables, so a crash names the guest function; false compiles it about 40% faster
          
          [default: true]

      --package.appimage <package.appimage>
          the desktop release output: an AppImage on Linux, a signed .app on macOS. auto (an AppImage when appimagetool is found), always (a Linux build without appimagetool fails) or never (no desktop package; the Android APK is always built)
          
          [default: auto]

      --package.android-id <package.android-id>
          the application id of the Android APK (empty: dev.relift.game.<project folder>)

      --package.android-keystore <package.android-keystore>
          the keystore that signs the APK, with the key alias relift (empty: a debug key in the cache)

      --package.android-keystore-pass <package.android-keystore-pass>
          the password of the keystore and its key

      --package.notarize [<package.notarize>]
          notarize a macOS game build with Apple (needs toolchain.codesign-identity and the notarize-* keys)
          
          [default: false]

      --package.notarize-key <package.notarize-key>
          the App Store Connect API key file (.p8) that notarization signs in with

      --package.notarize-key-id <package.notarize-key-id>
          the key ID of the notarization API key

      --package.notarize-issuer <package.notarize-issuer>
          the issuer ID of the notarization API key

      --package.ios-id <package.ios-id>
          the bundle identifier of the iOS app (empty: dev.relift.game.<project folder>)

      --package.ios-identity <package.ios-identity>
          the keychain identity that signs the iOS app for a device (empty: the first Apple Development identity)

      --package.ios-data <package.ios-data>
          where an iOS app finds the game data: bundle (copied into the app, sealed by its signature) or documents (added later to the app's Documents, for example with relift push)
          
          [default: bundle]

      --package.ios-profile <package.ios-profile>
          the provisioning profile of the iOS app (empty: one from Xcode's profile folders that covers the bundle identifier)

  -h, --help
          Print help (see a summary with '-h')

  -V, --version
          Print version
```
<!-- /docgen:usage -->

### relift info

<!-- docgen:usage crate=relift bin=relift args="help info" -->
```text
Print the self-description of a built game binary, without running it: the JSON document its `--relift-info` flag prints

Usage: relift info [OPTIONS] [BINARY]

Arguments:
  [BINARY]
          The binary to read. Default: the project's last build

Options:
  -q, --quiet
          Print only the final summary and errors.
          
          A flag of its own, not a setting: configulator's boolean flags take an optional value and would swallow the subcommand in `relift -q build`.

  -v, --verbose
          Also print each file as it compiles and every instruction that does not lift

  -c, --config <config>
          config file
          
          [default: relift.toml]

      --cache-dir <cache-dir>
          where lifted, emitted and built artifacts are cached (empty: the user cache dir)

  -j, --jobs <jobs>
          worker threads for every parallel stage of a build: analysis, emit and compile (0: one per CPU)
          
          [default: 0]

      --runtime-dir <runtime-dir>
          the C runtime sources to build against (empty: the ones beside this relift's source)

      --color <color>
          color in output: auto (a terminal that shows it, and NO_COLOR unset), always or never
          
          [default: auto]

      --shader-math <shader-math>
          the GPU float math of the shader tables a build makes for this machine: exact (the console's results), or native or both (also the table of the host GPU's own math, which a run uses with gpu.math=native). Packages and phone targets always carry both
          
          [default: exact]

      --toolchain.clang <toolchain.clang>
          the clang driver

      --toolchain.zig <toolchain.zig>
          zig, for `zig cc` player builds

      --toolchain.ninja <toolchain.ninja>
          the ninja build tool

      --toolchain.appimagetool <toolchain.appimagetool>
          appimagetool, which packs the Linux release output

      --toolchain.android-sdk <toolchain.android-sdk>
          the Android SDK with build-tools and platforms (empty: ANDROID_HOME, then common places)

      --toolchain.android-ndk <toolchain.android-ndk>
          the Android NDK (empty: ANDROID_NDK_HOME, then the SDK's newest)

      --toolchain.codesign-identity <toolchain.codesign-identity>
          the identity that signs macOS game builds, such as a Developer ID Application certificate name or its SHA-1 (empty: an ad-hoc signature)

      --toolchain.codesign-keychain <toolchain.codesign-keychain>
          the keychain that holds the signing identity (empty: the default keychain search list)

      --emit.shard-kib <emit.shard-kib>
          the most KiB of C per emitted file; clang needs about 120 bytes of memory per byte of C
          
          [default: 4096]

      --emit.piece-kib <emit.piece-kib>
          the most KiB of C per C function; a larger guest function is split into several
          
          [default: 1024]

      --emit.cold [<emit.cold>]
          also emit a cold copy of every function that dispatch enters at any instruction, so no guest code is missed
          
          [default: true]

      --emit.cold-piece-kib <emit.cold-piece-kib>
          the most KiB of C per C function of the cold copy
          
          [default: 4]

      --emit.cold-first [<emit.cold-first>]
          for tests: run every function from the cold copy, which then calls only itself
          
          [default: false]

      --emit.fpcr-guards [<emit.fpcr-guards>]
          for debugging: keep the FPCR guards of lifted code even when the build proves the FP mode constant (RELIFT_FP_EXACT=1 needs them)
          
          [default: false]

      --emit.trampoline [<emit.trampoline>]
          make every guest return and indirect branch go through dispatch, so the host stack never mirrors the guest stack (a Linux program that the build proves safe otherwise returns directly)
          
          [default: false]

      --emit.line-tables [<emit.line-tables>]
          give the lifted C of a host build line tables, so a crash names the guest function; false compiles it about 40% faster
          
          [default: true]

      --package.appimage <package.appimage>
          the desktop release output: an AppImage on Linux, a signed .app on macOS. auto (an AppImage when appimagetool is found), always (a Linux build without appimagetool fails) or never (no desktop package; the Android APK is always built)
          
          [default: auto]

      --package.android-id <package.android-id>
          the application id of the Android APK (empty: dev.relift.game.<project folder>)

      --package.android-keystore <package.android-keystore>
          the keystore that signs the APK, with the key alias relift (empty: a debug key in the cache)

      --package.android-keystore-pass <package.android-keystore-pass>
          the password of the keystore and its key

      --package.notarize [<package.notarize>]
          notarize a macOS game build with Apple (needs toolchain.codesign-identity and the notarize-* keys)
          
          [default: false]

      --package.notarize-key <package.notarize-key>
          the App Store Connect API key file (.p8) that notarization signs in with

      --package.notarize-key-id <package.notarize-key-id>
          the key ID of the notarization API key

      --package.notarize-issuer <package.notarize-issuer>
          the issuer ID of the notarization API key

      --package.ios-id <package.ios-id>
          the bundle identifier of the iOS app (empty: dev.relift.game.<project folder>)

      --package.ios-identity <package.ios-identity>
          the keychain identity that signs the iOS app for a device (empty: the first Apple Development identity)

      --package.ios-data <package.ios-data>
          where an iOS app finds the game data: bundle (copied into the app, sealed by its signature) or documents (added later to the app's Documents, for example with relift push)
          
          [default: bundle]

      --package.ios-profile <package.ios-profile>
          the provisioning profile of the iOS app (empty: one from Xcode's profile folders that covers the bundle identifier)

  -h, --help
          Print help (see a summary with '-h')

  -V, --version
          Print version
```
<!-- /docgen:usage -->

### relift coverage

<!-- docgen:usage crate=relift bin=relift args="help coverage" -->
```text
List what discovery could not resolve: per module, functions, instructions not lifted and every unresolved indirect call or jump, with hints from earlier hot-load runs. Switch projects only; for a Game Boy ROM use `relift-gbrom functions`

Usage: relift coverage [OPTIONS]

Options:
      --json
          Print JSON instead of text

  -q, --quiet
          Print only the final summary and errors.
          
          A flag of its own, not a setting: configulator's boolean flags take an optional value and would swallow the subcommand in `relift -q build`.

      --module <NAME>
          Report only the module of this name

  -v, --verbose
          Also print each file as it compiles and every instruction that does not lift

  -c, --config <config>
          config file
          
          [default: relift.toml]

      --cache-dir <cache-dir>
          where lifted, emitted and built artifacts are cached (empty: the user cache dir)

  -j, --jobs <jobs>
          worker threads for every parallel stage of a build: analysis, emit and compile (0: one per CPU)
          
          [default: 0]

      --runtime-dir <runtime-dir>
          the C runtime sources to build against (empty: the ones beside this relift's source)

      --color <color>
          color in output: auto (a terminal that shows it, and NO_COLOR unset), always or never
          
          [default: auto]

      --shader-math <shader-math>
          the GPU float math of the shader tables a build makes for this machine: exact (the console's results), or native or both (also the table of the host GPU's own math, which a run uses with gpu.math=native). Packages and phone targets always carry both
          
          [default: exact]

      --toolchain.clang <toolchain.clang>
          the clang driver

      --toolchain.zig <toolchain.zig>
          zig, for `zig cc` player builds

      --toolchain.ninja <toolchain.ninja>
          the ninja build tool

      --toolchain.appimagetool <toolchain.appimagetool>
          appimagetool, which packs the Linux release output

      --toolchain.android-sdk <toolchain.android-sdk>
          the Android SDK with build-tools and platforms (empty: ANDROID_HOME, then common places)

      --toolchain.android-ndk <toolchain.android-ndk>
          the Android NDK (empty: ANDROID_NDK_HOME, then the SDK's newest)

      --toolchain.codesign-identity <toolchain.codesign-identity>
          the identity that signs macOS game builds, such as a Developer ID Application certificate name or its SHA-1 (empty: an ad-hoc signature)

      --toolchain.codesign-keychain <toolchain.codesign-keychain>
          the keychain that holds the signing identity (empty: the default keychain search list)

      --emit.shard-kib <emit.shard-kib>
          the most KiB of C per emitted file; clang needs about 120 bytes of memory per byte of C
          
          [default: 4096]

      --emit.piece-kib <emit.piece-kib>
          the most KiB of C per C function; a larger guest function is split into several
          
          [default: 1024]

      --emit.cold [<emit.cold>]
          also emit a cold copy of every function that dispatch enters at any instruction, so no guest code is missed
          
          [default: true]

      --emit.cold-piece-kib <emit.cold-piece-kib>
          the most KiB of C per C function of the cold copy
          
          [default: 4]

      --emit.cold-first [<emit.cold-first>]
          for tests: run every function from the cold copy, which then calls only itself
          
          [default: false]

      --emit.fpcr-guards [<emit.fpcr-guards>]
          for debugging: keep the FPCR guards of lifted code even when the build proves the FP mode constant (RELIFT_FP_EXACT=1 needs them)
          
          [default: false]

      --emit.trampoline [<emit.trampoline>]
          make every guest return and indirect branch go through dispatch, so the host stack never mirrors the guest stack (a Linux program that the build proves safe otherwise returns directly)
          
          [default: false]

      --emit.line-tables [<emit.line-tables>]
          give the lifted C of a host build line tables, so a crash names the guest function; false compiles it about 40% faster
          
          [default: true]

      --package.appimage <package.appimage>
          the desktop release output: an AppImage on Linux, a signed .app on macOS. auto (an AppImage when appimagetool is found), always (a Linux build without appimagetool fails) or never (no desktop package; the Android APK is always built)
          
          [default: auto]

      --package.android-id <package.android-id>
          the application id of the Android APK (empty: dev.relift.game.<project folder>)

      --package.android-keystore <package.android-keystore>
          the keystore that signs the APK, with the key alias relift (empty: a debug key in the cache)

      --package.android-keystore-pass <package.android-keystore-pass>
          the password of the keystore and its key

      --package.notarize [<package.notarize>]
          notarize a macOS game build with Apple (needs toolchain.codesign-identity and the notarize-* keys)
          
          [default: false]

      --package.notarize-key <package.notarize-key>
          the App Store Connect API key file (.p8) that notarization signs in with

      --package.notarize-key-id <package.notarize-key-id>
          the key ID of the notarization API key

      --package.notarize-issuer <package.notarize-issuer>
          the issuer ID of the notarization API key

      --package.ios-id <package.ios-id>
          the bundle identifier of the iOS app (empty: dev.relift.game.<project folder>)

      --package.ios-identity <package.ios-identity>
          the keychain identity that signs the iOS app for a device (empty: the first Apple Development identity)

      --package.ios-data <package.ios-data>
          where an iOS app finds the game data: bundle (copied into the app, sealed by its signature) or documents (added later to the app's Documents, for example with relift push)
          
          [default: bundle]

      --package.ios-profile <package.ios-profile>
          the provisioning profile of the iOS app (empty: one from Xcode's profile folders that covers the bundle identifier)

  -h, --help
          Print help (see a summary with '-h')

  -V, --version
          Print version
```
<!-- /docgen:usage -->

### relift report

<!-- docgen:usage crate=relift bin=relift args="help report" -->
```text
Explain a stop report a shipped build wrote (missed code, an instruction it could not run) and add the missed target as a root for the next build.

Prints the transfer site and target with the guest instructions around them, the site's lifted function and its C at the site, and what the build knew about the target. Run in the project directory to read its build and add the root (a hint in relift.toml for a Switch project, the learned file for a Game Boy one).

Usage: relift report [OPTIONS] <FILE>

Arguments:
  <FILE>
          The report file (`relift-stop-*.txt`)

Options:
      --build-dir <DIR>
          The build directory to read instead of the project's

  -q, --quiet
          Print only the final summary and errors.
          
          A flag of its own, not a setting: configulator's boolean flags take an optional value and would swallow the subcommand in `relift -q build`.

      --no-hint
          Print only; add no root

  -v, --verbose
          Also print each file as it compiles and every instruction that does not lift

  -c, --config <config>
          config file
          
          [default: relift.toml]

      --cache-dir <cache-dir>
          where lifted, emitted and built artifacts are cached (empty: the user cache dir)

  -j, --jobs <jobs>
          worker threads for every parallel stage of a build: analysis, emit and compile (0: one per CPU)
          
          [default: 0]

      --runtime-dir <runtime-dir>
          the C runtime sources to build against (empty: the ones beside this relift's source)

      --color <color>
          color in output: auto (a terminal that shows it, and NO_COLOR unset), always or never
          
          [default: auto]

      --shader-math <shader-math>
          the GPU float math of the shader tables a build makes for this machine: exact (the console's results), or native or both (also the table of the host GPU's own math, which a run uses with gpu.math=native). Packages and phone targets always carry both
          
          [default: exact]

      --toolchain.clang <toolchain.clang>
          the clang driver

      --toolchain.zig <toolchain.zig>
          zig, for `zig cc` player builds

      --toolchain.ninja <toolchain.ninja>
          the ninja build tool

      --toolchain.appimagetool <toolchain.appimagetool>
          appimagetool, which packs the Linux release output

      --toolchain.android-sdk <toolchain.android-sdk>
          the Android SDK with build-tools and platforms (empty: ANDROID_HOME, then common places)

      --toolchain.android-ndk <toolchain.android-ndk>
          the Android NDK (empty: ANDROID_NDK_HOME, then the SDK's newest)

      --toolchain.codesign-identity <toolchain.codesign-identity>
          the identity that signs macOS game builds, such as a Developer ID Application certificate name or its SHA-1 (empty: an ad-hoc signature)

      --toolchain.codesign-keychain <toolchain.codesign-keychain>
          the keychain that holds the signing identity (empty: the default keychain search list)

      --emit.shard-kib <emit.shard-kib>
          the most KiB of C per emitted file; clang needs about 120 bytes of memory per byte of C
          
          [default: 4096]

      --emit.piece-kib <emit.piece-kib>
          the most KiB of C per C function; a larger guest function is split into several
          
          [default: 1024]

      --emit.cold [<emit.cold>]
          also emit a cold copy of every function that dispatch enters at any instruction, so no guest code is missed
          
          [default: true]

      --emit.cold-piece-kib <emit.cold-piece-kib>
          the most KiB of C per C function of the cold copy
          
          [default: 4]

      --emit.cold-first [<emit.cold-first>]
          for tests: run every function from the cold copy, which then calls only itself
          
          [default: false]

      --emit.fpcr-guards [<emit.fpcr-guards>]
          for debugging: keep the FPCR guards of lifted code even when the build proves the FP mode constant (RELIFT_FP_EXACT=1 needs them)
          
          [default: false]

      --emit.trampoline [<emit.trampoline>]
          make every guest return and indirect branch go through dispatch, so the host stack never mirrors the guest stack (a Linux program that the build proves safe otherwise returns directly)
          
          [default: false]

      --emit.line-tables [<emit.line-tables>]
          give the lifted C of a host build line tables, so a crash names the guest function; false compiles it about 40% faster
          
          [default: true]

      --package.appimage <package.appimage>
          the desktop release output: an AppImage on Linux, a signed .app on macOS. auto (an AppImage when appimagetool is found), always (a Linux build without appimagetool fails) or never (no desktop package; the Android APK is always built)
          
          [default: auto]

      --package.android-id <package.android-id>
          the application id of the Android APK (empty: dev.relift.game.<project folder>)

      --package.android-keystore <package.android-keystore>
          the keystore that signs the APK, with the key alias relift (empty: a debug key in the cache)

      --package.android-keystore-pass <package.android-keystore-pass>
          the password of the keystore and its key

      --package.notarize [<package.notarize>]
          notarize a macOS game build with Apple (needs toolchain.codesign-identity and the notarize-* keys)
          
          [default: false]

      --package.notarize-key <package.notarize-key>
          the App Store Connect API key file (.p8) that notarization signs in with

      --package.notarize-key-id <package.notarize-key-id>
          the key ID of the notarization API key

      --package.notarize-issuer <package.notarize-issuer>
          the issuer ID of the notarization API key

      --package.ios-id <package.ios-id>
          the bundle identifier of the iOS app (empty: dev.relift.game.<project folder>)

      --package.ios-identity <package.ios-identity>
          the keychain identity that signs the iOS app for a device (empty: the first Apple Development identity)

      --package.ios-data <package.ios-data>
          where an iOS app finds the game data: bundle (copied into the app, sealed by its signature) or documents (added later to the app's Documents, for example with relift push)
          
          [default: bundle]

      --package.ios-profile <package.ios-profile>
          the provisioning profile of the iOS app (empty: one from Xcode's profile folders that covers the bundle identifier)

  -h, --help
          Print help (see a summary with '-h')

  -V, --version
          Print version
```
<!-- /docgen:usage -->

## Tools

### relift-nup

<!-- docgen:usage crate=relift-switch-tools bin=relift-nup -->
```text
List and extract NUP containers

Usage: relift-nup <COMMAND>

Commands:
  ls       List the packages and their files
  extract  Extract one package, or the effective title, verifying every file
  romfs    Write the RomFS map of the effective title's romfs (or one DLC's) into a directory: `romfs.map`, which reads the files in place in the `.nup`, and `romfs.blob` for the zstd files, after checking every file's SHA-256. The runtime serves it as the game's romfs (`RELIFT_ROMFS=<dir>/romfs.map`) without extracting a file
  help     Print this message or the help of the given subcommand(s)

Options:
  -h, --help     Print help
  -V, --version  Print version
```
<!-- /docgen:usage -->

### relift-romfs

<!-- docgen:usage crate=relift-switch-tools bin=relift-romfs -->
```text
Work with a decrypted romfs: an image, a RomFS map or an extracted tree

Usage: relift-romfs <COMMAND>

Commands:
  ls       List the files: path, size
  extract  Write the whole tree into a directory
  cat      Write one file to stdout
  hash     Print the SHA-256 of every file (`<sha256>  <size>  <path>`, sorted by path) and last of the whole image, so two forms of one romfs (a tree and the map of a `.nup`) can be compared
  image    Write the romfs as one plain image file (what a phone gets)
  help     Print this message or the help of the given subcommand(s)

Options:
  -h, --help     Print help
  -V, --version  Print version
```
<!-- /docgen:usage -->

### relift-nso

<!-- docgen:usage crate=relift-switch-tools bin=relift-nso -->
```text
Inspect an NSO executable module

Usage: relift-nso <COMMAND>

Commands:
  show        Print the module name and path, build ID, segments, bss, MOD0 and the symbol count
  symbols     List the dynamic symbols: name, value, size, type, binding, defined
  decompress  Write the NSO uncompressed (hactool layout), or flat with --flat
  help        Print this message or the help of the given subcommand(s)

Options:
  -h, --help     Print help
  -V, --version  Print version
```
<!-- /docgen:usage -->

### relift-nro

<!-- docgen:usage crate=relift-switch-tools bin=relift-nro -->
```text
Inspect and unpack NRO executables

Usage: relift-nro <COMMAND>

Commands:
  show     Describe an NRO
  extract  Write the segments, the flat image and the assets to a directory
  help     Print this message or the help of the given subcommand(s)

Options:
  -h, --help     Print help
  -V, --version  Print version
```
<!-- /docgen:usage -->

### relift-npdm

<!-- docgen:usage crate=relift-switch-tools bin=relift-npdm -->
```text
Dump an NPDM as JSON

Usage: relift-npdm <INPUT>

Arguments:
  <INPUT>  The NPDM file, or - for stdin

Options:
  -h, --help     Print help
  -V, --version  Print version
```
<!-- /docgen:usage -->

### relift-ir

<!-- docgen:usage crate=relift-switch-tools bin=relift-ir -->
```text
Print the IR of A64 instructions

Usage: relift-ir [OPTIONS] [WORDS]...

Arguments:
  [WORDS]...  Instruction words in hex (`0x` optional), lifted at `--pc` onward

Options:
      --pc <PC>              The address of the first word [default: 0x1000]
      --nso <NSO>            Take the instructions from this NSO (an image offset is an address)
      --sym <SYM>            With `--nso`: lift the symbol of this name, over its size
      --addr <ADDR>          With `--nso`: lift from this address
      --count <COUNT>        With `--addr`, or a symbol of unknown size: how many instructions [default: 16]
      --features <FEATURES>  The CPU's architecture features: a base (`cortex-a57`, `armv8.0`, `all`) and features such as `FEAT_LSE`, comma-separated [default: cortex-a57]
      --dce                  Remove dead values before printing
      --grep <GREP>          Print only the instruction lines that contain this text, with each block's header and exit line
      --json                 Print JSON
  -h, --help                 Print help
  -V, --version              Print version
```
<!-- /docgen:usage -->

### relift-vkgen

<!-- docgen:usage crate=relift-switch-vkgen bin=relift-vkgen -->
```text
Generate the Switch runtime's Vulkan forwarding layer from vk.xml

Usage: relift-vkgen [OPTIONS] --xml <XML> --config <CONFIG> --out <OUT>

Options:
      --xml <XML>        The registry file (vk.xml at the pinned tag)
      --config <CONFIG>  Configuration files, merged in order (lists concatenate)
      --out <OUT>        The output directory
      --check            Compare with the files in the output directory instead of writing; exit 1 on any difference
  -h, --help             Print help
  -V, --version          Print version
```
<!-- /docgen:usage -->

### relift-shaders

<!-- docgen:usage crate=relift-switch-shaders bin=relift-shaders -->
```text
Find every NVIDIA shader program of a game

Usage: relift-shaders <COMMAND>

Commands:
  scan  Scan a `.nup`, an extracted `exefs`/`romfs` tree or an import folder
  help  Print this message or the help of the given subcommand(s)

Options:
  -h, --help     Print help
  -V, --version  Print version
```
<!-- /docgen:usage -->

### relift-gbrom

<!-- docgen:usage crate=relift-gb-tools bin=relift-gbrom -->
```text
Show a ROM's cartridge header and check its logo, checksums and size

Usage: relift-gbrom [OPTIONS] <ROM>
       relift-gbrom <COMMAND>

Commands:
  functions  Find the functions of a ROM across its mapper banks: per bank, with the transfers that cannot be resolved and the code run from RAM
  ir         Print the IR of SM83 instructions given as hex bytes, as the Game Boy guest lifts them
  help       Print this message or the help of the given subcommand(s)

Arguments:
  <ROM>  The ROM image, or - for stdin

Options:
      --json     Print JSON
      --check    Exit with code 1 when any check fails
  -h, --help     Print help
  -V, --version  Print version
```
<!-- /docgen:usage -->

### relift-gbarom

<!-- docgen:usage crate=relift-gba-tools bin=relift-gbarom -->
```text
Show a ROM's cartridge header, check its logo hash, complement and size, and list the save library strings it carries and the multiboot entry of an image that fits work RAM

Usage: relift-gbarom [OPTIONS] <ROM>
       relift-gbarom <COMMAND>

Commands:
  functions  Find the functions of a ROM across ARM and Thumb code, with the code copied to work RAM and the transfers that cannot be resolved
  help       Print this message or the help of the given subcommand(s)

Arguments:
  <ROM>  The ROM image, or - for stdin

Options:
      --json               Print JSON
      --check              Exit with code 1 when any check fails
      --logo-sha256 <HEX>  The SHA-256 of the logo field to compare against, 64 hex digits; default is the hash of the real logo
  -h, --help               Print help
  -V, --version            Print version
```
<!-- /docgen:usage -->

### relift-n64rom

<!-- docgen:usage crate=relift-n64-tools bin=relift-n64rom -->
```text
Inspect and normalize Nintendo 64 ROM images. Reads bytes only; the boot code is hashed to name the variant and never interpreted

Usage: relift-n64rom <COMMAND>

Commands:
  info       Show the byte order, header, boot variant and checksum check
  streams    List the Yay0, Yaz0 and MIO0 streams that decode completely
  functions  Find the code: function entries, code units by source and link address, RSP images and what stays unknown
  ir         Print the IR of MIPS or RSP instruction words, as the Nintendo 64 guest lifts them
  normalize  Write the image as big-endian (`.z64`)
  help       Print this message or the help of the given subcommand(s)

Options:
  -h, --help     Print help
  -V, --version  Print version
```
<!-- /docgen:usage -->

### relift-ndsrom

<!-- docgen:usage crate=relift-nds-tools bin=relift-ndsrom -->
```text
Inspect a decrypted Nintendo DS ROM

Usage: relift-ndsrom <COMMAND>

Commands:
  header     Show the cartridge header and the results of its checks
  units      List the static binaries, autoload blocks and overlays with their addresses, sizes, compression and the SHA-256 of each decompressed unit
  extract    Write every decompressed unit into a directory, with a units.json
  files      List the files of the file system with their sizes
  functions  Find the functions of every unit: one discovery run per static binary and per overlay, repeated while calls between units give new entries
  help       Print this message or the help of the given subcommand(s)

Options:
  -h, --help     Print help
  -V, --version  Print version
```
<!-- /docgen:usage -->

### relift-smsrom

<!-- docgen:usage crate=relift-sms-tools bin=relift-smsrom -->
```text
Inspect Master System, Game Gear and SG-1000 ROM images

Usage: relift-smsrom <COMMAND>

Commands:
  info        Print the size, header, checksums, system and mapper guess
  lift-trace  Development: lift the instructions an ares trace executed and write the C of the program into a directory
  help        Print this message or the help of the given subcommand(s)

Options:
  -h, --help     Print help
  -V, --version  Print version
```
<!-- /docgen:usage -->
