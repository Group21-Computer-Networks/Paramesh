// pmd: the ParaMesh daemon of one node. See pmd.h for what this first form of it does.

#include "platform/factory.h"
#include "pmd/pmd.h"

#include <pthread.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace {

paramesh::Pmd*& running() {
    static paramesh::Pmd* daemon = nullptr;
    return daemon;
}

// Ctrl+C is a graceful leave; a second one, or SIGTERM, stops at once.
void on_signal(int signal) {
    static volatile std::sig_atomic_t interrupted = 0;
    const int saved = errno;  // the write below must not change it under the interrupted code
    if (signal == SIGINT && interrupted == 0) {
        interrupted = 1;
        running()->leave();  // one write to an eventfd
    } else {
        running()->stop();
    }
    errno = saved;
}

// SIGINT and SIGTERM, held back from every thread until the main one is ready for them.
sigset_t stop_signals() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    return set;
}

// log.level: info unless set. A job process defaults to warn.
paramesh::LogLevel log_level() {
    const char* set = std::getenv("PARAMESH_CFG_LOG_LEVEL");  // NOLINT(concurrency-mt-unsafe)
    const std::string_view name = set != nullptr ? set : "info";
    if (name == "debug") {
        return paramesh::LogLevel::kDebug;
    }
    if (name == "warn") {
        return paramesh::LogLevel::kWarn;
    }
    return name == "error" ? paramesh::LogLevel::kError : paramesh::LogLevel::kInfo;
}

int serve(const std::vector<std::string_view>& args) {
    if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) {
        static_cast<void>(std::fputs(paramesh::pmd_usage().data(), stdout));
        return 0;
    }
    // Blocked before any thread starts, so the daemon's other threads never take them, and one
    // that comes before the handler is ready waits instead of being lost.
    const sigset_t held = stop_signals();
    pthread_sigmask(SIG_BLOCK, &held, nullptr);
    paramesh::Result<paramesh::PmdConfig> config = paramesh::pmd_parse_args(args);
    if (!config.ok()) {
        static_cast<void>(
            std::fprintf(stderr, "pmd: %s\n%s", config.error().what, paramesh::pmd_usage().data()));
        return 2;
    }
    // The tunables, for this daemon and the workers it starts (docs/PLAN.md, Configuration).
    if (const paramesh::Result<void> loaded =
            paramesh::platform_load_settings(config.value().state_dir);
        !loaded.ok()) {
        static_cast<void>(std::fprintf(stderr, "pmd: cannot %s\n", loaded.error().what));
        return 2;
    }
    paramesh::Result<paramesh::Platform> platform = paramesh::platform_open(
        {}, paramesh::PlatformOptions{config.value().node, paramesh::JobId{0}, 2, log_level()});
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
    pthread_sigmask(SIG_UNBLOCK, &held, nullptr);  // the main thread only
    daemon.run();
    pthread_sigmask(SIG_BLOCK, &held, nullptr);  // none may reach the handler from here on
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
