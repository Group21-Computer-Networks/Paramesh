#include "platform/adapters/adapters.h"

#include <picosha2.h>

#include <algorithm>
#include <array>

namespace paramesh {
namespace {

class PicoSha2Hasher final : public Hasher {
public:
    [[nodiscard]] Sha256 sha256(std::span<const std::byte> data) const override {
        const auto* first = reinterpret_cast<const unsigned char*>(data.data());
        std::array<unsigned char, 32> digest{};
        picosha2::hash256(first, first + data.size(), digest.begin(), digest.end());
        Sha256 out{};
        std::transform(digest.begin(), digest.end(), out.begin(),
                       [](unsigned char c) { return std::byte{c}; });
        return out;
    }
};

}  // namespace

std::unique_ptr<Hasher> make_picosha2_hasher(const Platform& /*made*/,
                                             const PlatformOptions& /*options*/) {
    return std::make_unique<PicoSha2Hasher>();
}

}  // namespace paramesh
