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
TEST(loga_falls_back_to_w_callback) {
    static std::wstring got;
    LogProcW = [](int, int, WCHAR* s) { got = u16_to_ws(s); };
    LogA(1, "hello");
    LogProcW = nullptr;
    CHECK(got == L"hello");
}

int main() { return run_all(); }
