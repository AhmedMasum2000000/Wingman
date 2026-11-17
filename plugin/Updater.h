#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <optional>

// Keeps M1D1 up to date from its GitHub releases. One updater is shared by every M1D1 window (plugin instances and
// the standalone app). It asks GitHub for the latest full release (at most every 12 hours unless asked to now),
// downloads M1D1-Windows.zip, checks its size, its SHA-256 and what it holds, and on Windows starts a small
// "M1D1 update" window that waits until no program has M1D1 open and then runs the package's own Install.bat.
// Nothing about the user is sent: only a request for the latest release, with "M1D1/<version>" as the user agent.
namespace m1d1
{
// < 0 when a is older than b. "v0.4.10" > "0.4.9"; a pre-release ("0.5.0-beta.1") is older than its release.
int compareVersions (const juce::String& a, const juce::String& b);

struct Release
{
    juce::String version;   // without the leading v
    juce::String name, notes, page, zipUrl, sha256;   // sha256: lower-case hex, empty when GitHub gave none
    juce::int64 zipSize = 0;
};

// GitHub's answer for the latest release, or nothing when it is a draft, a pre-release or carries no version.
// A release without an M1D1-Windows.zip has an empty zipUrl.
std::optional<Release> parseRelease (const juce::String& json);

// What is missing from an unpacked package for it to be installed as `version` (empty: nothing).
juce::String checkPackage (const juce::File& unpacked, const juce::String& version);

// Unpacks a zip into a folder, refusing any entry that would land outside it (empty: done).
juce::String unpackZip (const juce::File& zip, const juce::File& into);

class Updater : public juce::ChangeBroadcaster, private juce::Thread
{
public:
    enum class State { idle, checking, upToDate, available, downloading, ready, failed };
    struct Status
    {
        State state = State::idle;
        juce::String current, latest, name, notes, page, message, folder;
        juce::int64 size = 0;
        double progress = 0.0;       // 0..1 while downloading
        bool autoCheck = true;       // check when M1D1 opens
        bool skipped = false;        // the available version is one the user chose to skip
    };

    Updater();
    ~Updater() override;

    static juce::String currentVersion();
    static const char* stateName (State);

    // force: ask GitHub now. Otherwise, only when checks are on and the last answer is more than 12 hours old
    // (until then the kept answer is used).
    void check (bool force);
    // Download, check and unpack the available version; on Windows, then install it once M1D1 is closed.
    void download();
    void skip();                 // no more notices for the available version
    void setAutoCheck (bool);
    Status getStatus() const;

    // for the tests: where the updater keeps its settings (updates.json) and its downloads
    static juce::File settingsFolder();

private:
    void run() override;
    void runCheck (bool force);
    void runDownload();
    void evaluate (const Release&);                    // available / up to date, from a release
    void setState (State, const juce::String& message = {});
    void loadSettings();
    void saveSettings() const;
    juce::String launchInstaller (const juce::File& folder, const juce::String& version);

    mutable juce::CriticalSection lock;
    Status status;
    std::optional<Release> release;      // the latest full release, as last heard
    juce::int64 lastCheck = 0;           // when GitHub last answered (ms since 1970)
    juce::String skipVersion;
    std::atomic<int> wantCheck { 0 }, wantDownload { 0 };   // wantCheck: 1 when due, 2 forced
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Updater)
};
} // namespace m1d1
