// M1D1's updater from the command line, for the build's end-to-end test and for checking a release by hand.
//   m1d1_update_tool check     asks GitHub (or M1D1_UPDATE_API) for the latest release and prints what M1D1 would do
//   m1d1_update_tool update    the same, then downloads, checks and unpacks the new version and, on Windows, starts
//                              the "M1D1 update" window that installs it (as M1D1 does when you press Update)
// Exit code: 0 done (update: the installer was started), 1 no update to install, 2 failed.
#include <juce_events/juce_events.h>
#include "../plugin/Updater.h"

using State = m1d1::Updater::State;

static m1d1::Updater::Status waitWhile (m1d1::Updater& u, std::initializer_list<State> busy, int seconds)
{
    const auto until = juce::Time::getMillisecondCounter() + (juce::uint32) seconds * 1000;
    auto s = u.getStatus();
    juce::String shown;
    while (std::find (busy.begin(), busy.end(), s.state) != busy.end() && juce::Time::getMillisecondCounter() < until)
    {
        juce::Thread::sleep (100);
        s = u.getStatus();
        const auto line = juce::String (m1d1::Updater::stateName (s.state)) + (s.state == State::downloading ? " " + juce::String (juce::roundToInt (s.progress * 100)) + "%" : juce::String());
        if (line != shown && (s.state != State::downloading || juce::roundToInt (s.progress * 100) % 10 == 0)) { std::cout << "  " << line << std::endl; shown = line; }
    }
    return s;
}

static void print (const m1d1::Updater::Status& s)
{
    std::cout << "state=" << m1d1::Updater::stateName (s.state) << "\ncurrent=" << s.current << "\nlatest=" << s.latest
              << "\nname=" << s.name << "\nsize=" << s.size << "\nskipped=" << (s.skipped ? "yes" : "no")
              << "\nnotes=" << s.notes.replace ("\n", " / ") << "\nmessage=" << s.message << "\nfolder=" << s.folder << std::endl;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::String command = argc > 1 ? argv[1] : "check";
    m1d1::Updater updater;
    // after the constructor: the first state may be "idle" until the worker takes the request
    updater.check (true);
    auto s = waitWhile (updater, { State::idle, State::checking }, 60);
    print (s);
    if (s.state == State::failed) return 2;
    if (command != "update") return 0;
    if (s.state != State::available) { std::cout << "no update to install" << std::endl; return 1; }

    updater.download();
    s = waitWhile (updater, { State::available, State::downloading }, 600);
    print (s);
    return s.state == State::ready ? 0 : 2;
}
