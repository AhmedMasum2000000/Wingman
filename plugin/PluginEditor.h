#pragma once
#include "PluginProcessor.h"

// A tile the user drags into the DAW to drop a MIDI file on a track.
class DragTile : public juce::Component
{
public:
    DragTile (juce::String text, std::function<juce::File()> makeFile);
    void paint (juce::Graphics&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override { dragging = false; }

private:
    juce::String label;
    std::function<juce::File()> createFile;
    bool dragging = false;
};

// The chord progression; click a chord to swap it for an alternative.
class ChordStrip : public juce::Component
{
public:
    explicit ChordStrip (BacklineProcessor& p) : processor (p) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    BacklineProcessor& processor;
};

class BacklineEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit BacklineEditor (BacklineProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void refresh();

    BacklineProcessor& processor;
    int lastRevision = -1;

    juce::TextButton recordButton { "Record" };
    juce::Label status;
    juce::ComboBox tonicBox, modeBox, changeBox, chordRhythmBox, bassRhythmBox;
    juce::Label keyLabel { {}, "Key" }, changeLabel { {}, "Chord every" },
                chordRhythmLabel { {}, "Chord rhythm" }, bassRhythmLabel { {}, "Bass rhythm" },
                ideasLabel { {}, "Ideas" };
    juce::OwnedArray<juce::TextButton> ideaButtons;
    ChordStrip chordStrip;
    juce::ToggleButton previewButton { "Preview" };
    juce::Slider volume;
    DragTile chordsTile, bassTile, melodyTile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BacklineEditor)
};
