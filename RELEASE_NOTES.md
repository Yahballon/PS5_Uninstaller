# PS5 Uninstaller v0.1.18

First standalone release of **PS5 Uninstaller** by YAHBALLON.

- Native PS5 homebrew UI with All, Games, Homebrew and USB PKGs tabs, title artwork, metadata and confirmation-based deletion.
- ELF-based read-only inventory helper (`unscan.elf`) and authenticated removal helper (`unremove.elf`) with local elfldr on port 9021.
- Supports PS5 AppInstUtil uninstall requests, validated USB homebrew deletion and standalone USB PKG removal.
- ShadowMountPlus integration for managed game sources, with deferred background/helper deletion when the running PS5 game context prevents removal.
- Classic two-pass validated USB source cleanup as fallback for supported unmounted sources.
- Native scanner, unit tests, crash reporting and logs under `/data/Uninstaller`.
- Standalone public source, reproducible GitHub Actions workflow and an installation ZIP named `PPSA99986.zip`.

**Install:** unzip the release to obtain a `PPSA99986` native homebrew folder containing `eboot.bin`, `sce_sys`, `unscan.elf`, and `unremove.elf`.

**Important:** Game deletion is permanent and dependent on firmware, jailbreak privileges and ShadowMount state. A successful PS5 system uninstall request does not necessarily mean backing files have been deleted. Save-data folders are not intentionally targeted.

Screenshot and usage guide: [README](https://github.com/Yahballon/PS5_Uninstaller#readme).
