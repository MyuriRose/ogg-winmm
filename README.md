# ogg-winmm CD Audio Emulator

ogg-winmm is a wrapper of "winmm.dll", which is used by many games and programs to play CD-DA audio tracks.

ogg-winmm emulates the CD-DA audio tracks by reading them from ogg files instead of audio tracks on a CD-ROM.

ogg-winmm also provides separate volume control for CDDA/MIDI/WAVE, which has been missing in Windows Vista/7/8/10.

# Usage:

1. Rip the audio tracks from the CD and encode them to ogg files, following naming convention:
> **Track02.ogg, Track03.ogg, Track04.ogg, ...**

  Note that the file names can not contain whitespace, and the track numbers must be contiguous without gap in the middle.

  Also note the numbering usually starts from 02 since on mixed mode CDs the first track is a data track.
  
  In rare cases some games may use a pure audio CD with no data tracks in which case you should start numbering them from "Track01.ogg".

2. Extract the dll file and the ini file to the game folder where the game's main executable file is located.

3. Create a subfolder called "Music" there and put the ogg files in it. 

4. (Optional) Override the volume for CDDA/MIDI/WAVE in "winmm.ini" file if desired.

5. Run the game and enjoy the emulated CD audio.

# Requirement:

- Minimum operating system: Windows XP or newer. 

NOTE: It actually can also work on Win95/98 if you follow the extra procedure below:
1. Rename "winmm.dll" to "wincd.dll", and rename "winmm.ini" to "wincd.ini".
2. Use a hex editor to open the game's exe file, then replace all occurrences of string "winmm.dll" (case-insensitive) to "wincd.dll".

# Building:

- Use MinGW 6.3.0-1 or later to build on Windows OS.
- Dependencies:
  - libogg (https://github.com/gcp/libogg.git)
  - libvorbis (https://github.com/xiph/vorbis.git)

# Revisions:

v.2024.11.12
- Compatibility fix for thread scheduling on Windows XP or later.

v.2024.10.24
- Add an option to customize the music folder name.

v.2024.08.08
- Small compiler optimizations.

v.2024.03.21
- Fix the truncation at the end of each song ranging from 1ms to 500ms.

v.2023.07.24
- Correctly report stereo CD separate channel volume control capability.

v.2023.06.01
- Fix MCI_PAUSE & MCI_RESUME when not playing.
- Fix MCI_STATUS_LENGTH when time format is set to TMSF.
- Fix MCI_STATUS_POSITION when time format is set to milliseconds or MSF.

v.2023.04.14
- Implement MCI_SEEK & MCI_GETDEVCAPS.
- Allow aux volume control on left/right channel separately.  

v.2023.03.11
- Allow more than 1 data track before audio track.  
- Fix total track number.

v.2023.02.28
- Improve time range precision from second to millisecond.
- Add compatibility support for Win95/98.

v.2023.02.26:
- Implement time range playing down to seconds instead of always track-wise playing.
- Implement pause and resume.

v.2023.01.18:
- Implement play/seek/length/position for mciSendString.

v.2022.06.26:
- Fix MIDI file open bypass.

v.2022.05.26:
- Remove unnecessary monitor thread.
- Thread calling convention fix.
- Position calculation and status fix.
- Various compatibility fix.

v.2022.02.28:
- Implement MCI_INFO
- Fix MCI_SYSINFO
- Bugfixes for MCISendString

v.2022.02.27:
- Treat MCI_PAUSE as MCI_STOP and MCI_RESUME as MCI_PLAY, since MCICDA does not support resume.

v.2021.11.15:
- Ignore case when opening device.

v.2021.11.11:
- Post notify message to the correct window.
- Fix a race condition that sometimes causes music to stutter.

v.2021.11.10:
- Completely rewrite player control logic.
- Use persistent thread to improve performance.
- Use persistent buffer to improve performance.
- Fix various crash/freeze scenarios related to thread.
- Fix various memory leak scenarios related to buffer.

v.2021.08.08:
- Fix MCI_STATUS_LENGTH and MCI_STATUS_POSITION.
- Optimize player thread control.

v.2021.07.26:
- Relay MCI commands if they are not targeting cdaudio device.
- Add more stub functions to support more games.
- Filter MIDI source by the full driver name (wdmaud.drv).

v.2021.07.11:
- Implement an alternative method to control MIDI volume, which should make scaling coefficients identical to CDDA/WAVE volume control.

v.2021.07.10:
- Implement separate volume control for CDDA/MIDI/WAVE.
  This is necessary because Windows does not provide any method to separate the volume control of CDDA/MIDI/WAVE ever since Vista.

v.2021.07.09:
- Fix the start tick of each track.
  Since MCICDA does not support pause, unnecessary time accumulation is removed.

v.2021.07.08:
- Implement realtime TMSF report for MCI_STATUS_POSITION.
  Many games use this realtime status to sync up video and audio frame by frame.
