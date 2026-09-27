# SunrisePlus

Destiny 2 Offline Exploration Mod based on [Sunrise](https://github.com/stanuwu/Sunrise)

> SunrisePlus is a personal extension of Sunrise focused on additional gameplay, exploration and quality-of-life features
>
> It keeps the original Sunrise project as its foundation while providing optional features and customization maintained independently from upstream

- [Original Sunrise Project](https://github.com/stanuwu/Sunrise)
- [Install Instructions](https://projectsunrise.dev/guides/installing/)
- [FAQ](https://projectsunrise.dev/faq/)
- [Documentation](https://projectsunrise.dev/docs/)
- [Discord](https://discord.gg/22JS6et5k9)

## Opening the overlay

After launching Destiny 2, press Insert to open or close the Sunrise overlay

Insert is the default key and can be changed in `bin\x64\Sunrise\settings.json`

See the [FAQ](https://projectsunrise.dev/faq/) for more information

## SunrisePlus Features

Additional features implemented for SunrisePlus

- [x] Infinite Magazine
- [x] No Damage
- [x] Configurable Jump Height from 1x to 10x
- [x] Controller Support for Fly Movement
- [x] Configurable Movement Speed
- [x] Ability No Cooldown for Grenade, Super, Melee and Class Ability
- [x] Season Progression controls for XP, ranks, Artifact Power Bonus and reset

These features are optional and are designed for offline gameplay and exploration

## Original Sunrise Features

SunrisePlus includes the core functionality provided by the original Sunrise project

- Load into any Destination
- Script Missions
- Exploration features including Fly, Noclip and Activity Override
- Persistent Save

Some features from the original Sunrise project are still under development, including full progression, multiplayer and additional missions

## About SunrisePlus

SunrisePlus is my personal extended fork of Sunrise, focused on gameplay options, quality-of-life improvements and experimentation with the preserved Destiny 2 build

It builds on the excellent preservation work of the original Sunrise project while allowing me to continue developing and testing features independently

## Future Ideas

Some features being considered or researched for SunrisePlus

- Disable Fall Damage
- Instant Summon Vehicle / Sparrow
- Damage Modifier
- Cinematics Library

These are ideas rather than commitments and may change as the project evolves

## WIP

SunrisePlus and Sunrise are works in progress

Things may break or work in unexpected ways while features are being developed and tested

## Original Project

SunrisePlus would not exist without the work of the original Sunrise developers and contributors

Original project

https://github.com/stanuwu/Sunrise

Please consider starring and supporting the original Sunrise project

All content released under SunrisePlus remains free and open source

If someone is trying to sell SunrisePlus or its included features, do not purchase it

## Rules

Issues are for bug reports related to SunrisePlus

Pull Requests are welcome for fixes, improvements and additional features

Please keep discussions constructive and focused on the project

## Building

### Windows

Install Visual Studio 2026 with the **Desktop development with C++** workload

The project builds against the v145 toolset and the 10.0.26100 Windows SDK, so check that both are selected in the installer

The easiest route is to open `Sunrise.sln`, select the `Release` `x64` configuration and build

To build from a command line, use the Developer PowerShell for VS 2026

1. Clone the repository

```powershell
git clone https://github.com/saulob/SunrisePlus.git
cd SunrisePlus
```

2. Build the solution

```powershell
msbuild Sunrise.sln /m /p:Configuration=Release /p:Platform=x64
```

### Linux

Make sure you have `git`, `cmake`, `clang`, `ninja`, `llvm` and `xwin` installed

1. Clone the repository

```bash
git clone https://github.com/saulob/SunrisePlus.git
cd SunrisePlus
```

2. Download Windows headers

```bash
xwin --sdk-version 10.0.26100 --accept-license splat --include-debug-libs --output .xwin-cache
```

3. Configure and build the project

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$(pwd)/linux-to-win-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Keeping SunrisePlus Updated

The original Sunrise repository should be configured as the upstream remote

```powershell
git remote add upstream https://github.com/stanuwu/Sunrise.git
```

Updates from Sunrise can then be integrated with

```powershell
git fetch upstream
git checkout master
git merge upstream/master
```

SunrisePlus-specific features are maintained on top of the upstream project

## Contributing

Pull Requests are welcome

Please follow these guidelines

- **No Copyrighted Data** - All game data should be extracted at runtime
- **Code Formatting** - Follow the provided clang-format and clang-tidy configurations
- **Clean Code** - Keep changes readable and consistent with the existing project
- **Provide Documentation** - Explain what changed, why it changed and its effects
- **One Feature** - Keep unrelated features in separate Pull Requests
- **Complete Implementations** - Avoid submitting incomplete or non-functional features
- **Offline Focus** - SunrisePlus is intended for the supported offline game build and not live Destiny 2 servers

## SunrisePlus Maintainer

- [Saulo Benigno](https://github.com/saulob) - SunrisePlus maintainer and additional feature development

## Original Sunrise Team

- [stan](https://github.com/stanuwu) - Creator and lead developer of Sunrise
- [techno](https://github.com/Techno453) - P2P multiplayer
- [gage](https://github.com/gagefulwood) - Investment and progression
- All Sunrise open source contributors

## Credits

### Original Project

SunrisePlus is based on [Sunrise](https://github.com/stanuwu/Sunrise)

Original architecture, research and core implementation belong to the Sunrise project and its contributors

### Dependencies

- [ImGui](https://github.com/ocornut/imgui)
- [Detours](https://github.com/microsoft/detours)
- [Lua](https://lua.org/)
- [SQLite](https://www.sqlite.org/)

### Artwork

- [Solus](https://www.youtube.com/@Solus-yt)

### Original Sunrise Testing Credits

- [Ferr](https://x.com/light_fades_awy)
- [gage](https://x.com/_Quolu_)
- [Jenka](https://youtube.com/@jenkad2oob?si=OQpCGeBCEJBS0zHx)
- [Katie](https://github.com/Confetti3)
- [Kody Ivie](https://x.com/Kody_Ivie)
- [Solus](https://www.youtube.com/@Solus-yt)
- Breshi
- [Deltadog55](https://www.youtube.com/@deltadog55)
- Moosh
- [MoveableFormula](https://youtube.com/@movableformula)
- Z
- The Cube17

### Inspiration and Helpful Repositories

- https://github.com/v4nguard/tiger-pkg
- https://github.com/cohaereo/alkahest
- https://codeberg.org/V4NGUARD/tachyscope
- https://github.com/MontagueM/D2TagParser
- https://github.com/MontagueM/DestinyUnpackerCPP
- https://github.com/nblockbuster/D2TextureRipper
- https://github.com/v4nguard/tiger-parse
- https://github.com/Demonware-Custom-Server/demonware-cod4
- https://github.com/hosseinpourziyaie/demonware-companion
- https://github.com/jordam/demonbugger
- https://github.com/project-bo4/shield-development
- https://github.com/MontagueM/Charm
- https://github.com/v4nguard/quicktag
- https://github.com/nblockbuster/D2StaticDocs
- https://github.com/MontagueM/D2Maps
- https://github.com/MontagueM/DestinyMapmining
- https://github.com/nblockbuster/tachyscope
- https://github.com/cohaereo/destinydocs
- https://github.com/MontagueM/DestinyUnpacker
- https://github.com/nblockbuster/bungie-lua-decompiler

### Other

- [Ginsor](https://x.com/GinsorKR) - Provided useful pointers to the original Sunrise project

## Content Disclaimer

SunrisePlus installs onto an old supported build of Destiny 2 and runs completely locally

SunrisePlus does not connect to live Destiny 2 servers and is not intended for use with current or online versions of the game

Everyone must provide their own copy of the game

No game copy, copyrighted game data, online service or server is provided by this repository

## Legal Disclaimer

This project is not for profit

It does not affect live servers or newer versions of the game where research like this could pose a security risk

No copyrighted game data is included in the repository or releases

SunrisePlus is an independent open source extension of Sunrise

## AI Disclaimer

AI may be used for research, development, code review and documentation

All publicly released changes are reviewed by a human before being merged

AI is treated as a development tool and the developer remains responsible for the resulting code

## Affiliation Disclaimer

SunrisePlus is not affiliated with Bungie or Sony in any way
