#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "State/StateIO.h"

#include <limits>

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
    synth.attachModEngine (&modEngine);
    backend.attachModEngine (&modEngine);             // builds + publishes the default LFO tables, matrix and lfoCount

    // D9: presets, state host and settings behind the editor's native functions (all run on the message thread)
    backend.attachPresetManager (&presetManager);

    // "Init" factory preset = this plugin's pure default state, captured NOW (the APVTS still holds
    // every parameter's default - ampAttack 0 ms included - the UI model is the default and the sample
    // is the default wave, so the container carries blob 0).  Loading Init is a guaranteed clean reset
    // through the ordinary preset path.
    {
        juce::MemoryBlock initState (captureState());
        presetManager.setFactoryPresets ({ { "Init", std::move (initState) } });
    }
    backend.attachStateHost ([this] { return captureState(); },
                             [this] (const void* data, std::size_t numBytes) { return loadStateBlock (data, numBytes); });
    backend.attachSettings ([this] { return appSettings.load(); },
                            [this] (const warpr::AppSettingsData& data)
                            {
                                appSettings.save (data);
                                applyAppSettings (data);
                            });

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
    guardState.reset();                               // D10: the guard restarts from "idle" (the synth level was reset in prepare)
    guardWasActive = false;

    backend.setSampleRate (sampleRate);                               // the response display is evaluated at this rate
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

    // D10 CPU guard: the block's wall time is only measured while the (opt-in) setting is on
    const bool guardRequested = cpuGuard.load (std::memory_order_relaxed);
    const juce::int64 guardT0 = guardRequested ? juce::Time::getHighResolutionTicks() : (juce::int64) 0;

    const int numSamples = buffer.getNumSamples();
    buffer.clear();

    if (buffer.getNumChannels() < 2 || numSamples <= 0)
    {
        // Review 2026-11-16: some hosts flush MIDI in zero-sample blocks (transport changes) - a
        // dropped note-off is a stuck note.  Hand the events to the synth; it renders nothing.
        if (! midiMessages.isEmpty())
        {
            if (sampleStore.acquire())
                synth.setCurrentSample (sampleStore.get());

            paramSnapshot.read (paramBlock);
            synth.setBlockParams (paramBlock);

            float silent = 0.0f;
            synth.renderBlock (&silent, &silent, 0, midiMessages);
        }

        return;
    }

    // Block start: adopt a pending sample publication (single atomic exchange; the previous sample is
    // handed to the retire list, never freed here), read the parameters once.
    if (sampleStore.acquire())
        synth.setCurrentSample (sampleStore.get());

    paramSnapshot.read (paramBlock);
    synth.setBlockParams (paramBlock);

    // Host transport (ppq / tempo / play state / time signature) for the tempo-synced LFOs; once per block
    {
        warpr::TransportInfo info;

        if (auto* head = getPlayHead())
        {
            if (const auto pos = head->getPosition())
            {
                if (const auto ppq = pos->getPpqPosition())
                {
                    info.hasPpq = true;
                    info.ppq = *ppq;
                }

                if (const auto bpm = pos->getBpm())
                {
                    info.hasBpm = true;
                    info.bpm = *bpm;
                }

                info.playing = pos->getIsPlaying();

                if (const auto sig = pos->getTimeSignature())
                {
                    info.timeSigNumerator = sig->numerator;
                    info.timeSigDenominator = sig->denominator;
                }
            }
        }

        synth.setTransport (info);
    }

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
    master.process (left, right, numSamples, synth.modulatedMasterGainDb (paramBlock.get (warpr::ParamId::masterGain)));
    vizAnalyzer.process (left, right, numSamples, backend.vizFifo(), &synth);      // wait-free, no allocation

    activeVoices.store (synth.getNumActiveVoices(), std::memory_order_relaxed);

    // D10 CPU guard: load = this block's processing time / its duration, fed to the deterministic CpuGuard (1 s EMA,
    // engage 0.85 / release 0.60, one level per 0.25 s up / 2 s down).  The level reaches the NEXT block's note-ons.
    if (guardRequested)
    {
        const double fs = getSampleRate();

        if (fs > 0.0)
        {
            const double blockSeconds = (double) numSamples / fs;
            const double injected = injectedGuardLoad.load (std::memory_order_relaxed);
            const double load = injected >= 0.0
                                    ? injected
                                    : juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - guardT0) / blockSeconds;

            synth.setGuardLevel (guardState.update (load, blockSeconds));
            guardWasActive = true;
        }
    }
    else if (guardWasActive)
    {
        guardState.reset();
        synth.setGuardLevel (0);
        guardWasActive = false;
    }
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
juce::MemoryBlock PluginProcessor::captureState()
{
    // The sample is encoded lazily here (FLAC24 for integer-grid sources, float32 otherwise; the default wave is not
    // embedded) and memoised per SampleData, so a host that polls the state does not re-encode megabytes each time.
    return warpr::state::write (apvts, backend.copyModel(), blobCache.blobFor (sampleStore.latest()));
}

warpr::state::LoadOutcome PluginProcessor::loadStateBlock (const void* data, std::size_t numBytes)
{
    using warpr::state::LoadOutcome;

    if (data == nullptr || numBytes == 0)
        return LoadOutcome::rejected;

    warpr::UiModel model;
    warpr::state::SampleLoad sample;

    if (warpr::state::readContainer (data, numBytes, apvts, model, &sample))
    {
        // Publish the sample BEFORE adoptModel so the Revision::sample bump it performs sees the new sample.
        // No blob (or a default-wave flag) = the default wave: regenerated, never embedded.  A corrupt blob falls
        // back to it too - the parameters and the UI state are still valuable.
        if (sample.sample != nullptr)
            sampleStore.publish (sample.sample);
        else if (const auto current = sampleStore.latest(); current == nullptr || ! current->isDefaultWave)
            sampleStore.publish (warpr::SampleData::createDefaultWave());

        backend.adoptModel (model);
        return sample.corruptBlob ? LoadOutcome::loadedDefaultSampleAfterCorruptBlob : LoadOutcome::loaded;
    }

    // Fallback: plain XML written by getXmlFromBinary-style hosts / older builds.  It says nothing about the sample,
    // so the current one is kept.
    if (numBytes <= static_cast<std::size_t> (std::numeric_limits<int>::max()))
        if (auto xml = getXmlFromBinary (data, static_cast<int> (numBytes)))
            if (warpr::state::readXml (*xml, apvts, model))
            {
                backend.adoptModel (model);
                return LoadOutcome::loaded;
            }

    return LoadOutcome::rejected;                        // keep the current state
}

void PluginProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    destData = captureState();
}

void PluginProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;

    loadStateBlock (data, static_cast<std::size_t> (sizeInBytes));
}

//==============================================================================
void PluginProcessor::applyAppSettings (const warpr::AppSettingsData& requested)
{
    const auto data = warpr::AppSettings::sanitise (requested);
    synth.setOscillatorBudget (data.oscillatorBudget);            // relaxed atomics, read per block
    synth.setInterpolationQuality (data.interpolationQuality);    // 0 High (sinc8), 1 Eco (Hermite4): every sample read
    cpuGuard.store (data.cpuGuard, std::memory_order_relaxed);    // processBlock measures + feeds the CpuGuard while true
}

void PluginProcessor::loadAppSettings()
{
    applyAppSettings (appSettings.load());
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    auto* processor = new PluginProcessor();
    processor->loadAppSettings();                        // the user's budget / quality / guard (never in direct construction)
    return processor;
}
