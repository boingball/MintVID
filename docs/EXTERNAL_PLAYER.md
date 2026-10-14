# External players

MintVID's GUIs can start a player other than `mrplay` for some display
modes. Today there is one: **PiStorm (VideoCore)**, played by `mrberry` from
[MichiB210/MintVID-PiStorm](https://github.com/MichiB210/MintVID-PiStorm).
It decodes H.264 on the Raspberry Pi's VideoCore and shows it full screen on
the Pi's HDMI output.

MintVID does not ship `mrberry`. The GUIs list the PiStorm mode only when a
file called `mrberry` sits in the same drawer as MintVID (`PROGDIR:mrberry`).
Without it nothing changes.

The IPTV and YouTube browsers still start `mrplay`. If the controller is set
to PiStorm, they play in a P96 window.

## What the GUI does

The GUI starts the player as a new process:

- task name `MintVID player`, the same as `mrplay`
- `NP_Cli`, stack `MRPLAY_STACK_SIZE`
- arguments: `"<file>" [--no-audio]`

The file name is quoted. A `"` inside it is written as `*"`, and a `*` as
`**`: the usual AmigaDOS rules, so `ReadArgs()` or the C runtime's `argv`
read it back correctly.

Pause, Stop and the status line then work exactly as they do for `mrplay`.

## What the player must do

1. **Signals** (sent to the task named `MintVID player`):
   - `Ctrl-F`: stop, clean up, exit. This is what the Stop button sends.
     Treat `Ctrl-C` the same way.
   - `Ctrl-D`: pause, or continue if already paused.
   - `Ctrl-E`: read and clear `commands` in the control port below, then act
     on the bits it understands. `MR_PLAYER_COMMAND_PAUSE` toggles pause,
     and `MR_PLAYER_COMMAND_FAST` (or `Ctrl-E` with no bits set) jumps
     forward. Ignore the other bits.

2. **Control port.** Publish an `mr_player_control_port`
   (`player/amiga/mr_player_status.h`) named `MintVID.player`
   (`MR_IPTV_PLAYER_PORT`) while running, and remove it before exit. Do not
   publish it if a port of that name already exists.

3. **Status.** Keep `status` in that port up to date with
   `mr_player_status_set()`: `OPENING` while starting, `PLAYING` with a short
   codec name and a text line while playing (`"Paused"` in the text while
   paused is fine), and `ENDED`, `ERROR` or `UNSUPPORTED` at the end.

4. **Final status file.** On every way out, write the final state to
   `T:MintVID.player-status` (an `mr_player_status_snapshot`, written to
   `T:MintVID.player-status.tmp` first and then renamed over it). The GUI
   reads it after the process has gone, so it can show why playback ended.

5. **Clean exit.** Leave the screen and input as they were, on every path,
   errors included.

Keyboard control inside the player is up to the player. `mrplay` uses ESC
to stop, Space to pause and the cursor keys to jump.
