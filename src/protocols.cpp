#include "protocols.hpp"

#include <algorithm>

namespace yadro {
namespace {
constexpr double mph(double value) { return value * 1.609344; }
}

std::vector<Protocol> standard_protocols() {
    Protocol bruce{
        .id = "bruce_classic",
        .name = "Bruce — классический",
        .description = "7 ступеней по 3 минуты. Параметры совпадают с исходным интерфейсом Reaterra на целевой дорожке.",
        .implemented = true,
        .standard = true,
        .intervals = {
            {180, 2.7, 10.0, 3}, {180, 4.0, 12.0, 3}, {180, 5.4, 14.0, 3},
            {180, 6.7, 16.0, 3}, {180, 8.0, 18.0, 3}, {180, 8.8, 20.0, 3},
            {180, 9.6, 22.0, 3},
        }
    };

    Protocol naughton{
        .id = "naughton", .name = "Naughton",
        .description = "2-минутные ступени; вариант таблицы AHA/AFP.",
        .implemented = true, .standard = true,
        .intervals = {
            {120, mph(1.0), 0.0, 3}, {120, mph(2.0), 0.0, 3}, {120, mph(2.0), 3.5, 3},
            {120, mph(2.0), 7.0, 3}, {120, mph(2.0), 10.5, 3}, {120, mph(2.0), 14.0, 3},
            {120, mph(2.0), 17.5, 3}
        }
    };

    Protocol ellestad{
        .id = "ellestad", .name = "Ellestad",
        .description = "Вариант Philips ST80i: 7 ступеней с изменением скорости и уклона.",
        .implemented = true, .standard = true,
        .intervals = {
            {180, mph(1.7), 10.0, 3}, {120, mph(3.0), 10.0, 3}, {120, mph(4.0), 10.0, 3},
            {180, mph(5.0), 10.0, 3}, {120, mph(6.0), 15.0, 3}, {120, mph(7.0), 15.0, 3},
            {120, mph(8.0), 15.0, 3}
        }
    };

    Protocol cornell{
        .id = "cornell", .name = "Cornell — модифицированный Bruce",
        .description = "11 ступеней по 2 минуты; Cornell-modified Bruce.",
        .implemented = true, .standard = true,
        .intervals = {
            {120, mph(1.7), 0.0, 3}, {120, mph(1.7), 5.0, 3}, {120, mph(1.7), 10.0, 3},
            {120, mph(2.1), 11.0, 3}, {120, mph(2.5), 12.0, 3}, {120, mph(3.0), 13.0, 3},
            {120, mph(3.4), 14.0, 3}, {120, mph(3.8), 15.0, 3}, {120, mph(4.2), 16.0, 3},
            {120, mph(4.6), 17.0, 3}, {120, mph(5.0), 18.0, 3}
        }
    };

    Protocol gardner{
        .id = "gardner", .name = "Gardner",
        .description = "3.2 км/ч; уклон увеличивается на 2% каждые 2 минуты до 18%, затем удерживается.",
        .implemented = true, .standard = true,
        .intervals = {
            {120, 3.2, 0.0, 3}, {120, 3.2, 2.0, 3}, {120, 3.2, 4.0, 3},
            {120, 3.2, 6.0, 3}, {120, 3.2, 8.0, 3}, {120, 3.2, 10.0, 3},
            {120, 3.2, 12.0, 3}, {120, 3.2, 14.0, 3}, {120, 3.2, 16.0, 3},
            {120, 3.2, 18.0, 3}, {1800, 3.2, 18.0, 3}
        }
    };

    return {
        std::move(bruce), std::move(naughton),
        {"balke", "Balke", "Требуется подтвердить именно вариант Reaterra (Balke/Balke-Ware/modified Balke).", false, true, {}},
        std::move(ellestad), std::move(cornell),
        {"kattus", "Kattus", "Требуется подтвердить таблицу варианта Reaterra.", false, true, {}},
        {"steep", "STEEP", "Требуется подтвердить таблицу варианта Reaterra.", false, true, {}},
        std::move(gardner)
    };
}

std::optional<Protocol> find_protocol(const std::vector<Protocol>& protocols, std::string_view id) {
    const auto it = std::find_if(protocols.begin(), protocols.end(), [&](const Protocol& p) { return p.id == id; });
    if (it == protocols.end()) return std::nullopt;
    return *it;
}

} // namespace yadro
