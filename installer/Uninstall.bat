@echo off
setlocal EnableExtensions
title M1D1 uninstaller
rem Removes M1D1: the VST3 plugin, the standalone app, its shortcuts and its entry in Settings > Apps.
rem Your exported MIDI and audio files are never touched. Set M1D1_QUIET=1 to skip the questions and pauses.

if /i "%~1"=="elevated" goto ready
if /i "%~1"=="fromtemp" goto ready
net session >nul 2>&1
if %errorlevel% equ 0 goto ready
echo Asking for administrator rights (needed to remove files from the VST3 and Program Files folders)...
set "SELF=%~f0"
powershell -NoProfile -Command "Start-Process -FilePath $env:SELF -ArgumentList elevated -Verb RunAs"
if %errorlevel% neq 0 (
    echo Could not get administrator rights. Right-click Uninstall.bat and choose "Run as administrator".
    if not "%M1D1_QUIET%"=="1" pause
)
exit /b

:ready
set "VSTDIR=%CommonProgramFiles%\VST3"
set "APPDIR=%ProgramFiles%\M1D1"
set "KEY=HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\M1D1"

rem A batch file cannot delete the folder it runs from, so run a copy from the temp folder.
if /i not "%~1"=="fromtemp" (
    if /i "%~dp0"=="%APPDIR%\" (
        copy /Y "%~f0" "%TEMP%\M1D1-uninstall.bat" >nul
        rem no "call": control passes to the copy for good, so nothing reads this file after it is deleted
        "%TEMP%\M1D1-uninstall.bat" fromtemp
    )
)

echo.
echo  Uninstalling M1D1
echo  -----------------
echo.
if not "%M1D1_QUIET%"=="1" (
    choice /C YN /M "Remove the M1D1 plugin and app from this computer"
    if errorlevel 2 exit /b 0
)

for %%P in (FL64.exe FL.exe "FL Studio.exe" M1D1.exe) do (
    tasklist /FI "IMAGENAME eq %%~P" 2>nul | find /i "%%~P" >nul && (
        echo %%~P is running. Close FL Studio and M1D1, then run the uninstaller again.
        goto fail
    )
)

if exist "%VSTDIR%\M1D1.vst3\" (
    echo Removing the VST3 plugin...
    rmdir /s /q "%VSTDIR%\M1D1.vst3"
    if exist "%VSTDIR%\M1D1.vst3\" goto busy
)
if exist "%APPDIR%\" (
    echo Removing the standalone app...
    rmdir /s /q "%APPDIR%"
    if exist "%APPDIR%\" goto busy
)
echo Removing shortcuts...
powershell -NoProfile -Command ^
  "foreach ($dir in @([Environment]::GetFolderPath('CommonPrograms'), [Environment]::GetFolderPath('CommonDesktopDirectory'))) {" ^
  "  Remove-Item (Join-Path $dir 'M1D1.lnk') -ErrorAction SilentlyContinue }" >nul 2>&1
reg delete "%KEY%" /f >nul 2>&1

if not "%M1D1_QUIET%"=="1" if exist "%APPDATA%\M1D1\" (
    echo.
    choice /C YN /M "Also delete your M1D1 settings and sample sounds in %APPDATA%\M1D1"
    if not errorlevel 2 rmdir /s /q "%APPDATA%\M1D1"
)

echo.
echo  M1D1 has been removed. Files you exported (Music\M1D1\Exports) were kept.
echo  In FL Studio, run Options ^> Manage plugins ^> Find plugins once so it forgets M1D1.
echo.
if not "%M1D1_QUIET%"=="1" pause
if /i "%~1"=="fromtemp" (goto) 2>nul & del "%~f0"
exit /b 0

:busy
echo A file is still in use. Close FL Studio and M1D1, then run the uninstaller again.
:fail
echo.
if not "%M1D1_QUIET%"=="1" pause
exit /b 1
