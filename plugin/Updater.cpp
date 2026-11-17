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

    // a version as it may appear in a folder name and in the update script: digits, letters, dots, + and -
    bool plainVersion (const juce::String& v)
    {
        return v.isNotEmpty() && v.length() <= 40 && juce::CharacterFunctions::isDigit (v[0])
            && v.containsOnly ("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.+-");
    }

    // The "M1D1 update" window keeps waiting.lock open with no sharing: on Windows the file cannot be deleted then.
    // A lock nobody holds is deleted here (it is left from a window that ended).
    bool heldOpen (const juce::File& lockFile) { return lockFile.existsAsFile() && ! lockFile.deleteFile(); }

    juce::var releaseVar (const Release& r)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("version", r.version); o->setProperty ("name", r.name); o->setProperty ("notes", r.notes);
        o->setProperty ("page", r.page); o->setProperty ("zipUrl", r.zipUrl); o->setProperty ("sha256", r.sha256);
        o->setProperty ("zipSize", r.zipSize);
        return juce::var (o);
    }
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
    if (! plainVersion (r.version)) return std::nullopt;
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

namespace
{
    constexpr juce::int64 launchGraceMs = 30 * 1000;            // the window takes a moment to start and take its lock
    constexpr juce::int64 staleMs = 6 * 60 * 60 * 1000;         // leftover download folders are removed after 6 hours
}

// Drops a due (not forced) check that came in while a download ran, so the download's result stays on screen.
struct DropDueCheck
{
    Updater& u;
    ~DropDueCheck() { int due = 1; u.wantCheck.compare_exchange_strong (due, 0); }
};

Updater::Updater() : juce::Thread ("M1D1 updates")
{
    status.current = currentVersion();
    loadSettings (true);
    startThread (juce::Thread::Priority::low);
}

Updater::~Updater()
{
    removeAllChangeListeners();
    signalThreadShouldExit();
    {
        // a connection that hangs is closed now, so the thread ends at once instead of holding the DAW
        const juce::ScopedLock sl (streamLock);
        if (active != nullptr) active->cancel();
    }
    notify();
    stopThread (10000);
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
    if (custom.isNotEmpty()) return juce::File (custom);
    // answers from a test release are never kept where the real M1D1 reads them
    if (testApi().isNotEmpty()) return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("M1D1UpdateTest");
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("M1D1");
}

juce::File Updater::downloadsFolder()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("M1D1Update");
}

juce::File Updater::findPending (const juce::String& version)
{
    if (! plainVersion (version)) return {};
    for (const auto& d : downloadsFolder().findChildFiles (juce::File::findDirectories, false, version + "-*"))
        if (d.getFileName().fromFirstOccurrenceOf (version + "-", false, false).containsOnly ("0123456789")
            && heldOpen (d.getChildFile ("waiting.lock")))
            return d;
    return {};
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
    juce::String v;
    {
        const juce::ScopedLock sl (lock);
        if (status.latest.isEmpty()) return;
        v = skipVersion = status.latest;
        status.skipped = true;
    }
    writeSettings ([&] (juce::DynamicObject& o) { o.setProperty ("skip", v); });
    sendChangeMessage();
}

void Updater::setAutoCheck (bool on)
{
    {
        const juce::ScopedLock sl (lock);
        status.autoCheck = on;
    }
    writeSettings ([&] (juce::DynamicObject& o) { o.setProperty ("auto", on); });
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
        if (s != State::failed) status.downloadFailed = false;
        if (s != State::ready) { status.launched = false; status.installed = false; }
    }
    sendChangeMessage();
}

void Updater::setReady (const juce::File& folder, bool launched)
{
    {
        const juce::ScopedLock sl (lock);
        status.state = State::ready;
        status.message = {};
        status.progress = 1.0;
        status.downloadFailed = false;
        status.installed = false;
        status.launched = launched;
        status.folder = folder.getFullPathName();
    }
    sendChangeMessage();
}

void Updater::failDownload (const juce::String& message, const juce::File& folder)
{
    if (folder != juce::File() && ! heldOpen (folder.getChildFile ("waiting.lock"))) folder.deleteRecursively();
    {
        const juce::ScopedLock sl (lock);
        status.state = State::failed;
        status.message = message;
        status.progress = 0.0;
        status.downloadFailed = true;
        status.launched = status.installed = false;
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

juce::InputStream* Updater::openStream (const juce::String& address, int timeoutMs, bool api, int& httpStatus)
{
    httpStatus = 0;
    const juce::URL url (address);
    if (url.isLocalFile())
    {
        const auto file = url.getLocalFile();
        httpStatus = file.existsAsFile() ? 200 : 404;
        if (httpStatus != 200) return nullptr;
        const juce::ScopedLock sl (streamLock);
        current = file.createInputStream();
        return current.get();
    }
    juce::String headers = "User-Agent: M1D1/" + currentVersion() + "\r\n";
    if (api) headers << "Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    auto web = std::make_unique<juce::WebInputStream> (url, false);
    web->withExtraHeaders (headers).withConnectionTimeout (timeoutMs).withNumRedirectsToFollow (8);
    auto* w = web.get();
    {
        const juce::ScopedLock sl (streamLock);
        if (threadShouldExit()) return nullptr;
        current = std::move (web);
        active = w;
    }
    const bool connected = w->connect (nullptr);
    httpStatus = connected ? w->getStatusCode() : 0;
    if (! connected || threadShouldExit()) { streamDone(); return nullptr; }
    return w;
}

void Updater::streamDone()
{
    const juce::ScopedLock sl (streamLock);
    active = nullptr;
    current.reset();
}

void Updater::evaluate (const Release& r, const juce::String& message)
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
    if (newer && downloadable)
    {
        // an "M1D1 update" window for this version is still waiting (from an M1D1 that was closed since): ready
        if (const auto pending = findPending (r.version); pending != juce::File())
        {
            setReady (pending, true);
            return;
        }
        setState (State::available, message);
    }
    else if (newer) setState (State::upToDate, "M1D1 " + r.version + " is out, but it has no Windows download yet.");
    else setState (State::upToDate);
}

void Updater::revalidateReady()
{
    juce::String version, folder;
    {
        const juce::ScopedLock sl (lock);
        version = status.latest;
        folder = status.folder;
    }
    if (juce::Time::currentTimeMillis() - launchedAt < launchGraceMs) return;   // the window is still starting
    if (folder.isNotEmpty() && heldOpen (juce::File (folder).getChildFile ("waiting.lock"))) return;   // still waiting
   #if JUCE_WINDOWS
    const auto installed = juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory)
                               .getChildFile ("M1D1").getChildFile ("version.txt").loadFileAsString().trim();
   #else
    const juce::String installed;
   #endif
    if (version.isNotEmpty() && installed == version)
    {
        {
            const juce::ScopedLock sl (lock);
            status.installed = true;
        }
        sendChangeMessage();
        return;
    }
    std::optional<Release> r;
    {
        const juce::ScopedLock sl (lock);
        r = release;
        status.folder = {};
    }
    if (r) evaluate (*r, "The \"M1D1 update\" window closed before M1D1 " + version + " was installed. Click Update now to try again.");
}

void Updater::runCheck (bool force)
{
    loadSettings (false);   // the user's choices, as another M1D1 may have changed them
    State st;
    std::optional<Release> kept;
    juce::int64 last = 0;
    bool autoOn = true, downloadFailed = false, launched = false;
    {
        const juce::ScopedLock sl (lock);
        st = status.state; kept = release; last = lastCheck; autoOn = status.autoCheck;
        downloadFailed = status.downloadFailed; launched = status.launched;
    }
    if (st == State::downloading || st == State::checking) return;
    if (st == State::ready) { if (launched) revalidateReady(); return; }
    if (! force && ! autoOn) return;
    if (! force && st == State::failed && downloadFailed) return;   // keep the download's failure until the user acts

    const auto now = juce::Time::currentTimeMillis();
    if (! force && last > 0 && now - last >= 0 && now - last < recheckMs)
    {
        if (kept) evaluate (*kept);
        else setState (State::upToDate);
        return;
    }

    setState (State::checking);
    const auto api = testApi().isNotEmpty() ? testApi() : juce::String (latestApi);
    int code = 0;
    juce::String text;
    if (auto* in = openStream (api, 10000, true, code))
        text = in->readEntireStreamAsString();
    streamDone();
    if (threadShouldExit()) return;

    if (code == 200 || code == 404)
    {
        // 404: no full release yet
        const auto r = code == 200 ? parseRelease (text) : std::nullopt;
        {
            const juce::ScopedLock sl (lock);
            lastCheck = now;
            release = r;
        }
        writeSettings ([&] (juce::DynamicObject& o)
        {
            o.setProperty ("lastCheck", now);
            o.setProperty ("release", r ? releaseVar (*r) : juce::var());
        });
        if (r) evaluate (*r);
        else setState (State::upToDate);
    }
    else if (code == 403 || code == 429)
        setState (State::failed, "GitHub is busy right now. M1D1 asks again later.");
    else if (code == 0)
        setState (State::failed, "No connection to GitHub. Check your internet and try again.");
    else
        setState (State::failed, "GitHub answered with an error (HTTP " + juce::String (code) + "). Try again later.");
}

void Updater::runDownload()
{
    const DropDueCheck dropDue { *this };
    std::optional<Release> r;
    {
        const juce::ScopedLock sl (lock);
        // available, or a download that failed and is tried again
        const bool canTry = status.state == State::available || (status.state == State::failed && status.downloadFailed);
        if (canTry && release && release->zipUrl.isNotEmpty() && compareVersions (currentVersion(), release->version) < 0)
            r = release;
    }
    if (! r) { sendChangeMessage(); return; }   // nothing to download: the page still gets an answer
    if (! allowedDownload (r->zipUrl)) { failDownload ("The download is not on M1D1's GitHub page. It was not installed.", {}); return; }

    // an "M1D1 update" window for this version still waits: never download it again over its files
    if (const auto pending = findPending (r->version); pending != juce::File())
    {
        setReady (pending, true);
        return;
    }
    // folders left from earlier downloads whose window ended
    for (const auto& d : downloadsFolder().findChildFiles (juce::File::findDirectories, false))
        if (! heldOpen (d.getChildFile ("waiting.lock"))
            && juce::Time::currentTimeMillis() - d.getLastModificationTime().toMilliseconds() > staleMs)
            d.deleteRecursively();

    auto stamp = juce::Time::currentTimeMillis();
    while (downloadsFolder().getChildFile (r->version + "-" + juce::String (stamp)).exists()) ++stamp;   // a folder per download
    const auto folder = downloadsFolder().getChildFile (r->version + "-" + juce::String (stamp));
    {
        const juce::ScopedLock sl (lock);
        status.progress = 0.0;
        status.folder = folder.getFullPathName();
    }
    if (! folder.createDirectory()) { failDownload ("M1D1 cannot write to " + folder.getFullPathName() + ".", {}); return; }
    const auto zip = folder.getChildFile ("M1D1-Windows.zip");
    setState (State::downloading);

    int code = 0;
    auto* in = openStream (r->zipUrl, 15000, false, code);
    if (in == nullptr || code != 200)
    {
        streamDone();
        if (threadShouldExit()) return;
        failDownload (code == 0 ? "Could not start the download. Check your internet and try again."
                                : "The download answered with an error (HTTP " + juce::String (code) + "). Try again later.", folder);
        return;
    }
    const auto total = r->zipSize > 0 ? r->zipSize : in->getTotalLength();
    juce::int64 written = 0;
    juce::String error;
    {
        juce::FileOutputStream out (zip);
        if (! out.openedOk()) error = "M1D1 cannot write the download to " + zip.getFullPathName() + ".";
        juce::HeapBlock<char> buffer (65536);
        double shown = 0.0;
        while (error.isEmpty())
        {
            if (threadShouldExit()) break;
            const int n = in->read (buffer, 65536);
            if (n <= 0) break;
            if (! out.write (buffer, (size_t) n)) { error = "The disk is full or not writable. The update was not installed."; break; }
            written += n;
            if (written > maxDownload) { error = "The download is far bigger than M1D1. It was not installed."; break; }
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
    streamDone();
    if (threadShouldExit()) { folder.deleteRecursively(); return; }
    if (error.isNotEmpty()) { failDownload (error, folder); return; }

    if (r->zipSize > 0 && written != r->zipSize)
    {
        failDownload ("The download stopped before the end (" + juce::String (written) + " of " + juce::String (r->zipSize) + " bytes). Try again.", folder);
        return;
    }
    if (r->sha256.isNotEmpty() && juce::SHA256 (zip).toHexString().toLowerCase() != r->sha256)
    {
        failDownload ("The download does not match its checksum on GitHub. It was not installed. Try again.", folder);
        return;
    }
    const auto unpacked = folder.getChildFile ("unpacked");
    if (auto e = unpackZip (zip, unpacked); e.isNotEmpty()) { failDownload (e, folder); return; }
    if (auto e = checkPackage (unpacked, r->version); e.isNotEmpty()) { failDownload (e, folder); return; }

    bool launched = false;
    if (auto e = launchInstaller (folder, r->version); e.isNotEmpty()) { failDownload (e, {}); return; }
   #if JUCE_WINDOWS
    launched = juce::SystemStats::getEnvironmentVariable ("M1D1_UPDATE_NO_LAUNCH", {}) != "1";
   #endif
    if (launched) launchedAt = juce::Time::currentTimeMillis();
    setReady (folder, launched);
}

juce::String Updater::launchInstaller (const juce::File& folder, const juce::String& version)
{
   #if JUCE_WINDOWS
    if (juce::SystemStats::getEnvironmentVariable ("M1D1_UPDATE_NO_LAUNCH", {}) == "1") return {};
    // Waits until no program has M1D1 open, then runs the downloaded Install.bat (as administrator, which asks once).
    juce::String script = R"PS(# M1D1 update: installs the downloaded M1D1 once no program has it open.
$ErrorActionPreference = 'Continue'
try { $Host.UI.RawUI.WindowTitle = 'M1D1 update' } catch {}
$folder = $PSScriptRoot
$version = '@VERSION@'
$test = $env:M1D1_UPDATE_API
if ($test) { try { Start-Transcript -Path (Join-Path ([IO.Path]::GetTempPath()) 'M1D1-update-test.log') -Force | Out-Null } catch {} }
# held open while this window lives: M1D1 sees that this update is waiting and never downloads over it
try { $lock = [IO.File]::Open((Join-Path $folder 'waiting.lock'), 'OpenOrCreate', 'ReadWrite', 'None') }
catch { Write-Host '  Another M1D1 update window is already waiting. This one closes.'; exit 0 }
$installer = Join-Path $folder 'unpacked\M1D1\Install.bat'
$plugin = Join-Path $env:CommonProgramFiles 'VST3\M1D1.vst3\Contents\x86_64-win\M1D1.vst3'
$app = Join-Path $env:ProgramFiles 'M1D1\version.txt'
function Busy {
  if (Get-Process -Name 'FL64', 'FL', 'FL Studio', 'M1D1' -ErrorAction SilentlyContinue) { return $true }
  if (Test-Path $plugin) {
    try { $f = [IO.File]::Open($plugin, 'Open', 'Read', 'None'); $f.Close() } catch [IO.IOException] { return $true } catch {}
    # a DAW that has loaded M1D1 (Ableton Live, Reaper, Bitwig, Cubase, Studio One...)
    foreach ($p in Get-Process) { try { foreach ($m in $p.Modules) { if ($m.FileName -ieq $plugin) { return $true } } } catch {} }
  }
  return $false
}
function Done($code) {
  Write-Host "M1D1-UPDATE-EXIT $code"
  if ($test) { try { Stop-Transcript | Out-Null } catch {} }
  $lock.Close()
  if ($code -eq 0) { Set-Location ([IO.Path]::GetTempPath()); Remove-Item -Recurse -Force $folder -ErrorAction SilentlyContinue }
  exit $code
}
Write-Host ''
Write-Host "  M1D1 $version is downloaded and checked."
if (Busy) {
  Write-Host '  Close FL Studio, the M1D1 app and any other program that has M1D1 open.'
  Write-Host '  The update installs as soon as they are closed. You can leave this window open.'
  while (Busy) { Start-Sleep -Seconds 3 }
}
# one install at a time, even with several M1D1 update windows
$mutex = New-Object Threading.Mutex($false, 'Local\M1D1Update')
try { [void]$mutex.WaitOne() } catch [Threading.AbandonedMutexException] {}
if ((Test-Path $app) -and ((Get-Content $app -Raw).Trim() -eq $version)) {
  Write-Host "  M1D1 $version is already installed."
  Done 0
}
$admin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
while ($true) {
  if (-not (Test-Path $installer)) { Write-Host '  The downloaded update is gone. Open M1D1 and update again.'; Done 1 }
  Write-Host '  Installing. Windows asks for permission...'
  $code = 1
  try {
    if ($admin) { & $installer elevated quiet; $code = $LASTEXITCODE }
    else { $p = Start-Process -FilePath $installer -ArgumentList 'elevated', 'quiet' -Verb RunAs -Wait -PassThru; $code = $p.ExitCode }
  } catch { Write-Host "  The installer did not start: $($_.Exception.Message)" }
  if ($code -eq 0) {
    Write-Host "  M1D1 $version is installed. Open your project again (or restart the M1D1 app) to use it."
    if (-not $test) { Start-Sleep -Seconds 6 }
    Done 0
  }
  if ($test) { Done $code }
  Write-Host '  The update did not install. Close every program that has M1D1 open,'
  Write-Host '  then press Enter to try again, or close this window to stop.'
  [void](Read-Host)
}
)PS";
    if (! plainVersion (version)) return "The new version's number is not valid (" + version + "). It was not installed.";
    script = script.replace ("@VERSION@", version);
    // UTF-16 with a byte order mark: Windows PowerShell 5.1 reads a file without one in the old ANSI code page
    const auto file = folder.getChildFile ("finish-update.ps1");
    if (! file.replaceWithText (script, true, true, "\r\n")) return "M1D1 cannot write the update script to " + folder.getFullPathName() + ".";
    const auto ps = juce::File::getSpecialLocation (juce::File::windowsSystemDirectory).getChildFile ("WindowsPowerShell\\v1.0\\powershell.exe");
    if (! ps.startAsProcess ("-NoProfile -ExecutionPolicy Bypass -File \"" + file.getFullPathName() + "\""))
        return "Windows did not start the update. Open " + folder.getFullPathName() + "\\unpacked\\M1D1 and run Install.bat.";
    return {};
   #else
    juce::ignoreUnused (folder, version);   // not on Windows: the package stays in the folder
    return {};
   #endif
}

void Updater::loadSettings (bool all)
{
    juce::var v;
    {
        const juce::ScopedLock sl (settingsLock);
        v = juce::JSON::parse (settingsFolder().getChildFile ("updates.json"));
    }
    if (! v.isObject()) return;
    const juce::ScopedLock sl (lock);
    skipVersion = v["skip"].toString();
    status.autoCheck = v.hasProperty ("auto") ? (bool) v["auto"] : true;
    if (! all) return;
    lastCheck = (juce::int64) v["lastCheck"];
    if (const auto r = v["release"]; r.isObject() && plainVersion (r["version"].toString()))
    {
        Release k;
        k.version = r["version"].toString(); k.name = r["name"].toString(); k.notes = r["notes"].toString();
        k.page = r["page"].toString(); k.zipUrl = r["zipUrl"].toString(); k.sha256 = r["sha256"].toString();
        k.zipSize = (juce::int64) r["zipSize"];
        if (! allowedPage (k.page)) k.page = {};
        release = k;
    }
}

// Changes only the given values, on top of what the file holds now: several M1D1s (the plugin in a DAW, the app)
// share the file, and one must not undo another's choice.
void Updater::writeSettings (const std::function<void (juce::DynamicObject&)>& change) const
{
    const juce::ScopedLock sl (settingsLock);
    juce::InterProcessLock processes ("M1D1-update-settings");
    const bool locked = processes.enter (3000);
    const auto file = settingsFolder().getChildFile ("updates.json");
    auto v = juce::JSON::parse (file);
    if (! v.isObject()) v = juce::var (new juce::DynamicObject());
    change (*v.getDynamicObject());
    file.getParentDirectory().createDirectory();
    file.replaceWithText (juce::JSON::toString (v));
    if (locked) processes.exit();
}
} // namespace m1d1
