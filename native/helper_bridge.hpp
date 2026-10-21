// SPDX-License-Identifier: GPL-3.0-or-later
// LauncherX-style read-only PS5 elfldr inventory bridge. No removal commands.
#pragma once
#include "inventory.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
namespace uninstaller::bridge {
struct Volume {std::string path;std::uint64_t total=0,free=0;};
struct Result {
  std::vector<Item> items;
  std::vector<Volume> volumes;
  std::string message;
  unsigned roots=0,denied=0,missing=0;
  bool connected=false,complete=false;
};
inline bool pathAllowed(const std::string& p,char kind){
  const char* prefixes[]={"/data/homebrew/","/user/app/","/system_ex/app/",
     "/data/etaHEN/games/","/user/data/etaHEN/games/","/data/games/",
     "/mnt/usb","/mnt/ext"};
  bool allowed=false;
  for(const char* pre:prefixes)if(p.rfind(pre,0)==0){allowed=true;break;}
  if(!allowed||p.find("/../")!=std::string::npos||p.find("//")!=std::string::npos ||
     p.find("\\")!=std::string::npos||p.size()>900)return false;
  if(kind=='P' && p.rfind("/mnt/usb",0)!=0 && p.rfind("/mnt/ext",0)!=0)
    return false;
  return kind=='G'||kind=='H'||kind=='P';
}
inline Result parse(const std::string& raw,ScanLog report=nullptr){
  Result out;
  const size_t marker=raw.find("UNSCN1\n");
  if(marker==std::string::npos){out.message="Helper catalogue marker missing";return out;}
  size_t pos=marker+7;
  int ordinal=0;
  while(pos<raw.size()&&out.items.size()<550){
    const size_t end=raw.find('\n',pos);
    if(end==std::string::npos||end-pos>128)break;
    const std::string header=raw.substr(pos,end-pos);pos=end+1;
    if(header.rfind("DONE ",0)==0){out.complete=true;break;}
    if(header.rfind("S ",0)==0){
      int code=-1;size_t len=0;unsigned long long total=0,free=0;
      if(std::sscanf(header.c_str(),"S %d %zu %llu %llu",
           &code,&len,&total,&free)!=4||len==0||len>64||
           len>raw.size()-pos)break;
      const std::string mount=raw.substr(pos,len);pos+=len;
      const bool known=mount=="/user"||
          (mount.size()==9 &&
           (mount.rfind("/mnt/usb",0)==0||mount.rfind("/mnt/ext",0)==0) &&
           mount[8]>='0'&&mount[8]<='7');
      if(code==0&&known&&total>0&&free<=total&&
         total<=70ull*1024*1024*1024*1024&&
         (mount=="/user"?total>=128ull*1024*1024*1024:
          mount.rfind("/mnt/ext",0)==0?total>=8ull*1024*1024*1024:
          total>=128ull*1024*1024)){
         out.volumes.push_back({mount,(std::uint64_t)total,(std::uint64_t)free});
         if(report)report("ELFLDR_STORAGE",
             (mount+" total="+std::to_string(total)+" free="+std::to_string(free)).c_str());
      } else if(report)report("ELFLDR_STORAGE_ERROR",
             (mount+" code="+std::to_string(code)).c_str());
      continue;
    }
    if(header.rfind("R ",0)==0){
      int code=-1;unsigned visited=0,found=0;size_t length=0;
      if(std::sscanf(header.c_str(),"R %d %u %u %zu",&code,&visited,&found,&length)!=4
        ||length==0||length>900||length>raw.size()-pos)break;
      const std::string path=raw.substr(pos,length);pos+=length;
      ++out.roots;if(code==1||code==13)++out.denied;else if(code==2)++out.missing;
      if(report){
        char message[248];
        std::snprintf(message,sizeof(message),"rc=%d visited=%u found=%u %s",
                      code,visited,found,path.c_str());
        report("ELFLDR_ROOT",message);
      }
      continue;
    }
    if(header.rfind("I ",0)==0){
      char kind='?';size_t plen=0,nlen=0,mlen=0;
      unsigned long long size=0;
      if(std::sscanf(header.c_str(),"I %c %zu %zu %zu %llu",
                    &kind,&plen,&nlen,&mlen,&size)!=5)break;
      if(plen==0||plen>900||nlen==0||nlen>160||mlen>8192||
         plen+nlen+mlen>raw.size()-pos)break;
      const std::string path=raw.substr(pos,plen);pos+=plen;
      const std::string name=raw.substr(pos,nlen);pos+=nlen;
      const std::string metadata=raw.substr(pos,mlen);pos+=mlen;
      const int itemIndex=ordinal++;
      if(!pathAllowed(path,kind)||!normalName(name))continue;
      Item item;item.remote=true;item.remoteIndex=itemIndex;item.path=path;item.root=path.substr(0,path.rfind('/'));
      item.name=field(metadata,"titleName");
      if(item.name.empty())item.name=name;
      item.id=field(metadata,"titleId");
      if(!validTitleId(item.id))item.id=titleIdFromFolder(name);
      item.version=field(metadata,"contentVersion");
      item.kind=kind=='G'?Kind::Installed:kind=='H'?Kind::Homebrew:Kind::UsbPackage;
      if(kind=='P'){item.name=name;item.size=(std::uint64_t)size;item.sizeKnown=true;}
      out.items.push_back(std::move(item));
      if(report&&out.items.size()<=35)report("ELFLDR_FOUND",path.c_str());
      continue;
    }
    break;
  }
  out.message=out.complete?"Read-only helper found "+std::to_string(out.items.size())+" entries":
                         "Helper response incomplete; retry after starting elfldr";
  return out;
}
inline Result scan(const char* elf="/app0/unscan.elf",ScanLog report=nullptr){
  Result res;
  FILE* f=std::fopen(elf,"rb");
  if(!f){
    res.message="Bundled unscan.elf missing at /app0";
    if(report)report("ELFLDR_ERROR",res.message.c_str());
    return res;
  }
  int fd=socket(AF_INET,SOCK_STREAM,0);
  if(fd<0){std::fclose(f);res.message="Cannot create local loader socket";return res;}
  struct timeval timeout{8,0};
  setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
  setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
  sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(9021);
  addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  if(connect(fd,(sockaddr*)&addr,sizeof(addr))!=0){
    std::fclose(f);close(fd);
    res.message="elfldr unavailable at 127.0.0.1:9021";
    if(report)report("ELFLDR_ERROR",res.message.c_str());
    return res;
  }
  res.connected=true;
  if(report)report("ELFLDR","connected, sending read-only inventory ELF");
  char buffer[8192];bool submitted=true;
  while(true){
    const size_t n=fread(buffer,1,sizeof(buffer),f);
    if(!n){if(ferror(f))submitted=false;break;}
    size_t off=0;
    while(off<n){
      const ssize_t wrote=send(fd,buffer+off,n-off,0);
      if(wrote<=0){submitted=false;break;}
      off+=(size_t)wrote;
    }
    if(!submitted)break;
  }
  std::fclose(f);
  if(!submitted){
    close(fd);res.message="ELF submission to elfldr failed";
    if(report)report("ELFLDR_ERROR",res.message.c_str());
    return res;
  }
  shutdown(fd,SHUT_WR);
  std::string raw;raw.reserve(16384);
  while(raw.size()<2*1024*1024){
    const ssize_t n=recv(fd,buffer,sizeof(buffer),0);
    if(n<=0)break;
    raw.append(buffer,(size_t)n);
    if(raw.find("DONE ",raw.size()>256?raw.size()-256:0)!=std::string::npos &&
       raw.rfind('\n')!=std::string::npos)break;
  }
  close(fd);
  if(report)report("ELFLDR",(std::string("received bytes=")+std::to_string(raw.size())).c_str());
  Result parsed=parse(raw,report);
  parsed.connected=res.connected;
  if(report)report("ELFLDR_RESULT",parsed.message.c_str());
  return parsed;
}

// This endpoint exists only while the previously launched read-only helper is alive.
// It accepts an enumeration ordinal, never a path. PNG bytes are not cached on disk.
inline void stop_icons(){
  int fd=socket(AF_INET,SOCK_STREAM,0);
  if(fd<0)return;
  struct timeval tv={1,0};
  setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
  setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
  sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(9175);
  addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  if(connect(fd,(sockaddr*)&addr,sizeof(addr))==0){
    const char request[]="QUIT\n";
    send(fd,request,sizeof(request)-1,0);
  }
  close(fd);
}
inline bool fetch_icon(int ordinal,std::string& png){
  png.clear();
  if(ordinal<0||ordinal>=550)return false;
  int fd=socket(AF_INET,SOCK_STREAM,0);
  if(fd<0)return false;
  struct timeval timeout{2,0};
  setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
  setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
  sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(9175);
  addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  if(connect(fd,(sockaddr*)&addr,sizeof(addr))!=0){close(fd);return false;}
  const std::string request="ICON "+std::to_string(ordinal)+"\n";
  if(send(fd,request.data(),request.size(),0)!=(ssize_t)request.size()){
    close(fd);return false;
  }
  char header[64]={0};size_t used=0;char c=0;
  while(used+1<sizeof(header)){
    if(recv(fd,&c,1,0)!=1)break;
    if(c=='\n')break;
    header[used++]=c;
  }
  unsigned long long len=0;
  if(std::sscanf(header,"DATA %llu",&len)!=1||len<32||len>3*1024*1024){
    close(fd);return false;
  }
  png.resize((size_t)len);
  size_t off=0;
  while(off<png.size()){
    const ssize_t got=recv(fd,png.data()+off,png.size()-off,0);
    if(got<=0)break;
    off+=(size_t)got;
  }
  close(fd);
  if(off!=png.size()||png.size()<8||
     std::memcmp(png.data(),"\x89PNG\r\n\x1a\n",8)!=0){
     png.clear();return false;
  }
  return true;
}

inline void merge(std::vector<Item>& local,const Result& result){
  for(const auto& candidate:result.items){
    bool exists=false;
    for(const auto& x:local)if(x.path==candidate.path){exists=true;break;}
    if(!exists&&local.size()<750)local.push_back(candidate);
  }
  std::sort(local.begin(),local.end(),[](const Item& a,const Item& b){
    if(a.kind!=b.kind)return (int)a.kind<(int)b.kind;
    if(a.name!=b.name)return a.name<b.name;
    return a.path<b.path;
  });
}
} // namespace uninstaller::bridge
