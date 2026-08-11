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
  plugin sends `host:transport-any` (the single connected device, USB or
  wireless TCP; `ADBFS_SERIAL` switches this to `host:transport:<serial>`)
  then `shell:`, leaving a raw pty shell on the socket.
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
| `wireless_device_test` | `test_wireless_device.cpp` | Wireless (TCP) devices: the server FAILs `host:transport-usb`, so the plugin must select via `host:transport-any`, and `ADBFS_SERIAL` must pin a specific serial. |
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
