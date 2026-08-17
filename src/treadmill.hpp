#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace yadro {

enum class MachineState { Stopped, Running, Stopping, EmergencyStopped, Fault };
enum class Direction { Forward, Reverse };
enum class SessionKind { None, FreeRun, Protocol, Profile, HeartRate };

struct Limits {
    double max_speed_kmh{20.0};
    double max_incline_percent{25.0};
    double max_accel_kmh_per_s{4.0};
    double max_decel_kmh_per_s{4.0};
    double max_incline_rate_percent_per_s{4.0};
    double watchdog_seconds{5.0};
};

struct Interval {
    int duration_seconds{60};
    double speed_kmh{1.0};
    double incline_percent{0.0};
    int acceleration_level{3};
};

struct Protocol {
    std::string id;
    std::string name;
    std::string description;
    bool implemented{true};
    bool standard{false};
    std::vector<Interval> intervals;
};

struct HeartRateProgram {
    int min_bpm{100};
    int max_bpm{130};
    double min_speed_kmh{1.0};
    double max_speed_kmh{6.0};
    double incline_percent{0.0};
    double speed_step_kmh{0.2};
    double adjust_period_seconds{5.0};
};

struct DriverSample {
    MachineState state{MachineState::Stopped};
    double speed_kmh{0.0};
    double incline_percent{0.0};
    Direction direction{Direction::Forward};
    std::string fault;
};

struct Telemetry {
    MachineState state{MachineState::Stopped};
    SessionKind session{SessionKind::None};
    Direction direction{Direction::Forward};
    double speed_kmh{0.0};
    double target_speed_kmh{1.0};
    double incline_percent{0.0};
    double target_incline_percent{0.0};
    double distance_km{0.0};
    double elapsed_seconds{0.0};
    double energy_kcal_estimate{0.0};
    double mechanical_power_w_estimate{0.0};
    int heart_rate_bpm{0};
    int max_heart_rate_bpm{0};
    int interval_index{-1};
    double interval_remaining_seconds{0.0};
    std::string active_program_id;
    std::string fault;
    std::string notice;
};

struct CommandResult {
    bool ok{true};
    std::string message;
};

class ITreadmillDriver {
public:
    virtual ~ITreadmillDriver() = default;
    virtual void configure(const Limits& limits) = 0;
    virtual CommandResult set_speed(double kmh) = 0;
    virtual CommandResult set_incline(double percent) = 0;
    virtual CommandResult set_direction(Direction direction) = 0;
    virtual CommandResult start() = 0;
    virtual CommandResult stop() = 0;
    virtual CommandResult emergency_stop() = 0;
    virtual CommandResult reset_emergency() = 0;
    virtual DriverSample sample(double dt_seconds) = 0;
};

class SimulationDriver final : public ITreadmillDriver {
public:
    void configure(const Limits& limits) override;
    CommandResult set_speed(double kmh) override;
    CommandResult set_incline(double percent) override;
    CommandResult set_direction(Direction direction) override;
    CommandResult start() override;
    CommandResult stop() override;
    CommandResult emergency_stop() override;
    CommandResult reset_emergency() override;
    DriverSample sample(double dt_seconds) override;

private:
    Limits limits_{};
    MachineState state_{MachineState::Stopped};
    Direction direction_{Direction::Forward};
    double speed_{0.0};
    double target_speed_{1.0};
    double incline_{0.0};
    double target_incline_{0.0};
};

class TreadmillController {
public:
    explicit TreadmillController(std::unique_ptr<ITreadmillDriver> driver, Limits limits = {});
    ~TreadmillController();

    TreadmillController(const TreadmillController&) = delete;
    TreadmillController& operator=(const TreadmillController&) = delete;

    CommandResult set_targets(double speed_kmh, double incline_percent);
    CommandResult set_direction(Direction direction);
    CommandResult start_free();
    CommandResult start_program(const Protocol& program, SessionKind kind);
    CommandResult start_heart_rate(const HeartRateProgram& program);
    CommandResult stop();
    CommandResult emergency_stop();
    CommandResult reset_emergency();
    void heartbeat();
    void set_heart_rate(int bpm);
    CommandResult update_limits(const Limits& limits);

    [[nodiscard]] Telemetry telemetry() const;
    [[nodiscard]] Limits limits() const;

private:
    void worker_loop();
    void tick(double dt_seconds);
    void apply_interval_locked(std::size_t index);
    void stop_locked(const std::string& notice = {});
    void reset_session_locked(SessionKind kind, const std::string& program_id = {});

    std::unique_ptr<ITreadmillDriver> driver_;
    Limits limits_{};
    mutable std::mutex mutex_;
    Telemetry telemetry_{};
    Protocol active_program_{};
    HeartRateProgram heart_rate_program_{};
    std::size_t active_interval_{0};
    double interval_elapsed_{0.0};
    double heart_rate_adjust_elapsed_{0.0};
    std::chrono::steady_clock::time_point last_heartbeat_;
    std::atomic<bool> running_{true};
    std::thread worker_;
};

std::string to_string(MachineState value);
std::string to_string(Direction value);
std::string to_string(SessionKind value);

} // namespace yadro
