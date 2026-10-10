// pmrun, the executable. See pmrun.h.

#include "tools/pmrun.h"

#include <cstdio>
#include <exception>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    try {
        return paramesh::pmrun(std::vector<std::string_view>(argv + 1, argv + argc));
    } catch (const std::exception& failure) {
        static_cast<void>(std::fprintf(stderr, "pmrun: %s\n", failure.what()));
    } catch (...) {
        static_cast<void>(std::fputs("pmrun: an unknown failure\n", stderr));
    }
    return 1;
}
