#include "wfxcompat.h"
#include "adbfsplugin.h"

namespace {

const uint32_t kReplacement = 0xFFFD;

void append_cp_utf8(std::string& out, uint32_t c) {
    if (c < 0x80) { out.push_back((char)c); }
    else if (c < 0x800) {
        out.push_back((char)(0xC0 | (c >> 6)));
        out.push_back((char)(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back((char)(0xE0 | (c >> 12)));
        out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (c & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (c >> 18)));
        out.push_back((char)(0x80 | ((c >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (c & 0x3F)));
    }
}

uint32_t next_cp_utf8(const std::string& s, size_t& i) {
    unsigned char c = (unsigned char)s[i++];
    if (c < 0x80) return c;
    int extra; uint32_t cp;
    if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else return kReplacement;
    for (int k = 0; k < extra; k++) {
        if (i >= s.size() || ((unsigned char)s[i] & 0xC0) != 0x80) return kReplacement;
        cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
    }
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return kReplacement;
    return cp;
}

uint32_t sanitize(uint32_t c) {
    if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return kReplacement;
    return c;
}

} // namespace

std::wstring u16_to_ws(const WCHAR* s) {
    std::wstring out;
    if (!s) return out;
    // s[i] after a high surrogate is safe: the terminating NUL fails the
    // low-surrogate range check before any further read
    for (size_t i = 0; s[i]; ) {
        uint32_t c = s[i++];
        if (c >= 0xD800 && c <= 0xDBFF && s[i] >= 0xDC00 && s[i] <= 0xDFFF)
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i++] - 0xDC00);
        out.push_back((wchar_t)c);
    }
    return out;
}

std::string ws_to_utf8(const std::wstring& s) {
    std::string out;
    for (wchar_t wc : s) append_cp_utf8(out, sanitize((uint32_t)wc));
    return out;
}

std::wstring utf8_to_ws(const std::string& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size(); ) out.push_back((wchar_t)next_cp_utf8(s, i));
    return out;
}

std::string u16_to_utf8(const WCHAR* s) { return ws_to_utf8(u16_to_ws(s)); }

size_t u16len(const WCHAR* s) { size_t n = 0; while (s && s[n]) n++; return n; }

void ws_to_u16buf(WCHAR* dst, size_t dstlen, const std::wstring& src) {
    size_t o = 0;
    for (wchar_t wc : src) {
        uint32_t c = sanitize((uint32_t)wc);
        if (c < 0x10000) {
            if (o + 1 >= dstlen) break;
            dst[o++] = (WCHAR)c;
        } else {
            if (o + 2 >= dstlen) break;
            c -= 0x10000;
            dst[o++] = (WCHAR)(0xD800 + (c >> 10));
            dst[o++] = (WCHAR)(0xDC00 + (c & 0x3FF));
        }
    }
    if (dstlen) dst[o] = 0;
}

void utf8_to_u16buf(WCHAR* dst, size_t dstlen, const char* src) {
    ws_to_u16buf(dst, dstlen, utf8_to_ws(src ? src : ""));
}

void u16_to_utf8buf(char* dst, size_t dstlen, const WCHAR* src) {
    ws_to_utf8buf(dst, dstlen, u16_to_ws(src));
}

void ws_to_utf8buf(char* dst, size_t dstlen, const std::wstring& src) {
    std::string u8 = ws_to_utf8(src);
    size_t n = u8.size();
    if (n >= dstlen) {
        n = dstlen ? dstlen - 1 : 0;
        while (n > 0 && ((unsigned char)u8[n] & 0xC0) == 0x80) n--;  // don't split a UTF-8 sequence
    }
    memcpy(dst, u8.data(), n);
    if (dstlen) dst[n] = 0;
}

WCHAR* awfilenamecopy_impl(WCHAR* dst, size_t n, const char* src) {
    utf8_to_u16buf(dst, n, src);
    return dst;
}

void copyfinddatawa(WIN32_FIND_DATAA* a, WIN32_FIND_DATAW* w) {
    u16_to_utf8buf(a->cFileName, sizeof(a->cFileName), w->cFileName);
    u16_to_utf8buf(a->cAlternateFileName, sizeof(a->cAlternateFileName), w->cAlternateFileName);
    a->dwFileAttributes = w->dwFileAttributes;
    a->dwReserved0 = w->dwReserved0;
    a->dwReserved1 = w->dwReserved1;
    a->ftCreationTime = w->ftCreationTime;
    a->ftLastAccessTime = w->ftLastAccessTime;
    a->ftLastWriteTime = w->ftLastWriteTime;
    a->nFileSizeHigh = w->nFileSizeHigh;
    a->nFileSizeLow = w->nFileSizeLow;
}

int ProgressT(const std::wstring& source, const std::wstring& target, int percent) {
    if (ProgressProcW) {
        WCHAR a[wdirtypemax], b[wdirtypemax];
        ws_to_u16buf(a, countof(a), source);
        ws_to_u16buf(b, countof(b), target);
        return ProgressProcW(PluginNumber, a, b, percent);
    }
    if (ProgressProc) {
        char a[wdirtypemax], b[wdirtypemax];
        ws_to_utf8buf(a, sizeof(a), source);
        ws_to_utf8buf(b, sizeof(b), target);
        return ProgressProc(PluginNumber, a, b, percent);
    }
    return 0;
}

void LogT(int msgtype, const std::wstring& msg) {
    if (LogProcW) {
        WCHAR buf[wdirtypemax];
        ws_to_u16buf(buf, countof(buf), msg);
        LogProcW(PluginNumber, msgtype, buf);
    } else if (LogProc) {
        char buf[wdirtypemax];
        ws_to_utf8buf(buf, sizeof(buf), msg);
        LogProc(PluginNumber, msgtype, buf);
    }
}

void LogA(int msgtype, const char* msg) {
    if (LogProc) {
        LogProc(PluginNumber, msgtype, (char*)msg);
    } else if (LogProcW) {
        WCHAR buf[wdirtypemax];
        utf8_to_u16buf(buf, countof(buf), msg);
        LogProcW(PluginNumber, msgtype, buf);
    }
}
