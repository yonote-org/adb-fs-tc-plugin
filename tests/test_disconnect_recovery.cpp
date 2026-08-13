// Regression test: device vanishing mid-session (hang report 2026-08-13,
// wireless device left the network). The adb server closes the shell stream;
// CleanBuffer's drain loop must notice EOF instead of spinning select/recv
// forever on Double Commander's UI thread, and the next listing must
// reconnect and succeed.
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

TEST(listing_recovers_after_connection_drop) {
    FakeAdbServer server;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", server.port());
    setenv("ANDROID_ADB_SERVER_PORT", portstr, 1);
    setenv("ADBFS_ADB", "/usr/bin/true", 1);
    setenv("ADBFS_NO_SU", "1", 1);
    unsetenv("ADBFS_SERIAL");

    CHECK_EQ(FsInitW(1, progW, logW, reqW), 0);
    CHECK_EQ(listRoot(), 4);

    server.dropConnection();
    usleep(100 * 1000);   // let the FIN reach the plugin's socket

    CHECK_EQ(listRoot(), 4);   // must reconnect promptly, not hang
    CHECK(server.transports().size() >= 2);   // proves a fresh connection was made

    auto root = W(L"\\");
    FsDisconnectW(root.data());
}

int main() {
    alarm(20);   // watchdog: the pre-fix behavior spins forever in CleanBuffer
    return run_all();
}
