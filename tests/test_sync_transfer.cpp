// Transfer modes: default (sync protocol, what adb pull/push speaks) versus
// the configurable base64-through-shell fallback. Real exported plugin
// functions against the fake ADB server.
#include "harness.h"
#include "fake_adb_server.h"
#include "../adbfsplugin.h"
#include "../adbhandler.h"
#include "../wfxcompat.h"

#include <sys/stat.h>
#include <unistd.h>

static int progressCalls = 0;
static int progressMax = 0;
static int progW(int, WCHAR*, WCHAR*, int percent) {
    progressCalls++;
    if (percent > progressMax) progressMax = percent;
    return 0;
}
static void logW(int, int, WCHAR*) {}
static BOOL reqW(int, int, WCHAR*, WCHAR*, WCHAR*, int) { return 0; }

static std::vector<WCHAR> W(const std::wstring& s) {
    std::vector<WCHAR> v(s.size() * 2 + 1);
    ws_to_u16buf(v.data(), v.size(), s);
    return v;
}

static void useServer(const FakeAdbServer& server) {
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", server.port());
    setenv("ANDROID_ADB_SERVER_PORT", portstr, 1);
    setenv("ADBFS_ADB", "/usr/bin/true", 1);
    setenv("ADBFS_NO_SU", "1", 1);
    unsetenv("ADBFS_TRANSFER_MODE");
    SetConfigIniPath("/nonexistent/wfx.ini");   // default settings
    FsInitW(7, progW, logW, reqW);
}

static std::string readAll(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return "";
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

static bool anyContains(const std::vector<std::string>& v, const std::string& needle) {
    for (auto& s : v)
        if (s.find(needle) != std::string::npos) return true;
    return false;
}

TEST(default_download_uses_sync_protocol) {
    FakeAdbServer server;
    useServer(server);

    // list first: the shell connection stays open while sync runs on its own
    WIN32_FIND_DATAW fd;
    auto root = W(L"\\");
    HANDLE h = FsFindFirstW(root.data(), &fd);
    CHECK(h != INVALID_HANDLE_VALUE);
    while (FsFindNextW(h, &fd)) {}
    FsFindClose(h);

    char tmpl[] = "/tmp/adbfs_sync_dl_XXXXXX";
    int tmpfd = mkstemp(tmpl);
    close(tmpfd);
    unlink(tmpl);
    RemoteInfoStruct ri;
    memset(&ri, 0, sizeof(ri));
    ri.SizeLow = 12;
    auto rpath = W(L"\\file one");
    auto lpath = W(utf8_to_ws(tmpl));
    progressCalls = 0;
    CHECK_EQ(FsGetFileW(rpath.data(), lpath.data(), 0, &ri), FS_FILE_OK);
    CHECK(progressCalls >= 2);
    CHECK(readAll(tmpl) == "hello adbfs!");
    unlink(tmpl);

    CHECK(anyContains(server.syncRequests(), "RECV /file one"));
    CHECK(!anyContains(server.commands(), "uuencode"));
    CHECK(!anyContains(server.commands(), "base64"));
    FsDisconnectW(root.data());
}

TEST(default_upload_uses_sync_protocol) {
    FakeAdbServer server;
    useServer(server);

    char tmpl[] = "/tmp/adbfs_sync_up_XXXXXX";
    int upfd = mkstemp(tmpl);
    CHECK_EQ((int)write(upfd, "hello adbfs!", 12), 12);
    close(upfd);
    auto lpath = W(utf8_to_ws(tmpl));
    auto rpath = W(L"\\up.txt");
    progressCalls = 0;
    CHECK_EQ(FsPutFileW(lpath.data(), rpath.data(), 0), FS_FILE_OK);
    CHECK(progressCalls >= 2);
    unlink(tmpl);

    CHECK_EQ(server.syncUploaded(), std::string("hello adbfs!"));
    CHECK(anyContains(server.syncRequests(), "SEND /up.txt,"));
    CHECK(!anyContains(server.commands(), "uudecode"));
    CHECK(!anyContains(server.commands(), "base64"));
    auto root = W(L"\\");
    FsDisconnectW(root.data());
}

TEST(download_progress_never_exceeds_100_percent) {
    // a stale listing (remote file grew since) must not push >100 to the
    // commander's progress callback
    FakeAdbServer server;
    useServer(server);

    char tmpl[] = "/tmp/adbfs_sync_dl2_XXXXXX";
    int tmpfd = mkstemp(tmpl);
    close(tmpfd);
    unlink(tmpl);
    RemoteInfoStruct ri;
    memset(&ri, 0, sizeof(ri));
    ri.SizeLow = 4;   // fake device actually serves 12 bytes
    auto rpath = W(L"\\file one");
    auto lpath = W(utf8_to_ws(tmpl));
    progressMax = 0;
    CHECK_EQ(FsGetFileW(rpath.data(), lpath.data(), 0, &ri), FS_FILE_OK);
    CHECK(progressMax <= 100);
    unlink(tmpl);
    auto root = W(L"\\");
    FsDisconnectW(root.data());
}

TEST(sync_pull_failure_reports_error_and_removes_partial_file) {
    FakeAdbServer server;
    useServer(server);

    const char* local = "/tmp/adbfs_sync_denied.bin";
    unlink(local);
    RemoteInfoStruct ri;
    memset(&ri, 0, sizeof(ri));
    ri.SizeLow = 12;
    auto rpath = W(L"\\denied file");
    auto lpath = W(utf8_to_ws(local));
    CHECK_EQ(FsGetFileW(rpath.data(), lpath.data(), 0, &ri), FS_FILE_READERROR);
    struct stat st;
    CHECK(stat(local, &st) != 0);   // no partial download left behind
    auto root = W(L"\\");
    FsDisconnectW(root.data());
}

TEST(sync_push_failure_reports_error) {
    FakeAdbServer server;
    useServer(server);

    char tmpl[] = "/tmp/adbfs_sync_up2_XXXXXX";
    int upfd = mkstemp(tmpl);
    CHECK_EQ((int)write(upfd, "hello adbfs!", 12), 12);
    close(upfd);
    auto lpath = W(utf8_to_ws(tmpl));
    auto rpath = W(L"\\denied.txt");
    CHECK_EQ(FsPutFileW(lpath.data(), rpath.data(), 0), FS_FILE_WRITEERROR);
    unlink(tmpl);
    auto root = W(L"\\");
    FsDisconnectW(root.data());
}

TEST(sync_push_times_out_when_device_stops_reading) {
    // a wedged device link mid-upload must error out via the socket send
    // timeout instead of blocking the commander's UI thread forever
    FakeAdbServer server;
    useServer(server);
    setenv("ADBFS_READ_TIMEOUT", "1", 1);

    char tmpl[] = "/tmp/adbfs_sync_big_XXXXXX";
    int upfd = mkstemp(tmpl);
    std::string chunk(1 << 20, 'x');
    for (int i = 0; i < 8; i++)   // 8 MB: far beyond any loopback socket buffer
        CHECK_EQ((ssize_t)write(upfd, chunk.data(), chunk.size()), (ssize_t)chunk.size());
    close(upfd);
    auto lpath = W(utf8_to_ws(tmpl));
    auto rpath = W(L"\\stall.bin");
    CHECK_EQ(FsPutFileW(lpath.data(), rpath.data(), 0), FS_FILE_WRITEERROR);
    unlink(tmpl);
    unsetenv("ADBFS_READ_TIMEOUT");
    auto root = W(L"\\");
    FsDisconnectW(root.data());
}

TEST(shell_mode_transfers_through_device_shell) {
    FakeAdbServer server;
    useServer(server);
    setenv("ADBFS_TRANSFER_MODE", "shell", 1);

    char tmpl[] = "/tmp/adbfs_shell_dl_XXXXXX";
    int tmpfd = mkstemp(tmpl);
    close(tmpfd);
    unlink(tmpl);
    RemoteInfoStruct ri;
    memset(&ri, 0, sizeof(ri));
    ri.SizeLow = 12;
    auto rpath = W(L"\\file one");
    auto lpath = W(utf8_to_ws(tmpl));
    CHECK_EQ(FsGetFileW(rpath.data(), lpath.data(), 0, &ri), FS_FILE_OK);
    CHECK(readAll(tmpl) == "hello adbfs!");
    unlink(tmpl);

    CHECK(anyContains(server.commands(), "uuencode"));
    CHECK(server.syncRequests().empty());
    unsetenv("ADBFS_TRANSFER_MODE");
    auto root = W(L"\\");
    FsDisconnectW(root.data());
}

// --- Configure button: FsExecuteFile(root, "properties") from the
// --- commander's plugin settings dialog shows a Yes/No mode dialog.

static int reqCalls = 0;
static int reqLastType = -1;
static BOOL reqAnswer = 1;
static std::wstring reqLastText;
static BOOL reqCaptureW(int, int RequestType, WCHAR*, WCHAR* CustomText, WCHAR*, int) {
    reqCalls++;
    reqLastType = RequestType;
    reqLastText = CustomText ? u16_to_ws(CustomText) : L"";
    return reqAnswer;
}

TEST(configure_dialog_persists_transfer_mode) {
    unsetenv("ADBFS_TRANSFER_MODE");
    char tmpl[] = "/tmp/adbfs_wfxini_XXXXXX";
    int fd = mkstemp(tmpl);
    close(fd);
    unlink(tmpl);
    FsDefaultParamStruct dps;
    memset(&dps, 0, sizeof(dps));
    dps.size = sizeof(dps);
    snprintf(dps.DefaultIniName, sizeof(dps.DefaultIniName), "%s", tmpl);
    FsSetDefaultParams(&dps);
    FsInitW(7, progW, logW, reqCaptureW);

    // Double Commander invokes the Configure button as verb "properties" on
    // the root (PathDelim, '/' on Unix); the plugin's own paths use '\'.
    auto slash = W(L"/");
    auto backslash = W(L"\\");
    auto props = W(L"properties");

    reqCalls = 0;
    reqAnswer = 0;   // No = base64 through the device shell
    CHECK_EQ(FsExecuteFileW(NULL, slash.data(), props.data()), FS_EXEC_OK);
    CHECK_EQ(reqCalls, 1);
    CHECK_EQ(reqLastType, RT_MsgYesNo);
    CHECK_EQ(GetTransferMode(), TRANSFER_SHELL);

    reqAnswer = 1;   // Yes = sync protocol
    CHECK_EQ(FsExecuteFileW(NULL, backslash.data(), props.data()), FS_EXEC_OK);
    CHECK_EQ(reqCalls, 2);
    CHECK_EQ(GetTransferMode(), TRANSFER_SYNC);
    CHECK(reqLastText.find(L"ADBFS_TRANSFER_MODE") == std::wstring::npos);

    // with the env override active the saved choice has no effect —
    // the dialog must say so instead of leaving the user puzzled
    setenv("ADBFS_TRANSFER_MODE", "shell", 1);
    CHECK_EQ(FsExecuteFileW(NULL, slash.data(), props.data()), FS_EXEC_OK);
    CHECK(reqLastText.find(L"ADBFS_TRANSFER_MODE") != std::wstring::npos);
    unsetenv("ADBFS_TRANSFER_MODE");

    // the choice reached the ini file, not just process state
    std::string content = readAll(tmpl);
    CHECK(content.find("[adbfsplugin]") != std::string::npos);
    CHECK(content.find("TransferMode=sync") != std::string::npos);
    unlink(tmpl);

    // properties on a file (Alt+Enter) is not the configure dialog
    auto file = W(L"\\subdir\\file");
    CHECK_EQ(FsExecuteFileW(NULL, file.data(), props.data()), FS_EXEC_ERROR);
    CHECK_EQ(reqCalls, 3);
}

int main() {
    alarm(60);   // hard stop if the protocol deadlocks
    return run_all();
}
