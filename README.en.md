[简体中文](README.md) | [繁體中文](README.zh-Hant.md) | **English**

# PhoneRecord — Call Recording Workbench

The current C++ desktop source version is **2.7.3**. On Windows it receives network recordings from a compatible phone, starts recording when the call connects, saves the file after hangup, and includes built-in playback, health checks, and troubleshooting guidance.
Version 2.7.3 suggests local IPv4 automatically and follows the phone route for untouched suggestions, while preserving saved and manually entered addresses.
Version 2.7.2 adds Settings > Configure phone: review and confirm network recording and callback changes directly in the app, with encrypted backups and verified readback. No script is required. See the [Traditional Chinese PDF manual](output/pdf/PhoneRecord_操作與設定手冊.pdf).
Version 2.7.0 adds [updates](docs/自動更新與發布.md): periodic background checks, separate update balloons and a persistent tray arrow. You choose when to download and restart while idle. Install an updater-enabled version manually once; official binaries are published under [GitHub Releases](https://github.com/ChenRanReal/PhoneRecord/releases).
Version 2.6.0 adds historical call titles: select a recording and choose Rename or press F2. Titles remain searchable after reopening. Save blank to restore the original title; original contacts, numbers, times and audio files are retained.
Version 2.5.0 adds optional startup at sign-in. The Settings checkbox applies immediately and defaults to off. When enabled, the app starts in the tray. After moving the exe, turn the option off and on to update its path.
Version 2.4.1 adds a phone setup script: preview first, then after confirmation configure network recording and the two callbacks, verify by reading the settings back, and keep an encrypted backup. The original icon and the three-language interface remain.

[Windows releases](https://github.com/ChenRanReal/PhoneRecord/releases) · [Legacy standalone download](https://github.com/ChenRanReal/PhoneRecord/raw/refs/heads/main/cpp/PhoneRecord.exe) · [Icons and assets](cpp/assets/README.md) (Chinese)

**For a first run, start with [快速上手](docs/快速上手.md)** (Chinese) and complete PC setup, phone setup, the compatibility check, and a two-way listening test in that order. An [English quick start](docs/QuickStart.en.md) is also available.

## Startup and first-time setup

In this project, double-click [Start-PhoneRecord.cmd](Start-PhoneRecord.cmd), or run `cpp/PhoneRecord.exe` directly.
When handing the program to someone else, the exe can start on its own and does not need Python. Put it in a writable folder. The first launch opens the settings page with the required addresses and account left blank, so that person enters their own details.
The optional [config.example.ini](cpp/config.example.ini) is a blank template. Do not substitute the personal configuration file from this computer.

The lower left switches among 简体中文, 繁體中文, and English. Complete offline help is built in, including first-time setup, what each field is for, and the phone-side counterpart.
After connection settings are saved, use **Exit** at the lower left and reopen the program. The **X** at the upper right only minimizes to the tray by default.

If the login check passes but a connected call is not recorded, read [話機自動設定](docs/話機自動設定.md) first (Chinese). The phone still needs Network recording, a recording destination, and two call callbacks; the new version can generate the script itself.

## Main features

| Page | Purpose |
|---|---|
| Overview (运行概览) | Automatic recording, separate local and phone health indicators, the recording pipeline, and a timeline that can follow the latest events |
| Recordings (录音资料) | Search by title, contact, number or date; rename or restore titles; in-app play, pause and seek; reveal the file; signal check |
| Diagnostics (诊断中心) | Millisecond events and stable codes, item-by-item guidance, copy or export a report |
| Settings (连接设置) | Positive-integer ports, per-field descriptions, save folder, health-check interval, close behavior, balloon notifications, and startup at sign-in |
| Help & devices (帮助与适配) | Trilingual first-run help, generic callback templates, a read-only compatibility check, and a compatibility report that omits actual IP addresses |

Tray balloons are off by default and can be turned on by the user. The check does not rewrite the phone, place a test call, or trigger recording. After settings match, a real call of about 20 seconds is still required, and both sides must be listened to.

## Compatibility and ports

Verified combination: Fanvil X3S, hardware V1.1, firmware 2.4.7.9.1. For other models, inspect the web interface and the recording protocol first. Brand name or SIP capability alone does not establish compatibility.
Currently supported: Network server recording and PCMA/PCMU at 8kHz. The phone must supply mixed two-way audio.

| Use | Default | Phone-side counterpart |
|---|---|---|
| Phone web HTTP | 80 | The phone's actual management web port |
| Recording control TCP | 10000 | Recording server port |
| Call-event TCP | 18080, adjustable | The event port in the two Action URLs |
| Audio UDP | 20000–20100 | The range the phone uses to send audio to the PC |

Every address is filled in by the user. Changing the event port requires updating both phone callbacks together. Saving ordinary settings does not rewrite the phone automatically.

## Documentation and development

- [Quick start and handing the program to someone else](docs/快速上手.md) (Chinese). [English quick start](docs/QuickStart.en.md)
- [Operation, every field, and device capability](docs/操作與機型適配.md) (Chinese)
- [Use and troubleshooting](docs/PhoneRecord_使用與排查.md) (Chinese)
- [C++ build and modules](cpp/README.md) (Chinese)
- [Phone setup script and failure diagnosis](docs/話機自動設定.md) (Chinese)
- [Historical call titles and backup](docs/歷史通話命名.md) (Chinese)
- [Development, verification, and the optional Python edition](docs/開發與驗證.md) (Chinese)
- [Apache-2.0 license](LICENSE)

From the project root, run `cmd /c cpp\build.bat` to build the C++17 / x86 / static-CRT desktop program. The optional Python edition is in `src/`; see the development notes for how to use it. Do not let the two editions occupy the same ports at the same time. Personal settings, the login cache, recordings, and on-site troubleshooting data stay on the machine and are not added to the repository.
