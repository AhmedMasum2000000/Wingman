// Instruments for the chord and bass parts: the built-in synth and multi-sample instruments
// loaded from folders of audio files, plus an offline renderer used for WAV export.
#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "Backline.h"

namespace instruments
{
// The name of the built-in synth. It always exists and is the default for both parts.
extern const char* const builtInName;

// One sample of a multi-sample instrument and the notes it plays.
struct Zone
{
    int rootNote = 60;                 // MIDI note the sample was recorded at
    int lowNote = 0, highNote = 127;   // notes it covers (pitch-shifted)
    double sampleRate = 44100.0;
    juce::AudioBuffer<float> audio;    // one or two channels
};

// An instrument. Immutable once made and shared between the live preview, the WAV export and
// the message thread, so the sample data is decoded once.
struct Instrument
{
    juce::String name;                       // builtInName, or the folder name
    std::vector<Zone> zones;                 // empty for the built-in synth
    std::array<int, 128> zoneForNote {};     // index into zones for each MIDI note (sampled only)
    bool isSampled() const { return ! zones.empty(); }
};
using InstrumentPtr = std::shared_ptr<const Instrument>;

// The built-in synth (one shared object).
InstrumentPtr builtIn();

// <user application data>/Backline/Sounds. Each sub-folder is one instrument.
juce::File defaultSoundsFolder();

// The MIDI note a sample file name refers to, or -1. A note name with octave wins (C4 = 60 is
// middle C; C#3, Db3, Bb2, Cs4, a2, C-1 also work, and the last one in the name counts),
// otherwise a number from 0 to 127 ("Piano 060.wav", "48.wav"). Takes the name without extension.
int noteFromFileName (const juce::String& nameWithoutExtension);

// True for the sample file types an instrument folder may hold (WAV, AIFF, FLAC).
bool isSampleFile (const juce::File& file);

// Names of the instrument folders under `root` that hold at least one usable sample, sorted.
// Only looks at file names, so it is cheap enough to call whenever a menu opens.
juce::StringArray scanInstruments (const juce::File& root);

// Decodes the instrument in root/<name>. Returns nullptr when the folder is missing or holds no
// usable sample. One sample per note (the first file by name wins, e.g. one velocity layer);
// each covers the notes nearest to its own pitch. Samples are capped at 12 seconds with a short
// fade at the end, and the whole instrument is scaled so its loudest sample peaks at full scale.
// Instruments stay cached while in use, so the same unchanged folder is decoded only once (for
// both parts and every plugin instance). Slow (decodes audio): never call it on the audio thread.
InstrumentPtr loadInstrument (const juce::File& root, const juce::String& name);

// 4-point Hermite interpolation of data[0..length) at a fractional position; silence outside.
float interpolate (const float* data, int length, double position) noexcept;

// A synthesiser that plays `instrument` (nullptr means the built-in synth). isBass picks the
// bass or chord flavour of the built-in voice and the level of sampled voices.
std::unique_ptr<juce::Synthesiser> createSynth (const InstrumentPtr& instrument, bool isBass, double sampleRate);

// Renders one loop of notes (`loopBeats` long at `bpm`, notes past the loop end are cut there)
// followed by the release tail, at most `maxTailSeconds`; trailing silence is trimmed but the
// result is never shorter than the loop. Stereo. Allocates: not for the audio thread.
juce::AudioBuffer<float> renderNotes (const std::vector<backline::Note>& notes, double loopBeats, double bpm,
                                      double sampleRate, const InstrumentPtr& instrument, bool isBass,
                                      double maxTailSeconds);
}
