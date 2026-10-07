// SHA-256, used for the binary hash of JOIN_JOB and SPAWN_REQ.
#ifndef PARAMESH_PLATFORM_HASHER_H
#define PARAMESH_PLATFORM_HASHER_H

#include <array>
#include <cstddef>
#include <span>

namespace paramesh {

using Sha256 = std::array<std::byte, 32>;

class Hasher {
public:
    Hasher() = default;
    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;
    Hasher(Hasher&&) = delete;
    Hasher& operator=(Hasher&&) = delete;
    virtual ~Hasher() = default;

    [[nodiscard]] virtual Sha256 sha256(std::span<const std::byte> data) const = 0;
};

}  // namespace paramesh

#endif  // PARAMESH_PLATFORM_HASHER_H
