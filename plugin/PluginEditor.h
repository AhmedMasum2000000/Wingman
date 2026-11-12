#pragma once
#include "PluginProcessor.h"

namespace ui
{
class Look : public juce::LookAndFeel_V4
{
public:
    Look();
    void drawComboBox (juce::Graphics&, int, int, bool, int, int, int, int, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;
    void drawPopupMenuBackground (juce::Graphics&, int, int) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool, bool) override;
    juce::Font getTextButtonFont (juce::TextButton&, int) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool, bool) override;
    void drawLinearSlider (juce::Graphics&, int, int, int, int, float, float, float,
                           juce::Slider::SliderStyle, juce::Slider&) override;
};

// Round record / stop button with a pulsing glow while a take is running.
class RecordButton : public juce::Button
{
public:
    RecordButton() : juce::Button ("Record") {}
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    bool armed = false, recording = false;
    float pulse = 0.0f;
};

// The captured audio, with chord boundaries and the playhead.
class WaveformView : public juce::Component
{
public:
    explicit WaveformView (BacklineProcessor& p) : processor (p) {}
    void paint (juce::Graphics&) override;
    void resized() override { peaks.clear(); }
    void update (bool force);
    juce::String status;

private:
    BacklineProcessor& processor;
    std::vector<float> peaks;
    int lastRevision = -1;
};

// The progression as cards; click one to swap the chord.
class ChordStrip : public juce::Component
{
public:
    explicit ChordStrip (BacklineProcessor& p) : processor (p) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override { hovered = -1; repaint(); }
    int activeSegment = -1;

private:
    int segmentAt (int x) const;
    BacklineProcessor& processor;
    int hovered = -1;
};

// A part (chords, bass, audio-to-MIDI) shown as a small piano roll you drag into the DAW.
class PartTile : public juce::Component
{
public:
    PartTile (BacklineProcessor&, BacklineProcessor::Part, juce::String title, juce::Colour colour, bool hasRhythm);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override { dragging = false; }
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
    void update();

    juce::ComboBox rhythm;

private:
    BacklineProcessor& processor;
    const BacklineProcessor::Part part;
    const juce::String title;
    const juce::Colour colour;
    std::vector<backline::Note> notes;
    bool dragging = false, hover = false;
};
}

class BacklineEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit BacklineEditor (BacklineProcessor&);
    ~BacklineEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();

private:
    void timerCallback() override;

    BacklineProcessor& processor;
    ui::Look look;
    int lastRevision = -1, tick = 0;
    bool lastArmed = false, lastRecording = false;

    ui::RecordButton recordButton;
    ui::WaveformView waveform;
    juce::ComboBox tonicBox, modeBox, changeBox;
    juce::OwnedArray<juce::TextButton> ideaButtons;
    ui::ChordStrip chordStrip;
    juce::ToggleButton previewButton { "Preview" };
    juce::Slider volume;
    ui::PartTile chordsTile, bassTile, melodyTile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BacklineEditor)
};
