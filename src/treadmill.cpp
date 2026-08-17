#include "treadmill.hpp"

#include <algorithm>
#include <cmath>

namespace yadro {
namespace {

double approach(double current, double target, double step) {
    if (current < target) return std::min(target, current + step);
    if (current > target) return std::max(target, current - step);
    return current;
}

CommandResult ok(std::string message = {}) { return {true, std::move(message)}; }
CommandResult fail(std::string message) { return {false, std::move(message)}; }

} // namespace

void SimulationDriver::configure(const Limits& limits) { limits_ = limits; }

CommandResult SimulationDriver::set_speed(double kmh) {
    if (kmh < 0.0 || kmh > limits_.max_speed_kmh) return fail("speed is outside configured limits");
    target_speed_ = kmh;
    return ok();
}

CommandResult SimulationDriver::set_incline(double percent) {
    if (percent < 0.0 || percent > limits_.max_incline_percent) return fail("incline is outside configured limits");
    target_incline_ = percent;
    return ok();
}

CommandResult SimulationDriver::set_direction(Direction direction) {
    if (speed_ > 0.2) return fail("direction can only be changed while belt is stopped");
    direction_ = direction;
    return ok();
}

CommandResult SimulationDriver::start() {
    if (state_ == MachineState::EmergencyStopped) return fail("emergency stop is active");
    if (state_ == MachineState::Fault) return fail("driver fault is active");
    state_ = MachineState::Running;
    return ok();
}

CommandResult SimulationDriver::stop() {
    if (state_ == MachineState::EmergencyStopped) return ok();
    target_speed_ = 0.0;
    state_ = speed_ > 0.01 ? MachineState::Stopping : MachineState::Stopped;
    return ok();
}

CommandResult SimulationDriver::emergency_stop() {
    target_speed_ = 0.0;
    speed_ = 0.0;
    state_ = MachineState::EmergencyStopped;
    return ok("emergency stop activated");
}

CommandResult SimulationDriver::reset_emergency() {
    if (state_ != MachineState::EmergencyStopped) return ok();
    state_ = MachineState::Stopped;
    return ok("emergency stop reset");
}

DriverSample SimulationDriver::sample(double dt_seconds) {
    const double speed_step = limits_.max_accel_kmh_per_s * dt_seconds;
    const double incline_step = limits_.max_incline_rate_percent_per_s * dt_seconds;

    if (state_ == MachineState::Running || state_ == MachineState::Stopping) {
        speed_ = approach(speed_, target_speed_, speed_step);
        if (state_ == MachineState::Stopping && speed_ <= 0.01) {
            speed_ = 0.0;
            state_ = MachineState::Stopped;
        }
    }
    incline_ = approach(incline_, target_incline_, incline_step);

    return {state_, speed_, incline_, direction_, {}};
}

TreadmillController::TreadmillController(std::unique_ptr<ITreadmillDriver> driver, Limits limits)
    : driver_(std::move(driver)), limits_(limits), last_heartbeat_(std::chrono::steady_clock::now()) {
    driver_->configure(limits_);
    telemetry_.target_speed_kmh = 1.0;
    worker_ = std::thread(&TreadmillController::worker_loop, this);
}

TreadmillController::~TreadmillController() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
    std::lock_guard lock(mutex_);
    driver_->stop();
}

CommandResult TreadmillController::set_targets(double speed_kmh, double incline_percent) {
    std::lock_guard lock(mutex_);
    if (telemetry_.state == MachineState::EmergencyStopped) return fail("emergency stop is active");
    if (speed_kmh < 0.0 || speed_kmh > limits_.max_speed_kmh) return fail("speed is outside configured limits");
    if (incline_percent < 0.0 || incline_percent > limits_.max_incline_percent) return fail("incline is outside configured limits");

    if (auto r = driver_->set_speed(speed_kmh); !r.ok) return r;
    if (auto r = driver_->set_incline(incline_percent); !r.ok) return r;
    telemetry_.target_speed_kmh = speed_kmh;
    telemetry_.target_incline_percent = incline_percent;
    last_heartbeat_ = std::chrono::steady_clock::now();
    return ok();
}

CommandResult TreadmillController::set_direction(Direction direction) {
    std::lock_guard lock(mutex_);
    auto r = driver_->set_direction(direction);
    if (r.ok) {
        telemetry_.direction = direction;
        last_heartbeat_ = std::chrono::steady_clock::now();
    }
    return r;
}

CommandResult TreadmillController::start_free() {
    std::lock_guard lock(mutex_);
    if (telemetry_.state == MachineState::EmergencyStopped) return fail("emergency stop is active");
    active_program_ = {};
    telemetry_.session = SessionKind::FreeRun;
    telemetry_.active_program_id.clear();
    telemetry_.interval_index = -1;
    telemetry_.distance_km = 0.0;
    telemetry_.elapsed_seconds = 0.0;
    telemetry_.energy_kcal_estimate = 0.0;
    telemetry_.notice.clear();
    if (telemetry_.target_speed_kmh <= 0.0) telemetry_.target_speed_kmh = 1.0;
    driver_->set_speed(telemetry_.target_speed_kmh);
    driver_->set_incline(telemetry_.target_incline_percent);
    auto r = driver_->start();
    if (!r.ok) return r;
    last_heartbeat_ = std::chrono::steady_clock::now();
    return ok("free run started");
}

CommandResult TreadmillController::start_program(const Protocol& program, SessionKind kind) {
    std::lock_guard lock(mutex_);
    if (!program.implemented) return fail("program is not implemented yet");
    if (program.intervals.empty()) return fail("program has no intervals");
    if (telemetry_.state == MachineState::EmergencyStopped) return fail("emergency stop is active");

    active_program_ = program;
    active_interval_ = 0;
    interval_elapsed_ = 0.0;
    telemetry_.session = kind;
    telemetry_.active_program_id = program.id;
    telemetry_.distance_km = 0.0;
    telemetry_.elapsed_seconds = 0.0;
    telemetry_.energy_kcal_estimate = 0.0;
    telemetry_.notice.clear();
    apply_interval_locked(0);
    auto r = driver_->start();
    if (!r.ok) return r;
    last_heartbeat_ = std::chrono::steady_clock::now();
    return ok("program started");
}

CommandResult TreadmillController::stop() {
    std::lock_guard lock(mutex_);
    stop_locked("stopped by operator");
    return ok();
}

CommandResult TreadmillController::emergency_stop() {
    std::lock_guard lock(mutex_);
    telemetry_.session = SessionKind::None;
    telemetry_.active_program_id.clear();
    telemetry_.notice = "EMERGENCY STOP";
    return driver_->emergency_stop();
}

CommandResult TreadmillController::reset_emergency() {
    std::lock_guard lock(mutex_);
    auto r = driver_->reset_emergency();
    if (r.ok) telemetry_.notice = "emergency stop reset";
    return r;
}

void TreadmillController::heartbeat() {
    std::lock_guard lock(mutex_);
    last_heartbeat_ = std::chrono::steady_clock::now();
}

void TreadmillController::set_heart_rate(int bpm) {
    std::lock_guard lock(mutex_);
    telemetry_.heart_rate_bpm = std::clamp(bpm, 0, 250);
    telemetry_.max_heart_rate_bpm = std::max(telemetry_.max_heart_rate_bpm, telemetry_.heart_rate_bpm);
}

Telemetry TreadmillController::telemetry() const {
    std::lock_guard lock(mutex_);
    return telemetry_;
}

Limits TreadmillController::limits() const { return limits_; }

void TreadmillController::worker_loop() {
    auto previous = std::chrono::steady_clock::now();
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto now = std::chrono::steady_clock::now();
        const std::chrono::duration<double> dt = now - previous;
        previous = now;
        tick(std::min(dt.count(), 0.25));
    }
}

void TreadmillController::tick(double dt_seconds) {
    std::lock_guard lock(mutex_);
    const auto sample = driver_->sample(dt_seconds);
    telemetry_.state = sample.state;
    telemetry_.speed_kmh = sample.speed_kmh;
    telemetry_.incline_percent = sample.incline_percent;
    telemetry_.direction = sample.direction;
    telemetry_.fault = sample.fault;

    const bool session_active = telemetry_.session != SessionKind::None;
    if (session_active && (sample.state == MachineState::Running || sample.state == MachineState::Stopping)) {
        telemetry_.elapsed_seconds += dt_seconds;
        telemetry_.distance_km += sample.speed_kmh * dt_seconds / 3600.0;

        // Simulation estimate only. A real driver should provide measured motor power/energy.
        telemetry_.mechanical_power_w_estimate = sample.speed_kmh <= 0.01
            ? 0.0
            : 60.0 + 14.0 * sample.speed_kmh + 7.0 * sample.incline_percent;
        telemetry_.energy_kcal_estimate += telemetry_.mechanical_power_w_estimate * dt_seconds / 4184.0;
    } else {
        telemetry_.mechanical_power_w_estimate = 0.0;
    }

    if (session_active && limits_.watchdog_seconds > 0.0 && sample.state == MachineState::Running) {
        const auto since_heartbeat = std::chrono::duration<double>(std::chrono::steady_clock::now() - last_heartbeat_).count();
        if (since_heartbeat > limits_.watchdog_seconds) {
            stop_locked("control watchdog timeout: belt stopped");
            return;
        }
    }

    if ((telemetry_.session == SessionKind::Protocol || telemetry_.session == SessionKind::Profile) &&
        sample.state == MachineState::Running && !active_program_.intervals.empty()) {
        interval_elapsed_ += dt_seconds;
        const auto& interval = active_program_.intervals[active_interval_];
        telemetry_.interval_remaining_seconds = std::max(0.0, interval.duration_seconds - interval_elapsed_);
        if (interval_elapsed_ >= interval.duration_seconds) {
            ++active_interval_;
            interval_elapsed_ = 0.0;
            if (active_interval_ >= active_program_.intervals.size()) {
                stop_locked("program completed");
            } else {
                apply_interval_locked(active_interval_);
            }
        }
    }
}

void TreadmillController::apply_interval_locked(std::size_t index) {
    const auto& interval = active_program_.intervals.at(index);
    const double speed = std::clamp(interval.speed_kmh, 0.0, limits_.max_speed_kmh);
    const double incline = std::clamp(interval.incline_percent, 0.0, limits_.max_incline_percent);
    driver_->set_speed(speed);
    driver_->set_incline(incline);
    telemetry_.target_speed_kmh = speed;
    telemetry_.target_incline_percent = incline;
    telemetry_.interval_index = static_cast<int>(index);
    telemetry_.interval_remaining_seconds = interval.duration_seconds;
}

void TreadmillController::stop_locked(const std::string& notice) {
    driver_->stop();
    telemetry_.session = SessionKind::None;
    telemetry_.active_program_id.clear();
    telemetry_.interval_index = -1;
    telemetry_.interval_remaining_seconds = 0.0;
    if (!notice.empty()) telemetry_.notice = notice;
}

std::string to_string(MachineState value) {
    switch (value) {
        case MachineState::Stopped: return "stopped";
        case MachineState::Running: return "running";
        case MachineState::Stopping: return "stopping";
        case MachineState::EmergencyStopped: return "emergency_stopped";
        case MachineState::Fault: return "fault";
    }
    return "unknown";
}

std::string to_string(Direction value) { return value == Direction::Forward ? "forward" : "reverse"; }

std::string to_string(SessionKind value) {
    switch (value) {
        case SessionKind::None: return "none";
        case SessionKind::FreeRun: return "free";
        case SessionKind::Protocol: return "protocol";
        case SessionKind::Profile: return "profile";
    }
    return "none";
}

} // namespace yadro
