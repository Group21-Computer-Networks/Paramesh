// MemoryEngine over userfaultfd: the shared region at its fixed address, one fault-handler
// thread, and the page operations of docs/INTERNAL_API.md, section 5.

#include "mem/memory_engine.h"
#include "platform/ids.h"
#include "platform/result.h"

#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace paramesh {
namespace {

Error io_error(const char* what) noexcept {
    return Error{Errc::kIo, errno, what};
}

// The region is at an address fixed by the design, so its pointers start life as numbers.
void* region_pointer(std::uint64_t address) noexcept {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<void*>(address);
}

// Owns a file descriptor.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~Fd() { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

// Owns a memory mapping.
class Mapping {
public:
    Mapping() = default;
    Mapping(void* addr, std::size_t bytes) noexcept : addr_(addr), bytes_(bytes) {}
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    Mapping(Mapping&& other) noexcept
        : addr_(std::exchange(other.addr_, nullptr)), bytes_(std::exchange(other.bytes_, 0)) {}
    Mapping& operator=(Mapping&& other) noexcept {
        if (this != &other) {
            reset();
            addr_ = std::exchange(other.addr_, nullptr);
            bytes_ = std::exchange(other.bytes_, 0);
        }
        return *this;
    }
    ~Mapping() { reset(); }

    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
    void reset() noexcept {
        if (addr_ != nullptr) {
            ::munmap(addr_, bytes_);
            addr_ = nullptr;
            bytes_ = 0;
        }
    }

private:
    void* addr_ = nullptr;
    std::size_t bytes_ = 0;
};

class UffdEngine final : public MemoryEngine {
public:
    UffdEngine(Mapping region, Fd uffd, Fd stop, FaultSink& sink) noexcept
        : uffd_(std::move(uffd)),
          stop_(std::move(stop)),
          region_(std::move(region)),
          sink_(&sink) {}
    UffdEngine(const UffdEngine&) = delete;
    UffdEngine& operator=(const UffdEngine&) = delete;
    UffdEngine(UffdEngine&&) = delete;
    UffdEngine& operator=(UffdEngine&&) = delete;

    ~UffdEngine() override {
        if (started_) {
            const std::uint64_t one = 1;
            // The handler thread waits on this eventfd as well as on the userfaultfd.
            const ssize_t written = ::write(stop_.get(), &one, sizeof one);
            static_cast<void>(written);
            ::pthread_join(thread_, nullptr);
        }
        // The region goes before the userfaultfd. In the other order a thread still parked on
        // a fault would be resumed on a fresh page of zeros; this way its access fails instead.
        region_.reset();
        uffd_.reset();
    }

    Result<void> start() noexcept {
        const int rc = ::pthread_create(&thread_, nullptr, &UffdEngine::thread_main, this);
        if (rc != 0) {
            return Error{Errc::kIo, rc, "start the fault-handler thread"};
        }
        started_ = true;
        return {};
    }

    Result<void> install(PageId page, PageView data, PageProtection protection) override {
        if (!contains(page)) {
            return Error{Errc::kInvalidArgument, 0, "install: page outside the region"};
        }
        uffdio_copy copy{};
        copy.dst = address_of(page);
        copy.src = reinterpret_cast<std::uintptr_t>(data.data());
        copy.len = kPageSize;
        copy.mode = protection == PageProtection::kReadOnly ? UFFDIO_COPY_MODE_WP : 0;
        for (;;) {
            if (::ioctl(uffd_.get(), UFFDIO_COPY, &copy) == 0) {
                return {};
            }
            if (errno == EEXIST) {
                // Already mapped: success. The copy that would have woken the page's waiters did
                // not happen, so wake them here.
                return wake(page);
            }
            if (errno != EAGAIN) {
                return io_error("install a page");
            }
            copy.copy = 0;
        }
    }

    Result<void> zap(PageId page) override {
        if (!contains(page)) {
            return Error{Errc::kInvalidArgument, 0, "zap: page outside the region"};
        }
        if (::madvise(region_pointer(address_of(page)), kPageSize, MADV_DONTNEED) != 0) {
            return io_error("discard a page");
        }
        return {};
    }

    Result<void> write_protect(PageId page, bool on) override {
        if (!contains(page)) {
            return Error{Errc::kInvalidArgument, 0, "write_protect: page outside the region"};
        }
        uffdio_writeprotect wp{};
        wp.range.start = address_of(page);
        wp.range.len = kPageSize;
        wp.mode = on ? UFFDIO_WRITEPROTECT_MODE_WP : 0;
        if (::ioctl(uffd_.get(), UFFDIO_WRITEPROTECT, &wp) != 0) {
            return io_error("change a page's write protection");
        }
        return {};
    }

    Result<void> wake(PageId page) override {
        if (!contains(page)) {
            return Error{Errc::kInvalidArgument, 0, "wake: page outside the region"};
        }
        uffdio_range range{};
        range.start = address_of(page);
        range.len = kPageSize;
        if (::ioctl(uffd_.get(), UFFDIO_WAKE, &range) != 0) {
            return io_error("wake a page's waiters");
        }
        return {};
    }

    Result<void> read(PageId page, PageBuffer out) override {
        if (!contains(page)) {
            return Error{Errc::kInvalidArgument, 0, "read: page outside the region"};
        }
        // Read through the kernel, not through the mapping: a missing page then gives EFAULT
        // here instead of a fault that would park this thread.
        iovec local{};
        local.iov_base = out.data();
        local.iov_len = kPageSize;
        iovec remote{};
        remote.iov_base = region_pointer(address_of(page));
        remote.iov_len = kPageSize;
        const ssize_t got = ::process_vm_readv(::getpid(), &local, 1, &remote, 1, 0);
        if (got == static_cast<ssize_t>(kPageSize)) {
            return {};
        }
        if (got >= 0 || errno == EFAULT) {
            return Error{Errc::kState, 0, "read: the page is not mapped"};
        }
        return io_error("read a page");
    }

private:
    [[nodiscard]] bool contains(PageId page) const noexcept {
        return page.value < region_.bytes() / kPageSize;
    }

    static void* thread_main(void* self) {
        static_cast<UffdEngine*>(self)->handler_loop();
        return nullptr;
    }

    // Waits only on the userfaultfd and the stop eventfd.
    void handler_loop() noexcept {
        std::array<pollfd, 2> fds{};
        fds[0].fd = uffd_.get();
        fds[0].events = POLLIN;
        fds[1].fd = stop_.get();
        fds[1].events = POLLIN;
        for (;;) {
            if (::poll(fds.data(), fds.size(), -1) < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return;
            }
            if (fds[1].revents != 0) {
                return;
            }
            if ((fds[0].revents & POLLIN) == 0) {
                if (fds[0].revents != 0) {
                    return;  // the userfaultfd reported an error or was closed
                }
                continue;
            }
            uffd_msg msg{};
            const ssize_t got = ::read(uffd_.get(), &msg, sizeof msg);
            if (got != static_cast<ssize_t>(sizeof msg)) {
                if (got < 0 && (errno == EAGAIN || errno == EINTR)) {
                    continue;  // another wake-up took the event, or the read was interrupted
                }
                return;
            }
            if (msg.event != UFFD_EVENT_PAGEFAULT) {
                continue;
            }
            const std::uint64_t writing = UFFD_PAGEFAULT_FLAG_WRITE | UFFD_PAGEFAULT_FLAG_WP;
            FaultEvent event;
            event.page = page_of(msg.arg.pagefault.address);
            event.kind =
                (msg.arg.pagefault.flags & writing) != 0 ? FaultKind::kWrite : FaultKind::kRead;
            sink_->on_fault(event);
        }
    }

    // Declared in this order so that, were the destructor's body not to do it, the region
    // would still be unmapped before the userfaultfd is closed.
    Fd uffd_;
    Fd stop_;
    Mapping region_;
    FaultSink* sink_;
    pthread_t thread_{};
    bool started_ = false;
};

}  // namespace

Result<std::unique_ptr<MemoryEngine>> mem_open(const RegionConfig& config, FaultSink& sink) {
    if (config.bytes == 0 || config.bytes > kRegionMaxBytes || config.bytes % kSegmentSize != 0) {
        return Error{Errc::kInvalidArgument, 0,
                     "the region must be a whole number of 2 MiB segments, at most 4 GiB"};
    }

    void* const want = region_pointer(kRegionBase);
    void* const got =
        ::mmap(want, config.bytes, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (got == MAP_FAILED) {
        const Errc code = errno == EEXIST ? Errc::kState : Errc::kIo;
        return Error{code, errno, "map the region at its fixed address"};
    }
    Mapping region{got, config.bytes};
    if (got != want) {
        // A kernel that does not know MAP_FIXED_NOREPLACE treats the address as a hint.
        return Error{Errc::kState, 0, "the region's fixed address is not available"};
    }
    // Huge pages would make one fault cover 2 MiB. Failure only means they are already off.
    static_cast<void>(::madvise(got, config.bytes, MADV_NOHUGEPAGE));

    const long raw = ::syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    if (raw < 0) {
        const bool unsupported = errno == EPERM || errno == ENOSYS || errno == EINVAL;
        return Error{unsupported ? Errc::kUnsupported : Errc::kIo, errno,
                     "open a user-mode-only userfaultfd"};
    }
    Fd uffd{static_cast<int>(raw)};

    uffdio_api api{};
    api.api = UFFD_API;
    api.features = UFFD_FEATURE_PAGEFAULT_FLAG_WP;
    if (::ioctl(uffd.get(), UFFDIO_API, &api) != 0 ||
        (api.features & UFFD_FEATURE_PAGEFAULT_FLAG_WP) == 0) {
        return Error{Errc::kUnsupported, errno, "userfaultfd write-protect is not available"};
    }

    uffdio_register reg{};
    reg.range.start = kRegionBase;
    reg.range.len = config.bytes;
    reg.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP;
    if (::ioctl(uffd.get(), UFFDIO_REGISTER, &reg) != 0) {
        return Error{Errc::kUnsupported, errno,
                     "register the region for missing and write-protect faults"};
    }
    const std::uint64_t need = (std::uint64_t{1} << static_cast<unsigned>(_UFFDIO_COPY)) |
                               (std::uint64_t{1} << static_cast<unsigned>(_UFFDIO_WAKE)) |
                               (std::uint64_t{1} << static_cast<unsigned>(_UFFDIO_WRITEPROTECT));
    if ((reg.ioctls & need) != need) {
        return Error{Errc::kUnsupported, 0, "the region lacks a userfaultfd operation"};
    }

    Fd stop{::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)};
    if (!stop.valid()) {
        return io_error("create the handler thread's stop eventfd");
    }

    auto engine =
        std::make_unique<UffdEngine>(std::move(region), std::move(uffd), std::move(stop), sink);
    if (const Result<void> started = engine->start(); !started) {
        return started.error();
    }
    return std::unique_ptr<MemoryEngine>{std::move(engine)};
}

}  // namespace paramesh
