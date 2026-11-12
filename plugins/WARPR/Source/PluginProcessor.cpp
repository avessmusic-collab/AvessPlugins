#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "State/StateIO.h"

//==============================================================================
PluginProcessor::PluginProcessor()
    : AudioProcessor (BusesProperties()
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),   // instrument: output only (pattern #4)
      apvts (*this, nullptr, juce::Identifier (warpr::identity::apvtsStateId), warpr::createParameterLayout())
{
    paramSnapshot.bind (apvts);
    backend.attachSampleStore (&sampleStore);
    backend.attachSynth (&synth);
    backend.attachApvts (&apvts);                     // shaperPreset / shaperSymmetry listeners
    synth.attachShaper (&shaperEngine);
    backend.attachShaper (&shaperEngine);             // builds + publishes the initial (identity) LUT set
    drainTimer.begin();
}

PluginProcessor::~PluginProcessor()
{
    drainTimer.end();
    sampleStore.shutdown();          // the loader's completion callback refers to `backend`
    synth.reset();                   // releases every voice's sample reference (audio is stopped)
    retireList.drain();
}

//==============================================================================
void PluginProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    drainRetired();

    shaperEngine.prepare (sampleRate);                // 10 ms LUT crossfade length; drops a running fade
    backend.servicePending();                         // a shaper preset / symmetry change that arrived while stopped
    synth.prepare (sampleRate, samplesPerBlock);      // designs the half-band, kills all voices
    synth.setCurrentSample (sampleStore.get());
    master.prepare (sampleRate);
    vizAnalyzer.prepare (sampleRate);
    activeVoices.store (0, std::memory_order_relaxed);

    backend.setSampleRate (sampleRate);                                // the response display is evaluated at this rate
    backend.bumpRevision (WarprUiBackend::Revision::filterResponse);   // sample-rate dependent display
}

void PluginProcessor::releaseResources()
{
}

bool PluginProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Stereo output only; no input bus (mono-only host layouts are not supported).
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainInputChannelSet().isDisabled();
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    buffer.clear();

    if (buffer.getNumChannels() < 2 || numSamples <= 0)
        return;

    // Block start: adopt a pending sample publication (single atomic exchange; the previous sample is
    // handed to the retire list, never freed here), read the parameters once.
    if (sampleStore.acquire())
        synth.setCurrentSample (sampleStore.get());

    paramSnapshot.read (paramBlock);
    synth.setBlockParams (paramBlock);

    // QWERTY keyboard notes queued by the UI (wait-free drain; MidiBuffer keeps events sorted)
    {
        int s1, n1, s2, n2;
        kbdFifo.prepareToRead (kbdFifo.getNumReady(), s1, n1, s2, n2);
        auto emit = [this, &midiMessages] (int start, int num)
        {
            for (int i = 0; i < num; ++i)
            {
                const KbdEvent& e = kbdEvents[(size_t) (start + i)];
                midiMessages.addEvent (e.on ? juce::MidiMessage::noteOn  (1, (int) e.note, e.vel)
                                            : juce::MidiMessage::noteOff (1, (int) e.note), 0);
            }
        };
        emit (s1, n1);
        emit (s2, n2);
        if (n1 + n2 > 0)
            kbdFifo.finishedRead (n1 + n2);
    }

    float* left = buffer.getWritePointer (0);
    float* right = buffer.getWritePointer (1);

    synth.renderBlock (left, right, numSamples, midiMessages);
    master.process (left, right, numSamples, paramBlock.get (warpr::ParamId::masterGain));
    vizAnalyzer.process (left, right, numSamples, backend.vizFifo(), &synth);      // wait-free, no allocation

    activeVoices.store (synth.getNumActiveVoices(), std::memory_order_relaxed);
}

void PluginProcessor::queueKeyboardNote (int midiNote, int velocity, bool noteOn) noexcept
{
    int s1, n1, s2, n2;
    kbdFifo.prepareToWrite (1, s1, n1, s2, n2);

    if (n1 > 0)
    {
        kbdEvents[(size_t) s1] = { (uint8_t) juce::jlimit (0, 127, midiNote),
                                   (uint8_t) juce::jlimit (1, 127, velocity),
                                   noteOn };
        kbdFifo.finishedWrite (1);
    }
}

//==============================================================================
juce::AudioProcessorEditor* PluginProcessor::createEditor()
{
    return new PluginEditor (*this);
}

//==============================================================================
void PluginProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // D1: the sample blob stays empty (the default wave is regenerated; embedding a loaded sample is D9).
    // SampleStore::latest() is the hook D9 will encode here.
    destData = warpr::state::write (apvts, backend.copyModel());
}

void PluginProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;

    warpr::UiModel model;

    if (warpr::state::readContainer (data, static_cast<std::size_t> (sizeInBytes), apvts, model))
    {
        backend.adoptModel (model);
        return;
    }

    // Fallback: plain XML written by getXmlFromBinary-style hosts / older builds (no sample).
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (warpr::state::readXml (*xml, apvts, model))
            backend.adoptModel (model);

    // Anything else: keep the current (default) state.
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PluginProcessor();
}
