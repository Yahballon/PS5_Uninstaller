// SPDX-License-Identifier: GPL-3.0-or-later
#include "../native/action_bridge.hpp"
#include <cassert>
#include <cstdio>
using namespace uninstaller;
int main(){
  Item system;system.remote=true;system.kind=Kind::Installed;
  system.id="PPSA11386";system.path="/user/app/PPSA11386";
  assert(actions::eligible(system));
  system.id="PPSA99986";system.path="/user/app/PPSA99986";
  assert(!actions::eligible(system));
  system.id="PPSA11386";system.path="/system_ex/app/PPSA11386";
  assert(!actions::eligible(system));
  system.path="/user/app/PPSA00001";
  assert(!actions::eligible(system));
  Item archive;archive.kind=Kind::UsbPackage;archive.remote=true;
  archive.path="/mnt/usb0/Downloads/Test.pkg";
  assert(actions::eligible(archive));
  archive.path="/mnt/usb9/Test.pkg";assert(!actions::eligible(archive));
  archive.path="/mnt/usb0/../Test.pkg";assert(!actions::eligible(archive));
  archive.path="/mnt/ext0/Test.pkg";assert(!actions::eligible(archive));
  archive.path="/mnt/usb0/System";assert(!actions::eligible(archive));
  Item usbhome;usbhome.remote=true;usbhome.kind=Kind::Homebrew;
  usbhome.path="/mnt/usb0/homebrew/MyTool";
  assert(actions::eligible(usbhome));
  usbhome.path="/mnt/usb0/homebrew/MyTool/subfolder";
  assert(!actions::eligible(usbhome));
  usbhome.path="/mnt/usb0/homebrew/../data";
  assert(!actions::eligible(usbhome));
  usbhome.path="/mnt/usb0/homebrew/.hidden";
  assert(!actions::eligible(usbhome));
  std::string t="0123456789abcdef0123456789abcdef";
  assert(actions::capability(t));
  auto ok=actions::parseResponse("loader noise\nUREM2_RESULT "+t+" 00000000 2\n",t,Kind::UsbPackage);
  assert(ok.accepted&&ok.verified&&ok.code==0);
  auto accepted=actions::parseResponse("UREM2_RESULT "+t+" 00000000 1\n",t,Kind::Installed);
  assert(accepted.accepted&&!accepted.verified);
  auto probed=actions::parseResponse("UREM2_SOURCE /mnt/usb0/PPSA21564-app\\nUREM2_RESULT "+t+" 00000000 3\\n",t,Kind::Installed);
  assert(probed.code==0 && !probed.accepted && !probed.verified);
  auto complete=actions::parseResponse("UREM2_RESULT "+t+" 00000000 2\\n",t,Kind::Installed);
  assert(complete.accepted&&complete.verified);
  auto partial=actions::parseResponse("UREM2_RESULT "+t+" 00000000 4\\n",t,Kind::Installed);
  assert(partial.accepted&&partial.verified);
  const auto deferred=actions::parseResponse(
      "UREM2_RESULT "+t+" 00000000 5\n",t,Kind::Installed);
  assert(deferred.accepted && !deferred.verified && deferred.code==0);
  assert(deferred.detail.find("scheduled")!=std::string::npos);
  const auto deferRejected=actions::parseResponse(
      "UREM2_RESULT "+t+" 00000001 0\n",t,Kind::Installed);
  assert(!deferRejected.accepted);
  auto wrong=actions::parseResponse("UREM2_RESULT bad 00000000 2\n",t,Kind::UsbPackage);
  assert(!wrong.accepted);
  Item dump;dump.remote=true;dump.kind=Kind::Installed;
  dump.id="PPSA11386";dump.path="/data/etaHEN/games/PPSA11386";
  assert(!actions::eligible(dump));
  puts("PASS: only explicitly supported installed title or USB PKG actions");
}
