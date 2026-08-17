#include "web_server.hpp"

#include "protocols.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
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

std::string now_iso8601() {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return out.str();
}

std::string generated_id(const char* prefix) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return std::string(prefix) + std::to_string(ms);
}

double array_number(const json& value, int index, double fallback) {
    if (!value.is_array() || index < 0 || index >= static_cast<int>(value.size())) return fallback;
    return value.at(index).is_number() ? value.at(index).get<double>() : fallback;
}

} // namespace

WebServer::WebServer(TreadmillController& controller,
                     std::filesystem::path data_dir,
                     std::filesystem::path static_dir)
    : controller_(controller),
      data_dir_(std::move(data_dir)),
      static_dir_(std::move(static_dir)),
      profiles_file_(data_dir_ / "profiles.json"),
      user_protocols_file_(data_dir_ / "user_protocols.json"),
      patients_file_(data_dir_ / "patients.json"),
      history_file_(data_dir_ / "history.json"),
      settings_file_(data_dir_ / "settings.json"),
      standard_protocols_(yadro::standard_protocols()) {
    std::filesystem::create_directories(data_dir_);
    const auto settings = load_settings();
    active_patient_id_ = settings.value("active_patient_id", "");
    apply_runtime_settings(settings);
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
        json_response(res, 200, {{"ok", true}, {"service", "yadro"}, {"version", "0.2.0"}, {"driver", "simulation"}});
    });

    server_.Get("/api/v1/state", [&](const httplib::Request&, httplib::Response& res) {
        const auto t = controller_.telemetry();
        update_session_tracking(t);
        json_response(res, 200, {{"ok", true}, {"telemetry", telemetry_json()}, {"active_patient_id", active_patient()}});
    });

    server_.Post("/api/v1/heartbeat", [&](const httplib::Request&, httplib::Response& res) {
        controller_.heartbeat();
        json_response(res, 200, {{"ok", true}});
    });

    server_.Post("/api/v1/control/targets", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            const auto body = json::parse(req.body);
            const auto current = controller_.telemetry();
            const auto result = controller_.set_targets(
                body.value("speed_kmh", current.target_speed_kmh),
                body.value("incline_percent", current.target_incline_percent));
            json_response(res, result.ok ? 200 : 400, command_json(result));
        } catch (const std::exception& e) { json_response(res, 400, {{"ok", false}, {"message", e.what()}}); }
    });

    server_.Post("/api/v1/control/direction", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            const auto body = json::parse(req.body);
            const auto result = controller_.set_direction(body.value("direction", "forward") == "reverse" ? Direction::Reverse : Direction::Forward);
            json_response(res, result.ok ? 200 : 400, command_json(result));
        } catch (const std::exception& e) { json_response(res, 400, {{"ok", false}, {"message", e.what()}}); }
    });

    server_.Post("/api/v1/control/start", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            const auto body = req.body.empty() ? json::object() : json::parse(req.body);
            const std::string mode = body.value("mode", "free");
            CommandResult result;
            if (mode == "free") {
                result = controller_.start_free();
            } else if (mode == "heart_rate") {
                HeartRateProgram p;
                p.min_bpm = body.value("min_bpm", 100);
                p.max_bpm = body.value("max_bpm", 130);
                p.min_speed_kmh = body.value("min_speed_kmh", 1.0);
                p.max_speed_kmh = body.value("max_speed_kmh", 6.0);
                p.incline_percent = body.value("incline_percent", 0.0);
                p.speed_step_kmh = body.value("speed_step_kmh", 0.2);
                p.adjust_period_seconds = body.value("adjust_period_seconds", 5.0);
                result = controller_.start_heart_rate(p);
            } else {
                const std::string id = body.value("program_id", "");
                std::optional<Protocol> program;
                SessionKind kind = SessionKind::Protocol;
                if (mode == "protocol") program = find_protocol(standard_protocols_, id);
                else if (mode == "user_protocol") program = find_protocol(load_user_protocols(), id);
                else if (mode == "profile") { program = find_protocol(load_profiles(), id); kind = SessionKind::Profile; }
                else { json_response(res, 400, command_json({false, "unknown start mode"})); return; }
                result = program ? controller_.start_program(*program, kind) : CommandResult{false, "program not found"};
            }
            if (result.ok) update_session_tracking(controller_.telemetry());
            json_response(res, result.ok ? 200 : 400, command_json(result));
        } catch (const std::exception& e) { json_response(res, 400, {{"ok", false}, {"message", e.what()}}); }
    });

    server_.Post("/api/v1/control/stop", [&](const httplib::Request&, httplib::Response& res) {
        update_session_tracking(controller_.telemetry());
        const auto result = controller_.stop();
        update_session_tracking(controller_.telemetry());
        json_response(res, 200, command_json(result));
    });

    server_.Post("/api/v1/control/emergency-stop", [&](const httplib::Request&, httplib::Response& res) {
        update_session_tracking(controller_.telemetry());
        const auto result = controller_.emergency_stop();
        update_session_tracking(controller_.telemetry());
        json_response(res, 200, command_json(result));
    });

    server_.Post("/api/v1/control/reset-emergency", [&](const httplib::Request&, httplib::Response& res) {
        const auto result = controller_.reset_emergency();
        json_response(res, result.ok ? 200 : 400, command_json(result));
    });

    server_.Post("/api/v1/simulation/heart-rate", [&](const httplib::Request& req, httplib::Response& res) {
        try { controller_.set_heart_rate(json::parse(req.body).value("bpm", 0)); json_response(res, 200, {{"ok", true}}); }
        catch (const std::exception& e) { json_response(res, 400, {{"ok", false}, {"message", e.what()}}); }
    });

    server_.Get("/api/v1/protocols", [&](const httplib::Request&, httplib::Response& res) {
        json items = json::array();
        for (const auto& p : standard_protocols_) items.push_back(protocol_json(p));
        json_response(res, 200, {{"ok", true}, {"items", items}});
    });

    auto register_program_crud = [&](const std::string& base, const std::filesystem::path& file, bool profiles) {
        server_.Get(base, [&, file, profiles](const httplib::Request&, httplib::Response& res) {
            json items = json::array();
            for (const auto& p : profiles ? load_profiles() : load_user_protocols()) items.push_back(protocol_json(p));
            json_response(res, 200, {{"ok", true}, {"items", items}});
        });
        server_.Post(base, [&, file](const httplib::Request& req, httplib::Response& res) {
            try {
                auto item = parse_profile(json::parse(req.body));
                auto items = load_protocol_file(file);
                auto it = std::find_if(items.begin(), items.end(), [&](const Protocol& p) { return p.id == item.id; });
                if (it == items.end()) items.push_back(item); else *it = item;
                save_protocol_file(file, items);
                json_response(res, 200, {{"ok", true}, {"item", protocol_json(item)}});
            } catch (const std::exception& e) { json_response(res, 400, {{"ok", false}, {"message", e.what()}}); }
        });
    };
    register_program_crud("/api/v1/profiles", profiles_file_, true);
    register_program_crud("/api/v1/user-protocols", user_protocols_file_, false);

    server_.Delete(R"(/api/v1/profiles/([A-Za-z0-9_-]+))", [&](const httplib::Request& req, httplib::Response& res) {
        auto items = load_profiles(); const std::string id = req.matches[1]; const auto before = items.size();
        items.erase(std::remove_if(items.begin(), items.end(), [&](const Protocol& p){ return p.id == id; }), items.end());
        if (items.size() == before) { json_response(res, 404, {{"ok", false}, {"message", "profile not found"}}); return; }
        save_protocol_file(profiles_file_, items); json_response(res, 200, {{"ok", true}});
    });
    server_.Delete(R"(/api/v1/user-protocols/([A-Za-z0-9_-]+))", [&](const httplib::Request& req, httplib::Response& res) {
        auto items = load_user_protocols(); const std::string id = req.matches[1]; const auto before = items.size();
        items.erase(std::remove_if(items.begin(), items.end(), [&](const Protocol& p){ return p.id == id; }), items.end());
        if (items.size() == before) { json_response(res, 404, {{"ok", false}, {"message", "protocol not found"}}); return; }
        save_protocol_file(user_protocols_file_, items); json_response(res, 200, {{"ok", true}});
    });

    server_.Get("/api/v1/patients", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard lock(storage_mutex_); json_response(res, 200, {{"ok", true}, {"items", read_array_file(patients_file_)}});
    });
    server_.Post("/api/v1/patients", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            auto patient = json::parse(req.body); const std::string id = patient.value("id", "");
            if (!valid_id(id)) throw std::runtime_error("patient id must match [A-Za-z0-9_-], max 64 chars");
            std::lock_guard lock(storage_mutex_); auto items = read_array_file(patients_file_); bool replaced = false;
            for (auto& row : items) if (row.value("id", "") == id) { row = patient; replaced = true; break; }
            if (!replaced) items.push_back(patient); write_json_file(patients_file_, items);
            json_response(res, 200, {{"ok", true}, {"patient", patient}});
        } catch (const std::exception& e) { json_response(res, 400, {{"ok", false}, {"message", e.what()}}); }
    });
    server_.Delete(R"(/api/v1/patients/([A-Za-z0-9_-]+))", [&](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.matches[1]; std::lock_guard lock(storage_mutex_); auto items = read_array_file(patients_file_); const auto before = items.size();
        items.erase(std::remove_if(items.begin(), items.end(), [&](const json& p){ return p.value("id", "") == id; }), items.end());
        if (items.size() == before) { json_response(res, 404, {{"ok", false}, {"message", "patient not found"}}); return; }
        write_json_file(patients_file_, items); json_response(res, 200, {{"ok", true}});
    });

    server_.Get("/api/v1/session/patient", [&](const httplib::Request&, httplib::Response& res) {
        json_response(res, 200, {{"ok", true}, {"patient_id", active_patient()}});
    });
    server_.Post("/api/v1/session/patient", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            const std::string id = json::parse(req.body).value("patient_id", "");
            if (!id.empty() && !valid_id(id)) throw std::runtime_error("invalid patient id");
            set_active_patient(id); auto settings = load_settings(); settings["active_patient_id"] = id; save_settings(settings);
            json_response(res, 200, {{"ok", true}, {"patient_id", id}});
        } catch (const std::exception& e) { json_response(res, 400, {{"ok", false}, {"message", e.what()}}); }
    });

    server_.Get("/api/v1/history", [&](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard lock(storage_mutex_); const auto all = read_array_file(history_file_); json items = json::array();
        const auto patient = req.has_param("patient_id") ? req.get_param_value("patient_id") : "";
        for (const auto& row : all) if (patient.empty() || row.value("patient_id", "") == patient) items.push_back(row);
        json_response(res, 200, {{"ok", true}, {"items", items}});
    });
    server_.Delete(R"(/api/v1/history/([A-Za-z0-9_-]+))", [&](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.matches[1]; std::lock_guard lock(storage_mutex_); auto items = read_array_file(history_file_); const auto before = items.size();
        items.erase(std::remove_if(items.begin(), items.end(), [&](const json& r){ return r.value("id", "") == id; }), items.end());
        if (items.size() == before) { json_response(res, 404, {{"ok", false}, {"message", "history row not found"}}); return; }
        write_json_file(history_file_, items); json_response(res, 200, {{"ok", true}});
    });

    server_.Get("/api/v1/settings", [&](const httplib::Request&, httplib::Response& res) {
        json_response(res, 200, {{"ok", true}, {"settings", load_settings()}});
    });
    server_.Post("/api/v1/settings", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            const auto patch = json::parse(req.body); auto settings = load_settings(); settings.merge_patch(patch);
            if (patch.contains("limits") || patch.contains("motion")) {
                const auto result = apply_runtime_settings(settings); if (!result.ok) { json_response(res, 400, command_json(result)); return; }
            }
            save_settings(settings); set_active_patient(settings.value("active_patient_id", active_patient()));
            json_response(res, 200, {{"ok", true}, {"settings", settings}});
        } catch (const std::exception& e) { json_response(res, 400, {{"ok", false}, {"message", e.what()}}); }
    });

    server_.Post("/api/v1/calibration/zero", [&](const httplib::Request&, httplib::Response& res) {
        const auto t = controller_.telemetry(); auto settings = load_settings(); settings["calibration"]["incline_zero_offset_percent"] = -t.incline_percent; save_settings(settings);
        json_response(res, 200, {{"ok", true}, {"offset_percent", -t.incline_percent}});
    });

    server_.Post("/api/v1/hr/scan", [&](const httplib::Request&, httplib::Response& res) {
        json devices = json::array(); devices.push_back({{"id", "sim_hr_01"}, {"name", "Simulation HR sensor"}, {"signal", 100}, {"simulated", true}});
        json_response(res, 200, {{"ok", true}, {"items", devices}});
    });
    server_.Get("/api/v1/hr/status", [&](const httplib::Request&, httplib::Response& res) {
        const auto s = load_settings(); json_response(res, 200, {{"ok", true}, {"sensor", s.value("heart_rate_sensor", json::object())}, {"bpm", controller_.telemetry().heart_rate_bpm}});
    });
    server_.Post("/api/v1/hr/connect", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            const auto body = json::parse(req.body); auto s = load_settings();
            s["heart_rate_sensor"] = {{"connected", true}, {"id", body.value("id", "sim_hr_01")}, {"name", body.value("name", "Simulation HR sensor")}, {"simulated", true}};
            save_settings(s); json_response(res, 200, {{"ok", true}, {"sensor", s["heart_rate_sensor"]}});
        } catch (const std::exception& e) { json_response(res, 400, {{"ok", false}, {"message", e.what()}}); }
    });
    server_.Post("/api/v1/hr/disconnect", [&](const httplib::Request&, httplib::Response& res) {
        auto s = load_settings(); s["heart_rate_sensor"] = {{"connected", false}, {"id", ""}, {"name", ""}, {"simulated", true}}; save_settings(s);
        controller_.set_heart_rate(0); json_response(res, 200, {{"ok", true}});
    });

    server_.Get("/api/v1/export", [&](const httplib::Request&, httplib::Response& res) {
        json payload; {
            std::lock_guard lock(storage_mutex_);
            payload = {{"format", "yadro-export-v1"}, {"exported_at", now_iso8601()}, {"patients", read_array_file(patients_file_)},
                       {"profiles", read_array_file(profiles_file_)}, {"user_protocols", read_array_file(user_protocols_file_)},
                       {"history", read_array_file(history_file_)}, {"settings", read_json_file(settings_file_, default_settings())}};
        }
        res.set_content(payload.dump(2), "application/json; charset=utf-8");
        res.set_header("Content-Disposition", "attachment; filename=yadro-export.json");
    });
    server_.Post("/api/v1/import", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            const auto payload = json::parse(req.body); if (payload.value("format", "") != "yadro-export-v1") throw std::runtime_error("unsupported import format");
            {
                std::lock_guard lock(storage_mutex_);
                if (payload.contains("patients")) write_json_file(patients_file_, payload.at("patients"));
                if (payload.contains("profiles")) write_json_file(profiles_file_, payload.at("profiles"));
                if (payload.contains("user_protocols")) write_json_file(user_protocols_file_, payload.at("user_protocols"));
                if (payload.contains("history")) write_json_file(history_file_, payload.at("history"));
                if (payload.contains("settings")) write_json_file(settings_file_, payload.at("settings"));
            }
            const auto settings = load_settings(); set_active_patient(settings.value("active_patient_id", ""));
            const auto runtime = apply_runtime_settings(settings); if (!runtime.ok) { json_response(res, 400, command_json(runtime)); return; }
            json_response(res, 200, {{"ok", true}});
        } catch (const std::exception& e) { json_response(res, 400, {{"ok", false}, {"message", e.what()}}); }
    });

    server_.Get("/api/v1/config", [&](const httplib::Request&, httplib::Response& res) {
        const auto l = controller_.limits();
        json_response(res, 200, {{"ok", true}, {"limits", {{"max_speed_kmh", l.max_speed_kmh}, {"max_incline_percent", l.max_incline_percent},
            {"max_accel_kmh_per_s", l.max_accel_kmh_per_s}, {"max_decel_kmh_per_s", l.max_decel_kmh_per_s}, {"watchdog_seconds", l.watchdog_seconds}}}});
    });

    server_.set_error_handler([](const httplib::Request&, httplib::Response& res) {
        if (res.status == 404) json_response(res, 404, {{"ok", false}, {"message", "not found"}});
    });
}

json WebServer::telemetry_json() const {
    const auto t = controller_.telemetry();
    const auto settings = load_settings();
    const double offset = settings.value("calibration", json::object()).value("incline_zero_offset_percent", 0.0);
    return {{"state", to_string(t.state)}, {"session", to_string(t.session)}, {"direction", to_string(t.direction)},
        {"speed_kmh", t.speed_kmh}, {"target_speed_kmh", t.target_speed_kmh}, {"incline_percent", t.incline_percent + offset},
        {"raw_incline_percent", t.incline_percent}, {"target_incline_percent", t.target_incline_percent}, {"distance_km", t.distance_km},
        {"elapsed_seconds", t.elapsed_seconds}, {"energy_kcal_estimate", t.energy_kcal_estimate}, {"mechanical_power_w_estimate", t.mechanical_power_w_estimate},
        {"heart_rate_bpm", t.heart_rate_bpm}, {"max_heart_rate_bpm", t.max_heart_rate_bpm}, {"interval_index", t.interval_index},
        {"interval_remaining_seconds", t.interval_remaining_seconds}, {"active_program_id", t.active_program_id}, {"fault", t.fault}, {"notice", t.notice}};
}

json WebServer::protocol_json(const Protocol& protocol) const {
    json intervals = json::array();
    for (const auto& i : protocol.intervals) intervals.push_back({{"duration_seconds", i.duration_seconds}, {"speed_kmh", i.speed_kmh}, {"incline_percent", i.incline_percent}, {"acceleration_level", i.acceleration_level}});
    return {{"id", protocol.id}, {"name", protocol.name}, {"description", protocol.description}, {"implemented", protocol.implemented}, {"standard", protocol.standard}, {"intervals", intervals}};
}

Protocol WebServer::parse_profile(const json& value) const {
    Protocol p; p.id = value.value("id", ""); p.name = value.value("name", ""); p.description = value.value("description", ""); p.implemented = true; p.standard = false;
    if (!valid_id(p.id)) throw std::runtime_error("id must match [A-Za-z0-9_-], max 64 chars");
    if (p.name.empty()) throw std::runtime_error("name is required");
    if (!value.contains("intervals") || !value.at("intervals").is_array()) throw std::runtime_error("intervals array is required");
    const auto limits = controller_.limits();
    for (const auto& item : value.at("intervals")) {
        Interval i; i.duration_seconds = item.value("duration_seconds", 0); i.speed_kmh = item.value("speed_kmh", 0.0); i.incline_percent = item.value("incline_percent", 0.0); i.acceleration_level = std::clamp(item.value("acceleration_level", 3), 1, 7);
        if (i.duration_seconds <= 0 || i.duration_seconds > 24 * 3600) throw std::runtime_error("invalid interval duration");
        if (i.speed_kmh < 0 || i.speed_kmh > limits.max_speed_kmh) throw std::runtime_error("interval speed exceeds limits");
        if (i.incline_percent < 0 || i.incline_percent > limits.max_incline_percent) throw std::runtime_error("interval incline exceeds limits");
        p.intervals.push_back(i);
    }
    if (p.intervals.empty()) throw std::runtime_error("at least one interval is required");
    return p;
}

json WebServer::read_json_file(const std::filesystem::path& path, const json& fallback) const {
    if (!std::filesystem::exists(path)) return fallback;
    std::ifstream in(path, std::ios::binary); if (!in) throw std::runtime_error("cannot read " + path.string()); json value; in >> value; return value;
}
json WebServer::read_array_file(const std::filesystem::path& path) const {
    const auto value = read_json_file(path, json::array()); if (!value.is_array()) throw std::runtime_error(path.string() + " must contain a JSON array"); return value;
}
void WebServer::write_json_file(const std::filesystem::path& path, const json& value) const {
    const auto tmp = path.string() + ".tmp"; { std::ofstream out(tmp, std::ios::binary | std::ios::trunc); if (!out) throw std::runtime_error("cannot write " + tmp); out << value.dump(2) << '\n'; }
    std::error_code ec; std::filesystem::remove(path, ec); std::filesystem::rename(tmp, path);
}

std::vector<Protocol> WebServer::load_protocol_file(const std::filesystem::path& path) const {
    std::lock_guard lock(storage_mutex_); std::vector<Protocol> result; for (const auto& item : read_array_file(path)) result.push_back(parse_profile(item)); return result;
}
void WebServer::save_protocol_file(const std::filesystem::path& path, const std::vector<Protocol>& protocols) const {
    std::lock_guard lock(storage_mutex_); json value = json::array(); for (const auto& p : protocols) value.push_back(protocol_json(p)); write_json_file(path, value);
}
std::vector<Protocol> WebServer::load_profiles() const { return load_protocol_file(profiles_file_); }
std::vector<Protocol> WebServer::load_user_protocols() const { return load_protocol_file(user_protocols_file_); }

json WebServer::default_settings() const {
    return {{"active_patient_id", ""}, {"limits", {{"max_speed_kmh", 20.0}, {"max_incline_percent", 25.0}, {"watchdog_seconds", 5.0}}},
        {"motion", {{"acceleration_times_seconds", json::array({64.0,48.0,32.0,16.0,12.0,8.0,4.0})}, {"braking_times_seconds", json::array({64.0,48.0,32.0,16.0,12.0,8.0,4.0})}, {"acceleration_level", 4}, {"braking_level", 4}}},
        {"calibration", {{"incline_zero_offset_percent", 0.0}}}, {"units", {{"distance", "km"}, {"speed", "kmh"}, {"angle", "percent"}, {"energy", "kcal"}, {"power", "kw"}, {"aerobic", "ml_min_kg"}}},
        {"ui", {{"font_scale", 1.0}, {"high_contrast", false}, {"compact", false}}}, {"security", {{"pin_required", false}, {"operator_pin_sha256", ""}}},
        {"heart_rate_sensor", {{"connected", false}, {"id", ""}, {"name", ""}, {"simulated", true}}}};
}
json WebServer::load_settings() const {
    std::lock_guard lock(storage_mutex_); auto settings = read_json_file(settings_file_, default_settings()); if (!settings.is_object()) settings = default_settings(); auto defaults = default_settings(); defaults.merge_patch(settings); return defaults;
}
void WebServer::save_settings(const json& settings) const { std::lock_guard lock(storage_mutex_); write_json_file(settings_file_, settings); }

CommandResult WebServer::apply_runtime_settings(const json& settings) {
    auto limits = controller_.limits(); const auto ls = settings.value("limits", json::object()); limits.max_speed_kmh = ls.value("max_speed_kmh", limits.max_speed_kmh); limits.max_incline_percent = ls.value("max_incline_percent", limits.max_incline_percent); limits.watchdog_seconds = ls.value("watchdog_seconds", limits.watchdog_seconds);
    const auto motion = settings.value("motion", json::object()); const int a = std::clamp(motion.value("acceleration_level", 4), 1, 7) - 1; const int b = std::clamp(motion.value("braking_level", 4), 1, 7) - 1;
    const double at = std::max(1.0, array_number(motion.value("acceleration_times_seconds", json::array()), a, 16.0)); const double bt = std::max(1.0, array_number(motion.value("braking_times_seconds", json::array()), b, 16.0));
    limits.max_accel_kmh_per_s = 10.0 / at; limits.max_decel_kmh_per_s = 10.0 / bt; return controller_.update_limits(limits);
}

std::string WebServer::active_patient() const { std::lock_guard lock(session_mutex_); return active_patient_id_; }
void WebServer::set_active_patient(std::string patient_id) { std::lock_guard lock(session_mutex_); active_patient_id_ = std::move(patient_id); }

void WebServer::begin_session_tracking(const Telemetry& t) {
    session_tracker_ = {}; session_tracker_.active = true; session_tracker_.id = generated_id("session_"); session_tracker_.patient_id = active_patient_id_; session_tracker_.session = to_string(t.session); session_tracker_.program_id = t.active_program_id; session_tracker_.started_at = now_iso8601(); session_tracker_.max_speed_kmh = t.speed_kmh; session_tracker_.max_incline_percent = t.incline_percent; session_tracker_.max_power_w = t.mechanical_power_w_estimate; session_tracker_.max_heart_rate_bpm = t.heart_rate_bpm; session_tracker_.previous_speed_kmh = t.speed_kmh; session_tracker_.previous_sample = std::chrono::steady_clock::now();
}
void WebServer::finalize_session_tracking(const Telemetry& t) {
    json row = {{"id", session_tracker_.id}, {"patient_id", session_tracker_.patient_id}, {"session", session_tracker_.session}, {"program_id", session_tracker_.program_id}, {"started_at", session_tracker_.started_at}, {"finished_at", now_iso8601()}, {"duration_seconds", t.elapsed_seconds}, {"distance_km", t.distance_km}, {"average_speed_kmh", t.elapsed_seconds > 0.0 ? t.distance_km / (t.elapsed_seconds / 3600.0) : 0.0}, {"max_speed_kmh", session_tracker_.max_speed_kmh}, {"max_acceleration_m_s2", session_tracker_.max_acceleration_m_s2}, {"max_incline_percent", session_tracker_.max_incline_percent}, {"max_heart_rate_bpm", session_tracker_.max_heart_rate_bpm}, {"energy_kcal_estimate", t.energy_kcal_estimate}, {"max_power_w_estimate", session_tracker_.max_power_w}, {"notice", t.notice}};
    { std::lock_guard lock(storage_mutex_); auto history = read_array_file(history_file_); history.push_back(row); write_json_file(history_file_, history); } session_tracker_ = {};
}
void WebServer::update_session_tracking(const Telemetry& t) {
    std::lock_guard lock(session_mutex_); const bool active_now = t.session != SessionKind::None; if (!session_tracker_.active && active_now) begin_session_tracking(t); if (!session_tracker_.active) return;
    const auto now = std::chrono::steady_clock::now(); const std::chrono::duration<double> dt = now - session_tracker_.previous_sample; if (dt.count() > 0.02) { const double dv_ms = (t.speed_kmh - session_tracker_.previous_speed_kmh) / 3.6; session_tracker_.max_acceleration_m_s2 = std::max(session_tracker_.max_acceleration_m_s2, std::abs(dv_ms / dt.count())); session_tracker_.previous_speed_kmh = t.speed_kmh; session_tracker_.previous_sample = now; }
    session_tracker_.max_speed_kmh = std::max(session_tracker_.max_speed_kmh, t.speed_kmh); session_tracker_.max_incline_percent = std::max(session_tracker_.max_incline_percent, t.incline_percent); session_tracker_.max_power_w = std::max(session_tracker_.max_power_w, t.mechanical_power_w_estimate); session_tracker_.max_heart_rate_bpm = std::max(session_tracker_.max_heart_rate_bpm, t.heart_rate_bpm); if (!active_now) finalize_session_tracking(t);
}

} // namespace yadro
