Backline for Windows (64-bit)

Backline.exe      Runs on its own, no DAW needed. Double-click to try it.
Backline.vst3     The plugin for your DAW.
Install.bat       Double-click to install the plugin (asks for administrator rights).

Installing by hand from a Command Prompt opened as administrator, in this folder:

    xcopy /E /I /Y Backline.vst3 "%CommonProgramFiles%\VST3\Backline.vst3"

Then rescan plugins in your DAW and put Backline on the audio track you want to build around.

Windows may warn that the files come from an unknown publisher because they are not
code-signed. Choose "More info" then "Run anyway".
