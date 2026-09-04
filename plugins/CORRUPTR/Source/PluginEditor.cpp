#include "PluginEditor.h"
#include "BinaryData.h"

//==============================================================================
// Constructor - CRITICAL: Initialize in correct order (Relays -> WebView -> Attachments)
//==============================================================================

CORRUPTRAudioProcessorEditor::CORRUPTRAudioProcessorEditor(CORRUPTRAudioProcessor& p)
    : AudioProcessorEditor(&p), processorRef(p)
{
    // ------------------------------------------------------------------------
    // STEP 1: CREATE RELAYS (before WebView!) - Phase 5.1 scope, 14 parameters
    // ------------------------------------------------------------------------

    // --- Top Bar / Global ---
    qualityModeRelay = std::make_unique<juce::WebComboBoxRelay>("qualityMode");

    // --- Distortion ---
    graphBypassSaturationRelay = std::make_unique<juce::WebToggleButtonRelay>("graphBypassSaturation");
    distortionAlgorithmRelay = std::make_unique<juce::WebComboBoxRelay>("distortionAlgorithm");
    driveRelay = std::make_unique<juce::WebSliderRelay>("drive");
    foldRelay = std::make_unique<juce::WebSliderRelay>("fold");
    graphBypassWaveshaperRelay = std::make_unique<juce::WebToggleButtonRelay>("graphBypassWaveshaper");

    // --- Filter & EQ ---
    graphBypassFilterRelay = std::make_unique<juce::WebToggleButtonRelay>("graphBypassFilter");
    filterTypeRelay = std::make_unique<juce::WebComboBoxRelay>("filterType");
    filterCutoffRelay = std::make_unique<juce::WebSliderRelay>("filterCutoff");
    filterResonanceRelay = std::make_unique<juce::WebSliderRelay>("filterResonance");

    // --- Center Master (Output/Global) ---
    mixRelay = std::make_unique<juce::WebSliderRelay>("mix");
    outputLimiterStyleRelay = std::make_unique<juce::WebComboBoxRelay>("outputLimiterStyle");
    inputGainRelay = std::make_unique<juce::WebSliderRelay>("inputGain");
    outputGainRelay = std::make_unique<juce::WebSliderRelay>("outputGain");

    // --- Phase 5.2: Bitcrush ---
    graphBypassBitcrushRelay = std::make_unique<juce::WebToggleButtonRelay>("graphBypassBitcrush");
    bitDepthRelay = std::make_unique<juce::WebSliderRelay>("bitDepth");

    // --- Phase 5.2: Glitch ---
    graphBypassGlitchRelay = std::make_unique<juce::WebToggleButtonRelay>("graphBypassGlitch");
    glitchModeRelay = std::make_unique<juce::WebComboBoxRelay>("glitchMode");
    glitchBufferLengthRelay = std::make_unique<juce::WebComboBoxRelay>("glitchBufferLength");
    glitchProbabilityRelay = std::make_unique<juce::WebSliderRelay>("glitchProbability");

    // --- Phase 5.2: Rhythmic Sequencer ---
    sequencerEnabledRelay = std::make_unique<juce::WebToggleButtonRelay>("sequencerEnabled");
    sequencerRateRelay = std::make_unique<juce::WebComboBoxRelay>("sequencerRate");
    sequencerStepsRelay = std::make_unique<juce::WebComboBoxRelay>("sequencerSteps");

    // --- Phase 5.3: Macros (8) ---
    macroDamageRelay = std::make_unique<juce::WebSliderRelay>("macroDamage");
    macroCrushRelay = std::make_unique<juce::WebSliderRelay>("macroCrush");
    macroGlitchRelay = std::make_unique<juce::WebSliderRelay>("macroGlitch");
    macroChaosRelay = std::make_unique<juce::WebSliderRelay>("macroChaos");
    macroRhythmRelay = std::make_unique<juce::WebSliderRelay>("macroRhythm");
    macroMovementRelay = std::make_unique<juce::WebSliderRelay>("macroMovement");
    macroWidthRelay = std::make_unique<juce::WebSliderRelay>("macroWidth");
    macroMixRelay = std::make_unique<juce::WebSliderRelay>("macroMix");

    // --- Phase 5.3: Modulation Matrix module enable + 4 LFOs ---
    modMatrixEnabledRelay = std::make_unique<juce::WebToggleButtonRelay>("modMatrixEnabled");
    lfo1RateRelay = std::make_unique<juce::WebSliderRelay>("lfo1Rate");
    lfo1ShapeRelay = std::make_unique<juce::WebComboBoxRelay>("lfo1Shape");
    lfo1SyncRelay = std::make_unique<juce::WebToggleButtonRelay>("lfo1Sync");
    lfo2RateRelay = std::make_unique<juce::WebSliderRelay>("lfo2Rate");
    lfo2ShapeRelay = std::make_unique<juce::WebComboBoxRelay>("lfo2Shape");
    lfo2SyncRelay = std::make_unique<juce::WebToggleButtonRelay>("lfo2Sync");
    lfo3RateRelay = std::make_unique<juce::WebSliderRelay>("lfo3Rate");
    lfo3ShapeRelay = std::make_unique<juce::WebComboBoxRelay>("lfo3Shape");
    lfo3SyncRelay = std::make_unique<juce::WebToggleButtonRelay>("lfo3Sync");
    lfo4RateRelay = std::make_unique<juce::WebSliderRelay>("lfo4Rate");
    lfo4ShapeRelay = std::make_unique<juce::WebComboBoxRelay>("lfo4Shape");
    lfo4SyncRelay = std::make_unique<juce::WebToggleButtonRelay>("lfo4Sync");

    // --- Phase 5.3: Mod Matrix slots (8 x {Source,Destination,Amount,Enable}) ---
    modSlot1SourceRelay = std::make_unique<juce::WebComboBoxRelay>("modSlot1Source");
    modSlot1DestinationRelay = std::make_unique<juce::WebComboBoxRelay>("modSlot1Destination");
    modSlot1AmountRelay = std::make_unique<juce::WebSliderRelay>("modSlot1Amount");
    modSlot1EnableRelay = std::make_unique<juce::WebToggleButtonRelay>("modSlot1Enable");
    modSlot2SourceRelay = std::make_unique<juce::WebComboBoxRelay>("modSlot2Source");
    modSlot2DestinationRelay = std::make_unique<juce::WebComboBoxRelay>("modSlot2Destination");
    modSlot2AmountRelay = std::make_unique<juce::WebSliderRelay>("modSlot2Amount");
    modSlot2EnableRelay = std::make_unique<juce::WebToggleButtonRelay>("modSlot2Enable");
    modSlot3SourceRelay = std::make_unique<juce::WebComboBoxRelay>("modSlot3Source");
    modSlot3DestinationRelay = std::make_unique<juce::WebComboBoxRelay>("modSlot3Destination");
    modSlot3AmountRelay = std::make_unique<juce::WebSliderRelay>("modSlot3Amount");
    modSlot3EnableRelay = std::make_unique<juce::WebToggleButtonRelay>("modSlot3Enable");
    modSlot4SourceRelay = std::make_unique<juce::WebComboBoxRelay>("modSlot4Source");
    modSlot4DestinationRelay = std::make_unique<juce::WebComboBoxRelay>("modSlot4Destination");
    modSlot4AmountRelay = std::make_unique<juce::WebSliderRelay>("modSlot4Amount");
    modSlot4EnableRelay = std::make_unique<juce::WebToggleButtonRelay>("modSlot4Enable");
    modSlot5SourceRelay = std::make_unique<juce::WebComboBoxRelay>("modSlot5Source");
    modSlot5DestinationRelay = std::make_unique<juce::WebComboBoxRelay>("modSlot5Destination");
    modSlot5AmountRelay = std::make_unique<juce::WebSliderRelay>("modSlot5Amount");
    modSlot5EnableRelay = std::make_unique<juce::WebToggleButtonRelay>("modSlot5Enable");
    modSlot6SourceRelay = std::make_unique<juce::WebComboBoxRelay>("modSlot6Source");
    modSlot6DestinationRelay = std::make_unique<juce::WebComboBoxRelay>("modSlot6Destination");
    modSlot6AmountRelay = std::make_unique<juce::WebSliderRelay>("modSlot6Amount");
    modSlot6EnableRelay = std::make_unique<juce::WebToggleButtonRelay>("modSlot6Enable");
    modSlot7SourceRelay = std::make_unique<juce::WebComboBoxRelay>("modSlot7Source");
    modSlot7DestinationRelay = std::make_unique<juce::WebComboBoxRelay>("modSlot7Destination");
    modSlot7AmountRelay = std::make_unique<juce::WebSliderRelay>("modSlot7Amount");
    modSlot7EnableRelay = std::make_unique<juce::WebToggleButtonRelay>("modSlot7Enable");
    modSlot8SourceRelay = std::make_unique<juce::WebComboBoxRelay>("modSlot8Source");
    modSlot8DestinationRelay = std::make_unique<juce::WebComboBoxRelay>("modSlot8Destination");
    modSlot8AmountRelay = std::make_unique<juce::WebSliderRelay>("modSlot8Amount");
    modSlot8EnableRelay = std::make_unique<juce::WebToggleButtonRelay>("modSlot8Enable");

    // --- Phase 5.4: XY Performance Pad (2) ---
    xyPadXRelay = std::make_unique<juce::WebSliderRelay>("xyPadX");
    xyPadYRelay = std::make_unique<juce::WebSliderRelay>("xyPadY");

    // --- Phase 5.4: Performance Mode triggers (7, momentary) ---
    performanceKillRelay = std::make_unique<juce::WebToggleButtonRelay>("performanceKill");
    performanceGlitchRelay = std::make_unique<juce::WebToggleButtonRelay>("performanceGlitch");
    performanceDestroyRelay = std::make_unique<juce::WebToggleButtonRelay>("performanceDestroy");
    performanceFreezeRelay = std::make_unique<juce::WebToggleButtonRelay>("performanceFreeze");
    performanceReverseRelay = std::make_unique<juce::WebToggleButtonRelay>("performanceReverse");
    performanceStutterRelay = std::make_unique<juce::WebToggleButtonRelay>("performanceStutter");
    performanceChaosRelay = std::make_unique<juce::WebToggleButtonRelay>("performanceChaos");

    // ------------------------------------------------------------------------
    // STEP 2: CREATE WEBVIEW (with relay options + Phase 5.2's sequencer
    // pattern-data native-function bridge - see PluginEditor.h's "PHASE 5.2
    // ADDITIONS" doc comment for the full design)
    // ------------------------------------------------------------------------
    webView = std::make_unique<juce::WebBrowserComponent>(
        juce::WebBrowserComponent::Options{}
            .withNativeIntegrationEnabled()
            .withResourceProvider([this](const auto& url) { return getResource(url); })
            .withKeepPageLoadedWhenBrowserIsHidden()

            .withOptionsFrom(*qualityModeRelay)
            .withOptionsFrom(*graphBypassSaturationRelay)
            .withOptionsFrom(*distortionAlgorithmRelay)
            .withOptionsFrom(*driveRelay)
            .withOptionsFrom(*foldRelay)
            .withOptionsFrom(*graphBypassWaveshaperRelay)
            .withOptionsFrom(*graphBypassFilterRelay)
            .withOptionsFrom(*filterTypeRelay)
            .withOptionsFrom(*filterCutoffRelay)
            .withOptionsFrom(*filterResonanceRelay)
            .withOptionsFrom(*mixRelay)
            .withOptionsFrom(*outputLimiterStyleRelay)
            .withOptionsFrom(*inputGainRelay)
            .withOptionsFrom(*outputGainRelay)
            .withOptionsFrom(*graphBypassBitcrushRelay)
            .withOptionsFrom(*bitDepthRelay)
            .withOptionsFrom(*graphBypassGlitchRelay)
            .withOptionsFrom(*glitchModeRelay)
            .withOptionsFrom(*glitchBufferLengthRelay)
            .withOptionsFrom(*glitchProbabilityRelay)
            .withOptionsFrom(*sequencerEnabledRelay)
            .withOptionsFrom(*sequencerRateRelay)
            .withOptionsFrom(*sequencerStepsRelay)

            .withOptionsFrom(*macroDamageRelay)
            .withOptionsFrom(*macroCrushRelay)
            .withOptionsFrom(*macroGlitchRelay)
            .withOptionsFrom(*macroChaosRelay)
            .withOptionsFrom(*macroRhythmRelay)
            .withOptionsFrom(*macroMovementRelay)
            .withOptionsFrom(*macroWidthRelay)
            .withOptionsFrom(*macroMixRelay)

            .withOptionsFrom(*modMatrixEnabledRelay)
            .withOptionsFrom(*lfo1RateRelay)
            .withOptionsFrom(*lfo1ShapeRelay)
            .withOptionsFrom(*lfo1SyncRelay)
            .withOptionsFrom(*lfo2RateRelay)
            .withOptionsFrom(*lfo2ShapeRelay)
            .withOptionsFrom(*lfo2SyncRelay)
            .withOptionsFrom(*lfo3RateRelay)
            .withOptionsFrom(*lfo3ShapeRelay)
            .withOptionsFrom(*lfo3SyncRelay)
            .withOptionsFrom(*lfo4RateRelay)
            .withOptionsFrom(*lfo4ShapeRelay)
            .withOptionsFrom(*lfo4SyncRelay)

            .withOptionsFrom(*modSlot1SourceRelay)
            .withOptionsFrom(*modSlot1DestinationRelay)
            .withOptionsFrom(*modSlot1AmountRelay)
            .withOptionsFrom(*modSlot1EnableRelay)
            .withOptionsFrom(*modSlot2SourceRelay)
            .withOptionsFrom(*modSlot2DestinationRelay)
            .withOptionsFrom(*modSlot2AmountRelay)
            .withOptionsFrom(*modSlot2EnableRelay)
            .withOptionsFrom(*modSlot3SourceRelay)
            .withOptionsFrom(*modSlot3DestinationRelay)
            .withOptionsFrom(*modSlot3AmountRelay)
            .withOptionsFrom(*modSlot3EnableRelay)
            .withOptionsFrom(*modSlot4SourceRelay)
            .withOptionsFrom(*modSlot4DestinationRelay)
            .withOptionsFrom(*modSlot4AmountRelay)
            .withOptionsFrom(*modSlot4EnableRelay)
            .withOptionsFrom(*modSlot5SourceRelay)
            .withOptionsFrom(*modSlot5DestinationRelay)
            .withOptionsFrom(*modSlot5AmountRelay)
            .withOptionsFrom(*modSlot5EnableRelay)
            .withOptionsFrom(*modSlot6SourceRelay)
            .withOptionsFrom(*modSlot6DestinationRelay)
            .withOptionsFrom(*modSlot6AmountRelay)
            .withOptionsFrom(*modSlot6EnableRelay)
            .withOptionsFrom(*modSlot7SourceRelay)
            .withOptionsFrom(*modSlot7DestinationRelay)
            .withOptionsFrom(*modSlot7AmountRelay)
            .withOptionsFrom(*modSlot7EnableRelay)
            .withOptionsFrom(*modSlot8SourceRelay)
            .withOptionsFrom(*modSlot8DestinationRelay)
            .withOptionsFrom(*modSlot8AmountRelay)
            .withOptionsFrom(*modSlot8EnableRelay)

            .withOptionsFrom(*xyPadXRelay)
            .withOptionsFrom(*xyPadYRelay)

            .withOptionsFrom(*performanceKillRelay)
            .withOptionsFrom(*performanceGlitchRelay)
            .withOptionsFrom(*performanceDestroyRelay)
            .withOptionsFrom(*performanceFreezeRelay)
            .withOptionsFrom(*performanceReverseRelay)
            .withOptionsFrom(*performanceStutterRelay)
            .withOptionsFrom(*performanceChaosRelay)

            // --- Phase 5.2: Rhythmic Sequencer pattern-data bridge (custom
            // state, NOT an APVTS parameter - cannot use a Relay/Attachment
            // pair, see PluginEditor.h's top doc comment for the full design) ---
            .withNativeFunction("sequencerRequestPattern",
                [this](const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion completion)
                {
                    emitSequencerPatternChanged();
                    completion(true);
                })
            .withNativeFunction("sequencerSetStep",
                [this](const juce::Array<juce::var>& args, juce::WebBrowserComponent::NativeFunctionCompletion completion)
                {
                    if (args.size() >= 3)
                    {
                        const int lane = static_cast<int>(args[0]);
                        const int step = static_cast<int>(args[1]);
                        const float value = static_cast<float>(args[2]);
                        // setStep() alone does NOT publish (RhythmicSequencer.h's own
                        // doc comment recommends batching many setStep() calls into one
                        // publishSnapshot() per gesture) - this bridge deliberately
                        // publishes on EVERY call instead, for immediate audible
                        // feedback while drag-editing a step. publishSnapshot() is
                        // real-time-safe by design (fixed-size struct copy + one
                        // atomic store, no allocation), and this call site is only
                        // ever reached from human mouse interaction (bounded to a few
                        // hundred Hz at most, nowhere near audio-thread rates), so the
                        // extra publish-per-cell cost is negligible.
                        processorRef.getRhythmicSequencer().setStep(lane, step, value);
                        processorRef.getRhythmicSequencer().publishSnapshot();
                    }
                    completion(true);
                })
            .withNativeFunction("sequencerPatternOp",
                [this](const juce::Array<juce::var>& args, juce::WebBrowserComponent::NativeFunctionCompletion completion)
                {
                    if (args.size() >= 1)
                    {
                        const juce::String op = args[0].toString();
                        const int numSteps = getCurrentSequencerNumSteps();
                        auto& seq = processorRef.getRhythmicSequencer();

                        // Only "random"/"mutate"/"reverse"/"mirror"/"shift"/"clear" have
                        // markup buttons in index.html's Sequencer card this phase
                        // (matching v5-ui.html's 6 action buttons) - "half"/"double"/
                        // "syncopate"/"humanize" are wired here anyway (all 10 of
                        // RhythmicSequencer's opXxx() operations, per its own API) so a
                        // future mockup iteration can add buttons for them with zero
                        // C++ changes, consistent with this phase's "extend, don't
                        // redesign" markup constraint.
                        if (op == "random")         seq.opRandom(numSteps);
                        else if (op == "mutate")    seq.opMutate(numSteps);
                        else if (op == "reverse")   seq.opReverse(numSteps);
                        else if (op == "mirror")    seq.opMirror(numSteps);
                        else if (op == "shift")     seq.opShift(numSteps, 1); // default +1 step; no shift-amount control exists in the mockup
                        else if (op == "half")      seq.opHalf(numSteps);
                        else if (op == "double")    seq.opDouble(numSteps);
                        else if (op == "syncopate") seq.opSyncopate(numSteps);
                        else if (op == "humanize")  seq.opHumanize(numSteps);
                        else if (op == "clear")     seq.opClear(numSteps);
                        // unrecognised op name: no-op (still falls through to the
                        // refresh emit below, harmless - JS will just re-render the
                        // pattern unchanged).
                    }
                    // Every opXxx() call above already published internally - this
                    // event tells the JS side (which cannot predict the resulting
                    // values) to re-fetch/re-render.
                    emitSequencerPatternChanged();
                    completion(true);
                })
    );

    // ------------------------------------------------------------------------
    // STEP 3: CREATE PARAMETER ATTACHMENTS (after WebView!) - 14 total
    // JUCE 8 requires the 3-parameter constructor (parameter, relay, undoManager)
    // ------------------------------------------------------------------------

    // --- Top Bar / Global ---
    qualityModeAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("qualityMode"), *qualityModeRelay, nullptr);

    // --- Distortion ---
    graphBypassSaturationAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("graphBypassSaturation"), *graphBypassSaturationRelay, nullptr);
    distortionAlgorithmAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("distortionAlgorithm"), *distortionAlgorithmRelay, nullptr);
    driveAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("drive"), *driveRelay, nullptr);
    foldAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("fold"), *foldRelay, nullptr);
    graphBypassWaveshaperAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("graphBypassWaveshaper"), *graphBypassWaveshaperRelay, nullptr);

    // --- Filter & EQ ---
    graphBypassFilterAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("graphBypassFilter"), *graphBypassFilterRelay, nullptr);
    filterTypeAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("filterType"), *filterTypeRelay, nullptr);
    filterCutoffAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("filterCutoff"), *filterCutoffRelay, nullptr);
    filterResonanceAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("filterResonance"), *filterResonanceRelay, nullptr);

    // --- Center Master (Output/Global) ---
    mixAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("mix"), *mixRelay, nullptr);
    outputLimiterStyleAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("outputLimiterStyle"), *outputLimiterStyleRelay, nullptr);
    inputGainAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("inputGain"), *inputGainRelay, nullptr);
    outputGainAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("outputGain"), *outputGainRelay, nullptr);

    // --- Phase 5.2: Bitcrush ---
    graphBypassBitcrushAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("graphBypassBitcrush"), *graphBypassBitcrushRelay, nullptr);
    bitDepthAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("bitDepth"), *bitDepthRelay, nullptr);

    // --- Phase 5.2: Glitch ---
    graphBypassGlitchAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("graphBypassGlitch"), *graphBypassGlitchRelay, nullptr);
    glitchModeAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("glitchMode"), *glitchModeRelay, nullptr);
    glitchBufferLengthAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("glitchBufferLength"), *glitchBufferLengthRelay, nullptr);
    glitchProbabilityAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("glitchProbability"), *glitchProbabilityRelay, nullptr);

    // --- Phase 5.2: Rhythmic Sequencer ---
    sequencerEnabledAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("sequencerEnabled"), *sequencerEnabledRelay, nullptr);
    sequencerRateAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("sequencerRate"), *sequencerRateRelay, nullptr);
    sequencerStepsAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("sequencerSteps"), *sequencerStepsRelay, nullptr);

    // --- Phase 5.3: Macros (8) ---
    macroDamageAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("macroDamage"), *macroDamageRelay, nullptr);
    macroCrushAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("macroCrush"), *macroCrushRelay, nullptr);
    macroGlitchAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("macroGlitch"), *macroGlitchRelay, nullptr);
    macroChaosAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("macroChaos"), *macroChaosRelay, nullptr);
    macroRhythmAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("macroRhythm"), *macroRhythmRelay, nullptr);
    macroMovementAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("macroMovement"), *macroMovementRelay, nullptr);
    macroWidthAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("macroWidth"), *macroWidthRelay, nullptr);
    macroMixAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("macroMix"), *macroMixRelay, nullptr);

    // --- Phase 5.3: Modulation Matrix module enable + 4 LFOs ---
    modMatrixEnabledAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modMatrixEnabled"), *modMatrixEnabledRelay, nullptr);
    lfo1RateAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("lfo1Rate"), *lfo1RateRelay, nullptr);
    lfo1ShapeAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("lfo1Shape"), *lfo1ShapeRelay, nullptr);
    lfo1SyncAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("lfo1Sync"), *lfo1SyncRelay, nullptr);
    lfo2RateAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("lfo2Rate"), *lfo2RateRelay, nullptr);
    lfo2ShapeAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("lfo2Shape"), *lfo2ShapeRelay, nullptr);
    lfo2SyncAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("lfo2Sync"), *lfo2SyncRelay, nullptr);
    lfo3RateAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("lfo3Rate"), *lfo3RateRelay, nullptr);
    lfo3ShapeAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("lfo3Shape"), *lfo3ShapeRelay, nullptr);
    lfo3SyncAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("lfo3Sync"), *lfo3SyncRelay, nullptr);
    lfo4RateAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("lfo4Rate"), *lfo4RateRelay, nullptr);
    lfo4ShapeAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("lfo4Shape"), *lfo4ShapeRelay, nullptr);
    lfo4SyncAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("lfo4Sync"), *lfo4SyncRelay, nullptr);

    // --- Phase 5.3: Mod Matrix slots (8 x {Source,Destination,Amount,Enable}) ---
    modSlot1SourceAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot1Source"), *modSlot1SourceRelay, nullptr);
    modSlot1DestinationAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot1Destination"), *modSlot1DestinationRelay, nullptr);
    modSlot1AmountAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot1Amount"), *modSlot1AmountRelay, nullptr);
    modSlot1EnableAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot1Enable"), *modSlot1EnableRelay, nullptr);

    modSlot2SourceAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot2Source"), *modSlot2SourceRelay, nullptr);
    modSlot2DestinationAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot2Destination"), *modSlot2DestinationRelay, nullptr);
    modSlot2AmountAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot2Amount"), *modSlot2AmountRelay, nullptr);
    modSlot2EnableAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot2Enable"), *modSlot2EnableRelay, nullptr);

    modSlot3SourceAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot3Source"), *modSlot3SourceRelay, nullptr);
    modSlot3DestinationAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot3Destination"), *modSlot3DestinationRelay, nullptr);
    modSlot3AmountAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot3Amount"), *modSlot3AmountRelay, nullptr);
    modSlot3EnableAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot3Enable"), *modSlot3EnableRelay, nullptr);

    modSlot4SourceAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot4Source"), *modSlot4SourceRelay, nullptr);
    modSlot4DestinationAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot4Destination"), *modSlot4DestinationRelay, nullptr);
    modSlot4AmountAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot4Amount"), *modSlot4AmountRelay, nullptr);
    modSlot4EnableAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot4Enable"), *modSlot4EnableRelay, nullptr);

    modSlot5SourceAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot5Source"), *modSlot5SourceRelay, nullptr);
    modSlot5DestinationAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot5Destination"), *modSlot5DestinationRelay, nullptr);
    modSlot5AmountAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot5Amount"), *modSlot5AmountRelay, nullptr);
    modSlot5EnableAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot5Enable"), *modSlot5EnableRelay, nullptr);

    modSlot6SourceAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot6Source"), *modSlot6SourceRelay, nullptr);
    modSlot6DestinationAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot6Destination"), *modSlot6DestinationRelay, nullptr);
    modSlot6AmountAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot6Amount"), *modSlot6AmountRelay, nullptr);
    modSlot6EnableAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot6Enable"), *modSlot6EnableRelay, nullptr);

    modSlot7SourceAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot7Source"), *modSlot7SourceRelay, nullptr);
    modSlot7DestinationAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot7Destination"), *modSlot7DestinationRelay, nullptr);
    modSlot7AmountAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot7Amount"), *modSlot7AmountRelay, nullptr);
    modSlot7EnableAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot7Enable"), *modSlot7EnableRelay, nullptr);

    modSlot8SourceAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot8Source"), *modSlot8SourceRelay, nullptr);
    modSlot8DestinationAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot8Destination"), *modSlot8DestinationRelay, nullptr);
    modSlot8AmountAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot8Amount"), *modSlot8AmountRelay, nullptr);
    modSlot8EnableAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("modSlot8Enable"), *modSlot8EnableRelay, nullptr);

    // --- Phase 5.4: XY Performance Pad (2) ---
    xyPadXAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("xyPadX"), *xyPadXRelay, nullptr);
    xyPadYAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("xyPadY"), *xyPadYRelay, nullptr);

    // --- Phase 5.4: Performance Mode triggers (7, momentary) ---
    performanceKillAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("performanceKill"), *performanceKillRelay, nullptr);
    performanceGlitchAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("performanceGlitch"), *performanceGlitchRelay, nullptr);
    performanceDestroyAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("performanceDestroy"), *performanceDestroyRelay, nullptr);
    performanceFreezeAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("performanceFreeze"), *performanceFreezeRelay, nullptr);
    performanceReverseAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("performanceReverse"), *performanceReverseRelay, nullptr);
    performanceStutterAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("performanceStutter"), *performanceStutterRelay, nullptr);
    performanceChaosAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("performanceChaos"), *performanceChaosRelay, nullptr);

    // ------------------------------------------------------------------------
    // WEBVIEW SETUP
    // ------------------------------------------------------------------------

    // Cache-bust every launch (Pattern #24) - avoids WKWebView serving a
    // stale snapshot of a previous load of this exact URL during active
    // development where HTML/JS content changes between builds.
    const juce::String cacheBuster = "?t=" + juce::String(juce::Time::getMillisecondCounterHiRes(), 0);
    webView->goToURL(juce::WebBrowserComponent::getResourceProviderRoot() + cacheBuster);

    addAndMakeVisible(*webView);

    // Window size from v5-ui.yaml: width 1200, viewport_height 800 (fixed
    // JUCE editor size). Total scrollable content is 1650px tall and scrolls
    // INTERNALLY within the WebView's .plugin-frame div - do NOT setSize(1200,1650).
    setSize(1200, 800);
    setResizable(false, false);

    // Phase 5.6: start the ~30Hz visualization push (meters, sequencer
    // playhead, modulation-range indicators - see this class's header doc
    // comment, "PHASE 5.6 ADDITIONS"). Started LAST, after webView/relays/
    // attachments all exist, so the very first timerCallback() (which can
    // fire as soon as ~33ms from now) never races construction.
    startTimerHz(30);
}

//==============================================================================
// Destructor
//==============================================================================

CORRUPTRAudioProcessorEditor::~CORRUPTRAudioProcessorEditor()
{
    // Stop the Timer FIRST, before any member (webView in particular) starts
    // tearing down - juce::Timer callbacks can in principle still fire
    // during destruction otherwise. Explicit stopTimer() (rather than
    // relying on ~Timer()'s own automatic stop, which only runs AFTER this
    // derived class's members are already gone) guarantees timerCallback()
    // can never touch a partially-destroyed webView.
    stopTimer();

    // Remaining members are automatically destroyed in reverse order of
    // declaration: Attachments -> WebView -> Relays. No further manual
    // cleanup needed.
}

//==============================================================================
// AudioProcessorEditor Overrides
//==============================================================================

void CORRUPTRAudioProcessorEditor::paint(juce::Graphics& g)
{
    // WebView fills the entire editor, so no custom painting needed.
    g.fillAll(juce::Colours::black);
}

void CORRUPTRAudioProcessorEditor::resized()
{
    if (webView)
        webView->setBounds(getLocalBounds());
}

//==============================================================================
// Resource Provider (JUCE 8 Required Pattern - explicit URL mapping, Pattern #8)
//==============================================================================

std::optional<juce::WebBrowserComponent::Resource> CORRUPTRAudioProcessorEditor::getResource(
    const juce::String& url)
{
    auto makeVector = [](const char* data, int size)
    {
        return std::vector<std::byte>(
            reinterpret_cast<const std::byte*>(data),
            reinterpret_cast<const std::byte*>(data) + size);
    };

    // Strip query string (cache-buster) before matching (Pattern #24)
    const juce::String path = url.upToFirstOccurrenceOf("?", false, false);

    if (path == "/" || path == "/index.html")
    {
        return juce::WebBrowserComponent::Resource {
            makeVector(BinaryData::index_html, BinaryData::index_htmlSize),
            juce::String("text/html")
        };
    }

    if (path == "/js/juce/index.js")
    {
        return juce::WebBrowserComponent::Resource {
            makeVector(BinaryData::index_js, BinaryData::index_jsSize),
            juce::String("application/javascript")
        };
    }

    if (path == "/js/juce/check_native_interop.js")
    {
        return juce::WebBrowserComponent::Resource {
            makeVector(BinaryData::check_native_interop_js, BinaryData::check_native_interop_jsSize),
            juce::String("application/javascript")
        };
    }

    return std::nullopt;
}

//==============================================================================
// Phase 5.2: Rhythmic Sequencer pattern-data bridge helpers
// (see PluginEditor.h's "PHASE 5.2 ADDITIONS" doc comment for the full design)
//==============================================================================

juce::var CORRUPTRAudioProcessorEditor::buildSequencerPatternVar() const
{
    auto* obj = new juce::DynamicObject();
    auto& seq = processorRef.getRhythmicSequencer();

    juce::Array<juce::var> lanesArray;
    for (int lane = 0; lane < RhythmicSequencer::kNumLanes; ++lane)
    {
        juce::Array<juce::var> laneArray;
        for (int step = 0; step < RhythmicSequencer::kMaxSteps; ++step)
            laneArray.add(seq.getStep(lane, step));
        lanesArray.add(juce::var(laneArray));
    }
    obj->setProperty("lanes", lanesArray);

    return juce::var(obj);
}

void CORRUPTRAudioProcessorEditor::emitSequencerPatternChanged()
{
    // Defensive null check only - by the time any JS-triggered native
    // function callback can run, the page has already loaded, meaning the
    // constructor (which assigns webView) has long since returned.
    if (webView)
        webView->emitEventIfBrowserIsVisible("sequencerPatternChanged", buildSequencerPatternVar());
}

int CORRUPTRAudioProcessorEditor::getCurrentSequencerNumSteps() const
{
    // Same formula as CORRUPTRAudioProcessor::updateSequencerStepAndContributions()'s
    // own sequencerNumSteps resolution (PluginProcessor.cpp): AudioParameterChoice
    // raw values are stored as their integer index (0="16"/1="32"), NOT a
    // normalised 0-1 value.
    if (auto* p = processorRef.getAPVTS().getRawParameterValue("sequencerSteps"))
        return (static_cast<int>(p->load()) == 1) ? 32 : 16;
    return 16;
}

//==============================================================================
// Phase 5.6: ~30Hz visualization push (see this class's header doc comment,
// "PHASE 5.6 ADDITIONS", for the full design).
//==============================================================================

juce::var CORRUPTRAudioProcessorEditor::buildVisUpdateVar() const
{
    auto* obj = new juce::DynamicObject();

    // Meters — linear peak (0..~few), converted to dB in JS (matches
    // v5-ui.yaml's `input_meter_topbar`/`output_meter_topbar` [-60, 6] dB
    // range declaration). Sent as raw linear peak, not pre-converted, so
    // the JS-side ballistic-motion loop (Pattern #20) owns all of the
    // dB-mapping/smoothing/interpolation math in one place.
    obj->setProperty("inputPeak", processorRef.getVisInputPeakLevel());
    obj->setProperty("outputPeak", processorRef.getVisOutputPeakLevel());

    // Sequencer playhead — mirrors the SAME per-block step resolution the
    // audio thread already computed (including the documented no-transport
    // freeze-at-step-0 fallback), plus the current step COUNT (16/32) so
    // the JS step-grid (which may be showing either) can correctly no-op
    // an out-of-range index rather than mis-highlighting.
    obj->setProperty("seqStep", processorRef.getVisSequencerStepIndex());
    obj->setProperty("seqNumSteps", getCurrentSequencerNumSteps());

    // Modulation-range indicators — 7 destinations with live DSP + a visible
    // bound control in v5-ui.html (see header doc comment for the full
    // scope list/rationale). Native units, matching each control's own
    // data-min/data-max/data-current-value space exactly (no normalisation
    // here — JS does the min/max mapping itself, consistent with how
    // bindKnobs()/bindFaders() already render these same controls).
    obj->setProperty("modDrive", processorRef.getVisModDriveDb());
    obj->setProperty("modFold", processorRef.getVisModFoldPct());
    obj->setProperty("modBitDepth", processorRef.getVisModBitDepth());
    obj->setProperty("modFilterCutoff", processorRef.getVisModFilterCutoffHz());
    obj->setProperty("modMix", processorRef.getVisModMixPct());
    obj->setProperty("modFeedbackAmount", processorRef.getVisModFeedbackAmountPct());
    obj->setProperty("modGlitchProbability", processorRef.getVisModGlitchProbabilityPct());

    return juce::var(obj);
}

void CORRUPTRAudioProcessorEditor::timerCallback()
{
    // Defensive null check only, matching emitSequencerPatternChanged()'s
    // identical precedent above - webView is always non-null by the time
    // this Timer's callbacks can fire (started at the very end of the
    // constructor, after webView is constructed), but the check costs
    // nothing and guards against any future reordering.
    if (webView)
        webView->emitEventIfBrowserIsVisible("visUpdate", buildVisUpdateVar());
}
