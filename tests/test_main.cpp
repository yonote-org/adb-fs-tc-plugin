#include "harness.h"

TEST(harness_sanity) { CHECK_EQ(1 + 1, 2); }

int main() { return run_all(); }
