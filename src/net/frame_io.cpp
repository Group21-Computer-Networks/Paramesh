#include "net/frame_io.h"

#include <sys/socket.h>

#include <array>
#include <cerrno>

namespace paramesh {

namespace {

// Sends or receives exactly `size` bytes. 1 done, 0 the other end closed, -1 an error.
int write_all(int fd, const std::byte* data, std::size_t size) {
    while (size > 0) {
        const ssize_t sent = ::send(fd, data, size, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0) {
            return errno == EPIPE || errno == ECONNRESET ? 0 : -1;
        }
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return 1;
}

int read_all(int fd, std::byte* data, std::size_t size) {
    while (size > 0) {
        const ssize_t got = ::recv(fd, data, size, 0);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            return got == 0 || errno == ECONNRESET ? 0 : -1;
        }
        data += got;
        size -= static_cast<std::size_t>(got);
    }
    return 1;
}

Error failed(int outcome, const char* what) {
    return outcome == 0 ? Error{Errc::kClosed, 0, what} : Error{Errc::kIo, errno, what};
}

}  // namespace

Result<void> frame_write(int fd, const Checksum& checksum, FrameHeader header,
                         std::span<const std::byte> payload) {
    header.payload_len = static_cast<std::uint32_t>(payload.size());
    header.payload_crc = wire_payload_crc(checksum, payload);
    std::array<std::byte, kFrameHeaderSize> bytes{};
    wire_encode_header(header, bytes);
    int outcome = write_all(fd, bytes.data(), bytes.size());
    if (outcome == 1) {
        outcome = write_all(fd, payload.data(), payload.size());
    }
    if (outcome != 1) {
        return failed(outcome, "write a frame");
    }
    return {};
}

Result<Frame> frame_read(int fd, const Checksum& checksum) {
    std::array<std::byte, kFrameHeaderSize> bytes{};
    int outcome = read_all(fd, bytes.data(), bytes.size());
    if (outcome != 1) {
        return failed(outcome, "read a frame header");
    }
    Result<FrameHeader> header = wire_decode_header(bytes);
    if (!header.ok()) {
        return header.error();
    }
    Frame frame;
    frame.header = header.value();
    frame.payload.resize(frame.header.payload_len);
    outcome = read_all(fd, frame.payload.data(), frame.payload.size());
    if (outcome != 1) {
        return failed(outcome, "read a frame's payload");
    }
    if (const Result<void> checked = wire_check_payload(checksum, frame.header, frame.payload);
        !checked.ok()) {
        return checked.error();
    }
    return frame;
}

}  // namespace paramesh
