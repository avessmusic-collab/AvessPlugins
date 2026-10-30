// Offscreen editor renderer (dev tool): renders the real TENSIONAudioProcessorEditor
// to PNG without needing a display/window server session.
//   cmake --build build --target TENSION_UISnapshot
//   ./build/plugins/TENSION/TENSION_UISnapshot <out.png> [tensionPercent=65] [riseMode=1] [animSeconds=1.5]
#include "PluginEditor.h"
#include "PluginProcessor.h"

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;

    const juce::String out = argc > 1 ? argv[1] : "tension_ui.png";
    const double tension = argc > 2 ? juce::String (argv[2]).getDoubleValue() : 65.0;
    const int mode = argc > 3 ? juce::String (argv[3]).getIntValue() : 1;
    const int animMs = (int) ((argc > 4 ? juce::String (argv[4]).getDoubleValue() : 1.5) * 1000.0);

    TENSIONAudioProcessor proc;
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    auto* ed = static_cast<TENSIONAudioProcessorEditor*> (editor.get());
    ed->setBounds (0, 0, 520, 520);
    ed->setPreviewState (tension, mode);

    juce::MessageManager::getInstance()->runDispatchLoopUntil (animMs);

    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
    juce::File f (juce::File::getCurrentWorkingDirectory().getChildFile (out));
    f.deleteFile();
    juce::FileOutputStream os (f);
    juce::PNGImageFormat png;
    png.writeImageToStream (img, os);
    std::printf ("wrote %s (%dx%d)\n", f.getFullPathName().toRawUTF8(), img.getWidth(), img.getHeight());
    editor.reset();
    return 0;
}
