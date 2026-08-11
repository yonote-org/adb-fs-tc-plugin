// Stock Android device (no busybox, toybox applets only): the plugin must
// detect the available toolbox and fall back for listing, file management,
// and transfers (base64 instead of uuencode/uudecode).
#include "harness.h"
#include "fake_adb_server.h"
#include "../adbfsplugin.h"
#include "../adbhandler.h"
#include "../wfxcompat.h"

#include <set>
#include <unistd.h>

static int progW(int, WCHAR*, WCHAR*, int) { return 0; }
static void logW(int, int, WCHAR*) {}
static BOOL reqW(int, int, WCHAR*, WCHAR*, WCHAR*, int) { return 0; }

static std::vector<WCHAR> W(const std::wstring& s) {
    std::vector<WCHAR> v(s.size() * 2 + 1);
    ws_to_u16buf(v.data(), v.size(), s);
    return v;
}

static bool hasCommand(FakeAdbServer& server, const std::string& want) {
    for (auto& c : server.commands())
        if (c == want) return true;
    return false;
}

TEST(stock_device_listing_and_transfers) {
    FakeAdbServer server(/*stock=*/true);
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", server.port());
    setenv("ANDROID_ADB_SERVER_PORT", portstr, 1);
    setenv("ADBFS_ADB", "/usr/bin/true", 1);
    setenv("ADBFS_NO_SU", "1", 1);   // su timing is covered by test_su_hang

    CHECK_EQ(FsInitW(3, progW, logW, reqW), 0);

    // --- listing falls back to toybox ---
    WIN32_FIND_DATAW fd;
    auto root = W(L"\\");
    HANDLE h = FsFindFirstW(root.data(), &fd);
    CHECK(h != INVALID_HANDLE_VALUE);
    std::set<std::wstring> names;
    names.insert(u16_to_ws(fd.cFileName));
    std::map<std::wstring, WIN32_FIND_DATAW> entries;
    entries[u16_to_ws(fd.cFileName)] = fd;
    while (FsFindNextW(h, &fd)) {
        names.insert(u16_to_ws(fd.cFileName));
        entries[u16_to_ws(fd.cFileName)] = fd;
    }
    FsFindClose(h);

    CHECK_EQ(names.size(), (size_t)4);
    CHECK(names.count(L"file one") == 1);
    CHECK(names.count(L"subdir") == 1);
    CHECK(entries[L"subdir"].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    CHECK_EQ(entries[L"file one"].dwReserved0, (DWORD)0644);
    CHECK_EQ(entries[L"file one"].nFileSizeLow, (DWORD)12);

    // a symlink to a directory (like /sdcard) must be enterable: directory
    // attribute set, marked as a link
    CHECK(entries[L"link1"].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    CHECK(entries[L"link1"].dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    {
        auto lpath = W(L"\\link1");
        char text[64] = {0};
        CHECK_EQ(FsContentGetValueW(lpath.data(), 3, 0, text, sizeof(text), 0), ft_string);
        CHECK(std::string(text) == "link");
    }

    // probes ran in order
    CHECK(hasCommand(server, "busybox echo adbfsprobe"));
    CHECK(hasCommand(server, "toybox echo adbfsprobe"));

    // --- content plugin still fed from the cache ---
    auto fpath = W(L"\\file one");
    int uid = 0;
    CHECK_EQ(FsContentGetValueW(fpath.data(), 1, 0, &uid, sizeof(uid), 0), ft_numeric_32);
    CHECK_EQ(uid, 2000);

    // --- mkdir via toybox ---
    auto ndir = W(L"\\newdir");
    CHECK(FsMkDirW(ndir.data()));
    CHECK(hasCommand(server, "toybox mkdir '/newdir'"));

    // --- download via base64 ---
    char tmpl[] = "/tmp/adbfs_stock_XXXXXX";
    int tmpfd = mkstemp(tmpl);
    CHECK(tmpfd >= 0);
    close(tmpfd);
    unlink(tmpl);
    RemoteInfoStruct ri;
    memset(&ri, 0, sizeof(ri));
    ri.SizeLow = 12;
    auto lpath = W(utf8_to_ws(tmpl));
    CHECK_EQ(FsGetFileW(fpath.data(), lpath.data(), 0, &ri), FS_FILE_OK);
    {
        FILE* f = fopen(tmpl, "rb");
        CHECK(f != NULL);
        if (f) {
            char buf[64] = {0};
            size_t n = fread(buf, 1, sizeof(buf), f);
            fclose(f);
            CHECK(std::string(buf, n) == "hello adbfs!");
            unlink(tmpl);
        }
    }

    // --- upload via base64 -d (no uuencode framing) ---
    char tmpl2[] = "/tmp/adbfs_stock_up_XXXXXX";
    int upfd = mkstemp(tmpl2);
    CHECK(upfd >= 0);
    CHECK_EQ((int)write(upfd, "hello adbfs!", 12), 12);
    close(upfd);
    auto uppath = W(utf8_to_ws(tmpl2));
    auto rpath = W(L"\\up.txt");
    CHECK_EQ(FsPutFileW(uppath.data(), rpath.data(), 0), FS_FILE_OK);
    unlink(tmpl2);
    for (int i = 0; i < 100 && server.uploaded().empty(); i++) usleep(20000);
    CHECK(server.uploaded() == "aGVsbG8gYWRiZnMh\n");
    CHECK(hasCommand(server, "toybox base64 -d > '/up.txt'"));

    FsDisconnectW(rpath.data());
}

int main() {
    alarm(30);
    return run_all();
}
