#include "pmd/pool_key.h"

#include <cstddef>
#include <fstream>
#include <span>
#include <string_view>
#include <vector>

namespace paramesh {

std::string read_pool_token(const std::string& state_dir) {
    std::string token;
    std::ifstream{state_dir + "/playground.token"} >> token;
    return token;
}

Sha256 spawn_tag(const Hasher& hasher, const std::string& token, std::uint64_t job,
                 std::uint64_t req, const SpawnReqPayload& request) {
    if (token.empty()) {
        return {};
    }
    std::vector<std::byte> message;
    const auto plain = [&message](std::string_view part) {
        const auto bytes = std::as_bytes(std::span{part.data(), part.size()});
        message.insert(message.end(), bytes.begin(), bytes.end());
    };
    const auto text = [&](std::string_view part) {
        plain(part);
        message.push_back(std::byte{0});
    };
    const auto number = [&message](std::uint64_t value) {
        for (int i = 0; i < 8; i++) {
            message.push_back(static_cast<std::byte>(value >> (i * 8)));
        }
    };
    plain(token);
    number(job);
    number(req);
    message.insert(message.end(), request.binary_hash.begin(), request.binary_hash.end());
    text(request.path);
    text(request.cwd);
    for (const std::string& argument : request.argv) {
        text(argument);
    }
    plain(token);
    return hasher.sha256(message);
}

bool same_tag(const Sha256& a, const Sha256& b) noexcept {
    std::byte difference{0};
    for (std::size_t i = 0; i < a.size(); i++) {
        difference |= a.at(i) ^ b.at(i);
    }
    return difference == std::byte{0};
}

}  // namespace paramesh
