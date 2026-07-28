#include <cstdint>
#include <cstdio>
#include <vector>

#include "hardware/tracker/resample_grid.h"

static bool expect_equal(const std::vector<uint64_t>& actual,
                         const std::vector<uint64_t>& expected,
                         const char* name) {
    if (actual == expected) return true;
    fprintf(stderr, "%s: expected %zu entries, got %zu\n",
            name, expected.size(), actual.size());
    return false;
}

int main() {
    bool ok = true;
    ok &= expect_equal(make_resample_grid(1000, 1920999, 480000),
                       {1000, 481000, 961000, 1441000}, "100 Hz grid");
    ok &= expect_equal(make_resample_grid(20, 10, 480000), {}, "reverse range");
    ok &= expect_equal(make_resample_grid(10, 20, 0), {}, "zero interval");
    return ok ? 0 : 1;
}
