// SPDX-License-Identifier: GPL-3.0-or-later
#include "../native/inventory.hpp"
#include <cassert>
#include <cstdio>
using namespace uninstaller;
int main() {
  assert(validTitleId("PPSA99986"));
  assert(validTitleId("CUSA12345"));
  assert(!validTitleId("PPSA0000/"));
  assert(!validTitleId("../../../.."));
  assert(!normalName(".."));
  assert(!normalName("../homebrew"));
  assert(endsPkg("my_game.pkg") && endsPkg("MY_GAME.PKG"));
  assert(!endsPkg("my_game.pkg.exe"));
  Item root;root.kind=Kind::Homebrew;root.id="PPSA99986";root.path="/data/homebrew";
  assert(!isAllowedRemoval(root));
  Item sys;sys.kind=Kind::Installed;sys.id="PPSA99986";sys.path="/user/app/PPSA99986";
  assert(!isAllowedRemoval(sys));
  Item fake;fake.kind=Kind::UsbPackage;fake.path="/etc/passwd";assert(!isAllowedRemoval(fake));
  assert(field(R"({"localizedParameters":{"en-US":{"titleName":"Demo"}},"contentVersion":"01.000.002"})","titleName")=="Demo");
  assert(!bytes(1024).empty());
  puts("Inventory parsing and unsafe-path rejection: PASS");
}
