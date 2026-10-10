# cs2ac (Plugify)

CS2AC 1.0.14 - server-side anti-cheat for Counter-Strike 2 by karola3vax, ported from its Metamod:Source plugin to Plugify/s2sdk. Watches the usercmds, movement, aim, shot and settings data players send to the server and reports and punishes 18 detection categories (aimbot, aimlock, antiaim, autostrafe, bhop, dll injection, desubticking, doubletap, hyperscroll, inhuman accuracy, invalid cvar, invalid input, irregular behavior, namechanger, nulls, silentaim, subtick spam, triggerbot). Usercmd and movement data come from polyhook detours on the gamedata-resolved libserver.so functions, everything else from s2sdk (events, net messages, client cvars, schema). Configuration and translations are the original cs2ac.cfg and translations/&lt;lang&gt;.txt. Administrator commands: cs2ac_status, cs2ac_help, cs2ac_reload, cs2ac_check_config, cs2ac_test_announcement, cs2ac_webhook_test, cs2ac_config_loaded, plus the diagnostics cs2ac_hooks and cs2ac_usercmd_dump. Reports go to the server console, chat, center screen, a configurable admin-plugin ban/kick command and an optional Discord webhook. Known port limitations (see PORT_PLAN.md): the auto-updater is not ported, the DLL Injection detector stays off unless its vtable index is verified in gamedata, and the usercmd protobuf reader reports through cs2ac_usercmd_dump so it can be confirmed on a live server.

Plugify port (fork) of **CS2AC** by karola3vax (Metamod (C++)).
Original: https://github.com/karola3vax/CS2AC
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, polyhook

## Installation
Download `cs2ac-1.0.15.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/cs2ac`.

## License
AGPL-3.0 (as the original), see LICENSE.

