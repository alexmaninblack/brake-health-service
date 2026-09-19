// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#pragma once
// Function-observation v3: private delivery allocation, not model or advisory state.
// The two packaged copies are intentionally byte-identical.
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace aosedge {
template<class Codec> class FunctionObservation {
public:
 using J = typename Codec::Json;
 using O = typename J::Object;
 using A = typename J::Array;
 using Fault = std::function<void(const char*)>;
 static constexpr std::int64_t maximum_integer=9007199254740991LL;
 static J text(const std::string& value) {return J{value};}
 static J number(std::int64_t value) {return J{value};}
 static void fail() {throw std::runtime_error("FUNCTION_OBSERVATION_UNAVAILABLE");}
 static bool null(const J& value) {return std::holds_alternative<std::nullptr_t>(value.value);}
 static void keys(const J& value,std::initializer_list<const char*> names) {
  if(value.object().size()!=names.size())fail();
  for(const auto* name:names)(void)value.at(name);
 }
 static void pick(const J& value,std::initializer_list<const char*> values) {
  if(std::none_of(values.begin(),values.end(),[&](const char* s){return value.string()==s;}))fail();
 }
 static void id(const J& value) {
  const auto& s=value.string();
  const auto alnum=[](char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9');};
  if(s.empty()||s.size()>128||!alnum(s[0])||
     std::any_of(s.begin(),s.end(),[&](char c){return !alnum(c)&&c!='_'&&c!='.'&&c!=':'&&c!='-';}))fail();
 }
 static void integer(const J& value,bool positive=false) {
  if(value.integer()<(positive?1:0)||value.integer()>maximum_integer)fail();
 }
 static void version(const J& value) {
  const auto& s=value.string();if(s.empty()||s.size()>32)fail();
  std::size_t begin=0;
  for(int i=0;i<3;++i) {
   const auto end=s.find('.',begin);const auto count=(end==std::string::npos?s.size():end)-begin;
   if(!count||(count>1&&s[begin]=='0')||(i==2)!=(end==std::string::npos))fail();
   for(std::size_t j=begin;j<begin+count;++j)if(s[j]<'0'||s[j]>'9')fail();
   begin=end==std::string::npos?s.size():end+1;
  }
 }
 static void instant(const J& value) {
  const auto& s=value.string();
  if(s.size()!=24||s[4]!='-'||s[7]!='-'||s[10]!='T'||s[13]!=':'||s[16]!=':'||s[19]!='.'||s[23]!='Z')fail();
  const auto n=[&](std::size_t p,std::size_t length){int v=0;for(std::size_t i=p;i<p+length;++i){if(s[i]<'0'||s[i]>'9')fail();v=v*10+s[i]-'0';}return v;};
  const int y=n(0,4),m=n(5,2),d=n(8,2);const int days[]={0,31,28,31,30,31,30,31,31,30,31,30,31};
  if(m<1||m>12||d<1||d>days[m]+(m==2&&y%4==0&&(y%100!=0||y%400==0))||n(11,2)>23||n(14,2)>59||n(17,2)>59)fail();
  (void)n(20,3);
 }
 static void content(const J& c,const std::string& profile,bool brake) {
  keys(c,{"connection","input","activity","delivery","advisory","lastResult"});
  pick(c.at("connection"),{"STARTING","CONNECTED","REAUTHENTICATING","DISCONNECTED","ACCESS_DENIED"});
  const auto& input=c.at("input");keys(input,{"state","reason"});
  pick(input.at("state"),{"WAITING","RECEIVING","STALE","DISCONNECTED","ACCESS_DENIED","INVALID"});
  pick(input.at("reason"),{"NONE","AWAITING_INPUT","SOURCE_GAP","INVALID_SAMPLE","TRANSPORT_LOST","ACCESS_DENIED","REAUTHENTICATING"});
  if((input.at("state").string()=="RECEIVING")!=(input.at("reason").string()=="NONE"))fail();
  const auto& activity=c.at("activity");keys(activity,{"state","reason","episodeId"});
  pick(activity.at("state"),{"WAITING","PRE","ACTIVE","POST","COMPLETED","SKIPPED"});
  pick(activity.at("reason"),{"NONE","NOT_QUALIFIED","INSUFFICIENT_SAMPLES","INVALID_INPUT","SOURCE_DISCONTINUITY","REAUTHENTICATING","RESET","STORAGE_UNAVAILABLE"});
  if(!null(activity.at("episodeId")))id(activity.at("episodeId"));
  if(activity.at("state").string()=="SKIPPED"&&activity.at("reason").string()=="NONE")fail();
  const auto& delivery=c.at("delivery");keys(delivery,{"state","queuedMessages","lastReceiptAt"});
  pick(delivery.at("state"),{"IDLE","PENDING","RETRYING","BLOCKED"});integer(delivery.at("queuedMessages"));
  if(!null(delivery.at("lastReceiptAt")))instant(delivery.at("lastReceiptAt"));
  const auto& advisory=c.at("advisory");keys(advisory,{"state","requestId"});
  pick(advisory.at("state"),{"NOT_SUPPORTED","WAITING","CONFIRMED","UNAVAILABLE","REAUTHENTICATING"});
  if(!null(advisory.at("requestId")))id(advisory.at("requestId"));
  if(advisory.at("state").string()=="CONFIRMED"&&null(advisory.at("requestId")))fail();
  if(brake&&profile!="v3") {
   if(advisory.at("state").string()!="NOT_SUPPORTED"||!null(advisory.at("requestId")))fail();
  } else if(advisory.at("state").string()=="NOT_SUPPORTED")fail();
  const auto& result=c.at("lastResult");
  if(!null(result)){keys(result,{"kind","id","sourceTime","serviceVersion"});pick(result.at("kind"),{"WINDOW","ASSESSMENT"});id(result.at("id"));instant(result.at("sourceTime"));version(result.at("serviceVersion"));}
 }
 static std::string validate(const J& m) {
  keys(m,{"schemaVersion","contractVersion","messageType","unitSystemUid","unitRole","serviceVersion","serviceProfile","serviceInstance","generation","sequence","observedAt","content","contentSha256"});
  if(m.at("schemaVersion").integer()!=3||m.at("contractVersion").string()!="3.0.0")fail();
  pick(m.at("messageType"),{"BRAKE_FUNCTION_OBSERVATION","TIRE_FUNCTION_OBSERVATION"});
  const bool brake=m.at("messageType").string()=="BRAKE_FUNCTION_OBSERVATION";
  id(m.at("unitSystemUid"));pick(m.at("unitRole"),{"VALIDATION","PRODUCTION"});version(m.at("serviceVersion"));
  const auto& binding=m.at("serviceInstance");keys(binding,{"serviceId","subjectId","instanceIndex","instanceId"});
  id(binding.at("serviceId"));id(binding.at("subjectId"));id(binding.at("instanceId"));integer(binding.at("instanceIndex"));
  const auto profile=m.at("serviceProfile").string();pick(m.at("serviceProfile"),{"v1","v2","v3"});if(!brake&&profile!="v1")fail();
  integer(m.at("generation"),true);integer(m.at("sequence"),true);instant(m.at("observedAt"));content(m.at("content"),profile,brake);
  if(Codec::encode(m).size()>8192||m.at("contentSha256").string()!=Codec::digest(Codec::encode(m.at("content"))))fail();
  return Codec::digest(Codec::encode(J{A{m.at("unitSystemUid"),m.at("messageType"),binding,m.at("generation"),m.at("sequence")}}));
 }
 FunctionObservation(const std::filesystem::path& root,J envelope,Fault fault={})
  :base_(std::move(envelope)),fault_(std::move(fault)) {
  try {
   // Reject substituted ancestors; create only the private owned allocation.
   std::filesystem::path part;
   for(const auto& element:root){part/=element;struct stat st{};if(::lstat(part.c_str(),&st)==0&&S_ISLNK(st.st_mode))fail();}
   std::filesystem::create_directories(root);
   struct stat st{};
   if(::lstat(root.c_str(),&st)||!S_ISDIR(st.st_mode)||st.st_uid!=::geteuid())fail();
   if(::chmod(root.c_str(),0700))fail();
   dir_=::open(root.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
   if(dir_<0||::flock(dir_,LOCK_EX|LOCK_NB))fail();
   const auto binding=J{A{base_.at("unitSystemUid"),base_.at("messageType"),base_.at("serviceInstance")}};
   const auto existing=read("ledger.json");
   ledger_=existing?Codec::parse(*existing,700000):J{O{{"schemaVersion",number(1)},{"binding",binding},{"generation",number(0)},{"sequence",number(0)},{"pending",J{A{}}}}};
   keys(ledger_,{"schemaVersion","binding","generation","sequence","pending"});
   if(ledger_.at("schemaVersion").integer()!=1||!same_allocation(ledger_.at("binding"),binding))fail();
   integer(ledger_.at("generation"));integer(ledger_.at("sequence"));
   std::set<std::string> identities;
   const auto& pending=std::get<A>(ledger_.at("pending").value);if(pending.size()>64)fail();
   for(const auto& entry:pending) {
    keys(entry,{"attempted","message","digest"});(void)entry.at("attempted").boolean();
    const auto& m=entry.at("message");const auto key=validate(m);
    if(!identities.insert(key).second||entry.at("digest").string()!=Codec::digest(Codec::encode(m))||
       !same_allocation(J{A{m.at("unitSystemUid"),m.at("messageType"),m.at("serviceInstance")}},binding)||
       m.at("generation").integer()>ledger_.at("generation").integer()||
       (m.at("generation").integer()==ledger_.at("generation").integer()&&m.at("sequence").integer()>ledger_.at("sequence").integer()))fail();
   }
   if(ledger_.at("generation").integer()==maximum_integer)fail();
   auto next=ledger_.object();next["binding"]=binding;
   next["generation"]=number(ledger_.at("generation").integer()+1);next["sequence"]=number(0);
   // A stale private temp file was never committed; it is safe to replace under this lock.
   if(::unlinkat(dir_,"ledger.next",0)&&errno!=ENOENT)fail();
   commit(J{next});
  } catch(...){if(dir_>=0){::close(dir_);dir_=-1;}throw;}
 }
 ~FunctionObservation(){if(dir_>=0)::close(dir_);}
 FunctionObservation(const FunctionObservation&)=delete;
 FunctionObservation& operator=(const FunctionObservation&)=delete;
 std::int64_t generation()const{return ledger_.at("generation").integer();}
 std::size_t queued()const{return std::get<A>(ledger_.at("pending").value).size();}
 // Only the existing delivery worker calls these methods. Acquisition merely
 // supplies a bounded in-memory snapshot, never performs an HTTP or ledger write.
 bool observe(const J& c,std::int64_t wall,std::int64_t monotonic) {
  if(poisoned_)fail();
  content(c,base_.at("serviceProfile").string(),base_.at("messageType").string()=="BRAKE_FUNCTION_OBSERVATION");
  const auto bytes=Codec::encode(c);
  if(last_emit_>=0&&(monotonic<last_emit_||monotonic-last_emit_<5000||
     (bytes==last_content_&&monotonic-last_emit_<30000)))return false;
  auto entries=std::get<A>(ledger_.at("pending").value);
  if(entries.size()==64) {
   const auto unsent=std::find_if(entries.rbegin(),entries.rend(),[](const J& e){return !e.at("attempted").boolean();});
   if(unsent==entries.rend())return false;
   entries.erase(std::next(unsent).base());
  }
  if(ledger_.at("sequence").integer()==maximum_integer)fail();
  auto m=base_.object();m["schemaVersion"]=number(3);m["contractVersion"]=text("3.0.0");
  m["generation"]=ledger_.at("generation");m["sequence"]=number(ledger_.at("sequence").integer()+1);
  m["observedAt"]=text(Codec::timestamp(wall));m["content"]=c;m["contentSha256"]=text(Codec::digest(bytes));
  validate(J{m});
  entries.push_back(J{O{{"attempted",J{false}},{"message",J{m}},{"digest",text(Codec::digest(Codec::encode(J{m})))}}});
  auto next=ledger_.object();next["sequence"]=m.at("sequence");next["pending"]=J{entries};commit(J{next});
  last_emit_=monotonic;last_content_=bytes;return true;
 }
 std::optional<std::string> next() {
  if(poisoned_)fail();
  auto entries=std::get<A>(ledger_.at("pending").value);if(entries.empty())return std::nullopt;
  if(!entries.front().at("attempted").boolean()) {
   auto e=entries.front().object();e["attempted"]=J{true};entries.front()=J{e};
   auto next=ledger_.object();next["pending"]=J{entries};commit(J{next});
  }
  return Codec::encode(entries.front().at("message"));
 }
 bool accept(const std::string& attempted,int status,const std::string& response) {
  if(poisoned_)fail();
  if(status!=200&&status!=201)return false;
  auto entries=std::get<A>(ledger_.at("pending").value);
  if(entries.empty()||!entries.front().at("attempted").boolean()||Codec::encode(entries.front().at("message"))!=attempted)return false;
  try {
   const auto ack=Codec::parse(response,8192);keys(ack,{"schemaVersion","contractVersion","receiptId","messageKeySha256","contentSha256","state","receivedAt"});
   const auto& m=entries.front().at("message");
   if(ack.at("schemaVersion").integer()!=1||ack.at("contractVersion").string()!="1.0.0"||
      !Codec::uuid(ack.at("receiptId").string())||ack.at("messageKeySha256").string()!=validate(m)||
      ack.at("contentSha256").string()!=m.at("contentSha256").string()||
      ack.at("state").string()!=(status==201?"DURABLE_ACCEPTED":"DUPLICATE_ACCEPTED"))return false;
   instant(ack.at("receivedAt"));
  }catch(...){return false;}
  entries.erase(entries.begin());auto next=ledger_.object();next["pending"]=J{entries};commit(J{next});return true;
 }
private:
 static bool same_allocation(const J& left,const J& right) {
  const auto& a=std::get<A>(left.value);const auto& b=std::get<A>(right.value);
  if(a.size()!=3||b.size()!=3||Codec::encode(a[0])!=Codec::encode(b[0])||Codec::encode(a[1])!=Codec::encode(b[1]))return false;
  keys(a[2],{"serviceId","subjectId","instanceIndex","instanceId"});keys(b[2],{"serviceId","subjectId","instanceIndex","instanceId"});
  // Native process instance may change on SOTA. The existing service/Subject/
  // index allocation owns the monotonic reservation; retained envelope bytes
  // keep their original instance. No other Unit or Subject can inherit it.
  for(const auto* key:{"serviceId","subjectId","instanceIndex"})
   if(Codec::encode(a[2].at(key))!=Codec::encode(b[2].at(key)))return false;
  return true;
 }
 std::optional<std::string> read(const char* name) {
  const int fd=::openat(dir_,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
  if(fd<0){if(errno==ENOENT)return std::nullopt;fail();}
  struct Close{int fd;~Close(){::close(fd);}} close{fd};struct stat st{};
  if(::fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=::geteuid()||(st.st_mode&0777)!=0600||st.st_size<=0||st.st_size>700000)fail();
  std::string result;char buffer[4096];
  for(;;){auto n=::read(fd,buffer,sizeof(buffer));if(n<0&&errno==EINTR)continue;if(n<0)fail();if(!n)break;result.append(buffer,static_cast<std::size_t>(n));if(result.size()>700000)fail();}
  return result;
 }
 void commit(const J& next) {
  const auto bytes=Codec::encode(next);if(bytes.size()>700000)fail();
  const int fd=::openat(dir_,"ledger.next",O_CREAT|O_EXCL|O_WRONLY|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)fail();
  bool open=true;
  try {
   if(fault_)fault_("write");
   std::size_t at=0;while(at<bytes.size()){auto n=::write(fd,bytes.data()+at,bytes.size()-at);if(n<0&&errno==EINTR)continue;if(n<=0)fail();at+=static_cast<std::size_t>(n);}
   if(::fsync(fd))fail();
   ::close(fd);open=false;if(fault_)fault_("rename");
   if(::renameat(dir_,"ledger.next",dir_,"ledger.json"))fail();
   if(fault_)fault_("directory-sync");
   if(::fsync(dir_))fail();
   ledger_=next;
  }catch(...){poisoned_=true;if(open)::close(fd);::unlinkat(dir_,"ledger.next",0);throw;}
 }
 int dir_{-1};J base_,ledger_;Fault fault_;bool poisoned_{};std::int64_t last_emit_{-1};std::string last_content_;
};

// Snapshots are protected by the owning runtime's existing mutex. Setters only
// touch memory; HTTP, observation fsync and retries remain in its delivery worker.
template<class Codec> class FunctionFacts {
public:
 using J=typename Codec::Json;using O=typename J::Object;
 static J s(const std::string& v){return J{v};}
 explicit FunctionFacts(bool advisory):supports_(advisory) {
  value_=J{O{{"connection",s("STARTING")},{"input",J{O{{"state",s("WAITING")},{"reason",s("AWAITING_INPUT")}}}},
   {"activity",J{O{{"state",s("WAITING")},{"reason",s("NOT_QUALIFIED")},{"episodeId",J{nullptr}}}}},
   {"delivery",J{O{{"state",s("IDLE")},{"queuedMessages",J{std::int64_t{0}}},{"lastReceiptAt",J{nullptr}}}}},
   {"advisory",J{O{{"state",s(advisory?"WAITING":"NOT_SUPPORTED")},{"requestId",J{nullptr}}}}},{"lastResult",J{nullptr}}}};
 }
 void input(const std::string& connection,const std::string& state,const std::string& reason) {
  auto v=value_.object();v["connection"]=s(connection);v["input"]=J{O{{"state",s(state)},{"reason",s(reason)}}};value_=J{v};
 }
 void activity(const std::string& state,const std::string& reason,const std::optional<std::string>& id={}) {
  auto v=value_.object();v["activity"]=J{O{{"state",s(state)},{"reason",s(reason)},{"episodeId",id? s(*id):J{nullptr}}}};value_=J{v};
 }
 void interruption(const std::string& reason) {
  const auto phase=value_.at("activity").at("state").string();
  if(phase=="ACTIVE"||phase=="POST"||phase=="PRE") {
   const auto id=value_.at("activity").at("episodeId");
   activity("SKIPPED",reason,std::holds_alternative<std::nullptr_t>(id.value)?std::nullopt:std::optional<std::string>{id.string()});
  }
 }
 void advisory(const std::string& state,const std::optional<std::string>& request={}) {
  if(!supports_)return;
  auto v=value_.object();v["advisory"]=J{O{{"state",s(state)},{"requestId",request?s(*request):J{nullptr}}}};value_=J{v};
 }
 void result(const std::string& kind,const std::string& id,const std::string& source,const std::string& version) {
  auto v=value_.object();v["lastResult"]=J{O{{"kind",s(kind)},{"id",s(id)},{"sourceTime",s(source)},{"serviceVersion",s(version)}}};value_=J{v};
 }
 void delivery(bool accepted,int status,const std::string& receipt) {
  auto v=value_.object();auto d=v.at("delivery").object();
  d["state"]=s(accepted?"PENDING":(status==0||status==408||status==429||status>=500)?"RETRYING":"BLOCKED");
  if(accepted) {
   try {auto a=Codec::parse(receipt,8192);FunctionObservation<Codec>::instant(a.at("receivedAt"));d["lastReceiptAt"]=a.at("receivedAt");}catch(...){}
  }
  v["delivery"]=J{d};value_=J{v};
 }
 J snapshot(std::size_t queued,bool blocked=false)const {
  auto v=value_.object();auto d=v.at("delivery").object();d["queuedMessages"]=J{static_cast<std::int64_t>(queued)};
  if(blocked)d["state"]=s("BLOCKED");
  else if(!queued)d["state"]=s("IDLE");
  else if(d.at("state").string()=="IDLE")d["state"]=s("PENDING");
  v["delivery"]=J{d};return J{v};
 }
private:
 J value_;bool supports_;
};

} // namespace aosedge
