@echo off
rem Copies the Backline VST3 plugin to the standard Windows VST3 folder.
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Asking for administrator rights...
    powershell -NoProfile -Command "Start-Process -FilePath %~f0 -Verb RunAs"
    exit /b
)
set "DEST=%CommonProgramFiles%\VST3"
if not exist "%DEST%" mkdir "%DEST%"
xcopy /E /I /Y "%~dp0Backline.vst3" "%DEST%\Backline.vst3" >nul
if %errorlevel% neq 0 (
    echo Installation failed.
) else (
    echo Backline was installed to %DEST%
    echo Rescan plugins in your DAW, then add Backline to an audio track.
)
pause
