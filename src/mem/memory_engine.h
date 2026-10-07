// The memory engine: the shared region, its page faults, and the four operations on a page.
//
// Interface file drafted by M0-7; see docs/INTERNAL_API.md, section 5. Implemented by M0-8
// (userfaultfd) and extended by M2-3 (write side).
#ifndef PARAMESH_MEM_MEMORY_ENGINE_H
#define PARAMESH_MEM_MEMORY_ENGINE_H

#include "platform/ids.h"
#include "platform/result.h"

#include <cstdint>
#include <memory>

namespace paramesh {

enum class FaultKind : std::uint8_t {
    kRead,   // a thread read a page that is not mapped
    kWrite,  // a thread wrote a page that is not mapped, or one that is write-protected
};

struct FaultEvent {
    PageId page;
    FaultKind kind = FaultKind::kRead;
};

// Receives fault events. Implemented by src/lib/.
class FaultSink {
public:
    FaultSink() = default;
    FaultSink(const FaultSink&) = delete;
    FaultSink& operator=(const FaultSink&) = delete;
    FaultSink(FaultSink&&) = delete;
    FaultSink& operator=(FaultSink&&) = delete;
    virtual ~FaultSink() = default;

    // Called on the fault-handler thread, once per fault the kernel reports. It must not wait
    // for the network, the disk or a lock a blocked thread can hold: it queues or sends and
    // returns. The faulting thread stays parked until install(), write_protect(off) or wake().
    virtual void on_fault(const FaultEvent& event) noexcept = 0;
};

enum class PageProtection : std::uint8_t { kReadOnly, kWritable };

// The HLD's helpers: mem_install() is install(), mem_zap() is zap(), mem_wp() is
// write_protect(), mem_wake() is wake(). All may be called from any thread.
class MemoryEngine {
public:
    MemoryEngine() = default;
    MemoryEngine(const MemoryEngine&) = delete;
    MemoryEngine& operator=(const MemoryEngine&) = delete;
    MemoryEngine(MemoryEngine&&) = delete;
    MemoryEngine& operator=(MemoryEngine&&) = delete;
    // Unmaps the region and stops the fault-handler thread.
    virtual ~MemoryEngine() = default;

    // Maps the page with these bytes and wakes every thread parked on it. A page that is
    // already mapped is left as it is, and that counts as success.
    virtual Result<void> install(PageId page, PageView data, PageProtection protection) = 0;

    // Unmaps the page. The next access to it faults. Parked threads are not woken.
    virtual Result<void> zap(PageId page) = 0;

    // on: a thread that then writes the page parks and a kWrite fault is reported.
    // off: writes are allowed again, and threads parked on the page are woken.
    virtual Result<void> write_protect(PageId page, bool on) = 0;

    // Wakes every thread parked on the page. Each retries its access and faults again if the
    // page still does not allow it.
    virtual Result<void> wake(PageId page) = 0;

    // Copies the mapped page out, for FETCH_DATA and WRITEBACK. The caller write-protects the
    // page first so the bytes cannot change. Errc::kState if the page is not mapped.
    virtual Result<void> read(PageId page, PageBuffer out) = 0;
};

struct RegionConfig {
    std::uint64_t bytes = kRegionMaxBytes;  // a whole number of segments
};

// Maps `bytes` at kRegionBase, registers it for missing and write-protect faults, and starts
// the fault-handler thread, which reports to `sink`. `sink` must outlive the engine.
// Errc::kUnsupported if the kernel lacks what is needed; Errc::kState if the address is taken.
Result<std::unique_ptr<MemoryEngine>> mem_open(const RegionConfig& config, FaultSink& sink);

}  // namespace paramesh

#endif  // PARAMESH_MEM_MEMORY_ENGINE_H
