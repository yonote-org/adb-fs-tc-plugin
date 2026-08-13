// Regression test: a device that goes silent while the TCP link stays up
// (Wi-Fi died without the adb server noticing). Reads must give up after the
// ADBFS_READ_TIMEOUT inactivity window instead of blocking recv() forever on
// Double Commander's UI thread.
#include "harness.h"
#include "fake_adb_server.h"
#include "../adbfsplugin.h"
#include "../adbhandler.h"
#include "../wfxcompat.h"

#include <unistd.h>

static int progW(int, WCHAR*, WCHAR*, int) { return 0; }
static void logW(int, int, WCHAR*) {}
static BOOL reqW(int, int, WCHAR*, WCHAR*, WCHAR*, int) { return 0; }

static std::vector<WCHAR> W(const std::wstring& s) {
    std::vector<WCHAR> v(s.size() * 2 + 1);
    ws_to_u16buf(v.data(), v.size(), s);
    return v;
}

static int listRoot() {
    WIN32_FIND_DATAW fd;
    auto root = W(L"\\");
    HANDLE h = FsFindFirstW(root.data(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int count = 1;
    while (FsFindNextW(h, &fd)) count++;
    FsFindClose(h);
    return count;
}

TEST(silent_device_times_out_instead_of_hanging) {
    FakeAdbServer server;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", server.port());
    setenv("ANDROID_ADB_SERVER_PORT", portstr, 1);
    setenv("ADBFS_ADB", "/usr/bin/true", 1);
    setenv("ADBFS_NO_SU", "1", 1);
    setenv("ADBFS_READ_TIMEOUT", "1", 1);
    unsetenv("ADBFS_SERIAL");

    CHECK_EQ(FsInitW(1, progW, logW, reqW), 0);
    CHECK_EQ(listRoot(), 4);

    server.goSilent();

    CHECK_EQ(listRoot(), 1);   // one <...> error pseudo-entry, within ~1s

    unsetenv("ADBFS_READ_TIMEOUT");
    auto root = W(L"\\");
    FsDisconnectW(root.data());
}

int main() {
    alarm(20);   // watchdog: the pre-fix behavior blocks in recv forever
    return run_all();
}
