// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Uninstaller v0.1.9: one-shot capability-authenticated local ELF action.
// This never writes request files under the sandbox-inaccessible /data/actions.
#include "inventory.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>
namespace uninstaller::actions {
struct Result {
  bool launched=false,accepted=false,verified=false;
  int code=-1;
  std::string detail;
  std::string source;
};
inline bool safeUsbPath(const std::string& s){
  if(s.size()<18||s.size()>690||s.rfind("/mnt/usb",0)!=0 ||
     s[8]<'0'||s[8]>'7'||s[9]!='/'||s.back()=='/' ||
     s.find("//")!=std::string::npos||s.find("/../")!=std::string::npos ||
     s.find("/./")!=std::string::npos||s.find('\\')!=std::string::npos)return false;
  for(unsigned char ch:s)if(ch<32||ch==127)return false;
  return true;
}
inline bool usbHomePath(const std::string& s){
  if(!safeUsbPath(s)||s.compare(10,9,"homebrew/")!=0)return false;
  const std::string child=s.substr(19);
  return !child.empty() && child!="." && child!=".." && child[0]!='.' &&
         child.find('/')==std::string::npos;
}
inline bool eligible(const Item& item){
  if(!item.remote)return isAllowedRemoval(item);
  if(item.kind==Kind::Installed)
    return validTitleId(item.id)&&item.id!="PPSA99986" &&
           item.path=="/user/app/"+item.id;
  if(item.kind==Kind::UsbPackage)
    return safeUsbPath(item.path)&&endsPkg(item.path);
  if(item.kind==Kind::Homebrew)return usbHomePath(item.path);
  return false;
}
inline bool sendAll(int fd,const void* data,size_t n){
  const char* p=(const char*)data;
  while(n){
    const ssize_t sent=send(fd,p,n,0);
    if(sent<=0)return false;
    p+=(size_t)sent;n-=(size_t)sent;
  }
  return true;
}
inline bool capability(const std::string& s){
  if(s.size()!=32)return false;
  for(char ch:s)if(!((ch>='0'&&ch<='9')||(ch>='a'&&ch<='f')))return false;
  return true;
}
inline Result parseResponse(const std::string& reply,const std::string& token,Kind kind){
  Result result;
  const std::string marker="UREM2_RESULT "+token+" ";
  const auto at=reply.find(marker);
  if(at==std::string::npos){
    result.detail="No authenticated removal result; consult REMOVE_* logs";
    return result;
  }
  unsigned code=0,state=0;
  if(std::sscanf(reply.c_str()+at+marker.size(),"%8x %u",&code,&state)!=2){
    result.detail="Malformed removal reply";return result;
  }
  result.code=(int)code;
  result.accepted=(code==0&&(state==1||state==2||state==4||state==5));
  result.verified=(code==0&&(state==2||state==4));
  if(code!=0){
    char codeText[24];std::snprintf(codeText,sizeof codeText,"%08X",code);
    result.detail="Native remove failed: 0x"+std::string(codeText);
  }else if(result.verified){
    result.detail=kind==Kind::Installed?
       (state==4?"ShadowMount source deleted; registration cleanup pending":
                 "Game source deleted and PS5 uninstall requested"):
       kind==Kind::Homebrew?"USB homebrew folder removed":
                 "USB PKG file deleted (installed game unaffected)";
  }else if(state==5&&code==0){
    result.detail="Deferred ShadowMount deletion scheduled; close UI to begin";
  }else if(state==3&&code==0){
    result.detail="ShadowMount USB source scan completed";
  }else if(result.accepted){
    result.detail="PS5 accepted uninstall; rescan to verify actual removal";
  }else{
    result.detail="Native remove returned unknown state";
  }
  return result;
}
enum class Mode { Normal, ProbeGame, UninstallAll, DeferShadow };
inline Result execute(const Item& item,ScanLog report=nullptr,
                      const char* elf="/app0/unremove.elf",
                      Mode mode=Mode::Normal,const std::string& expectedSource=""){
  Result result;
  if(!item.remote || !eligible(item)){
    result.detail="Removal source not supported by native helper";return result;
  }
  FILE* file=std::fopen(elf,"rb");
  if(!file){result.detail="unremove.elf not present";return result;}
  int fd=socket(AF_INET,SOCK_STREAM,0);
  if(fd<0){std::fclose(file);result.detail="ELF upload socket failed";return result;}
  struct timeval timeout{8,0};
  setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
  setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
  sockaddr_in addr{};addr.sin_family=AF_INET;
  addr.sin_port=htons(9021);
  addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  if(connect(fd,(sockaddr*)&addr,sizeof addr)!=0){
    close(fd);std::fclose(file);
    result.detail="elfldr not available on localhost:9021";return result;
  }
  if(report)report("REMOVE_DISPATCH",(std::string("uploading unremove.elf; target ")+item.path).c_str());
  result.launched=true;
  bool submitted=true;
  char buffer[8192];
  while(true){
    const size_t n=std::fread(buffer,1,sizeof buffer,file);
    if(!n){if(std::ferror(file))submitted=false;break;}
    if(!sendAll(fd,buffer,n)){submitted=false;break;}
  }
  std::fclose(file);
  if(!submitted){
    close(fd);result.detail="Sending removal payload failed";return result;
  }
  shutdown(fd,SHUT_WR);
  std::string output;
  const std::string marker="UREM2_READY ";
  size_t pos=std::string::npos,ending=std::string::npos;
  while(output.size()<8192){
    const ssize_t n=recv(fd,buffer,sizeof buffer,0);
    if(n<=0)break;
    output.append(buffer,(size_t)n);
    pos=output.find(marker);
    if(pos!=std::string::npos){
      ending=output.find('\n',pos+marker.size());
      if(ending!=std::string::npos)break;
    }
    if(output.find("UREM2_ERROR ")!=std::string::npos)break;
  }
  if(pos==std::string::npos||ending==std::string::npos){
    close(fd);
    result.detail=output.find("no_random")!=std::string::npos
       ?"Removal helper could not obtain secure randomness":
       output.find("bind")!=std::string::npos
       ?"Removal helper port 9176 unavailable":
       "Removal helper did not start: inspect /data/Uninstaller/crash.log";
    if(report)report("REMOVE_HELPER_ERROR",result.detail.c_str());
    return result;
  }
  const std::string token=output.substr(pos+marker.size(),ending-pos-marker.size());
  if(!capability(token)){
    close(fd);result.detail="Untrusted removal helper response";return result;
  }
  if(report)report("REMOVE_HELPER","ready, authenticated one-shot request");
  int action=socket(AF_INET,SOCK_STREAM,0);
  if(action<0){close(fd);result.detail="Cannot connect to removal helper";return result;}
  struct timeval responseTimeout{90,0};
  setsockopt(action,SOL_SOCKET,SO_SNDTIMEO,&responseTimeout,sizeof responseTimeout);
  setsockopt(action,SOL_SOCKET,SO_RCVTIMEO,&responseTimeout,sizeof responseTimeout);
  addr.sin_port=htons(9176);
  if(connect(action,(sockaddr*)&addr,sizeof addr)!=0){
    close(action);close(fd);
    result.detail="Removal helper listening socket not reachable";return result;
  }
  const std::string kind=item.kind==Kind::Installed?
     (mode==Mode::ProbeGame?"TITLEPROBE":
      mode==Mode::UninstallAll?"TITLEAUTO":
      mode==Mode::DeferShadow?"SHADOWDEFER":"TITLE"):
     item.kind==Kind::Homebrew?"USBHOME":"USBPKG";
  const std::string target=item.kind==Kind::Installed?
     ((mode==Mode::UninstallAll||mode==Mode::DeferShadow)?
       item.id+"|"+expectedSource:item.id):item.path;
  if(mode==Mode::DeferShadow &&
     (item.kind!=Kind::Installed||expectedSource.empty())){
     close(action);close(fd);result.detail="Invalid deferred ShadowMount request";return result;
  }
  if(target.find('\n')!=std::string::npos || target.find('\r')!=std::string::npos ||
     target.size()>690){
     close(action);close(fd);result.detail="Unsafe removal target";return result;
  }
  const std::string command="ACT "+token+" "+kind+" "+target+"\n";
  if(!sendAll(action,command.data(),command.size())){
    close(action);close(fd);result.detail="Removal request transmission failed";
    return result;
  }
  // The classic helper emits a stage per 512 files. Large games may produce
  // far more than 4 KiB of messages before its final authenticated result.
  // Read line-by-line, retaining only the final reply and source details.
  std::string pending,lastStage,answer,selectedSource,stagedPath,readError;
  bool finished=false;
  while(!finished){
    const ssize_t n=recv(action,buffer,sizeof buffer,0);
    if(n<=0){if(n<0)readError="Removal helper timed out or stopped";break;}
    pending.append(buffer,(size_t)n);
    if(pending.size()>8192){
      readError="Malformed or oversized removal line";break;
    }
    size_t eol;
    while((eol=pending.find('\n'))!=std::string::npos){
      const std::string line=pending.substr(0,eol);
      pending.erase(0,eol+1);
      if(line.rfind("UREM2_STAGE ",0)==0){
        lastStage=line.substr(sizeof("UREM2_STAGE ")-1);
        // Limit verbose disk scans to one logged counter every 4096 files.
        bool logStage=true;
        const size_t at=lastStage.find("_files_");
        if(at!=std::string::npos){
          const char* digits=lastStage.c_str()+at+7;
          unsigned count=0;
          logStage=std::sscanf(digits,"%u",&count)==1&&count%4096==0;
        }
        if(logStage&&report)report("REMOVE_STAGE",lastStage.c_str());
      }else if(line.rfind("UREM2_SOURCE ",0)==0){
        selectedSource=line.substr(sizeof("UREM2_SOURCE ")-1);
        if(selectedSource=="-")selectedSource.clear();
        if(report)report("SHADOWMOUNT_SOURCE",
           selectedSource.empty()?"none":selectedSource.c_str());
      }else if(line.rfind("UREM2_DIAG ",0)==0){
        if(report)report("SOURCE_DIAG",line.c_str()+sizeof("UREM2_DIAG ")-1);
      }else if(line.rfind("UREM2_STAGED ",0)==0){
        stagedPath=line.substr(sizeof("UREM2_STAGED ")-1);
      }else if(line.rfind("UREM2_RESULT ",0)==0){
        answer=line+"\n";finished=true;break;
      }
    }
  }
  close(action);close(fd);
  if(!answer.empty())result=parseResponse(answer,token,item.kind);
  else result.detail=readError.empty()?
      "No authenticated removal result":readError;
  result.source=selectedSource;
  result.launched=true;
  if(!stagedPath.empty()&&!result.accepted)
    result.detail+="; quarantined files at "+stagedPath;
  if(!result.accepted&&result.code==-1&&!lastStage.empty())
    result.detail="Title uninstall ended during "+lastStage+" (native helper may have stopped)";
  if(report)report("REMOVE_RESULT",result.detail.c_str());
  return result;
}
} // namespace uninstaller::actions
