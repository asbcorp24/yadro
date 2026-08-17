#pragma once

#include "treadmill.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace yadro {

std::vector<Protocol> standard_protocols();
std::optional<Protocol> find_protocol(const std::vector<Protocol>& protocols, std::string_view id);

} // namespace yadro
