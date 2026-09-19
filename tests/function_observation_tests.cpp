// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#include "brake_health/runtime/function_observation.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace brake_health::runtime;
using Stream=ObservationStream;using J=Json;using O=J::Object;using A=J::Array;
namespace {
template<class F>void rejects(F fn){bool rejected=false;try{fn();}catch(...){rejected=true;}assert(rejected);}
struct Temp{std::filesystem::path root;Temp(){char value[]="/tmp/observation-XXXXXX";root=::mkdtemp(value);}~Temp(){std::filesystem::remove_all(root);}};
J s(const std::string& v){return J{v};}J n(std::int64_t v){return J{v};}
J envelope(){return J{O{{"messageType",s("BRAKE_FUNCTION_OBSERVATION")},{"unitSystemUid",s("test-fixture")},{"unitRole",s("VALIDATION")},{"serviceVersion",s("10.0.0")},{"serviceProfile",s("v3")},{"serviceInstance",J{O{{"serviceId",s("service")},{"subjectId",s("subject")},{"instanceIndex",n(0)},{"instanceId",s("instance")}}}}}};}
J content(){return J{O{{"connection",s("STARTING")},{"input",J{O{{"state",s("WAITING")},{"reason",s("AWAITING_INPUT")}}}},{"activity",J{O{{"state",s("WAITING")},{"reason",s("NOT_QUALIFIED")},{"episodeId",J{nullptr}}}}},{"delivery",J{O{{"state",s("IDLE")},{"queuedMessages",n(0)},{"lastReceiptAt",J{nullptr}}}}},{"advisory",J{O{{"state",s("WAITING")},{"requestId",J{nullptr}}}}},{"lastResult",J{nullptr}}}};}
std::string ack(const std::string& bytes,bool duplicate=false) {
 const auto m=ObservationCodec::parse(bytes,8192);
 return ObservationCodec::encode(J{O{{"schemaVersion",n(1)},{"contractVersion",s("1.0.0")},
  {"receiptId",s("11111111-1111-4111-8111-111111111111")},{"messageKeySha256",s(Stream::validate(m))},
  {"contentSha256",m.at("contentSha256")},{"state",s(duplicate?"DUPLICATE_ACCEPTED":"DURABLE_ACCEPTED")},
  {"receivedAt",s("2026-09-18T12:00:01.000Z")}}});
}
constexpr std::int64_t now=1789732800000LL;
void cadence_and_ack() {
 Temp t;Stream stream(t.root/"delivery",envelope());assert(stream.generation()==1&&!stream.next());
 assert(stream.observe(content(),now,0));assert(!stream.observe(content(),now+4999,4999));
 assert(!stream.observe(content(),now+29999,29999));assert(stream.observe(content(),now+30000,30000));
 const auto first=*stream.next();assert(*stream.next()==first);
 assert(!stream.accept(first,500,ack(first)));assert(!stream.accept(first+" ",201,ack(first)));
 auto bad=ObservationCodec::parse(ack(first),8192).object();bad["contentSha256"]=s(std::string(64,'0'));
 assert(!stream.accept(first,201,ObservationCodec::encode(J{bad})));assert(stream.queued()==2);
 assert(stream.accept(first,201,ack(first)));const auto second=*stream.next();
 assert(stream.accept(second,200,ack(second,true)));assert(stream.queued()==0);
 rejects([&]{Stream concurrent(t.root/"delivery",envelope());});
}
void restart_and_bound() {
 Temp t;std::ofstream(t.root/"product.json")<<"original-product";std::string first;
 {
  Stream stream(t.root/"delivery",envelope());stream.observe(content(),now,0);first=*stream.next();
  for(int i=1;i<70;++i){auto c=content().object();auto d=c.at("delivery").object();d["queuedMessages"]=n(i);c["delivery"]=J{d};assert(stream.observe(J{c},now+i*5000,i*5000));}
  assert(stream.queued()==64&&*stream.next()==first);
 }
 {
  auto v=envelope().object();v["serviceVersion"]=s("11.0.0");
  auto instance=v.at("serviceInstance").object();instance["instanceId"]=s("replacement-instance");v["serviceInstance"]=J{instance};
  Stream repaired(t.root/"delivery",J{v});assert(repaired.generation()==2&&repaired.queued()==64);
  assert(*repaired.next()==first);assert(repaired.observe(content(),now+999999,0));
  assert(repaired.queued()==64);std::int64_t previous_gen=0,previous_seq=0;
  while(const auto pending=repaired.next()){
   const auto m=ObservationCodec::parse(*pending,8192);const auto gen=m.at("generation").integer(),seq=m.at("sequence").integer();
   assert(gen>previous_gen||(gen==previous_gen&&seq>previous_seq));previous_gen=gen;previous_seq=seq;
   assert(repaired.accept(*pending,200,ack(*pending,true)));
  }
  assert(previous_gen==2&&previous_seq==1);
 }
 auto different=envelope().object();different["unitSystemUid"]=s("foreign");
 rejects([&]{Stream foreign(t.root/"delivery",J{different});});
 std::ifstream product(t.root/"product.json");std::string bytes;product>>bytes;assert(bytes=="original-product");
}
void rollback_and_uncertainty(){
 for(const std::string stage:{"write","rename","directory-sync"}) {
  Temp t;std::string first;bool armed=false;
  {
   Stream stream(t.root/"delivery",envelope(),[&](const char* s){if(armed&&stage==s)throw std::runtime_error("INJECTED_STORAGE_FAILURE");});
   stream.observe(content(),now,0);first=*stream.next();armed=true;
   rejects([&]{stream.observe(content(),now+30000,30000);});
   rejects([&]{(void)stream.next();}); // Do not continue after an uncertain persistence result.
  }
  Stream recovered(t.root/"delivery",envelope());assert(recovered.generation()==2&&*recovered.next()==first);
  assert(recovered.queued()==(stage=="directory-sync"?2:1));
 }
}
void negative(){
 Temp t;Stream stream(t.root/"delivery",envelope());stream.observe(content(),now,0);
 const auto original=ObservationCodec::parse(*stream.next(),8192);
 auto bad=original.object();bad["unknown"]=n(1);rejects([&]{Stream::validate(J{bad});});
 bad=original.object();bad["sequence"]=n(0);rejects([&]{Stream::validate(J{bad});});
 bad=original.object();bad["observedAt"]=s("2026-02-30T12:00:00.000Z");rejects([&]{Stream::validate(J{bad});});
 auto c=content().object();c["input"]=J{O{{"state",s("RECEIVING")},{"reason",s("SOURCE_GAP")}}};
 rejects([&]{stream.observe(J{c},now+30000,30000);});
 c=content().object();c["advisory"]=J{O{{"state",s("CONFIRMED")},{"requestId",J{nullptr}}}};
 rejects([&]{stream.observe(J{c},now+30000,30000);});
 c=content().object();c["activity"]=J{O{{"state",s("SKIPPED")},{"reason",s("NONE")},{"episodeId",J{nullptr}}}};
 rejects([&]{stream.observe(J{c},now+30000,30000);});
}
void filesystem_negatives(){
 for(int kind=0;kind<3;++kind){
  Temp t;{
   Stream stream(t.root/"delivery",envelope());stream.observe(content(),now,0);
  }
  const auto ledger=t.root/"delivery/ledger.json";
  if(kind==0)::chmod(ledger.c_str(),0644);
  if(kind==1){std::ofstream f(ledger);f<<"{}";}
  if(kind==2){std::filesystem::rename(ledger,t.root/"original");std::filesystem::create_symlink(t.root/"original",ledger);}
  rejects([&]{Stream bad(t.root/"delivery",envelope());});
 }
}
}
int main(int argc,char**argv){
 if(argc==2&&std::string(argv[1])=="--emit-conformance"){
  Temp t;
  for(const std::string profile:{"v1","v2","v3"}){
   auto base=envelope().object();base["serviceProfile"]=s(profile);
   auto binding=base.at("serviceInstance").object();binding["instanceId"]=s("instance-"+profile);base["serviceInstance"]=J{binding};
   Stream stream(t.root/profile,J{base});auto c=content().object();
   if(std::string("brake")=="brake"&&profile!="v3")c["advisory"]=J{O{{"state",s("NOT_SUPPORTED")},{"requestId",J{nullptr}}}};
   stream.observe(J{c},now,0);const auto first=*stream.next();std::cout<<first<<"\n";assert(stream.accept(first,201,ack(first)));
   c["connection"]=s("CONNECTED");c["input"]=J{O{{"state",s("RECEIVING")},{"reason",s("NONE")}}};
   stream.observe(J{c},now+5000,5000);std::cout<<*stream.next()<<"\n";
  }
  return 0;
 }
 cadence_and_ack();restart_and_bound();rollback_and_uncertainty();negative();filesystem_negatives();
 std::cout<<"PASS observation generation, cadence, bounded exact retry, receipt, rollback and schema\n";
}
