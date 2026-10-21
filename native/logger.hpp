// SPDX-License-Identifier: GPL-3.0-or-later
// Safe append-only diagnostics for PS5 homebrew. No user files are modified.
#pragma once
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
namespace uninstaller::diag {
inline constexpr const char* kDir="/data/Uninstaller";
inline constexpr const char* kLog="/data/Uninstaller/uninstaller.log";
inline constexpr const char* kCrash="/data/Uninstaller/crash.log";
inline bool ensureDir(){return mkdir(kDir,0755)==0 || errno==EEXIST;}
inline bool write(const char* phase,const char* detail=nullptr){
  if(!ensureDir())return false;
  FILE* f=std::fopen(kLog,"ab");
  if(!f)return false;
  const int rc=std::fprintf(f,"[Uninstaller v0.1.18] %.80s%s%.240s\n",
                          phase,detail?" ":"",detail?detail:"");
  const bool ok=(rc>=0&&std::fflush(f)==0);
  if(std::fclose(f)!=0)return false;
  return ok;
}
} // namespace uninstaller::diag
