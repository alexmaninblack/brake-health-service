// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "brake_health/runtime/runtime.hpp"
#include "brake_health/v2/episode.hpp"
#include <deque>

namespace brake_health::runtime {
inline constexpr std::array<const char*, 12> model_paths = {
    "Vehicle.Speed", "Vehicle.Acceleration.Longitudinal", "Vehicle.Chassis.Brake.PedalPosition",
    "Vehicle.Chassis.Axle.Row1.SteeringAngle",
    "Vehicle.Chassis.Axle.Row1.Wheel.Left.AngularSpeed", "Vehicle.Chassis.Axle.Row1.Wheel.Right.AngularSpeed",
    "Vehicle.Chassis.Axle.Row2.Wheel.Left.AngularSpeed", "Vehicle.Chassis.Axle.Row2.Wheel.Right.AngularSpeed",
    "Vehicle.Chassis.Axle.Row1.Wheel.Left.Speed", "Vehicle.Chassis.Axle.Row1.Wheel.Right.Speed",
    "Vehicle.Chassis.Axle.Row2.Wheel.Left.Speed", "Vehicle.Chassis.Axle.Row2.Wheel.Right.Speed"};
struct ModelFrame {
    v2::FixedSignals signals;
    std::int64_t source_epoch_ms{};
    std::int64_t monotonic_ms{};
    std::int32_t source_age_ms{};
};
std::int64_t quantize_milli(double value, double minimum, double maximum);
std::optional<ModelFrame> complete_model_frame(const std::array<Signal, 12>& signals,
    std::int64_t wall_ms, std::int64_t monotonic_ms, std::int64_t previous_epoch_ms);
// Same D4-016 episode boundary as v1, operating on the actual 12 model inputs.
// No excluded v1 signals are invented to pass the v1 frame validator.
class ModelCapture {
public:
    explicit ModelCapture(v1::UuidSource uuid = random_uuid);
    std::optional<v2::CompletedEpisode> ingest(const ModelFrame& frame);
    std::optional<v2::CompletedEpisode> abort(v2::TerminalState state);
    bool capturing() const { return state_ != State::Idle; }
private:
    enum class State { Idle, Active, Post };
    struct Retained { v2::Sample sample; std::int64_t monotonic_ms; };
    static bool hold(bool condition, std::int64_t now, std::int64_t duration, std::optional<std::int64_t>& start);
    void retain(const ModelFrame& frame, v2::Phase phase);
    void prune(std::int64_t now);
    std::size_t count(v2::Phase phase) const;
    State state_{State::Idle};
    v1::UuidSource uuid_;
    std::deque<Retained> pre_;
    v2::CompletedEpisode episode_;
    std::optional<std::int64_t> previous_source_, previous_mono_, trigger_hold_, clear_hold_, suppression_hold_;
    std::int64_t event_start_{}, active_start_{}, active_elapsed_{}, post_start_{};
    std::optional<std::int64_t> retained_bucket_;
    bool suppressed_{};
};
}  // namespace brake_health::runtime
