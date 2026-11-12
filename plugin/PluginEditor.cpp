#include "PluginEditor.h"

using namespace backline;

namespace ui
{
namespace
{
const juce::Colour bgTop (0xff14161f), bgBottom (0xff0a0b10), panel (0xff191c27), panelEdge (0x14ffffff),
                   control (0xff232735), text (0xffeef0f6), textDim (0xff8b90a3),
                   accent (0xff5eead4), record (0xffff4d5e), bassColour (0xfffbbf5a), melodyColour (0xffa78bfa);

juce::Font font (float size, bool bold = false)
{
    return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
}

// Small spaced-out capitals used for section labels.
void drawLabel (juce::Graphics& g, const juce::String& s, juce::Rectangle<int> area,
                juce::Colour colour = textDim, juce::Justification j = juce::Justification::centredLeft)
{
    g.setColour (colour);
    g.setFont (font (11.0f, true).withExtraKerningFactor (0.14f));
    g.drawText (s.toUpperCase(), area, j);
}

void drawPanel (juce::Graphics& g, juce::Rectangle<float> area, float radius = 12.0f)
{
    g.setColour (panel);
    g.fillRoundedRectangle (area, radius);
    g.setColour (panelEdge);
    g.drawRoundedRectangle (area.reduced (0.5f), radius, 1.0f);
}

// Each scale degree gets its own hue, so the shape of a progression is visible at a glance.
juce::Colour chordColour (Chord chord, Key key)
{
    static const float hues[12] = { 0.47f, 0.52f, 0.58f, 0.63f, 0.70f, 0.09f, 0.13f, 0.96f, 0.76f, 0.83f, 0.32f, 0.38f };
    return juce::Colour::fromHSV (hues[(chord.root - key.tonic + 12) % 12], 0.62f, 0.96f, 1.0f);
}

juce::String romanNumeral (Chord chord, Key key)
{
    static const char* const numerals[12] = { "I", "bII", "II", "bIII", "III", "IV", "#IV", "V", "bVI", "VI", "bVII", "VII" };
    const juce::String n (numerals[(chord.root - key.tonic + 12) % 12]);
    return chord.quality == Quality::Minor ? n.toLowerCase() : n;
}

// One octave of piano keys with the chord's notes lit.
void drawKeyboard (juce::Graphics& g, juce::Rectangle<float> area, Chord chord, juce::Colour lit)
{
    bool on[12] = {};
    on[chord.root] = on[(chord.root + (chord.quality == Quality::Major ? 4 : 3)) % 12] = on[(chord.root + 7) % 12] = true;
    static const int whites[7] = { 0, 2, 4, 5, 7, 9, 11 };
    static const int blacks[5] = { 1, 3, 6, 8, 10 };
    static const float blackPos[5] = { 1.0f, 2.0f, 4.0f, 5.0f, 6.0f };
    const float w = area.getWidth() / 7.0f;
    for (int i = 0; i < 7; ++i)
    {
        g.setColour (on[whites[i]] ? lit : juce::Colours::white.withAlpha (0.16f));
        g.fillRoundedRectangle (area.getX() + (float) i * w + 0.75f, area.getY(), w - 1.5f, area.getHeight(), 1.5f);
    }
    for (int i = 0; i < 5; ++i)
    {
        const juce::Rectangle<float> key (area.getX() + blackPos[i] * w - w * 0.3f, area.getY(), w * 0.6f, area.getHeight() * 0.58f);
        g.setColour (juce::Colour (0xff0d0e14));
        g.fillRoundedRectangle (key.expanded (0.75f, 0.0f), 1.5f);
        g.setColour (on[blacks[i]] ? lit : juce::Colour (0xff2a2e3c));
        g.fillRoundedRectangle (key, 1.5f);
    }
}
}

//==============================================================================
Look::Look()
{
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::arrowColourId, textDim);
    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff1e2230));
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::headerTextColourId, textDim);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.22f));
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
    setColour (juce::TextButton::textColourOffId, textDim);
    setColour (juce::TextButton::textColourOnId, juce::Colour (0xff06231f));
}

void Look::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto area = juce::Rectangle<float> (0, 0, (float) width, (float) height).reduced (0.5f);
    g.setColour (box.isMouseOver (true) ? control.brighter (0.08f) : control);
    g.fillRoundedRectangle (area, 8.0f);
    g.setColour (juce::Colours::white.withAlpha (box.hasKeyboardFocus (true) ? 0.25f : 0.08f));
    g.drawRoundedRectangle (area, 8.0f, 1.0f);

    juce::Path arrow;
    const float cx = (float) width - 15.0f, cy = (float) height * 0.5f;
    arrow.startNewSubPath (cx - 4.0f, cy - 2.0f);
    arrow.lineTo (cx, cy + 2.0f);
    arrow.lineTo (cx + 4.0f, cy - 2.0f);
    g.setColour (textDim);
    g.strokePath (arrow, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font Look::getComboBoxFont (juce::ComboBox&) { return font (14.0f); }
juce::Font Look::getPopupMenuFont() { return font (14.5f); }
juce::Font Look::getTextButtonFont (juce::TextButton&, int) { return font (13.0f, true); }

void Look::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (findColour (juce::PopupMenu::backgroundColourId));
    g.setColour (juce::Colours::white.withAlpha (0.10f));
    g.drawRect (0, 0, width, height);
}

void Look::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&, bool highlighted, bool down)
{
    const auto area = button.getLocalBounds().toFloat().reduced (0.5f);
    const float radius = area.getHeight() * 0.5f;
    if (button.getToggleState())
    {
        g.setGradientFill ({ accent.brighter (0.15f), 0.0f, 0.0f, accent.darker (0.15f), 0.0f, area.getHeight(), false });
        g.fillRoundedRectangle (area, radius);
        return;
    }
    const float alpha = ! button.isEnabled() ? 0.4f : 1.0f;
    g.setColour ((down ? control.brighter (0.15f) : highlighted ? control.brighter (0.08f) : control).withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (area, radius);
    g.setColour (juce::Colours::white.withAlpha (0.07f * alpha));
    g.drawRoundedRectangle (area, radius, 1.0f);
}

void Look::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool)
{
    const bool on = button.getToggleState();
    const auto track = juce::Rectangle<float> (0.0f, ((float) button.getHeight() - 18.0f) * 0.5f, 32.0f, 18.0f);
    g.setColour (on ? accent.withAlpha (highlighted ? 1.0f : 0.9f) : control.brighter (highlighted ? 0.15f : 0.05f));
    g.fillRoundedRectangle (track, 9.0f);
    g.setColour (on ? juce::Colour (0xff06231f) : textDim);
    g.fillEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ on ? track.getRight() - 9.0f : track.getX() + 9.0f, track.getCentreY() }));
    g.setColour (on ? text : textDim);
    g.setFont (font (13.5f));
    g.drawText (button.getButtonText(), button.getLocalBounds().withTrimmedLeft (40), juce::Justification::centredLeft);
}

void Look::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos, float, float,
                             juce::Slider::SliderStyle, juce::Slider& slider)
{
    const float cy = (float) y + (float) height * 0.5f;
    const auto track = juce::Rectangle<float> ((float) x, cy - 2.0f, (float) width, 4.0f);
    g.setColour (control.brighter (0.05f));
    g.fillRoundedRectangle (track, 2.0f);
    g.setColour (accent.withAlpha (slider.isEnabled() ? 1.0f : 0.4f));
    g.fillRoundedRectangle (track.withRight (sliderPos), 2.0f);
    g.setColour (juce::Colours::white);
    g.fillEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ sliderPos, cy }));
}

//==============================================================================
void RecordButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto area = getLocalBounds().toFloat();
    const auto centre = area.getCentre();
    const float radius = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f - 8.0f;

    if (armed || recording) // soft pulsing halo
        for (int i = 3; i >= 1; --i)
        {
            g.setColour (record.withAlpha ((recording ? 0.10f : 0.05f) * (0.5f + 0.5f * pulse)));
            g.fillEllipse (juce::Rectangle<float> (2.0f, 2.0f).withCentre (centre).expanded (radius + (float) i * 2.5f * (0.6f + 0.4f * pulse)));
        }

    const auto disc = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre);
    if (recording)
        g.setGradientFill ({ record.brighter (0.2f), centre.x, disc.getY(), record.darker (0.25f), centre.x, disc.getBottom(), false });
    else
        g.setColour (down ? control.brighter (0.15f) : highlighted ? control.brighter (0.08f) : control);
    g.fillEllipse (disc);
    g.setColour ((recording ? juce::Colours::white : record).withAlpha (recording ? 0.25f : armed ? 0.5f + 0.5f * pulse : 0.55f));
    g.drawEllipse (disc.reduced (0.75f), 1.5f);

    if (armed || recording) // stop square
    {
        g.setColour (recording ? juce::Colours::white : record);
        g.fillRoundedRectangle (juce::Rectangle<float> (radius * 0.62f, radius * 0.62f).withCentre (centre), 3.0f);
    }
    else
    {
        g.setColour (record);
        g.fillEllipse (juce::Rectangle<float> (radius * 0.72f, radius * 0.72f).withCentre (centre));
    }
}

//==============================================================================
void WaveformView::update (bool force)
{
    if (! force && processor.getRevision() == lastRevision && ! processor.isRecording())
        return;
    lastRevision = processor.getRevision();
    peaks = processor.getWaveform (juce::jmax (1, getWidth() / 4));
    repaint();
}

void WaveformView::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    const bool recording = processor.isRecording();
    const bool hasAudio = processor.getRecordedSeconds() > 0.0 && (recording || processor.hasCapture());
    const float midY = area.getCentreY() + 8.0f, maxHalf = area.getHeight() * 0.5f - 14.0f;

    drawLabel (g, status, getLocalBounds().removeFromTop (16), recording ? record : textDim);

    if (! hasAudio)
    {
        // Resting state: a quiet dotted line.
        g.setColour (juce::Colours::white.withAlpha (0.10f));
        for (float x = area.getX(); x < area.getRight(); x += 6.0f)
            g.fillRoundedRectangle (x, midY - 1.0f, 3.0f, 2.0f, 1.0f);
        return;
    }

    // Chord boundaries.
    const int numChords = (int) processor.getProgression().size();
    if (! recording && numChords > 1)
    {
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        for (int i = 1; i < numChords; ++i)
            g.fillRect (area.getX() + area.getWidth() * (float) i / (float) numChords, area.getY() + 20.0f, 1.0f, area.getHeight() - 20.0f);
    }

    // Mirrored bars, lit up to the playhead.
    const double loop = processor.getLoopBeats(), head = processor.getPlayheadBeats();
    const float headX = (head >= 0.0 && loop > 0.0 && ! recording) ? area.getX() + area.getWidth() * (float) (head / loop) : -1.0f;
    const juce::Colour base = recording ? record : accent;
    float peakMax = 0.05f;
    for (float p : peaks) peakMax = juce::jmax (peakMax, p);
    for (size_t i = 0; i < peaks.size(); ++i)
    {
        const float x = area.getX() + (float) i * 4.0f;
        const float h = juce::jmax (1.0f, std::sqrt (peaks[i] / peakMax) * maxHalf);
        g.setColour (base.withAlpha (headX < 0.0f || recording ? 0.75f : (x <= headX ? 1.0f : 0.35f)));
        g.fillRoundedRectangle (x, midY - h, 2.5f, h * 2.0f, 1.25f);
    }
    if (headX >= 0.0f)
    {
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.fillRoundedRectangle (headX - 0.75f, area.getY() + 20.0f, 1.5f, area.getHeight() - 20.0f, 0.75f);
    }
}

//==============================================================================
int ChordStrip::segmentAt (int x) const
{
    const int n = (int) processor.getProgression().size();
    return n == 0 ? -1 : juce::jlimit (0, n - 1, x * n / juce::jmax (1, getWidth()));
}

void ChordStrip::mouseMove (const juce::MouseEvent& e)
{
    const int now = segmentAt (e.x);
    if (now != hovered) { hovered = now; repaint(); }
}

void ChordStrip::paint (juce::Graphics& g)
{
    const auto& progression = processor.getProgression();
    const auto key = processor.getKey();
    if (progression.empty())
    {
        // Resting state: one dashed drop zone.
        const auto zone = getLocalBounds().toFloat().reduced (4.0f, 0.0f);
        g.setColour (juce::Colours::white.withAlpha (0.025f));
        g.fillRoundedRectangle (zone, 12.0f);
        juce::Path outline, dashed;
        outline.addRoundedRectangle (zone.reduced (0.5f), 12.0f);
        const float dashes[2] = { 5.0f, 5.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.fillPath (dashed);
        g.setColour (text.withAlpha (0.85f));
        g.setFont (font (17.0f, true));
        g.drawText ("Record a few bars to get chord ideas", getLocalBounds().translated (0, -11), juce::Justification::centred);
        g.setColour (textDim);
        g.setFont (font (13.5f));
        g.drawText ("Start playback on a bar line, press the red button, then stop.", getLocalBounds().translated (0, 13), juce::Justification::centred);
        return;
    }

    const float width = (float) getWidth() / (float) progression.size();
    const float gap = progression.size() > 8 ? 2.0f : 4.0f;
    for (size_t i = 0; i < progression.size(); ++i)
    {
        const auto chord = progression[i];
        const auto colour = chordColour (chord, key);
        const bool active = (int) i == activeSegment, hot = (int) i == hovered;
        const auto cell = juce::Rectangle<float> ((float) i * width, 0.0f, width, (float) getHeight()).reduced (gap, active ? 0.0f : 3.0f);

        g.setGradientFill ({ colour.withAlpha (active ? 0.62f : hot ? 0.34f : 0.24f), 0.0f, cell.getY(),
                             colour.withAlpha (active ? 0.30f : hot ? 0.16f : 0.09f), 0.0f, cell.getBottom(), false });
        g.fillRoundedRectangle (cell, 12.0f);
        g.setColour (colour.withAlpha (active ? 1.0f : hot ? 0.75f : 0.38f));
        g.drawRoundedRectangle (cell.reduced (0.75f), 12.0f, active ? 2.0f : 1.0f);

        auto inner = cell.reduced (juce::jmin (12.0f, width * 0.12f), 10.0f);
        if (width > 44.0f)
            drawLabel (g, romanNumeral (chord, key), inner.removeFromTop (14.0f).toNearestInt(), colour.brighter (0.3f));
        const float keysHeight = juce::jmin (26.0f, inner.getHeight() * 0.28f);
        const auto keys = inner.removeFromBottom (keysHeight);
        if (width > 60.0f)
            drawKeyboard (g, keys.withSizeKeepingCentre (juce::jmin (keys.getWidth(), 110.0f), keysHeight), chord, colour);

        g.setColour (juce::Colours::white);
        g.setFont (font (juce::jlimit (15.0f, 40.0f, width * 0.36f), true));
        g.drawFittedText (chordName (chord), inner.toNearestInt(), juce::Justification::centred, 1);
    }
}

void ChordStrip::mouseDown (const juce::MouseEvent& e)
{
    const int segment = segmentAt (e.x);
    if (segment < 0)
        return;
    const auto& progression = processor.getProgression();
    const auto alternatives = processor.getAlternatives (segment);
    const auto key = processor.getKey();
    juce::PopupMenu menu;
    menu.setLookAndFeel (&getLookAndFeel());
    menu.addSectionHeader ("Chords that fit here, best first");
    for (size_t i = 0; i < alternatives.size(); ++i)
        menu.addItem ((int) i + 1, juce::String (chordName (alternatives[i])) + "    " + romanNumeral (alternatives[i], key),
                      true, alternatives[i] == progression[(size_t) segment]);

    const float width = (float) getWidth() / (float) progression.size();
    const auto target = localAreaToGlobal (juce::Rectangle<int> ((int) ((float) segment * width), 0, (int) width, getHeight()));
    juce::Component::SafePointer<ChordStrip> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (target).withMinimumWidth (190),
                        [safe, segment, alternatives] (int result)
                        {
                            if (safe != nullptr && result > 0)
                                safe->processor.setChord (segment, alternatives[(size_t) result - 1]);
                        });
}

//==============================================================================
PartTile::PartTile (BacklineProcessor& p, BacklineProcessor::Part whichPart, juce::String name, juce::Colour c, bool hasRhythm)
    : processor (p), part (whichPart), title (std::move (name)), colour (c)
{
    if (hasRhythm)
        addAndMakeVisible (rhythm);
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
}

void PartTile::resized()
{
    rhythm.setBounds (getWidth() - 150, 10, 138, 26);
}

void PartTile::update()
{
    notes = processor.getPartNotes (part);
    repaint();
}

void PartTile::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    const bool ready = ! notes.empty();
    drawPanel (g, area);
    if (ready && hover)
    {
        g.setColour (colour.withAlpha (0.55f));
        g.drawRoundedRectangle (area.reduced (0.75f), 12.0f, 1.5f);
    }

    auto inner = getLocalBounds().reduced (14, 10);
    auto header = inner.removeFromTop (26);
    g.setColour (colour.withAlpha (ready ? 1.0f : 0.4f));
    g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ (float) header.getX() + 4.0f, (float) header.getCentreY() }));
    drawLabel (g, title, header.withTrimmedLeft (16), ready ? text : textDim);

    auto footer = inner.removeFromBottom (16);
    drawLabel (g, ready ? "Drag into your DAW" : (part == BacklineProcessor::Part::Melody ? "Single-note audio only" : "Nothing yet"),
               footer, ready && hover ? colour : textDim.withAlpha (0.8f));
    if (ready) // grip dots
    {
        g.setColour (hover ? colour : textDim.withAlpha (0.6f));
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 3; ++c)
                g.fillEllipse ((float) footer.getRight() - 16.0f + (float) c * 6.0f, (float) footer.getY() + 3.0f + (float) r * 6.0f, 2.5f, 2.5f);
    }

    // Piano roll of the part.
    const auto roll = inner.reduced (0, 6).toFloat();
    g.setColour (juce::Colours::black.withAlpha (0.22f));
    g.fillRoundedRectangle (roll, 6.0f);
    if (! ready)
        return;
    int low = 127, high = 0;
    double length = 0.0;
    for (const auto& n : notes)
    {
        low = juce::jmin (low, n.pitch); high = juce::jmax (high, n.pitch);
        length = juce::jmax (length, n.start + n.length);
    }
    length = juce::jmax (length, part == BacklineProcessor::Part::Melody ? 0.0 : processor.getLoopBeats());
    const int rows = juce::jmax (6, high - low + 1);
    const auto grid = roll.reduced (5.0f, 4.0f);
    const float rowHeight = grid.getHeight() / (float) rows;
    const float offset = (float) (rows - (high - low + 1)) * 0.5f;
    g.setColour (colour.withAlpha (hover ? 1.0f : 0.85f));
    for (const auto& n : notes)
    {
        const float x = grid.getX() + grid.getWidth() * (float) (n.start / length);
        const float w = juce::jmax (2.0f, grid.getWidth() * (float) (n.length / length) - 1.0f);
        const float y = grid.getBottom() - ((float) (n.pitch - low) + offset + 1.0f) * rowHeight;
        g.fillRoundedRectangle (x, y + 0.5f, w, juce::jmax (1.5f, rowHeight - 1.0f), 1.5f);
    }
}

void PartTile::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging || notes.empty() || e.getDistanceFromDragStart() < 6)
        return;
    const auto file = processor.writeMidiFile (part);
    if (file.existsAsFile())
    {
        dragging = true;
        juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this);
    }
}
}

//==============================================================================
using namespace ui;

BacklineEditor::BacklineEditor (BacklineProcessor& p)
    : AudioProcessorEditor (p), processor (p), waveform (p), chordStrip (p),
      chordsTile (p, BacklineProcessor::Part::Chords, "Chords", accent, true),
      bassTile (p, BacklineProcessor::Part::Bass, "Bass", bassColour, true),
      melodyTile (p, BacklineProcessor::Part::Melody, "Audio to MIDI", melodyColour, false)
{
    setLookAndFeel (&look);
    for (auto* c : std::initializer_list<juce::Component*> { &recordButton, &waveform, &tonicBox, &modeBox, &changeBox,
             &chordStrip, &previewButton, &volume, &chordsTile, &bassTile, &melodyTile })
        addAndMakeVisible (c);

    recordButton.setTooltip ("Record audio from this track");
    recordButton.onClick = [this] { processor.setRecordArmed (! processor.isRecordArmed()); refresh(); };

    for (int i = 0; i < 12; ++i) tonicBox.addItem (pitchClassName (i), i + 1);
    modeBox.addItemList ({ "major", "minor" }, 1);
    auto keyChanged = [this] { processor.setKey ({ tonicBox.getSelectedId() - 1, modeBox.getSelectedId() == 2, 1.0f }); };
    tonicBox.onChange = keyChanged;
    modeBox.onChange = keyChanged;

    changeBox.addItem ("Every 2 beats", 2); changeBox.addItem ("Every bar", 4); changeBox.addItem ("Every 2 bars", 8);
    changeBox.onChange = [this] { processor.setBeatsPerChord ((double) changeBox.getSelectedId()); };

    chordsTile.rhythm.addItemList ({ "Sustained", "Quarter notes", "Offbeat stabs", "Dotted" }, 1);
    chordsTile.rhythm.onChange = [this] { processor.setChordRhythm ((ChordRhythm) (chordsTile.rhythm.getSelectedId() - 1)); };
    bassTile.rhythm.addItemList ({ "Sustained", "Eighth notes", "Octaves", "Root and fifth", "Offbeat" }, 1);
    bassTile.rhythm.onChange = [this] { processor.setBassRhythm ((BassRhythm) (bassTile.rhythm.getSelectedId() - 1)); };

    for (int i = 0; i < 4; ++i)
    {
        auto* b = ideaButtons.add (new juce::TextButton ("Idea " + juce::String (i + 1)));
        b->onClick = [this, i] { processor.selectIdea (i); };
        addAndMakeVisible (b);
    }

    previewButton.onClick = [this] { processor.previewEnabled = previewButton.getToggleState(); };
    volume.setSliderStyle (juce::Slider::LinearHorizontal);
    volume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    volume.setRange (0.0, 1.0);
    volume.onValueChange = [this] { processor.previewGain = (float) volume.getValue(); };

    setSize (900, 580);
    refresh();
    startTimerHz (30);
}

BacklineEditor::~BacklineEditor() { setLookAndFeel (nullptr); }

void BacklineEditor::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    g.setGradientFill ({ bgTop, 0.0f, 0.0f, bgBottom, 0.0f, area.getHeight(), false });
    g.fillAll();
    juce::ColourGradient glow (accent.withAlpha (0.10f), 80.0f, -40.0f, accent.withAlpha (0.0f), 80.0f, 300.0f, true);
    g.setGradientFill (glow);
    g.fillRect (area.withHeight (320.0f));

    // Logo: three stacked bars, like a chord.
    const float lx = 24.0f, ly = 27.0f;
    const float widths[3] = { 22.0f, 15.0f, 19.0f };
    for (int i = 0; i < 3; ++i)
    {
        g.setColour (accent.withAlpha (1.0f - 0.25f * (float) i));
        g.fillRoundedRectangle (lx, ly + (float) i * 7.0f, widths[i], 4.5f, 2.25f);
    }
    g.setColour (text);
    g.setFont (font (19.0f, true).withExtraKerningFactor (0.16f));
    g.drawText ("BACKLINE", 56, 22, 180, 30, juce::Justification::centredLeft);

    drawLabel (g, "Key", { tonicBox.getX() - 40, tonicBox.getY(), 34, tonicBox.getHeight() }, textDim, juce::Justification::centredRight);
    drawPanel (g, recordButton.getBounds().getUnion (waveform.getBounds()).expanded (14, 12).toFloat());
}

void BacklineEditor::resized()
{
    auto area = getLocalBounds().reduced (24, 20);

    auto header = area.removeFromTop (34);
    modeBox.setBounds (header.removeFromRight (96));
    header.removeFromRight (6);
    tonicBox.setBounds (header.removeFromRight (66));
    header.removeFromRight (64);
    volume.setBounds (header.removeFromRight (96).reduced (0, 4));
    header.removeFromRight (4);
    previewButton.setBounds (header.removeFromRight (104));
    area.removeFromTop (30);

    auto capture = area.removeFromTop (88).reduced (14, 0);
    recordButton.setBounds (capture.removeFromLeft (76).withSizeKeepingCentre (76, 76));
    capture.removeFromLeft (12);
    waveform.setBounds (capture);
    area.removeFromTop (28);

    auto row = area.removeFromTop (30);
    for (auto* b : ideaButtons)
    {
        b->setBounds (row.removeFromLeft (78));
        row.removeFromLeft (6);
    }
    changeBox.setBounds (row.removeFromRight (140));
    area.removeFromTop (12);

    chordStrip.setBounds (area.removeFromTop (156));
    area.removeFromTop (16);

    const int tileWidth = (area.getWidth() - 24) / 3;
    chordsTile.setBounds (area.removeFromLeft (tileWidth));
    area.removeFromLeft (12);
    bassTile.setBounds (area.removeFromLeft (tileWidth));
    area.removeFromLeft (12);
    melodyTile.setBounds (area);
}

void BacklineEditor::timerCallback()
{
    ++tick;
    const bool armed = processor.isRecordArmed(), recording = processor.isRecording();
    if (processor.getRevision() != lastRevision || armed != lastArmed || recording != lastRecording || (recording && tick % 6 == 0))
        refresh();

    if (armed || recording)
    {
        recordButton.pulse = 0.5f + 0.5f * std::sin ((float) tick * 0.21f);
        recordButton.repaint();
    }
    if (recording && tick % 3 == 0)
        waveform.update (true);

    // Follow the transport.
    const double head = processor.getPlayheadBeats(), loop = processor.getLoopBeats();
    const int count = (int) processor.getProgression().size();
    const int active = (head >= 0.0 && loop > 0.0 && count > 0) ? juce::jlimit (0, count - 1, (int) (head / loop * count)) : -1;
    if (active != chordStrip.activeSegment)
    {
        chordStrip.activeSegment = active;
        chordStrip.repaint();
    }
    if (head >= 0.0 && ! recording)
        waveform.repaint();
}

void BacklineEditor::refresh()
{
    lastRevision = processor.getRevision();
    const bool armed = lastArmed = processor.isRecordArmed();
    const bool recording = lastRecording = processor.isRecording();
    recordButton.armed = armed;
    recordButton.recording = recording;
    recordButton.repaint();

    if (recording)
        waveform.status = "Recording  " + juce::String (processor.getRecordedSeconds(), 1) + " s";
    else if (armed)
        waveform.status = "Waiting for playback to start";
    else if (processor.hasCapture())
        waveform.status = juce::String (juce::roundToInt (processor.getLoopBeats() / 4.0)) + " bars   "
                        + juce::String (processor.getBpm(), 1) + " BPM   " + keyName (processor.getKey());
    else if (processor.hasProgression())
        waveform.status = "Progression restored - record again for new ideas";
    else if (processor.getRecordedSeconds() > 0.0)
        waveform.status = "That take was too short - record at least a bar";
    else
        waveform.status = "Put Backline on the track you want to build around";

    const auto key = processor.getKey();
    tonicBox.setSelectedId (key.tonic + 1, juce::dontSendNotification);
    modeBox.setSelectedId (key.minor ? 2 : 1, juce::dontSendNotification);
    changeBox.setSelectedId ((int) processor.getBeatsPerChord(), juce::dontSendNotification);
    chordsTile.rhythm.setSelectedId ((int) processor.getChordRhythm() + 1, juce::dontSendNotification);
    bassTile.rhythm.setSelectedId ((int) processor.getBassRhythm() + 1, juce::dontSendNotification);
    previewButton.setToggleState (processor.previewEnabled.load(), juce::dontSendNotification);
    volume.setValue (processor.previewGain.load(), juce::dontSendNotification);

    for (int i = 0; i < ideaButtons.size(); ++i)
    {
        ideaButtons[i]->setEnabled (i < processor.getNumIdeas());
        ideaButtons[i]->setToggleState (i == processor.getSelectedIdea() && processor.getNumIdeas() > 0, juce::dontSendNotification);
    }
    waveform.update (true);
    chordStrip.repaint();
    chordsTile.update(); bassTile.update(); melodyTile.update();
}
