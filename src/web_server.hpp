#pragma once

#include "treadmill.hpp"

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
    void register_routes();
    nlohmann::json telemetry_json() const;
    nlohmann::json protocol_json(const Protocol& protocol) const;
    Protocol parse_profile(const nlohmann::json& value) const;
    nlohmann::json read_array_file(const std::filesystem::path& path) const;
    void write_json_file(const std::filesystem::path& path, const nlohmann::json& value) const;
    std::vector<Protocol> load_profiles() const;
    void save_profiles(const std::vector<Protocol>& profiles) const;

    TreadmillController& controller_;
    std::filesystem::path data_dir_;
    std::filesystem::path static_dir_;
    std::filesystem::path profiles_file_;
    std::filesystem::path patients_file_;
    std::vector<Protocol> standard_protocols_;
    mutable std::mutex storage_mutex_;
    httplib::Server server_;
};

} // namespace yadro
