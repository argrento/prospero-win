# Headset and microphone: console probe

The title's headset routing and microphone are built on PS4 values (from
shadPS4) that nobody has checked on a PS5. This probe answers the open
questions from a real game title. A payload has other rights than a title,
so a payload result does not count. Each step names the log lines that
answer it.

## What to build and stage

- The title shell and runtime from the same commit: the title shell links
  the libSceAudioIn facade (`native/stubs/libSceAudioIn.c`), and ntdll
  exports the input calls the driver looks for.
- `tests/lab/audio_capture_lab.c`, built for 64 bits:

  ```sh
  x86_64-w64-mingw32-gcc -O2 -o audio_capture_lab.exe \
      tests/lab/audio_capture_lab.c -lole32 -luuid
  ```

  Copy it into a game's folder, or run it as its own program with a profile
  of its own. It writes `audio_capture_lab.txt` beside itself.

The log lines below are in the title's session log.

## 0. The title starts

Start any game with the new title shell, before anything else. Each title
imports libSceAudioIn now.

- The game starts, and the log has `PW_WINE64 microphone=ok user=<id>`.
- `microphone=not-found` means the firmware did not resolve libSceAudioIn
  for the title. The title then lists no microphone and carries on. Note it:
  the module must then be loaded before the first call, and no module id for
  it is known yet.

## 1. Where the main port plays

On the console, set Settings > Sound > Audio Output > Output to Headphones
to "All Audio". Start a game that plays sound all the time, such as a title
screen with music.

1. Without a headset, then with one in the DualSense jack, then with a USB
   headset. Plug in and pull out each one while the game plays.
2. At each step, write down where you hear the game, and copy the lines
   `PW_WINE64 audio main status=… output=… reroute=… raw=…`. The title logs
   one at start and one each time the state changes.
3. If the game moves to the headset and back by itself, the console routes
   the main port, and nothing more is needed. Note the `output` bits it
   reports with a headset (PS4: `0x40`).
4. If it stays on the TV, repeat the run with each of these lines in the
   profile:
   - `[debug]` `env = PW_AUDIO_ROUTE=main-user`: the main port is opened as
     the player instead of the system user.
   - `[debug]` `env = PW_AUDIO_ROUTE=headset`: the title also opens the
     player's personal port, and plays there while that port reports a
     headset (`PW_WINE64 audio route=personal`).
5. Set Output to Headphones to "Chat Audio" and repeat step 1 with no
   `[debug]` line. Then repeat it with `PW_AUDIO_ROUTE=headset`. The game
   should stay on the TV. The headset policy cannot read this setting, so
   note whether the main port's `output` bits differ between the two
   settings while a headset is plugged in. If they differ, the policy can
   follow the setting.

## 2. The microphone

Run `audio_capture_lab.exe` (`audio_capture_lab.exe 5` records five
seconds). Speak while it records. It plays the recording back.

1. Run it with only the DualSense, then with a headset with a microphone in
   the jack, then with a USB headset.
2. For each run, copy `audio_capture_lab.txt` (endpoints, mix format, frames,
   peak and RMS) and these log lines:
   - `PW_WINE64 microphone open first user=… type=… rate=…` and
     `… result=…`: which open the system took. If every attempt is
     refused, note the results: the output port opens only under the
     console's umask 0, and the microphone may need the same, which the
     title cannot set safely while the game runs.
   - `PW_WINE64 microphone silent=…`: the silent state.
   - `PW_WINE64 microphone close reads=… failures=… reopens=…`.
3. Start a recording with only the DualSense. Plug in a headset halfway
   through, then pull it out. Does the voice move to the headset microphone
   and back? Look for `silent=` changes and for `open silent` (a reopen).
   If the voice stays on the controller, run it again with
   `env = PW_AUDIO_MIC_REOPEN=plug`: the title then reopens the microphone
   when the output's port state changes.
4. Press the DualSense mute button during a recording, and also try the
   system's microphone mute. Note the `silent=` value and whether the peak
   drops to 0. Note whether the microphone indicator is on only while the lab
   records.
5. With a party chat running, run the lab again and note the `result=` of
   the open.

## 3. Profile settings

1. Add `[audio]` `volume = 25` to the profile and play: the game is
   quieter, on the TV and on a headset. The log has `volume=25`.
2. Add `microphone = off`: the lab says "no default capture device". The log
   has `PW_WINE64 microphone=off`, and the microphone indicator stays off.

## Capabilities

A kit with this title shell and runtime has:

- `audio-capture`: Wine lists the microphone as its default capture device.
- `settings-audio-volume`: the title reads `[audio] volume`.
- `audio-microphone`: the title reads `[audio] microphone`.

Older title shells refuse a profile with an `[audio]` section.
