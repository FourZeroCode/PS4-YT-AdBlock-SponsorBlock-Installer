# PS4 YT AdBlock + SponsorBlock Installer

A PS4 homebrew app for configuring SponsorBlock categories and installing/updating the YouTube AdBlock + SponsorBlock.

## Features

- Configure all 12 SponsorBlock categories.
- Choose **Auto Skip** or **Ask for Confirmation** for segments.
- Save settings to `/data/youtube/config.json`.
- Download the latest PRX from [GitHub Releases](https://github.com/FourZeroCode/yt_adblock_sponsorblock_ps4/releases).
- Fall back to the bundled PRX if the download fails.
- Install/update the PRX and manage its GoldHEN `plugins.ini` entry.
- Uninstall the managed plugin and SponsorBlock configuration.

## Controls

| Button | Action |
| --- | --- |
| D-pad Up / Down | Navigate the menu |
| Square | Toggle the selected mode or category |
| R2 | Select all categories, or clear all if already selected |
| Triangle | Save SponsorBlock settings |
| Cross | Save settings and install/update the PRX |
| Options | Open the uninstall prompt |

## Build (Windows)

1. Install the [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain).
2. Make sure `OO_PS4_TOOLCHAIN` points to your toolchain installation.
3. Keep the `_common` folder beside `SponsorBlockConfig.sln`.
4. From `SponsorBlockConfig/SponsorBlockConfig`, run:

   ```bat
   .\build.bat .\x64\Debug "SponsorBlockConfig" "..\\"
   ```

## Credits

- **[OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain)** — PS4 homebrew toolchain, headers, libraries, and build tools.

- **[SponsorBlock](https://github.com/ajayyy/SponsorBlock)** — the community project and category IDs this app configures.

## Disclaimer

This is an unofficial homebrew project and is not affiliated with or endorsed by SponsorBlock, or YouTube. Use at your own risk.