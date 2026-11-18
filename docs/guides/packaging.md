# Packaging

What `relift build` makes for each host, and where the game's title and icon
come from. The settings named here (`package.*`, `toolchain.*`) are listed in
[Settings](../reference/configuration.md).

| Host | `relift build` output |
|---|---|
| Linux | the program and `<project>.AppImage` beside it |
| macOS | `<project>.app`, signed, optionally notarized |
| Android (`--target android-arm64` or `android-x64`) | a signed APK |
| iOS (`--target ios-arm64` or `ios-sim-arm64`) | an app signed for a device, or ad hoc for the simulator |

`relift run` and the dev build are not packaged.

## Game title and icon

Where the game carries them, they become the game's own on every host. A
Switch title's control data gives the title in each language and the icon
(a JPEG, converted to PNG once at build time); the identity section stores
both (`identity.names`, `identity.default_language`, `identity.icon` with its
format, width and height), so a launcher reads them without running the
game, and the program registers the same bytes with the runtime at start-up.
The SDL3 window then takes the title in the language of the `language`
setting (`RELIFT_LANGUAGE`), else of the host's language settings, falling
back to the default language, and the game's icon as its window icon (Linux
under X11 and Wayland, macOS, where it is also the Dock icon, and Windows).
On Windows the program also links the icon and the title as its icon and
version resources. The AppImage's desktop entry and icon, the Android
launcher label and icon (`relift::icon::launcher_pngs`), the iOS home screen
icons and the name and `.icns` icon of the macOS `.app` come from the same
data.

A cartridge header holds neither a readable title nor an image, so a Game
Boy, Game Boy Color or Game Boy Advance program is titled with the project
name, its window has no icon, and a Windows executable keeps the system's
default one. Only the packages that need some icon (the AppImage desktop
entry, the Android launcher, the iOS home screen and the macOS `.app`) get a
generated one.

## Linux: AppImage

On Linux `relift build` also writes `<project>.AppImage` beside the program:
one executable that runs on a mainstream distribution with nothing installed
but glibc and the system's graphics stack. It holds the program, the shared
libraries it needs that a distribution cannot be relied on to have (SDL3), a
desktop file and icon, the self-description (`relift info <game>.AppImage`
reads it without running the game) and the license of everything bundled
(`usr/share/licenses/<game>/BUNDLED.txt`). The Vulkan loader, GPU drivers,
the C library and the display and audio servers are always the system's. A
Switch game's romfs and DLC go inside as plain images (`romfs.img`); saves,
settings and the SD card go to `$XDG_DATA_HOME/<game>` (default
`~/.local/share/<game>`), never into the AppImage.

It needs `appimagetool` on `PATH` (or `toolchain.appimagetool`); without it
the build prints how to get it and still finishes. The AppImage runtime (the
start of the file; MIT, from github.com/AppImage/type2-runtime) is pinned by
release tag and SHA-256 in `relift/src/appimage_runtime.rs`. The first
packaging build fetches it into `<cache>/sdk/appimage-runtime/<tag>/<arch>`,
checks the hash and moves it into place in one step; later builds, on any
number of processes at once, use the cached file with no network. On a
machine with no network, copy that file there from another. Its license is
listed in `BUNDLED.txt` and shipped as `type2-runtime-LICENSE`. Test and
corpus runs that exercise the program, not the package, set
`RELIFT_PACKAGE_APPIMAGE=never`. `package.appimage` is `auto` (the default),
`always` (a missing tool fails the build) or `never`.

To make the AppImage run on distributions older than the build machine, run
`runtime/tools/linux-sdk.sh` once. It builds SDL3 for glibc 2.28 into the
cache; `relift build` then compiles with `zig cc` for that glibc and links
that SDL3, and the AppImage asks for glibc 2.28 or newer. Without it the
program is built with the host compiler and asks for the host's glibc (the
build summary prints it).

## macOS: app bundle

The program goes into `Contents/MacOS` behind a launcher script that sets
the per-user folders; the libraries it links (SDL3, MoltenVK and what they
link, from outside `/usr/lib` and `/System`) go into `Contents/Frameworks`
with their install names rewritten to `@rpath`, and a Switch game's `romfs/`
and `dlc/` go into `Contents/Resources`. Everything is signed inside out:
with `toolchain.codesign-identity` under the hardened runtime and a secure
timestamp, or ad hoc when there is none. The program needs no entitlement.
`package.notarize` submits a zip of the bundle with the `package.notarize-*`
API key and staples the ticket to the bundle, so it opens offline.

## Android: APK

The program is a shared library, `libmain.so`, that SDL's Java activity
loads and enters through `SDL_main`. relift drives the Android SDK's own
tools (`aapt2`, `d8`, `zipalign`, `apksigner`) and `javac`, with no Gradle;
`toolchain.android-sdk` and `toolchain.android-ndk` find them. The APK
needs Android 11 (API 30) or newer. It is signed with
`package.android-keystore`, else a debug key in the cache, under the
application id `package.android-id`. The app has settings pages, including
the choice of a custom Vulkan driver package. Game Boy and GBA apps keep the
battery save in the app's own folder and show the on-screen controller on
the touch screen. A Nintendo DS app holds the whole cartridge image in
`libmain.so` (stored uncompressed and page aligned in the APK, so one file is
the game), keeps its save in the app's own folder (`battery.sav`, also written
when the app goes to the background), turns with the device (portrait and
landscape, live), draws the two screens by the `video.nds_layout` setting and
takes touches on the bottom screen's picture as the pen, wherever it is drawn
and scaled. A device with a second display (a dual-screen handheld, a
foldable, a phone with a connected display) shows the top screen on the first
display and the bottom screen on the second through an Android Presentation,
by the `video.nds_displays` setting, with touches on the second display as the
pen ([input reference](../reference/input.md#nintendo-ds)). A Switch game's data is not in the APK: `relift push
--target android-arm64` copies it to the device.

## iOS: app

The program is one statically linked executable (iOS loads no code besides
the system's), handed to SDL's UIKit glue. A device build is signed with
`package.ios-identity` (else the first Apple Development identity) and
`package.ios-profile` (else a profile from Xcode's folders that covers
`package.ios-id`); a simulator build is signed ad hoc. A Switch game's data
goes into the bundle (`package.ios-data = "bundle"`, the default) or into
the app's Documents (`documents`), copied there with `relift push --target
ios-arm64`.

## Data on phones

Android and iOS apps of a Switch game check the game data against the app's
`data-manifest.txt` at start (sizes, and SHA-256 with
`RELIFT_DATA_VERIFY=hash`; [the C runtime](../../runtime/README.md)); a missing or short file
stops the app with a message naming it. `relift push` copies the data and
checks it the same way.
