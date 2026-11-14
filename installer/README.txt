M1D1 for Windows (64-bit) - a plugin by ACND
MIDI extractor and generator: audio in -> key, BPM, Camelot and chords -> MIDI for bass, chords and lead.

WHAT IS IN THIS FOLDER
  Install.bat      Double-click to install everything (asks for administrator rights).
  Uninstall.bat    Double-click to remove everything again.
  M1D1.vst3        The plugin for FL Studio (and any other VST3 host).
  M1D1.exe         The standalone app. Runs on its own, no DAW needed.
  version.txt      The version of this download.

INSTALL
  1. Close FL Studio.
  2. Unzip the download, open the M1D1 folder and double-click Install.bat. Click Yes when Windows asks.
     It installs:
       - the plugin to  C:\Program Files\Common Files\VST3\M1D1.vst3
       - the app to     C:\Program Files\M1D1\M1D1.exe, with Start menu and desktop shortcuts
       - the uninstaller, also listed in Settings > Apps > Installed apps
     An older "Backline" install (this plugin's earlier name) is removed so FL Studio does not show two copies.
  3. Open FL Studio: Options > Manage plugins > Find plugins. M1D1 appears in the list.

USE IT IN FL STUDIO
  - Put M1D1 on the mixer insert of the audio you want to build around.
  - Extract: press Play in FL Studio. M1D1 listens to the track and, after 4 bars, shows its key and
    chords on FL Studio's own tempo and bars, and writes bass, chords and lead that line up with the song.
    While FL Studio plays it keeps listening until it is sure (an intro gives way to the song, an 8-bar
    progression is read whole). Press LISTEN TO TRACK to read it again (for example from another part).
    Put M1D1 on your audio's own mixer insert, not on the Master or on the instrument playing its MIDI.
    Or press OPEN AUDIO (or drop a WAV/MP3 on M1D1) to read a file instead.
    Until it has heard something, M1D1 sends no MIDI.
  - Generate: pick a key, scale and tempo and M1D1 writes the parts for you.
  - KEEP a part you like and press NEW or GENERATE: the other parts are rewritten around it.
  - PLAY on a part (or the big Play button) previews it through FL Studio's mixer.
    When FL Studio plays, M1D1 follows its tempo and position.
  - Drag a part (the orange "bass.mid" tile) into the Playlist or the Piano roll, or press SAVE.
  - Your parts and settings are saved with the FL Studio project.
  M1D1's window uses Microsoft Edge WebView2, which comes with Windows 11 and current Windows 10.
  If the window stays empty, install it from https://go.microsoft.com/fwlink/p/?LinkId=2124703

  Play M1D1's MIDI on any synth (MIDI out):
  1. Open M1D1's plugin window, click the cog at the top left (Plugin options), and in the MIDI
     section set "Output port" to a free number, for example 1.
  2. Load your synth (FLEX, Serum, a piano...). In its Plugin options, set "Input port" to the same number.
  3. Chords come out on MIDI channel 1, bass on channel 2 and the lead melody on channel 3.
     A synth on that port plays every part M1D1 sends: in M1D1, press MIDI OUT and tick only the
     parts you want (for example only Chords for a piano).
  4. Press Play in FL Studio. The synth plays M1D1's parts in time with your project.

UNINSTALL
  Settings > Apps > Installed apps > M1D1 > Uninstall, or double-click Uninstall.bat
  (in this folder or in C:\Program Files\M1D1). Your exported MIDI and audio in Music\M1D1\Exports is kept.
  Afterwards run Options > Manage plugins > Find plugins once in FL Studio.

BY HAND (no installer)
  From a Command Prompt opened as administrator, in this folder:
      xcopy /E /I /Y M1D1.vst3 "%CommonProgramFiles%\VST3\M1D1.vst3"
  To remove:
      rmdir /s /q "%CommonProgramFiles%\VST3\M1D1.vst3"

WINDOWS WARNINGS
  The files are not code-signed, so Windows SmartScreen may say "Windows protected your PC".
  Choose "More info", then "Run anyway". The source code is public (AGPLv3).
