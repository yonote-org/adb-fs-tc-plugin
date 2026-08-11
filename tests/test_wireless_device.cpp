// Regression test: devices connected over wireless ADB (TCP transport).
// The USB-only selector host:transport-usb gets "FAIL no devices found" from
// the server for such devices; the plugin must select the device with
// host:transport-any instead, and honor ADBFS_SERIAL for explicit selection.
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

static bool contains(const std::vector<std::string>& v, const std::string& s) {
    for (auto& e : v)
        if (e == s) return true;
    return false;
}

static int listRoot() {
    WIN32_FIND_DATAW fd;
    auto root = W(L"\\");
    HANDLE h = FsFindFirstW(root.data(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int count = 1;
    while (FsFindNextW(h, &fd)) count++;
    FsFindClose(h);
    FsDisconnectW(root.data());
    return count;
}

TEST(wireless_device_listing_works) {
    FakeAdbServer server(false, true);
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", server.port());
    setenv("ANDROID_ADB_SERVER_PORT", portstr, 1);
    setenv("ADBFS_ADB", "/usr/bin/true", 1);
    setenv("ADBFS_NO_SU", "1", 1);
    unsetenv("ADBFS_SERIAL");

    CHECK_EQ(FsInitW(1, progW, logW, reqW), 0);
    CHECK_EQ(listRoot(), 4);
    CHECK(contains(server.transports(), "host:transport-any"));
}

TEST(adbfs_serial_selects_specific_device) {
    FakeAdbServer server(false, true);
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", server.port());
    setenv("ANDROID_ADB_SERVER_PORT", portstr, 1);
    setenv("ADBFS_ADB", "/usr/bin/true", 1);
    setenv("ADBFS_NO_SU", "1", 1);
    setenv("ADBFS_SERIAL", "172.17.10.12:37761", 1);

    CHECK_EQ(FsInitW(1, progW, logW, reqW), 0);
    CHECK_EQ(listRoot(), 4);
    CHECK(contains(server.transports(), "host:transport:172.17.10.12:37761"));
    unsetenv("ADBFS_SERIAL");
}

int main() {
    alarm(20);   // watchdog: a hang kills the test instead of blocking make
    return run_all();
}
