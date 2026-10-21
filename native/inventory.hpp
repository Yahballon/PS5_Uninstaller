// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only filesystem discovery with narrowly scoped file deletion safeguards.
#pragma once
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

namespace uninstaller {
enum class Kind { Installed, Homebrew, UsbPackage };
struct Item {
  std::string name, id, version, path, icon, root;
  Kind kind=Kind::Homebrew;
  std::uint64_t size=0;
  bool sizeKnown=false;
  bool remote=false; // Read-only entries obtained through the trusted local ELF helper.
  int remoteIndex=-1; // Enumeration ordinal, never a client-provided filesystem path.
};
struct Storage {std::uint64_t total=0,free=0;bool available=false;};
using ScanLog=void(*)(const char* phase,const char* detail);

inline bool validTitleId(const std::string& id) {
  if(id.size()!=9)return false;
  const std::string p=id.substr(0,4);
  if(p!="PPSA"&&p!="CUSA"&&p!="LAPY"&&p!="FAKE")return false;
  for(size_t i=4;i<9;i++)if(id[i]<'0'||id[i]>'9')return false;
  return true;
}
inline bool normalName(const std::string& name){
  return !name.empty()&&name!="."&&name!=".."&&name.size()<230
   &&name.find('/')==std::string::npos&&name.find('\\')==std::string::npos;
}
inline std::string titleIdFromFolder(const std::string& folder){
  if(folder.size()>=9){
    const std::string prefix=folder.substr(0,9);
    if(validTitleId(prefix) && (folder.size()==9 || folder[9]=='-' || folder[9]=='_' || folder[9]==' '))return prefix;
  }
  return {};
}
inline bool regular(const std::string& p){
  struct stat s{};return lstat(p.c_str(),&s)==0&&S_ISREG(s.st_mode);
}
inline bool directory(const std::string& p){
  struct stat s{};return lstat(p.c_str(),&s)==0&&S_ISDIR(s.st_mode);
}
inline std::string readSmall(const std::string& path){
  FILE* f=fopen(path.c_str(),"rb");if(!f)return {};
  char b[8193];size_t n=fread(b,1,sizeof(b)-1,f);fclose(f);b[n]=0;return {b,n};
}
inline std::string field(const std::string& data,const char* key){
  const std::string k=std::string("\"")+key+"\"";
  size_t at=data.find(k);if(at==std::string::npos)return {};
  at=data.find(':',at+k.size());if(at==std::string::npos)return {};
  at=data.find('"',at+1);if(at==std::string::npos)return {};
  std::string out;bool esc=false;
  for(size_t i=at+1;i<data.size()&&out.size()<95;i++){
    const unsigned char c=(unsigned char)data[i];
    if(!esc && c=='"')break;
    if(!esc && c=='\\'){esc=true;continue;}
    if(c>=32)out+=(char)c;
    esc=false;
  }
  return out;
}
inline bool endsPkg(const std::string& n){
  return n.size()>4&&(n.substr(n.size()-4)==".pkg"||n.substr(n.size()-4)==".PKG");
}
inline void report(ScanLog log,const char* phase,const std::string& data){
  if(log)log(phase,data.c_str());
}
inline void reportNumber(ScanLog log,const char* phase,const std::string& root,
                         unsigned visited,unsigned found,int code=0){
  if(!log)return;
  char buffer[244];
  std::snprintf(buffer,sizeof(buffer),"%s visited=%u found=%u errno=%d",
                root.c_str(),visited,found,code);
  log(phase,buffer);
}
inline bool looksLikeGameDump(const std::string& name){
  const size_t n=name.size();
  return (n>=4 && name.substr(n-4)=="-app")
      || (n>=5 && name.substr(n-5)=="-app0");
}
inline void folderToItem(std::vector<Item>& out,const std::string& root,
                        const std::string& path,const std::string& folder,
                        Kind rootKind,ScanLog log){
  const std::string metaPath=path+"/sce_sys/param.json";
  const bool elf=regular(path+"/eboot.bin");
  const bool json=regular(metaPath);
  const bool sfo=regular(path+"/sce_sys/param.sfo");
  if(!elf&&!json&&!sfo)return;
  const std::string metadata=json?readSmall(metaPath):std::string();
  Item entry;entry.root=root;entry.path=path;
  entry.id=field(metadata,"titleId");
  if(!validTitleId(entry.id))entry.id=titleIdFromFolder(folder);
  entry.name=field(metadata,"titleName");
  if(entry.name.empty())entry.name=folder;
  entry.version=field(metadata,"contentVersion");
  entry.kind=(rootKind==Kind::Installed||looksLikeGameDump(folder))
      ?Kind::Installed:Kind::Homebrew;
  if(regular(path+"/sce_sys/icon0.png"))entry.icon=path+"/sce_sys/icon0.png";
  // Title directories belonging to the app manager or games are never trusted
  // as removable merely because they have a recognizable Title ID.
  out.push_back(std::move(entry));
  if(log&&out.size()<=65)report(log,"SCAN_FOUND",path);
}
// Bounded two-level scan: handles homebrew folders, etaHEN game folders and
// category/subfolder structures. All traversals use lstat; symlinks skipped.
inline void scanFolders(std::vector<Item>& out,const std::string& root,
                        Kind kind,ScanLog log=nullptr,unsigned maxDepth=2){
  if(out.size()>=750)return;
  report(log,"SCAN_ROOT_BEGIN",root);
  DIR* top=opendir(root.c_str());
  if(!top){reportNumber(log,"SCAN_ROOT_ERROR",root,0,0,errno);return;}
  closedir(top);
  struct Pending{std::string path;unsigned depth;};
  std::vector<Pending> stack{{root,0}};
  unsigned visited=0,found=0;int lastError=0;
  while(!stack.empty()&&visited<3500&&out.size()<750){
    const Pending node=std::move(stack.back());stack.pop_back();
    DIR* d=opendir(node.path.c_str());
    if(!d){lastError=errno;reportNumber(log,"SCAN_SUBDIR_ERROR",node.path,visited,found,lastError);continue;}
    errno=0;struct dirent* e;
    while((e=readdir(d))!=nullptr&&visited<3500&&out.size()<750){
      const std::string name=e->d_name;
      if(!normalName(name)||name[0]=='.')continue;
      ++visited;
      const std::string path=node.path+"/"+name;
      struct stat st{};if(lstat(path.c_str(),&st)!=0)continue;
      if(!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode))continue;
      const size_t before=out.size();
      folderToItem(out,root,path,name,kind,log);
      if(out.size()!=before){++found;continue;} // Never enumerate an app's content tree.
      if(node.depth<maxDepth&&stack.size()<1300 &&
         name!="sce_sys"&&name!="sce_module"&&name!="system_data" &&
         name!="user"&&name!="patch"&&name!="addcont"&&name!="trophy"){
        stack.push_back({path,node.depth+1});
      }
    }
    if(errno!=0)lastError=errno;
    closedir(d);
  }
  reportNumber(log,"SCAN_ROOT_DONE",root,visited,found,lastError);
}
inline void scanPkgs(std::vector<Item>& out,const std::string& root,
                     ScanLog log=nullptr,unsigned maxDepth=2){
  report(log,"PKG_ROOT_BEGIN",root);
  DIR* t=opendir(root.c_str());
  if(!t){reportNumber(log,"PKG_ROOT_ERROR",root,0,0,errno);return;}
  closedir(t);
  struct Pending{std::string path;unsigned depth;};
  std::vector<Pending> stack{{root,0}};
  unsigned seen=0,found=0;int lastError=0;
  while(!stack.empty()&&seen<3500&&out.size()<750){
    const Pending node=std::move(stack.back());stack.pop_back();
    DIR* d=opendir(node.path.c_str());
    if(!d){lastError=errno;continue;}
    errno=0;struct dirent* e;
    while((e=readdir(d))!=nullptr && seen<3500 && out.size()<750){
      const std::string name=e->d_name;
      if(!normalName(name)||name[0]=='.')continue;
      ++seen;
      const std::string path=node.path+"/"+name;
      struct stat st{};if(lstat(path.c_str(),&st)!=0||S_ISLNK(st.st_mode))continue;
      if(S_ISREG(st.st_mode)&&endsPkg(name)){
        Item item;item.kind=Kind::UsbPackage;item.name=name;
        item.root=root;item.path=path;item.size=(std::uint64_t)st.st_size;
        item.sizeKnown=true;out.push_back(std::move(item));++found;
        if(log && found<=32)report(log,"PKG_FOUND",path);
      }else if(S_ISDIR(st.st_mode)&&node.depth<maxDepth&&stack.size()<1300 &&
               name!="homebrew"&&name!="user"&&name!="system_data"&&name!="etaHEN"){
        stack.push_back({path,node.depth+1});
      }
    }
    if(errno!=0)lastError=errno;
    closedir(d);
  }
  reportNumber(log,"PKG_ROOT_DONE",root,seen,found,lastError);
}
inline std::vector<Item> scan(ScanLog log=nullptr){
  std::vector<Item> out;
  // Read-only system database probe (never modify or parse protected app.db).
  struct stat database{};
  const char* dbPath="/system_data/priv/mms/app.db";
  if(lstat(dbPath,&database)==0)
    report(log,"APP_DB_PATH","present (not parsed; access may require privileged helper)");
  else
    reportNumber(log,"APP_DB_PATH_ERROR",dbPath,0,0,errno);
  // Read-only scan of installed title directories. Access is firmware/payload dependent.
  scanFolders(out,"/user/app",Kind::Installed,log,0);
  scanFolders(out,"/system_ex/app",Kind::Installed,log,0);
  scanFolders(out,"/data/homebrew",Kind::Homebrew,log,2);
  scanFolders(out,"/data/etaHEN/games",Kind::Installed,log,2);
  scanFolders(out,"/user/data/etaHEN/games",Kind::Installed,log,2);
  scanFolders(out,"/data/games",Kind::Installed,log,1);
  for(int i=0;i<8;i++){
    const std::string base="/mnt/usb"+std::to_string(i);
    scanFolders(out,base+"/homebrew",Kind::Homebrew,log,2);
    scanFolders(out,base+"/etaHEN/games",Kind::Installed,log,2);
    scanPkgs(out,base,log,2);
  }
  for(int i=0;i<4;i++){
    const std::string base="/mnt/ext"+std::to_string(i);
    scanFolders(out,base+"/homebrew",Kind::Homebrew,log,2);
    scanFolders(out,base+"/etaHEN/games",Kind::Installed,log,2);
    scanFolders(out,base+"/user/app",Kind::Installed,log,0);
    scanPkgs(out,base,log,2);
  }
  std::sort(out.begin(),out.end(),[](const Item& a,const Item& b){
    if(a.kind!=b.kind)return (int)a.kind<(int)b.kind;
    if(a.name!=b.name)return a.name<b.name;
    return a.path<b.path;
  });
  out.erase(std::unique(out.begin(),out.end(),[](const Item& a,const Item& b){
    return a.path==b.path;
  }),out.end());
  reportNumber(log,"SCAN_COMPLETE","all roots",0,(unsigned)out.size());
  // We intentionally do not parse or modify the system SQLite app.db.
  // If no files are visible, root failures tell whether sandboxing is involved.
  return out;
}
inline Storage storage(const std::string& path="/data"){
  Storage s;struct statvfs v{};
  if(statvfs(path.c_str(),&v)==0&&v.f_frsize&&v.f_blocks){
    s.total=(std::uint64_t)v.f_blocks*v.f_frsize;
    s.free=(std::uint64_t)v.f_bavail*v.f_frsize;
    s.available=s.free<=s.total;
  }
  return s;
}
inline std::string bytes(std::uint64_t n){
  double v=(double)n;int i=0;const char* unit[]={"B","KB","MB","GB","TB"};
  while(v>=1024&&i<4){v/=1024;++i;}
  char b[40];std::snprintf(b,sizeof(b),"%.1f %s",v,unit[i]);return b;
}
// File deletion is limited to specifically authorized top-level homebrew
// installations and USB PKG archives, never system game mountpoints.
inline bool isAllowedRemoval(const Item& a){
  if(a.remote)return false; // Never delete via sandbox-bypassing inventory results.
  if(a.kind==Kind::Installed)return false;
  if(a.kind==Kind::UsbPackage){
    if(a.path.rfind("/mnt/usb",0)!=0||a.path.size()<15)return false;
    const size_t slash=a.path.rfind('/');
    if(slash==std::string::npos)return false;
    const std::string name=a.path.substr(slash+1);
    return endsPkg(name)&&normalName(name)&&regular(a.path);
  }
  if(!validTitleId(a.id)||a.id=="PPSA99986")return false;
  if(a.root=="/data/homebrew"&&a.path==a.root+"/"+a.id)
    return directory(a.path);
  for(int i=0;i<8;i++){
    const std::string base="/mnt/usb"+std::to_string(i)+"/homebrew";
    if(a.root==base&&a.path==base+"/"+a.id)return directory(a.path);
  }
  return false;
}
inline bool clearTree(const std::string& path,unsigned level=0){
  if(level>20)return false;
  struct stat st{};if(lstat(path.c_str(),&st)!=0)return false;
  if(!S_ISDIR(st.st_mode))return unlink(path.c_str())==0;
  DIR* d=opendir(path.c_str());if(!d)return false;
  bool ok=true;struct dirent* e;
  while((e=readdir(d))){
    const std::string child=e->d_name;
    if(child=="."||child=="..")continue;
    if(!normalName(child)||!clearTree(path+"/"+child,level+1)){ok=false;break;}
  }
  closedir(d);if(ok)ok=rmdir(path.c_str())==0;return ok;
}
inline int removeFiles(const Item& a){
  if(!isAllowedRemoval(a))return EPERM;
  if(a.kind==Kind::UsbPackage)return unlink(a.path.c_str())==0?0:errno;
  return clearTree(a.path)?0:(errno?errno:EIO);
}
inline std::uint64_t shallowSize(const Item& a){return a.sizeKnown?a.size:0;}
}
