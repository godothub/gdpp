#pragma once

#include <cstdint>

namespace gdpp {

enum class RpcPermission : std::uint8_t { authority, any_peer };

enum class RpcTransferMode : std::uint8_t { unreliable, unreliable_ordered, reliable };

// Normalized language-level RPC metadata. The frontend validates GDScript's positional
// annotation syntax once, and every later compiler stage consumes this typed contract.
struct RpcConfiguration {
    RpcPermission permission{RpcPermission::authority};
    RpcTransferMode transfer_mode{RpcTransferMode::unreliable};
    bool call_local{false};
    std::int64_t channel{0};
    bool transfer_mode_explicit{false};
    bool call_local_explicit{false};
    bool channel_explicit{false};
};

} // namespace gdpp
