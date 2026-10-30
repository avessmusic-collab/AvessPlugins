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

    // The visualizer reads engine telemetry, so pump real audio (pink-ish noise)
    // through the processor in ~real time while the dispatch loop animates.
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);
    {
        juce::Random rng (1234);
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        float lp = 0.0f;
        const juce::int64 endMs = juce::Time::currentTimeMillis() + animMs;
        while (juce::Time::currentTimeMillis() < endMs)
        {
            for (int n = 0; n < 512; ++n)
            {
                lp += 0.2f * (rng.nextFloat() * 2.0f - 1.0f - lp);
                buf.setSample (0, n, lp * 1.2f);
                buf.setSample (1, n, lp * 1.2f);
            }
            proc.processBlock (buf, midi);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (10);   // ~48k real time
        }
    }

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
