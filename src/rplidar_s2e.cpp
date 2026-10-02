#include "rplidar_s2e.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

#if defined(YADRO_RPLIDAR_S2E_SDK)
#include "sl_lidar.h"
#include "sl_lidar_driver.h"
#endif

namespace yadro {
namespace {
constexpr double kPi = 3.14159265358979323846;

double average(const std::deque<double>& values) {
    if (values.empty()) return 0.0;
    double sum = 0.0;
    for (double v : values) sum += v;
    return sum / static_cast<double>(values.size());
}

void push_recent(std::deque<double>& values, double value, std::size_t limit = 5) {
    values.push_back(value);
    while (values.size() > limit) values.pop_front();
}

std::string hex_serial(const unsigned char* bytes, std::size_t count) {
    std::ostringstream out;
    out << std::hex << std::uppercase << std::setfill('0');
    for (std::size_t i = 0; i < count; ++i) out << std::setw(2) << static_cast<unsigned>(bytes[i]);
    return out.str();
}
} // namespace

RplidarS2E::RplidarS2E() {
#if defined(YADRO_RPLIDAR_S2E_SDK)
    telemetry_.sdk_available = true;
#endif
}

RplidarS2E::~RplidarS2E() { stop(); }

void RplidarS2E::configure(const RplidarS2EConfig& config) {
    const bool restart = running();
    if (restart) stop();
    {
        std::lock_guard lock(mutex_);
        config_ = config;
        telemetry_.geometry_configured =
            config_.belt_length_m > 0.0 &&
            config_.belt_width_m > 0.0 &&
            config_.front_margin_m >= 0.0 &&
            config_.rear_margin_m >= 0.0 &&
            config_.side_margin_m >= 0.0 &&
            config_.front_margin_m + config_.rear_margin_m < config_.belt_length_m &&
            2.0 * config_.side_margin_m < config_.belt_width_m;
        if (!telemetry_.geometry_configured) {
            telemetry_.safety_state = "not_configured";
            telemetry_.safety_reason = "Не заданы реальные размеры полотна и зоны безопасности";
        }
    }
    if (restart && config.enabled) start();
}

RplidarS2EConfig RplidarS2E::config() const {
    std::lock_guard lock(mutex_);
    return config_;
}

bool RplidarS2E::start() {
    stop();
    {
        std::lock_guard lock(mutex_);
        telemetry_.error.clear();
        if (!config_.enabled) {
            telemetry_.error = "RPLIDAR S2E выключен в настройках";
            return false;
        }
        if (config_.ip.empty() || config_.udp_port <= 0 || config_.udp_port > 65535) {
            telemetry_.error = "Не заданы IP-адрес и UDP-порт RPLIDAR S2E";
            return false;
        }
#if !defined(YADRO_RPLIDAR_S2E_SDK)
        telemetry_.error = "Поддержка SLAMTEC SDK не включена при сборке";
        telemetry_.sdk_available = false;
        return false;
#else
        telemetry_.sdk_available = true;
#endif
    }
#if defined(YADRO_RPLIDAR_S2E_SDK)
    stop_requested_ = false;
    worker_thread_ = std::thread(&RplidarS2E::worker, this);
    return true;
#else
    return false;
#endif
}

void RplidarS2E::stop() {
    stop_requested_ = true;
    if (worker_thread_.joinable()) worker_thread_.join();
    std::lock_guard lock(mutex_);
    telemetry_.running = false;
    telemetry_.connected = false;
    telemetry_.scanning = false;
}

bool RplidarS2E::running() const {
    std::lock_guard lock(mutex_);
    return telemetry_.running;
}

RplidarS2ETelemetry RplidarS2E::snapshot() const {
    std::lock_guard lock(mutex_);
    auto result = telemetry_;
    if (last_scan_ != std::chrono::steady_clock::time_point{}) {
        result.scan_age_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - last_scan_).count();
    }
    return result;
}

std::vector<RplidarPoint> RplidarS2E::point_snapshot() const {
    std::lock_guard lock(mutex_);
    return latest_points_;
}

std::vector<RplidarS2E::Cluster> RplidarS2E::clusters_from_scan(
    const std::vector<RplidarPoint>& points,
    std::size_t& filtered_count) const {

    RplidarS2EConfig cfg;
    {
        std::lock_guard lock(mutex_);
        cfg = config_;
    }

    struct XY { double x; double y; };
    std::vector<XY> filtered;
    filtered.reserve(points.size());

    const double yaw = cfg.sensor_yaw_deg * kPi / 180.0;
    const double cy = std::cos(yaw);
    const double sy = std::sin(yaw);

    for (const auto& p : points) {
        if (!(p.distance_m > 0.03) || !std::isfinite(p.distance_m)) continue;
        const double a = p.angle_deg * kPi / 180.0;
        const double lx = p.distance_m * std::cos(a);
        const double ly = p.distance_m * std::sin(a);
        const double x = cfg.sensor_x_m + lx * cy - ly * sy;
        const double y = cfg.sensor_y_m + lx * sy + ly * cy;

        if (cfg.belt_length_m > 0.0 && (x < 0.0 || x > cfg.belt_length_m)) continue;
        if (cfg.belt_width_m > 0.0 && std::abs(y) > cfg.belt_width_m * 0.5) continue;
        filtered.push_back({x, y});
    }
    filtered_count = filtered.size();

    std::vector<Cluster> clusters;
    if (filtered.empty()) return clusters;

    Cluster current{};
    double sum_x = 0.0, sum_y = 0.0;
    XY previous = filtered.front();

    auto finish = [&]() {
        if (current.count >= static_cast<std::size_t>(std::max(1, cfg.min_cluster_points))) {
            current.x_m = sum_x / static_cast<double>(current.count);
            current.y_m = sum_y / static_cast<double>(current.count);
            clusters.push_back(current);
        }
        current = {};
        sum_x = sum_y = 0.0;
    };

    for (std::size_t i = 0; i < filtered.size(); ++i) {
        const auto& point = filtered[i];
        const double distance = i == 0 ? 0.0 : std::hypot(point.x - previous.x, point.y - previous.y);
        if (i != 0 && distance > std::max(0.03, cfg.cluster_link_m)) finish();
        ++current.count;
        sum_x += point.x;
        sum_y += point.y;
        previous = point;
    }
    finish();

    std::sort(clusters.begin(), clusters.end(), [](const Cluster& a, const Cluster& b) {
        return a.count > b.count;
    });
    if (clusters.size() > 8) clusters.resize(8);
    return clusters;
}

void RplidarS2E::update_tracks(const std::vector<Cluster>& clusters,
                               std::chrono::steady_clock::time_point now) {
    std::vector<Cluster> candidates = clusters;
    if (candidates.size() > 2) candidates.resize(2);

    FootObservation left{}, right{};
    if (candidates.size() == 1) {
        if (candidates[0].y_m >= 0.0) {
            left = {true, candidates[0].x_m, candidates[0].y_m, candidates[0].count};
        } else {
            right = {true, candidates[0].x_m, candidates[0].y_m, candidates[0].count};
        }
    } else if (candidates.size() >= 2) {
        std::sort(candidates.begin(), candidates.end(), [](const Cluster& a, const Cluster& b) {
            return a.y_m > b.y_m;
        });
        left = {true, candidates[0].x_m, candidates[0].y_m, candidates[0].count};
        right = {true, candidates[1].x_m, candidates[1].y_m, candidates[1].count};
    }

    auto update = [&](FootTrack& track, const FootObservation& obs) {
        if (!obs.valid) {
            track.valid = false;
            return;
        }

        const bool had_previous = track.last_seen != std::chrono::steady_clock::time_point{};
        const double dt = had_previous
            ? std::chrono::duration<double>(now - track.last_seen).count()
            : 0.0;

        track.previous_x_m = track.x_m;
        track.previous_y_m = track.y_m;
        if (had_previous && dt > 0.005 && dt < 1.0) {
            track.vx_m_s = (obs.x_m - track.x_m) / dt;
            track.vy_m_s = (obs.y_m - track.y_m) / dt;
        } else {
            track.vx_m_s = track.vy_m_s = 0.0;
        }

        // Landing candidate: after a tracking gap, or after a substantial foot
        // relocation with a physiologically plausible minimum interval. This is
        // intentionally conservative and must be validated/calibrated on the
        // actual S2E installation before clinical use.
        const bool gap = had_previous && dt > 0.12;
        const double since_land = track.last_land == std::chrono::steady_clock::time_point{}
            ? std::numeric_limits<double>::infinity()
            : std::chrono::duration<double>(now - track.last_land).count();
        const double relocation = had_previous ? std::hypot(obs.x_m - track.x_m, obs.y_m - track.y_m) : 0.0;
        const bool relocation_event = relocation > 0.15 && since_land > 0.25;

        if (track.last_land == std::chrono::steady_clock::time_point{} || gap || relocation_event) {
            if (track.last_land != std::chrono::steady_clock::time_point{}) {
                const double cycle = std::chrono::duration<double>(now - track.last_land).count();
                if (cycle > 0.2 && cycle < 5.0) {
                    track.cycle_s = cycle;
                    push_recent(track.cycle_history, cycle);
                }
                const double dx_cm = std::abs(obs.x_m - track.x_m) * 100.0;
                if (dx_cm > 1.0 && dx_cm < 300.0) {
                    track.previous_step_cm = track.current_step_cm;
                    track.current_step_cm = dx_cm;
                    push_recent(track.step_x_history, dx_cm);
                    push_recent(track.step_y_history, std::abs(obs.y_m - track.y_m) * 100.0);
                }
            }
            track.last_land = now;
        }

        track.valid = true;
        track.x_m = obs.x_m;
        track.y_m = obs.y_m;
        track.last_seen = now;
    };

    update(left_track_, left);
    update(right_track_, right);
    telemetry_.left = left;
    telemetry_.right = right;
}

double RplidarS2E::symmetry(double right, double left) {
    const double denom = 0.5 * (right + left);
    if (std::abs(denom) < 1e-9) return 0.0;
    return ((right - left) / denom) * 100.0;
}

double RplidarS2E::symmetry_score(double right, double left) {
    if (right <= 0.0 || left <= 0.0) return 0.0;
    const double index = std::abs(symmetry(right, left));
    return std::clamp(100.0 - index, 0.0, 100.0);
}

void RplidarS2E::update_metrics() {
    telemetry_.current_step_left_cm = left_track_.current_step_cm;
    telemetry_.current_step_right_cm = right_track_.current_step_cm;
    telemetry_.previous_step_left_cm = left_track_.previous_step_cm;
    telemetry_.previous_step_right_cm = right_track_.previous_step_cm;
    telemetry_.cycle_left_s = average(left_track_.cycle_history);
    telemetry_.cycle_right_s = average(right_track_.cycle_history);

    telemetry_.symmetry_time_percent =
        symmetry(telemetry_.cycle_right_s, telemetry_.cycle_left_s);
    telemetry_.symmetry_x_percent =
        symmetry(average(right_track_.step_x_history), average(left_track_.step_x_history));
    telemetry_.symmetry_y_percent =
        symmetry(average(right_track_.step_y_history), average(left_track_.step_y_history));
    telemetry_.longitudinal_symmetry_percent =
        symmetry_score(average(right_track_.step_x_history), average(left_track_.step_x_history));

    // Transverse symmetry is based on the lateral distance of each tracked foot
    // from the treadmill centre line.  Using absolute Y avoids reporting the
    // expected left/right sign difference as an artificial asymmetry.
    if (telemetry_.left.valid && telemetry_.right.valid) {
        telemetry_.transverse_symmetry_percent =
            symmetry_score(std::abs(telemetry_.right.y_m), std::abs(telemetry_.left.y_m));
    } else {
        telemetry_.transverse_symmetry_percent = 0.0;
    }

    telemetry_.metrics_valid =
        !left_track_.cycle_history.empty() &&
        !right_track_.cycle_history.empty() &&
        !left_track_.step_x_history.empty() &&
        !right_track_.step_x_history.empty();

    telemetry_.recommended_speed_delta_kmh = 0.0;
    if (config_.target_step_length_m > 0.0 && telemetry_.metrics_valid) {
        const double mean_step_m =
            (average(left_track_.step_x_history) + average(right_track_.step_x_history)) / 200.0;
        const double error = mean_step_m - config_.target_step_length_m;
        if (std::abs(error) > 0.02) {
            telemetry_.recommended_speed_delta_kmh =
                (error > 0.0 ? config_.speed_correction_step_kmh : -config_.speed_correction_step_kmh);
        }
    }
}

void RplidarS2E::update_safety() {
    telemetry_.geometry_configured =
        config_.belt_length_m > 0.0 &&
        config_.belt_width_m > 0.0 &&
        config_.front_margin_m + config_.rear_margin_m < config_.belt_length_m &&
        2.0 * config_.side_margin_m < config_.belt_width_m;

    if (!telemetry_.geometry_configured) {
        telemetry_.safety_state = "not_configured";
        telemetry_.safety_reason = "Размеры полотна/зоны безопасности не настроены";
        return;
    }

    auto critical = [&](const FootObservation& f) {
        if (!f.valid) return false;
        return f.x_m <= config_.front_margin_m ||
               f.x_m >= config_.belt_length_m - config_.rear_margin_m ||
               std::abs(f.y_m) >= config_.belt_width_m * 0.5 - config_.side_margin_m;
    };

    if (critical(telemetry_.left) || critical(telemetry_.right)) {
        telemetry_.safety_state = "critical";
        telemetry_.safety_reason = "Ступня вошла в ограничительную зону";
        return;
    }

    bool predicted = false;
    auto predict = [&](const FootTrack& t, const FootObservation& f) {
        if (!f.valid) return false;
        const double cycle = std::max(average(t.cycle_history), 0.0);
        if (cycle <= 0.0) return false;
        const double horizon = cycle * static_cast<double>(std::max(1, config_.prediction_steps));
        const double px = f.x_m + t.vx_m_s * horizon;
        const double py = f.y_m + t.vy_m_s * horizon;
        return px <= config_.front_margin_m ||
               px >= config_.belt_length_m - config_.rear_margin_m ||
               std::abs(py) >= config_.belt_width_m * 0.5 - config_.side_margin_m;
    };
    predicted = predict(left_track_, telemetry_.left) || predict(right_track_, telemetry_.right);

    telemetry_.safety_state = predicted ? "warning" : "safe";
    telemetry_.safety_reason = predicted
        ? "По текущей динамике возможно вхождение в зону заступа в пределах заданного прогноза"
        : "Ступни находятся в центральной безопасной зоне";
}

void RplidarS2E::process_scan(const std::vector<RplidarPoint>& points) {
    std::size_t filtered = 0;
    const auto clusters = clusters_from_scan(points, filtered);
    const auto now = std::chrono::steady_clock::now();

    std::lock_guard lock(mutex_);
    telemetry_.raw_point_count = points.size();
    telemetry_.filtered_point_count = filtered;
    latest_points_ = points;
    if (latest_points_.size() > 12000) latest_points_.resize(12000);
    ++telemetry_.scan_sequence;
    last_scan_ = now;

    update_tracks(clusters, now);
    update_metrics();
    update_safety();
}

void RplidarS2E::worker() {
#if defined(YADRO_RPLIDAR_S2E_SDK)
    using namespace sl;

    RplidarS2EConfig cfg;
    {
        std::lock_guard lock(mutex_);
        cfg = config_;
        telemetry_.running = true;
        telemetry_.connected = false;
        telemetry_.scanning = false;
        telemetry_.error.clear();
    }

    auto driver_result = createLidarDriver();
    ILidarDriver* driver = driver_result ? *driver_result : nullptr;
    auto channel_result = createUdpChannel(cfg.ip, cfg.udp_port);
    IChannel* channel = channel_result ? *channel_result : nullptr;

    if (!driver || !channel) {
        std::lock_guard lock(mutex_);
        telemetry_.error = "Не удалось создать SLAMTEC driver/UDP channel";
        telemetry_.running = false;
        if (driver) delete driver;
        if (channel) delete channel;
        return;
    }

    auto fail = [&](const std::string& message) {
        std::lock_guard lock(mutex_);
        telemetry_.error = message;
        telemetry_.connected = false;
        telemetry_.scanning = false;
    };

    if (SL_IS_FAIL(driver->connect(channel))) {
        fail("Не удалось подключиться к RPLIDAR S2E по UDP " + cfg.ip + ":" + std::to_string(cfg.udp_port));
    } else {
        sl_lidar_response_device_info_t info{};
        if (SL_IS_OK(driver->getDeviceInfo(info))) {
            std::lock_guard lock(mutex_);
            telemetry_.connected = true;
            telemetry_.serial_number = hex_serial(info.serialnum, sizeof(info.serialnum));
            telemetry_.firmware = std::to_string(info.firmware_version >> 8) + "." +
                                  std::to_string(info.firmware_version & 0xFF);
        } else {
            fail("RPLIDAR S2E: нет ответа на getDeviceInfo");
        }

        sl_lidar_response_device_health_t health{};
        if (telemetry_.connected && SL_IS_OK(driver->getHealth(health))) {
            std::lock_guard lock(mutex_);
            telemetry_.health =
                health.status == SL_LIDAR_STATUS_OK ? "ok" :
                health.status == SL_LIDAR_STATUS_WARNING ? "warning" : "error";
        }

        if (telemetry_.connected && SL_IS_OK(driver->startScan(false, true))) {
            {
                std::lock_guard lock(mutex_);
                telemetry_.scanning = true;
            }

            std::array<sl_lidar_response_measurement_node_hq_t, 8192> nodes{};
            while (!stop_requested_) {
                std::size_t count = nodes.size();
                const auto result = driver->grabScanDataHq(nodes.data(), count,
                    static_cast<sl_u32>(std::max(50, cfg.scan_timeout_ms)));
                if (SL_IS_OK(result)) {
                    driver->ascendScanData(nodes.data(), count);
                    std::vector<RplidarPoint> scan;
                    scan.reserve(count);
                    for (std::size_t i = 0; i < count; ++i) {
                        scan.push_back({
                            (nodes[i].angle_z_q14 * 90.0) / 16384.0,
                            (nodes[i].dist_mm_q2 / 4.0) / 1000.0,
                            static_cast<int>(nodes[i].quality >> SL_LIDAR_RESP_MEASUREMENT_QUALITY_SHIFT)
                        });
                    }
                    process_scan(scan);
                } else if (result != SL_RESULT_OPERATION_TIMEOUT) {
                    fail("Ошибка чтения скана RPLIDAR S2E");
                    break;
                }
            }
        } else if (telemetry_.connected) {
            fail("RPLIDAR S2E: не удалось запустить сканирование");
        }
    }

    driver->stop();
    driver->disconnect();
    delete driver;
    delete channel;

    std::lock_guard lock(mutex_);
    telemetry_.running = false;
    telemetry_.connected = false;
    telemetry_.scanning = false;
#endif
}

} // namespace yadro
