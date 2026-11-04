# Rainbow Six Vegas 2: LAN & Soundloop patch

A small `dsound.dll` for the PC version of *Tom Clancy's Rainbow Six Vegas 2*. It fixes the stuck gunfire sound in
multiplayer, turns voice chat off, and raises the player and terrorist limits. You no longer need a CPU affinity tool.

It is made for LAN play. This repository contains no game files, so you need your own copy of the game.

## Install

1. Close the game.
2. Copy `Binaries\dsound.dll` from this repository into the `Binaries` folder of your game, next to `R6Vegas2_Game.exe`.
3. Start `Binaries\R6Vegas2_Game.exe` directly.

Do not start the game through another launcher or patcher. Running this patch together with one that changes the same
limits has not been tested, and it is not needed.

**To uninstall,** delete `Binaries\dsound.dll`. The patch changes the game only in memory and leaves the game's files alone.

## What it does

| Change | Effect | Where the DLL has to be |
|---|---|---|
| Sound fix | The "end of burst" sound is sent reliably, so a lost network packet no longer leaves gunfire looping | Host or dedicated server |
| Sound watchdog | Ends a loop that has run longer than any burst can last (6 s, 18 s for light machine guns), or whose source has gone | Client |
| Voice chat off | The voice chat engine never starts. This cannot be switched off without removing the DLL | Client |
| More players | Removes the limits on players per server and per team, and sets 16 players for a game hosted from the menu | Host or dedicated server |
| More terrorists | 99 terrorists per terrorist hunt match, 8 at one time by default | Host or dedicated server |
| Server list | A dedicated terrorist hunt server shows up in the terrorist hunt list | Dedicated server |

**A client alone gets only part of the sound fix.** Its watchdog stops a stuck loop after a few seconds, but only the DLL
on the host or server prevents the loop. Install it on every PC, including the server.

The game also runs on all CPU cores with this setup, so you do not need to restrict the CPU affinity.

## Terrorist settings

These settings count on the PC that runs the match: the dedicated server, or the game that hosts it. To change them, create
`Binaries\r6v2lan-mods.ini` there:

```ini
[Terrorists]
Total=99
AtOnce=8
```

| Key | Meaning | Default | Allowed |
|---|---|---|---|
| `Total` | Terrorists in a match | 99 | 1 to 999 |
| `AtOnce` | Terrorists alive at one time | 8 | 1 to 32 |

The file is read when the game or server starts. Values outside the range are set to the nearest allowed value.

## Dedicated server

Copy `dsound.dll` into `Binaries` as well and start `RainbowSixVegas2_SADS.exe` as usual. Your server's options file has to
allow the players you want: set `m_iMaxPlayers=16`. A dedicated server cannot be given a name with this patch: it shows the
game's menu text for your language, such as `PLAYER1`.

## Requirements

The patch was built and tested against these two files:

| File | MD5 |
|---|---|
| `R6Vegas2_Game.exe` | `04e295c4c51bfd3a1e9ad42a86b6462a` |
| `RainbowSixVegas2_SADS.exe` | `293587401f9f580c984a9d8f886738ca` |

The DLL compares the bytes it wants to change before it changes them. With another build it leaves those parts of the game
unpatched and writes the reason to `Binaries\r6soundfix.log`. An exe with another file name is left alone without a log
entry. Sound still works in both cases, because audio is passed on to the Windows DirectSound.

## Tested and not tested

Tested on one LAN with the game on a Windows 10 PC, a dedicated server on a Windows 11 PC, and the mod *The Complete
Overhaul* and ReShade 2.0.3 installed:

- The game and the server start with only this DLL added.
- The server appears in the terrorist hunt list, and a client can join it.
- Gunfire loops stopped: lost end sounds were measured with a throttled connection, once with the fix and once without.
- The watchdog stopped a stuck loop, and a loop also ended when the shooter died.
- With `AtOnce=32`, more terrorists were active at one time.

Not confirmed yet:

- More than 4 players on one terrorist hunt server.
- `AtOnce=32` in a long match or with several clients. Lower the value if the server struggles.
- `Total` above 99.
- Whether a match with 99 terrorists runs to the end. The mission keeps its own count of 45, and no test got that far.
- A game without the mod.
- Online play. Anti-cheat on someone else's server may react to a game that is changed in memory, so use this on LAN.

## Known side effects

The player limit works by keeping two game counters at 0: the number of players and the size of a team. As a result:

- The player limit you set is not enforced, so more than 16 players could try to join.
- The game itself counts 0 players, which may show in the server's window title.
- Team balance in team modes may be off, because both teams count as empty. This has not been checked.

## Troubleshooting

**The game shows a black screen and seems to hang.** If you have `Binaries\fpslimiter.cfg` from a community fix pack, check
its `InitialFps` line. With `InitialFps=1.0` the game runs at one frame per second. Set it to `InitialFps=90.0`.

**No server shows up in the list.** Allow inbound UDP for `R6Vegas2_Game.exe` and `RainbowSixVegas2_SADS.exe` in the Windows
Firewall. If you once cancelled the firewall prompt, Windows may have added a rule that blocks them. Remove that rule too.

**Something else does not work.** Check whether `Binaries\r6soundfix.log` exists. It is only written when a part of the
patch could not be applied.

The game and ReShade write their own logs to `KellerGame\Logs` and `Binaries\d3d9.log`. They have nothing to do with this
patch.

## Building from source

The source is in [`source`](source). You need [zig](https://ziglang.org) on your `PATH`. Run `source\build.cmd`, which
writes `_release\Binaries\dsound.dll`. The DLL is 32-bit and uses no C runtime.

## License

The code in this repository is under the [MIT License](LICENSE). It covers neither the game nor any of its files.

This project is not affiliated with Ubisoft. *Rainbow Six* is a trademark of Ubisoft.
