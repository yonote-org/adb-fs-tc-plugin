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
        if (f) {
            char buf[64] = {0};
            size_t n = fread(buf, 1, sizeof(buf), f);
            fclose(f);
            CHECK_EQ(n, (size_t)12);
            CHECK(std::string(buf, n) == "hello adbfs!");
            unlink(tmpl);
        }
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

TEST(execute_open_delegates_download_to_commander) {
    // "open" must answer FS_EXEC_YOURSELF: Double Commander then downloads
    // the file to its temp dir, opens it with the default app, and cleans
    // the copy up itself — no device round-trip happens in this call.
    auto path = W(L"\\subdir\\file one");
    auto open = W(L"open");
    CHECK_EQ(FsExecuteFileW(NULL, path.data(), open.data()), FS_EXEC_YOURSELF);

    auto props = W(L"properties");
    CHECK_EQ(FsExecuteFileW(NULL, path.data(), props.data()), FS_EXEC_ERROR);

    char apath[] = "/subdir/file one";
    char aopen[] = "open";
    CHECK_EQ(FsExecuteFile(NULL, apath, aopen), FS_EXEC_YOURSELF);
}

int main() {
    alarm(30);   // hard stop if the protocol deadlocks
    return run_all();
}
