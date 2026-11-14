@echo off
setlocal EnableExtensions
title M1D1 installer
rem Installs M1D1 for Windows (64-bit):
rem   the VST3 plugin   -> %CommonProgramFiles%\VST3\M1D1.vst3   (where FL Studio looks for VST3 plugins)
rem   the standalone app -> %ProgramFiles%\M1D1\M1D1.exe, with Start menu and desktop shortcuts
rem   the uninstaller    -> %ProgramFiles%\M1D1\Uninstall.bat, also listed in Settings > Apps
rem Set M1D1_QUIET=1 to skip the pauses (used by the automatic tests).

if /i "%~1"=="elevated" goto install
net session >nul 2>&1
if %errorlevel% equ 0 goto install
echo Asking for administrator rights (needed to write to the VST3 and Program Files folders)...
set "SELF=%~f0"
powershell -NoProfile -Command "Start-Process -FilePath $env:SELF -ArgumentList elevated -Verb RunAs"
if %errorlevel% neq 0 (
    echo Could not get administrator rights. Right-click Install.bat and choose "Run as administrator".
    if not "%M1D1_QUIET%"=="1" pause
)
exit /b

:install
set "SRC=%~dp0"
set "VSTDIR=%CommonProgramFiles%\VST3"
set "APPDIR=%ProgramFiles%\M1D1"
set "KEY=HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\M1D1"
set "VERSION=0.0.0"
if exist "%SRC%version.txt" set /p VERSION=<"%SRC%version.txt"

echo.
echo  M1D1 %VERSION%  -  a plugin by ACND
echo  ----------------------------------
echo.

if not exist "%SRC%M1D1.vst3\" (
    echo M1D1.vst3 was not found next to Install.bat. Unzip the whole download first, then run Install.bat from the unzipped folder.
    goto fail
)

rem FL Studio keeps the plugin file open while it runs, so it has to be closed.
for %%P in (FL64.exe FL.exe "FL Studio.exe" M1D1.exe) do (
    tasklist /FI "IMAGENAME eq %%~P" 2>nul | find /i "%%~P" >nul && (
        echo %%~P is running. Close FL Studio and M1D1, then run Install.bat again.
        goto fail
    )
)

rem The earlier name of this plugin was Backline. It has the same plugin ID, so remove it to avoid two copies in FL Studio.
if exist "%VSTDIR%\Backline.vst3\" (
    echo Removing the old Backline plugin...
    rmdir /s /q "%VSTDIR%\Backline.vst3"
)
if exist "%ProgramFiles%\Backline\" rmdir /s /q "%ProgramFiles%\Backline"

echo Installing the VST3 plugin...
if not exist "%VSTDIR%" mkdir "%VSTDIR%"
if exist "%VSTDIR%\M1D1.vst3\" rmdir /s /q "%VSTDIR%\M1D1.vst3"
xcopy /E /I /Y /Q "%SRC%M1D1.vst3" "%VSTDIR%\M1D1.vst3" >nul
if errorlevel 1 goto copyfail

echo Installing the standalone app...
if not exist "%APPDIR%" mkdir "%APPDIR%"
copy /Y "%SRC%M1D1.exe" "%APPDIR%\M1D1.exe" >nul || goto copyfail
copy /Y "%SRC%Uninstall.bat" "%APPDIR%\Uninstall.bat" >nul || goto copyfail
if exist "%SRC%README.txt" copy /Y "%SRC%README.txt" "%APPDIR%\README.txt" >nul
> "%APPDIR%\version.txt" echo %VERSION%

echo Adding shortcuts...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$s = New-Object -ComObject WScript.Shell;" ^
  "foreach ($dir in @([Environment]::GetFolderPath('CommonPrograms'), [Environment]::GetFolderPath('CommonDesktopDirectory'))) {" ^
  "  $l = $s.CreateShortcut((Join-Path $dir 'M1D1.lnk')); $l.TargetPath = Join-Path $env:APPDIR 'M1D1.exe';" ^
  "  $l.WorkingDirectory = $env:APPDIR; $l.Description = 'M1D1 MIDI extractor and generator'; $l.Save() }" >nul 2>&1

echo Registering the uninstaller...
reg add "%KEY%" /f /v DisplayName /t REG_SZ /d "M1D1" >nul
reg add "%KEY%" /f /v DisplayVersion /t REG_SZ /d "%VERSION%" >nul
reg add "%KEY%" /f /v Publisher /t REG_SZ /d "ACND" >nul
reg add "%KEY%" /f /v InstallLocation /t REG_SZ /d "%APPDIR%" >nul
reg add "%KEY%" /f /v DisplayIcon /t REG_SZ /d "%APPDIR%\M1D1.exe" >nul
reg add "%KEY%" /f /v UninstallString /t REG_SZ /d "\"%APPDIR%\Uninstall.bat\"" >nul
reg add "%KEY%" /f /v NoModify /t REG_DWORD /d 1 >nul
reg add "%KEY%" /f /v NoRepair /t REG_DWORD /d 1 >nul

echo.
echo  Done.
echo.
echo  Plugin:      %VSTDIR%\M1D1.vst3
echo  App:         %APPDIR%\M1D1.exe  (Start menu and desktop: M1D1)
echo  Uninstall:   Settings ^> Apps ^> M1D1, or run %APPDIR%\Uninstall.bat
echo.
echo  In FL Studio: Options ^> Manage plugins ^> Find plugins, then add M1D1 as an effect on a mixer
echo  insert (or load it in a channel). To play its MIDI on a synth, see README.txt.
echo.
if not "%M1D1_QUIET%"=="1" pause
exit /b 0

:copyfail
echo Copying failed. If FL Studio or M1D1 is open, close it and run Install.bat again.
:fail
echo.
if not "%M1D1_QUIET%"=="1" pause
exit /b 1
