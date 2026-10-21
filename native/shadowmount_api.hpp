// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// ShadowMountPlus 1.7 official HTTP API: verified game sources only.
// No arbitrary filesystem deletion, shell commands, or direct mount operations.
#include "inventory.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
namespace uninstaller::shadow {
struct Reply {int http=0;std::string body,error;};
inline bool validId(const std::string& id){
  return validTitleId(id) && id!="PPSA99986";
}
inline int number(const std::string& json,const char* key,int fallback=-1){
  std::string tag=std::string("\"")+key+"\"";
  size_t pos=json.find(tag);
  if(pos==std::string::npos)return fallback;
  pos=json.find(':',pos+tag.size());
  if(pos==std::string::npos)return fallback;
  const char* p=json.c_str()+pos+1;
  while(*p==' '||*p=='\t')++p;
  if(*p<'0'||*p>'9')return fallback;
  const long n=std::strtol(p,nullptr,10);
  return n>=0&&n<=2147483647?(int)n:fallback;
}
inline bool flag(const std::string& json,const char* key){
  std::string tag=std::string("\"")+key+"\"";
  size_t p=json.find(tag);
  if(p==std::string::npos)return false;
  p=json.find(':',p+tag.size());
  if(p==std::string::npos)return false;
  p=json.find_first_not_of(" \t",p+1);
  return p!=std::string::npos&&json.compare(p,4,"true")==0;
}
inline Reply post(const char* route,const std::string& json){
  Reply r;
  if(!route||std::strlen(route)>92||route[0]!='/'||
     json.size()>250)return {0,"","Unsupported API request"};
  int fd=socket(AF_INET,SOCK_STREAM,0);
  if(fd<0)return {0,"","Cannot open local API socket"};
  timeval timeout{3,0};
  setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
  setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
  sockaddr_in addr{};addr.sin_family=AF_INET;
  addr.sin_port=htons(10101);addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  if(connect(fd,(sockaddr*)&addr,sizeof addr)!=0){
    close(fd);return {0,"","ShadowMountPlus 1.7 API not reachable on 127.0.0.1:10101"};
  }
  const std::string req=std::string("POST ")+route+
     " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"+
     "Connection: close\r\nContent-Length: "+std::to_string(json.size())+
     "\r\n\r\n"+json;
  size_t off=0;
  while(off<req.size()){
    ssize_t n=send(fd,req.data()+off,req.size()-off,0);
    if(n<=0){close(fd);return {0,"","Cannot send ShadowMount request"};}
    off+=(size_t)n;
  }
  std::string response;response.reserve(4096);
  char buffer[4096];
  while(response.size()<128*1024){
    const ssize_t n=recv(fd,buffer,sizeof buffer,0);
    if(n<=0)break;
    response.append(buffer,(size_t)n);
  }
  close(fd);
  const size_t split=response.find("\r\n\r\n");
  if(split==std::string::npos||
     std::sscanf(response.c_str(),"HTTP/%*d.%*d %d",&r.http)!=1){
    r.error="Invalid ShadowMount HTTP response";return r;
  }
  r.body=response.substr(split+4);
  if(r.http<200||r.http>=300){
    const std::string detail=field(r.body,"error");
    r.error="ShadowMount HTTP "+std::to_string(r.http)+(detail.empty()?"":": "+detail);
  }
  return r;
}
// apiAvailable means the ShadowMount service/capability was reachable;
 // sourceKnown prevents bypassing its safety refusal with raw unlink.
struct Source {
  bool valid=false,apiAvailable=false,sourceKnown=false;
  std::string title,path,type,error;
};
inline std::string jsonString(const std::string& body,const char* key,size_t limit=900){
  const std::string needle=std::string("\"")+key+"\"";
  size_t pos=body.find(needle);
  if(pos==std::string::npos)return {};
  pos=body.find(':',pos+needle.size());if(pos==std::string::npos)return {};
  pos=body.find_first_not_of(" \t\r\n",pos+1);
  if(pos==std::string::npos||body[pos]!='"')return {};
  std::string value;
  bool escaped=false;
  for(size_t i=pos+1;i<body.size();i++){
    const char c=body[i];
    if(escaped){
      if(c!='"'&&c!='\\'&&c!='/')return {};
      value+=c;escaped=false;
    }else if(c=='\\'){escaped=true;}
    else if(c=='"')return value;
    else if((unsigned char)c<32)return {};
    else value+=c;
    if(value.size()>limit)return {};
  }
  return {};
}
inline bool allowedSourcePath(const std::string& path){
  if(path.size()<17||path.size()>900||path.find("//")!=std::string::npos ||
      path.find("/../")!=std::string::npos||path.find("/./")!=std::string::npos ||
      path.back()=='/'||path.find('\\')!=std::string::npos)return false;
  for(unsigned char c:path)if(c<32||c==127)return false;
  // ShadowMount's own source policy remains authoritative; these are
  // additional hard exclusions for system files and mount targets.
  bool approved=path.rfind("/mnt/usb",0)==0||
                path.rfind("/mnt/ext",0)==0||
                path.rfind("/data/",0)==0;
  if(!approved||path.rfind("/user/",0)==0||
     path.rfind("/system",0)==0)return false;
  // Reject path traversal even if the last component is .. or .
  size_t segmentStart=1;
  unsigned segments=0;
  while(segmentStart<path.size()){
    const size_t end=path.find('/',segmentStart);
    const std::string segment=path.substr(segmentStart,
        end==std::string::npos?std::string::npos:end-segmentStart);
    if(segment.empty()||segment=="."||segment=="..")return false;
    segments++;
    if(end==std::string::npos)break;
    segmentStart=end+1;
  }
  return segments>=3;
}
inline Source parseSource(const std::string& body,const std::string& requested){
  Source s;
  s.apiAvailable=true;
  s.title=jsonString(body,"title_id",20);
  s.path=jsonString(body,"path");
  s.type=jsonString(body,"source_type",16);
  if(number(body,"status")!=0){s.error="Game info returned an error";return s;}
  if(s.title!=requested || !validId(requested)){
    s.error="ShadowMount title ID mismatch";return s;
  }
  if(s.type=="pkg"){s.error="Installed PKG: use native AppInstUtil";return s;}
  s.sourceKnown=(s.type=="folder"||s.type=="image");
  if(!s.sourceKnown){s.error="Unknown ShadowMount source type";return s;}
  if(!flag(body,"can_manage_source")||!flag(body,"source_available")){
    s.error="ShadowMount forbids deleting this source (busy, shared or missing)";
    return s;
  }
  if(!allowedSourcePath(s.path)){
    s.error="ShadowMount source path is outside approved data/USB storage";
    return s;
  }
  s.valid=true;return s;
}
inline Source inspect(const std::string& title){
  Source s;
  if(!validId(title)){s.error="Invalid title ID";return s;}
  Reply v=post("/api/v1/version","{}");
  if(v.http!=200||number(v.body,"status")!=0||
     v.body.find("\"delete_game_source\"")==std::string::npos){
    s.error="ShadowMountPlus 1.7 delete API not available";return s;
  }
  s.apiAvailable=true;
  Reply info=post("/api/v1/games/info",std::string("{\"title_id\":\"")+title+"\"}");
  if(info.http==404){s.error="Title not present in ShadowMount library";return s;}
  if(info.http!=200){s.error=info.error; s.sourceKnown=true;return s;}
  return parseSource(info.body,title);
}
struct DeleteResult{
  bool queued=false,absent=false;
  int job=-1;
  std::string message;
};
inline DeleteResult remove(const std::string& title,const std::string& expected){
  DeleteResult out;
  // Re-validate the physical source immediately before issuing a destructive job.
  const Source source=inspect(title);
  if(!source.valid){out.message=source.error;return out;}
  if(expected.empty()||source.path!=expected){
    out.message="Source path changed. No deletion submitted; inspect again";return out;
  }
  Reply response=post("/api/v1/games/delete",
       std::string("{\"title_id\":\"")+title+"\",\"confirm\":true}");
  if(response.http==202&&number(response.body,"status")==0){
    out.job=number(response.body,"job_id");
    out.queued=out.job>0;
    out.message=out.queued?"ShadowMount source deletion queued (job "+
        std::to_string(out.job)+")":"No valid ShadowMount job identifier";
  }else if(response.http==200&&number(response.body,"status")==0){
    out.absent=true;
    out.message="Source already absent. ShadowMount library reconciliation queued";
  }else out.message=response.error.empty()?"ShadowMount refused source deletion":response.error;
  return out;
}
struct Job{
  bool finished=false,success=false,error=false;
  int progress=0;
  std::string label;
};
inline Job parseJob(const std::string& body,int expectedJob,
                    const std::string& expectedTitle,
                    const std::string& expectedSource){
  Job result;
  if(number(body,"status")!=0 ||
     number(body,"job_id")!=expectedJob ||
     jsonString(body,"title_id",20)!=expectedTitle ||
     jsonString(body,"source")!=expectedSource ||
     jsonString(body,"operation",15)!="delete"){
    result.error=true;result.label="Unexpected ShadowMount job identity";return result;
  }
  const std::string state=jsonString(body,"state",20);
  result.progress=number(body,"progress_percent",0);
  if(result.progress>100)result.progress=100;
  result.finished=state=="completed"||state=="failed"||state=="cancelled";
  result.success=state=="completed"&&number(body,"result_status",1)==0;
  result.error=state=="failed"||state=="cancelled" ||
               (state=="completed"&&!result.success);
  result.label=result.success?"ShadowMount source deletion completed":
    result.error?"ShadowMount delete "+state+": "+jsonString(body,"result_error",100):
    "ShadowMount: "+state+" "+std::to_string(result.progress)+"%";
  return result;
}
inline Job status(int job,const std::string& title,const std::string& source){
  Job result;
  if(job<=0){result.error=true;result.label="Invalid ShadowMount job";return result;}
  Reply r=post("/api/v1/games/storage/status",
     std::string("{\"job_id\":")+std::to_string(job)+"}");
  if(r.http!=200||number(r.body,"status")!=0){
    result.label=r.error.empty()?"ShadowMount status temporarily unavailable":r.error;
    return result;
  }
  return parseJob(r.body,job,title,source);
}
} // namespace uninstaller::shadow
