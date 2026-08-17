#include "protocols.hpp"

#include <algorithm>

namespace yadro {

std::vector<Protocol> standard_protocols() {
    // Bruce Classic values are taken from the existing Reaterra UI shown on the target machine.
    Protocol bruce{
        .id = "bruce_classic",
        .name = "Bruce — классический",
        .description = "7 ступеней по 3 минуты. Скорость и уклон соответствуют текущему интерфейсу дорожки.",
        .implemented = true,
        .standard = true,
        .intervals = {
            {180, 2.7, 10.0, 3},
            {180, 4.0, 12.0, 3},
            {180, 5.4, 14.0, 3},
            {180, 6.7, 16.0, 3},
            {180, 8.0, 18.0, 3},
            {180, 8.8, 20.0, 3},
            {180, 9.6, 22.0, 3},
        }
    };

    // Names are present in the legacy UI, but their exact stage tables still need to be verified
    // against the original device documentation before they are allowed to drive hardware.
    return {
        std::move(bruce),
        {"naughton", "Naughton", "Таблица ступеней требует верификации.", false, true, {}},
        {"balke", "Balke", "Таблица ступеней требует верификации.", false, true, {}},
        {"ellestad", "Ellestad", "Таблица ступеней требует верификации.", false, true, {}},
        {"cornell", "Cornell", "Таблица ступеней требует верификации.", false, true, {}},
        {"kattus", "Kattus", "Таблица ступеней требует верификации.", false, true, {}},
        {"steep", "STEEP", "Таблица ступеней требует верификации.", false, true, {}},
        {"gardner", "Gardner", "Таблица ступеней требует верификации.", false, true, {}},
    };
}

std::optional<Protocol> find_protocol(const std::vector<Protocol>& protocols, std::string_view id) {
    const auto it = std::find_if(protocols.begin(), protocols.end(), [&](const Protocol& p) { return p.id == id; });
    if (it == protocols.end()) return std::nullopt;
    return *it;
}

} // namespace yadro
