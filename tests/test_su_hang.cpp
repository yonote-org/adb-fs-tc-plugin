// Regression test: the su handshake must not deadlock when the device answers
// faster than the 50ms echo-drain window (crash report: FsFindFirstW ->
// PushCommandW -> __select blocked forever in CleanBuffer(true)).
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

TEST(su_handshake_does_not_deadlock) {
    FakeAdbServer server;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", server.port());
    setenv("ANDROID_ADB_SERVER_PORT", portstr, 1);
    setenv("ADBFS_ADB", "/usr/bin/true", 1);
    unsetenv("ADBFS_NO_SU");        // the su dance must run

    CHECK_EQ(FsInitW(1, progW, logW, reqW), 0);

    WIN32_FIND_DATAW fd;
    auto root = W(L"\\");
    HANDLE h = FsFindFirstW(root.data(), &fd);
    CHECK(h != INVALID_HANDLE_VALUE);
    int count = 1;
    while (FsFindNextW(h, &fd)) count++;
    FsFindClose(h);
    CHECK_EQ(count, 4);

    FsDisconnectW(root.data());
}

int main() {
    alarm(20);   // watchdog: a deadlock kills the test instead of hanging make
    return run_all();
}
