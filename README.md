# DRUMBOX 64
A 16-step drum machine for the Commodore 64.

## Features
- 7 tracks: Kick, Snare, Closed Hat, Open Hat, Tom, Clap, Crash
- 16 steps per track with 4 velocity levels (off / soft / medium / loud)
- 3 drum kits: 909 (punchy electronic), 808 (deep boomy), Rock 
- 36 built-in presets including DnB, Jungle, Metal Thrash (240 BPM) and Blast Beat (300 BPM)
- Swing control (0–99%)
- Dual SID support (4 selectable addresses)
- Save/load patterns to disk (10 slots)
- Joystick support (port 2) with hold-fire edit mode

## Build
Requires the [Oscar64 compiler](https://github.com/drmortalwombat/oscar64).

```sh
oscar64 -o=drumbox.prg -O2 -g \
    main.c sid.c seq.c ui.c presets.c diskio.c -ii=../oscar64/include
```

## Run
```sh
x64sc -autostart drumbox.prg
```

## Controls

### Grid
| Key | Action |
|-----|--------|
| Cursor keys | Move around the grid |
| Space | Cycle step velocity: off → loud → medium → soft → off |
| P | Play / Stop |
| S | Stop |
| N / B | Next / Previous preset |
| + / - | Tempo ±2 BPM |
| < / > | Swing ±4% |
| C | Clear pattern |
| R | Reload preset |
| F1 / F3 / F5 | Kit: 909 / 808 / Rock |
| Q | Quit to BASIC |

### Edit Mode (swing + velocity)
Press **F7** or hold joystick fire (~1 second) to enter edit mode.
The help area is replaced with two parameter bars:

```
>> SW [||||||||........] 54  CLSC   <>   ← active row
   VL [||||............]  1  SOFT
   UP/DN:SWITCH  LFT/RGT:ADJUST  FIRE/F7:EXIT
```

| Control | Action |
|---------|--------|
| F7 | Toggle edit mode on/off |
| Joystick U/D | Switch between Swing and Velocity rows |
| Joystick L/R | Adjust the active row's value |
| Joystick fire | Exit edit mode |

### SID
| Key | Action |
|-----|--------|
| 2 | Toggle dual SID on/off |
| 3 | Cycle SID2 address (DE00 / DF00 / D500 / D420) |

### Disk I/O
| Key | Action |
|-----|--------|
| W | Save pattern to current slot |
| L | Load pattern from current slot |
| [ / ] | Previous / Next disk slot (0–9) |
| D | Cycle drive number (8–12) |

Patterns are saved as sequential files named `DBOX0` through `DBOX9`.

### Joystick (port 2)
- **Directional**: move cursor (up/down = track, left/right = step)
- **Short fire**: cycle step velocity
- **Hold fire ~1 sec**: enter edit mode (progress bar shown in status bar)
- **Fire in edit mode**: exit edit mode

## Velocity

Each step has 4 velocity levels cycled with Space or joystick fire.