#pragma once

// X-lock-pmd: only a node of the pool starts a job here. The pool's nodes share a token, which
// the website keeps in the state directory; SPAWN_REQ carries a tag made with it.

#include "platform/hasher.h"
#include "wire/payloads.h"

#include <cstdint>
#include <string>

namespace paramesh {

// The token in this state directory, or empty if there is none: this node takes any request.
[[nodiscard]] std::string read_pool_token(const std::string& state_dir);

// docs/PROTOCOL.md, SPAWN_REQ, pool_tag. All zero for an empty token.
[[nodiscard]] Sha256 spawn_tag(const Hasher& hasher, const std::string& token, std::uint64_t job,
                               std::uint64_t req, const SpawnReqPayload& request);

// Compares every byte, so the time taken says nothing about how much of a tag was right.
[[nodiscard]] bool same_tag(const Sha256& a, const Sha256& b) noexcept;

}  // namespace paramesh
