#include "web_server.hpp"

#include "protocols.hpp"

#include <algorithm>
#include <fstream>
#include <regex>
#include <stdexcept>

namespace yadro {
using json = nlohmann::json;
namespace {

void json_response(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json; charset=utf-8");
    res.set_header("Cache-Control", "no-store");
}

json command_json(const CommandResult& result) {
    return {{"ok", result.ok}, {"message", result.message}};
}

bool valid_id(const std::string& id) {
    static const std::regex pattern("^[A-Za-z0-9_-]{1,64}$");
    return std::regex_match(id, pattern);
}

} // namespace

WebServer::WebServer(TreadmillController& controller,
                     std::filesystem::path data_dir,
                     std::filesystem::path static_dir)
    : controller_(controller),
      data_dir_(std::move(data_dir)),
      static_dir_(std::move(static_dir)),
      profiles_file_(data_dir_ / "profiles.json"),
      patients_file_(data_dir_ / "patients.json"),
      standard_protocols_(yadro::standard_protocols()) {
    std::filesystem::create_directories(data_dir_);
    register_routes();
}

bool WebServer::listen(const std::string& bind_address, int port) {
    if (!std::filesystem::exists(static_dir_)) {
        throw std::runtime_error("static directory does not exist: " + static_dir_.string());
    }
    if (!server_.set_mount_point("/", static_dir_.string())) {
        throw std::runtime_error("cannot mount static directory: " + static_dir_.string());
    }
    return server_.listen(bind_address, port);
}

void WebServer::stop() { server_.stop(); }

void WebServer::register_routes() {
    server_.Get("/api/v1/health", [&](const httplib::Request&, httplib::Response& res) {
        json_response(res, 200, {{"ok", true}, {"service", "yadro"}, {"version", "0.1.0"}, {"driver", "simulation"}});
    });

    server_.Get("/api/v1/state", [&](const httplib::Request&, httplib::Response& res) {
        json_response(res, 200, {{"ok", true}, {"telemetry", telemetry_json()}});
    });

    server_.Post("/api/v1/heartbeat", [&](const httplib::Request&, httplib::Response& res) {
        controller_.heartbeat();
        json_response(res, 200, {{"ok", true}});
    });

    server_.Post("/api/v1/control/targets", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            const auto body = json::parse(req.body);
            const auto current = controller_.telemetry();
            const double speed = body.value("speed_kmh", current.target_speed_kmh);
            const double incline = body.value("incline_percent", current.target_incline_percent);
            auto result = controller_.set_targets(speed, incline);
            json_response(res, result.ok ? 200 : 400, command_json(result));
        } catch (const std::exception& e) {
            json_response(res, 400, {{"ok", false}, {"message", e.what()}});
        }
    });

    server_.Post("/api/v1/control/direction", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            const auto body = json::parse(req.body);
            const std::string value = body.value("direction", "forward");
            auto result = controller_.set_direction(value == "reverse" ? Direction::Reverse : Direction::Forward);
            json_response(res, result.ok ? 200 : 400, command_json(result));
        } catch (const std::exception& e) {
            json_response(res, 400, {{"ok", false}, {"message", e.what()}});
        }
    });

    server_.Post("/api/v1/control/start", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            const auto body = req.body.empty() ? json::object() : json::parse(req.body);
            const std::string mode = body.value("mode", "free");
            CommandResult result;
            if (mode == "free") {
                result = controller_.start_free();
            } else {
                const std::string id = body.value("program_id", "");
                std::optional<Protocol> program;
                SessionKind kind = SessionKind::Protocol;
                if (mode == "protocol") {
                    program = find_protocol(standard_protocols_, id);
                } else if (mode == "profile") {
                    program = find_protocol(load_profiles(), id);
                    kind = SessionKind::Profile;
                } else {
                    result = {false, "unknown start mode"};
                    json_response(res, 400, command_json(result));
                    return;
                }
                result = program ? controller_.start_program(*program, kind) : CommandResult{false, "program not found"};
            }
            json_response(res, result.ok ? 200 : 400, command_json(result));
        } catch (const std::exception& e) {
            json_response(res, 400, {{"ok", false}, {"message", e.what()}});
        }
    });

    server_.Post("/api/v1/control/stop", [&](const httplib::Request&, httplib::Response& res) {
        const auto result = controller_.stop();
        json_response(res, 200, command_json(result));
    });

    server_.Post("/api/v1/control/emergency-stop", [&](const httplib::Request&, httplib::Response& res) {
        const auto result = controller_.emergency_stop();
        json_response(res, 200, command_json(result));
    });

    server_.Post("/api/v1/control/reset-emergency", [&](const httplib::Request&, httplib::Response& res) {
        const auto result = controller_.reset_emergency();
        json_response(res, result.ok ? 200 : 400, command_json(result));
    });

    server_.Post("/api/v1/simulation/heart-rate", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            const auto body = json::parse(req.body);
            controller_.set_heart_rate(body.value("bpm", 0));
            json_response(res, 200, {{"ok", true}});
        } catch (const std::exception& e) {
            json_response(res, 400, {{"ok", false}, {"message", e.what()}});
        }
    });

    server_.Get("/api/v1/protocols", [&](const httplib::Request&, httplib::Response& res) {
        json items = json::array();
        for (const auto& p : standard_protocols_) items.push_back(protocol_json(p));
        json_response(res, 200, {{"ok", true}, {"items", items}});
    });

    server_.Get("/api/v1/profiles", [&](const httplib::Request&, httplib::Response& res) {
        json items = json::array();
        for (const auto& p : load_profiles()) items.push_back(protocol_json(p));
        json_response(res, 200, {{"ok", true}, {"items", items}});
    });

    server_.Post("/api/v1/profiles", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            auto profile = parse_profile(json::parse(req.body));
            auto profiles = load_profiles();
            auto it = std::find_if(profiles.begin(), profiles.end(), [&](const Protocol& p) { return p.id == profile.id; });
            if (it == profiles.end()) profiles.push_back(profile); else *it = profile;
            save_profiles(profiles);
            json_response(res, 200, {{"ok", true}, {"profile", protocol_json(profile)}});
        } catch (const std::exception& e) {
            json_response(res, 400, {{"ok", false}, {"message", e.what()}});
        }
    });

    server_.Delete(R"(/api/v1/profiles/([A-Za-z0-9_-]+))", [&](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.matches[1];
        auto profiles = load_profiles();
        const auto old_size = profiles.size();
        profiles.erase(std::remove_if(profiles.begin(), profiles.end(), [&](const Protocol& p) { return p.id == id; }), profiles.end());
        if (profiles.size() == old_size) {
            json_response(res, 404, {{"ok", false}, {"message", "profile not found"}});
            return;
        }
        save_profiles(profiles);
        json_response(res, 200, {{"ok", true}});
    });

    server_.Get("/api/v1/patients", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard lock(storage_mutex_);
        json_response(res, 200, {{"ok", true}, {"items", read_array_file(patients_file_)}});
    });

    server_.Post("/api/v1/patients", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            auto patient = json::parse(req.body);
            std::string id = patient.value("id", "");
            if (!valid_id(id)) throw std::runtime_error("patient id must match [A-Za-z0-9_-], max 64 chars");
            std::lock_guard lock(storage_mutex_);
            auto patients = read_array_file(patients_file_);
            bool replaced = false;
            for (auto& item : patients) {
                if (item.value("id", "") == id) { item = patient; replaced = true; break; }
            }
            if (!replaced) patients.push_back(patient);
            write_json_file(patients_file_, patients);
            json_response(res, 200, {{"ok", true}, {"patient", patient}});
        } catch (const std::exception& e) {
            json_response(res, 400, {{"ok", false}, {"message", e.what()}});
        }
    });

    server_.Get("/api/v1/config", [&](const httplib::Request&, httplib::Response& res) {
        const auto l = controller_.limits();
        json_response(res, 200, {{"ok", true}, {"limits", {
            {"max_speed_kmh", l.max_speed_kmh},
            {"max_incline_percent", l.max_incline_percent},
            {"watchdog_seconds", l.watchdog_seconds}
        }}});
    });

    server_.set_error_handler([](const httplib::Request&, httplib::Response& res) {
        if (res.status == 404) json_response(res, 404, {{"ok", false}, {"message", "not found"}});
    });
}

json WebServer::telemetry_json() const {
    const auto t = controller_.telemetry();
    return {
        {"state", to_string(t.state)},
        {"session", to_string(t.session)},
        {"direction", to_string(t.direction)},
        {"speed_kmh", t.speed_kmh},
        {"target_speed_kmh", t.target_speed_kmh},
        {"incline_percent", t.incline_percent},
        {"target_incline_percent", t.target_incline_percent},
        {"distance_km", t.distance_km},
        {"elapsed_seconds", t.elapsed_seconds},
        {"energy_kcal_estimate", t.energy_kcal_estimate},
        {"mechanical_power_w_estimate", t.mechanical_power_w_estimate},
        {"heart_rate_bpm", t.heart_rate_bpm},
        {"max_heart_rate_bpm", t.max_heart_rate_bpm},
        {"interval_index", t.interval_index},
        {"interval_remaining_seconds", t.interval_remaining_seconds},
        {"active_program_id", t.active_program_id},
        {"fault", t.fault},
        {"notice", t.notice}
    };
}

json WebServer::protocol_json(const Protocol& protocol) const {
    json intervals = json::array();
    for (const auto& i : protocol.intervals) {
        intervals.push_back({
            {"duration_seconds", i.duration_seconds},
            {"speed_kmh", i.speed_kmh},
            {"incline_percent", i.incline_percent},
            {"acceleration_level", i.acceleration_level}
        });
    }
    return {
        {"id", protocol.id}, {"name", protocol.name}, {"description", protocol.description},
        {"implemented", protocol.implemented}, {"standard", protocol.standard}, {"intervals", intervals}
    };
}

Protocol WebServer::parse_profile(const json& value) const {
    Protocol p;
    p.id = value.value("id", "");
    p.name = value.value("name", "");
    p.description = value.value("description", "");
    p.implemented = true;
    p.standard = false;
    if (!valid_id(p.id)) throw std::runtime_error("profile id must match [A-Za-z0-9_-], max 64 chars");
    if (p.name.empty()) throw std::runtime_error("profile name is required");
    if (!value.contains("intervals") || !value.at("intervals").is_array()) throw std::runtime_error("intervals array is required");
    const auto limits = controller_.limits();
    for (const auto& item : value.at("intervals")) {
        Interval i;
        i.duration_seconds = item.value("duration_seconds", 0);
        i.speed_kmh = item.value("speed_kmh", 0.0);
        i.incline_percent = item.value("incline_percent", 0.0);
        i.acceleration_level = item.value("acceleration_level", 3);
        if (i.duration_seconds <= 0 || i.duration_seconds > 24 * 3600) throw std::runtime_error("invalid interval duration");
        if (i.speed_kmh < 0 || i.speed_kmh > limits.max_speed_kmh) throw std::runtime_error("interval speed exceeds limits");
        if (i.incline_percent < 0 || i.incline_percent > limits.max_incline_percent) throw std::runtime_error("interval incline exceeds limits");
        i.acceleration_level = std::clamp(i.acceleration_level, 1, 7);
        p.intervals.push_back(i);
    }
    if (p.intervals.empty()) throw std::runtime_error("at least one interval is required");
    return p;
}

json WebServer::read_array_file(const std::filesystem::path& path) const {
    if (!std::filesystem::exists(path)) return json::array();
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    json value;
    in >> value;
    if (!value.is_array()) throw std::runtime_error(path.string() + " must contain a JSON array");
    return value;
}

void WebServer::write_json_file(const std::filesystem::path& path, const json& value) const {
    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + tmp);
        out << value.dump(2) << '\n';
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::rename(tmp, path);
}

std::vector<Protocol> WebServer::load_profiles() const {
    std::lock_guard lock(storage_mutex_);
    std::vector<Protocol> result;
    for (const auto& item : read_array_file(profiles_file_)) result.push_back(parse_profile(item));
    return result;
}

void WebServer::save_profiles(const std::vector<Protocol>& profiles) const {
    std::lock_guard lock(storage_mutex_);
    json value = json::array();
    for (const auto& p : profiles) value.push_back(protocol_json(p));
    write_json_file(profiles_file_, value);
}

} // namespace yadro
