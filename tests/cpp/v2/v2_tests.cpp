// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#include "brake_health/v1/sha256.hpp"
#include "brake_health/v2/messages.hpp"
#include "brake_health/v2/model.hpp"
#include "brake_health/v2/state_store.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace brake_health::v2;

class TestFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            throw TestFailure(std::string("check failed: ") + #condition + " at line " + \
                              std::to_string(__LINE__));                                  \
        }                                                                                 \
    } while (false)

template <typename Exception, typename Callable>
void check_throws(Callable&& callable) {
    bool threw = false;
    try {
        callable();
    } catch (const Exception&) {
        threw = true;
    }
    CHECK(threw);
}

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(std::string suffix) {
        static int sequence = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("brake-health-v2-" + std::to_string(::getpid()) + "-" +
                 std::to_string(sequence++) + "-" + std::move(suffix));
        std::filesystem::create_directory(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::string timestamp(std::uint32_t milliseconds) {
    const std::uint32_t seconds = milliseconds / 1000U;
    const std::uint32_t fraction = milliseconds % 1000U;
    std::ostringstream output;
    output << "2026-08-22T12:00:" << std::setw(2) << std::setfill('0') << seconds
           << '.' << std::setw(3) << fraction << 'Z';
    return output.str();
}

std::string uuid4_for(std::uint64_t value) {
    std::ostringstream output;
    output << "10000000-0000-4000-8000-" << std::setw(12) << std::setfill('0') << value;
    return output.str();
}

CompletedEpisode golden_episode(std::string source_event_id =
                                    "4cba2d80-c04a-4d24-9f03-f4a85d56da13") {
    CompletedEpisode episode;
    episode.source_event_id = std::move(source_event_id);
    for (std::uint32_t index = 0; index < 80U; ++index) {
        Sample sample;
        sample.sample_index = index;
        sample.source_timestamp = timestamp(index * 100U);
        sample.max_source_age_ms = 20;
        if (index < 30U) {
            sample.phase = Phase::Pre;
            sample.signals.speed_milli_kph = 42000;
            sample.signals.wheel_linear_milli_kph.fill(42000);
            sample.signals.wheel_angular_milli_degree_per_second.fill(400000);
        } else if (index < 60U) {
            sample.phase = Phase::Active;
            const std::uint32_t active = index - 30U;
            const std::int64_t speed = active == 29U
                ? 10000 : 42000 - static_cast<std::int64_t>(active) * 1000;
            sample.signals.speed_milli_kph = speed;
            sample.signals.longitudinal_acceleration_milli_mps2 =
                active == 15U ? -6400 : -6000;
            sample.signals.brake_effort_milli_percent = 75000;
            sample.signals.wheel_linear_milli_kph.fill(speed);
            if (active == 0U) {
                sample.signals.wheel_linear_milli_kph = {36400, 40000, 40000, 40000};
            }
            sample.signals.wheel_angular_milli_degree_per_second.fill(400000);
        } else {
            sample.phase = Phase::Post;
        }
        episode.samples.push_back(std::move(sample));
    }
    return episode;
}

DeploymentMetadata metadata() {
    return {
        "demo-unit-validation-001",
        UnitRole::Validation,
        "2.0.0",
        std::string(64U, '3'),
        "2.0.0",
        std::string(64U, '2'),
        std::string(64U, '3'),
        kModelConfigSha256,
        "2026-08-22T12:00:08.000Z",
    };
}

const char* golden_assessment_json() {
    return R"JSON({"assessedAt":"2026-08-22T12:00:08.000Z","assessmentId":"a8ff857c-a7ca-5a59-a3fc-4e4ae90ffe3a","content":{"activeSampleCount":30,"conditionScore":38,"currentBand":"INSPECTION_RECOMMENDED","episodeLoadBps":6750,"features":{"activeDurationBps":6000,"activeDurationSeconds":3,"meanBrakeEffortBps":5000,"meanBrakeEffortPercent":75,"peakDecelerationBps":8000,"peakDecelerationMps2":6.4,"speedReductionBps":8000,"speedReductionKph":32,"wheelDispersionBps":6000,"wheelDispersionRatio":0.09},"previousBand":"MONITOR","quality":"VALID_DEMO_SYNTHETIC","sourceWindowEndTimestamp":"2026-08-22T12:00:07.900Z","sourceWindowStartTimestamp":"2026-08-22T12:00:00.000Z","straightActiveSampleCount":30,"wearIncrement":8,"wearIndexAfter":62,"wearIndexBefore":54},"contentSha256":"b1b5858b114898519fa0f4fe600864727df3c9f28c8228b182661ab6f183b932","contractVersion":"1.0.0","messageType":"BRAKE_HEALTH_ASSESSMENT","modelArtifactSha256":"3333333333333333333333333333333333333333333333333333333333333333","modelConfigSha256":"ea74cda63116d1f9fc969ec292aedb7cd0935ae899775bd8bcd73c230190e028","modelId":"brake-condition-demo-v1","modelVersion":"1.0.0","provenance":"DEMO_SYNTHETIC","schemaVersion":1,"serviceArtifactSha256":"3333333333333333333333333333333333333333333333333333333333333333","serviceVersion":"2.0.0","sourceEventId":"4cba2d80-c04a-4d24-9f03-f4a85d56da13","unitRole":"VALIDATION","unitSystemUid":"demo-unit-validation-001","vdpContractSha256":"2222222222222222222222222222222222222222222222222222222222222222","vdpContractVersion":"2.0.0"})JSON";
}

const char* golden_event_json() {
    return R"JSON({"assessmentId":"a8ff857c-a7ca-5a59-a3fc-4e4ae90ffe3a","content":{"conditionScore":38,"currentBand":"INSPECTION_RECOMMENDED","effectiveAt":"2026-08-22T12:00:07.900Z","eventType":"BRAKE_CONDITION_BAND_CHANGED","previousBand":"MONITOR","quality":"VALID_DEMO_SYNTHETIC","reasonCode":"SYNTHETIC_ACCUMULATED_STRESS_THRESHOLD"},"contentSha256":"df168403b59741dc61edb630312a15a658f45bd45a427c32d41f0952fc976001","contractVersion":"1.0.0","eventId":"5d6f208d-b041-566e-b91f-c847e35667a3","messageType":"BRAKE_HEALTH_EVENT","modelConfigSha256":"ea74cda63116d1f9fc969ec292aedb7cd0935ae899775bd8bcd73c230190e028","modelId":"brake-condition-demo-v1","modelVersion":"1.0.0","provenance":"DEMO_SYNTHETIC","schemaVersion":1,"serviceArtifactSha256":"3333333333333333333333333333333333333333333333333333333333333333","serviceVersion":"2.0.0","sourceEventId":"4cba2d80-c04a-4d24-9f03-f4a85d56da13","unitRole":"VALIDATION","unitSystemUid":"demo-unit-validation-001"})JSON";
}

const char* golden_state_json() {
    return R"JSON({"conditionBand":"INSPECTION_RECOMMENDED","conditionScore":38,"generation":1,"lastAppliedSourceEventId":"4cba2d80-c04a-4d24-9f03-f4a85d56da13","lastAssessmentId":"a8ff857c-a7ca-5a59-a3fc-4e4ae90ffe3a","modelConfigSha256":"ea74cda63116d1f9fc969ec292aedb7cd0935ae899775bd8bcd73c230190e028","modelId":"brake-condition-demo-v1","modelVersion":"1.0.0","nextAdvisorySequence":1,"producerEpoch":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa","profile":"DEMO_PRECONDITIONED","recentSourceEventIds":["4cba2d80-c04a-4d24-9f03-f4a85d56da13"],"schemaVersion":1,"wearIndex":62})JSON";
}

Evaluation evaluate_golden(const CompletedEpisode& episode = golden_episode()) {
    return SyntheticModel{}.evaluate(
        episode, initial_state("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"));
}

mode_t mode(const std::filesystem::path& path) {
    struct stat status {};
    CHECK(::stat(path.c_str(), &status) == 0);
    return status.st_mode & 0777;
}

void overwrite(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    CHECK(static_cast<bool>(output));
    output << bytes;
    CHECK(static_cast<bool>(output));
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    CHECK(static_cast<bool>(input));
    return std::string(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

void replace_once(std::string& value, const std::string& from, const std::string& to) {
    const std::size_t at = value.find(from);
    CHECK(at != std::string::npos);
    CHECK(value.find(from, at + from.size()) == std::string::npos);
    value.replace(at, from.size(), to);
}

std::filesystem::path only_journal(const std::filesystem::path& state_root) {
    std::vector<std::filesystem::path> values;
    for (const auto& entry : std::filesystem::directory_iterator(
             state_root / "transactions")) {
        if (entry.is_directory() && entry.path().filename().string().front() != '.') {
            values.push_back(entry.path());
        }
    }
    CHECK(values.size() == 1U);
    return values.front();
}

void overwrite_state_and_rebind_identity_ledger(
    const std::filesystem::path& state_root,
    const ModelState& state) {
    const std::string bytes = state_json(state);
    std::string ledger = read_file(state_root / "identity-ledger.json");
    const std::size_t at = ledger.find("\"stateSha256\":\"");
    CHECK(at != std::string::npos);
    const std::size_t digest = at + std::string("\"stateSha256\":\"").size();
    CHECK(digest + 64U <= ledger.size());
    ledger.replace(digest, 64U, brake_health::v1::sha256_hex(bytes));
    overwrite(state_root / "state.json", bytes);
    overwrite(state_root / "identity-ledger.json", ledger);
}

void test_rounding_and_model_boundaries() {
    CHECK(round_half_up(0U, 2U) == 0U);
    CHECK(round_half_up(1U, 2U) == 1U);
    CHECK(round_half_up(4U, 3U) == 1U);
    CHECK(round_half_up(5U, 3U) == 2U);
    for (std::uint64_t denominator :
         {100U, 1500U, 5000U, 8000U, 10000U, 40000U, 50000U}) {
        const std::uint64_t half = 10U * denominator + denominator / 2U;
        CHECK(round_half_up(half - 1U, denominator) == 10U);
        CHECK(round_half_up(half, denominator) == 11U);
        CHECK(round_half_up(half + 1U, denominator) == 11U);
    }
    check_throws<std::overflow_error>([] { round_half_up(1U, 0U); });
    check_throws<std::overflow_error>([] {
        round_half_up(std::numeric_limits<std::uint64_t>::max(), 2U);
    });
    CHECK(band_for_score(70U) == ConditionBand::Good);
    CHECK(band_for_score(69U) == ConditionBand::Monitor);
    CHECK(band_for_score(40U) == ConditionBand::Monitor);
    CHECK(band_for_score(39U) == ConditionBand::InspectionRecommended);

    CompletedEpisode low = golden_episode();
    for (Sample& sample : low.samples) {
        if (sample.phase == Phase::Active) {
            sample.signals.longitudinal_acceleration_milli_mps2 = 0;
            sample.signals.brake_effort_milli_percent = 0;
            sample.signals.wheel_linear_milli_kph.fill(sample.signals.speed_milli_kph);
        }
    }
    const Evaluation lower = evaluate_golden(low);
    CHECK(lower.assessment.has_value());
    CHECK(lower.assessment->features.episode_load_bps < 5000U);
    CHECK(lower.next_state.generation == 1U);

    ModelState saturated = initial_state("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    saturated.wear_index = 99U;
    saturated.condition_score = 1U;
    saturated.condition_band = ConditionBand::InspectionRecommended;
    const Evaluation capped = SyntheticModel{}.evaluate(golden_episode(), saturated);
    CHECK(capped.assessment->wear_index_after == 100U);
    CHECK(capped.assessment->condition_score == 0U);
    CHECK(!build_messages(metadata(), golden_episode(), *capped.assessment).event.has_value());

    ModelState exhausted = initial_state("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    exhausted.generation = std::numeric_limits<std::uint64_t>::max();
    check_throws<std::invalid_argument>([&] {
        SyntheticModel{}.evaluate(golden_episode(), exhausted);
    });
}

void expect_skip(const CompletedEpisode& episode, SkipReason reason) {
    const ModelState before = initial_state("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    const Evaluation result = SyntheticModel{}.evaluate(episode, before);
    CHECK(result.skip_reason == reason);
    CHECK(!result.assessment.has_value());
    CHECK(state_json(result.next_state) == state_json(before));
}

void test_input_quality_priority_and_boundaries() {
    CompletedEpisode value = golden_episode();
    value.terminal_state = TerminalState::AbortedServiceStop;
    value.samples[30].complete = false;
    expect_skip(value, SkipReason::EpisodeNotComplete);

    value = golden_episode();
    value.samples[30].complete = false;
    value.samples[30].max_source_age_ms = 5001;
    expect_skip(value, SkipReason::MissingRequiredSignal);

    value = golden_episode();
    value.samples[30].max_source_age_ms = 5001;
    value.samples[30].finite = false;
    expect_skip(value, SkipReason::StaleSample);

    value = golden_episode();
    value.samples[30].finite = false;
    value.samples[30].signals.brake_effort_milli_percent = 101000;
    expect_skip(value, SkipReason::NonFiniteValue);

    value = golden_episode();
    value.samples[30].signals.brake_effort_milli_percent = 101000;
    value.samples[31].source_timestamp = value.samples[30].source_timestamp;
    expect_skip(value, SkipReason::OutOfRangeValue);

    value = golden_episode();
    value.samples[31].source_timestamp = value.samples[30].source_timestamp;
    expect_skip(value, SkipReason::NonMonotonicSourceTime);

    value = golden_episode();
    for (std::size_t index = 34U; index < 60U; ++index) value.samples[index].phase = Phase::Post;
    expect_skip(value, SkipReason::InsufficientActiveSamples);

    value = golden_episode();
    for (std::size_t index = 34U; index < 60U; ++index) {
        value.samples[index].signals.steering_milli_degree = 5001;
    }
    expect_skip(value, SkipReason::InsufficientQualifiedWheelSamples);

    value = golden_episode();
    for (std::size_t index = 35U; index < 60U; ++index) {
        value.samples[index].signals.steering_milli_degree = 5001;
    }
    for (std::size_t index = 30U; index < 35U; ++index) {
        value.samples[index].signals.steering_milli_degree = index == 30U ? -5000 : 5000;
        value.samples[index].signals.speed_milli_kph = 10000;
    }
    CHECK(SyntheticModel{}.evaluate(
        value, initial_state("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa")).assessment.has_value());

    value.samples[34].signals.speed_milli_kph = 9999;
    expect_skip(value, SkipReason::InsufficientQualifiedWheelSamples);

    value = golden_episode();
    std::swap(value.samples[10].phase, value.samples[35].phase);
    expect_skip(value, SkipReason::MissingRequiredSignal);
}

void test_golden_features_messages_and_uuid() {
    const CompletedEpisode episode = golden_episode();
    const Evaluation result = evaluate_golden(episode);
    CHECK(result.assessment.has_value());
    const Assessment& assessment = *result.assessment;
    CHECK(assessment.features.peak_deceleration_milli_mps2 == 6400);
    CHECK(assessment.features.peak_deceleration_bps == 8000U);
    CHECK(assessment.features.active_duration_milliseconds == 3000U);
    CHECK(assessment.features.active_duration_bps == 6000U);
    CHECK(assessment.features.speed_reduction_milli_kph == 32000);
    CHECK(assessment.features.speed_reduction_bps == 8000U);
    CHECK(assessment.features.mean_brake_milli_percent == 75000);
    CHECK(assessment.features.mean_brake_bps == 5000U);
    CHECK(assessment.features.wheel_dispersion_raw_bps == 900U);
    CHECK(assessment.features.wheel_dispersion_bps == 6000U);
    CHECK(assessment.features.episode_load_bps == 6750U);
    CHECK(assessment.wear_index_before == 54U);
    CHECK(assessment.wear_increment == 8U);
    CHECK(assessment.wear_index_after == 62U);
    CHECK(assessment.condition_score == 38U);
    CHECK(assessment.previous_band == ConditionBand::Monitor);
    CHECK(assessment.current_band == ConditionBand::InspectionRecommended);
    CHECK(assessment.source_window_start_timestamp == "2026-08-22T12:00:00.000Z");
    CHECK(assessment.source_window_end_timestamp == "2026-08-22T12:00:07.900Z");

    const DerivedMessages messages = build_messages(metadata(), episode, assessment);
    CHECK(messages.assessment.id == "a8ff857c-a7ca-5a59-a3fc-4e4ae90ffe3a");
    CHECK(messages.assessment.content_sha256 ==
          "b1b5858b114898519fa0f4fe600864727df3c9f28c8228b182661ab6f183b932");
    CHECK(messages.assessment.idempotency_key_sha256 ==
          "84fcd9283e033b5858056c1a6ed7756ae7811d580183d79eb1b66c21be338c4a");
    CHECK(messages.assessment.canonical_json.size() == 1529U);
    CHECK(messages.assessment.canonical_json == golden_assessment_json());
    CHECK(brake_health::v1::sha256_hex(messages.assessment.canonical_json) ==
          "4bb1d6d9a8a5577f9338f300f537d8a20471cb59b38dd994c51ba4ebe289c629");
    CHECK(messages.event.has_value());
    CHECK(messages.event->id == "5d6f208d-b041-566e-b91f-c847e35667a3");
    CHECK(messages.event->content_sha256 ==
          "df168403b59741dc61edb630312a15a658f45bd45a427c32d41f0952fc976001");
    CHECK(messages.event->idempotency_key_sha256 ==
          "030574864e7e8f7003c5b38e7dfaf7a254a156c5d90e44bec294e31ebd50b092");
    CHECK(messages.event->canonical_json.size() == 947U);
    CHECK(messages.event->canonical_json == golden_event_json());
    CHECK(brake_health::v1::sha256_hex(messages.event->canonical_json) ==
          "2e1983ca56949df524b140b69aaa9871ac90c5d1207f915b0830cff0fc1427ce");

    CHECK(uuid_v5("6ba7b810-9dad-11d1-80b4-00c04fd430c8", {"www.widgets.com"}) ==
          "21f7f8de-8051-5b89-8680-0195ef798b6a");
    CHECK(uuid_v5("6ba7b810-9dad-11d1-80b4-00c04fd430c8", {"a", "b"}) !=
          uuid_v5("6ba7b810-9dad-11d1-80b4-00c04fd430c8", {"b", "a"}));
    check_throws<std::invalid_argument>([] {
        uuid_v5("6ba7b810-9dad-11d1-80b4-00c04fd430c8", {"a\nb"});
    });
    check_throws<std::invalid_argument>([] {
        uuid_v5("6ba7b810-9dad-11d1-80b4-00c04fd430c8", {std::string("a\0b", 3U)});
    });

    DeploymentMetadata invalid = metadata();
    invalid.unit_system_uid = std::string(129U, 'a');
    check_throws<std::invalid_argument>([&] { build_messages(invalid, episode, assessment); });
    invalid = metadata();
    invalid.model_config_sha256 = std::string(64U, '0');
    check_throws<std::invalid_argument>([&] { build_messages(invalid, episode, assessment); });
    invalid = metadata();
    invalid.assessed_at = "2026-08-22T12:00:07.899Z";
    check_throws<std::invalid_argument>([&] { build_messages(invalid, episode, assessment); });

    DeploymentMetadata maximum_uid = metadata();
    maximum_uid.unit_system_uid = std::string(128U, 'a');
    const DerivedMessages maximum_messages = build_messages(maximum_uid, episode, assessment);
    CHECK(maximum_messages.assessment.canonical_json.size() <= 16384U);
    CHECK(maximum_messages.event.has_value());
    CHECK(maximum_messages.event->canonical_json.size() <= 16384U);
    CHECK(message_idempotency_key_sha256(
              maximum_uid.unit_system_uid,
              maximum_messages.assessment.message_type,
              maximum_messages.assessment.id) ==
          maximum_messages.assessment.idempotency_key_sha256);
}

void test_state_schema_and_preserved_v3_fields() {
    ModelState state = initial_state("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    state.next_advisory_sequence = 42U;
    const std::string encoded = state_json(state);
    const ModelState decoded = parse_state_json(encoded);
    CHECK(decoded.producer_epoch == "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    CHECK(decoded.next_advisory_sequence == 42U);
    CHECK(state_json(decoded) == encoded);
    auto legacy=decoded;legacy.model_config_sha256=kLegacyModelConfigSha256;
    const auto legacy_bytes=state_json(legacy);
    const auto restored=parse_state_json(legacy_bytes);
    CHECK(state_json(restored)==legacy_bytes);
    CHECK(restored.next_advisory_sequence==42U && restored.wear_index==decoded.wear_index);
    check_throws<std::invalid_argument>([&] { parse_state_json(encoded + " "); });

    std::string wrong = encoded;
    const std::size_t digest = wrong.find(kModelConfigSha256);
    CHECK(digest != std::string::npos);
    wrong[digest] = '0';
    check_throws<std::invalid_argument>([&] { parse_state_json(wrong); });

    state.recent_source_event_ids = {uuid4_for(1), uuid4_for(1)};
    check_throws<std::invalid_argument>([&] { state_json(state); });

    std::string unknown = encoded;
    unknown.insert(unknown.size() - 1U, ",\"unknown\":true");
    check_throws<std::invalid_argument>([&] { parse_state_json(unknown); });
    std::string duplicate = encoded;
    duplicate.insert(duplicate.size() - 1U, ",\"generation\":0");
    check_throws<std::invalid_argument>([&] { parse_state_json(duplicate); });
    check_throws<std::invalid_argument>([] { parse_state_json("{}"); });
}

void test_timing_profile_preserves_state_and_outbox() {
    TemporaryDirectory temporary("timing-profile");
    const auto state_root=temporary.path()/"state", outbox_root=temporary.path()/"outbox";
    const std::string epoch="aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
    ModelState before;std::vector<OutboxEntry> pending;
    {
        StateStore store(state_root,outbox_root,epoch);
        CHECK(store.process(golden_episode(),metadata()).status==ProcessStatus::Produced);
        before=store.state();before.model_config_sha256=kLegacyModelConfigSha256;
        before.next_advisory_sequence=42;
        overwrite_state_and_rebind_identity_ledger(state_root,before);
        pending=store.inventory();CHECK(pending.size()==2);
    }
    StateStore recovered(state_root,outbox_root,epoch);CHECK(recovered.ready());
    CHECK(state_json(recovered.state())==state_json(before));
    CHECK(recovered.process(golden_episode(),metadata()).status==ProcessStatus::Duplicate);
    CHECK(state_json(recovered.state())==state_json(before));
    auto invalid=golden_episode(uuid4_for(2));invalid.terminal_state=TerminalState::IncompleteSourceGap;
    CHECK(recovered.process(invalid,metadata()).status==ProcessStatus::SkippedInputQuality);
    CHECK(state_json(recovered.state())==state_json(before));
    CHECK(recovered.process(golden_episode(uuid4_for(3)),metadata()).status==ProcessStatus::Produced);
    const auto after=recovered.state();
    CHECK(after.model_config_sha256==kModelConfigSha256 && after.generation==before.generation+1);
    CHECK(after.wear_index>=before.wear_index && after.next_advisory_sequence==42 && after.producer_epoch==epoch);
    for(const auto& old:pending) {
        bool found=false;for(const auto& retained:recovered.inventory())
            if(retained.id==old.id){CHECK(retained.canonical_json==old.canonical_json);found=true;}
        CHECK(found);
    }
}

void test_store_first_start_duplicate_and_ack() {
    TemporaryDirectory temporary("store");
    const auto state_root = temporary.path() / "model-state";
    const auto outbox_root = temporary.path() / "outbox";
    StateStore store(
        state_root, outbox_root, "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    CHECK(store.ready());
    CHECK(store.state().generation == 0U);
    CHECK(mode(state_root) == 0700);
    CHECK(mode(outbox_root) == 0700);
    CHECK(mode(state_root / "state.json") == 0600);
    CHECK(mode(state_root / "identity-ledger.json") == 0600);

    const ProcessResult result = store.process(golden_episode(), metadata());
    CHECK(result.status == ProcessStatus::Produced);
    CHECK(result.event_created);
    CHECK(store.state().generation == 1U);
    CHECK(store.state().recent_source_event_ids.size() == 1U);
    CHECK(state_json(store.state()) == golden_state_json());
    const std::vector<OutboxEntry> entries = store.inventory();
    CHECK(entries.size() == 2U);
    CHECK(mode(outbox_root / *result.assessment_id) == 0700);
    CHECK(mode(outbox_root / *result.assessment_id / "assessment.json") == 0600);

    const ProcessResult duplicate = store.process(golden_episode(), metadata());
    CHECK(duplicate.status == ProcessStatus::Duplicate);
    CHECK(duplicate.assessment_id == result.assessment_id);
    CHECK(store.state().generation == 1U);
    CHECK(store.inventory().size() == 2U);

    const auto event = std::find_if(entries.begin(), entries.end(), [](const OutboxEntry& value) {
        return value.message_type == "BRAKE_HEALTH_EVENT";
    });
    const auto assessment = std::find_if(entries.begin(), entries.end(), [](const OutboxEntry& value) {
        return value.message_type == "BRAKE_HEALTH_ASSESSMENT";
    });
    CHECK(event != entries.end() && assessment != entries.end());
    CHECK(store.acknowledge(event->id, event->idempotency_key_sha256, event->content_sha256));
    CHECK(store.inventory().size() == 1U);
    CHECK(store.acknowledge(
        assessment->id,
        assessment->idempotency_key_sha256,
        assessment->content_sha256));
    CHECK(store.inventory().empty());
    CHECK(!store.acknowledge(
        assessment->id,
        assessment->idempotency_key_sha256,
        assessment->content_sha256));
}

void test_ack_conflict_quarantine() {
    TemporaryDirectory temporary("ack-conflict");
    StateStore store(
        temporary.path() / "state",
        temporary.path() / "outbox",
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    CHECK(store.process(golden_episode(), metadata()).status == ProcessStatus::Produced);
    const OutboxEntry entry = store.inventory().front();
    CHECK(!store.acknowledge(entry.id, std::string(64U, '0'), entry.content_sha256));
    const std::vector<OutboxEntry> retained = store.inventory();
    CHECK(retained.size() == 2U);
    CHECK(retained[0].quarantined && retained[1].quarantined);
}

void test_interrupted_transaction_recovery() {
    for (WriteStage stage : {
             WriteStage::JournalFiles,
             WriteStage::Journal,
             WriteStage::State,
             WriteStage::IdentityLedger,
             WriteStage::BundleFiles,
             WriteStage::Bundle,
             WriteStage::CommitMarker,
             WriteStage::JournalRemoval}) {
        TemporaryDirectory temporary("recovery-" + std::to_string(static_cast<int>(stage)));
        bool injected = false;
        StateStore interrupted(
            temporary.path() / "state",
            temporary.path() / "outbox",
            "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
            [&](WriteStage current) {
                if (!injected && current == stage) {
                    injected = true;
                    return true;
                }
                return false;
            });
        check_throws<std::runtime_error>([&] {
            static_cast<void>(interrupted.process(golden_episode(), metadata()));
        });
        CHECK(injected);
        if (stage == WriteStage::Journal) {
            for (const auto& entry : std::filesystem::directory_iterator(
                     temporary.path() / "state" / "transactions")) {
                if (entry.is_directory() && entry.path().filename().string().front() != '.') {
                    overwrite(entry.path() / ".committed.tmp-interrupted", "partial");
                }
            }
        }

        StateStore recovered(
            temporary.path() / "state",
            temporary.path() / "outbox",
            "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
        CHECK(recovered.ready());
        const bool unpublished_journal = stage == WriteStage::JournalFiles;
        CHECK(recovered.state().generation == (unpublished_journal ? 0U : 1U));
        CHECK(recovered.state().wear_index == (unpublished_journal ? 54U : 62U));
        CHECK(recovered.inventory().size() == (unpublished_journal ? 0U : 2U));
        CHECK(std::filesystem::is_empty(temporary.path() / "state" / "transactions"));
    }
}

void test_corruption_and_generation_conflict_fail_closed() {
    TemporaryDirectory malformed("malformed");
    StateStore initial(
        malformed.path() / "state",
        malformed.path() / "outbox",
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    overwrite(malformed.path() / "state" / "state.json", "{}");
    StateStore rejected(
        malformed.path() / "state",
        malformed.path() / "outbox",
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    CHECK(!rejected.ready());
    CHECK(rejected.process(golden_episode(), metadata()).status == ProcessStatus::NotReadyState);

    TemporaryDirectory epoch("epoch-mismatch");
    StateStore epoch_owner(
        epoch.path() / "state",
        epoch.path() / "outbox",
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    CHECK(epoch_owner.ready());
    StateStore wrong_epoch(
        epoch.path() / "state",
        epoch.path() / "outbox",
        "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    CHECK(!wrong_epoch.ready());

    TemporaryDirectory conflict("generation-conflict");
    bool injected = false;
    StateStore interrupted(
        conflict.path() / "state",
        conflict.path() / "outbox",
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
        [&](WriteStage stage) {
            if (!injected && stage == WriteStage::Journal) {
                injected = true;
                return true;
            }
            return false;
        });
    check_throws<std::runtime_error>([&] {
        static_cast<void>(interrupted.process(golden_episode(), metadata()));
    });
    ModelState unrelated = initial_state("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    unrelated.generation = 9U;
    overwrite(conflict.path() / "state" / "state.json", state_json(unrelated));
    StateStore quarantined(
        conflict.path() / "state",
        conflict.path() / "outbox",
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    CHECK(!quarantined.ready());
}

void test_manifest_inventory_and_config_fail_closed() {
    const std::vector<std::pair<std::string, std::string>> event_fields_to_remove = {
        {"eventId", "5d6f208d-b041-566e-b91f-c847e35667a3"},
        {"eventContentSha256",
         "df168403b59741dc61edb630312a15a658f45bd45a427c32d41f0952fc976001"},
        {"eventIdempotencyKeySha256",
         "030574864e7e8f7003c5b38e7dfaf7a254a156c5d90e44bec294e31ebd50b092"},
        {"eventMessageSha256",
         "2e1983ca56949df524b140b69aaa9871ac90c5d1207f915b0830cff0fc1427ce"}};
    for (std::size_t index = 0U; index < event_fields_to_remove.size(); ++index) {
        TemporaryDirectory malformed_event("malformed-event-tuple-" + std::to_string(index));
        bool injected = false;
        StateStore interrupted(
            malformed_event.path() / "state",
            malformed_event.path() / "outbox",
            "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
            [&](WriteStage stage) {
                if (!injected && stage == WriteStage::Journal) {
                    injected = true;
                    return true;
                }
                return false;
            });
        check_throws<std::runtime_error>([&] {
            static_cast<void>(interrupted.process(golden_episode(), metadata()));
        });
        const std::filesystem::path journal = only_journal(malformed_event.path() / "state");
        std::string manifest = read_file(journal / "manifest.json");
        const std::string key = "\"" + event_fields_to_remove[index].first + "\"";
        replace_once(
            manifest,
            key + ":\"" + event_fields_to_remove[index].second + "\"",
            key + ":null");
        overwrite(journal / "manifest.json", manifest);
        StateStore rejected_event(
            malformed_event.path() / "state",
            malformed_event.path() / "outbox",
            "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
        CHECK(!rejected_event.ready());
        CHECK(std::filesystem::exists(journal / "QUARANTINED"));
    }

    TemporaryDirectory inventory_corruption("inventory-corruption");
    const auto state_root = inventory_corruption.path() / "state";
    const auto outbox_root = inventory_corruption.path() / "outbox";
    StateStore store(
        state_root, outbox_root, "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    const ProcessResult produced = store.process(golden_episode(), metadata());
    CHECK(produced.status == ProcessStatus::Produced);
    const std::filesystem::path outbox_manifest =
        outbox_root / *produced.assessment_id / "manifest.json";
    std::string corrupted = read_file(outbox_manifest);
    replace_once(
        corrupted,
        "\"assessmentContentSha256\":\"" +
            std::string("b1b5858b114898519fa0f4fe600864727df3c9f28c8228b182661ab6f183b932") +
            "\"",
        "\"assessmentContentSha256\":\"" + std::string(64U, '0') + "\"");
    overwrite(outbox_manifest, corrupted);
    CHECK(store.inventory().empty());
    CHECK(!store.ready());
    CHECK(std::filesystem::exists(
        outbox_root / *produced.assessment_id / "QUARANTINED"));
    CHECK(store.process(golden_episode(uuid4_for(2U)), metadata()).status ==
          ProcessStatus::NotReadyState);

    TemporaryDirectory config_conflict("config-conflict");
    StateStore config_store(
        config_conflict.path() / "state",
        config_conflict.path() / "outbox",
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    DeploymentMetadata invalid = metadata();
    invalid.model_config_sha256 = std::string(64U, '0');
    CHECK(config_store.process(golden_episode(), invalid).status ==
          ProcessStatus::NotReadyState);
    CHECK(!config_store.ready());
    CHECK(parse_state_json(read_file(config_conflict.path() / "state" / "state.json")).generation ==
          0U);
    CHECK(std::filesystem::is_empty(config_conflict.path() / "outbox"));

    TemporaryDirectory ledger_conflict("identity-ledger-conflict");
    StateStore ledger_store(
        ledger_conflict.path() / "state",
        ledger_conflict.path() / "outbox",
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    const ProcessResult ledger_result = ledger_store.process(golden_episode(), metadata());
    CHECK(ledger_result.status == ProcessStatus::Produced);
    std::string identity_ledger = read_file(
        ledger_conflict.path() / "state" / "identity-ledger.json");
    replace_once(
        identity_ledger,
        *ledger_result.assessment_id,
        ledger_result.assessment_id->substr(0U, 35U) + "f");
    overwrite(
        ledger_conflict.path() / "state" / "identity-ledger.json",
        identity_ledger);
    StateStore rejected_ledger(
        ledger_conflict.path() / "state",
        ledger_conflict.path() / "outbox",
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    CHECK(!rejected_ledger.ready());
}

void test_duplicate_identity_assessment_only_and_byte_edges() {
    CHECK(derived_outbox_admissible(0U, 1048576U, 0U, 0U));
    CHECK(!derived_outbox_admissible(0U, 1048577U, 0U, 0U));
    CHECK(derived_outbox_admissible(63U, 1048000U, 1U, 576U));
    CHECK(!derived_outbox_admissible(63U, 1048000U, 1U, 577U));
    CHECK(!derived_outbox_admissible(64U, 0U, 1U, 1U));

    TemporaryDirectory temporary("duplicate-identity");
    StateStore store(
        temporary.path() / "state",
        temporary.path() / "outbox",
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    const CompletedEpisode first = golden_episode(uuid4_for(1U));
    const CompletedEpisode second = golden_episode(uuid4_for(2U));
    const ProcessResult first_result = store.process(first, metadata());
    CHECK(first_result.status == ProcessStatus::Produced);
    CHECK(first_result.event_created);
    const ProcessResult second_result = store.process(second, metadata());
    CHECK(second_result.status == ProcessStatus::Produced);
    CHECK(!second_result.event_created);
    const std::vector<OutboxEntry> before_ack = store.inventory();
    CHECK(before_ack.size() == 3U);
    CHECK(std::count_if(
              before_ack.begin(), before_ack.end(), [&](const OutboxEntry& value) {
                  return value.source_event_id == second.source_event_id;
              }) == 1);

    DeploymentMetadata changed_metadata = metadata();
    changed_metadata.unit_system_uid = "demo-unit-validation-reprovisioned";
    const ProcessResult retained_duplicate = store.process(first, changed_metadata);
    CHECK(retained_duplicate.status == ProcessStatus::Duplicate);
    CHECK(retained_duplicate.assessment_id == first_result.assessment_id);
    for (const OutboxEntry& entry : before_ack) {
        if (entry.source_event_id == first.source_event_id) {
            CHECK(store.acknowledge(
                entry.id, entry.idempotency_key_sha256, entry.content_sha256));
        }
    }
    const ProcessResult historical_duplicate = store.process(first, changed_metadata);
    CHECK(historical_duplicate.status == ProcessStatus::Duplicate);
    CHECK(historical_duplicate.assessment_id == first_result.assessment_id);
    CHECK(store.state().generation == 2U);
    StateStore restarted(
        temporary.path() / "state",
        temporary.path() / "outbox",
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    CHECK(restarted.ready());
    const ProcessResult recovered_duplicate = restarted.process(first, changed_metadata);
    CHECK(recovered_duplicate.status == ProcessStatus::Duplicate);
    CHECK(recovered_duplicate.assessment_id == first_result.assessment_id);

    for (const OutboxEntry& entry : restarted.inventory()) {
        CHECK(restarted.acknowledge(
            entry.id, entry.idempotency_key_sha256, entry.content_sha256));
    }
    CHECK(restarted.inventory().empty());
    StateStore fully_acknowledged(
        temporary.path() / "state",
        temporary.path() / "outbox",
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    CHECK(fully_acknowledged.ready());
    const ProcessResult fully_acknowledged_duplicate =
        fully_acknowledged.process(first, changed_metadata);
    CHECK(fully_acknowledged_duplicate.status == ProcessStatus::Duplicate);
    CHECK(fully_acknowledged_duplicate.assessment_id == first_result.assessment_id);

    std::string tampered_ledger = read_file(
        temporary.path() / "state" / "identity-ledger.json");
    const std::string valid_but_wrong_assessment =
        assessment_id(metadata(), uuid4_for(99U));
    CHECK(valid_but_wrong_assessment != *first_result.assessment_id);
    CHECK(valid_but_wrong_assessment != *second_result.assessment_id);
    replace_once(
        tampered_ledger,
        *first_result.assessment_id,
        valid_but_wrong_assessment);
    overwrite(
        temporary.path() / "state" / "identity-ledger.json",
        tampered_ledger);
    StateStore tampered_history(
        temporary.path() / "state",
        temporary.path() / "outbox",
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    CHECK(!tampered_history.ready());
    CHECK(tampered_history.process(first, changed_metadata).status ==
          ProcessStatus::NotReadyState);
}

void test_ledger_rollover_pair_overflow_and_v1_coexistence() {
    TemporaryDirectory temporary("rollover");
    const std::filesystem::path v1 = temporary.path() / "v1-spool" / "event";
    std::filesystem::create_directories(v1);
    overwrite(v1 / "completion.json", "retained-v1-bytes");
    const std::string v1_before = brake_health::v1::sha256_hex("retained-v1-bytes");

    const auto state_root = temporary.path() / "state";
    const auto outbox_root = temporary.path() / "outbox";
    StateStore store(
        state_root, outbox_root, "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    for (std::uint64_t index = 1U; index <= 62U; ++index) {
        CompletedEpisode episode = golden_episode(uuid4_for(index));
        const ProcessResult result = store.process(episode, metadata());
        CHECK(result.status == ProcessStatus::Produced);
    }
    CHECK(store.inventory().size() == 63U);
    ModelState reset_band = store.state();
    reset_band.wear_index = 54U;
    reset_band.condition_score = 46U;
    reset_band.condition_band = ConditionBand::Monitor;
    overwrite_state_and_rebind_identity_ledger(state_root, reset_band);
    StateStore resumed(
        state_root, outbox_root, "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    CHECK(resumed.ready());
    const ProcessResult overflow = resumed.process(golden_episode(uuid4_for(63U)), metadata());
    CHECK(overflow.status == ProcessStatus::DerivedOutboxFull);
    CHECK(!overflow.event_created);
    CHECK(resumed.inventory().size() == 63U);
    CHECK(resumed.state().generation == 63U);
    CHECK(resumed.state().recent_source_event_ids.size() == 63U);

    CHECK(resumed.process(golden_episode(uuid4_for(64U)), metadata()).status ==
          ProcessStatus::Produced);
    for (std::uint64_t index = 65U; index <= 66U; ++index) {
        CHECK(resumed.process(golden_episode(uuid4_for(index)), metadata()).status ==
              ProcessStatus::DerivedOutboxFull);
    }
    CHECK(resumed.state().generation == 66U);
    CHECK(resumed.state().recent_source_event_ids.size() == 64U);
    CHECK(resumed.state().recent_source_event_ids.front() == uuid4_for(3U));
    CHECK(brake_health::v1::sha256_hex(
        [&] {
            std::ifstream input(v1 / "completion.json", std::ios::binary);
            return std::string(
                (std::istreambuf_iterator<char>(input)),
                std::istreambuf_iterator<char>());
        }()) == v1_before);
}

void test_post_retrigger_preserves_one_episode() {
    auto episode = golden_episode();
    episode.samples.resize(75); // Keep PRE=30, ACTIVE=25, total POST=20.
    for (std::size_t i = 40; i < 45; ++i) episode.samples[i].phase = Phase::Post;
    const auto evaluation = evaluate_golden(episode);
    CHECK(!evaluation.skip_reason && evaluation.assessment);
    CHECK(evaluation.assessment->features.active_sample_count == 25);
    CHECK(evaluation.assessment->features.active_duration_milliseconds == 2500);
    CHECK(evaluation.assessment->features.episode_load_bps == 6550);
    CHECK(evaluation.assessment->wear_increment == 8);
    CHECK(evaluation.assessment->source_window_end_timestamp == timestamp(7400));
    TemporaryDirectory temporary("post-retrigger");
    StateStore store(temporary.path() / "state", temporary.path() / "outbox",
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    const auto result = store.process(episode, metadata());
    CHECK(result.status == ProcessStatus::Produced && result.event_created);
    CHECK(store.state().generation == 1 && store.state().wear_index == 62);
    CHECK(store.process(episode, metadata()).status == ProcessStatus::Duplicate);
    CHECK(store.state().generation == 1 && store.state().wear_index == 62);
    CHECK(store.inventory().size() == 2);

    auto invalid = episode;
    invalid.samples[50].phase = Phase::Pre;
    CHECK(evaluate_golden(invalid).skip_reason == SkipReason::MissingRequiredSignal);
    invalid = episode;
    invalid.samples[0].phase = Phase::Post;
    CHECK(evaluate_golden(invalid).skip_reason == SkipReason::MissingRequiredSignal);
}

void run(const char* name, const std::function<void()>& test) {
    try {
        test();
        std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        throw;
    }
}

}  // namespace

int main() {
    run("rounding and model boundaries", test_rounding_and_model_boundaries);
    run("input quality priority and boundaries", test_input_quality_priority_and_boundaries);
    run("golden features messages and uuid", test_golden_features_messages_and_uuid);
    run("state schema and preserved v3 fields", test_state_schema_and_preserved_v3_fields);
    run("store first start duplicate and ack", test_store_first_start_duplicate_and_ack);
    run("timing profile preserves state and outbox", test_timing_profile_preserves_state_and_outbox);
    run("ack conflict quarantine", test_ack_conflict_quarantine);
    run("interrupted transaction recovery", test_interrupted_transaction_recovery);
    run("corruption and generation conflict", test_corruption_and_generation_conflict_fail_closed);
    run("manifest inventory config fail closed", test_manifest_inventory_and_config_fail_closed);
    run("duplicate identity assessment only byte edges", test_duplicate_identity_assessment_only_and_byte_edges);
    run("ledger rollover pair overflow v1 coexistence", test_ledger_rollover_pair_overflow_and_v1_coexistence);
    run("POST retrigger preserves one episode", test_post_retrigger_preserves_one_episode);
    return 0;
}
