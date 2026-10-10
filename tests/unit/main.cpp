// The one main() shared by every unit-test executable in tests/unit/.
//
// A test executable can also be started as a helper process by its own tests (pmd_test starts
// itself as a job process). A test file that wants this defines paramesh_test_helper(); it
// returns the exit status when the command line asks for the helper, or a negative number to
// run the tests as usual.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

extern "C" int paramesh_test_helper(int argc, char** argv) __attribute__((weak));

int main(int argc, char** argv) {
    if (paramesh_test_helper != nullptr) {
        const int status = paramesh_test_helper(argc, argv);
        if (status >= 0) {
            return status;
        }
    }
    return doctest::Context(argc, argv).run();
}
