// Renders the plugin interface to PNG files without a DAW or a display (development aid).
// Usage: backline_screenshot <output-directory>
#include "../plugin/PluginEditor.h"

struct FakePlayHead : juce::AudioPlayHead
{
    bool playing = false, musical = true;
    double ppq = 0.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        if (! musical) // like the standalone app: a clock, but no tempo or position
        {
            info.setTimeInSamples (0);
            return info;
        }
        info.setIsPlaying (playing);
        info.setBpm (120.0);
        info.setPpqPosition (ppq);
        return info;
    }
};

static void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

static bool waitFor (const std::function<bool()>& done)
{
    for (int i = 0; i < 400 && ! done(); ++i)
        pump (50);
    return done();
}

static void save (juce::Component& c, const juce::File& file)
{
    pump (300);
    const auto image = c.createComponentSnapshot (c.getLocalBounds(), true, 2.0f);
    file.deleteFile();
    juce::FileOutputStream out (file);
    juce::PNGImageFormat().writeImageToStream (image, out);
    std::printf ("wrote %s\n", file.getFullPathName().toRawUTF8());
}

// A processor with its editor, driven block by block like a host would.
struct Session
{
    static constexpr double sampleRate = 48000.0;
    static constexpr int blockSize = 512;
    BacklineProcessor processor;
    FakePlayHead head;
    juce::AudioBuffer<float> buffer { 2, blockSize };
    juce::MidiBuffer midi;
    juce::int64 pos = 0;
    std::unique_ptr<juce::AudioProcessorEditor> editor; // after the processor: destroyed first

    explicit Session (bool hostTransport)
    {
        head.musical = hostTransport;
        processor.setPlayHead (&head);
        processor.setPlayConfigDetails (2, 2, sampleRate, blockSize);
        processor.prepareToPlay (sampleRate, blockSize);
        run (0.05, false); // the processor learns whether there is a host transport
        if (! hostTransport)
            processor.setTransportPlaying (false);
        editor.reset (processor.createEditor());
    }

    BacklineEditor& ui() { return *dynamic_cast<BacklineEditor*> (editor.get()); }

    // Feeds a little melody in A minor / C major (two-second phrase) or silence.
    void run (double seconds, bool melody)
    {
        const int notes[16] = { 69, 72, 76, 72, 65, 69, 72, 69, 67, 72, 76, 72, 67, 71, 74, 71 };
        for (int done = 0; done < (int) (seconds * sampleRate); done += blockSize, pos += blockSize)
        {
            buffer.clear();
            for (int i = 0; melody && i < blockSize; ++i)
            {
                const double t = (double) (pos + i) / sampleRate, inNote = std::fmod (t, 0.5);
                const double f = juce::MidiMessage::getMidiNoteInHertz (notes[((int) (t / 0.5)) % 16]);
                const float v = (float) (0.4 * std::exp (-inNote * 5.0) * juce::jmin (1.0, inNote * 300.0)
                                         * (std::sin (juce::MathConstants<double>::twoPi * f * t) + 0.4 * std::sin (2.0 * juce::MathConstants<double>::twoPi * f * t)));
                buffer.setSample (0, i, v);
                buffer.setSample (1, i, v);
            }
            head.ppq = (double) pos / sampleRate * 2.0;
            processor.processBlock (buffer, midi);
        }
    }

    // Records from the host (16 seconds: 8 bars at 120 BPM).
    void recordTake (double seconds = 16.0)
    {
        processor.setRecordArmed (true);
        head.playing = true;
        run (seconds, true);
        head.playing = false;
        run (0.1, false);
        waitFor ([this] { return processor.hasCapture(); });
    }

    // Plays from a position (in seconds of host time) for a moment so the playhead shows.
    void playAt (double seconds)
    {
        head.playing = true;
        pos = (juce::int64) (seconds * sampleRate);
        run (0.2, false);
    }
};

// A song sketch at 96 BPM: a pad playing C Am F G and a plucked melody, 8 bars.
static juce::File writeDemoFile (const juce::File& folder)
{
    const double rate = 44100.0, bpm = 96.0, beat = 60.0 / bpm;
    const int bars = 8, length = (int) (bars * 4 * beat * rate);
    const int chords[4][3] = { { 60, 64, 67 }, { 57, 60, 64 }, { 53, 57, 60 }, { 55, 59, 62 } };
    const int tune[8] = { 72, 76, 79, 76, 72, 69, 72, 74 };
    juce::AudioBuffer<float> audio (2, length);
    for (int i = 0; i < length; ++i)
    {
        const double t = i / rate, beats = t / beat, inBeat = std::fmod (beats, 1.0) * beat;
        const int bar = (int) (beats / 4.0) % 4;
        double v = 0.0;
        for (int n : chords[bar])
            v += 0.08 * std::sin (juce::MathConstants<double>::twoPi * juce::MidiMessage::getMidiNoteInHertz (n - 12) * t);
        const double f = juce::MidiMessage::getMidiNoteInHertz (tune[(int) beats % 8]);
        v += 0.35 * std::exp (-inBeat * 6.0) * juce::jmin (1.0, inBeat * 400.0) * std::sin (juce::MathConstants<double>::twoPi * f * t);
        v += 0.25 * std::exp (-inBeat * 30.0) * std::sin (juce::MathConstants<double>::twoPi * 60.0 * t); // kick on every beat
        audio.setSample (0, i, (float) v);
        audio.setSample (1, i, (float) v);
    }
    folder.createDirectory();
    auto file = folder.getChildFile ("Night drive demo.wav");
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream().release());
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), rate, 2u, 16, {}, 0));
    if (writer != nullptr)
    {
        stream.release();
        writer->writeFromAudioSampleBuffer (audio, 0, length);
    }
    return file;
}

// The chord card menu is a desktop window, which needs a display; draw its items offscreen with
// the same LookAndFeel calls the menu window makes, so its look can be checked here.
static void saveMenu (const juce::PopupMenu& menu, juce::LookAndFeel& look, const juce::File& file)
{
    struct Row { const juce::PopupMenu::Item* item; int height; };
    std::vector<Row> rows;
    int width = 220, height = 0;
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
    {
        const auto& item = it.getItem();
        int w = 0, h = 0;
        look.getIdealPopupMenuItemSize (item.text, item.isSeparator, -1, w, h);
        if (item.shortcutKeyDescription.isNotEmpty())
            w += juce::roundToInt (juce::GlyphArrangement::getStringWidth (look.getPopupMenuFont().withHeight (look.getPopupMenuFont().getHeight() * 0.75f),
                                                                          item.shortcutKeyDescription)) + 24;
        width = juce::jmax (width, w);
        rows.push_back ({ &item, h });
        height += h;
    }
    const int border = look.getPopupMenuBorderSizeWithOptions (juce::PopupMenu::Options());
    juce::Image image (juce::Image::ARGB, (width + 2 * border) * 2, (height + 2 * border) * 2, true);
    {
        juce::Graphics g (image);
        g.addTransform (juce::AffineTransform::scale (2.0f));
        look.drawPopupMenuBackground (g, width + 2 * border, height + 2 * border);
        int y = border;
        for (const auto& row : rows)
        {
            const auto area = juce::Rectangle<int> (border, y, width, row.height);
            const auto& item = *row.item;
            if (item.isSectionHeader)
                look.drawPopupMenuSectionHeader (g, area, item.text);
            else
            {
                juce::Graphics::ScopedSaveState state (g);
                g.reduceClipRegion (area);
                g.setOrigin (area.getPosition());
                look.drawPopupMenuItem (g, area.withPosition ({}), item.isSeparator, item.isEnabled, false, item.isTicked,
                                        item.subMenu != nullptr, item.text, item.shortcutKeyDescription, nullptr, nullptr);
            }
            y += row.height;
        }
    }
    file.deleteFile();
    juce::FileOutputStream out (file);
    juce::PNGImageFormat().writeImageToStream (image, out);
    std::printf ("wrote %s\n", file.getFullPathName().toRawUTF8());
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const auto dir = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : ".");
    dir.createDirectory();
    const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("backline-screenshots");
    using backline::Chord;
    using backline::Quality;

    // 1. A DAW, nothing recorded yet; then recording.
    {
        Session s (true);
        save (*s.editor, dir.getChildFile ("ui-empty.png"));
        s.processor.setRecordArmed (true);
        s.head.playing = true;
        s.run (3.0, true);
        save (*s.editor, dir.getChildFile ("ui-recording.png"));
    }

    // 2. A take recorded from the DAW track, ideas ready, playing.
    {
        Session s (true);
        s.recordTake();
        s.processor.setChordRhythm (backline::ChordRhythm::Dotted);
        s.processor.setBassRhythm (backline::BassRhythm::Octaves);
        s.playAt (5.2);
        save (*s.editor, dir.getChildFile ("ui-take-host.png"));

        // 3. Edited: mixed lengths, a seventh, a sus, a slash chord and a passing chord.
        s.processor.setChord (0, Chord { 0, Quality::Major7 });         // Cmaj7
        s.processor.splitChord (1);                                     // F | F
        s.processor.setChord (2, Chord { 5, Quality::Sus2 });           // Fsus2
        s.processor.setChord (3, Chord { 0, Quality::Major, 4 });       // C/E
        s.processor.insertPassingChord (4);                             // into the next chord
        s.processor.setChord (4, Chord { 7, Quality::Sus4 });           // Gsus4
        s.processor.moveChordBoundary (0, 3.0);                         // Cmaj7 3 beats, F 3 beats
        s.processor.setChord (8, Chord { 7, Quality::Dominant7 });      // G7
        s.playAt (2.2);
        save (*s.editor, dir.getChildFile ("ui-edited.png"));
        {
            auto built = s.ui().getChordStrip().buildMenu (3);
            saveMenu (built.menu, s.editor->getLookAndFeel(), dir.getChildFile ("ui-menu.png"));
            for (juce::PopupMenu::MenuItemIterator it (built.menu); it.next();)
                if (it.getItem().subMenu != nullptr && it.getItem().text == "Chord type")
                    saveMenu (*it.getItem().subMenu, s.editor->getLookAndFeel(), dir.getChildFile ("ui-menu-types.png"));
        }

        // 4. Dragging the boundary between two cards.
        s.head.playing = false;
        s.run (0.1, false);
        s.ui().getChordStrip().showBoundary (3, true);
        save (*s.editor, dir.getChildFile ("ui-resize.png"));
        s.ui().getChordStrip().showBoundary (-1, false);
    }

    // 5. A long progression: half-bar chords, each split in two, plus passing chords.
    {
        Session s (true);
        s.recordTake (24.0);
        s.processor.setChordRate (0.5);
        for (int i = (int) s.processor.getProgression().size(); --i >= 0;)
            s.processor.splitChord (i);
        s.processor.insertPassingChord (6);
        s.processor.insertPassingChord (20);
        s.processor.setSevenths (true);
        s.playAt (9.1);
        std::printf ("long progression: %d chords\n", (int) s.processor.getProgression().size());
        save (*s.editor, dir.getChildFile ("ui-long.png"));
    }

    // 6. The standalone app (no host transport): empty, then a loaded file with the tempo
    //    estimate applied and the own transport playing; then a file dragged over the window.
    {
        Session s (false);
        save (*s.editor, dir.getChildFile ("ui-standalone-empty.png"));
        const auto file = writeDemoFile (temp);
        s.ui().loadFile (file);
        save (*s.editor, dir.getChildFile ("ui-standalone-loading.png"));
        waitFor ([&] { return ! s.processor.isLoadingFile(); });
        std::printf ("loaded %s: %.1f BPM estimated, %d chords, error '%s'\n", s.processor.getTakeName().toRawUTF8(),
                     s.processor.getEstimatedTempo(), (int) s.processor.getProgression().size(), s.processor.getLoadError().toRawUTF8());
        s.processor.setTransportPlaying (true);
        s.run (7.3, false);
        save (*s.editor, dir.getChildFile ("ui-standalone-file.png"));

        s.ui().fileDragEnter ({ temp.getChildFile ("Another song idea with a rather long file name.wav").getFullPathName() }, 10, 10);
        save (*s.editor, dir.getChildFile ("ui-drop.png"));
        s.ui().fileDragExit ({});
    }
    temp.deleteRecursively();
    return 0;
}
