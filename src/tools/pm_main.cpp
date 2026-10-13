// pm, the executable. See pm.h.

#include "tools/pm.h"

#include <cstdio>
#include <exception>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    try {
        return paramesh::pm(std::vector<std::string_view>(argv + 1, argv + argc));
    } catch (const std::exception& failure) {
        static_cast<void>(std::fprintf(stderr, "pm: %s\n", failure.what()));
    } catch (...) {
        static_cast<void>(std::fputs("pm: an unknown failure\n", stderr));
    }
    return 1;
}
