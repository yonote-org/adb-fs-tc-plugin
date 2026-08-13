# adbfsplugin for macOS (Double Commander)

Browse an Android device's filesystem from
[Double Commander](https://doublecmd.sourceforge.io/) on macOS — over ADB, with
USB debugging. This is a WFX (virtual file system) plugin: the device shows up
as an **Android** entry in Double Commander's drive list, and you can list,
copy, move, rename, delete and create directories on it like on any other
filesystem. Double-clicking a file opens it in its default macOS application
(Double Commander downloads a temporary local copy and cleans it up itself).

The plugin talks to the local ADB server, opens a shell on the device, and
drives it with `busybox` commands (falling back to stock Android's `toybox`
applets automatically). File transfers run in-band through the shell,
base64-encoded — slower than `adb pull/push`, but they work even on paths adb
itself cannot access directly (e.g. with root via `su`).

## Requirements

- Double Commander for macOS (Apple Silicon or Intel)
- `adb` — Android platform-tools, e.g. `brew install android-platform-tools`
- An Android device with **USB debugging** enabled. busybox is used when
  present; stock devices fall back to toybox (`ls`, `stat`, `mkdir`,
  `base64`, …). Root is optional — the plugin tries `su` once on connect and
  carries on without it (set `ADBFS_NO_SU=1` to skip the attempt entirely).

## Install

1. Download the latest release zip from the
   [releases page](https://github.com/yonote-org/adbfsplugin/releases/latest) —
   or build it yourself with `make dist` (see below).
2. Pick the zip matching your Mac: `…-arm64.zip` for Apple Silicon,
   `…-x86_64.zip` for Intel (`uname -m` tells you which). The plugin ships as
   a thin single-architecture binary on purpose — Double Commander's plugin
   check rejects universal (fat) binaries as "This is not a valid plugin!".
3. Unpack the zip somewhere permanent, e.g.
   `~/Library/Application Support/doublecmd/plugins/wfx/adbfsplugin/`
   (the directory Double Commander's own installer uses).
4. In Double Commander: **Configuration → Options… → Plugins →
   File System Plugins (WFX) → Add** — select `adbfsplugin.wfx` — **Apply**.
5. Open the drive list (Alt+F1 / Alt+F2) or the network/VFS button and enter
   the new **Android** entry.

> Note: unlike Total Commander, Double Commander (verified against the 1.2.8
> sources) does **not** offer to install a plugin when you open its zip in the
> file panel. Its archive-install code path is only reachable through the
> internal command `cm_AddPlugin <path-to-zip>`, which is not bound to any menu
> or hotkey by default. The zip keeps `pluginst.inf` for Total Commander
> compatibility and for `cm_AddPlugin` if you bind it yourself.

## Configuration

Everything is optional, via environment variables:

| Variable | Meaning |
|---|---|
| `ADBFS_ADB` | Full path to the `adb` binary (default: search `$PATH`, `/opt/homebrew/bin`, `/usr/local/bin`, `~/Library/Android/sdk/platform-tools`) |
| `ANDROID_ADB_SERVER_PORT` | ADB server port (default `5037`, the same variable adb itself uses) |
| `ADBFS_SERIAL` | Serial of the device to use (as shown by `adb devices`), for when several devices are attached. Default: the single connected device, USB or wireless |
| `ADBFS_NO_SU` | If set, never run `su` after connecting (for unrooted devices) |
| `ADBFS_READ_TIMEOUT` | Seconds of read inactivity before giving up on the device (default `30`, `0` disables). Raise it if silent long-running operations — a huge `rm -r` or `cp` — legitimately produce no output for longer |

## Building from source

```sh
make            # build/adbfsplugin.wfx (native arch)
make test       # unit + integration + dlopen loadability tests
make dist       # run tests and produce per-arch release zips in dist/
```

No dependencies beyond the Xcode command-line tools. The integration tests run
against an in-process fake ADB server — no device needed. See
[docs/MAINTAINING.md](docs/MAINTAINING.md) for the architecture, code
conventions, test layout and release process.

## Limitations

- One device at a time. USB and wireless (TCP) connections both work; with
  several devices attached at once, set `ADBFS_SERIAL` to pick one.
- No `FsSetTime` / `FsSetAttr` (timestamps and permissions cannot be edited).
- Transfers are slower than `adb pull/push` because they run base64-encoded
  through the device shell.

## Credits and license

This is a macOS (Apple Silicon and Intel) port of
**[adbfsplugin](https://github.com/sztupy/adbfsplugin)** by
**Zsolt Sz. Sztupák** (<http://sztupy.hu>), originally written for Total
Commander on Windows. All credit for the original design — the ADB
smart-socket client, the in-band base64 transfer trick, and the WFX plugin
implementation it is built on — goes to him. This repository preserves the
upstream history; commits up to `a8a9132` are the original work, everything
after is the port.

Licensed under the **GNU AGPL-3.0** with the linking exception for Total
Commander described in [`LICENCE`](LICENCE); the port keeps the original
license.
