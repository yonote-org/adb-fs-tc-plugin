# adbfsplugin macOS Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `adbfsplugin.wfx` as a macOS dylib installable in Double Commander, with a passing test suite and a release zip.

**Architecture:** Keep the upstream two-module design (`adbfsplugin.cpp` = WFX API surface, `adbhandler.cpp` = ADB protocol). Add `platform.h` (DC SDK types on Unix) and `wfxcompat.cpp` (UTF-16⇄UTF-32⇄UTF-8 conversions, null-safe callback wrappers). Internals stay `std::wstring`; the ABI boundary is UTF-16 `WCHAR = uint16_t` per Double Commander's vendored SDK headers in `sdk/`.

**Tech Stack:** C++17, Apple clang, GNU make, BSD sockets, `posix_spawnp`. Tests: homegrown header-only harness, a fake ADB server on localhost TCP, and a `dlopen` loadability check. No external dependencies.

## Global Constraints

- ABI types come ONLY from `sdk/common.h`/`sdk/wfxplugin.h` (packed structs, `WCHAR = uint16_t`). Never include `sdk/wfxplugin.h` directly — always via `platform.h` (the SDK header has no include guard).
- All exported functions: `DCEXPORT` (default visibility) with C linkage; build with `-fvisibility=hidden -fvisibility-inlines-hidden`.
- Replace `__int64`/`unsigned __int64` with `int64_t`/`uint64_t` everywhere.
- Env vars honored by the plugin: `ADBFS_ADB` (adb binary override), `ANDROID_ADB_SERVER_PORT` (default 5037), `ADBFS_NO_SU` (skip the `su` step).
- The old Windows sources (`StdAfx.*`, `cunicode.*`, `.vcxproj`, `.def`, `.rc`) stay in the tree untouched; the Makefile ignores them.
- Version: `1.1.0`, single-sourced in the Makefile.
- Commit after every task (all tests passing) on branch `macos-port`.

---

### Task 1: Test harness + minimal Makefile

**Files:**
- Create: `tests/harness.h`, `tests/test_main.cpp`, `Makefile`, `.gitignore` (append `build/`, `dist/`)

**Interfaces:**
- Produces: `TEST(name)` macro, `CHECK(cond)`, `CHECK_EQ(a,b)`, `run_all()`; `make test` convention (non-zero exit on failure).

- [ ] **Step 1: Write `tests/harness.h`**

```cpp
#pragma once
#include <cstdio>
#include <vector>

struct TestCase { const char* name; void (*fn)(); };
inline std::vector<TestCase>& test_registry() { static std::vector<TestCase> r; return r; }
inline int& test_failures() { static int f = 0; return f; }
struct TestRegistrar { TestRegistrar(const char* n, void (*f)()) { test_registry().push_back({n, f}); } };

#define TEST(name) \
    static void test_##name(); \
    static TestRegistrar reg_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); test_failures()++; } } while (0)
#define CHECK_EQ(a, b) CHECK((a) == (b))

inline int run_all() {
    for (auto& t : test_registry()) { std::printf("RUN  %s\n", t.name); t.fn(); }
    if (test_failures()) { std::printf("%d check(s) FAILED\n", test_failures()); return 1; }
    std::printf("OK: %zu test(s) passed\n", test_registry().size());
    return 0;
}
```

- [ ] **Step 2: Write `tests/test_main.cpp` with one sanity test**

```cpp
#include "harness.h"

TEST(harness_sanity) { CHECK_EQ(1 + 1, 2); }

int main() { return run_all(); }
```

- [ ] **Step 3: Write minimal `Makefile`**

```make
VERSION := 1.1.0
CXX := clang++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden -fvisibility-inlines-hidden -I.
BUILD := build

UNIT_SRCS := tests/test_main.cpp

.PHONY: all test clean

all: test

$(BUILD):
	mkdir -p $@

$(BUILD)/unit_tests: $(UNIT_SRCS) tests/harness.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $(UNIT_SRCS)

test: $(BUILD)/unit_tests
	$(BUILD)/unit_tests

clean:
	rm -rf $(BUILD) dist
```

- [ ] **Step 4: Run `make test` — expect `OK: 1 test(s) passed`, exit 0**
- [ ] **Step 5: Append `build/` and `dist/` to `.gitignore`; commit** (`git add -A && git commit -m "Add test harness and Makefile skeleton"`)

---

### Task 2: platform layer + wfxcompat UTF conversions (TDD)

**Files:**
- Create: `platform.h`, `wfxcompat.h`, `wfxcompat.cpp`, `tests/test_globals.cpp`
- Replace: `adbfsplugin.h` (becomes a thin externs header; SDK supplies the API decls)
- Modify: `Makefile` (add sources), `tests/test_main.cpp` (add tests)

**Interfaces:**
- Produces:
  - `platform.h`: DC SDK include (extern "C"), `DCEXPORT`, `SOCKET`/`INVALID_SOCKET`/`SOCKET_ERROR`/`closesocket`, `INVALID_HANDLE_VALUE`, `TIMEVAL`, `Sleep(ms)`, `ZeroMemory`, `countof`, `wdirtypemax` (=1024).
  - `wfxcompat.h`:
    ```cpp
    std::wstring u16_to_ws(const WCHAR* s);
    std::string  ws_to_utf8(const std::wstring& s);
    std::wstring utf8_to_ws(const std::string& s);
    std::string  u16_to_utf8(const WCHAR* s);
    size_t u16len(const WCHAR* s);
    void ws_to_u16buf(WCHAR* dst, size_t dstlen, const std::wstring& src);
    void utf8_to_u16buf(WCHAR* dst, size_t dstlen, const char* src);
    void u16_to_utf8buf(char* dst, size_t dstlen, const WCHAR* src);
    void ws_to_utf8buf(char* dst, size_t dstlen, const std::wstring& src);
    void copyfinddatawa(WIN32_FIND_DATAA* a, WIN32_FIND_DATAW* w);
    int  ProgressT(const std::wstring& source, const std::wstring& target, int percent);
    void LogT(int msgtype, const std::wstring& msg);
    void LogA(int msgtype, const char* msg);
    WCHAR* awfilenamecopy_impl(WCHAR* dst, size_t n, const char* src);
    #define awfilenamecopy(out, in) awfilenamecopy_impl(out, countof(out), in)
    ```
  - `adbfsplugin.h`: `extern int PluginNumber;` + the six callback globals.
  - `tests/test_globals.cpp`: defines those globals for unit-test binaries (integration links the real `adbfsplugin.cpp` instead).

- [ ] **Step 1: Write `platform.h`**

```cpp
#pragma once
// Platform layer: on Unix the plugin ABI comes from Double Commander's SDK
// headers (sdk/). Include this file, never sdk/wfxplugin.h directly (no guard).

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/in.h>

extern "C" {
#include "sdk/wfxplugin.h"
}

#define DCEXPORT __attribute__((visibility("default")))

typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket ::close
typedef struct timeval TIMEVAL;
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)(-1))

inline void Sleep(unsigned int ms) { usleep(ms * 1000); }
#define ZeroMemory(p, s) memset((p), 0, (s))

#ifndef countof
#define countof(a) (sizeof(a) / sizeof((a)[0]))
#endif
#define wdirtypemax 1024

#include <string>
#include <list>
#include <map>
#include <vector>
```

- [ ] **Step 2: Replace `adbfsplugin.h`**

```cpp
#pragma once
#include "platform.h"

extern int PluginNumber;
extern tProgressProc ProgressProc;
extern tLogProc LogProc;
extern tRequestProc RequestProc;
extern tProgressProcW ProgressProcW;
extern tLogProcW LogProcW;
extern tRequestProcW RequestProcW;
```

- [ ] **Step 3: Write `wfxcompat.h`** (signatures as in Interfaces above, with `#pragma once`, includes `platform.h` + `<string>`)

- [ ] **Step 4: Write failing tests in `tests/test_main.cpp`**

```cpp
#include "harness.h"
#include "../wfxcompat.h"

static std::vector<WCHAR> U16(std::initializer_list<int> units) {
    std::vector<WCHAR> v; for (int u : units) v.push_back((WCHAR)u); v.push_back(0); return v;
}

TEST(u16_to_ws_ascii) {
    auto s = U16({'a', 'b', 'c'});
    CHECK(u16_to_ws(s.data()) == L"abc");
}
TEST(u16_to_ws_surrogate_pair) {
    auto s = U16({0xD83D, 0xDE00});             // 😀 U+1F600
    std::wstring w = u16_to_ws(s.data());
    CHECK_EQ(w.size(), (size_t)1);
    CHECK_EQ((uint32_t)w[0], (uint32_t)0x1F600);
}
TEST(ws_to_u16buf_roundtrip_and_surrogates) {
    std::wstring w = L"ab";
    w.push_back((wchar_t)0x1F600);
    WCHAR buf[16];
    ws_to_u16buf(buf, countof(buf), w);
    CHECK_EQ(u16len(buf), (size_t)4);           // a b + surrogate pair
    CHECK_EQ((int)buf[2], 0xD83D);
    CHECK_EQ((int)buf[3], 0xDE00);
    CHECK(u16_to_ws(buf) == w);
}
TEST(ws_to_u16buf_never_splits_surrogate) {
    std::wstring w = L"ab";
    w.push_back((wchar_t)0x1F600);
    WCHAR buf[4];                                // room for a, b, NUL + 1 — pair needs 2
    ws_to_u16buf(buf, countof(buf), w);
    CHECK_EQ(u16len(buf), (size_t)2);            // truncated before the pair
}
TEST(utf8_ws_roundtrip) {
    std::string u8 = "h\xC3\xA9llo \xF0\x9F\x98\x80";   // "héllo 😀"
    std::wstring w = utf8_to_ws(u8);
    CHECK_EQ(w.size(), (size_t)7);
    CHECK(ws_to_utf8(w) == u8);
}
TEST(u16_utf8_full_chain) {
    auto s = U16({'x', 0xD83D, 0xDE00, 'y'});
    CHECK(u16_to_utf8(s.data()) == "x\xF0\x9F\x98\x80y");
}
TEST(utf8_to_u16buf_truncates_safely) {
    WCHAR buf[3];
    utf8_to_u16buf(buf, countof(buf), "abcdef");
    CHECK_EQ(u16len(buf), (size_t)2);
    CHECK_EQ((int)buf[0], 'a');
    CHECK_EQ((int)buf[2], 0);
}
TEST(copyfinddata_wa) {
    WIN32_FIND_DATAW w; memset(&w, 0, sizeof(w));
    w.dwFileAttributes = 0x80000010u;
    w.dwReserved0 = 0755;
    w.nFileSizeHigh = 1; w.nFileSizeLow = 5;
    WCHAR nm[] = {'f', 0xD83D, 0xDE00, 0};
    memcpy(w.cFileName, nm, sizeof(nm));
    WIN32_FIND_DATAA a; memset(&a, 0, sizeof(a));
    copyfinddatawa(&a, &w);
    CHECK_EQ(a.dwFileAttributes, 0x80000010u);
    CHECK_EQ(a.dwReserved0, (DWORD)0755);
    CHECK_EQ(a.nFileSizeHigh, (DWORD)1);
    CHECK(std::string(a.cFileName) == "f\xF0\x9F\x98\x80");
}
TEST(loga_falls_back_to_w_callback) {
    extern tLogProcW LogProcW;   // from test_globals.cpp
    static std::wstring got;
    LogProcW = [](int, int, WCHAR* s) { got = u16_to_ws(s); };
    LogA(1, "hello");
    LogProcW = nullptr;
    CHECK(got == L"hello");
}

int main() { return run_all(); }
```

- [ ] **Step 5: Write `tests/test_globals.cpp`**

```cpp
#include "../adbfsplugin.h"

int PluginNumber = 0;
tProgressProc ProgressProc = NULL;
tLogProc LogProc = NULL;
tRequestProc RequestProc = NULL;
tProgressProcW ProgressProcW = NULL;
tLogProcW LogProcW = NULL;
tRequestProcW RequestProcW = NULL;
```

- [ ] **Step 6: Run `make test` after adding `tests/test_globals.cpp wfxcompat.cpp` to `UNIT_SRCS` — expect compile FAIL (wfxcompat.cpp missing)**

- [ ] **Step 7: Implement `wfxcompat.cpp`**

```cpp
#include "wfxcompat.h"
#include "adbfsplugin.h"

namespace {

const uint32_t kReplacement = 0xFFFD;

void append_cp_utf8(std::string& out, uint32_t c) {
    if (c < 0x80) { out.push_back((char)c); }
    else if (c < 0x800) {
        out.push_back((char)(0xC0 | (c >> 6)));
        out.push_back((char)(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back((char)(0xE0 | (c >> 12)));
        out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (c & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (c >> 18)));
        out.push_back((char)(0x80 | ((c >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (c & 0x3F)));
    }
}

uint32_t next_cp_utf8(const std::string& s, size_t& i) {
    unsigned char c = (unsigned char)s[i++];
    if (c < 0x80) return c;
    int extra; uint32_t cp;
    if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else return kReplacement;
    for (int k = 0; k < extra; k++) {
        if (i >= s.size() || ((unsigned char)s[i] & 0xC0) != 0x80) return kReplacement;
        cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
    }
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return kReplacement;
    return cp;
}

uint32_t sanitize(uint32_t c) {
    if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return kReplacement;
    return c;
}

} // namespace

std::wstring u16_to_ws(const WCHAR* s) {
    std::wstring out;
    if (!s) return out;
    for (size_t i = 0; s[i]; ) {
        uint32_t c = s[i++];
        if (c >= 0xD800 && c <= 0xDBFF && s[i] >= 0xDC00 && s[i] <= 0xDFFF)
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i++] - 0xDC00);
        out.push_back((wchar_t)c);
    }
    return out;
}

std::string ws_to_utf8(const std::wstring& s) {
    std::string out;
    for (wchar_t wc : s) append_cp_utf8(out, sanitize((uint32_t)wc));
    return out;
}

std::wstring utf8_to_ws(const std::string& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size(); ) out.push_back((wchar_t)next_cp_utf8(s, i));
    return out;
}

std::string u16_to_utf8(const WCHAR* s) { return ws_to_utf8(u16_to_ws(s)); }

size_t u16len(const WCHAR* s) { size_t n = 0; while (s && s[n]) n++; return n; }

void ws_to_u16buf(WCHAR* dst, size_t dstlen, const std::wstring& src) {
    size_t o = 0;
    for (wchar_t wc : src) {
        uint32_t c = sanitize((uint32_t)wc);
        if (c < 0x10000) {
            if (o + 1 >= dstlen) break;
            dst[o++] = (WCHAR)c;
        } else {
            if (o + 2 >= dstlen) break;
            c -= 0x10000;
            dst[o++] = (WCHAR)(0xD800 + (c >> 10));
            dst[o++] = (WCHAR)(0xDC00 + (c & 0x3FF));
        }
    }
    dst[o] = 0;
}

void utf8_to_u16buf(WCHAR* dst, size_t dstlen, const char* src) {
    ws_to_u16buf(dst, dstlen, utf8_to_ws(src ? src : ""));
}

void u16_to_utf8buf(char* dst, size_t dstlen, const WCHAR* src) {
    ws_to_utf8buf(dst, dstlen, u16_to_ws(src));
}

void ws_to_utf8buf(char* dst, size_t dstlen, const std::wstring& src) {
    std::string u8 = ws_to_utf8(src);
    size_t n = u8.size();
    if (n >= dstlen) {
        n = dstlen ? dstlen - 1 : 0;
        while (n > 0 && ((unsigned char)u8[n] & 0xC0) == 0x80) n--;  // don't split a UTF-8 sequence
    }
    memcpy(dst, u8.data(), n);
    if (dstlen) dst[n] = 0;
}

WCHAR* awfilenamecopy_impl(WCHAR* dst, size_t n, const char* src) {
    utf8_to_u16buf(dst, n, src);
    return dst;
}

void copyfinddatawa(WIN32_FIND_DATAA* a, WIN32_FIND_DATAW* w) {
    u16_to_utf8buf(a->cFileName, sizeof(a->cFileName), w->cFileName);
    u16_to_utf8buf(a->cAlternateFileName, sizeof(a->cAlternateFileName), w->cAlternateFileName);
    a->dwFileAttributes = w->dwFileAttributes;
    a->dwReserved0 = w->dwReserved0;
    a->dwReserved1 = w->dwReserved1;
    a->ftCreationTime = w->ftCreationTime;
    a->ftLastAccessTime = w->ftLastAccessTime;
    a->ftLastWriteTime = w->ftLastWriteTime;
    a->nFileSizeHigh = w->nFileSizeHigh;
    a->nFileSizeLow = w->nFileSizeLow;
}

int ProgressT(const std::wstring& source, const std::wstring& target, int percent) {
    if (ProgressProcW) {
        WCHAR a[wdirtypemax], b[wdirtypemax];
        ws_to_u16buf(a, countof(a), source);
        ws_to_u16buf(b, countof(b), target);
        return ProgressProcW(PluginNumber, a, b, percent);
    }
    if (ProgressProc) {
        char a[wdirtypemax], b[wdirtypemax];
        ws_to_utf8buf(a, sizeof(a), source);
        ws_to_utf8buf(b, sizeof(b), target);
        return ProgressProc(PluginNumber, a, b, percent);
    }
    return 0;
}

void LogT(int msgtype, const std::wstring& msg) {
    if (LogProcW) {
        WCHAR buf[wdirtypemax];
        ws_to_u16buf(buf, countof(buf), msg);
        LogProcW(PluginNumber, msgtype, buf);
    } else if (LogProc) {
        char buf[wdirtypemax];
        ws_to_utf8buf(buf, sizeof(buf), msg);
        LogProc(PluginNumber, msgtype, buf);
    }
}

void LogA(int msgtype, const char* msg) {
    if (LogProc) {
        LogProc(PluginNumber, msgtype, (char*)msg);
    } else if (LogProcW) {
        WCHAR buf[wdirtypemax];
        utf8_to_u16buf(buf, countof(buf), msg);
        LogProcW(PluginNumber, msgtype, buf);
    }
}
```

Note: `u16_to_ws` reads `s[i]` after a high surrogate — safe because the string is NUL-terminated (a NUL fails the low-surrogate range check before any further read).

- [ ] **Step 8: Update `Makefile`: `UNIT_SRCS := tests/test_main.cpp tests/test_globals.cpp wfxcompat.cpp`; add `platform.h wfxcompat.h adbfsplugin.h sdk/common.h sdk/wfxplugin.h` to the unit_tests dependency line**
- [ ] **Step 9: Run `make test` — expect all tests PASS**
- [ ] **Step 10: Commit** (`git add -A && git commit -m "Add platform layer and UTF-16/UTF-8 compat with tests"`)

---

### Task 3: Port adbhandler (pure helpers TDD + POSIX AdbCommunicator)

**Files:**
- Replace: `adbhandler.h`, `adbhandler.cpp`
- Modify: `Makefile` (add `adbhandler.cpp` to UNIT_SRCS), `tests/test_main.cpp` (add tests)

**Interfaces:**
- Produces (used by Task 4/5):
  ```cpp
  class AdbCommunicator;                      // singleton: instance()/disconnect()
  enum FileTypeEnum { REGFILE, DIRECTORY, LINK, OTHER };
  class FileData;                             // type, mode, size(int64_t), times, uid, gid, name/alt_name/cache_name (wstring)
  int decode64(const char* in, char* out);    // 4 chars -> up to 3 bytes; "====" -> 0
  int encode64(const char* in, char* out);    // 3 bytes -> 4 chars
  std::wstring QuoteString(std::wstring str);
  std::string trim(std::string const& str, const char* sepSet);
  std::wstring PathConverter(std::wstring path);          // '\' -> '/'
  bool ParseStatLine(const std::wstring& line, FileData* fd);
  int64_t unixTimeToFileTime(unsigned int utime);
  unsigned int fileTimeToUnixTime(int64_t ftime);
  std::string FindAdbBinary();
  std::list<FileData*>* DirList(std::wstring filename);
  void GetStat(WIN32_FIND_DATAW* fs, FileData* fd);
  bool RunCommand(std::wstring comm);
  #define BUF_SIZE 8192
  ```

- [ ] **Step 1: Add failing tests to `tests/test_main.cpp`** (include `../adbhandler.h`)

```cpp
TEST(base64_encode_groups) {
    char out[5] = {0};
    encode64("Man", out);
    CHECK(std::string(out, 4) == "TWFu");
}
TEST(base64_decode_full_and_padded) {
    char out[8];
    CHECK_EQ(decode64("TWFu", out), 3); CHECK(std::string(out, 3) == "Man");
    CHECK_EQ(decode64("TWE=", out), 2); CHECK(std::string(out, 2) == "Ma");
    CHECK_EQ(decode64("TQ==", out), 1); CHECK(std::string(out, 1) == "M");
    CHECK_EQ(decode64("====", out), 0);
}
TEST(base64_roundtrip_binary) {
    unsigned char raw[3] = {0x00, 0xFF, 0x7F};
    char enc[5] = {0}, dec[4];
    encode64((const char*)raw, enc);
    CHECK_EQ(decode64(enc, dec), 3);
    CHECK_EQ(memcmp(raw, dec, 3), 0);
}
TEST(quote_string) {
    CHECK(QuoteString(L"abc") == L"'abc'");
    CHECK(QuoteString(L"a'b") == L"'a'\\''b'");
    CHECK(QuoteString(L"") == L"''");
}
TEST(trim_strips) {
    CHECK(trim("  x y \r\n", " \t\r\n") == "x y");
    CHECK(trim("\r\n", " \t\r\n") == "");
}
TEST(path_converter) {
    CHECK(PathConverter(L"\\a\\b c\\d") == L"/a/b c/d");
}
TEST(time_conversion) {
    CHECK_EQ(unixTimeToFileTime(0), (int64_t)116444736000000000LL);
    CHECK_EQ(fileTimeToUnixTime(unixTimeToFileTime(1600000000u)), 1600000000u);
}
TEST(parse_stat_regular_file) {
    FileData fd;
    CHECK(ParseStatLine(L"644 -regular file- 1000 2000 12 1700000000 1700000001 1700000002 'file one'", &fd));
    CHECK_EQ(fd.mode, 0644u);
    CHECK_EQ((int)fd.type, (int)REGFILE);
    CHECK_EQ(fd.gid, 1000u);
    CHECK_EQ(fd.uid, 2000u);
    CHECK_EQ(fd.size, (int64_t)12);
    CHECK_EQ(fd.accessTime, 1700000000u);
    CHECK_EQ(fd.modificationTime, 1700000001u);
    CHECK_EQ(fd.changeTime, 1700000002u);
    CHECK(fd.alt_name == L"'file one'");
}
TEST(parse_stat_directory_and_link) {
    FileData d, l;
    CHECK(ParseStatLine(L"755 -directory- 0 0 4096 1 2 3 'subdir'", &d));
    CHECK_EQ((int)d.type, (int)DIRECTORY);
    CHECK(ParseStatLine(L"777 -symbolic link- 0 0 11 1 2 3 'link1' -> '/target'", &l));
    CHECK_EQ((int)l.type, (int)LINK);
    CHECK(l.alt_name == L"'link1' -> '/target'");
}
TEST(parse_stat_large_size) {
    FileData fd;
    CHECK(ParseStatLine(L"600 -regular file- 0 0 4294967301 1 2 3 'big'", &fd));
    CHECK_EQ(fd.size, (int64_t)4294967301LL);   // > 32 bits
}
TEST(parse_stat_rejects_malformed) {
    FileData fd;
    CHECK(!ParseStatLine(L"", &fd));
    CHECK(!ParseStatLine(L"not a stat line", &fd));
    CHECK(!ParseStatLine(L"644 -regular file- 0 0 5 1 2 3", &fd));  // missing name
}
TEST(getstat_maps_find_data) {
    FileData fd;
    fd.name = L"sub";
    fd.type = DIRECTORY;
    fd.mode = 0755;
    fd.size = ((int64_t)1 << 32) | 5;
    fd.modificationTime = 1600000000u;
    WIN32_FIND_DATAW fs;
    GetStat(&fs, &fd);
    CHECK(fs.dwFileAttributes & 0x80000000u);            // FILE_ATTRIBUTE_UNIX_MODE
    CHECK(fs.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    CHECK_EQ(fs.dwReserved0, (DWORD)0755);
    CHECK_EQ(fs.nFileSizeHigh, (DWORD)1);
    CHECK_EQ(fs.nFileSizeLow, (DWORD)5);
    int64_t ft = ((int64_t)fs.ftLastWriteTime.dwHighDateTime << 32) | fs.ftLastWriteTime.dwLowDateTime;
    CHECK_EQ(ft, unixTimeToFileTime(1600000000u));
    CHECK(u16_to_ws(fs.cFileName) == L"sub");
}
TEST(find_adb_binary_env_override) {
    setenv("ADBFS_ADB", "/usr/bin/true", 1);
    CHECK(FindAdbBinary() == "/usr/bin/true");
    unsetenv("ADBFS_ADB");
}
```

- [ ] **Step 2: Run — expect compile FAIL (adbhandler not ported yet)**

- [ ] **Step 3: Replace `adbhandler.h`**

```cpp
#pragma once
#include "platform.h"
#include "wfxcompat.h"

#define BUF_SIZE 8192

// Singleton talking to the local ADB server (smart-socket protocol + device shell)
class AdbCommunicator {
public:
    static AdbCommunicator* instance() { if (!_global_adb) _global_adb = new AdbCommunicator(); return _global_adb; }
    static void disconnect() { if (_global_adb) delete _global_adb; _global_adb = NULL; }
    std::wstring* ReadLineW();
    std::string* ReadLine();
    int PutData(const char* data, int length);
    void CleanBuffer(bool timeout);
    void PushCommandW(std::wstring command);
    void SetSU(bool needsu) { _needsu = needsu; }
private:
    AdbCommunicator();
    ~AdbCommunicator();
    void ReConnect();
    void Close();
    void SendStringToServer(const char* str);
    int ReadBuf(void);

    SOCKET s;
    bool _needsu;
    static AdbCommunicator* _global_adb;

    char actbuf[BUF_SIZE];
    char* actbufpospoint;
    int actbufsize;
    int actbufpos;
};

enum FileTypeEnum { REGFILE, DIRECTORY, LINK, OTHER };

class FileData {
public:
    FileData(std::wstring _name) : type(REGFILE), mode(0), size(0), accessTime(0),
        modificationTime(0), changeTime(0), uid(0), gid(0),
        alt_name(_name), name(_name), cache_name(_name) {}
    FileData() : type(REGFILE), mode(0), size(0), accessTime(0),
        modificationTime(0), changeTime(0), uid(0), gid(0) {}
    FileTypeEnum type;
    unsigned int mode;
    int64_t size;
    unsigned int accessTime, modificationTime, changeTime;
    unsigned int uid, gid;
    std::wstring alt_name;
    std::wstring name;
    std::wstring cache_name;
};

int decode64(const char* input, char* output);
int encode64(const char* input, char* output);
std::wstring QuoteString(std::wstring str);
std::string trim(std::string const& str, const char* sepSet);
std::wstring PathConverter(std::wstring path);
bool ParseStatLine(const std::wstring& line, FileData* fd);
int64_t unixTimeToFileTime(unsigned int utime);
unsigned int fileTimeToUnixTime(int64_t ftime);
std::string FindAdbBinary();

std::list<FileData*>* DirList(std::wstring filename);
void GetStat(WIN32_FIND_DATAW* fs, FileData* fd);
bool RunCommand(std::wstring comm);
```

- [ ] **Step 4: Replace `adbhandler.cpp`** — POSIX port; changes vs upstream called out inline:

```cpp
#include "adbhandler.h"
#include "adbfsplugin.h"
#include <spawn.h>
#include <sys/wait.h>

extern char** environ;

using namespace std;

/* -------------------------------
   ---- Some helper functions ----
   ------------------------------- */

#define EPOCH_DIFF 0x019DB1DED53E8000LL /* 116444736000000000 nsecs */
#define RATE_DIFF 10000000 /* 100 nsecs */

int64_t unixTimeToFileTime(unsigned int utime) {
    return ((int64_t)utime * RATE_DIFF) + EPOCH_DIFF;
}

unsigned int fileTimeToUnixTime(int64_t ftime) {
    return (unsigned int)((ftime - EPOCH_DIFF) / RATE_DIFF);
}

string trim(string const& str, const char* sepSet) {
    string::size_type const first = str.find_first_not_of(sepSet);
    return (first == string::npos) ? string() : str.substr(first, str.find_last_not_of(sepSet) - first + 1);
}

wstring PathConverter(wstring path) {
    for (auto& c : path)
        if (c == L'\\') c = L'/';
    return path;
}

// adb binary discovery: $ADBFS_ADB, then PATH, then common macOS install locations
string FindAdbBinary() {
    const char* env = getenv("ADBFS_ADB");
    if (env && *env) return env;
    vector<string> dirs;
    if (const char* path = getenv("PATH")) {
        string p = path;
        size_t start = 0;
        while (start <= p.size()) {
            size_t end = p.find(':', start);
            if (end == string::npos) end = p.size();
            if (end > start) dirs.push_back(p.substr(start, end - start));
            start = end + 1;
        }
    }
    dirs.push_back("/opt/homebrew/bin");
    dirs.push_back("/usr/local/bin");
    if (const char* home = getenv("HOME"))
        dirs.push_back(string(home) + "/Library/Android/sdk/platform-tools");
    for (auto& d : dirs) {
        string candidate = d + "/adb";
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return "adb";
}

wstring QuoteString(wstring str) {
    wstring result = L"'";
    for (auto i = str.begin(); i != str.end(); i++) {
        if (*i == L'\'') result.append(L"'\\''");
        else result.push_back(*i);
    }
    result.append(L"'");
    return result;
}

unsigned char base64table[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
unsigned char base64table2[257] = "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\x3E~~~\x3F\x34\x35\x36\x37\x38\x39\x3A\x3B\x3C\x3D~~~\x00~~~\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A\x0B\x0C\x0D\x0E\x0F\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19~~~~~~\x1A\x1B\x1C\x1D\x1E\x1F\x20\x21\x22\x23\x24\x25\x26\x27\x28\x29\x2A\x2B\x2C\x2D\x2E\x2F\x30\x31\x32\x33~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~";

int decode64(const char* input, char* output) {
    if ((input[3] == '=') && (input[2] == '=') && (input[0] == '=') && (input[1] == '=')) return 0;
    unsigned int n = (base64table2[(unsigned char)input[0]] << 18) | (base64table2[(unsigned char)input[1]] << 12) |
                     (base64table2[(unsigned char)input[2]] << 6) | (base64table2[(unsigned char)input[3]]);
    output[0] = (char)(n >> 16);
    output[1] = (char)((n >> 8) & 0xFF);
    output[2] = (char)(n & 0xFF);
    return 1 + ((input[3] != '=') ? 1 : 0) + ((input[2] != '=') ? 1 : 0);
}

int encode64(const char* input, char* output) {
    unsigned int n = ((unsigned char)input[0] << 16) | ((unsigned char)input[1] << 8) | (unsigned char)input[2];
    output[0] = base64table[n >> 18];
    output[1] = base64table[(n >> 12) & 0x3F];
    output[2] = base64table[(n >> 6) & 0x3F];
    output[3] = base64table[n & 0x3F];
    return 4;
}

/* ---------------------------
   ---- Adb Communicator -----
   --------------------------- */

AdbCommunicator* AdbCommunicator::_global_adb = 0;

AdbCommunicator::~AdbCommunicator() {
    Close();
    LogA(MSGTYPE_DISCONNECT, "Closing plugin");
}

AdbCommunicator::AdbCommunicator() {
    string adb = FindAdbBinary();
    LogA(MSGTYPE_DETAILS, ("Starting ADB server: " + adb).c_str());
    pid_t pid = 0;
    const char* argv[] = { adb.c_str(), "start-server", NULL };
    // upstream threw when adb couldn't be spawned; here we log and still try to
    // connect — the server may already be running
    if (posix_spawnp(&pid, adb.c_str(), NULL, NULL, (char* const*)argv, environ) == 0) {
        int status = 0;
        waitpid(pid, &status, 0);   // adb start-server returns once the daemon is up
    } else {
        LogA(MSGTYPE_IMPORTANTERROR, "Could not run 'adb start-server' (is adb installed?), trying to connect anyway");
    }
    s = INVALID_SOCKET;
    _needsu = (getenv("ADBFS_NO_SU") == NULL);
    actbufsize = 0;
    actbufpos = 0;
    actbufpospoint = actbuf;
}

void AdbCommunicator::Close() {
    LogA(MSGTYPE_DISCONNECT, "Closing connection /");
    if (s != INVALID_SOCKET) closesocket(s);
    s = INVALID_SOCKET;
    actbufsize = 0;
    actbufpos = 0;
}

void AdbCommunicator::SendStringToServer(const char* str) {
    if (send(s, str, strlen(str), 0) == SOCKET_ERROR) {
        Close();
        throw wstring(L"<0009 - could not switch to usb mode>");
    }
    char recbuf[5];
    recbuf[4] = '\0';
    ssize_t bytesRead = recv(s, recbuf, 4, MSG_WAITALL);
    if (bytesRead != 4) {
        Close();
        throw wstring(L"<000A - no ack data from adb server>");
    }
    if (strcasecmp("FAIL", recbuf) == 0) {
        recv(s, recbuf, 4, MSG_WAITALL);
        int datalen = 0;
        sscanf(recbuf, "%x", &datalen);
        if (datalen > 0) {
            char* data = new char[datalen + 1];
            recv(s, data, datalen, MSG_WAITALL);
            delete[] data;
        }
        Close();
        throw wstring(L"<000B - FAIL response from adb server>");
    } else if (strcasecmp("OKAY", recbuf) != 0) {
        Close();
        throw wstring(L"<000C - Bad response from adb server>");
    }
}

void AdbCommunicator::ReConnect() {
    LogA(MSGTYPE_CONNECT, "CONNECT /");
    struct addrinfo* result = NULL;
    struct addrinfo hints;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const char* port = getenv("ANDROID_ADB_SERVER_PORT");
    if (!port || !*port) port = "5037";

    if (getaddrinfo("127.0.0.1", port, &hints, &result) != 0) {
        throw wstring(L"<0007 - localhost not found>");
    }
    s = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(result);
        throw wstring(L"<0006 - socket initialization failed>");
    }
    if (connect(s, result->ai_addr, result->ai_addrlen) == SOCKET_ERROR) {
        freeaddrinfo(result);
        Close();
        throw wstring(L"<0008 - could not connect to local adb server>");
    }
    freeaddrinfo(result);

    // switch to usb mode
    // TODO: multiple devices support
    SendStringToServer("0012host:transport-usb");
    // start shell
    SendStringToServer("0006shell:");

    if (_needsu) {
        Sleep(500);         // let the shell start
        CleanBuffer(false); // remove everything in buffer
        send(s, "su\n", 3, 0);
        Sleep(50);          // small timeout for the echo
        CleanBuffer(false); // remove echo
        CleanBuffer(true);  // wait for root
    }
}

void AdbCommunicator::CleanBuffer(bool timeout) {
    actbufsize = 0;
    actbufpos = 0;
    actbufpospoint = actbuf;
    for (;;) {
        TIMEVAL tv;
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        fd_set set;
        FD_ZERO(&set);
        FD_SET(s, &set);
        if (select(s + 1, &set, NULL, NULL, timeout ? NULL : &tv) <= 0) return;
        recv(s, actbuf, BUF_SIZE, 0);
        if (timeout) return;
    }
}

void AdbCommunicator::PushCommandW(wstring command) {
    if (s == INVALID_SOCKET) {
        ReConnect();
        Sleep(500); // wait for the shell to start
    }

    CleanBuffer(false);

    // add some garbage data to determine where sending starts and where it stops
    command = L"echo \"===adbfsplugin<--\" ;" + command + L" ; echo \"===adbfsplugin-->\"";

    string comm = ws_to_utf8(command);
    comm.push_back('\n');
    if (send(s, comm.c_str(), comm.size(), 0) == SOCKET_ERROR) {
        Close();
        throw wstring(L"<000D - Command send failed>");
    }

    // throw out initial garbage (the shell echoing the command back)
    string* line = ReadLine();
    while ((line != NULL) && (*line != "===adbfsplugin<--")) {
        delete line;
        line = ReadLine();
    }
    if (line) delete line;
}

int AdbCommunicator::ReadBuf(void) {
    actbufpos++;
    actbufpospoint++;
    if (actbufsize <= actbufpos) {
        ssize_t n = recv(s, actbuf, BUF_SIZE, 0);
        actbufpos = 0;
        actbufpospoint = actbuf;
        if (n < 0) {
            actbufsize = 0;
            return SOCKET_ERROR;
        }
        actbufsize = (int)n;
    }
    return actbufsize - actbufpos;
}

int AdbCommunicator::PutData(const char* data, int length) {
    return (int)send(s, data, length, 0);
}

string* AdbCommunicator::ReadLine() {
    string input = "";
    int bytesRead = ReadBuf();
    int size = 0;
    while ((bytesRead != SOCKET_ERROR) && (bytesRead != 0) && (*actbufpospoint != '\n') &&
           ((size != 17) || (input != "===adbfsplugin-->"))) {
        size++;
        input.push_back(*actbufpospoint);
        bytesRead = ReadBuf();
    }
    if (bytesRead == SOCKET_ERROR) {
        Close();
        throw wstring(L"Socket Error");
    }
    if (input.empty() || input == "===adbfsplugin-->") {
        return NULL;
    }
    return new string(trim(input, " \t\r\n"));
}

wstring* AdbCommunicator::ReadLineW() {
    string* input = ReadLine();
    if (input == NULL) return NULL;
    wstring* result = new wstring(utf8_to_ws(*input));
    delete input;
    return result;
}

/* ---------------------------
   ---- FileData Helpers -----
   --------------------------- */

// Parses one line of `busybox stat -c "%a -%F- %g %u %s %X %Y %Z %N"` output.
// Example: 644 -regular file- 1000 2000 12 1700000000 1700000001 1700000002 'file one'
bool ParseStatLine(const wstring& line, FileData* fd) {
    size_t pos = 0;
    auto skipSpaces = [&] { while (pos < line.size() && line[pos] == L' ') pos++; };
    auto parseUInt = [&](uint64_t* out) -> bool {
        skipSpaces();
        size_t start = pos;
        uint64_t v = 0;
        while (pos < line.size() && line[pos] >= L'0' && line[pos] <= L'9') {
            v = v * 10 + (line[pos] - L'0');
            pos++;
        }
        *out = v;
        return pos > start;
    };

    skipSpaces();
    size_t start = pos;
    unsigned int mode = 0;
    while (pos < line.size() && line[pos] >= L'0' && line[pos] <= L'7') {
        mode = mode * 8 + (unsigned int)(line[pos] - L'0');
        pos++;
    }
    if (pos == start) return false;

    skipSpaces();
    if (pos >= line.size() || line[pos] != L'-') return false;
    pos++;
    size_t typeEnd = line.find(L'-', pos);
    if (typeEnd == wstring::npos) return false;
    wstring type = line.substr(pos, typeEnd - pos);
    pos = typeEnd + 1;

    uint64_t gid, uid, size, atime, mtime, ctime;
    if (!parseUInt(&gid) || !parseUInt(&uid) || !parseUInt(&size) ||
        !parseUInt(&atime) || !parseUInt(&mtime) || !parseUInt(&ctime))
        return false;

    if (pos < line.size() && line[pos] == L' ') pos++;
    if (pos >= line.size()) return false;   // name is mandatory

    fd->mode = mode;
    fd->gid = (unsigned int)gid;
    fd->uid = (unsigned int)uid;
    fd->size = (int64_t)size;
    fd->accessTime = (unsigned int)atime;
    fd->modificationTime = (unsigned int)mtime;
    fd->changeTime = (unsigned int)ctime;
    fd->type = OTHER;
    if (type == L"directory") fd->type = DIRECTORY;
    else if (type == L"symbolic link") fd->type = LINK;
    else if (type == L"regular file" || type == L"regular empty file") fd->type = REGFILE;
    fd->alt_name = line.substr(pos);
    return true;
}

void FillStat(wstring directory, list<FileData*>* fd) {
    try {
        wstring command = L"busybox stat -c \"%a -%F- %g %u %s %X %Y %Z %N\" ";
        for (auto i = fd->begin(); i != fd->end(); i++) {
            command.append(L" ");
            command.append(QuoteString(directory + (*i)->name));
        }
        AdbCommunicator::instance()->PushCommandW(command);
        wstring* line = AdbCommunicator::instance()->ReadLineW();
        auto i = fd->begin();
        while ((line != NULL) && (i != fd->end())) {
            (*i)->cache_name = directory + (*i)->name;
            if (!ParseStatLine(*line, *i)) {
                (*i)->alt_name = L"<0005 - stat failed>";
            }
            delete line;
            line = AdbCommunicator::instance()->ReadLineW();
            i++;
        }
        if (line) delete line;
    } catch (wstring&) {
    }
}

void GetStat(WIN32_FIND_DATAW* fs, FileData* fd) {
    memset(fs, 0, sizeof(WIN32_FIND_DATAW));
    ws_to_u16buf(fs->cFileName, countof(fs->cFileName), fd->name);
    fs->dwFileAttributes = FILE_ATTRIBUTE_UNIX_MODE;
    if (fd->type == DIRECTORY) {
        fs->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    }
    fs->dwReserved0 = fd->mode;
    int64_t ft = unixTimeToFileTime(fd->modificationTime);
    fs->ftLastWriteTime.dwLowDateTime = (DWORD)(ft & 0xFFFFFFFF);
    fs->ftLastWriteTime.dwHighDateTime = (DWORD)(ft >> 32);
    fs->nFileSizeHigh = (DWORD)(fd->size >> 32);
    fs->nFileSizeLow = (DWORD)(fd->size & 0x0FFFFFFFF);
}

list<FileData*>* DirList(wstring filename) {
    auto* result = new list<FileData*>();
    try {
        AdbCommunicator::instance()->PushCommandW(wstring(L"busybox ls --color=never -1 ") + QuoteString(filename));
        wstring* line = AdbCommunicator::instance()->ReadLineW();
        while (line != NULL) {
            result->push_back(new FileData(*line));
            delete line;
            line = AdbCommunicator::instance()->ReadLineW();
        }
    } catch (wstring& e) {
        result->push_back(new FileData(e));
    }

    // Get stat in batches of 10 files
    auto* l = new list<FileData*>();
    for (auto i = result->begin(); i != result->end(); i++) {
        l->push_back(*i);
        if (l->size() > 10) {
            FillStat(filename, l);
            l->clear();
        }
    }
    if (!l->empty()) {
        FillStat(filename, l);
    }
    delete l;
    return result;
}

bool RunCommand(wstring comm) {
    try {
        AdbCommunicator::instance()->PushCommandW(comm);
        wstring* line = AdbCommunicator::instance()->ReadLineW();
        while (line != NULL) {
            LogT(MSGTYPE_DETAILS, *line);
            delete line;
            line = AdbCommunicator::instance()->ReadLineW();
        }
        return true;
    } catch (wstring& e) {
        LogT(MSGTYPE_IMPORTANTERROR, e);
        return false;
    }
}
```

- [ ] **Step 5: Add `adbhandler.cpp` to `UNIT_SRCS`; run `make test` — expect PASS**
- [ ] **Step 6: Commit** (`"Port adbhandler to POSIX with stat-line parser and tests"`)

---

### Task 4: Port the WFX API surface + build the .wfx + dlopen test

**Files:**
- Replace: `adbfsplugin.cpp`
- Create: `tests/test_dlopen.cpp`
- Modify: `Makefile` (plugin + dlopen targets)

**Interfaces:**
- Consumes: everything from Tasks 2–3.
- Produces: `build/adbfsplugin.wfx` with all 44 exports from `adbfsplugin.def`; content-plugin field order `mode, uid, gid, type, name` (indices 0–4).

- [ ] **Step 1: Replace `adbfsplugin.cpp`** (full port; behavior changes vs upstream: null-safe callbacks, `FsRenMovFileW` failure returns `FS_FILE_WRITEERROR` instead of 5/USERABORT, `FsSetAttrW` unsupported, preview bitmaps disabled, fixed fclose leaks, fixed `FsFindFirstW` empty-list leak):

```cpp
// adbfsplugin.cpp : WFX plugin entry points (Double Commander / Total Commander API)

#include "platform.h"
#include "adbfsplugin.h"
#include "adbhandler.h"
#include "wfxcompat.h"

#define pluginrootlen 1

using namespace std;

char inifilename[MAX_PATH] = "adbfsplugin.ini";  // Unused in this plugin, may be used to save data

int PluginNumber;
tProgressProc ProgressProc = NULL;
tLogProc LogProc = NULL;
tRequestProc RequestProc = NULL;
tProgressProcW ProgressProcW = NULL;
tLogProcW LogProcW = NULL;
tRequestProcW RequestProcW = NULL;
map<wstring, FileData> cacheMap;

DCEXPORT int DCPCALL FsInit(int PluginNr, tProgressProc pProgressProc, tLogProc pLogProc, tRequestProc pRequestProc) {
    ProgressProc = pProgressProc;
    LogProc = pLogProc;
    RequestProc = pRequestProc;
    PluginNumber = PluginNr;
    return 0;
}

DCEXPORT int DCPCALL FsInitW(int PluginNr, tProgressProcW pProgressProcW, tLogProcW pLogProcW, tRequestProcW pRequestProcW) {
    ProgressProcW = pProgressProcW;
    LogProcW = pLogProcW;
    RequestProcW = pRequestProcW;
    PluginNumber = PluginNr;
    return 0;
}

typedef struct {
    list<FileData*>* result;
    wstring path;
    int origlength;
} FindDataHandle;

DCEXPORT HANDLE DCPCALL FsFindFirstW(WCHAR* Path, WIN32_FIND_DATAW* FindData) {
    cacheMap.clear();
    wstring path = PathConverter(u16_to_ws(Path));
    if (path.empty() || path.back() != L'/') {
        path.push_back(L'/');
    }
    list<FileData*>* result = DirList(path);
    memset(FindData, 0, sizeof(WIN32_FIND_DATAW));
    if (result->empty()) {
        delete result;
        return INVALID_HANDLE_VALUE;
    }
    for (auto i = result->begin(); i != result->end(); i++) {
        cacheMap[(*i)->cache_name] = **i;
    }
    FindDataHandle* r = new FindDataHandle;
    r->path = path;

    FileData* back = result->back();
    result->pop_back();
    GetStat(FindData, back);
    delete back;

    r->result = result;
    r->origlength = (int)result->size();
    return r;
}

DCEXPORT HANDLE DCPCALL FsFindFirst(char* Path, WIN32_FIND_DATAA* FindData) {
    WIN32_FIND_DATAW FindDataW;
    WCHAR PathW[wdirtypemax];
    HANDLE retval = FsFindFirstW(awfilenamecopy(PathW, Path), &FindDataW);
    if (retval != INVALID_HANDLE_VALUE)
        copyfinddatawa(FindData, &FindDataW);
    return retval;
}

DCEXPORT BOOL DCPCALL FsFindNextW(HANDLE Hdl, WIN32_FIND_DATAW* FindData) {
    FindDataHandle* r = (FindDataHandle*)Hdl;
    list<FileData*>* result = r->result;
    if (result->empty()) {
        return 0;
    }
    FileData* str = result->back();
    result->pop_back();
    GetStat(FindData, str);
    delete str;
    return 1;
}

DCEXPORT BOOL DCPCALL FsFindNext(HANDLE Hdl, WIN32_FIND_DATAA* FindData) {
    WIN32_FIND_DATAW FindDataW;
    BOOL retval = FsFindNextW(Hdl, &FindDataW);
    if (retval)
        copyfinddatawa(FindData, &FindDataW);
    return retval;
}

DCEXPORT int DCPCALL FsFindClose(HANDLE Hdl) {
    if (Hdl == NULL || Hdl == INVALID_HANDLE_VALUE || Hdl == (HANDLE)(intptr_t)1)
        return 0;
    FindDataHandle* r = (FindDataHandle*)Hdl;
    list<FileData*>* result = r->result;
    while (!result->empty()) {
        delete result->back();
        result->pop_back();
    }
    delete result;
    delete r;
    return 0;
}

DCEXPORT BOOL DCPCALL FsMkDirW(WCHAR* Path) {
    return RunCommand(L"busybox mkdir " + QuoteString(PathConverter(u16_to_ws(Path))));
}

DCEXPORT BOOL DCPCALL FsMkDir(char* Path) {
    WCHAR wbuf[wdirtypemax];
    return FsMkDirW(awfilenamecopy(wbuf, Path));
}

DCEXPORT int DCPCALL FsExecuteFile(HWND MainWin, char* RemoteName, char* Verb) {
    return FS_EXEC_ERROR;
}

DCEXPORT int DCPCALL FsExecuteFileW(HWND MainWin, WCHAR* RemoteName, WCHAR* Verb) {
    return FS_EXEC_ERROR;
}

DCEXPORT int DCPCALL FsRenMovFileW(WCHAR* OldName, WCHAR* NewName, BOOL Move, BOOL OverWrite, RemoteInfoStruct* ri) {
    wstring oldq = QuoteString(PathConverter(u16_to_ws(OldName)));
    wstring newq = QuoteString(PathConverter(u16_to_ws(NewName)));
    if (Move) {
        return RunCommand(L"busybox mv -f " + oldq + L" " + newq) ? FS_FILE_OK : FS_FILE_WRITEERROR;
    } else {
        return RunCommand(L"busybox cp -f " + oldq + L" " + newq) ? FS_FILE_OK : FS_FILE_WRITEERROR;
    }
}

DCEXPORT int DCPCALL FsRenMovFile(char* OldName, char* NewName, BOOL Move, BOOL OverWrite, RemoteInfoStruct* ri) {
    WCHAR OldNameW[wdirtypemax], NewNameW[wdirtypemax];
    return FsRenMovFileW(awfilenamecopy(OldNameW, OldName), awfilenamecopy(NewNameW, NewName), Move, OverWrite, ri);
}

DCEXPORT int DCPCALL FsGetFileW(WCHAR* RemoteName, WCHAR* LocalName, int CopyFlags, RemoteInfoStruct* ri) {
    wstring remote = u16_to_ws(RemoteName);
    wstring local = u16_to_ws(LocalName);
    string local8 = ws_to_utf8(local);
    struct stat st;
    bool exists = (stat(local8.c_str(), &st) == 0);
    if (exists && (CopyFlags == 0 || CopyFlags == FS_COPYFLAGS_MOVE)) {
        return FS_FILE_EXISTS;
    }
    FILE* f = fopen(local8.c_str(), "wb+");
    if (f == NULL) return FS_FILE_WRITEERROR;
    try {
        AdbCommunicator::instance()->PushCommandW(L"busybox uuencode -m " + QuoteString(PathConverter(remote)) + L" x");
        string* line = AdbCommunicator::instance()->ReadLine();
        int64_t savedsize = 0;
        int64_t fullsize = ((int64_t)ri->SizeHigh << 32) | ri->SizeLow;
        if (fullsize <= 0) fullsize = 1;
        ProgressT(remote, local, 0);
        int outsize = 0;
        char out[BUF_SIZE * 4];
        while (line != NULL) {
            if (line->find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") == string::npos) {
                const char* pos = line->c_str();
                size_t len = line->size();
                for (size_t i = 0; i + 4 <= len; i += 4) {
                    outsize += decode64(pos + i, out + outsize);
                }
            }
            if (outsize > BUF_SIZE * 4 - 128) {
                if ((int)fwrite(out, 1, outsize, f) != outsize) {
                    fclose(f);
                    delete line;
                    return FS_FILE_WRITEERROR;
                }
                savedsize += outsize;
                outsize = 0;
                if (ProgressT(remote, local, (int)((double)savedsize / fullsize * 100))) {
                    fclose(f);
                    delete line;
                    AdbCommunicator::disconnect();
                    return FS_FILE_USERABORT;
                }
            }
            delete line;
            line = AdbCommunicator::instance()->ReadLine();
        }
        if (outsize != 0) {
            if ((int)fwrite(out, 1, outsize, f) != outsize) {
                fclose(f);
                return FS_FILE_WRITEERROR;
            }
        }
        ProgressT(remote, local, 100);
        fclose(f);
        return FS_FILE_OK;
    } catch (wstring&) {
        fclose(f);
        return FS_FILE_READERROR;
    }
}

DCEXPORT int DCPCALL FsGetFile(char* RemoteName, char* LocalName, int CopyFlags, RemoteInfoStruct* ri) {
    WCHAR RemoteNameW[wdirtypemax], LocalNameW[wdirtypemax];
    return FsGetFileW(awfilenamecopy(RemoteNameW, RemoteName), awfilenamecopy(LocalNameW, LocalName), CopyFlags, ri);
}

DCEXPORT int DCPCALL FsPutFileW(WCHAR* LocalName, WCHAR* RemoteName, int CopyFlags) {
    wstring local = u16_to_ws(LocalName);
    wstring remote = u16_to_ws(RemoteName);
    string local8 = ws_to_utf8(local);
    FILE* f = fopen(local8.c_str(), "rb");
    if (f == NULL) {
        return FS_FILE_READERROR;
    }
    try {
        AdbCommunicator::instance()->PushCommandW(L"busybox uudecode -o " + QuoteString(PathConverter(remote)));
        ProgressT(local, remote, 0);

        AdbCommunicator::instance()->PutData("begin-base64 644 x\n", 19);

        struct stat st;
        int64_t fullsize = (stat(local8.c_str(), &st) == 0) ? (int64_t)st.st_size : 0;
        if (fullsize <= 0) fullsize = 1;
        int64_t savedsize = 0;

        char buf[45];
        int read = (int)fread(buf, 1, 45, f);
        char out[61];
        out[60] = '\n';
        while (read == 45) {
            int outwr = 0;
            int inr = 0;
            while (inr != 45) {
                encode64(buf + inr, out + outwr);
                outwr += 4;
                inr += 3;
            }
            AdbCommunicator::instance()->PutData(out, 61);
            AdbCommunicator::instance()->CleanBuffer(false);
            read = (int)fread(buf, 1, 45, f);
            savedsize += inr;
            if (ProgressT(local, remote, (int)((double)savedsize / fullsize * 100))) {
                fclose(f);
                AdbCommunicator::disconnect();
                return FS_FILE_USERABORT;
            }
        }
        if (read > 0) {
            int outwr = 0;
            int inr = 0;
            while (read > 2) {
                encode64(buf + inr, out + outwr);
                inr += 3;
                outwr += 4;
                read -= 3;
            }
            if (read == 2) {
                buf[inr + 2] = 0;
                encode64(buf + inr, out + outwr);
                outwr += 4;
                out[outwr - 1] = '=';
                out[outwr] = '\n';
            } else if (read == 1) {
                buf[inr + 1] = 0;
                buf[inr + 2] = 0;
                encode64(buf + inr, out + outwr);
                outwr += 4;
                out[outwr - 2] = '=';
                out[outwr - 1] = '=';
                out[outwr] = '\n';
            } else {
                out[outwr] = '\n';   // upstream bug: sent uninitialized byte when size % 3 == 0
            }
            AdbCommunicator::instance()->PutData(out, outwr + 1);
        }
        AdbCommunicator::instance()->PutData("====\x04\n", 6);
        Sleep(100);

        ProgressT(local, remote, 100);
        fclose(f);
    } catch (wstring&) {
        fclose(f);
        return FS_FILE_WRITEERROR;
    }
    return FS_FILE_OK;
}

DCEXPORT int DCPCALL FsPutFile(char* LocalName, char* RemoteName, int CopyFlags) {
    WCHAR LocalNameW[wdirtypemax], RemoteNameW[wdirtypemax];
    return FsPutFileW(awfilenamecopy(LocalNameW, LocalName), awfilenamecopy(RemoteNameW, RemoteName), CopyFlags);
}

DCEXPORT BOOL DCPCALL FsDeleteFileW(WCHAR* RemoteName) {
    return RunCommand(L"busybox rm " + QuoteString(PathConverter(u16_to_ws(RemoteName))));
}

DCEXPORT BOOL DCPCALL FsDeleteFile(char* RemoteName) {
    WCHAR RemoteNameW[wdirtypemax];
    return FsDeleteFileW(awfilenamecopy(RemoteNameW, RemoteName));
}

DCEXPORT BOOL DCPCALL FsRemoveDirW(WCHAR* RemoteName) {
    if (u16len(RemoteName) < pluginrootlen + 2)
        return 0;
    return RunCommand(L"rm -r " + QuoteString(PathConverter(u16_to_ws(RemoteName))));
}

DCEXPORT BOOL DCPCALL FsRemoveDir(char* RemoteName) {
    WCHAR RemoteNameW[wdirtypemax];
    return FsRemoveDirW(awfilenamecopy(RemoteNameW, RemoteName));
}

DCEXPORT BOOL DCPCALL FsSetAttrW(WCHAR* RemoteName, int NewAttr) {
    // Windows file attributes don't map to the device; unsupported
    return 0;
}

DCEXPORT BOOL DCPCALL FsSetAttr(char* RemoteName, int NewAttr) {
    return 0;
}

DCEXPORT BOOL DCPCALL FsSetTimeW(WCHAR* RemoteName, FILETIME* CreationTime, FILETIME* LastAccessTime, FILETIME* LastWriteTime) {
    return 0;
}

DCEXPORT BOOL DCPCALL FsSetTime(char* RemoteName, FILETIME* CreationTime, FILETIME* LastAccessTime, FILETIME* LastWriteTime) {
    return 0;
}

DCEXPORT void DCPCALL FsStatusInfo(char* RemoteDir, int InfoStartEnd, int InfoOperation) {
}

DCEXPORT void DCPCALL FsGetDefRootName(char* DefRootName, int maxlen) {
    snprintf(DefRootName, maxlen, "%s", "Android");
}

DCEXPORT int DCPCALL FsExtractCustomIconW(WCHAR* RemoteName, int ExtractFlags, PWfxIcon TheIcon) {
    return FS_ICON_USEDEFAULT;
}

DCEXPORT int DCPCALL FsExtractCustomIcon(char* RemoteName, int ExtractFlags, PWfxIcon TheIcon) {
    return FS_ICON_USEDEFAULT;
}

DCEXPORT int DCPCALL FsGetPreviewBitmap(char* RemoteName, int width, int height, HBITMAP* ReturnedBitmap) {
    return FS_BITMAP_NONE;
}

DCEXPORT int DCPCALL FsGetPreviewBitmapW(WCHAR* RemoteName, int width, int height, HBITMAP* ReturnedBitmap) {
    return FS_BITMAP_NONE;
}

DCEXPORT void DCPCALL FsSetDefaultParams(FsDefaultParamStruct* dps) {
    snprintf(inifilename, sizeof(inifilename), "%s", dps->DefaultIniName);
}

/**************************************************************************************/
/*********************** content plugin = custom columns part! ************************/
/**************************************************************************************/

#define fieldcount 5
static const char* fieldnames[fieldcount] = {"mode", "uid", "gid", "type", "name"};
static int fieldtypes[fieldcount] = {ft_string, ft_numeric_32, ft_numeric_32, ft_string, ft_string};
static const char* fieldunits_and_multiplechoicestrings[fieldcount] = {"", "", "", "", ""};
static int fieldflags[fieldcount] = {0, 0, 0, 0, 0};
static int sortorders[fieldcount] = {-1, -1, -1, -1, -1};

DCEXPORT int DCPCALL FsContentGetSupportedField(int FieldIndex, char* FieldName, char* Units, int maxlen) {
    if (FieldIndex < 0 || FieldIndex >= fieldcount)
        return ft_nomorefields;
    snprintf(FieldName, maxlen, "%s", fieldnames[FieldIndex]);
    snprintf(Units, maxlen, "%s", fieldunits_and_multiplechoicestrings[FieldIndex]);
    return fieldtypes[FieldIndex];
}

static int ContentGetValue(const wstring& path, int FieldIndex, void* FieldValue, int maxlen) {
    auto it = cacheMap.find(path);
    if (it == cacheMap.end())
        return ft_fileerror;
    FileData* fd = &it->second;
    switch (FieldIndex) {
    case 0: {
        char* text = (char*)FieldValue;
        if (maxlen < 12) return ft_fileerror;
        snprintf(text, maxlen, "%s", "--- --- ---");
        if (fd->mode & 0400) { text[0] = 'r'; }
        if (fd->mode & 0200) { text[1] = 'w'; }
        if (fd->mode & 0100) { text[2] = 'x'; }
        if (fd->mode & 040) { text[4] = 'r'; }
        if (fd->mode & 020) { text[5] = 'w'; }
        if (fd->mode & 010) { text[6] = 'x'; }
        if (fd->mode & 04) { text[8] = 'r'; }
        if (fd->mode & 02) { text[9] = 'w'; }
        if (fd->mode & 01) { text[10] = 'x'; }
        break;
    }
    case 1:
        *(int*)FieldValue = (int)fd->uid;
        break;
    case 2:
        *(int*)FieldValue = (int)fd->gid;
        break;
    case 3: {
        char* text = (char*)FieldValue;
        if (fd->type == REGFILE) snprintf(text, maxlen, "%s", "file");
        else if (fd->type == DIRECTORY) snprintf(text, maxlen, "%s", "dir");
        else if (fd->type == LINK) snprintf(text, maxlen, "%s", "link");
        else snprintf(text, maxlen, "%s", "other");
        break;
    }
    case 4:
        ws_to_utf8buf((char*)FieldValue, maxlen, fd->alt_name);
        break;
    default:
        return ft_nosuchfield;
    }
    return fieldtypes[FieldIndex];  // very important!
}

DCEXPORT int DCPCALL FsContentGetValueW(WCHAR* FileName, int FieldIndex, int UnitIndex, void* FieldValue, int maxlen, int flags) {
    return ContentGetValue(PathConverter(u16_to_ws(FileName)), FieldIndex, FieldValue, maxlen);
}

DCEXPORT int DCPCALL FsContentGetValue(char* FileName, int FieldIndex, int UnitIndex, void* FieldValue, int maxlen, int flags) {
    return ContentGetValue(PathConverter(utf8_to_ws(FileName)), FieldIndex, FieldValue, maxlen);
}

DCEXPORT int DCPCALL FsContentGetSupportedFieldFlags(int FieldIndex) {
    if (FieldIndex == -1)
        return contflags_substmask | contflags_edit;
    else if (FieldIndex < 0 || FieldIndex >= fieldcount)
        return 0;
    else
        return fieldflags[FieldIndex];
}

DCEXPORT int DCPCALL FsContentGetDefaultSortOrder(int FieldIndex) {
    if (FieldIndex < 0 || FieldIndex >= fieldcount)
        return 1;
    else
        return sortorders[FieldIndex];
}

DCEXPORT BOOL DCPCALL FsContentGetDefaultView(char* ViewContents, char* ViewHeaders, char* ViewWidths, char* ViewOptions, int maxlen) {
    snprintf(ViewContents, maxlen, "%s", "[=tc.size]\\n[=<fs>.mode]\\n[=<fs>.uid]\\n[=<fs>.gid]\\n[=<fs>.type]\\n[=<fs>.name]");
    snprintf(ViewHeaders, maxlen, "%s", "size\\nmode\\nuid\\ngid\\ntype\\nname");
    snprintf(ViewWidths, maxlen, "%s", "148,23,-35,40,-18,-18,16,148");
    snprintf(ViewOptions, maxlen, "%s", "-1|0");
    return 1;
}

DCEXPORT int DCPCALL FsContentSetValueW(WCHAR* FileName, int FieldIndex, int UnitIndex, int FieldType, void* FieldValue, int flags) {
    return ft_fileerror;
}

DCEXPORT int DCPCALL FsContentSetValue(char* FileName, int FieldIndex, int UnitIndex, int FieldType, void* FieldValue, int flags) {
    return ft_fileerror;
}

DCEXPORT void DCPCALL FsContentPluginUnloading(void) {
    AdbCommunicator::disconnect();
}

DCEXPORT BOOL DCPCALL FsDisconnect(char* DisconnectRoot) {
    AdbCommunicator::disconnect();
    return 1;
}

DCEXPORT BOOL DCPCALL FsDisconnectW(WCHAR* DisconnectRoot) {
    AdbCommunicator::disconnect();
    return 1;
}
```

- [ ] **Step 2: Write `tests/test_dlopen.cpp`**

```cpp
// Verifies the built .wfx loads with dlopen and exposes every export from adbfsplugin.def
#include <dlfcn.h>
#include <cstdio>

static const char* kExports[] = {
    "FsInit", "FsInitW", "FsFindFirst", "FsFindFirstW", "FsFindNext", "FsFindNextW",
    "FsFindClose", "FsMkDir", "FsMkDirW", "FsExecuteFile", "FsExecuteFileW",
    "FsRenMovFile", "FsRenMovFileW", "FsGetFile", "FsGetFileW", "FsPutFile",
    "FsPutFileW", "FsDeleteFile", "FsDeleteFileW", "FsRemoveDir", "FsRemoveDirW",
    "FsSetAttr", "FsSetAttrW", "FsSetTime", "FsSetTimeW", "FsStatusInfo",
    "FsGetDefRootName", "FsExtractCustomIcon", "FsExtractCustomIconW",
    "FsSetDefaultParams", "FsGetPreviewBitmap", "FsGetPreviewBitmapW",
    "FsContentGetSupportedField", "FsContentGetValue", "FsContentGetValueW",
    "FsContentGetSupportedFieldFlags", "FsContentGetDefaultSortOrder",
    "FsContentGetDefaultView", "FsContentSetValue", "FsContentSetValueW",
    "FsContentPluginUnloading", "FsDisconnect", "FsDisconnectW",
};

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "build/adbfsplugin.wfx";
    void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        std::fprintf(stderr, "dlopen(%s) failed: %s\n", path, dlerror());
        return 1;
    }
    int missing = 0;
    for (const char* name : kExports) {
        if (!dlsym(h, name)) {
            std::fprintf(stderr, "missing export: %s\n", name);
            missing++;
        }
    }
    if (missing) {
        std::fprintf(stderr, "%d export(s) missing from %s\n", missing, path);
        return 1;
    }
    std::printf("OK: %zu exports resolved in %s\n", sizeof(kExports) / sizeof(kExports[0]), path);
    return 0;
}
```

(43 names: the `.def` list of 44 minus none — `FsStatusInfo` counts once; the array above is the def file verbatim.)

- [ ] **Step 3: Update `Makefile`** — add:

```make
PLUGIN_SRCS := adbfsplugin.cpp adbhandler.cpp wfxcompat.cpp
HDRS := platform.h wfxcompat.h adbfsplugin.h adbhandler.h sdk/common.h sdk/wfxplugin.h

all: $(BUILD)/adbfsplugin.wfx

$(BUILD)/adbfsplugin.wfx: $(PLUGIN_SRCS) $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -dynamiclib -o $@ $(PLUGIN_SRCS)

$(BUILD)/dlopen_test: tests/test_dlopen.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ tests/test_dlopen.cpp
```

and extend `test:` to run `$(BUILD)/dlopen_test $(BUILD)/adbfsplugin.wfx` after unit tests.

- [ ] **Step 4: Run `make test` — expect unit tests PASS, dlopen test PASS (all exports resolved)**
- [ ] **Step 5: Commit** (`"Port WFX API surface to Double Commander ABI; add dlopen export test"`)

---

### Task 5: Fake ADB server + end-to-end integration test

**Files:**
- Create: `tests/fake_adb_server.h`, `tests/fake_adb_server.cpp`, `tests/test_integration.cpp`
- Modify: `Makefile`

**Interfaces:**
- Consumes: exported `Fs*` functions (linked directly from `adbfsplugin.cpp`), `AdbCommunicator` env knobs (`ANDROID_ADB_SERVER_PORT`, `ADBFS_ADB`, `ADBFS_NO_SU`).
- Produces: `FakeAdbServer` class — `port()`, `commands()` (captured shell commands), `uploaded()` (raw lines captured after a `uudecode` command).

Fake device state (canned): root dir contains `file one` (regular, mode 644, gid 1000, uid 2000, size 12, mtime 1700000001, content "hello adbfs!"), `subdir` (directory 755), `link1` (symlink 777 → `/target`), `😀.txt` (regular 644). uuencode of any path returns base64 of "hello adbfs!" (`aGVsbG8gYWRiZnMh`).

- [ ] **Step 1: Write `tests/fake_adb_server.h`**

```cpp
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Minimal in-process stand-in for the local ADB server plus a device shell.
// Speaks just enough of the smart-socket protocol for adbfsplugin:
//   <4-hex-len><payload> requests, OKAY responses, then a line-based shell.
class FakeAdbServer {
public:
    FakeAdbServer();
    ~FakeAdbServer();
    int port() const { return port_; }
    std::vector<std::string> commands();   // device shell commands, marker framing stripped
    std::string uploaded();                // raw lines captured after a uudecode command

private:
    void run();
    void serveConnection(int fd);
    void shellSession(int fd);
    void handleShellCommand(int fd, const std::string& cmd);

    int listen_fd_ = -1;
    int port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::vector<std::string> commands_;
    std::string uploaded_;
    std::string rbuf_;                     // connection read buffer
    bool readLine(int fd, std::string* line);
    static void sendAll(int fd, const std::string& data);
};
```

- [ ] **Step 2: Write `tests/fake_adb_server.cpp`**

```cpp
#include "fake_adb_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>

namespace {

std::string base64(const std::string& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i + 3 <= in.size()) {
        unsigned n = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        out += tbl[n >> 18]; out += tbl[(n >> 12) & 63]; out += tbl[(n >> 6) & 63]; out += tbl[n & 63];
        i += 3;
    }
    size_t rem = in.size() - i;
    if (rem == 1) {
        unsigned n = (unsigned char)in[i] << 16;
        out += tbl[n >> 18]; out += tbl[(n >> 12) & 63]; out += "==";
    } else if (rem == 2) {
        unsigned n = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        out += tbl[n >> 18]; out += tbl[(n >> 12) & 63]; out += tbl[(n >> 6) & 63]; out += '=';
    }
    return out;
}

// paths appear as '...'-quoted arguments; names in the tests contain no quotes
std::vector<std::string> quotedArgs(const std::string& cmd) {
    std::vector<std::string> out;
    size_t pos = 0;
    for (;;) {
        size_t a = cmd.find('\'', pos);
        if (a == std::string::npos) break;
        size_t b = cmd.find('\'', a + 1);
        if (b == std::string::npos) break;
        out.push_back(cmd.substr(a + 1, b - a - 1));
        pos = b + 1;
    }
    return out;
}

std::string basenameOf(const std::string& path) {
    size_t sl = path.find_last_of('/');
    return sl == std::string::npos ? path : path.substr(sl + 1);
}

const char kFileContent[] = "hello adbfs!";

} // namespace

FakeAdbServer::FakeAdbServer() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(listen_fd_, (sockaddr*)&addr, sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(listen_fd_, (sockaddr*)&addr, &len);
    port_ = ntohs(addr.sin_port);
    listen(listen_fd_, 4);
    thread_ = std::thread([this] { run(); });
}

FakeAdbServer::~FakeAdbServer() {
    stop_ = true;
    if (listen_fd_ >= 0) { shutdown(listen_fd_, SHUT_RDWR); close(listen_fd_); }
    if (thread_.joinable()) thread_.join();
}

std::vector<std::string> FakeAdbServer::commands() {
    std::lock_guard<std::mutex> lk(mu_);
    return commands_;
}

std::string FakeAdbServer::uploaded() {
    std::lock_guard<std::mutex> lk(mu_);
    return uploaded_;
}

void FakeAdbServer::run() {
    while (!stop_) {
        int fd = accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) return;
        rbuf_.clear();
        serveConnection(fd);
        close(fd);
    }
}

void FakeAdbServer::sendAll(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = send(fd, data.data() + off, data.size() - off, 0);
        if (n <= 0) return;
        off += (size_t)n;
    }
}

// smart-socket phase: <4 hex chars length><payload>
void FakeAdbServer::serveConnection(int fd) {
    for (;;) {
        char lenbuf[5] = {0};
        ssize_t n = recv(fd, lenbuf, 4, MSG_WAITALL);
        if (n != 4) return;
        int msglen = (int)strtol(lenbuf, nullptr, 16);
        std::string payload(msglen, 0);
        if (recv(fd, &payload[0], msglen, MSG_WAITALL) != msglen) return;
        if (payload == "host:transport-usb") {
            sendAll(fd, "OKAY");
        } else if (payload == "shell:") {
            sendAll(fd, "OKAY");
            shellSession(fd);
            return;
        } else {
            sendAll(fd, "FAIL0013unknown fake request");
            return;
        }
    }
}

bool FakeAdbServer::readLine(int fd, std::string* line) {
    for (;;) {
        size_t nl = rbuf_.find('\n');
        if (nl != std::string::npos) {
            *line = rbuf_.substr(0, nl);
            rbuf_.erase(0, nl + 1);
            if (!line->empty() && line->back() == '\r') line->pop_back();
            return true;
        }
        char buf[4096];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        rbuf_.append(buf, (size_t)n);
    }
}

void FakeAdbServer::shellSession(int fd) {
    std::string line;
    while (readLine(fd, &line)) {
        if (line == "su") {
            sendAll(fd, "# ");
            continue;
        }
        const std::string prefix = "echo \"===adbfsplugin<--\" ;";
        const std::string suffix = " ; echo \"===adbfsplugin-->\"";
        if (line.compare(0, prefix.size(), prefix) != 0) continue;
        std::string cmd = line.substr(prefix.size());
        size_t tail = cmd.rfind(suffix);
        if (tail != std::string::npos) cmd.erase(tail);
        while (!cmd.empty() && cmd.front() == ' ') cmd.erase(0, 1);
        {
            std::lock_guard<std::mutex> lk(mu_);
            commands_.push_back(cmd);
        }
        sendAll(fd, line + "\r\n");             // pty echo, discarded by the plugin
        sendAll(fd, "===adbfsplugin<--\n");
        handleShellCommand(fd, cmd);
        sendAll(fd, "===adbfsplugin-->\n");
    }
}

void FakeAdbServer::handleShellCommand(int fd, const std::string& cmd) {
    if (cmd.rfind("busybox ls ", 0) == 0) {
        sendAll(fd, "file one\nsubdir\nlink1\n\xF0\x9F\x98\x80.txt\n");
    } else if (cmd.rfind("busybox stat ", 0) == 0) {
        auto args = quotedArgs(cmd);
        std::string out;
        for (auto& a : args) {
            std::string base = basenameOf(a);
            if (base.empty()) base = "/";
            if (base == "subdir")
                out += "755 -directory- 0 0 4096 1600000000 1600000100 1600000200 '" + a + "'\n";
            else if (base == "link1")
                out += "777 -symbolic link- 0 0 11 1600000000 1600000100 1600000200 '" + a + "' -> '/target'\n";
            else
                out += "644 -regular file- 1000 2000 12 1700000000 1700000001 1700000002 '" + a + "'\n";
        }
        sendAll(fd, out);
    } else if (cmd.rfind("busybox uuencode ", 0) == 0) {
        sendAll(fd, "begin-base64 644 x\n" + base64(kFileContent) + "\n====\n");
    } else if (cmd.rfind("busybox uudecode ", 0) == 0) {
        // consume the in-band upload until the plugin's EOT marker
        std::string data, l;
        while (readLine(fd, &l)) {
            if (l == "====\x04") break;
            data += l;
            data += '\n';
        }
        std::lock_guard<std::mutex> lk(mu_);
        uploaded_ = data;
    }
    // mkdir/rm/mv/cp: recorded in commands_, empty output
}
```

- [ ] **Step 3: Write `tests/test_integration.cpp`**

```cpp
// End-to-end: real exported plugin functions against the fake ADB server.
#include "harness.h"
#include "fake_adb_server.h"
#include "../adbfsplugin.h"
#include "../adbhandler.h"
#include "../wfxcompat.h"

#include <set>
#include <unistd.h>

static int progressCalls = 0;
static int progW(int, WCHAR*, WCHAR*, int) { progressCalls++; return 0; }
static void logW(int, int, WCHAR*) {}
static BOOL reqW(int, int, WCHAR*, WCHAR*, WCHAR*, int) { return 0; }

static std::vector<WCHAR> W(const std::wstring& s) {
    std::vector<WCHAR> v(s.size() * 2 + 1);
    ws_to_u16buf(v.data(), v.size(), s);
    return v;
}

TEST(end_to_end_against_fake_adb) {
    FakeAdbServer server;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", server.port());
    setenv("ANDROID_ADB_SERVER_PORT", portstr, 1);
    setenv("ADBFS_ADB", "/usr/bin/true", 1);
    setenv("ADBFS_NO_SU", "1", 1);

    CHECK_EQ(FsInitW(7, progW, logW, reqW), 0);

    // --- directory listing ---
    WIN32_FIND_DATAW fd;
    auto root = W(L"\\");
    HANDLE h = FsFindFirstW(root.data(), &fd);
    CHECK(h != INVALID_HANDLE_VALUE);
    std::set<std::wstring> names;
    std::map<std::wstring, WIN32_FIND_DATAW> entries;
    names.insert(u16_to_ws(fd.cFileName));
    entries[u16_to_ws(fd.cFileName)] = fd;
    while (FsFindNextW(h, &fd)) {
        names.insert(u16_to_ws(fd.cFileName));
        entries[u16_to_ws(fd.cFileName)] = fd;
    }
    FsFindClose(h);

    std::wstring emoji = L"";
    emoji.push_back((wchar_t)0x1F600);
    emoji += L".txt";
    CHECK_EQ(names.size(), (size_t)4);
    CHECK(names.count(L"file one") == 1);
    CHECK(names.count(L"subdir") == 1);
    CHECK(names.count(L"link1") == 1);
    CHECK(names.count(emoji) == 1);

    CHECK(entries[L"subdir"].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    CHECK(entries[L"file one"].dwFileAttributes & 0x80000000u);
    CHECK(!(entries[L"file one"].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
    CHECK_EQ(entries[L"file one"].dwReserved0, (DWORD)0644);
    CHECK_EQ(entries[L"file one"].nFileSizeLow, (DWORD)12);
    int64_t ft = ((int64_t)entries[L"file one"].ftLastWriteTime.dwHighDateTime << 32) |
                 entries[L"file one"].ftLastWriteTime.dwLowDateTime;
    CHECK_EQ(ft, unixTimeToFileTime(1700000001u));

    // --- content plugin reads from the listing cache ---
    auto fpath = W(L"\\file one");
    int uid = 0, gid = 0;
    CHECK_EQ(FsContentGetValueW(fpath.data(), 1, 0, &uid, sizeof(uid), 0), ft_numeric_32);
    CHECK_EQ(uid, 2000);
    CHECK_EQ(FsContentGetValueW(fpath.data(), 2, 0, &gid, sizeof(gid), 0), ft_numeric_32);
    CHECK_EQ(gid, 1000);
    char text[64];
    CHECK_EQ(FsContentGetValueW(fpath.data(), 3, 0, text, sizeof(text), 0), ft_string);
    CHECK(std::string(text) == "file");
    CHECK_EQ(FsContentGetValueW(fpath.data(), 0, 0, text, sizeof(text), 0), ft_string);
    CHECK(std::string(text) == "rw- r-- r--");

    // --- mkdir goes through the shell ---
    auto ndir = W(L"\\newdir");
    CHECK(FsMkDirW(ndir.data()));
    {
        auto cmds = server.commands();
        bool found = false;
        for (auto& c : cmds) found = found || c == "busybox mkdir '/newdir'";
        CHECK(found);
    }

    // --- download ---
    char tmpl[] = "/tmp/adbfs_test_XXXXXX";
    int tmpfd = mkstemp(tmpl);
    CHECK(tmpfd >= 0);
    close(tmpfd);
    unlink(tmpl);                     // plugin expects to create it
    RemoteInfoStruct ri;
    memset(&ri, 0, sizeof(ri));
    ri.SizeLow = 12;
    auto lpath = W(utf8_to_ws(tmpl));
    progressCalls = 0;
    CHECK_EQ(FsGetFileW(fpath.data(), lpath.data(), 0, &ri), FS_FILE_OK);
    CHECK(progressCalls >= 2);
    {
        FILE* f = fopen(tmpl, "rb");
        CHECK(f != NULL);
        char buf[64] = {0};
        size_t n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        CHECK_EQ(n, (size_t)12);
        CHECK(std::string(buf, n) == "hello adbfs!");
        unlink(tmpl);
    }

    // --- upload ---
    char tmpl2[] = "/tmp/adbfs_up_XXXXXX";
    int upfd = mkstemp(tmpl2);
    CHECK(upfd >= 0);
    CHECK_EQ((int)write(upfd, "hello adbfs!", 12), 12);
    close(upfd);
    auto uppath = W(utf8_to_ws(tmpl2));
    auto rpath = W(L"\\up.txt");
    CHECK_EQ(FsPutFileW(uppath.data(), rpath.data(), 0), FS_FILE_OK);
    unlink(tmpl2);
    for (int i = 0; i < 100 && server.uploaded().empty(); i++) usleep(20000);
    {
        std::string up = server.uploaded();
        CHECK(up.find("begin-base64 644 x\n") == 0);
        CHECK(up.find("aGVsbG8gYWRiZnMh\n") != std::string::npos);
        auto cmds = server.commands();
        bool found = false;
        for (auto& c : cmds) found = found || c == "busybox uudecode -o '/up.txt'";
        CHECK(found);
    }

    FsDisconnectW(rpath.data());
}

int main() {
    alarm(30);   // hard stop if the protocol deadlocks
    return run_all();
}
```

- [ ] **Step 4: Update `Makefile`** — add:

```make
INTEG_SRCS := tests/test_integration.cpp tests/fake_adb_server.cpp $(PLUGIN_SRCS)

$(BUILD)/integration_tests: $(INTEG_SRCS) $(HDRS) tests/harness.h tests/fake_adb_server.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $(INTEG_SRCS)
```

and run it from `test:` between unit tests and the dlopen test.

- [ ] **Step 5: Run `make test` — expect FAIL first (mode column string), fix expectations vs implementation, then all PASS.** Note: the mode string for 0644 is `rw- r-- r--` (11 chars, upstream format with spaces).
- [ ] **Step 6: Commit** (`"Add fake ADB server and end-to-end integration test"`)

---

### Task 6: README, universal binary, release zip

**Files:**
- Create: `README.md`
- Modify: `Makefile` (universal + dist targets)

**Interfaces:**
- Consumes: `build/adbfsplugin-universal.wfx`.
- Produces: `dist/adbfsplugin-1.1.0-macos.zip` with `adbfsplugin.wfx`, `pluginst.inf`, `README.md`, `LICENCE` at the zip root.

- [ ] **Step 1: Write `README.md`** — sections: what it is (macOS port of sztupy's adbfsplugin for Double Commander), requirements (Double Commander for macOS, `adb` installed e.g. `brew install android-platform-tools`, device with USB debugging + busybox), install (open the zip in Double Commander → confirm the install prompt; or Configuration → Options → Plugins → File System Plugins → Add `adbfsplugin.wfx`), usage (new "Android" entry in the file system plugins list / `wfx://` panel), environment knobs (`ADBFS_ADB`, `ANDROID_ADB_SERVER_PORT`, `ADBFS_NO_SU`), building (`make`, `make test`, `make dist`), history/license (AGPL-3.0 + linking exception, original author Zsolt Sz. Sztupák; Windows sources remain in the tree but are unmaintained).

- [ ] **Step 2: Add Makefile targets**

```make
$(BUILD)/adbfsplugin-universal.wfx: $(PLUGIN_SRCS) $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -arch arm64 -arch x86_64 -dynamiclib -o $@ $(PLUGIN_SRCS)

universal: $(BUILD)/adbfsplugin-universal.wfx

dist: test $(BUILD)/adbfsplugin-universal.wfx $(BUILD)/dlopen_test
	$(BUILD)/dlopen_test $(BUILD)/adbfsplugin-universal.wfx
	rm -rf $(BUILD)/dist-stage dist
	mkdir -p $(BUILD)/dist-stage dist
	cp $(BUILD)/adbfsplugin-universal.wfx $(BUILD)/dist-stage/adbfsplugin.wfx
	cp pluginst.inf README.md LICENCE $(BUILD)/dist-stage/
	cd $(BUILD)/dist-stage && zip -r ../../dist/adbfsplugin-$(VERSION)-macos.zip .
	@echo "Release: dist/adbfsplugin-$(VERSION)-macos.zip"
```

- [ ] **Step 3: Run `make dist` — expect: full test suite passes, universal dylib passes dlopen test, zip created.**
- [ ] **Step 4: Verify the artifact:** `unzip -l dist/adbfsplugin-1.1.0-macos.zip` shows the four files at root; `file` + `lipo -archs` on the staged `.wfx` shows `x86_64 arm64`.
- [ ] **Step 5: Commit** (`"Add README, universal build, and release packaging"`)

---

## Final verification checklist

- [ ] `make clean && make test` — everything green from scratch
- [ ] `make dist` — release zip built and export-checked
- [ ] `git log --oneline` — one commit per task on `macos-port`
- [ ] Superpowers verification-before-completion skill before claiming done
