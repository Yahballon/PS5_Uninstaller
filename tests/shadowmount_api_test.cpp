// SPDX-License-Identifier: GPL-3.0-or-later
#include "../native/shadowmount_api.hpp"
#include <cassert>
#include <cstdio>
using namespace uninstaller::shadow;
int main(){
  const std::string ok=R"({"status":0,"title_id":"PPSA21564","path":"/mnt/usb0/etaHEN/games/AstroBot","source_type":"folder","can_manage_source":true,"source_available":true,"mounted":true})";
  const Source source=parseSource(ok,"PPSA21564");
  assert(source.valid&&source.apiAvailable&&source.sourceKnown);
  assert(source.title=="PPSA21564");
  assert(source.path=="/mnt/usb0/etaHEN/games/AstroBot");
  assert(!parseSource(ok,"PPSA21567").valid);
  const auto pkg=parseSource(R"({"status":0,"title_id":"PPSA21564","path":"/user/app/PPSA21564/app.pkg","source_type":"pkg","can_manage_source":false,"source_available":true})","PPSA21564");
  assert(pkg.apiAvailable&&!pkg.valid&&!pkg.sourceKnown);
  const auto blocked=parseSource(R"({"status":0,"title_id":"PPSA21564","path":"/mnt/usb0/PPSA21564-app","source_type":"folder","can_manage_source":false,"source_available":true})","PPSA21564");
  assert(blocked.apiAvailable&&!blocked.valid&&blocked.sourceKnown);
  const auto missing=parseSource(R"({"status":0,"title_id":"PPSA21564","path":"/mnt/usb0/PPSA21564-app","source_type":"folder","can_manage_source":true,"source_available":false})","PPSA21564");
  assert(missing.sourceKnown&&!missing.valid);
  const auto system=parseSource(R"({"status":0,"title_id":"PPSA21564","path":"/user/app/PPSA21564","source_type":"folder","can_manage_source":true,"source_available":true})","PPSA21564");
  assert(system.sourceKnown&&!system.valid);
  assert(!allowedSourcePath("/mnt/usb0/foo/.."));
  assert(!allowedSourcePath("/mnt/usb0/foo/."));
  assert(!allowedSourcePath("/mnt/usb0/../fake"));
  assert(allowedSourcePath("/mnt/usb0/PPSA21564-app"));
  assert(allowedSourcePath("/mnt/ext0/games/PPSA21564"));
  assert(allowedSourcePath("/data/etaHEN/games/PPSA21564"));
  // Previously the shared inventory field() silently truncated after 95 bytes.
  const std::string longPath="/mnt/usb0/games/"+std::string(160,'A')+"/PPSA21564";
  const auto large=parseSource(std::string(R"({"status":0,"title_id":"PPSA21564","path":")")+
      longPath+R"(","source_type":"folder","can_manage_source":true,"source_available":true})","PPSA21564");
  assert(large.valid&&large.path==longPath);
  assert(number(R"({"status":0,"job_id":42})","job_id")==42);
  assert(flag(R"({"can_manage_source":true})","can_manage_source"));
  const std::string done=R"({"status":0,"job_id":42,"operation":"delete","state":"completed","title_id":"PPSA21564","source":"/mnt/usb0/PPSA21564-app","progress_percent":100,"result_status":0})";
  const Job job=parseJob(done,42,"PPSA21564","/mnt/usb0/PPSA21564-app");
  assert(job.finished&&job.success&&!job.error&&job.progress==100);
  assert(parseJob(done,43,"PPSA21564","/mnt/usb0/PPSA21564-app").error);
  assert(parseJob(done,42,"PPSA21567","/mnt/usb0/PPSA21564-app").error);
  assert(parseJob(done,42,"PPSA21564","/mnt/usb0/PPSA99794-app").error);
  const Job failed=parseJob(R"({"status":0,"job_id":42,"operation":"delete","state":"failed","title_id":"PPSA21564","source":"/mnt/usb0/PPSA21564-app","progress_percent":85,"result_status":5,"result_error":"access denied"})",
      42,"PPSA21564","/mnt/usb0/PPSA21564-app");
  assert(failed.finished&&!failed.success&&failed.error);
  assert(!validId("PPSA99986"));
  assert(!validId("PPSA2221564"));
  puts("PASS: verified ShadowMount sources, API permissions and authenticated deletion job identity");
}
