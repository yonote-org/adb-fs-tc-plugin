# Repo Cleanup, Docs Rewrite & Release Rebuild Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove every Windows-only leftover and junk file from the repo, add a maintainer guide, rewrite the README around what the plugin is today (with credit to the original author), and rebuild/re-test the release artifacts.

**Architecture:** No behavior changes. The macOS build (`Makefile` + `adbfsplugin.cpp`, `adbhandler.cpp`, `wfxcompat.cpp`, `platform.h`, `sdk/`) is already fully POSIX; the cleanup deletes dead files that nothing references, and the docs work is pure writing. `make test` (25 unit checks + 3 scenario binaries + dlopen export check) is the regression gate after every task.

**Tech Stack:** clang++ (C++17), GNU make, git. No dependencies beyond Xcode command-line tools.

## Global Constraints

- License stays **AGPL-3.0 with the Total Commander linking exception** (`LICENCE` untouched); original author **Zsolt Sz. Sztupák** must be credited in the README.
- Upstream/port git-history boundary is commit `a8a9132b9b235f986016fe3781a1f502e48e17f5` (last upstream commit); do not rewrite history.
- Keep: `pluginst.inf` (shipped in the zip for Total Commander / `cm_AddPlugin` compatibility), `sdk/` (vendored Double Commander SDK headers), `docs/superpowers/` (project history), `LICENCE`, all of `tests/`.
- Version stays `1.1.0` (Makefile `VERSION`); cleanup does not change the binary.
- `make test` must pass after every task; a task is not done until it does.
- Commit after each task (no pushing).

---

### Task 1: Delete Windows-only and junk files, modernize .gitignore

**Files:**
- Delete: `adbfsplugin.def`, `adbfsplugin.rc`, `adbfsplugin.sln`, `adbfsplugin.vcxproj` (MSVC project + Win32 resources + export table)
- Delete: `StdAfx.cpp`, `StdAfx.h` (MSVC precompiled headers), `cunicode.cpp`, `cunicode.h` (superseded by `wfxcompat.*`; only referenced from `StdAfx.h`), `resource.h` (Win32 resource IDs)
- Delete: `bitmap1.bmp`, `COMPUTER.ICO`, `icon2.ico` (Win32 resources), `FSPLUGIN.GID`, `FSPLUGIN.HLP` (Windows help files)
- Delete: `ReadMe.txt` (obsolete Windows/Total Commander readme; author credit moves to README.md in Task 2)
- Delete: `double-commander report.txt` (90k-line macOS stackshot captured while debugging the port; a one-off debug artifact)
- Delete from git + disk: `.DS_Store` (Finder junk, currently tracked)
- Modify: `.gitignore` (drop the nine MSVC patterns, add `.DS_Store`)

**Interfaces:**
- Consumes: nothing.
- Produces: a tree where `git ls-files` contains only macOS-relevant files; later tasks rely on `ReadMe.txt` being gone (README.md becomes the only readme) and on `.gitignore` covering `.DS_Store`.

- [ ] **Step 1: Verify nothing references the doomed files**

Run: `grep -rn 'cunicode\|StdAfx\|resource\.h\|\.def\|\.rc\b\|FSPLUGIN\|COMPUTER\.ICO\|icon2\|bitmap1\|ReadMe\.txt' Makefile *.cpp *.h sdk tests`
Expected: no hits outside the files being deleted themselves (pre-verified: only `StdAfx.h:27` includes `cunicode.h`).

- [ ] **Step 2: git rm the files**

```bash
git rm -q adbfsplugin.def adbfsplugin.rc adbfsplugin.sln adbfsplugin.vcxproj \
  StdAfx.cpp StdAfx.h cunicode.cpp cunicode.h resource.h \
  bitmap1.bmp COMPUTER.ICO icon2.ico FSPLUGIN.GID FSPLUGIN.HLP \
  ReadMe.txt .DS_Store "double-commander report.txt"
```

- [ ] **Step 3: Rewrite .gitignore**

Replace the whole file with:

```gitignore
build/
dist/
.idea/
.DS_Store
```

- [ ] **Step 4: Verify the build is unaffected**

Run: `make clean && make test`
Expected: `OK: 25 test(s) passed`, then OK from integration/su-hang/stock-device binaries, then `43 exports resolved`.

- [ ] **Step 5: Commit**

```bash
git add .gitignore
git commit -m "Remove Windows-only sources, resources and junk files"
```

---

### Task 2: Rewrite README.md

**Files:**
- Modify: `README.md` (full rewrite)

**Interfaces:**
- Consumes: Task 1 (no more `ReadMe.txt`, no Windows sources "kept for reference" — the old README's claim to that effect must not survive).
- Produces: `README.md` that Task 3 links to from the guide and that Task 4 packages into the release zips (`make dist` copies `README.md`).

- [ ] **Step 1: Replace README.md with the following content**

````markdown
# adbfsplugin for macOS (Double Commander)

Browse an Android device's filesystem from
[Double Commander](https://doublecmd.sourceforge.io/) on macOS — over ADB, with
USB debugging. This is a WFX (virtual file system) plugin: the device shows up
as an **Android** entry in Double Commander's drive list, and you can list,
copy, move, rename, delete and create directories on it like on any other
filesystem.

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

1. Pick the zip matching your Mac: `…-arm64.zip` for Apple Silicon,
   `…-x86_64.zip` for Intel (`uname -m` tells you which). The plugin ships as
   a thin single-architecture binary on purpose — Double Commander's plugin
   check rejects universal (fat) binaries as "This is not a valid plugin!".
2. Unpack the zip somewhere permanent, e.g.
   `~/Library/Application Support/doublecmd/plugins/wfx/adbfsplugin/`
   (the directory Double Commander's own installer uses).
3. In Double Commander: **Configuration → Options… → Plugins →
   File System Plugins (WFX) → Add** — select `adbfsplugin.wfx` — **Apply**.
4. Open the drive list (Alt+F1 / Alt+F2) or the network/VFS button and enter
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
| `ADBFS_NO_SU` | If set, never run `su` after connecting (for unrooted devices) |

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

- Single device only (`host:transport-usb`) — with several devices attached,
  adb picks the USB one.
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
````

- [ ] **Step 2: Verify the README's claims against the tree**

Run: `ls StdAfx.h cunicode.h ReadMe.txt 2>&1; grep -n 'MAINTAINING' README.md`
Expected: the three deleted files report "No such file or directory"; the MAINTAINING link is present (target file arrives in Task 3 — acceptable ordering, verified again in Task 4).

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "Rewrite README for the macOS plugin, credit original author"
```

---

### Task 3: Write the maintainer guide

**Files:**
- Create: `docs/MAINTAINING.md`

**Interfaces:**
- Consumes: Task 2's README (linked from the guide's intro).
- Produces: the file `docs/MAINTAINING.md` that README.md links to.

- [ ] **Step 1: Create docs/MAINTAINING.md with the following content**

````markdown
# Maintainer guide

Everything you need to work on adbfsplugin that is not in the
[README](../README.md). The README covers what the plugin does, installing and
basic building; this file covers how the code is put together and how to change
it safely.

## Repository layout

| Path | Responsibility |
|---|---|
| `adbfsplugin.cpp` | All WFX entry points (`FsInit`, `FsFindFirst`, `FsGetFile`, …) plus the content-plugin (custom columns) API. No device logic — it delegates to `adbhandler`. |
| `adbfsplugin.h` | Externs for the plugin-global callback pointers (`ProgressProcW`, `LogProcW`, …) registered in `FsInit/FsInitW`. |
| `adbhandler.cpp/.h` | Everything device-side: `AdbCommunicator` (socket to the ADB server, shell session), `DirList`/`FillStat`/`GetStat` (listing + stat), `ParseStatLine`, base64 codec, helpers (`QuoteString`, `StripAnsiEscapes`, `FindAdbBinary`, `Tool`). |
| `wfxcompat.cpp/.h` | String/ABI compatibility: UTF-16 (`WCHAR`, the plugin ABI) ↔ `std::wstring` (UTF-32 on macOS) ↔ UTF-8, truncation-safe fixed-buffer variants, and `ProgressT`/`LogT`/`LogA` wrappers that tolerate either callback set being NULL. |
| `platform.h` | POSIX includes, small Win32 shims (`Sleep`, `SOCKET`, `ZeroMemory`), and the single sanctioned include of the SDK headers. Include this, never `sdk/wfxplugin.h` directly (it has no include guard). |
| `sdk/` | Vendored Double Commander plugin SDK headers (from github.com/doublecmd/doublecmd). Defines the Unix WFX ABI. Do not edit; re-vendor to update. |
| `pluginst.inf` | Plugin auto-install descriptor, shipped in the release zip for Total Commander / `cm_AddPlugin` compatibility. |
| `tests/` | Test harness, fake ADB server, unit + scenario tests (see below). |
| `Makefile` | The entire build: plugin, tests, per-arch release zips. `VERSION` lives here. |
| `docs/superpowers/` | Historical design doc and implementation plans from the port. Background reading, not living documentation. |

## Architecture

### The ABI: why the code looks Windows-flavored

The WFX plugin API was designed for Total Commander on Windows; Double
Commander reimplements it byte-compatibly on Unix. The vendored SDK headers
define: `WCHAR` = `uint16_t` (UTF-16, matching Free Pascal `WideChar`),
`HANDLE` = `void*`, `BOOL` = `int`, `WIN32_FIND_DATAW` **packed**
(`#pragma pack(1)`), and `FILE_ATTRIBUTE_UNIX_MODE` for Unix-mode reporting.
So `WIN32_FIND_DATAW`, `FILETIME` and friends are not leftovers — they are the
actual ABI. Internally the code works in `std::wstring` and converts at the
boundary via `wfxcompat`.

Two hard packaging rules that follow from Double Commander's loader:

- **Thin binaries only.** DC's `GetPluginBinaryType` rejects universal (fat)
  Mach-O files ("This is not a valid plugin!"). `make dist` builds one thin
  `.wfx` per architecture and zips them separately.
- **Both ANSI and Unicode entry points are exported.** DC uses the `W`
  (UTF-16) set; the ANSI set (UTF-8 on Unix) is kept for API completeness and
  is a thin conversion wrapper around the `W` implementation in every case.

### How a directory listing works

1. DC calls `FsFindFirstW("/some/path", …)`.
2. `DirList` sends `ls -1 '<path>' | cat` through `AdbCommunicator`. The
   `| cat` makes ls's stdout a pipe instead of the pty, which suppresses
   colors and backslash-escaping at the source.
3. `FillStat` batches the resulting names (10 per command) into
   `stat -c "%a -%F- %g %u %s %X %Y %Z %N"` calls and `ParseStatLine` parses
   each output line into a `FileData`. A second `stat -L` pass resolves
   symlink targets so directory links (e.g. `/sdcard`) are enterable.
4. `GetStat` converts `FileData` into the packed `WIN32_FIND_DATAW`:
   Unix mode goes into `dwReserved0` with `FILE_ATTRIBUTE_UNIX_MODE` set,
   mtime becomes a Windows `FILETIME`, directories report size 0.
5. Results are also cached in `cacheMap` (keyed by full path) to serve the
   content-plugin custom columns (`mode`, `uid`, `gid`, `type`, `name`)
   without extra device round-trips.

### The device connection

`AdbCommunicator` is a lazily-created singleton (`instance()`), torn down by
`disconnect()` (called from `FsDisconnect` and `FsContentPluginUnloading`).

- **Connect:** run `adb start-server` (via `posix_spawnp`; failure is logged,
  not fatal — the server may already run), then TCP to `127.0.0.1:5037`
  (`ANDROID_ADB_SERVER_PORT` overrides), then the ADB *smart-socket* protocol:
  `<4-hex-digit length><payload>` requests, `OKAY`/`FAIL` responses. The
  plugin sends `host:transport-usb` (single USB device) then `shell:`,
  leaving a raw pty shell on the socket.
- **su:** unless `ADBFS_NO_SU` is set, `su\n` is sent once after connect.
  The result is drained with a **bounded** 2-second `select()`
  (`CleanBuffer(true)`) — an unbounded wait deadlocked the panel on devices
  that answered fast (see `test_su_hang.cpp`).
- **Command framing:** every command is wrapped as
  `echo "===adbfsplugin<--" ; <cmd> ; echo "===adbfsplugin-->"`. `ReadLine`
  skips everything before the start marker (the pty echoes the command back)
  and returns NULL at the end marker. Lines are stripped of ANSI/OSC escape
  sequences (`StripAnsiEscapes`) and trimmed — the pty may color or retitle
  mid-output.
- **Tool selection:** `ToolMode()` probes once per connection —
  `busybox echo adbfsprobe`, then `toybox echo adbfsprobe`, else plain
  applets — and `Tool(L"ls")` prefixes commands accordingly. Mode 0 =
  busybox, 1 = toybox, 2 = bare shell applets.
- **Transfers:** downloads run `uuencode -m` (busybox) or `base64` (toybox)
  on the device and decode locally; uploads encode locally and pipe into
  `uudecode` / `base64 -d` on the device, terminated by `====` framing or a
  `^D` at line start. Errors are thrown as `std::wstring` markers like
  `<000B - FAIL response from adb server>` — caught at the entry-point layer
  and either returned as `FS_FILE_*` codes or, in listings, surfaced as a
  pseudo-file with the marker as its name (upstream's convention).

## Code conventions

- C++17, `clang++`, warnings on (`-Wall -Wextra`), no external dependencies,
  no exceptions to that without a very good reason.
- Internal strings are `std::wstring`; convert at the ABI/device boundaries
  only, using `wfxcompat` helpers. Fixed-size ABI buffers must be filled with
  the truncation-safe `*buf` variants (they never split a surrogate pair or a
  UTF-8 sequence).
- Symbols are hidden by default (`-fvisibility=hidden`); every plugin export
  is marked `DCEXPORT`. If you add an entry point, also add it to `kExports`
  in `tests/test_dlopen.cpp` — the dlopen test resolves that exact list
  against the built binary.
- Error propagation inside device code is `throw std::wstring(L"<hex - text>")`;
  entry points translate to WFX return codes. Match this rather than
  introducing a second mechanism.
- The container-of-pointers style (`std::list<FileData*>*`) is inherited from
  upstream. Keep new code memory-safe, but don't restructure working upstream
  code for style alone — diffability against the original matters for a port.
- Comments state constraints the code can't (ABI quirks, device oddities,
  protocol rules), not narration.

## Tests

`make test` builds and runs everything; no device, network, or adb binary is
needed (`FindAdbBinary` failures are non-fatal by design).

| Binary | Sources | What it covers |
|---|---|---|
| `unit_tests` | `test_main.cpp`, `test_globals.cpp` | Pure-function coverage: UTF conversions and truncation edges, base64 codec, `QuoteString`, `ParseStatLine` (incl. malformed input), `GetStat` field mapping, `StripAnsiEscapes`, `FindAdbBinary` env override, callback-wrapper fallbacks. |
| `integration_tests` | `test_integration.cpp` | Full plugin lifecycle against `FakeAdbServer`: connect, list, stat, download, upload, verifying both the results and the exact shell commands sent. |
| `su_hang_test` | `test_su_hang.cpp` | Regression: the su handshake must not deadlock when the device answers within the drain window. |
| `stock_device_test` | `test_stock_device.cpp` | The no-busybox path: toybox fallback for listing, stat and transfers. |
| `dlopen_test` | `test_dlopen.cpp` | Loads the built `.wfx` with `dlopen` and resolves every export in its `kExports` list — catches missing symbols and ABI breaks. `--magic-only` variant checks the Mach-O header of a foreign-arch build. |

Mechanics:

- The harness (`tests/harness.h`) is ~25 lines: `TEST(name) { CHECK(...); }`
  auto-registers; `run_all()` in each `main`. No framework dependency.
- `FakeAdbServer` (`tests/fake_adb_server.*`) is an in-process TCP server
  speaking just enough smart-socket + shell to drive the plugin; `stock=true`
  emulates a busybox-less device. Tests point the plugin at it via
  `ANDROID_ADB_SERVER_PORT` and inspect `commands()` / `uploaded()`.
- Scenario tests are **separate binaries** on purpose: the plugin is a
  process-global singleton (`AdbCommunicator`, callback pointers, `cacheMap`),
  so each connection-lifecycle scenario gets a fresh process.
- Adding a unit test: add a `TEST` to `test_globals.cpp` (or a new file listed
  in `UNIT_SRCS`). Adding a scenario: new `tests/test_<name>.cpp` with its own
  `main`, plus a Makefile target mirroring `su_hang_test`, added to `test:`.

## Building and releasing

```sh
make            # native-arch plugin  -> build/adbfsplugin.wfx
make test       # everything above
make dist       # tests + thin arm64 & x86_64 zips -> dist/
make clean
```

`make dist` cross-compiles both architectures (`-arch arm64` / `-arch
x86_64`), dlopen-checks the native one, header-checks the foreign one, and
stages `adbfsplugin.wfx` + `pluginst.inf` + `README.md` + `LICENCE` into
`dist/adbfsplugin-<VERSION>-macos-<arch>.zip`.

Releasing a new version:

1. Bump `VERSION` in the `Makefile`.
2. `make dist` (green tests are a prerequisite of the target).
3. Sanity-check on a real device if the device-facing code changed: connect a
   device with USB debugging, `adb devices`, then browse the **Android** entry
   in Double Commander. The fake server is faithful but it is not a phone.

## Licensing and provenance

AGPL-3.0 with a linking exception for Total Commander — see
[`LICENCE`](../LICENCE). The project is a fork of
[sztupy/adbfsplugin](https://github.com/sztupy/adbfsplugin) by Zsolt Sz.
Sztupák; upstream history ends at commit `a8a9132`, everything later is the
macOS port. Keep the license and the README credit intact in any derivative;
new code enters under the same license.
````

- [ ] **Step 2: Verify the guide's cross-references**

Run: `grep -n 'MAINTAINING' README.md && ls docs/MAINTAINING.md && grep -c 'su_hang\|stock_device\|dlopen' docs/MAINTAINING.md`
Expected: README link present, file exists, test-name references found.

- [ ] **Step 3: Commit**

```bash
git add docs/MAINTAINING.md
git commit -m "Add maintainer guide (architecture, conventions, tests, release)"
```

---

### Task 4: Rebuild the plugin and release zips, final verification

**Files:**
- Modify: none (build outputs `build/`, `dist/` — both gitignored)

**Interfaces:**
- Consumes: Tasks 1–3 (clean tree, new README packaged into zips).
- Produces: `build/adbfsplugin.wfx`, `dist/adbfsplugin-1.1.0-macos-arm64.zip`, `dist/adbfsplugin-1.1.0-macos-x86_64.zip`.

- [ ] **Step 1: Full clean rebuild with tests and dist**

Run: `make clean && make dist`
Expected: all test binaries pass, `43 exports resolved`, and the final line `Release: dist/adbfsplugin-1.1.0-macos-arm64.zip and dist/adbfsplugin-1.1.0-macos-x86_64.zip`.

- [ ] **Step 2: Verify zip contents and binary architectures**

Run: `unzip -l dist/adbfsplugin-1.1.0-macos-arm64.zip && lipo -info build/thin-arm64/adbfsplugin.wfx build/thin-x86_64/adbfsplugin.wfx`
Expected: each zip holds exactly `adbfsplugin.wfx`, `pluginst.inf`, `README.md`, `LICENCE`; lipo reports `Non-fat file … arm64` and `… x86_64`.

- [ ] **Step 3: Verify the packaged README is the new one**

Run: `unzip -p dist/adbfsplugin-1.1.0-macos-arm64.zip README.md | grep -c 'Sztupák'`
Expected: at least 1 (the credit made it into the shipped README).

- [ ] **Step 4: Final tree check**

Run: `git status --short && git ls-files | grep -i 'stdafx\|cunicode\|\.ico\|\.bmp\|\.sln\|vcxproj\|\.def\|\.rc\|FSPLUGIN\|DS_Store\|ReadMe.txt\|report'`
Expected: clean status (build/, dist/ ignored), and the grep finds nothing.

- [ ] **Step 5: Commit the plan document**

```bash
git add docs/superpowers/plans/2026-08-11-cleanup-docs-release.md
git commit -m "Add cleanup/docs/release plan"
```
