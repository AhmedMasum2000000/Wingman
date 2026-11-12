# Installs Backline for Windows: the VST3 plugin and the standalone app.
# Run from Command Prompt:
#   powershell -NoProfile -ExecutionPolicy Bypass -Command "irm https://github.com/AhmedMasum2000000/Wingman/releases/latest/download/install.ps1 | iex"
$ErrorActionPreference = 'Stop'
$base = 'https://github.com/AhmedMasum2000000/Wingman/releases/latest/download'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin = ([Security.Principal.WindowsPrincipal] $identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host 'Asking for administrator rights (needed to write to the VST3 folder)...'
    # Keep the elevated window open whatever happens, so an error is never mistaken for success.
    $again = "try { irm $base/install.ps1 | iex } catch { Write-Host `$_ -ForegroundColor Red } finally { Read-Host 'Press Enter to close' }"
    Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', $again
    return
}

$work = Join-Path $env:TEMP 'BacklineInstall'
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Path $work | Out-Null

$zip = Join-Path $work 'Backline-Windows.zip'
if ($env:BACKLINE_ZIP) {
    Copy-Item $env:BACKLINE_ZIP $zip            # local package, used by the build to test this script
} else {
    Write-Host 'Downloading Backline...'
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest "$base/Backline-Windows.zip" -OutFile $zip -UseBasicParsing
}
Expand-Archive -Path $zip -DestinationPath $work -Force

$vstFolder = Join-Path $env:CommonProgramFiles 'VST3'
New-Item -ItemType Directory -Path $vstFolder -Force | Out-Null
$target = Join-Path $vstFolder 'Backline.vst3'
if (Test-Path $target) {
    try { Remove-Item $target -Recurse -Force }
    catch { Write-Host 'Backline is in use. Close your DAW and run the installer again.' -ForegroundColor Red; throw }
}
Copy-Item (Join-Path $work 'Backline\Backline.vst3') $vstFolder -Recurse -Force

$appFolder = Join-Path $env:ProgramFiles 'Backline'
New-Item -ItemType Directory -Path $appFolder -Force | Out-Null
Copy-Item (Join-Path $work 'Backline\Backline.exe') $appFolder -Force

Remove-Item $work -Recurse -Force
Write-Host ''
Write-Host "Plugin installed to     $target"
Write-Host "Standalone app is at    $(Join-Path $appFolder 'Backline.exe')"
Write-Host 'Rescan plugins in your DAW, then add Backline to an audio track.'
