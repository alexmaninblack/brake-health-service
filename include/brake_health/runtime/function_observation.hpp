// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "brake_health/runtime/runtime.hpp"
#include "brake_health/runtime/json.hpp"
#include "brake_health/v1/sha256.hpp"
#include "aosedge/function_observation.hpp"
namespace brake_health::runtime {
struct ObservationCodec {
 using Json=brake_health::runtime::Json;
 static Json parse(const std::string& text,std::size_t limit){return parse_json(text,limit);}
 static std::string encode(const Json& value){return encode_json(value);}
 static std::string digest(const std::string& value){return brake_health::v1::sha256_hex(value);}
 static std::string timestamp(std::int64_t value){return utc_timestamp(value);}
 static bool uuid(const std::string& value){return is_uuid(value);}
};
using ObservationStream=aosedge::FunctionObservation<ObservationCodec>;
}

