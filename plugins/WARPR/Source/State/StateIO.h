#pragma once
/*
  StateIO - plugin state container (final, phase D9).  Identical bytes for getStateInformation and .void preset files.

  Container layout (little-endian; identifiers come from PluginIdentity.h and are FROZEN):
      u32  magic             = 'W' 'R' 'P' 'R'  (identity::containerMagic)
      u32  containerVersion  = 1                (identity::containerVersion)
      u32  xmlByteCount      ; followed by that many bytes of UTF-8 XML:
                             ;   APVTS tree (all 135 parameters) + <WARPR_UI_STATE .../> child
                             ;   (LFO count / shapes, mod matrix, shaper curves, preset name, warprStateVersion)
      u32  sampleBlobByteCount   ; 0 = default wave / none; otherwise the blob described in SampleCodec.h
      u8[] sample blob           ; codec, channels, flags, sample rate, frames, name, payload (FLAC24 or float32)
      ...                        ; anything after the blob is ignored (room for later versions)

  Reading is all-or-nothing for everything that is not the sample:
    * a bad magic, a truncated header / XML / blob, unparseable or absurdly nested XML or a foreign XML root
      return false and leave the APVTS and the model UNTOUCHED ("defaults retained");
    * a complete blob that cannot be decoded (unknown codec, corrupt FLAC, inconsistent lengths) is not fatal:
      the parameters and UI state load and `SampleLoad::corruptBlob` reports that the default wave must be used.
  A container version above ours is read best-effort (the layout above is the contract for every version >= 1);
  version < 1 is rejected.  The UI-state child carries warprStateVersion; migrate() is the single place where
  older documents are upgraded (version 1 is current, so it is the identity today).
*/

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstddef>

#include "Backend/UiModel.h"
#include "DSP/SampleData.h"

namespace warpr::state
{
    inline constexpr std::size_t kHeaderBytes = 12;                  // magic + containerVersion + xmlByteCount

    /** What the container said about the sample. */
    struct SampleLoad
    {
        SampleData::Ptr sample;            // non-null: an embedded sample decoded successfully
        bool hadBlob = false;              // sampleBlobByteCount > 0
        bool corruptBlob = false;          // a blob was present but unusable -> default wave

        bool embedded() const noexcept     { return sample != nullptr; }
    };

    /** Result of a full state load at processor level (container or legacy XML, sample publication, model adoption). */
    enum class LoadOutcome { rejected, loaded, loadedDefaultSampleAfterCorruptBlob };

    /** Serialises the APVTS parameters + UiModel into the container with NO embedded sample (blob byte count 0). */
    juce::MemoryBlock write (juce::AudioProcessorValueTreeState& apvts, const UiModel& model);

    /** Same, with the sample blob from samplecodec::encode() (an empty block writes blob byte count 0). */
    juce::MemoryBlock write (juce::AudioProcessorValueTreeState& apvts, const UiModel& model, const juce::MemoryBlock& sampleBlob);

    /** Parses the container.  Returns false (APVTS and model untouched) if the magic / lengths / XML are invalid.
        When `sampleOut` is non-null the blob is decoded into it (synchronously: FLAC decode + mip build). */
    bool readContainer (const void* data, std::size_t numBytes,
                        juce::AudioProcessorValueTreeState& apvts, UiModel& modelOut,
                        SampleLoad* sampleOut = nullptr);

    /** Applies an already-parsed XML state (legacy plain-XML states from getXmlFromBinary use this).
        Returns false (state untouched) if the root tag is not the APVTS state type. */
    bool readXml (const juce::XmlElement& xml,
                  juce::AudioProcessorValueTreeState& apvts, UiModel& modelOut);

    /** Migration hook: upgrades a parsed state tree from `fromUiStateVersion` to the current layout (no-op at v1;
        unknown newer versions are left as they are - the readers take what they understand). */
    void migrate (juce::ValueTree& tree, int fromUiStateVersion);

    /** True when the XML text nests deeper than `limit` elements (guards the recursive XML parser against hostile input). */
    bool xmlNestsTooDeeply (const char* utf8, std::size_t numBytes, int limit = 256) noexcept;
}
