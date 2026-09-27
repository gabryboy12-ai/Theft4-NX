#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace rex::system::xam {
class LiveCompatibilityRuntime;
}

namespace gta4::input {

// Returns the remote session members whose authoritative GTA team matches the
// local member. Unknown/unassigned teams return nullopt so Team Chat can fail
// closed instead of accidentally broadcasting.
std::optional<std::vector<uint64_t>> FindTeamChatTargets(
    rex::system::xam::LiveCompatibilityRuntime* live);

}  // namespace gta4::input
