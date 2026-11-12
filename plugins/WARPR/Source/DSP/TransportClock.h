#pragma once
/*
  TransportClock - host ppq / tempo for the tempo-synced LFOs (architecture C11 "TransportClock"; phase D8).

  The processor reads juce::AudioPlayHead::getPosition() ONCE per block into a TransportInfo and calls beginBlock().  The
  clock then answers ppqAt(sampleOffset) for any offset inside the block (the synth asks at the start and the end of every
  32-sample chunk), so a block split at MIDI timestamps stays on the same line.

  Policy
    host playing and reporting ppq : blockPpq = host ppq (every block, so jumps, loops and stop/start RELOCK at once)
    host stopped, or no playhead   : an internal clock keeps running at the last known tempo (default 120 bpm), continuing from
                                     the last ppq it had; when the host starts again the next block relocks to the host.
    tempo                          : the host bpm when reported (clamped 1..999), else the last known value.
    bar                            : timeSigNumerator * 4 / timeSigDenominator beats (default 4/4 = 4 beats).
  Everything is double precision; ppq(offset) = blockPpq + offset * bpm / (60 fs).
*/

#include <algorithm>

namespace warpr
{
    struct TransportInfo
    {
        bool   hasPpq = false;
        double ppq = 0.0;
        bool   hasBpm = false;
        double bpm = 120.0;
        bool   playing = false;
        int    timeSigNumerator = 0;        // 0 = unknown
        int    timeSigDenominator = 0;
    };

    class TransportClock
    {
    public:
        void prepare (double sampleRate) noexcept
        {
            fs = sampleRate > 0.0 ? sampleRate : 44100.0;
            reset();
        }

        void reset() noexcept
        {
            bpmNow = 120.0;
            barBeatsNow = 4.0;
            blockPpq = 0.0;
            internalPpq = 0.0;
            ppqPerSample = bpmNow / (60.0 * fs);
            initialised = false;
            playingNow = false;
        }

        /** Once per block, before the first ppqAt(). */
        void beginBlock (const TransportInfo& host, int numSamples) noexcept
        {
            if (host.hasBpm && host.bpm >= 1.0)
                bpmNow = std::min (999.0, host.bpm);

            if (host.timeSigNumerator > 0 && host.timeSigDenominator > 0)
                barBeatsNow = (double) host.timeSigNumerator * 4.0 / (double) host.timeSigDenominator;

            ppqPerSample = bpmNow / (60.0 * fs);

            if (! initialised)
            {
                internalPpq = host.hasPpq ? host.ppq : 0.0;
                initialised = true;
            }

            playingNow = host.hasPpq && host.playing;

            if (playingNow)
            {
                blockPpq = host.ppq;                                   // relock to the host
                internalPpq = host.ppq + (double) numSamples * ppqPerSample;
            }
            else
            {
                blockPpq = internalPpq;                                // internal clock keeps running
                internalPpq += (double) numSamples * ppqPerSample;
            }
        }

        double ppqAt (int sampleOffset) const noexcept { return blockPpq + (double) sampleOffset * ppqPerSample; }
        double bpm() const noexcept                    { return bpmNow; }
        double barBeats() const noexcept               { return barBeatsNow; }
        bool   isHostPlaying() const noexcept          { return playingNow; }
        double sampleRate() const noexcept             { return fs; }

    private:
        double fs = 44100.0;
        double bpmNow = 120.0, barBeatsNow = 4.0;
        double blockPpq = 0.0, internalPpq = 0.0, ppqPerSample = 0.0;
        bool initialised = false, playingNow = false;
    };
}
