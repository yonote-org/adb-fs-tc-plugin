# adbfsplugin macOS port — design

Date: 2026-08-10
Status: approved (autonomous session — decisions documented in lieu of interactive review)

## Context

`adbfsplugin` is a Total Commander WFX (virtual filesystem) plugin from 2010–2014 that
browses Android devices over ADB. It connects to the local ADB server
(TCP 127.0.0.1:5037, ADB "smart socket" protocol), opens a `shell:` transport, and
drives the device with `busybox` commands (`ls`, `stat`, `mkdir`, `mv`, `cp`, `rm`).
File transfer is done in-band through the shell using `busybox uuencode -m` /
`uudecode` (base64).

The codebase is Windows-only: MSVC project, WinSock2, `CreateProcess`, UTF-16
`wchar_t`, MSVC secure-CRT functions, Win32 resources, and a `.def` export table.

Goal: make the plugin build, test, and package on macOS so it can be installed in
**Double Commander** (which implements the Total Commander plugin APIs on all
platforms).

## Verified facts (not assumptions)

- Double Commander 1.2.8 is installed on this machine at
  `/Applications/Double Commander.app`, built **arm64**.
- DC's official plugin SDK headers (`sdk/common.h`, `sdk/wfxplugin.h`, fetched from
  github.com/doublecmd/doublecmd) define the Unix ABI: `WCHAR = uint16_t` (UTF-16,
  matching Free Pascal `WideChar`), `HANDLE = void*`, `BOOL = int`,
  `DWORD = uint32_t`, `WIN32_FIND_DATAW` **packed** (`#pragma pack(1)`), `DCPCALL`
  empty (plain C calling convention), `FILE_ATTRIBUTE_UNIX_MODE = 0x80000000`
  (the plugin already sets this flag with the unix mode in `dwReserved0`).
  `FsExtractCustomIcon(W)` takes `PWfxIcon` on DC, not `HICON*`.
- On macOS `wchar_t` is 4 bytes (UTF-32), so `std::wstring` ≠ the ABI's `WCHAR*`.
  Every W-API boundary needs UTF-16 ⇄ UTF-32 conversion.
- Toolchain on this machine: Apple clang 21, GNU make. No CMake.
- `adb` is installed at `/opt/homebrew/bin/adb`.
- WFX plugins on macOS are Mach-O dylibs named `*.wfx`, loaded with `dlopen`/`dlsym`;
  DC installs them from a zip containing `pluginst.inf` or via Options → Plugins.
- License: AGPL-3.0 with a Total Commander linking exception; port stays AGPL.

## Goals

1. Compile on macOS (arm64 native; release binary built universal arm64 + x86_64)
   as `adbfsplugin.wfx` with all the exports from `adbfsplugin.def` visible to `dlsym`.
2. A test suite that passes locally (`make test`): unit tests for all pure logic and
   an integration test that exercises the real exported plugin API against a fake
   ADB server over a real TCP socket.
3. A release artifact: `adbfsplugin-<version>-macos.zip` containing
   `adbfsplugin.wfx`, `pluginst.inf`, README, LICENCE — installable by opening the
   zip in Double Commander.

## Non-goals

- Verifying the MSVC/Windows build still compiles (no Windows machine here). The
  port keeps the Windows code paths behind `#ifdef _WIN32` where cheap, but Windows
  buildability is not a promise of this work.
- GUI-driving Double Commander end-to-end with a real Android device.
- Multi-device selection, `FsSetTime`, `FsExecuteFile` — remain unsupported, as
  upstream.

## Approaches considered

**A. Compat-shim port (chosen).** Keep the existing two-module architecture
(`adbfsplugin.cpp` API surface, `adbhandler.cpp` ADB protocol) and internal
`std::wstring` strings. Introduce a small platform layer: DC SDK headers define the
ABI types on Unix; a `wfxcompat` module provides UTF-16⇄UTF-32⇄UTF-8 conversion,
POSIX sockets, `posix_spawnp` for `adb start-server`, and replacements for the
handful of Win32/MSVC calls. Smallest diff, ABI fidelity comes from DC's own
headers, behavior preserved.

**B. UTF-8-first rewrite.** Rewrite internals around `std::string`/UTF-8 and only
convert at the ABI edge. Cleaner long-term but a much larger diff with real risk of
behavior drift, for no user-visible gain. Rejected.

**C. CMake dual-platform build with GoogleTest.** More infrastructure than the
project needs; CMake isn't even installed here. Rejected — plain Makefile + a tiny
header-only test harness.

## Architecture

### File layout (new/changed)

```
sdk/common.h, sdk/wfxplugin.h     vendored DC SDK headers (ABI source of truth)
platform.h                        includes windows.h on _WIN32, else sdk + POSIX shims
wfxcompat.h / wfxcompat.cpp       UTF conversions + tiny portability helpers
adbfsplugin.cpp / .h              ported API surface (uses DC signatures on Unix)
adbhandler.cpp / .h               ported ADB communicator + parsing refactor
tests/test_main.cpp               unit tests (tiny homegrown TEST macro harness)
tests/fake_adb_server.h/.cpp      fake ADB server + fake device shell (TCP, thread)
tests/test_integration.cpp        drives the real exported Fs* API against the fake
tests/test_dlopen.cpp             dlopens the built .wfx, dlsyms every export
Makefile                          build, test, dist targets
pluginst.inf                      unchanged (already correct for DC)
README.md                         macOS install/usage docs
```

`cunicode.cpp` (Win32 file-API wrappers + ANSI/Wide converters) is Windows-only;
on Unix its few used pieces (`wcslcpy`, `walcopy`/`awlcopy` equivalents,
find-data A⇄W copy) move into `wfxcompat` implemented over proper UTF-8⇄UTF-16⇄
UTF-32 conversion (correct surrogate-pair handling, unlike `CP_ACP`).

### The string model

- ABI boundary: `WCHAR* = uint16_t*` UTF-16 (DC), `char*` UTF-8 (DC uses UTF-8 for
  the ANSI API on Unix).
- Internal: `std::wstring` (UTF-32 on mac) — unchanged internal code.
- `wfxcompat` provides: `u16_to_ws`, `ws_to_u16buf(dest,maxlen)`, `ws_to_utf8`,
  `utf8_to_ws`, plus `WcsBuf` helpers for fixed arrays (`cFileName[260]`).
- Wide literals (`L"busybox ls..."`) keep working since internals stay `wstring`.

### Windows → POSIX replacements

| Windows | macOS |
|---|---|
| `WSAStartup`, `SOCKET`, `closesocket`, `WSAGetLastError` | BSD sockets, `int` fd, `close`, `errno`; `FsInit` returns 0 |
| `select(0, …)` + `TIMEVAL` | `select(fd+1, …)` + `struct timeval` |
| `MSG_WAITALL` | `MSG_WAITALL` (exists on macOS) |
| `CreateProcess(adb.exe start-server)` | `posix_spawnp` of discovered adb binary |
| `GetModuleFileName`-relative adb.exe | discovery: `$ADBFS_ADB` → `adb` on PATH → `/opt/homebrew/bin/adb`, `/usr/local/bin/adb`, `~/Library/Android/sdk/platform-tools/adb`; spawn failure logs a warning and continues (server may already run) |
| `Sleep(ms)` | `usleep(ms*1000)` |
| `GetFileAttributesW` existence check | `stat()` on UTF-8 path |
| `_wfopen_s` | `fopen` on UTF-8 path |
| `GetCompressedFileSizeW` | `stat().st_size` (upstream used it only for progress %) |
| `strcpy_s`/`strncpy_s`/`sscanf_s`/`swscanf_s`/`_strcmpi` | `snprintf`/`sscanf`/`strcasecmp`; stat-line parsing rewritten as a real parser (see below) |
| `SetFileAttributesT` in `FsSetAttr` | dropped — was wrong even on Windows (called a *local* file API on a remote path); on Unix `FsSetAttrW` returns false (unsupported), like `FsSetTime` |
| `HICON*` in `FsExtractCustomIcon` | DC's `PWfxIcon`; still returns `FS_ICON_USEDEFAULT` |

### Targeted refactors (for correctness + testability)

1. **`ParseStatLine`**: extract the `busybox stat -c "%a -%F- %g %u %s %X %Y %Z %N"`
   line parser out of `FillStat` into a pure function
   `bool ParseStatLine(const std::wstring& line, FileData& out)` (the
   `swscanf_s` + `%I64i` MSVC format doesn't exist on mac anyway). Unit-testable
   against real busybox output shapes (names with spaces, `'link' -> 'target'`).
2. **ADB server address**: honor `ANDROID_ADB_SERVER_PORT` (the same env var adb
   itself uses), default 5037. Lets tests point the plugin at a fake server, and
   helps users with non-standard setups.
3. **Wire up modification time**: upstream collects `%Y` mtime and defines
   `unixTimeToFileTime` but never fills `ftLastWriteTime`; the port fills it so DC
   shows dates. Low risk, testable.
4. **`FsFindClose` handle check**: `if ((int)Hdl==1)` pointer-truncation fixed to a
   proper sentinel comparison.

Everything else keeps upstream behavior — including the `su` step on connect and the
in-band uuencode transfer scheme.

### Exports

`adbfsplugin.def` is Windows-only. On macOS: compile with `-fvisibility=hidden` and
mark every exported `Fs*` function with `__attribute__((visibility("default")))`
(via a `DCEXPORT` macro in `platform.h`). `extern "C"` on all exports (dlsym
lookup by unmangled name).

## Build

`Makefile` targets:

- `make` → `build/adbfsplugin.wfx` (native arch, `-std=c++17 -O2 -Wall`)
- `make universal` → arm64 + x86_64 dylib via `-arch arm64 -arch x86_64`
- `make test` → builds and runs `build/tests` (unit + integration + dlopen)
- `make dist` → `dist/adbfsplugin-<version>-macos.zip`
- Version single-sourced in the Makefile (`VERSION := 1.1.0`).

## Testing

Tiny header-only harness (`tests/harness.h`: `TEST(name)`, `CHECK`, `CHECK_EQ`,
non-zero exit on failure) — no external dependencies.

**Unit tests** (pure logic, no sockets):
- base64: `encode64`/`decode64` round-trips, padding (`=`, `==`), the `====`
  terminator returning 0.
- `QuoteString`: plain, embedded `'`, empty.
- `trim`, `PathConverter` (backslash→slash).
- `ParseStatLine`: regular file / directory / symlink lines, names with spaces,
  malformed lines rejected.
- UTF conversions: ASCII, BMP (e.g. Cyrillic), non-BMP (emoji → surrogate pairs),
  fixed-buffer truncation safety, round-trips UTF-8⇄UTF-16⇄UTF-32.
- `unixTimeToFileTime`/`fileTimeToUnixTime` round-trip + known epoch values.
- `GetStat`: `FILE_ATTRIBUTE_UNIX_MODE` always set, directory flag, 64-bit size
  split into high/low, mtime lands in `ftLastWriteTime`.

**Integration test** (`test_integration`): starts a fake ADB server on an ephemeral
localhost port in a thread — accepts the smart-socket handshake
(`0012host:transport-usb` → `OKAY`, `0006shell:` → `OKAY`), then emulates a device
shell: emits a prompt (so the plugin's `su` dance completes), recognizes the
`echo "===adbfsplugin<--"` framing, and serves canned `busybox ls`/`stat` output.
With `ANDROID_ADB_SERVER_PORT` pointed at it and `ADBFS_ADB=/usr/bin/true`, the test
calls the real `FsInitW`/`FsFindFirstW`/`FsFindNextW`/`FsFindClose`/content-plugin
functions (linked from the same objects as the .wfx) and asserts on the returned
UTF-16 find data. This covers the protocol framing, buffering, UTF-16 shim, and
directory-listing pipeline end to end.

**Loadability test** (`test_dlopen`): `dlopen("build/adbfsplugin.wfx")` and `dlsym`
each name from the `.def` list — proves DC's loading mechanism will find every
entry point, and that the dylib has no unresolved dependencies.

## Release

`make dist` produces `dist/adbfsplugin-1.1.0-macos.zip`:
`adbfsplugin.wfx` (universal), `pluginst.inf`, `README.md` (install: open zip in DC
→ auto-install prompt; or Configuration → Options → Plugins → WFX → Add; adb must be
installed, USB debugging + busybox on device), `LICENCE`.
Local artifact only — publishing (GitHub fork/release) needs the user's say-so.

## Risks

- **ABI mismatch** would crash DC → mitigated by vendoring DC's own SDK headers
  (packed structs) and the dlopen test; final proof is a manual DC launch.
- **Fake vs real device drift**: the fake shell is canned from real busybox output
  formats; a real-device smoke test needs hardware the session may not have —
  documented as a manual step in the README.
- **`su` requirement**: upstream always runs `su` after connect; devices without
  root print an error which the plugin's buffer-clean tolerates (same as upstream).
