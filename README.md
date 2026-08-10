# adbfsplugin for macOS (Double Commander)

A macOS port of [sztupy/adbfsplugin](https://github.com/sztupy/adbfsplugin) — a
file system (WFX) plugin that browses Android devices over ADB. Originally
written for Total Commander on Windows; this port targets **Double Commander**
on macOS, which implements the same plugin API.

The plugin connects to the local ADB server, opens a shell on the device, and
drives it with `busybox` commands. File transfers run in-band through the shell
using `uuencode`/`uudecode` (base64), so it works even on devices where `adb`
lacks direct file access to the paths you're browsing (e.g. with root via `su`).

## Requirements

- [Double Commander](https://doublecmd.sourceforge.io/) for macOS
- `adb` (Android platform tools), e.g. `brew install android-platform-tools`
- An Android device with **USB debugging** enabled and **busybox** installed
  (the plugin runs `su` on connect by default; set `ADBFS_NO_SU=1` to skip it
  on unrooted devices)

## Install

Option A — from the release zip:

1. In Double Commander, open the `adbfsplugin-*-macos.zip` archive and press
   Enter on it; confirm the plugin-install prompt.

Option B — manual:

1. Unpack the zip somewhere permanent.
2. Double Commander → Configuration → Options → Plugins → File System Plugins
   (WFX) → Add… → select `adbfsplugin.wfx`.

Then open the file-system-plugins root (the `wfx://` panel / network drive
button) and enter **Android**.

## Environment knobs

| Variable | Meaning |
|---|---|
| `ADBFS_ADB` | Full path to the `adb` binary (default: search `$PATH`, `/opt/homebrew/bin`, `/usr/local/bin`, `~/Library/Android/sdk/platform-tools`) |
| `ANDROID_ADB_SERVER_PORT` | ADB server port (default `5037`, same variable adb itself uses) |
| `ADBFS_NO_SU` | If set, skip running `su` after connecting (for unrooted devices) |

## Building from source

```sh
make            # build/adbfsplugin.wfx (native arch)
make test       # unit + integration + dlopen loadability tests
make universal  # arm64 + x86_64 dylib
make dist       # run tests and produce dist/adbfsplugin-<version>-macos.zip
```

No dependencies beyond Xcode command-line tools. The integration tests run
against an in-process fake ADB server; no device is needed. Testing against a
real device is a manual step: connect a device with USB debugging, run
`adb devices`, then browse the Android entry in Double Commander.

## Notes on the port

- The plugin ABI (UTF-16 `WCHAR`, packed find-data structs) comes from Double
  Commander's official SDK headers, vendored under `sdk/`.
- The original Windows sources (`StdAfx.*`, `cunicode.*`, `.vcxproj`, `.def`,
  `.rc`) remain in the tree for reference but are not used by the macOS build,
  which lives entirely in the `Makefile`.
- Known limitations (inherited from upstream): single device only
  (`host:transport-usb`), no `FsSetTime`/`FsSetAttr`, transfers are slower than
  `adb pull/push` because they run base64-encoded through the shell.

## License and credits

Original author: Zsolt Sz. Sztupák (<http://sztupy.hu>).
Licensed under the GNU AGPL-3.0 (with the linking exception for Total
Commander described in `LICENCE`); the macOS port keeps the same license.
