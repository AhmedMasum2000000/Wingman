#pragma once

#include "PluginProcessor.h"

// The M1D1 interface: the same page as the browser preview, shown in a web view (WebView2 on Windows)
// and connected to the processor. The page sends the notes to play, its state and file requests;
// the editor reports the host's transport back to it about 30 times a second.
class WebEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit WebEditor (BacklineProcessor&);
    ~WebEditor() override;
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;
    juce::WebBrowserComponent::Options makeOptions();
    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);

    BacklineProcessor& processor;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::WebBrowserComponent browser;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WebEditor)
};
