// Renders the plugin interface to PNG files without a DAW or a display (development aid).
// Usage: backline_screenshot <output-directory>
#include "../plugin/PluginEditor.h"

struct FakePlayHead : juce::AudioPlayHead
{
    bool playing = false;
    double ppq = 0.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying (playing);
        info.setBpm (120.0);
        info.setPpqPosition (ppq);
        return info;
    }
};

static void save (juce::Component& editor, const juce::File& file)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
    const auto image = editor.createComponentSnapshot (editor.getLocalBounds(), true, 2.0f);
    file.deleteFile();
    juce::FileOutputStream out (file);
    juce::PNGImageFormat().writeImageToStream (image, out);
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const auto dir = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : ".");
    const double sampleRate = 48000.0;
    const int blockSize = 512;

    BacklineProcessor processor;
    FakePlayHead playHead;
    processor.setPlayHead (&playHead);
    processor.setPlayConfigDetails (2, 2, sampleRate, blockSize);
    processor.prepareToPlay (sampleRate, blockSize);
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    save (*editor, dir.getChildFile ("ui-empty.png"));

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    juce::int64 pos = 0;
    auto run = [&] (double seconds, bool melody)
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
            playHead.ppq = (double) pos / sampleRate * 2.0;
            processor.processBlock (buffer, midi);
        }
    };

    processor.setRecordArmed (true);
    playHead.playing = true;
    run (3.0, true);
    save (*editor, dir.getChildFile ("ui-recording.png"));
    run (13.0, true);
    playHead.playing = false;
    run (0.1, false);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (400);
    processor.setChordRhythm (backline::ChordRhythm::Dotted);
    processor.setBassRhythm (backline::BassRhythm::Octaves);
    playHead.playing = true;
    pos = (juce::int64) (5.2 * sampleRate);
    run (0.2, false);
    save (*editor, dir.getChildFile ("ui-ideas.png"));
    return 0;
}
