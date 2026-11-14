# The M1D1 interface

The plugin's window is one self-contained web page, `m1d1.html`, compiled into the plugin
(`juce_add_binary_data` target `M1D1Web`) and shown by `plugin/WebEditor.cpp` in a web view
(WebView2 on Windows). The same page runs on its own in a browser as the interactive preview.

When the page finds JUCE's bridge (`window.__JUCE__`) it runs in plugin mode:

| Page calls       | What the plugin does                                                        |
|------------------|-----------------------------------------------------------------------------|
| `setSong`        | Plays the three parts (MIDI out on channels 1-3, sound for parts whose Play is on) in time with the host (`plugin/WebEngine.cpp`) |
| `setPreview`     | Runs its own clock from the page's position while the host is stopped       |
| `saveState`, `getState` | Keeps the page's session in the host project                         |
| `saveMidi`, `dragMidi`  | File dialog / a real Windows drag of the `.mid` file                 |
| `getTrackAudio`  | The last half minute of the track's audio, starting on a bar line, with the song beat it starts on (`startBeat`), the host tempo and its level |
| `getCaptureInfo` | How many beats of the track have been heard so far (for listening on its own) |

Inside the DAW the page starts empty (no example chords, no MIDI) and listens to the track on its own the
first time the host has played one whole 4-bar stretch of the song's grid. Track audio is analysed on the
host's grid (`M1D1Listen.analyse(..., { grid: { bpm, firstBeatSec, startBeat, beatsPerBar } })`): the host's
tempo, and the 16 beats that line up with the song's 4-bar grid, so the parts the plugin plays at
`ppq mod 16` are the chords at that place in the song. Repeated passes are averaged; a progression longer
than 4 bars is read from its latest pass. `node test-grid.js` measures this.

The plugin sends a `transport` event about 30 times a second: `{ host, preview, beat, bpm }`.

`src/` holds the page's sources: the template, the part generator (`m1d1-gen.js`) and the audio
analyser (`m1d1-listen.js`) with their tests (`node test-gen.js`, `node test-listen.js`).
The template's placeholders (`@@FRAMES@@`, `/*@@GEN@@*/`, ...) are filled with the generator,
the analyser, the character kit and the mascot frames to produce `m1d1.html`.
