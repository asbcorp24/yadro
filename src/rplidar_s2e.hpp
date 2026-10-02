#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace yadro {

struct RplidarS2EConfig {
    bool enabled{false};
    std::string ip;
    int udp_port{0};

    // Geometry is intentionally unset by default. Safety logic is not armed
    // until the real treadmill dimensions and lidar pose are configured.
    double belt_length_m{0.0};
    double belt_width_m{0.0};
    double sensor_x_m{0.0};
    double sensor_y_m{0.0};
    double sensor_yaw_deg{0.0};

    double front_margin_m{0.30};
    double rear_margin_m{0.30};
    double side_margin_m{0.10};
    int prediction_steps{5};

    double cluster_link_m{0.12};
    int min_cluster_points{3};
    int scan_timeout_ms{500};

    double target_step_length_m{0.0};
    double speed_correction_step_kmh{0.2};

    // Never enabled by default. This is a software interlock only; it does not
    // replace the physical emergency-stop chain.
    bool actuation_enabled{false};
};

struct RplidarPoint {
    double angle_deg{0.0};
    double distance_m{0.0};
    int quality{0};
};

struct FootObservation {
    bool valid{false};
    double x_m{0.0};
    double y_m{0.0};
    std::size_t point_count{0};
};

struct RplidarS2ETelemetry {
    bool sdk_available{false};
    bool running{false};
    bool connected{false};
    bool scanning{false};
    std::string device_model{"RPLIDAR S2E"};
    std::string serial_number;
    std::string firmware;
    std::string health{"unknown"};
    std::string error;

    std::uint64_t scan_sequence{0};
    std::size_t raw_point_count{0};
    std::size_t filtered_point_count{0};
    double scan_age_ms{-1.0};

    FootObservation left;
    FootObservation right;

    double current_step_left_cm{0.0};
    double current_step_right_cm{0.0};
    double previous_step_left_cm{0.0};
    double previous_step_right_cm{0.0};
    double cycle_left_s{0.0};
    double cycle_right_s{0.0};
    double symmetry_time_percent{0.0};
    double symmetry_x_percent{0.0};
    double symmetry_y_percent{0.0};

    std::string safety_state{"not_configured"}; // not_configured/safe/warning/critical
    std::string safety_reason;
    bool geometry_configured{false};
    bool metrics_valid{false};
    double recommended_speed_delta_kmh{0.0};
};

class RplidarS2E {
public:
    RplidarS2E();
    ~RplidarS2E();

    RplidarS2E(const RplidarS2E&) = delete;
    RplidarS2E& operator=(const RplidarS2E&) = delete;

    void configure(const RplidarS2EConfig& config);
    RplidarS2EConfig config() const;

    bool start();
    void stop();
    bool running() const;

    RplidarS2ETelemetry snapshot() const;

    // Public for deterministic replay/tests and for future capture-file tools.
    void process_scan(const std::vector<RplidarPoint>& points);

private:
    struct Cluster {
        double x_m{0.0};
        double y_m{0.0};
        std::size_t count{0};
    };
    struct FootTrack {
        bool valid{false};
        double x_m{0.0};
        double y_m{0.0};
        double previous_x_m{0.0};
        double previous_y_m{0.0};
        double vx_m_s{0.0};
        double vy_m_s{0.0};
        std::chrono::steady_clock::time_point last_seen{};
        std::chrono::steady_clock::time_point last_land{};
        double previous_step_cm{0.0};
        double current_step_cm{0.0};
        double cycle_s{0.0};
        std::deque<double> step_x_history;
        std::deque<double> step_y_history;
        std::deque<double> cycle_history;
    };

    void worker();
    std::vector<Cluster> clusters_from_scan(const std::vector<RplidarPoint>& points,
                                            std::size_t& filtered_count) const;
    void update_tracks(const std::vector<Cluster>& clusters,
                       std::chrono::steady_clock::time_point now);
    void update_safety();
    void update_metrics();
    static double symmetry(double right, double left);

    mutable std::mutex mutex_;
    RplidarS2EConfig config_;
    RplidarS2ETelemetry telemetry_;
    FootTrack left_track_;
    FootTrack right_track_;
    std::atomic<bool> stop_requested_{false};
    std::thread worker_thread_;
    std::chrono::steady_clock::time_point last_scan_{};
};

} // namespace yadro
