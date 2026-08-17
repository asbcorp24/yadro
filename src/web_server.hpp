#pragma once

#include "treadmill.hpp"

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace yadro {

class WebServer {
public:
    WebServer(TreadmillController& controller,
              std::filesystem::path data_dir,
              std::filesystem::path static_dir);

    bool listen(const std::string& bind_address, int port);
    void stop();

private:
    struct SessionTracker {
        bool active{false};
        std::string id;
        std::string patient_id;
        std::string session;
        std::string program_id;
        std::string started_at;
        double max_speed_kmh{0.0};
        double max_incline_percent{0.0};
        double max_power_w{0.0};
        double max_acceleration_m_s2{0.0};
        int max_heart_rate_bpm{0};
        double previous_speed_kmh{0.0};
        std::chrono::steady_clock::time_point previous_sample{};
    };

    void register_routes();
    nlohmann::json telemetry_json() const;
    nlohmann::json protocol_json(const Protocol& protocol) const;
    Protocol parse_profile(const nlohmann::json& value) const;

    nlohmann::json read_json_file(const std::filesystem::path& path, const nlohmann::json& fallback) const;
    nlohmann::json read_array_file(const std::filesystem::path& path) const;
    void write_json_file(const std::filesystem::path& path, const nlohmann::json& value) const;

    std::vector<Protocol> load_protocol_file(const std::filesystem::path& path) const;
    void save_protocol_file(const std::filesystem::path& path, const std::vector<Protocol>& protocols) const;
    std::vector<Protocol> load_profiles() const;
    std::vector<Protocol> load_user_protocols() const;

    nlohmann::json default_settings() const;
    nlohmann::json load_settings() const;
    void save_settings(const nlohmann::json& settings) const;
    CommandResult apply_runtime_settings(const nlohmann::json& settings);

    void update_session_tracking(const Telemetry& telemetry);
    void begin_session_tracking(const Telemetry& telemetry);
    void finalize_session_tracking(const Telemetry& telemetry);
    std::string active_patient() const;
    void set_active_patient(std::string patient_id);

    TreadmillController& controller_;
    std::filesystem::path data_dir_;
    std::filesystem::path static_dir_;
    std::filesystem::path profiles_file_;
    std::filesystem::path user_protocols_file_;
    std::filesystem::path patients_file_;
    std::filesystem::path history_file_;
    std::filesystem::path settings_file_;
    std::vector<Protocol> standard_protocols_;

    mutable std::mutex storage_mutex_;
    mutable std::mutex session_mutex_;
    SessionTracker session_tracker_;
    std::string active_patient_id_;
    httplib::Server server_;
};

} // namespace yadro
