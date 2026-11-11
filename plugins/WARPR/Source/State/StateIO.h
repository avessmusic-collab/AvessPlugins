#pragma once
/*
  StateIO - plugin state container (skeleton; the sample blob and migration hooks arrive in D9).

  Container layout (little-endian, identical for getStateInformation and future .warpr files):
      u32  magic             = 'W' 'R' 'P' 'R'  (PluginIdentity::containerMagic, FROZEN)
      u32  containerVersion  = 1
      u32  xmlByteCount      ; followed by that many bytes of UTF-8 XML:
                             ;   APVTS tree (all 135 parameters) + <WARPR_UI_STATE .../> child
      u32  sampleBlobByteCount   ; 0 in Stage 1 (default wave / none); D9 defines the blob
      u8[] sample blob
  The UI-state child carries warprStateVersion (migration tag).  Unknown newer versions load what
  is understood and never crash; unreadable input leaves the current state untouched.
*/

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstddef>

#include "Backend/UiModel.h"

namespace warpr::state
{
    /** Serialises the APVTS parameters + UiModel into the container. */
    juce::MemoryBlock write (juce::AudioProcessorValueTreeState& apvts, const UiModel& model);

    /** Parses the container.  Returns false (state untouched) if the magic/lengths/XML are invalid. */
    bool readContainer (const void* data, std::size_t numBytes,
                        juce::AudioProcessorValueTreeState& apvts, UiModel& modelOut);

    /** Applies an already-parsed XML state (legacy plain-XML states from getXmlFromBinary use this).
        Returns false (state untouched) if the root tag is not the APVTS state type. */
    bool readXml (const juce::XmlElement& xml,
                  juce::AudioProcessorValueTreeState& apvts, UiModel& modelOut);
}
