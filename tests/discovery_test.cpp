// SPDX-License-Identifier: GPL-3.0-or-later
#include "../native/inventory.hpp"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
using namespace uninstaller;
namespace{
int logs=0;
void onLog(const char*,const char*){logs++;}
void folder(const std::string& s){assert(mkdir(s.c_str(),0755)==0);}
void file(const std::string& s,const char* data=""){FILE* f=fopen(s.c_str(),"wb");assert(f);fputs(data,f);fclose(f);}
}
int main(){
  assert(validTitleId("PPSA99986"));assert(validTitleId("LAPY00001"));
  assert(validTitleId("FAKE00002"));assert(validTitleId("CUSA23456"));
  assert(!validTitleId("XXXXX1234"));
  assert(titleIdFromFolder("PPSA12345-app0")=="PPSA12345");
  assert(titleIdFromFolder("PPSA12345-app")=="PPSA12345");
  assert(titleIdFromFolder("LAPY00001")=="LAPY00001");
  assert(titleIdFromFolder("PPSA12345anything").empty());
  assert(looksLikeGameDump("PPSA12345-app0"));
  assert(!looksLikeGameDump("PPSA12345"));
  char root[]="/tmp/ps5-uninstaller-XXXXXX";
  char* temp=mkdtemp(root);assert(temp);
  const std::string base=temp;
  folder(base+"/homebrew");
  folder(base+"/homebrew/MyApp");
  folder(base+"/homebrew/MyApp/sce_sys");
  file(base+"/homebrew/MyApp/eboot.bin","a");
  file(base+"/homebrew/MyApp/sce_sys/param.json",
       R"({"titleId":"LAPY00001","localizedParameters":{"en-US":{"titleName":"My Homebrew"}},"contentVersion":"01.000.001"})");
  folder(base+"/games");
  folder(base+"/games/Collection");
  folder(base+"/games/Collection/PPSA12345-app0");
  file(base+"/games/Collection/PPSA12345-app0/eboot.bin");
  folder(base+"/usb");
  file(base+"/usb/Game.pkg","test-pkg");
  folder(base+"/usb/not-an-app");
  assert(symlink((base+"/homebrew/MyApp").c_str(),(base+"/games/symlink").c_str())==0);
  std::vector<Item> items;
  scanFolders(items,base+"/homebrew",Kind::Homebrew,onLog,2);
  assert(items.size()==1&&items[0].name=="My Homebrew");
  assert(items[0].id=="LAPY00001"&&items[0].kind==Kind::Homebrew);
  scanFolders(items,base+"/games",Kind::Installed,onLog,2);
  assert(items.size()==2&&items[1].id=="PPSA12345");
  assert(items[1].kind==Kind::Installed);
  scanPkgs(items,base+"/usb",onLog,2);
  assert(items.size()==3&&items[2].sizeKnown&&items[2].size==8);
  assert(items[2].kind==Kind::UsbPackage);
  assert(!isAllowedRemoval(items[1]));
  assert(!isAllowedRemoval(items[0])); // Test fixtures cannot be deleted by the app.
  assert(!isAllowedRemoval(items[2])); // Only explicit USB mount paths permitted.
  assert(logs>=7);
  puts("PASS: non-PPSA homebrew, nested game dumps, PKG, symlink skip, safe deletion");
}
