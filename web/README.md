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
| `getTrackAudio`  | The last half minute of the track's audio, starting on a bar line           |

The plugin sends a `transport` event about 30 times a second: `{ host, preview, beat, bpm }`.

`src/` holds the page's sources: the template, the part generator (`m1d1-gen.js`) and the audio
analyser (`m1d1-listen.js`) with their tests (`node test-gen.js`, `node test-listen.js`).
The template's placeholders (`@@FRAMES@@`, `/*@@GEN@@*/`, ...) are filled with the generator,
the analyser, the character kit and the mascot frames to produce `m1d1.html`.
