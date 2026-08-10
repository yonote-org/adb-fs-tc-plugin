#include "harness.h"
#include "../wfxcompat.h"
#include "../adbfsplugin.h"

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
#include "../adbhandler.h"

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
TEST(loga_falls_back_to_w_callback) {
    static std::wstring got;
    LogProcW = [](int, int, WCHAR* s) { got = u16_to_ws(s); };
    LogA(1, "hello");
    LogProcW = nullptr;
    CHECK(got == L"hello");
}

int main() { return run_all(); }
