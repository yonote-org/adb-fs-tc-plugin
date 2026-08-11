#pragma once
#include "platform.h"
#include <string>

// UTF-16 (WCHAR, the plugin ABI) <-> std::wstring (UTF-32 on Unix) <-> UTF-8
std::wstring u16_to_ws(const WCHAR* s);
std::string  ws_to_utf8(const std::wstring& s);
std::wstring utf8_to_ws(const std::string& s);
std::string  u16_to_utf8(const WCHAR* s);
size_t u16len(const WCHAR* s);

// Fixed-buffer variants: always NUL-terminate, never split a surrogate pair
// or a multi-byte UTF-8 sequence at the truncation point.
void ws_to_u16buf(WCHAR* dst, size_t dstlen, const std::wstring& src);
void utf8_to_u16buf(WCHAR* dst, size_t dstlen, const char* src);
void u16_to_utf8buf(char* dst, size_t dstlen, const WCHAR* src);
void ws_to_utf8buf(char* dst, size_t dstlen, const std::wstring& src);

void copyfinddatawa(WIN32_FIND_DATAA* a, WIN32_FIND_DATAW* w);

// Callback wrappers that tolerate either the ANSI or Unicode callback set
// being NULL (Double Commander only registers the Unicode set).
int  ProgressT(const std::wstring& source, const std::wstring& target, int percent);
void LogT(int msgtype, const std::wstring& msg);
void LogA(int msgtype, const char* msg);

WCHAR* awfilenamecopy_impl(WCHAR* dst, size_t n, const char* src);
#define awfilenamecopy(out, in) awfilenamecopy_impl(out, countof(out), in)
