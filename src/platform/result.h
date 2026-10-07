// Result<T>: how every function in libparamesh reports failure. The library is built with
// exceptions off, so nothing here throws.
//
// Interface file drafted by M0-7; see docs/INTERNAL_API.md, section 2.
#ifndef PARAMESH_PLATFORM_RESULT_H
#define PARAMESH_PLATFORM_RESULT_H

#include <cassert>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <variant>

namespace paramesh {

// Why an operation failed. Deliberately few: a caller either handles one of these or passes
// the Error up to src/lib/, which turns it into a pm_status or aborts the job.
enum class Errc : std::uint8_t {
    kInvalidArgument,  // a caller broke the function's contract
    kState,            // called at the wrong time: not open, already closed, already running
    kNotFound,         // no such page, peer, task or timer
    kNoSpace,          // the region, the home store or the spill file is full
    kNoMemory,         // the process could not allocate
    kIo,               // a system call failed; Error::os_error holds errno
    kClosed,           // the peer closed the connection
    kProtocol,         // a frame failed a check of docs/PROTOCOL.md, section 3
    kUnsupported,      // not implemented in this milestone, or not available on this machine
};

struct Error {
    Errc code;
    int os_error = 0;       // errno when a system call failed, else 0
    const char* what = "";  // a string literal saying what was being done
};

template <typename T>
class [[nodiscard]] Result {
    static_assert(!std::is_same_v<T, Error>, "Result<Error> is ambiguous");
    static_assert(!std::is_reference_v<T>, "Result of a reference is not supported");

public:
    // Implicit on purpose: `return value;` and `return Error{...};` both work.
    Result(T value) : state_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : state_(std::in_place_index<1>, error) {}

    [[nodiscard]] bool ok() const noexcept { return state_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    // value() requires ok(); error() requires !ok().
    [[nodiscard]] T& value() & noexcept {
        assert(ok());
        return *std::get_if<0>(&state_);
    }
    [[nodiscard]] const T& value() const& noexcept {
        assert(ok());
        return *std::get_if<0>(&state_);
    }
    [[nodiscard]] T&& value() && noexcept {
        assert(ok());
        return std::move(*std::get_if<0>(&state_));
    }
    [[nodiscard]] const Error& error() const noexcept {
        assert(!ok());
        return *std::get_if<1>(&state_);
    }

private:
    std::variant<T, Error> state_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(error), failed_(true) {}

    [[nodiscard]] bool ok() const noexcept { return !failed_; }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const Error& error() const noexcept {
        assert(!ok());
        return error_;
    }

private:
    Error error_{Errc::kState, 0, ""};  // meaningful only when failed_
    bool failed_ = false;
};

}  // namespace paramesh

#endif  // PARAMESH_PLATFORM_RESULT_H
