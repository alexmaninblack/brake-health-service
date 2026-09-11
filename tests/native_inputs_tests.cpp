// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#include "brake_health/runtime/application.hpp"
#include <iostream>
#include <stdexcept>
using namespace brake_health;
using namespace brake_health::runtime;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at " + std::to_string(__LINE__)); } while (false)
template<class F> void rejects(F operation) { bool failed=false; try {operation();} catch(...) {failed=true;} CHECK(failed); }
int main() {
 try {
  const std::string release=R"({"schemaVersion":1,"serviceVersion":"18.0.0"})";
  const std::map<std::string,std::string> environment{{"AOS_ITEM_ID","native-service"},{"AOS_SUBJECT_ID","native-subject"},{"AOS_INSTANCE_INDEX","0"},{"AOS_INSTANCE_ID","native-instance"}};
  const auto native=parse_service_inputs(release,environment);
  CHECK(native.service_version=="18.0.0" && native.instance.instance_index==0);
  const auto public_input=std::string(R"({"schemaVersion":2,"unitSystemUid":"fixture-unit","unitRole":"validation","vdpContractVersion":"3.0.0","vdpContractSha256":")")+std::string(64,'b')+"\"}";
  const auto m=parse_metadata(public_input,native);
  CHECK(m.service_version=="18.0.0" && m.service_artifact_sha256.empty());
  CHECK(m.service_instance==native.instance && m.vdp_contract_sha256==std::string(64,'b'));
  const auto binding=metadata_binding(m);
  const auto restored=parse_metadata_binding(binding);
  CHECK(restored.service_instance==m.service_instance && restored.service_version==m.service_version);
  CHECK(!binding.object().count("serviceArtifactSha256"));
  for(const auto* value:{"","01.0.0","1.0","1.0.0-dev","1.0.0+build","1.0.0\n","999999999999999999999999999999.0.0"}) {
   auto invalid=parse_json(release).object();invalid["serviceVersion"]=Json{std::string(value)};
   rejects([&]{parse_service_inputs(encode_json(Json{invalid}),environment);});
  }
  for(const auto* value:{"","-1","+1","01","0.0","1e0"," 1","1\n","9007199254740992","18446744073709551616"}) {
   auto invalid=environment;invalid["AOS_INSTANCE_INDEX"]=value;
   rejects([&]{parse_service_inputs(release,invalid);});
  }
  auto boundary=environment;boundary["AOS_INSTANCE_INDEX"]="9007199254740991";
  CHECK(parse_service_inputs(release,boundary).instance.instance_index==9007199254740991ULL);
  for(const auto* key:{"AOS_ITEM_ID","AOS_SUBJECT_ID","AOS_INSTANCE_INDEX","AOS_INSTANCE_ID"}) {
   auto invalid=environment;invalid.erase(key);rejects([&]{parse_service_inputs(release,invalid);});
  }
  for(const auto* key:{"AOS_ITEM_ID","AOS_SUBJECT_ID","AOS_INSTANCE_ID"}) {
   for(const auto& value:std::vector<std::string>{"","../x","x\n",std::string(129,'x'),"a/b","é"}) {
    auto invalid=environment;invalid[key]=value;rejects([&]{parse_service_inputs(release,invalid);});
   }
  }
  for(const auto* key:{"serviceVersion","serviceArtifactSha256","serviceInstance","AOS_ITEM_ID","extra"}) {
   auto invalid=parse_json(public_input).object();invalid[key]=Json{std::string("override")};
   rejects([&]{parse_metadata(encode_json(Json{invalid}),native);});
  }
  auto legacy=parse_json(public_input).object();legacy["schemaVersion"]=Json{std::int64_t{1}};
  legacy["serviceVersion"]=Json{std::string("3.0.0")};legacy["serviceArtifactSha256"]=Json{std::string(64,'a')};
  const auto old=encode_json(Json{legacy});
  rejects([&]{parse_metadata(old,native);});
  CHECK(!parse_legacy_metadata(old).service_instance);
  CHECK(encode_json(metadata_binding(parse_metadata_binding(Json{legacy})))==old);
  auto invalid_binding=binding.object();invalid_binding["serviceArtifactSha256"]=Json{std::string(64,'0')};
  rejects([&]{parse_metadata_binding(Json{invalid_binding});});
  auto mixed=m;mixed.service_artifact_sha256=std::string(64,'0');
  rejects([&]{metadata_binding(mixed);});
  for(const auto& text:std::vector<std::string>{R"({"schemaVersion":1,"schemaVersion":1,"serviceVersion":"18.0.0"})",R"({"schemaVersion":1,"serviceVersion":"18.0.0","extra":true})"}) {
   rejects([&]{parse_service_inputs(text,environment);});
  }
  ApplicationInputs inputs;
  rejects([&]{runtime_metadata(inputs,public_input);});
  inputs.native=native;
  CHECK(runtime_metadata(inputs,public_input).service_instance==native.instance);
  auto changed=parse_json(public_input).object();changed["vdpContractVersion"]=Json{std::string("4.0.0")};
  const auto updated=runtime_metadata(inputs,encode_json(Json{changed}));
  CHECK(updated.service_version==m.service_version && updated.service_instance==m.service_instance);
  std::cout<<"PASS closed package/public/native readers and private provenance binding\n";
 } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
