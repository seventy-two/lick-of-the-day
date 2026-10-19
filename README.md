# Lick of the Day

A small Windows app with a guitar lick to learn each day: a phrase of a
solo, picked from the Guitar Pro files on disk, with its tab and a
player to loop it, slow it down, and speed it up a step each time round.

It's written in C++ with Win32, Direct3D 11 and
[Dear ImGui](https://github.com/ocornut/imgui), drawn frameless like
kitchensink's Windows client, and builds to a single `dailylick.exe`.

## What it does

- **Reads Guitar Pro 3, 4, 5, 6 and 7–8** (`.gp3`, `.gp4`, `.gp5`, `.gpx`,
  `.gp`) itself: readers ported from [alphaTab](https://github.com/CoderLine/alphaTab)'s,
  checked against alphaTab on 787 real tabs (756 read identically; the rest
  differ where these are arguably righter, such as drums on channel 10 and
  accented names).
- **Finds the solo.** If the tab names its sections, "Solo", "Lead" and so
  on count for a lot. Otherwise it's the notes: each bar of each guitar
  track is scored for busy single notes and double stops up the neck,
  bends, slides, hammer-ons, tapping and vibrato, against palm muting,
  chords, tremolo riffs (one low note picked over and over), bars repeated
  over and over, and another guitar playing the same thing. A section
  named as a solo doesn't make a tremolo riff one, and when it's the tab's
  last marker it counts for 16 bars, not to the end of the song. A run of four or more high-scoring bars is a solo. Tested with the
  labels hidden on the 282 tabs that have them, the top pick falls in a
  labelled solo about 85% of the time.
- **Picks the lick**: a phrase of the solo that stands on its own. Phrases
  are counted from where the solo starts: its marker if the tab names it,
  else the bar where the lead clearly comes in; if it fades in, from the
  song's own count (from the top, or the last section marker, repeat or
  change of time signature). A marker, repeat or change of time signature
  inside the solo starts the count again. A phrase is four bars (eight
  short ones such as 2/4, two long ones such as 8/4). A lick is a whole
  phrase when it's at most 80 notes and 20 seconds at full speed, else
  half of one (or a quarter), still starting where the phrase or its second
  half does. The phrase that opens the solo is favoured; which one you get
  is seeded by the date. Sections named "Pre-solo" or "Post solo" don't
  count as solo. Tested with the markers hidden on tabs that have them, the
  count lines up with the marked solo 75% of the time.
- **One a day**: each day's song comes from a seeded shuffle of the songs
  (versions of one song count once), leaving out songs used in the last
  120 days. The day's lick is saved, so it's the same all day. **Another**
  skips to a different one.
- **Favourites and done**: the ☆ by the title keeps the lick as a
  favourite; the ✓ marks its solo done, so it never comes round as a lick
  of the day again (in that file or another version of the song; other
  solos in the song still can). The history button at the top opens
  Favourites, History (each day's lick) and Done: click one to play it
  (**Today** goes back), or star, tick and untick from there.
- **Plays it** through a General MIDI soundfont (Sonivox, built in) with
  [TinySoundFont](https://github.com/schellingb/TinySoundFont) and
  [miniaudio](https://miniaud.io): bends, slides and vibrato as pitch-wheel
  moves, palm muting and dead notes shorter, ties held, grace notes before
  the beat.
  - **Loop** goes round and round; **Ramp** starts at 60% and adds 5% each
    time round up to 100%. The "Ramp 60→100%, +5" link above the controls
    changes all three, and the ↺ beside it starts the ramp again: back to
    the start speed, from the top.
  - **Pitch − / +** shift everything but the drums a semitone at a time,
    to play along on a guitar tuned differently: the tab stays as written,
    so the shapes are the same. It remembers the guitar (by its top
    string), not the shift, so a D standard guitar gets +3 on a B standard
    tab and −2 on an E standard one without asking. Click the number to
    play tabs as written again.
  - **− / +** change the speed by 5% (25–150%).
  - **Click** adds a metronome and a bar's count-in.
  - **Band** plays the other parts too; otherwise only the lick's guitar.
  - **Whole solo** shows and plays the whole solo, scrolling with the
    cursor.
  - **Open** opens the file in whatever opens `.gp` files (Guitar Pro,
    TuxGuitar).
- **Out of the way while looping**: while a loop plays, the panel slides
  off to the left so the tab has the whole window, with the loop and speed
  in its corner. It comes back on pause, or when the mouse goes to the
  window's left edge.
- **The tab**: one line of tablature with bar numbers, sections, time
  signatures, ties, dead notes, harmonics, grace notes, hammer-ons and
  pull-offs, slides, bends and releases, vibrato, palm muting, taps,
  tremolo picking, and rhythm below (stems, beams, flags, dots, tuplets,
  rests). The notes being played light up.

Keys: Space play/pause, L loop, R ramp, Home start the ramp again,
← → speed, ↑ ↓ pitch, M click, B band, S whole solo, F favourite,
H favourites and history.

## Files

`%APPDATA%\dailylick`:

- `config.json`: `folders` searched for tabs (with subfolders; Documents\guitar
  and Downloads at first), `ramp` (`start`, `step`, `end`, in percent),
  `repeatDays`, the player's switches, `tuneTo` (the guitar's top string
  as a MIDI note, 0 for as written), and the window's place.
- `state.json`: each day's lick and what was skipped, the favourites and
  solos done, and the files found to have no solo (not read again until
  they change).

## Building and testing

`build.bat` builds `build\dailylick.exe` and the tests (`build\tests.exe`)
with Visual Studio 2022's C++ tools, and runs the tests. From WSL, copy the
checkout to a Windows folder and run `cmd.exe /c build.bat` there.

Everything but the window builds on Linux too:

```sh
gcc -O2 -Ithird_party -c third_party/miniz/miniz.c src/miniaudio.c
g++ -std=c++17 -O2 -Isrc -Ithird_party -o tests tests/tests.cpp src/daily.cpp src/library.cpp \
  src/player.cpp src/sequence.cpp src/lick.cpp src/gp.cpp src/gp345.cpp src/gpif.cpp src/model.cpp \
  third_party/pugixml/pugixml.cpp miniz.o miniaudio.o -lpthread -ldl -lm
./tests
```

`tools/` has command-line helpers built the same way: `dump` (what the
readers make of files), `licks` (the lick picked in each file), `today`
(the daily pick for some dates over a folder) and `render` (a file's lick
played into a WAV).

## Third-party code

In `third_party/`, each with its licence: Dear ImGui (MIT), pugixml (MIT),
miniz (MIT), TinySoundFont (MIT), miniaudio (public domain / MIT-0),
nlohmann/json (MIT). `res/sonivox.sf2` is the Sonivox General MIDI
soundfont (Apache 2.0), as shipped with alphaTab.
