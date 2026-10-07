// CRC-32C (Castagnoli), the payload checksum of docs/PROTOCOL.md, section 3.
#ifndef PARAMESH_PLATFORM_CHECKSUM_H
#define PARAMESH_PLATFORM_CHECKSUM_H

#include <cstddef>
#include <cstdint>
#include <span>

namespace paramesh {

class Checksum {
public:
    Checksum() = default;
    Checksum(const Checksum&) = delete;
    Checksum& operator=(const Checksum&) = delete;
    Checksum(Checksum&&) = delete;
    Checksum& operator=(Checksum&&) = delete;
    virtual ~Checksum() = default;

    [[nodiscard]] virtual std::uint32_t crc32c(std::span<const std::byte> data) const noexcept = 0;
};

}  // namespace paramesh

#endif  // PARAMESH_PLATFORM_CHECKSUM_H
