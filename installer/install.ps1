# Installs M1D1 for Windows: the VST3 plugin, the standalone app, shortcuts and the uninstaller.
# Run from Command Prompt:
#   powershell -NoProfile -ExecutionPolicy Bypass -Command "irm https://github.com/AhmedMasum2000000/Wingman/releases/latest/download/install.ps1 | iex"
# It downloads M1D1-Windows.zip and runs the Install.bat inside it, so both ways of installing do the same thing.
$ErrorActionPreference = 'Stop'
$base = 'https://github.com/AhmedMasum2000000/Wingman/releases/latest/download'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin = ([Security.Principal.WindowsPrincipal] $identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host 'Asking for administrator rights (needed to write to the VST3 and Program Files folders)...'
    # Keep the elevated window open whatever happens, so an error is never mistaken for success.
    $again = "try { irm $base/install.ps1 | iex } catch { Write-Host `$_ -ForegroundColor Red } finally { Read-Host 'Press Enter to close' }"
    Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', $again
    return
}

$work = Join-Path $env:TEMP 'M1D1Install'
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Path $work | Out-Null

$zip = Join-Path $work 'M1D1-Windows.zip'
if ($env:M1D1_ZIP) {
    Copy-Item $env:M1D1_ZIP $zip            # local package, used by the build to test this script
} else {
    Write-Host 'Downloading M1D1...'
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest "$base/M1D1-Windows.zip" -OutFile $zip -UseBasicParsing
}
Expand-Archive -Path $zip -DestinationPath $work -Force

$env:M1D1_QUIET = '1'
& cmd.exe /c "`"$(Join-Path $work 'M1D1\Install.bat')`" elevated"
$code = $LASTEXITCODE
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
if ($code -ne 0) { throw "The installer stopped (code $code). Close FL Studio and try again." }
