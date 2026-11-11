# External players

MintVID's GUIs can start a player other than `mrplay` for some display
modes. Today there is one: **PiStorm (VideoCore)**, played by `mrberry`,
written by Michael (MichiB210). Its source, and `berryvp.library`'s, is in
`pistorm/` (from MintVID-PiStorm v97.1); `make -f Makefile.amiga mrberry`
builds it. It decodes H.264 on the Raspberry Pi's VideoCore.

The GUIs list the PiStorm modes only when a file called `mrberry` sits in
the same drawer as MintVID (`PROGDIR:mrberry`). Without it nothing changes.

There are two PiStorm modes. Both play the sound through Paula, decoded on
the 68k by MintVID's own decoders (MintAMP AAC/MP3, MP2, AC-3, PCM), so a
stock Emu68 kernel is enough:

- **PiStorm (VideoCore)** / GadTools "PiStorm": full screen on the Pi's
  HDMI.
- **PiStorm (VideoCore, window)** / GadTools "PiStorm (Window)": `mrberry
  W=50`, the picture in berryvp.library's P96 window.
  **F** in that window makes `mrberry` save the position in `<film>.t` and
  end with the status text `Fullscreen`; the GUI then starts it again full
  screen from there (`"<film>" --paula ... R W=C`). Films it cannot seek in
  (TS) start again from the beginning.

With Paula, Pause stops `mrberry` at the spot (saved in `<film>.t`, status
text `Paused`). The GUI shows "Paused (Play continues)"; Play, or Pause
again, starts the same film with `R` from there.

v97.1 can also send the sound over the Pi's HDMI, decoded on the Pi's
second core. That needs Michael's patched Emu68 1.0.7 kernel (BerryCore),
so the GUIs no longer offer it; from Shell, `mrberry <film> HDMI` (or
`--hdmi-audio`) still asks for it. The saved display value of the old
"Amiga audio" entry is read as PiStorm (VideoCore).

The backend supports a PiStorm Classic with a Raspberry Pi 3A+ and a
PiStorm32 lite with a CM4, both on Emu68 1.0.7. Follow its installation
instructions (`pistorm/README.md`), including `gpu_mem=64` (CM4: 128).
MintVID's build turns off v97.1's upscaling of films below 1280x720 to
1920x1080 on the VideoCore (`UPSCALE` turns it back on): it needed
`gpu_mem=256` on a Pi 3A+.

With a PiStorm display chosen, the IPTV and YouTube browsers start
`mrberry` too (`amiga/mr_external_pick.h`):
`"<url>" [audio/window options] [--user-agent "<ua>"] [--referer "<ref>"]`.
mrberry opens http(s) files, HLS playlists and YouTube pages with
MintVID's own network code (`core/mr_http.c`, `mr_hls.c`, `mr_youtube.c`,
and the `amiga/hls_fetch.c` segment prefetcher), started and stopped in the
same order as in mrplay. It writes no `.t` or subtitle side files for a URL.
Without `PROGDIR:mrberry` the browsers fall back to `mrplay --p96`. The
video must still be H.264; YouTube's 360p format and most IPTV channels are.

## Troubleshooting startup

Both controllers read the player's terminal status file after it exits, so
a quick startup failure is shown in the status line. Live and saved status
have separate sequence counters; the handoff must not discard an error just
because those counters happen to match. Old snapshots are cleared before
each new playback launch.

When launching `mrberry`, its standard output is captured in
`T:MintVID-mrberry.log` (overwritten on the next PiStorm launch). This also
captures its library/capability diagnostics when started from Workbench.
If no status arrives, the controller points to this log after an early
exit or after about 60 seconds; it does not kill the player.

To separate controller issues from backend/setup issues, run the same file
from an Amiga Shell in the MintVID drawer:

    mrberry "NASDRIVE:MintVID-Codec-TestSuite/mintvid_h264_360p_25fps.mp4" --no-audio

Then leave out `--no-audio`: the sound plays through Paula by default.
HDMI audio (`HDMI`) should only be tested with the matching BerryCore
kernel and audio modules installed. If the direct command also fails, keep its output
and check the backend requirements before changing the display setup.

## What the GUI does

The GUI starts the player as a new process:

- task name `MintVID player`, the same as `mrplay`
- `NP_Cli`, stack `MRPLAY_STACK_SIZE`
- arguments: `"<file>" --paula [--audio-rate=low] [--audio-mono] [W=50]`
  (the same audio options mrplay takes; with No audio ticked it is
  `--no-audio` instead of the audio options)

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
