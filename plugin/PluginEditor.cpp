#include "PluginEditor.h"

using namespace backline;

namespace
{
const juce::Colour background (0xff15171c), panel (0xff22252d), accent (0xff4fb3a5), textColour (0xffe8eaed);
}

//==============================================================================
DragTile::DragTile (juce::String text, std::function<juce::File()> makeFile)
    : label (std::move (text)), createFile (std::move (makeFile))
{
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
}

void DragTile::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (panel);
    g.fillRoundedRectangle (area, 6.0f);
    g.setColour (accent.withAlpha (isEnabled() ? 0.9f : 0.3f));
    g.drawRoundedRectangle (area, 6.0f, 1.0f);
    g.setColour (textColour.withAlpha (isEnabled() ? 1.0f : 0.4f));
    g.setFont (14.0f);
    g.drawFittedText (label, getLocalBounds().reduced (6), juce::Justification::centred, 2);
}

void DragTile::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging || ! isEnabled() || e.getDistanceFromDragStart() < 6)
        return;
    const auto file = createFile();
    if (file.existsAsFile())
    {
        dragging = true;
        juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this);
    }
}

//==============================================================================
void ChordStrip::paint (juce::Graphics& g)
{
    const auto& progression = processor.getProgression();
    g.setColour (panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
    if (progression.empty())
    {
        g.setColour (textColour.withAlpha (0.6f));
        g.setFont (15.0f);
        g.drawFittedText ("Press Record, play a few bars of your track, then press Stop.",
                          getLocalBounds().reduced (10), juce::Justification::centred, 2);
        return;
    }
    const float width = (float) getWidth() / (float) progression.size();
    for (size_t i = 0; i < progression.size(); ++i)
    {
        const auto cell = juce::Rectangle<float> ((float) i * width, 0.0f, width, (float) getHeight()).reduced (3.0f);
        g.setColour (accent.withAlpha (0.25f));
        g.fillRoundedRectangle (cell, 5.0f);
        g.setColour (textColour);
        g.setFont (juce::jmin (22.0f, width * 0.4f));
        g.drawFittedText (chordName (progression[i]), cell.toNearestInt(), juce::Justification::centred, 1);
    }
}

void ChordStrip::mouseDown (const juce::MouseEvent& e)
{
    const auto& progression = processor.getProgression();
    if (progression.empty())
        return;
    const int segment = juce::jlimit (0, (int) progression.size() - 1,
                                      e.x * (int) progression.size() / juce::jmax (1, getWidth()));
    const auto alternatives = processor.getAlternatives (segment);
    juce::PopupMenu menu;
    menu.addSectionHeader ("Chords that fit here (best first)");
    for (size_t i = 0; i < alternatives.size(); ++i)
        menu.addItem ((int) i + 1, chordName (alternatives[i]), true, alternatives[i] == progression[(size_t) segment]);

    juce::Component::SafePointer<ChordStrip> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [safe, segment, alternatives] (int result)
    {
        if (safe != nullptr && result > 0)
            safe->processor.setChord (segment, alternatives[(size_t) result - 1]);
    });
}

//==============================================================================
BacklineEditor::BacklineEditor (BacklineProcessor& p)
    : AudioProcessorEditor (p), processor (p), chordStrip (p),
      chordsTile ("Drag chords MIDI", [&p] { return p.writeMidiFile (BacklineProcessor::Part::Chords); }),
      bassTile ("Drag bass MIDI", [&p] { return p.writeMidiFile (BacklineProcessor::Part::Bass); }),
      melodyTile ("Drag audio-to-MIDI", [&p] { return p.writeMidiFile (BacklineProcessor::Part::Melody); })
{
    for (auto* c : std::initializer_list<juce::Component*> { &recordButton, &status, &tonicBox, &modeBox, &changeBox,
             &chordRhythmBox, &bassRhythmBox, &keyLabel, &changeLabel, &chordRhythmLabel, &bassRhythmLabel,
             &ideasLabel, &chordStrip, &previewButton, &volume, &chordsTile, &bassTile, &melodyTile })
        addAndMakeVisible (c);

    for (auto* l : { &status, &keyLabel, &changeLabel, &chordRhythmLabel, &bassRhythmLabel, &ideasLabel })
        l->setColour (juce::Label::textColourId, textColour);

    recordButton.onClick = [this] { processor.setRecordArmed (! processor.isRecordArmed()); refresh(); };

    for (int i = 0; i < 12; ++i) tonicBox.addItem (pitchClassName (i), i + 1);
    modeBox.addItemList ({ "major", "minor" }, 1);
    auto keyChanged = [this]
    {
        processor.setKey ({ tonicBox.getSelectedId() - 1, modeBox.getSelectedId() == 2, 1.0f });
    };
    tonicBox.onChange = keyChanged;
    modeBox.onChange = keyChanged;

    changeBox.addItem ("2 beats", 2); changeBox.addItem ("1 bar", 4); changeBox.addItem ("2 bars", 8);
    changeBox.onChange = [this] { processor.setBeatsPerChord ((double) changeBox.getSelectedId()); };

    chordRhythmBox.addItemList ({ "Sustained", "Quarter notes", "Offbeat stabs", "Dotted" }, 1);
    chordRhythmBox.onChange = [this] { processor.setChordRhythm ((ChordRhythm) (chordRhythmBox.getSelectedId() - 1)); };
    bassRhythmBox.addItemList ({ "Sustained", "Eighth notes", "Octaves", "Root and fifth", "Offbeat" }, 1);
    bassRhythmBox.onChange = [this] { processor.setBassRhythm ((BassRhythm) (bassRhythmBox.getSelectedId() - 1)); };

    for (int i = 0; i < 4; ++i)
    {
        auto* b = ideaButtons.add (new juce::TextButton (juce::String (i + 1)));
        b->setClickingTogglesState (false);
        b->onClick = [this, i] { processor.selectIdea (i); };
        addAndMakeVisible (b);
    }

    previewButton.setColour (juce::ToggleButton::textColourId, textColour);
    previewButton.onClick = [this] { processor.previewEnabled = previewButton.getToggleState(); };
    volume.setSliderStyle (juce::Slider::LinearHorizontal);
    volume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    volume.setRange (0.0, 1.0);
    volume.onValueChange = [this] { processor.previewGain = (float) volume.getValue(); };

    setSize (720, 380);
    refresh();
    startTimerHz (10);
}

void BacklineEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);
    g.setColour (accent);
    g.setFont (juce::Font (juce::FontOptions (22.0f, juce::Font::bold)));
    g.drawText ("Backline", 16, 10, 200, 28, juce::Justification::centredLeft);
}

void BacklineEditor::resized()
{
    auto area = getLocalBounds().reduced (16);
    auto top = area.removeFromTop (30);
    top.removeFromLeft (120);
    recordButton.setBounds (top.removeFromRight (90));
    top.removeFromRight (10);
    status.setBounds (top);
    area.removeFromTop (12);

    auto row = area.removeFromTop (26);
    keyLabel.setBounds (row.removeFromLeft (36));
    tonicBox.setBounds (row.removeFromLeft (64));
    row.removeFromLeft (6);
    modeBox.setBounds (row.removeFromLeft (84));
    row.removeFromLeft (20);
    changeLabel.setBounds (row.removeFromLeft (84));
    changeBox.setBounds (row.removeFromLeft (90));
    row.removeFromLeft (20);
    ideasLabel.setBounds (row.removeFromLeft (44));
    for (auto* b : ideaButtons)
    {
        b->setBounds (row.removeFromLeft (34));
        row.removeFromLeft (4);
    }
    area.removeFromTop (12);

    chordStrip.setBounds (area.removeFromTop (110));
    area.removeFromTop (12);

    row = area.removeFromTop (26);
    chordRhythmLabel.setBounds (row.removeFromLeft (92));
    chordRhythmBox.setBounds (row.removeFromLeft (130));
    row.removeFromLeft (20);
    bassRhythmLabel.setBounds (row.removeFromLeft (84));
    bassRhythmBox.setBounds (row.removeFromLeft (130));
    row.removeFromLeft (20);
    previewButton.setBounds (row.removeFromLeft (84));
    volume.setBounds (row);
    area.removeFromTop (16);

    auto tiles = area.removeFromTop (60);
    const int tileWidth = (tiles.getWidth() - 20) / 3;
    chordsTile.setBounds (tiles.removeFromLeft (tileWidth));
    tiles.removeFromLeft (10);
    bassTile.setBounds (tiles.removeFromLeft (tileWidth));
    tiles.removeFromLeft (10);
    melodyTile.setBounds (tiles);
}

void BacklineEditor::timerCallback()
{
    if (processor.getRevision() != lastRevision || processor.isRecordArmed() || recordButton.getButtonText() != "Record")
        refresh();
}

void BacklineEditor::refresh()
{
    lastRevision = processor.getRevision();
    const bool armed = processor.isRecordArmed();
    recordButton.setButtonText (armed ? "Stop" : "Record");
    recordButton.setColour (juce::TextButton::buttonColourId, armed ? juce::Colours::firebrick : panel);

    if (processor.isRecording())
        status.setText ("Recording... " + juce::String (processor.getRecordedSeconds(), 1) + " s", juce::dontSendNotification);
    else if (armed)
        status.setText ("Waiting for playback to start", juce::dontSendNotification);
    else if (processor.hasCapture())
        status.setText ("Captured " + juce::String (processor.getRecordedSeconds(), 1) + " s at "
                            + juce::String (processor.getBpm(), 1) + " BPM - detected " + keyName (processor.getKey()),
                        juce::dontSendNotification);
    else if (processor.hasProgression())
        status.setText ("Progression restored. Record again to get new ideas.", juce::dontSendNotification);
    else if (processor.getRecordedSeconds() > 0.0)
        status.setText ("That capture was too short. Record at least a bar.", juce::dontSendNotification);
    else
        status.setText ("Put Backline on the track you want to build around.", juce::dontSendNotification);

    const auto key = processor.getKey();
    tonicBox.setSelectedId (key.tonic + 1, juce::dontSendNotification);
    modeBox.setSelectedId (key.minor ? 2 : 1, juce::dontSendNotification);
    changeBox.setSelectedId ((int) processor.getBeatsPerChord(), juce::dontSendNotification);
    chordRhythmBox.setSelectedId ((int) processor.getChordRhythm() + 1, juce::dontSendNotification);
    bassRhythmBox.setSelectedId ((int) processor.getBassRhythm() + 1, juce::dontSendNotification);
    previewButton.setToggleState (processor.previewEnabled.load(), juce::dontSendNotification);
    volume.setValue (processor.previewGain.load(), juce::dontSendNotification);

    for (int i = 0; i < ideaButtons.size(); ++i)
    {
        ideaButtons[i]->setEnabled (i < processor.getNumIdeas());
        ideaButtons[i]->setColour (juce::TextButton::buttonColourId,
                                   i == processor.getSelectedIdea() && processor.getNumIdeas() > 0 ? accent.darker (0.3f) : panel);
    }
    chordsTile.setEnabled (processor.hasProgression());
    bassTile.setEnabled (processor.hasProgression());
    melodyTile.setEnabled (processor.hasCapture());
    chordStrip.repaint();
    chordsTile.repaint(); bassTile.repaint(); melodyTile.repaint();
}
