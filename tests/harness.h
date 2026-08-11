#pragma once
#include <cstdio>
#include <vector>

struct TestCase { const char* name; void (*fn)(); };
inline std::vector<TestCase>& test_registry() { static std::vector<TestCase> r; return r; }
inline int& test_failures() { static int f = 0; return f; }
struct TestRegistrar { TestRegistrar(const char* n, void (*f)()) { test_registry().push_back({n, f}); } };

#define TEST(name) \
    static void test_##name(); \
    static TestRegistrar reg_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); test_failures()++; } } while (0)
#define CHECK_EQ(a, b) CHECK((a) == (b))

inline int run_all() {
    for (auto& t : test_registry()) { std::printf("RUN  %s\n", t.name); t.fn(); }
    if (test_failures()) { std::printf("%d check(s) FAILED\n", test_failures()); return 1; }
    std::printf("OK: %zu test(s) passed\n", test_registry().size());
    return 0;
}
