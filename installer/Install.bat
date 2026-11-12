@echo off
rem Copies the Backline VST3 plugin to the standard Windows VST3 folder.
if "%~1"=="elevated" goto install
net session >nul 2>&1
if %errorlevel% equ 0 goto install
echo Asking for administrator rights...
set "SELF=%~f0"
powershell -NoProfile -Command "Start-Process -FilePath $env:SELF -ArgumentList elevated -Verb RunAs"
if %errorlevel% neq 0 (
    echo Could not get administrator rights. Right-click Install.bat and choose "Run as administrator".
    pause
)
exit /b

:install
set "DEST=%CommonProgramFiles%\VST3"
if not exist "%DEST%" mkdir "%DEST%"
xcopy /E /I /Y "%~dp0Backline.vst3" "%DEST%\Backline.vst3" >nul
if %errorlevel% neq 0 (
    echo Installation failed. If your DAW is open, close it and run this again.
) else (
    echo Backline was installed to %DEST%
    echo Rescan plugins in your DAW, then add Backline to an audio track.
)
pause
