// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#include "brake_health/runtime/application.hpp"
#include "brake_health/runtime/demo_no_telemetry.hpp"
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <sys/stat.h>
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
  const auto public_input=std::string(R"({"schemaVersion":2,"unitSystemUid":"fixture-unit","unitRole":"validation","vdpContractVersion":"1.0.1","vdpContractSha256":")")+std::string(64,'b')+"\"}";
  const auto m=parse_metadata(public_input,native);
  CHECK(m.service_version=="18.0.0" && m.service_artifact_sha256.empty());
  CHECK(m.vdp_contract_version=="1.0.1");
  // The family document is common to VDP V1/V2/V3. These readers must not
  // silently accept a proposed active-profile interface in legacy metadata.
  for(const auto* key:{"activeVdpProfile","activeVdpRelease","capabilities"}) {
   auto extended=parse_json(public_input).object();extended[key]=Json{std::string("unapproved")};
   rejects([&]{parse_metadata(encode_json(Json{extended}),native);});
  }
  // Explicit lifecycle mode stays alive without AOS_SECRET and shuts down
  // cleanly for a native update; Production is rejected before starting.
  auto production=m; production.unit_role=brake_health::v1::UnitRole::Production;
  rejects([&]{run_demo_no_telemetry(production);});
  for (const int stop_signal : {SIGTERM, SIGINT}) {
   int output[2]; CHECK(::pipe(output)==0);
   const auto pid=::fork(); CHECK(pid>=0);
   if(pid==0) {
    ::close(output[0]); ::dup2(output[1],STDOUT_FILENO); ::close(output[1]);
    ::unsetenv("AOS_SECRET"); ::unsetenv("KUKSA_TOKEN_FILE");
    ::_exit(run_demo_no_telemetry(m));
   }
   ::close(output[1]);
   pollfd ready{output[0],POLLIN,0};
   const int observed=::poll(&ready,1,2000);
   char bytes[512]{}; const auto count=observed>0 ? ::read(output[0],bytes,sizeof(bytes)) : 0;
   int status=0; const bool alive=::waitpid(pid,&status,WNOHANG)==0;
   ::kill(pid,stop_signal);
   if(observed<=0) ::kill(pid,SIGKILL);
   ::waitpid(pid,&status,0); ::close(output[0]);
   CHECK(observed>0 && count>0 && alive);
   CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0);
   const auto event=parse_json(std::string(bytes,static_cast<std::size_t>(count))).object();
   CHECK(event.at("reasonCode").string()=="TELEMETRY_DISABLED");
   CHECK(event.at("currentState").string()=="NOT_READY");
   CHECK(event.at("serviceVersion").string()=="18.0.0");
  }
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
  {
   char path[]="/tmp/initial-public-inputs-XXXXXX";
   CHECK(::mkdtemp(path)!=nullptr);
   const std::filesystem::path root(path);
   struct Cleanup {std::filesystem::path p;~Cleanup(){std::filesystem::remove_all(p);}} cleanup{root};
   ApplicationInputs early{root/"metadata.json",root/"trust.pem",native};
   const auto put=[](const std::filesystem::path& file,const std::string& text) {
    std::ofstream stream(file);stream<<text;stream.close();CHECK(stream.good());
   };
   // Reproduce the original bootstrap's fatal read on first-input absence.
   rejects([&]{read_file(early.metadata_file,8192);});
   CHECK(!initial_runtime_metadata(early));
   put(early.metadata_file,public_input);
   CHECK(!initial_runtime_metadata(early)); // Trust has not arrived.
   put(early.ca_file,"public-trust-fixture");
   CHECK(initial_runtime_metadata(early)->service_instance==native.instance);
   put(early.metadata_file,"{}");
   rejects([&]{initial_runtime_metadata(early);});
   put(early.metadata_file,public_input);
   put(early.ca_file,"");
   rejects([&]{initial_runtime_metadata(early);});
   put(early.ca_file,std::string(65537,'x'));
   rejects([&]{initial_runtime_metadata(early);});
   std::filesystem::remove(early.ca_file);
   std::filesystem::create_symlink(root/"missing",early.ca_file);
   rejects([&]{initial_runtime_metadata(early);});
   std::filesystem::remove(early.ca_file);
   CHECK(::mkfifo(early.ca_file.c_str(),0600)==0);
   rejects([&]{initial_runtime_metadata(early);}); // Must not hang on a FIFO.
   std::filesystem::remove(early.ca_file);
   put(early.ca_file,"public-trust-fixture");
   CHECK(initial_runtime_metadata(early)->service_version==native.service_version);
   early.native.reset();
   std::filesystem::remove(early.metadata_file);
   rejects([&]{initial_runtime_metadata(early);}); // Missing identity is not waiting.
  }
  auto changed=parse_json(public_input).object();changed["vdpContractVersion"]=Json{std::string("4.0.0")};
  const auto updated=runtime_metadata(inputs,encode_json(Json{changed}));
  CHECK(updated.service_version==m.service_version && updated.service_instance==m.service_instance);
  std::cout<<"PASS closed package/public/native readers and private provenance binding\n";
 } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
