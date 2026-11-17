#include "Updater.h"

#include <juce_cryptography/juce_cryptography.h>

namespace m1d1
{
namespace
{
    const char* const repoReleases = "https://github.com/AhmedMasum2000000/Wingman/releases/";
    const char* const latestApi = "https://api.github.com/repos/AhmedMasum2000000/Wingman/releases/latest";
    constexpr juce::int64 recheckMs = 12 * 60 * 60 * 1000;     // a kept answer is used for 12 hours
    constexpr juce::int64 maxDownload = 512 * 1024 * 1024;     // a package is about 14 MB

    // The tests point the updater at their own release (a file or a local server) with M1D1_UPDATE_API; only then
    // may the download come from somewhere other than this project's GitHub releases.
    juce::String testApi() { return juce::SystemStats::getEnvironmentVariable ("M1D1_UPDATE_API", {}); }
    bool allowedDownload (const juce::String& url) { return testApi().isNotEmpty() || url.startsWith (repoReleases + juce::String ("download/")); }
    bool allowedPage (const juce::String& url) { return testApi().isNotEmpty() || url.startsWith (repoReleases); }

    struct Fetched { int status = 0; std::unique_ptr<juce::InputStream> stream; };

    Fetched fetchUrl (const juce::String& address, int timeoutMs, bool api)
    {
        Fetched f;
        const juce::URL url (address);
        if (url.isLocalFile())
        {
            const auto file = url.getLocalFile();
            f.status = file.existsAsFile() ? 200 : 404;
            if (f.status == 200) f.stream = file.createInputStream();
            return f;
        }
        juce::String headers = "User-Agent: M1D1/" + Updater::currentVersion() + "\r\n";
        if (api) headers << "Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        f.stream = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                              .withExtraHeaders (headers)
                                              .withConnectionTimeoutMs (timeoutMs)
                                              .withNumRedirectsToFollow (8)
                                              .withStatusCode (&f.status));
        if (f.stream == nullptr && f.status == 200) f.status = 0;
        return f;
    }

    juce::String plainNotes (juce::String body)
    {
        body = body.replace ("\r\n", "\n");
        // the release text: what is new first, then a line of dashes, then the install instructions
        const int cut = body.indexOf ("\n---");
        if (cut >= 0) body = body.substring (0, cut);
        else if (body.startsWith ("---")) body = {};
        juce::StringArray lines;
        lines.addLines (body);
        for (auto& l : lines)
        {
            l = l.trimEnd();
            while (l.startsWithChar ('#')) l = l.substring (1);
            l = l.replace ("**", "").replace ("`", "").trimStart();
        }
        lines.removeEmptyStrings (false);
        auto text = lines.joinIntoString ("\n").trim();
        if (text.length() > 600) text = text.substring (0, 597).trimEnd() + "...";
        return text;
    }

    [[maybe_unused]] juce::String quoted (const juce::String& s) { return "'" + s.replace ("'", "''") + "'"; }   // a PowerShell string
}

int compareVersions (const juce::String& a, const juce::String& b)
{
    auto split = [] (juce::String v, juce::StringArray& nums, juce::StringArray& pre)
    {
        v = v.trim();
        if (v.startsWithIgnoreCase ("v")) v = v.substring (1);
        v = v.upToFirstOccurrenceOf ("+", false, false);
        const auto release = v.upToFirstOccurrenceOf ("-", false, false);
        nums.addTokens (release, ".", {});
        if (v.containsChar ('-')) pre.addTokens (v.fromFirstOccurrenceOf ("-", false, false), ".", {});
    };
    juce::StringArray na, nb, pa, pb;
    split (a, na, pa);
    split (b, nb, pb);
    for (int i = 0; i < juce::jmax (na.size(), nb.size()); ++i)
    {
        const auto x = na[i].getLargeIntValue(), y = nb[i].getLargeIntValue();
        if (x != y) return x < y ? -1 : 1;
    }
    if (pa.isEmpty() != pb.isEmpty()) return pa.isEmpty() ? 1 : -1;   // a pre-release comes before its release
    for (int i = 0; i < juce::jmax (pa.size(), pb.size()); ++i)
    {
        if (i >= pa.size()) return -1;
        if (i >= pb.size()) return 1;
        const bool xn = pa[i].containsOnly ("0123456789"), yn = pb[i].containsOnly ("0123456789");
        if (xn && yn)
        {
            const auto x = pa[i].getLargeIntValue(), y = pb[i].getLargeIntValue();
            if (x != y) return x < y ? -1 : 1;
        }
        else if (xn != yn) return xn ? -1 : 1;
        else if (const int c = pa[i].compare (pb[i]); c != 0) return c < 0 ? -1 : 1;
    }
    return 0;
}

std::optional<Release> parseRelease (const juce::String& json)
{
    const auto v = juce::JSON::parse (json);
    if (! v.isObject() || (bool) v["draft"] || (bool) v["prerelease"]) return std::nullopt;
    Release r;
    r.version = v["tag_name"].toString().trim();
    if (r.version.startsWithIgnoreCase ("v")) r.version = r.version.substring (1);
    if (r.version.isEmpty() || ! juce::CharacterFunctions::isDigit (r.version[0])) return std::nullopt;
    r.name = v["name"].toString().trim();
    if (r.name.isEmpty()) r.name = "M1D1 " + r.version;
    r.page = v["html_url"].toString();
    if (! allowedPage (r.page)) r.page = {};
    r.notes = plainNotes (v["body"].toString());
    if (const auto* assets = v["assets"].getArray())
        for (const auto& a : *assets)
            if (a["name"].toString() == "M1D1-Windows.zip")
            {
                r.zipUrl = a["browser_download_url"].toString();
                r.zipSize = (juce::int64) a["size"];
                const auto digest = a["digest"].toString().trim().toLowerCase();
                if (digest.startsWith ("sha256:") && digest.length() == 7 + 64 && digest.substring (7).containsOnly ("0123456789abcdef"))
                    r.sha256 = digest.substring (7);
            }
    return r;
}

juce::String checkPackage (const juce::File& unpacked, const juce::String& version)
{
    const auto root = unpacked.getChildFile ("M1D1");
    const std::pair<const char*, const char*> parts[] = {
        { "Install.bat", "its installer (Install.bat)" },
        { "M1D1.exe", "the M1D1 app" },
        { "M1D1.vst3/Contents/x86_64-win/M1D1.vst3", "the Windows plugin" },
        { "version.txt", "its version number" } };
    for (const auto& [path, what] : parts)
        if (! root.getChildFile (path).existsAsFile())
            return juce::String ("The download does not hold ") + what + ". Download it again later.";
    const auto inside = root.getChildFile ("version.txt").loadFileAsString().trim();
    if (inside != version)
        return "The download holds M1D1 " + inside + ", not " + version + ".";
    return {};
}

juce::String unpackZip (const juce::File& zip, const juce::File& into)
{
    juce::ZipFile z (zip);
    if (z.getNumEntries() == 0) return "The download is not a zip file. Download it again later.";
    for (int i = 0; i < z.getNumEntries(); ++i)
    {
        const auto name = z.getEntry (i)->filename.replaceCharacter ('\\', '/');
        juce::StringArray segments;
        segments.addTokens (name, "/", {});
        if (name.startsWithChar ('/') || name.containsChar (':') || segments.contains ("..")
            || ! into.getChildFile (name).isAChildOf (into))
            return "The download holds a file outside its folder (" + name + "). It was not installed.";
    }
    into.createDirectory();
    const auto result = z.uncompressTo (into, true);
    return result.wasOk() ? juce::String() : "The download could not be unpacked: " + result.getErrorMessage();
}

// ------------------------------------------------------------------------------------------------------------------

Updater::Updater() : juce::Thread ("M1D1 updates")
{
    status.current = currentVersion();
    loadSettings();
    startThread (juce::Thread::Priority::low);
}

Updater::~Updater()
{
    removeAllChangeListeners();
    signalThreadShouldExit();
    notify();
    stopThread (20000);
}

juce::String Updater::currentVersion()
{
   #if defined (M1D1_VERSION)
    return M1D1_VERSION;
   #elif defined (JucePlugin_VersionString)
    return JucePlugin_VersionString;
   #else
    return "0.0.0";
   #endif
}

const char* Updater::stateName (State s)
{
    switch (s)
    {
        case State::idle:        return "idle";
        case State::checking:    return "checking";
        case State::upToDate:    return "upToDate";
        case State::available:   return "available";
        case State::downloading: return "downloading";
        case State::ready:       return "ready";
        case State::failed:      return "failed";
    }
    return "idle";
}

juce::File Updater::settingsFolder()
{
    const auto custom = juce::SystemStats::getEnvironmentVariable ("M1D1_UPDATE_DIR", {});
    return custom.isNotEmpty() ? juce::File (custom)
                               : juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("M1D1");
}

void Updater::check (bool force)
{
    if (force) wantCheck = 2;
    else { int none = 0; wantCheck.compare_exchange_strong (none, 1); }
    notify();
}

void Updater::download()
{
    wantDownload = 1;
    notify();
}

void Updater::skip()
{
    {
        const juce::ScopedLock sl (lock);
        if (status.latest.isEmpty()) return;
        skipVersion = status.latest;
        status.skipped = true;
    }
    saveSettings();
    sendChangeMessage();
}

void Updater::setAutoCheck (bool on)
{
    {
        const juce::ScopedLock sl (lock);
        status.autoCheck = on;
    }
    saveSettings();
    sendChangeMessage();
}

Updater::Status Updater::getStatus() const
{
    const juce::ScopedLock sl (lock);
    return status;
}

void Updater::setState (State s, const juce::String& message)
{
    {
        const juce::ScopedLock sl (lock);
        status.state = s;
        status.message = message;
        if (s != State::downloading) status.progress = s == State::ready ? 1.0 : 0.0;
    }
    sendChangeMessage();
}

void Updater::run()
{
    while (! threadShouldExit())
    {
        if (const int c = wantCheck.exchange (0)) runCheck (c == 2);
        else if (wantDownload.exchange (0)) runDownload();
        else wait (-1);
    }
}

void Updater::evaluate (const Release& r)
{
    const bool newer = compareVersions (currentVersion(), r.version) < 0;
    const bool downloadable = r.zipUrl.isNotEmpty() && allowedDownload (r.zipUrl);
    {
        const juce::ScopedLock sl (lock);
        status.latest = r.version;
        status.name = r.name;
        status.notes = r.notes;
        status.page = r.page;
        status.size = r.zipSize;
        status.skipped = r.version == skipVersion;
    }
    if (newer && downloadable) setState (State::available);
    else if (newer) setState (State::upToDate, "M1D1 " + r.version + " is out, but it has no Windows download yet.");
    else setState (State::upToDate);
}

void Updater::runCheck (bool force)
{
    std::optional<Release> kept;
    juce::int64 last = 0;
    {
        const juce::ScopedLock sl (lock);
        if (status.state == State::downloading || status.state == State::ready || status.state == State::checking) return;
        if (! force && ! status.autoCheck) return;
        kept = release;
        last = lastCheck;
    }
    const auto now = juce::Time::currentTimeMillis();
    if (! force && last > 0 && now - last >= 0 && now - last < recheckMs)
    {
        if (kept) evaluate (*kept);
        else setState (State::upToDate);
        return;
    }

    setState (State::checking);
    const auto api = testApi().isNotEmpty() ? testApi() : juce::String (latestApi);
    auto f = fetchUrl (api, 10000, true);
    const auto text = f.stream != nullptr ? f.stream->readEntireStreamAsString() : juce::String();
    if (threadShouldExit()) return;

    if (f.status == 200 || f.status == 404)
    {
        // 404: no full release yet
        const auto r = f.status == 200 ? parseRelease (text) : std::nullopt;
        {
            const juce::ScopedLock sl (lock);
            lastCheck = now;
            release = r;
        }
        saveSettings();
        if (r) evaluate (*r);
        else setState (State::upToDate);
    }
    else if (f.status == 403 || f.status == 429)
        setState (State::failed, "GitHub is busy right now. M1D1 asks again later.");
    else if (f.status == 0)
        setState (State::failed, "No connection to GitHub. Check your internet and try again.");
    else
        setState (State::failed, "GitHub answered with an error (HTTP " + juce::String (f.status) + "). Try again later.");
}

void Updater::runDownload()
{
    std::optional<Release> r;
    {
        const juce::ScopedLock sl (lock);
        // available, or a download that failed and is tried again
        const bool canTry = status.state == State::available || (status.state == State::failed && status.folder.isNotEmpty());
        if (! canTry || ! release || release->zipUrl.isEmpty() || compareVersions (currentVersion(), release->version) >= 0) return;
        r = release;
    }
    if (! allowedDownload (r->zipUrl)) { setState (State::failed, "The download is not on M1D1's GitHub page. It was not installed."); return; }

    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("M1D1Update").getChildFile (r->version);
    folder.deleteRecursively();
    if (! folder.createDirectory()) { setState (State::failed, "M1D1 cannot write to " + folder.getFullPathName() + "."); return; }
    const auto zip = folder.getChildFile ("M1D1-Windows.zip");

    {
        const juce::ScopedLock sl (lock);
        status.progress = 0.0;
        status.folder = folder.getFullPathName();
    }
    setState (State::downloading);

    auto f = fetchUrl (r->zipUrl, 15000, false);
    if (f.stream == nullptr || f.status != 200)
    {
        setState (State::failed, f.status == 0 ? "Could not start the download. Check your internet and try again."
                                               : "The download answered with an error (HTTP " + juce::String (f.status) + "). Try again later.");
        return;
    }
    const auto total = r->zipSize > 0 ? r->zipSize : f.stream->getTotalLength();
    juce::int64 written = 0;
    {
        juce::FileOutputStream out (zip);
        if (! out.openedOk()) { setState (State::failed, "M1D1 cannot write the download to " + zip.getFullPathName() + "."); return; }
        juce::HeapBlock<char> buffer (65536);
        double shown = 0.0;
        for (;;)
        {
            if (threadShouldExit()) { out.flush(); zip.deleteFile(); return; }
            const int n = f.stream->read (buffer, 65536);
            if (n <= 0) break;
            if (! out.write (buffer, (size_t) n)) { setState (State::failed, "The disk is full or not writable. The update was not installed."); return; }
            written += n;
            if (written > maxDownload) { setState (State::failed, "The download is far bigger than M1D1. It was not installed."); return; }
            const double p = total > 0 ? juce::jlimit (0.0, 1.0, (double) written / (double) total) : 0.0;
            if (p - shown >= 0.01)
            {
                shown = p;
                { const juce::ScopedLock sl (lock); status.progress = p; }
                sendChangeMessage();
            }
        }
        out.flush();
    }

    if (r->zipSize > 0 && written != r->zipSize)
    {
        setState (State::failed, "The download stopped before the end (" + juce::String (written) + " of " + juce::String (r->zipSize) + " bytes). Try again.");
        return;
    }
    if (r->sha256.isNotEmpty() && juce::SHA256 (zip).toHexString().toLowerCase() != r->sha256)
    {
        setState (State::failed, "The download does not match its checksum on GitHub. It was not installed. Try again.");
        return;
    }
    const auto unpacked = folder.getChildFile ("unpacked");
    if (auto e = unpackZip (zip, unpacked); e.isNotEmpty()) { setState (State::failed, e); return; }
    if (auto e = checkPackage (unpacked, r->version); e.isNotEmpty()) { setState (State::failed, e); return; }

    const auto launched = launchInstaller (folder, r->version);
    if (launched.isNotEmpty()) setState (State::failed, launched);
    else setState (State::ready);
}

juce::String Updater::launchInstaller (const juce::File& folder, const juce::String& version)
{
   #if JUCE_WINDOWS
    if (juce::SystemStats::getEnvironmentVariable ("M1D1_UPDATE_NO_LAUNCH", {}) == "1") return {};
    // Waits until no program has M1D1 open, then runs the downloaded Install.bat (as administrator, which asks once).
    juce::String script = R"PS(# M1D1 update: installs the downloaded M1D1 once no program has it open.
$ErrorActionPreference = 'Continue'
try { $Host.UI.RawUI.WindowTitle = 'M1D1 update' } catch {}
$folder = @FOLDER@
$version = @VERSION@
$installer = Join-Path $folder 'unpacked\M1D1\Install.bat'
$plugin = Join-Path $env:CommonProgramFiles 'VST3\M1D1.vst3\Contents\x86_64-win\M1D1.vst3'
$test = $env:M1D1_UPDATE_API
function Busy {
  if (Get-Process -Name 'FL64', 'FL', 'FL Studio', 'M1D1' -ErrorAction SilentlyContinue) { return $true }
  if (Test-Path $plugin) {
    # a program that has the plugin open keeps others from opening its file exclusively
    try { $f = [IO.File]::Open($plugin, 'Open', 'Read', 'None'); $f.Close() } catch [IO.IOException] { return $true } catch {}
  }
  return $false
}
Write-Host ''
Write-Host "  M1D1 $version is downloaded and checked."
if (Busy) {
  Write-Host '  Close FL Studio (and the M1D1 app). The update installs as soon as they are closed.'
  Write-Host '  You can leave this window open.'
  while (Busy) { Start-Sleep -Seconds 2 }
}
$admin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
while ($true) {
  Write-Host '  Installing. Windows asks for permission...'
  $code = 1
  try {
    if ($admin) { & $installer elevated quiet; $code = $LASTEXITCODE }
    else { $p = Start-Process -FilePath $installer -ArgumentList 'elevated', 'quiet' -Verb RunAs -Wait -PassThru; $code = $p.ExitCode }
  } catch { Write-Host '  Windows did not give permission.' }
  if ($code -eq 0) {
    Write-Host "  M1D1 $version is installed. Open your project again to use it."
    Set-Location $env:TEMP
    Remove-Item -Recurse -Force $folder -ErrorAction SilentlyContinue
    if (-not $test) { Start-Sleep -Seconds 6 }
    exit 0
  }
  if ($test) { exit 1 }
  Write-Host '  The update did not install. Close every program that uses M1D1,'
  Write-Host '  then press Enter to try again, or close this window to stop.'
  [void](Read-Host)
}
)PS";
    script = script.replace ("@FOLDER@", quoted (folder.getFullPathName())).replace ("@VERSION@", quoted (version));
    const auto file = folder.getChildFile ("finish-update.ps1");
    if (! file.replaceWithText (script, false, false, "\r\n")) return "M1D1 cannot write the update script to " + folder.getFullPathName() + ".";
    const auto ps = juce::File::getSpecialLocation (juce::File::windowsSystemDirectory).getChildFile ("WindowsPowerShell\\v1.0\\powershell.exe");
    if (! ps.startAsProcess ("-NoProfile -ExecutionPolicy Bypass -File \"" + file.getFullPathName() + "\""))
        return "Windows did not start the update. Open " + folder.getFullPathName() + "\\unpacked\\M1D1 and run Install.bat.";
    return {};
   #else
    juce::ignoreUnused (folder, version);   // not on Windows: the package stays in the folder
    return {};
   #endif
}

void Updater::loadSettings()
{
    const auto v = juce::JSON::parse (settingsFolder().getChildFile ("updates.json"));
    if (! v.isObject()) return;
    const juce::ScopedLock sl (lock);
    lastCheck = (juce::int64) v["lastCheck"];
    skipVersion = v["skip"].toString();
    status.autoCheck = v.hasProperty ("auto") ? (bool) v["auto"] : true;
    if (const auto r = v["release"]; r.isObject() && r["version"].toString().isNotEmpty())
    {
        Release k;
        k.version = r["version"].toString(); k.name = r["name"].toString(); k.notes = r["notes"].toString();
        k.page = r["page"].toString(); k.zipUrl = r["zipUrl"].toString(); k.sha256 = r["sha256"].toString();
        k.zipSize = (juce::int64) r["zipSize"];
        if (! allowedPage (k.page)) k.page = {};
        release = k;
    }
}

void Updater::saveSettings() const
{
    auto* o = new juce::DynamicObject();
    {
        const juce::ScopedLock sl (lock);
        o->setProperty ("lastCheck", lastCheck);
        o->setProperty ("skip", skipVersion);
        o->setProperty ("auto", status.autoCheck);
        if (release)
        {
            auto* r = new juce::DynamicObject();
            r->setProperty ("version", release->version); r->setProperty ("name", release->name);
            r->setProperty ("notes", release->notes); r->setProperty ("page", release->page);
            r->setProperty ("zipUrl", release->zipUrl); r->setProperty ("sha256", release->sha256);
            r->setProperty ("zipSize", release->zipSize);
            o->setProperty ("release", juce::var (r));
        }
    }
    const auto folder = settingsFolder();
    folder.createDirectory();
    folder.getChildFile ("updates.json").replaceWithText (juce::JSON::toString (juce::var (o)));
}
} // namespace m1d1
