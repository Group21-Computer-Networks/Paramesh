// One frame of docs/PROTOCOL.md section 3 at a time on a stream socket, blocking: for the
// daemon's replies, and for what talks to the daemon (tools, tests).
#ifndef PARAMESH_PMD_FRAME_IO_H
#define PARAMESH_PMD_FRAME_IO_H

#include "platform/checksum.h"
#include "platform/result.h"
#include "wire/frame.h"

#include <cstddef>
#include <span>
#include <vector>

namespace paramesh {

struct Frame {
    FrameHeader header;
    std::vector<std::byte> payload;
};

// Writes the header and the payload. The header's payload_len and payload_crc are filled in
// here. Errc::kIo if the socket fails, Errc::kClosed if the other end has gone.
Result<void> frame_write(int fd, const Checksum& checksum, FrameHeader header,
                         std::span<const std::byte> payload);

// Reads one whole frame. Errc::kClosed at end of stream, Errc::kProtocol for a frame that
// fails the checks of section 3, Errc::kIo if the socket fails.
Result<Frame> frame_read(int fd, const Checksum& checksum);

}  // namespace paramesh

#endif  // PARAMESH_PMD_FRAME_IO_H
