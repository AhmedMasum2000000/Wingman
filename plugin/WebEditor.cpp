#include "WebEditor.h"
#include "M1D1Web.h"

#include <cstdlib>

namespace
{
    using Options = juce::WebBrowserComponent::Options;
    using Completion = juce::WebBrowserComponent::NativeFunctionCompletion;

    juce::MemoryBlock fromBase64 (const juce::String& text)
    {
        juce::MemoryOutputStream out;
        juce::Base64::convertFromBase64 (out, text);
        return out.getMemoryBlock();
    }

    juce::File m1d1Folder (juce::File::SpecialLocationType where)
    {
        auto folder = juce::File::getSpecialLocation (where).getChildFile ("M1D1");
        folder.createDirectory();
        return folder;
    }
}

WebEditor::WebEditor (BacklineProcessor& p)
    : juce::AudioProcessorEditor (p), processor (p), browser (makeOptions())
{
    addAndMakeVisible (browser);
    browser.goToURL (juce::WebBrowserComponent::getResourceProviderRoot());

    // always 4:3, like the design
    setResizable (true, true);
    setResizeLimits (800, 600, 1800, 1350);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio (4.0 / 3.0);
    setSize (1040, 780);
    startTimerHz (30);
}

WebEditor::~WebEditor() { stopTimer(); }

void WebEditor::paint (juce::Graphics& g) { g.fillAll (juce::Colour (0xff0b0b0b)); }

void WebEditor::resized() { browser.setBounds (getLocalBounds()); }

// where the music is: the host's position while it plays, or the editor's own preview clock
void WebEditor::timerCallback()
{
    auto& e = processor.getWebEngine();
    auto* o = new juce::DynamicObject();
    o->setProperty ("host", e.reportHostPlaying.load());
    o->setProperty ("preview", e.reportPreview.load());
    o->setProperty ("beat", e.reportBeat.load());
    o->setProperty ("bpm", e.reportBpm.load());
    browser.emitEventIfBrowserIsVisible ("transport", juce::var (o));
}

std::optional<juce::WebBrowserComponent::Resource> WebEditor::getResource (const juce::String& url)
{
    const auto path = url.upToFirstOccurrenceOf ("?", false, false);
    if (path != "/" && path != "/index.html")
        return std::nullopt;
    juce::WebBrowserComponent::Resource r;
    const auto* bytes = reinterpret_cast<const std::byte*> (M1D1Web::m1d1_html);
    r.data.assign (bytes, bytes + M1D1Web::m1d1_htmlSize);
    r.mimeType = "text/html; charset=utf-8";
    return r;
}

Options WebEditor::makeOptions()
{
   #if JUCE_WINDOWS
    // The page keeps its clock on an audio context; let it run without a click first.
    if (std::getenv ("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS") == nullptr)
        _putenv_s ("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", "--autoplay-policy=no-user-gesture-required");
   #endif

    auto options = Options {}
        .withNativeIntegrationEnabled()
        .withKeepPageLoadedWhenBrowserIsHidden()
        .withResourceProvider ([this] (const juce::String& url) { return getResource (url); })

        // the notes to play and the preview clock
        .withNativeFunction ("setSong", [this] (const juce::Array<juce::var>& a, Completion done)
        {
            processor.setWebSong (a.isEmpty() ? juce::String() : a[0].toString());
            done ({});
        })
        .withNativeFunction ("setPreview", [this] (const juce::Array<juce::var>& a, Completion done)
        {
            if (a.size() >= 3)
                processor.getWebEngine().setPreview ((bool) a[0], (double) a[1], (double) a[2]);
            done ({});
        })

        // the session, saved in the host's project
        .withNativeFunction ("saveState", [this] (const juce::Array<juce::var>& a, Completion done)
        {
            if (! a.isEmpty()) processor.setWebState (a[0].toString());
            done ({});
        })
        .withNativeFunction ("getState", [this] (const juce::Array<juce::var>&, Completion done)
        {
            done (processor.getWebState());
        })

        // Save: a file dialog
        .withNativeFunction ("saveMidi", [this] (const juce::Array<juce::var>& a, Completion done)
        {
            if (a.size() < 2) { done ("declined"); return; }
            const auto data = fromBase64 (a[1].toString());
            const auto name = juce::File::createLegalFileName ("M1D1 " + a[0].toString());
            chooser = std::make_unique<juce::FileChooser> ("Save the MIDI file", m1d1Folder (juce::File::userMusicDirectory).getChildFile (name), "*.mid");
            chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                      | juce::FileBrowserComponent::warnAboutOverwriting,
                                  [data, done] (const juce::FileChooser& fc)
                                  {
                                      auto file = fc.getResult();
                                      if (file == juce::File()) { done ("declined"); return; }
                                      file = file.withFileExtension ("mid");
                                      done (file.replaceWithData (data.getData(), data.getSize()) ? "saved" : "declined");
                                  });
        })

        // Drag to DAW: write the file, then a real drag the host accepts (FL Studio's playlist and piano roll)
        .withNativeFunction ("dragMidi", [this] (const juce::Array<juce::var>& a, Completion done)
        {
            if (a.size() < 2) { done ({}); return; }
            const auto data = fromBase64 (a[1].toString());
            auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("M1D1").getChildFile ("Drag");
            folder.createDirectory();
            const auto file = folder.getChildFile (juce::File::createLegalFileName ("M1D1 " + a[0].toString()));
            if (! file.replaceWithData (data.getData(), data.getSize())) { done ({}); return; }
            auto finished = std::make_shared<Completion> (std::move (done));
            if (! juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this,
                                                                             [finished] { (*finished) ({}); }))
                (*finished) ({});
        })

        // how much of the track has been heard (for listening on its own once there is enough)
        .withNativeFunction ("getCaptureInfo", [this] (const juce::Array<juce::var>&, Completion done)
        {
            auto& e = processor.getWebEngine();
            auto* o = new juce::DynamicObject();
            o->setProperty ("fromBeat", e.capturedFromBeat.load());
            o->setProperty ("beats", e.capturedBeats.load());
            done (juce::var (o));
        })

        // Listen to track: the audio the host played through this track
        .withNativeFunction ("getTrackAudio", [this] (const juce::Array<juce::var>&, Completion done)
        {
            const auto c = processor.getWebEngine().getCapture();
            if (c.samples.empty()) { done ({}); return; }
            auto* o = new juce::DynamicObject();
            o->setProperty ("sr", c.sampleRate);
            o->setProperty ("secs", (double) c.samples.size() / c.sampleRate);
            o->setProperty ("startBeat", c.startBeat);
            o->setProperty ("bpm", c.bpm);
            o->setProperty ("beatsPerBar", c.beatsPerBar);
            o->setProperty ("level", c.level);
            o->setProperty ("b64", juce::Base64::toBase64 (c.samples.data(), c.samples.size() * sizeof (float)));
            done (juce::var (o));
        });

   #if JUCE_WINDOWS
    options = options.withBackend (Options::Backend::webview2)
                     .withWinWebView2Options (Options::WinWebView2 {}
                                                  .withUserDataFolder (m1d1Folder (juce::File::userApplicationDataDirectory).getChildFile ("WebView2"))
                                                  .withBackgroundColour (juce::Colour (0xff0b0b0b))
                                                  .withStatusBarDisabled());
   #endif
    return options;
}
