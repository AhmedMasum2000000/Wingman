#pragma once
#include "PluginProcessor.h"

namespace ui
{
// Bundled typefaces, shared by every open editor and released with the last one.
struct Fonts
{
    Fonts();
    juce::Typeface::Ptr display, displayMedium, body, bodyBold;
};

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
    juce::Rectangle<int> getTooltipBounds (const juce::String&, juce::Point<int>, juce::Rectangle<int>) override;
    void drawTooltip (juce::Graphics&, const juce::String&, int, int) override;
};

// "V7", "ii", "IVmaj7", "vii" + degree sign: the chord's function in the key.
juce::String romanNumeral (backline::Chord chord, backline::Key key);

// Where the chord cards go. The loop is laid out in rows of equal length (whole bars when there
// is more than one row); a card's width is proportional to its length, and a chord that crosses
// a row break is drawn as one piece per row. Pure geometry, so it can be tested on its own.
struct StripLayout
{
    double loopBeats = 0.0, beatsPerRow = 1.0;
    int rows = 1;
    float width = 0.0f, height = 0.0f;

    static StripLayout make (const backline::Progression& progression, double beatsPerBar, float width, float height);
    float rowHeight() const { return height / (float) juce::jmax (1, rows); }
    // One rectangle per row the span covers, left to right, top to bottom.
    std::vector<juce::Rectangle<float>> pieces (double start, double length) const;
    // The beat under a point (the row under it, clamped to the loop).
    double beatAt (juce::Point<float> point) const;
    // The boundary (between chord i and i + 1) within `tolerance` pixels of the point, or -1.
    int boundaryAt (const backline::Progression& progression, juce::Point<float> point, float tolerance) const;
    // The line a boundary at `beat` is drawn on (at the end of a row for a beat on a row break).
    juce::Line<float> boundaryLine (double beat) const;
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

// Play / stop for the plugin's own transport (shown when the host has none).
class TransportButton : public juce::Button
{
public:
    TransportButton() : juce::Button ("Play") {}
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    bool playing = false;
};

// The manual tempo: drag up or down (or scroll) to change it, click to type a value. A change is
// reported through onCommit once the gesture ends, because each one re-analyses the take.
class TempoField : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    TempoField();
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void setTempo (double bpm);              // shown unless the user is changing it
    double getShownTempo() const { return shown; }
    bool isEditing() const { return dragging || editor != nullptr || isTimerRunning(); }
    void showEditor();
    std::function<void (double)> onCommit;
    static juce::String format (double bpm); // "120", "96.5"

private:
    void timerCallback() override;
    void commit (double bpm);
    void closeEditor (bool apply);
    double shown = BacklineProcessor::defaultTempo, dragStart = 0.0;
    bool dragging = false, moved = false;
    std::unique_ptr<juce::TextEditor> editor;
};

// A combo box listing the instruments; it rescans the sounds folder each time it opens.
class InstrumentBox : public juce::ComboBox
{
public:
    void showPopup() override { if (onOpen) onOpen(); juce::ComboBox::showPopup(); }
    std::function<void()> onOpen;
};

// The captured audio, with chord boundaries and the playhead.
class WaveformView : public juce::Component
{
public:
    explicit WaveformView (BacklineProcessor& p) : processor (p) {}
    void paint (juce::Graphics&) override;
    void resized() override { peaks.clear(); }
    void update (bool force);
    juce::String title;            // the take: its file name or "Recorded take"
    juce::String message;          // shown when there is no take to describe
    juce::StringArray facts;       // e.g. "8 bars", "120 BPM", "C major"
    bool messageIsError = false;
    int topRowInset = 0;           // room kept free on the right of the top row for controls
    static constexpr int topRow = 28;

private:
    BacklineProcessor& processor;
    std::vector<float> peaks;
    int lastRevision = -1;
};

// The progression as cards sized by their length. Click a card for its menu; drag the line
// between two cards to move the chord change (it snaps to the grid).
class ChordStrip : public juce::Component
{
public:
    explicit ChordStrip (BacklineProcessor& p) : processor (p) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    StripLayout layout() const;
    int chordAt (juce::Point<float> point) const;     // -1 between or outside cards
    int activeSegment = -1;                             // the chord playing, highlighted
    juce::String emptyTitle, emptyHint;                 // shown when there is no progression

    // The menu for one chord. Item ids 1..choices.size() pick choices[id - 1]; the rest are edits.
    struct Menu
    {
        juce::PopupMenu menu;
        std::vector<backline::Chord> choices;
    };
    enum { splitId = 1001, passingId, deleteId };
    Menu buildMenu (int index) const;
    // Does what choosing `result` from that menu means. Returns true if the progression changed.
    static bool applyMenuResult (BacklineProcessor&, int index, int result, const std::vector<backline::Chord>& choices);
    // Shows the resize feedback for a boundary as if the mouse were on it (-1 for none).
    void showBoundary (int boundary, bool dragging);

private:
    BacklineProcessor& processor;
    int hovered = -1, hoveredBoundary = -1, draggedBoundary = -1;
};

// A part (chords, bass, audio-to-MIDI) shown as a small piano roll, with MIDI (and for chords
// and bass, WAV) to drag into the DAW and the instrument that plays it.
class PartTile : public juce::Component, public juce::SettableTooltipClient
{
public:
    PartTile (BacklineProcessor&, BacklineProcessor::Part, juce::String title, juce::Colour colour, bool hasSound);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override { dragging = false; }
    void mouseMove (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent& e) override { mouseMove (e); }
    void mouseExit (const juce::MouseEvent&) override { hoverChip = -1; hover = false; repaint(); }
    void update();
    void updateInstruments (bool rescan);
    bool isDraggingOut() const { return exporting.load(); }
    enum Chip { none = -1, midi = 0, wav = 1 };
    juce::Rectangle<int> chipBounds (int chip) const;   // empty when that chip is not offered
    int chipAt (juce::Point<int>) const;

    juce::ComboBox rhythm;
    InstrumentBox instrument;

private:
    BacklineProcessor& processor;
    const BacklineProcessor::Part part;
    const juce::String title;
    const juce::Colour colour;
    const bool hasSound;
    std::vector<backline::Note> notes;
    juce::StringArray instrumentNames;
    bool dragging = false, hover = false;
    int hoverChip = -1, pressedChip = -1;
    std::shared_ptr<std::atomic<bool>> exportingFlag = std::make_shared<std::atomic<bool>> (false);
    std::atomic<bool>& exporting = *exportingFlag;
};
}

class BacklineEditor : public juce::AudioProcessorEditor, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    explicit BacklineEditor (BacklineProcessor&);
    ~BacklineEditor() override;
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void refresh();

    // Audio files dropped anywhere on the window become the take.
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int, int) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray& files, int, int) override;
    void loadFile (const juce::File& file);

    ui::ChordStrip& getChordStrip() { return chordStrip; }

private:
    void timerCallback() override;
    void openFileChooser();

    BacklineProcessor& processor;
    juce::SharedResourcePointer<ui::Fonts> fonts;
    ui::Look look;
    int lastRevision = -1, tick = 0;
    bool lastArmed = false, lastRecording = false, lastLoading = false, lastAnalysing = false, lastHost = false;
    juce::String pendingFile, dropName, shownError;
    juce::uint32 errorShownAt = 0;

    ui::RecordButton recordButton;
    ui::WaveformView waveform;
    juce::TextButton openButton { "Open audio file" };
    juce::ToggleButton playTakeButton { "Play take" };
    ui::TransportButton playButton;
    ui::TempoField tempoField;
    juce::ComboBox tonicBox, modeBox, changeBox;
    juce::OwnedArray<juce::TextButton> ideaButtons;
    juce::ToggleButton seventhsButton { "Sevenths" };
    ui::ChordStrip chordStrip;
    juce::ToggleButton previewButton { "Preview" };
    juce::Slider volume;
    ui::PartTile chordsTile, bassTile, melodyTile;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TooltipWindow tooltips { this, 700 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BacklineEditor)
};
