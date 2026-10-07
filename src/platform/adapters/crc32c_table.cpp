#include "platform/adapters/adapters.h"

#include <array>
#include <cstdint>

namespace paramesh {
namespace {

// CRC-32C: polynomial 0x1EDC6F41, reflected (0x82F63B78), initial value and final XOR all ones.
constexpr std::array<std::uint32_t, 256> kTable = [] {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; i++) {
        std::uint32_t crc = i;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 1U) != 0 ? (crc >> 1U) ^ 0x82F63B78U : crc >> 1U;
        }
        table.at(i) = crc;
    }
    return table;
}();

// ponytail: one byte per step; slicing-by-8 or the SSE4.2 instruction if M6-5 shows it matters.
class TableCrc32c final : public Checksum {
public:
    [[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> data) const noexcept override {
        std::uint32_t crc = 0xFFFFFFFFU;
        for (const std::byte b : data) {
            crc = (crc >> 8U) ^ kTable.at((crc ^ std::to_integer<std::uint32_t>(b)) & 0xFFU);
        }
        return ~crc;
    }
};

}  // namespace

std::unique_ptr<Checksum> make_table_crc32c(const Platform& /*made*/,
                                            const PlatformOptions& /*options*/) {
    return std::make_unique<TableCrc32c>();
}

}  // namespace paramesh
