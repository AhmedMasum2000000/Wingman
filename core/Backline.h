// Backline core: audio analysis and harmony generation. No external dependencies.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace backline
{
using Chroma = std::array<float, 12>;

struct Key
{
    int tonic = 0;       // pitch class, 0 = C
    bool minor = false;
    float confidence = 0.0f;
};

enum class Quality { Major, Minor };

struct Chord
{
    int root = 0;
    Quality quality = Quality::Major;
    bool operator== (const Chord& o) const { return root == o.root && quality == o.quality; }
    bool operator!= (const Chord& o) const { return ! (*this == o); }
};

// Times and lengths are in beats (quarter notes) from the start of the capture.
struct Note
{
    int pitch = 60;
    double start = 0.0;
    double length = 1.0;
    int velocity = 100;
};

enum class ChordRhythm { Sustain, Quarters, Offbeat, Dotted };
enum class BassRhythm { Sustain, Eighths, Octaves, RootFifth, Offbeat };

struct ChromaFrames
{
    std::vector<Chroma> frames;
    double hopSeconds = 0.0;
    double windowSeconds = 0.0;
};

std::string pitchClassName (int pitchClass);
std::string chordName (Chord chord);
std::string keyName (Key key);

// Analysis
ChromaFrames computeChroma (const float* mono, size_t numSamples, double sampleRate);
Key detectKey (const ChromaFrames& chroma);
std::vector<Chroma> segmentChroma (const ChromaFrames& chroma, double bpm,
                                   double beatsPerSegment, int numSegments);
std::vector<Note> audioToNotes (const float* mono, size_t numSamples, double sampleRate, double bpm);

// Harmony
std::vector<Chord> chordsInKey (Key key);
std::vector<Chord> rankChords (const Chroma& segment, Key key); // best fit first
std::vector<std::vector<Chord>> suggestProgressions (const std::vector<Chroma>& segments,
                                                     Key key, int count);

// Parts
std::vector<Note> renderChords (const std::vector<Chord>& progression, double beatsPerChord, ChordRhythm rhythm);
std::vector<Note> renderBass (const std::vector<Chord>& progression, double beatsPerChord, BassRhythm rhythm);

// Standard MIDI file (type 0)
std::vector<uint8_t> writeMidiFile (const std::vector<Note>& notes, double bpm);
}
