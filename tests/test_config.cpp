// Transfer-mode setting: [adbfsplugin] TransferMode= in the wfx.ini Double
// Commander hands the plugin via FsSetDefaultParams.
#include "harness.h"
#include "../adbhandler.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

static std::string readAll(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "";
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

static std::string tempIni() {
    char tmpl[] = "/tmp/adbfs_wfx_XXXXXX";
    int fd = mkstemp(tmpl);
    close(fd);
    return tmpl;
}

TEST(transfer_mode_defaults_to_sync) {
    unsetenv("ADBFS_TRANSFER_MODE");
    SetConfigIniPath("/nonexistent/dir/wfx.ini");
    CHECK_EQ(GetTransferMode(), TRANSFER_SYNC);
}

TEST(transfer_mode_read_from_ini_section) {
    unsetenv("ADBFS_TRANSFER_MODE");
    std::string ini = tempIni();
    FILE* f = fopen(ini.c_str(), "wb");
    fputs("[otherplugin]\nTransferMode=sync\n[adbfsplugin]\nTransferMode=shell\n", f);
    fclose(f);
    SetConfigIniPath(ini);
    CHECK_EQ(GetTransferMode(), TRANSFER_SHELL);
    unlink(ini.c_str());
}

TEST(transfer_mode_env_overrides_ini) {
    std::string ini = tempIni();
    FILE* f = fopen(ini.c_str(), "wb");
    fputs("[adbfsplugin]\nTransferMode=shell\n", f);
    fclose(f);
    SetConfigIniPath(ini);
    setenv("ADBFS_TRANSFER_MODE", "sync", 1);
    CHECK_EQ(GetTransferMode(), TRANSFER_SYNC);
    unsetenv("ADBFS_TRANSFER_MODE");
    unlink(ini.c_str());
}

TEST(save_transfer_mode_preserves_other_sections) {
    unsetenv("ADBFS_TRANSFER_MODE");
    std::string ini = tempIni();
    FILE* f = fopen(ini.c_str(), "wb");
    fputs("[ftpplugin]\nHost=example\n", f);
    fclose(f);
    SetConfigIniPath(ini);
    SaveTransferMode(TRANSFER_SHELL);
    CHECK_EQ(GetTransferMode(), TRANSFER_SHELL);
    std::string content = readAll(ini);
    CHECK(content.find("[ftpplugin]") != std::string::npos);
    CHECK(content.find("Host=example") != std::string::npos);
    CHECK(content.find("[adbfsplugin]") != std::string::npos);
    CHECK(content.find("TransferMode=shell") != std::string::npos);

    // update in place: no duplicated key or section
    SaveTransferMode(TRANSFER_SYNC);
    CHECK_EQ(GetTransferMode(), TRANSFER_SYNC);
    content = readAll(ini);
    CHECK(content.find("TransferMode=sync") != std::string::npos);
    CHECK(content.find("TransferMode=shell") == std::string::npos);
    CHECK_EQ(content.find("[adbfsplugin]"), content.rfind("[adbfsplugin]"));
    CHECK(content.find("Host=example") != std::string::npos);
    unlink(ini.c_str());
}

TEST(save_transfer_mode_reuses_existing_section) {
    unsetenv("ADBFS_TRANSFER_MODE");
    std::string ini = tempIni();
    FILE* f = fopen(ini.c_str(), "wb");
    fputs("[adbfsplugin]\nSomeFutureKey=1\n[ftpplugin]\nHost=example\n", f);
    fclose(f);
    SetConfigIniPath(ini);
    SaveTransferMode(TRANSFER_SHELL);
    CHECK_EQ(GetTransferMode(), TRANSFER_SHELL);
    std::string content = readAll(ini);
    CHECK_EQ(content.find("[adbfsplugin]"), content.rfind("[adbfsplugin]"));
    CHECK(content.find("SomeFutureKey=1") != std::string::npos);
    CHECK(content.find("Host=example") != std::string::npos);
    // the key must land inside our section, not after some other one
    CHECK(content.find("TransferMode=shell") < content.find("[ftpplugin]"));
    unlink(ini.c_str());
}

TEST(save_transfer_mode_follows_symlinked_ini) {
    // dotfile managers symlink DC's config; saving must write through the
    // link, not replace it with a regular file
    unsetenv("ADBFS_TRANSFER_MODE");
    std::string real = tempIni();
    FILE* f = fopen(real.c_str(), "wb");
    fputs("[ftpplugin]\nHost=example\n", f);
    fclose(f);
    std::string link = real + ".link";
    unlink(link.c_str());
    CHECK_EQ(symlink(real.c_str(), link.c_str()), 0);
    SetConfigIniPath(link);
    SaveTransferMode(TRANSFER_SHELL);
    CHECK_EQ(GetTransferMode(), TRANSFER_SHELL);
    struct stat st;
    CHECK_EQ(lstat(link.c_str(), &st), 0);
    CHECK(S_ISLNK(st.st_mode));
    std::string content = readAll(real);
    CHECK(content.find("TransferMode=shell") != std::string::npos);
    CHECK(content.find("Host=example") != std::string::npos);
    unlink(link.c_str());
    unlink(real.c_str());
}

TEST(save_transfer_mode_failure_leaves_ini_untouched) {
    unsetenv("ADBFS_TRANSFER_MODE");
    char dtmpl[] = "/tmp/adbfs_rodir_XXXXXX";
    CHECK(mkdtemp(dtmpl) != NULL);
    std::string ini = std::string(dtmpl) + "/wfx.ini";
    FILE* f = fopen(ini.c_str(), "wb");
    fputs("[adbfsplugin]\nTransferMode=sync\n[ftpplugin]\nHost=example\n", f);
    fclose(f);
    CHECK_EQ(chmod(dtmpl, 0555), 0);   // directory read-only: no writes possible
    SetConfigIniPath(ini);
    SaveTransferMode(TRANSFER_SHELL);
    std::string content = readAll(ini);
    CHECK(content.find("TransferMode=sync") != std::string::npos);
    CHECK(content.find("Host=example") != std::string::npos);
    chmod(dtmpl, 0755);
    unlink(ini.c_str());
    rmdir(dtmpl);
}

TEST(save_transfer_mode_creates_missing_file) {
    unsetenv("ADBFS_TRANSFER_MODE");
    std::string ini = tempIni();
    unlink(ini.c_str());   // path is valid but no file yet
    SetConfigIniPath(ini);
    SaveTransferMode(TRANSFER_SHELL);
    CHECK_EQ(GetTransferMode(), TRANSFER_SHELL);
    std::string content = readAll(ini);
    CHECK(content.find("[adbfsplugin]") != std::string::npos);
    CHECK(content.find("TransferMode=shell") != std::string::npos);
    unlink(ini.c_str());
}
