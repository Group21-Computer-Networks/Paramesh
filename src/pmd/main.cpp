// pmd: the ParaMesh daemon of one node. See pmd.h for what this first form of it does.

#include "pmd/pmd.h"

#include <csignal>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace {

paramesh::Pmd*& running() {
    static paramesh::Pmd* daemon = nullptr;
    return daemon;
}

void on_signal(int /*signal*/) {
    if (running() != nullptr) {
        running()->stop();  // one write to an eventfd
    }
}

int serve(const std::vector<std::string_view>& args) {
    if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) {
        static_cast<void>(std::fputs(paramesh::pmd_usage().data(), stdout));
        return 0;
    }
    paramesh::Result<paramesh::PmdConfig> config = paramesh::pmd_parse_args(args);
    if (!config.ok()) {
        static_cast<void>(
            std::fprintf(stderr, "pmd: %s\n%s", config.error().what, paramesh::pmd_usage().data()));
        return 2;
    }
    paramesh::Result<paramesh::Platform> platform = paramesh::platform_open(
        {}, paramesh::PlatformOptions{config.value().node, paramesh::JobId{0}, 2,
                                      paramesh::LogLevel::kInfo});
    if (!platform.ok()) {
        static_cast<void>(std::fprintf(stderr, "pmd: %s\n", platform.error().what));
        return 1;
    }
    paramesh::Pmd daemon{std::move(config).value(), platform.value()};
    if (const paramesh::Result<void> opened = daemon.open(); !opened.ok()) {
        static_cast<void>(std::fprintf(stderr, "pmd: cannot %s\n", opened.error().what));
        return 1;
    }
    running() = &daemon;
    static_cast<void>(std::signal(SIGINT, on_signal));
    static_cast<void>(std::signal(SIGTERM, on_signal));
    daemon.run();
    running() = nullptr;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return serve(std::vector<std::string_view>(argv + 1, argv + argc));
    } catch (const std::exception& failure) {
        static_cast<void>(std::fprintf(stderr, "pmd: %s\n", failure.what()));
    } catch (...) {
        static_cast<void>(std::fputs("pmd: an unknown failure\n", stderr));
    }
    return 1;
}
