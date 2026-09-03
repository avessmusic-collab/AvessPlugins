#include "Presets/PresetManager.h"
#include "Parameters/ParameterIDs.h"

#include <array>
#include <cmath>

namespace kickr
{
    namespace
    {
        namespace pid = id;

        struct Override { const char* id; float value; };

        struct Factory
        {
            const char*                   name;
            std::initializer_list<Override> overrides;
        };

        // 47 factory patches (17 original + 30 added 2026-09-02 on user request: realistic
        // kits / 808s / hip hop / techno) — a small set of ID->value overrides on top of the
        // APVTS defaults. All synth-only. Tuned by ear intent; refine at repo Stage 17.
        // 2026-09-03 smoothing pass (user request: "make the presets smoother. more
        // realistic and similar to iconic drum machines"): softened the brightest clicks
        // (clickTone pulled down / clickTime lengthened for a rounder beater edge), eased
        // the harshest transientAttack/drive/character values, and moved the 808 group
        // closer to the real TR-808 (smaller+faster sweeps, darker quieter clicks, less
        // drive — the hardware is a bridged-T resonator: tiny thump into a pure decaying
        // sine). Deliberately-hard genre presets (Hardstyle / Hardcore / Schranz / Acid /
        // Minimal Tick / Distorted / Clicky) keep their aggression — that's their point.
        const std::array<Factory, 47> kFactory { {
            { "Clean",       { {pid::drive,0.05f},{pid::character,0.0f},{pid::tailLevel,0.10f},
                               {pid::bodyDecay,350.0f},{pid::pitchStart,3.0f},{pid::clickLevel,0.25f} } },
            { "House",       { {pid::fundamental,50.0f},{pid::pitchStart,2.6f},{pid::pitchTime,45.0f},
                               {pid::bodyDecay,300.0f},{pid::drive,0.18f},{pid::clickLevel,0.30f},
                               {pid::clickTone,2800.0f},{pid::tailLevel,0.15f} } },   // 909-in-a-house-track: warmer, rounder
            { "Techno",      { {pid::fundamental,52.0f},{pid::pitchStart,5.0f},{pid::pitchTime,40.0f},
                               {pid::bodyDecay,250.0f},{pid::drive,0.32f},{pid::character,0.15f},
                               {pid::clickLevel,0.40f},{pid::clickTone,4200.0f},{pid::tailLevel,0.20f},
                               {pid::tailLength,180.0f} } },
            { "Hard Techno", { {pid::fundamental,55.0f},{pid::pitchStart,7.0f},{pid::pitchTime,35.0f},
                               {pid::bodyDecay,200.0f},{pid::drive,0.55f},{pid::character,0.35f},
                               {pid::transientAttack,0.40f},{pid::clickLevel,0.50f},{pid::tailDrive,0.40f},
                               {pid::tailLength,220.0f} } },
            { "Hardstyle",   { {pid::fundamental,60.0f},{pid::pitchStart,9.0f},{pid::pitchTime,30.0f},
                               {pid::bodyDecay,180.0f},{pid::drive,0.70f},{pid::character,0.50f},
                               {pid::tailLevel,0.45f},{pid::tailLength,700.0f},{pid::tailTone,0.75f},
                               {pid::tailDrive,0.70f},{pid::clickLevel,0.45f} } },
            { "Hardcore",    { {pid::fundamental,65.0f},{pid::pitchStart,8.0f},{pid::pitchTime,25.0f},
                               {pid::bodyDecay,150.0f},{pid::drive,0.85f},{pid::character,0.65f},
                               {pid::transientAttack,0.60f},{pid::tailLevel,0.30f},{pid::tailDrive,0.60f},
                               {pid::clickLevel,0.55f},{pid::low,4.0f} } },
            { "Industrial",  { {pid::fundamental,48.0f},{pid::pitchStart,4.0f},{pid::pitchTime,60.0f},
                               {pid::bodyDecay,400.0f},{pid::drive,0.60f},{pid::character,0.55f},
                               {pid::noiseLevel,0.25f},{pid::noiseType,2.0f},{pid::noiseTone,0.60f},
                               {pid::tailLength,350.0f},{pid::tailDrive,0.50f} } },
            { "Sub Heavy",   { {pid::fundamental,40.0f},{pid::pitchStart,2.5f},{pid::pitchTime,70.0f},
                               {pid::bodyLevel,1.0f},{pid::bodyDecay,600.0f},{pid::subLevel,0.80f},
                               {pid::subFreq,35.0f},{pid::subDecay,500.0f},{pid::drive,0.10f},
                               {pid::clickLevel,0.15f},{pid::tailLevel,0.10f} } },
            { "Short",       { {pid::fundamental,55.0f},{pid::pitchStart,4.0f},{pid::pitchTime,30.0f},
                               {pid::bodyDecay,90.0f},{pid::subDecay,80.0f},{pid::tailLevel,0.05f},
                               {pid::clickLevel,0.50f},{pid::clickTime,2.5f} } },
            { "Long",        { {pid::fundamental,50.0f},{pid::pitchStart,3.0f},{pid::pitchTime,80.0f},
                               {pid::bodyDecay,900.0f},{pid::subDecay,700.0f},{pid::tailLevel,0.35f},
                               {pid::tailLength,1200.0f},{pid::tailTone,0.40f} } },
            { "Distorted",   { {pid::fundamental,55.0f},{pid::pitchStart,5.0f},{pid::drive,0.80f},
                               {pid::character,0.55f},{pid::driveMix,0.85f},{pid::bodyHarmonics,0.40f},
                               {pid::transientAttack,0.30f},{pid::clickLevel,0.40f} } },
            { "Clicky",      { {pid::fundamental,55.0f},{pid::pitchStart,4.0f},{pid::bodyDecay,160.0f},
                               {pid::clickLevel,0.75f},{pid::clickTone,8000.0f},{pid::clickPitch,7000.0f},
                               {pid::clickTime,1.5f},{pid::transientAttack,0.50f},{pid::tailLevel,0.08f} } },
            { "Punchy",      { {pid::fundamental,55.0f},{pid::pitchStart,5.0f},{pid::pitchTime,38.0f},
                               {pid::pitchCurve,0.85f},{pid::bodyDecay,240.0f},{pid::transientAttack,0.45f},
                               {pid::transientSustain,-0.10f},{pid::drive,0.30f},{pid::clickLevel,0.45f} } },
            { "Warehouse",   { {pid::fundamental,52.0f},{pid::pitchStart,5.0f},{pid::pitchTime,42.0f},
                               {pid::bodyDecay,280.0f},{pid::drive,0.40f},{pid::character,0.20f},
                               {pid::clickLevel,0.40f},{pid::clickTone,3800.0f},{pid::tailLevel,0.25f},
                               {pid::tailLength,240.0f},{pid::tailTone,0.45f},{pid::low,2.0f} } },
            { "EDM",         { {pid::fundamental,48.0f},{pid::pitchStart,4.0f},{pid::pitchTime,50.0f},
                               {pid::bodyDecay,320.0f},{pid::drive,0.30f},{pid::clickLevel,0.50f},
                               {pid::clickTone,4800.0f},{pid::tailLevel,0.20f},{pid::high,3.0f},
                               {pid::transientAttack,0.35f} } },
            { "Trap",        { {pid::fundamental,42.0f},{pid::pitchStart,3.0f},{pid::pitchTime,120.0f},
                               {pid::pitchCurve,0.60f},{pid::bodyDecay,500.0f},{pid::subLevel,0.70f},
                               {pid::subFreq,38.0f},{pid::subDecay,600.0f},{pid::drive,0.25f},
                               {pid::clickLevel,0.30f},{pid::tailLevel,0.15f} } },
            { "Cinematic",   { {pid::fundamental,38.0f},{pid::pitchStart,3.0f},{pid::pitchTime,90.0f},
                               {pid::bodyLevel,1.0f},{pid::bodyDecay,1200.0f},{pid::subLevel,0.60f},
                               {pid::subDecay,900.0f},{pid::tailLevel,0.40f},{pid::tailLength,1600.0f},
                               {pid::tailTone,0.35f},{pid::tailDrive,0.30f},{pid::drive,0.15f},
                               {pid::low,3.0f} } },
            // ---- 2026-09-02 (user request): 30 more — realistic kits, 808s, hip hop, techno ----
            // Realistic / acoustic-flavoured kits: small pitch sweep, beater click, a touch of
            // pink noise, gentle Morph for a non-sine attack, minimal tail.
            { "Acoustic Rock", { {pid::fundamental,62.0f},{pid::pitchStart,2.2f},{pid::pitchTime,30.0f},
                               {pid::bodyDecay,220.0f},{pid::bodyHarmonics,0.30f},{pid::morph,0.25f},
                               {pid::clickLevel,0.45f},{pid::clickTone,2600.0f},{pid::clickTime,5.0f},
                               {pid::noiseLevel,0.18f},{pid::noiseDecay,45.0f},{pid::noiseTone,0.55f},{pid::noiseType,1.0f},
                               {pid::tailLevel,0.08f},{pid::drive,0.15f},{pid::transientAttack,0.25f},{pid::low,2.0f} } },
            { "Acoustic Jazz", { {pid::fundamental,70.0f},{pid::pitchStart,1.8f},{pid::pitchTime,25.0f},
                               {pid::bodyLevel,0.85f},{pid::bodyDecay,160.0f},{pid::bodyHarmonics,0.20f},{pid::morph,0.20f},
                               {pid::clickLevel,0.30f},{pid::clickTone,2500.0f},{pid::clickTime,5.0f},{pid::noiseLevel,0.12f},{pid::noiseDecay,40.0f},
                               {pid::noiseType,1.0f},{pid::tailLevel,0.05f},{pid::drive,0.05f} } },
            { "Studio Tight", { {pid::fundamental,58.0f},{pid::pitchStart,2.5f},{pid::pitchTime,28.0f},
                               {pid::bodyDecay,130.0f},{pid::bodyHarmonics,0.25f},{pid::clickLevel,0.50f},
                               {pid::clickTone,3200.0f},{pid::clickTime,3.5f},{pid::noiseLevel,0.15f},{pid::noiseDecay,35.0f},
                               {pid::transientAttack,0.35f},{pid::transientSustain,-0.20f},{pid::drive,0.20f},
                               {pid::tailLevel,0.05f},{pid::high,2.0f} } },
            { "Vintage Kit",  { {pid::fundamental,56.0f},{pid::pitchStart,2.0f},{pid::pitchTime,35.0f},
                               {pid::bodyDecay,260.0f},{pid::bodyHarmonics,0.35f},{pid::morph,0.35f},
                               {pid::clickLevel,0.30f},{pid::clickTone,2200.0f},{pid::clickTime,4.5f},{pid::noiseLevel,0.20f},{pid::noiseDecay,60.0f},
                               {pid::noiseTone,0.40f},{pid::noiseType,1.0f},{pid::drive,0.30f},{pid::character,0.10f},
                               {pid::tailLevel,0.10f},{pid::high,-2.0f} } },
            { "Metal Kit",    { {pid::fundamental,64.0f},{pid::pitchStart,3.0f},{pid::pitchTime,22.0f},
                               {pid::bodyDecay,110.0f},{pid::bodyHarmonics,0.30f},{pid::clickLevel,0.65f},
                               {pid::clickTone,5000.0f},{pid::clickPitch,6000.0f},{pid::clickTime,2.5f},
                               {pid::noiseLevel,0.15f},{pid::noiseDecay,30.0f},{pid::noiseTone,0.70f},
                               {pid::transientAttack,0.60f},{pid::drive,0.35f},{pid::character,0.20f},
                               {pid::tailLevel,0.04f},{pid::high,4.0f} } },
            { "Big Room Kit", { {pid::fundamental,52.0f},{pid::pitchStart,2.4f},{pid::pitchTime,40.0f},
                               {pid::bodyDecay,380.0f},{pid::bodyHarmonics,0.25f},{pid::morph,0.25f},
                               {pid::clickLevel,0.40f},{pid::clickTone,3200.0f},{pid::noiseLevel,0.20f},{pid::noiseDecay,80.0f},
                               {pid::noiseTone,0.45f},{pid::tailLevel,0.30f},{pid::tailLength,500.0f},{pid::tailTone,0.40f},
                               {pid::drive,0.20f},{pid::low,3.0f} } },
            { "Brush Kick",   { {pid::fundamental,66.0f},{pid::pitchStart,1.6f},{pid::pitchTime,30.0f},
                               {pid::bodyLevel,0.80f},{pid::bodyDecay,200.0f},{pid::bodyHarmonics,0.15f},{pid::morph,0.15f},
                               {pid::clickLevel,0.20f},{pid::clickTone,2000.0f},{pid::noiseLevel,0.30f},{pid::noiseDecay,90.0f},
                               {pid::noiseTone,0.35f},{pid::noiseType,1.0f},{pid::tailLevel,0.05f},{pid::drive,0.05f} } },
            { "Dead Kick",    { {pid::fundamental,60.0f},{pid::pitchStart,2.0f},{pid::pitchTime,20.0f},
                               {pid::bodyDecay,80.0f},{pid::bodyHarmonics,0.30f},{pid::clickLevel,0.40f},
                               {pid::clickTone,2800.0f},{pid::clickTime,3.0f},{pid::noiseLevel,0.10f},{pid::noiseDecay,25.0f},
                               {pid::subLevel,0.20f},{pid::subDecay,60.0f},{pid::tailLevel,0.02f},
                               {pid::transientSustain,-0.40f},{pid::drive,0.15f} } },
            // 808s: low fundamental, tiny sweep, long body + sub, little click, saturation on some.
            // 2026-09-03: pulled closer to the real TR-808 — its bridged-T resonator gives a
            // tiny FAST thump into a pure decaying sine: sweeps trimmed, clicks darker and
            // quieter, drive backed off.
            { "808 Classic",  { {pid::fundamental,42.0f},{pid::pitchStart,1.5f},{pid::pitchTime,45.0f},
                               {pid::bodyDecay,900.0f},{pid::bodyHarmonics,0.15f},{pid::subLevel,0.60f},{pid::subFreq,40.0f},
                               {pid::subDecay,800.0f},{pid::clickLevel,0.10f},{pid::clickTone,2000.0f},{pid::tailLevel,0.05f},
                               {pid::drive,0.10f},{pid::low,3.0f} } },
            { "808 Long",     { {pid::fundamental,38.0f},{pid::pitchStart,1.3f},{pid::pitchTime,80.0f},
                               {pid::bodyLevel,1.0f},{pid::bodyDecay,1800.0f},{pid::subLevel,0.70f},{pid::subFreq,36.0f},
                               {pid::subDecay,1600.0f},{pid::clickLevel,0.10f},{pid::tailLevel,0.0f},{pid::drive,0.10f},{pid::low,4.0f} } },
            { "808 Distorted",{ {pid::fundamental,44.0f},{pid::pitchStart,1.8f},{pid::pitchTime,55.0f},
                               {pid::bodyDecay,1000.0f},{pid::bodyHarmonics,0.50f},{pid::subLevel,0.50f},{pid::subDecay,900.0f},
                               {pid::clickLevel,0.20f},{pid::drive,0.60f},{pid::character,0.25f},{pid::driveMix,0.90f},
                               {pid::tailLevel,0.05f},{pid::low,2.0f} } },
            { "808 Punch",    { {pid::fundamental,46.0f},{pid::pitchStart,3.0f},{pid::pitchTime,45.0f},
                               {pid::bodyDecay,700.0f},{pid::bodyHarmonics,0.20f},{pid::subLevel,0.50f},{pid::subFreq,42.0f},
                               {pid::subDecay,600.0f},{pid::clickLevel,0.35f},{pid::clickTone,3000.0f},
                               {pid::transientAttack,0.35f},{pid::drive,0.25f},{pid::tailLevel,0.05f},{pid::low,2.0f} } },
            { "808 Sub Bass", { {pid::fundamental,34.0f},{pid::pitchStart,1.3f},{pid::pitchTime,90.0f},
                               {pid::bodyLevel,1.0f},{pid::bodyDecay,1500.0f},{pid::bodyHarmonics,0.05f},{pid::subLevel,0.90f},
                               {pid::subFreq,30.0f},{pid::subDecay,1400.0f},{pid::clickLevel,0.05f},{pid::tailLevel,0.0f},
                               {pid::drive,0.05f},{pid::low,5.0f} } },
            { "808 Glide",    { {pid::fundamental,40.0f},{pid::pitchStart,2.2f},{pid::pitchTime,200.0f},{pid::pitchCurve,0.50f},
                               {pid::bodyDecay,1200.0f},{pid::subLevel,0.60f},{pid::subFreq,38.0f},{pid::subDecay,1000.0f},
                               {pid::clickLevel,0.15f},{pid::tailLevel,0.03f},{pid::drive,0.20f},{pid::low,3.0f} } },
            { "808 Knock",    { {pid::fundamental,45.0f},{pid::pitchStart,2.5f},{pid::pitchTime,40.0f},{pid::pitchCurve,0.80f},
                               {pid::bodyDecay,800.0f},{pid::bodyHarmonics,0.30f},{pid::subLevel,0.40f},{pid::subDecay,500.0f},
                               {pid::clickLevel,0.45f},{pid::clickTone,2600.0f},{pid::clickTime,3.5f},
                               {pid::transientAttack,0.40f},{pid::drive,0.30f},{pid::character,0.10f},
                               {pid::tailLevel,0.05f},{pid::mid,2.0f} } },
            // Hip hop: mid-length bodies, crunchier drive curves, darker top, some sub under.
            { "Boom Bap",     { {pid::fundamental,52.0f},{pid::pitchStart,3.0f},{pid::pitchTime,50.0f},
                               {pid::bodyDecay,320.0f},{pid::bodyHarmonics,0.35f},{pid::morph,0.30f},
                               {pid::subLevel,0.40f},{pid::subFreq,45.0f},{pid::subDecay,300.0f},
                               {pid::clickLevel,0.35f},{pid::clickTone,2800.0f},{pid::clickTime,4.0f},
                               {pid::noiseLevel,0.12f},{pid::noiseDecay,50.0f},{pid::noiseTone,0.40f},{pid::noiseType,1.0f},
                               {pid::drive,0.40f},{pid::character,0.30f},{pid::driveMix,0.80f},
                               {pid::tailLevel,0.08f},{pid::low,3.0f},{pid::high,-3.0f} } },
            { "Dusty Vinyl",  { {pid::fundamental,50.0f},{pid::pitchStart,2.6f},{pid::pitchTime,45.0f},
                               {pid::bodyDecay,280.0f},{pid::bodyHarmonics,0.40f},{pid::morph,0.35f},
                               {pid::clickLevel,0.25f},{pid::clickTone,2200.0f},{pid::noiseLevel,0.20f},{pid::noiseDecay,70.0f},
                               {pid::noiseTone,0.30f},{pid::noiseType,1.0f},{pid::drive,0.50f},{pid::character,0.45f},
                               {pid::driveMix,0.70f},{pid::tailLevel,0.08f},{pid::low,2.0f},{pid::high,-5.0f} } },
            { "Lo-Fi Knock",  { {pid::fundamental,55.0f},{pid::pitchStart,3.5f},{pid::pitchTime,40.0f},
                               {pid::bodyDecay,240.0f},{pid::bodyHarmonics,0.30f},{pid::morph,0.40f},
                               {pid::subLevel,0.30f},{pid::subDecay,250.0f},{pid::clickLevel,0.40f},{pid::clickTone,3200.0f},
                               {pid::clickTime,3.0f},{pid::transientAttack,0.30f},{pid::drive,0.55f},{pid::character,0.60f},
                               {pid::driveMix,0.75f},{pid::tailLevel,0.06f},{pid::high,-4.0f} } },
            { "West Coast",   { {pid::fundamental,46.0f},{pid::pitchStart,2.8f},{pid::pitchTime,60.0f},
                               {pid::bodyDecay,450.0f},{pid::subLevel,0.55f},{pid::subFreq,40.0f},{pid::subDecay,450.0f},
                               {pid::clickLevel,0.30f},{pid::clickTone,3000.0f},{pid::drive,0.30f},{pid::character,0.20f},
                               {pid::tailLevel,0.10f},{pid::tailLength,300.0f},{pid::low,3.0f} } },
            { "Drill",        { {pid::fundamental,40.0f},{pid::pitchStart,2.0f},{pid::pitchTime,70.0f},
                               {pid::bodyDecay,600.0f},{pid::bodyHarmonics,0.20f},{pid::subLevel,0.70f},{pid::subFreq,36.0f},
                               {pid::subDecay,550.0f},{pid::clickLevel,0.30f},{pid::clickTone,3400.0f},{pid::clickTime,3.0f},
                               {pid::transientAttack,0.30f},{pid::drive,0.30f},{pid::tailLevel,0.05f},{pid::low,4.0f} } },
            { "Trap Hard",    { {pid::fundamental,44.0f},{pid::pitchStart,3.5f},{pid::pitchTime,50.0f},
                               {pid::bodyDecay,550.0f},{pid::bodyHarmonics,0.40f},{pid::subLevel,0.60f},{pid::subFreq,40.0f},
                               {pid::subDecay,500.0f},{pid::clickLevel,0.45f},{pid::clickTone,4000.0f},
                               {pid::transientAttack,0.45f},{pid::drive,0.50f},{pid::character,0.30f},{pid::driveMix,0.85f},
                               {pid::tailLevel,0.08f},{pid::low,3.0f} } },
            { "Neo Soul",     { {pid::fundamental,54.0f},{pid::pitchStart,2.2f},{pid::pitchTime,45.0f},
                               {pid::bodyLevel,0.90f},{pid::bodyDecay,300.0f},{pid::bodyHarmonics,0.20f},{pid::morph,0.25f},
                               {pid::subLevel,0.35f},{pid::subFreq,48.0f},{pid::subDecay,300.0f},
                               {pid::clickLevel,0.20f},{pid::clickTone,2400.0f},{pid::noiseLevel,0.08f},{pid::noiseDecay,40.0f},
                               {pid::drive,0.20f},{pid::character,0.10f},{pid::tailLevel,0.06f},{pid::low,2.0f} } },
            // Techno: fast deep sweeps, bright click, driven tails / rumble, forward transient.
            { "Berlin",       { {pid::fundamental,50.0f},{pid::pitchStart,5.0f},{pid::pitchTime,40.0f},
                               {pid::bodyDecay,240.0f},{pid::bodyHarmonics,0.20f},{pid::clickLevel,0.45f},
                               {pid::clickTone,4200.0f},{pid::clickTime,2.5f},{pid::tailLevel,0.30f},{pid::tailLength,220.0f},
                               {pid::tailTone,0.50f},{pid::tailDrive,0.35f},{pid::drive,0.45f},{pid::character,0.25f},
                               {pid::transientAttack,0.40f},{pid::low,2.0f} } },
            { "Rumble",       { {pid::fundamental,48.0f},{pid::pitchStart,4.5f},{pid::pitchTime,45.0f},
                               {pid::bodyDecay,260.0f},{pid::clickLevel,0.40f},{pid::clickTone,4200.0f},
                               {pid::tailLevel,0.55f},{pid::tailLength,420.0f},{pid::tailTone,0.35f},{pid::tailDrive,0.55f},
                               {pid::noiseLevel,0.15f},{pid::noiseDecay,200.0f},{pid::noiseTone,0.30f},{pid::noiseType,1.0f},
                               {pid::drive,0.40f},{pid::character,0.20f},{pid::low,3.0f} } },
            { "Peak Time",    { {pid::fundamental,54.0f},{pid::pitchStart,6.0f},{pid::pitchTime,36.0f},
                               {pid::bodyDecay,210.0f},{pid::clickLevel,0.55f},{pid::clickTone,5000.0f},{pid::clickPitch,6500.0f},
                               {pid::clickTime,2.0f},{pid::tailLevel,0.25f},{pid::tailLength,200.0f},{pid::tailTone,0.60f},
                               {pid::tailDrive,0.40f},{pid::drive,0.55f},{pid::character,0.35f},
                               {pid::transientAttack,0.55f},{pid::transientSustain,-0.10f},{pid::low,2.0f},{pid::high,2.0f} } },
            { "Acid Kick",    { {pid::fundamental,56.0f},{pid::pitchStart,7.0f},{pid::pitchTime,32.0f},{pid::pitchCurve,0.90f},
                               {pid::bodyDecay,190.0f},{pid::bodyHarmonics,0.30f},{pid::clickLevel,0.50f},{pid::clickTone,6000.0f},
                               {pid::tailLevel,0.20f},{pid::tailLength,160.0f},{pid::tailTone,0.70f},{pid::tailDrive,0.60f},
                               {pid::drive,0.65f},{pid::character,0.45f},{pid::driveMix,0.90f},{pid::transientAttack,0.50f} } },
            { "Dub Techno",   { {pid::fundamental,46.0f},{pid::pitchStart,3.5f},{pid::pitchTime,55.0f},
                               {pid::bodyDecay,420.0f},{pid::morph,0.20f},{pid::clickLevel,0.25f},{pid::clickTone,3000.0f},
                               {pid::tailLevel,0.40f},{pid::tailLength,600.0f},{pid::tailTone,0.30f},{pid::tailDrive,0.25f},
                               {pid::subLevel,0.40f},{pid::subFreq,42.0f},{pid::subDecay,400.0f},
                               {pid::drive,0.25f},{pid::character,0.10f},{pid::low,3.0f},{pid::high,-3.0f} } },
            { "Minimal Tick", { {pid::fundamental,52.0f},{pid::pitchStart,4.0f},{pid::pitchTime,30.0f},
                               {pid::bodyDecay,150.0f},{pid::clickLevel,0.60f},{pid::clickTone,7000.0f},{pid::clickPitch,8000.0f},
                               {pid::clickTime,1.5f},{pid::tailLevel,0.10f},{pid::tailLength,120.0f},
                               {pid::noiseLevel,0.08f},{pid::noiseDecay,25.0f},{pid::noiseTone,0.80f},
                               {pid::drive,0.30f},{pid::transientAttack,0.50f},{pid::transientSustain,-0.30f},{pid::high,3.0f} } },
            { "Schranz",      { {pid::fundamental,58.0f},{pid::pitchStart,8.0f},{pid::pitchTime,28.0f},
                               {pid::bodyDecay,170.0f},{pid::clickLevel,0.55f},{pid::clickTone,6500.0f},{pid::clickTime,2.0f},
                               {pid::tailLevel,0.35f},{pid::tailLength,260.0f},{pid::tailTone,0.75f},{pid::tailDrive,0.75f},
                               {pid::noiseLevel,0.12f},{pid::noiseDecay,60.0f},{pid::noiseTone,0.70f},{pid::noiseType,2.0f},
                               {pid::drive,0.80f},{pid::character,0.55f},{pid::driveMix,0.95f},
                               {pid::transientAttack,0.60f},{pid::low,2.0f},{pid::high,3.0f} } },
            { "Melodic Techno",{ {pid::fundamental,49.0f},{pid::pitchStart,3.8f},{pid::pitchTime,48.0f},
                               {pid::bodyDecay,340.0f},{pid::bodyHarmonics,0.15f},{pid::morph,0.25f},{pid::bodyWidth,0.15f},
                               {pid::clickLevel,0.35f},{pid::clickTone,3800.0f},{pid::tailLevel,0.30f},{pid::tailLength,380.0f},
                               {pid::tailTone,0.45f},{pid::tailDrive,0.30f},{pid::subLevel,0.35f},{pid::subFreq,44.0f},
                               {pid::subDecay,350.0f},{pid::drive,0.30f},{pid::character,0.15f},{pid::low,2.0f} } },
        } };


        // Musically sensible sub-ranges for Randomize (NOT the full param range).
        struct RandRange { const char* id; float lo; float hi; };
        const std::array<RandRange, 33> kRandRanges { {
            { pid::fundamental,      35.0f,   70.0f },
            { pid::pitchStart,        2.0f,    8.0f },
            { pid::pitchTime,        20.0f,  120.0f },
            { pid::pitchCurve,        0.40f,   1.0f },
            { pid::bodyLevel,         0.70f,   1.0f },
            { pid::bodyDecay,       120.0f,  700.0f },
            { pid::bodyHarmonics,     0.0f,    0.40f },
            { pid::morph,             0.0f,    0.60f },   // 2026-09-01 v2: attack-only phase-skew — gentle by design, so Randomize can reach a bit further
            { pid::subLevel,          0.20f,   0.80f },
            { pid::subFreq,          30.0f,   55.0f },
            { pid::subDecay,        100.0f,  500.0f },
            { pid::clickLevel,        0.20f,   0.80f },
            { pid::clickTone,      2500.0f, 9000.0f },
            { pid::clickTime,         1.0f,   12.0f },
            { pid::clickPitch,     3000.0f, 9000.0f },
            { pid::tailLevel,         0.0f,    0.50f },
            { pid::tailLength,       80.0f,  600.0f },
            { pid::tailTone,          0.20f,   0.80f },
            { pid::tailDrive,         0.0f,    0.50f },
            { pid::noiseDecay,       30.0f,  200.0f },
            { pid::noiseTone,         0.30f,   0.80f },
            { pid::transientAttack,  -0.30f,   0.70f },
            { pid::transientSustain, -0.30f,   0.40f },
            { pid::drive,             0.10f,   0.70f },
            { pid::character,         0.0f,    0.60f },
            { pid::driveMix,          0.60f,   1.0f },
            { pid::low,              -3.0f,    6.0f },
            { pid::mid,              -4.0f,    3.0f },
            { pid::high,             -3.0f,    5.0f },
            { pid::bodyWidth,         0.0f,    0.30f },
            { pid::clickWidth,        0.20f,   0.70f },
            { pid::outputWidth,       0.40f,   0.70f },
            { pid::noiseLevel,        0.0f,    0.30f },   // handled specially (often 0)
        } };
    }

    //==========================================================================
    PresetManager::PresetManager (juce::AudioProcessorValueTreeState& stateToManage,
                                  juce::UndoManager&                  undoToUse,
                                  std::function<void (const juce::String&)> reloadSampleByName,
                                  std::function<juce::String()>            currentSampleName)
        : apvts (stateToManage),
          undoManager (undoToUse),
          reloadSample (std::move (reloadSampleByName)),
          getSampleName (std::move (currentSampleName))
    {
    }

    //====================================================================== helpers
    // Everything below works at the state-tree level: build the target tree, then do one
    // undoable `replaceState` swap. This sidesteps the APVTS param->tree flush, which is
    // otherwise driven by a Timer and so would make `beginNewTransaction` + a bare
    // `setValueNotifyingHost` un-undoable in a headless / paused-UI context.

    juce::ValueTree PresetManager::snapshotState() const
    {
        return apvts.copyState();   // flushes pending param values, then returns a copy
    }

    float PresetManager::readTreeParam (const juce::ValueTree& tree, juce::StringRef pId) const
    {
        const auto child = tree.getChildWithProperty ("id", juce::String (pId));
        return child.isValid() ? (float) child.getProperty ("value") : defaultOf (pId);
    }

    void PresetManager::writeTreeParam (juce::ValueTree& tree, juce::StringRef pId, float denorm) const
    {
        if (auto* p = apvts.getParameter (pId))
        {
            const auto& r = p->getNormalisableRange();
            denorm = juce::jlimit (r.start, r.end, r.snapToLegalValue (denorm));
        }
        auto child = tree.getChildWithProperty ("id", juce::String (pId));
        if (child.isValid())
            child.setProperty ("value", denorm, nullptr);
    }

    float PresetManager::defaultOf (juce::StringRef pId) const
    {
        if (auto* p = apvts.getParameter (pId))
            return p->convertFrom0to1 (p->getDefaultValue());
        return 0.0f;
    }

    void PresetManager::applyStateUndoable (juce::ValueTree newState, const juce::String& transactionName)
    {
        struct SwapAction final : juce::UndoableAction
        {
            SwapAction (juce::AudioProcessorValueTreeState& a,
                        juce::ValueTree before, juce::ValueTree after,
                        std::function<void (const juce::String&)> reload)
                : apvts (a), oldTree (std::move (before)), newTree (std::move (after)),
                  reloadCb (std::move (reload)) {}

            bool apply (const juce::ValueTree& t)
            {
                apvts.replaceState (t.createCopy());
                if (reloadCb)
                    reloadCb (t.getProperty ("currentSampleName", juce::String()).toString());
                return true;
            }
            bool perform() override { return apply (newTree); }
            bool undo()    override { return apply (oldTree); }
            int  getSizeInUnits() override { return 2048; }

            juce::AudioProcessorValueTreeState& apvts;
            juce::ValueTree oldTree, newTree;
            std::function<void (const juce::String&)> reloadCb;
        };

        auto before = snapshotState();
        // carry the sample name the processor currently holds into `before` so an undo
        // restores it too.
        if (getSampleName)
            before.setProperty ("currentSampleName", getSampleName(), nullptr);

        undoManager.beginNewTransaction (transactionName);
        undoManager.perform (new SwapAction (apvts, before, std::move (newState), reloadSample),
                             transactionName);
    }

    void PresetManager::setPresetName (const juce::String& name, const juce::File& path)
    {
        apvts.state.setProperty ("currentPresetName", name, nullptr);
        apvts.state.setProperty ("currentPresetPath", path.getFullPathName(), nullptr);
    }

    juce::String PresetManager::getCurrentPresetName() const
    {
        return apvts.state.getProperty ("currentPresetName", juce::String()).toString();
    }

    void PresetManager::markDefaultIfUnnamed()
    {
        if (getCurrentPresetName().isEmpty())
            setPresetName ("Default");
    }

    bool PresetManager::isExcludedFromRandom (const juce::String& pId) noexcept
    {
        static const juce::StringArray excluded {
            pid::oversampling, pid::limiter, pid::output, pid::mix,
            pid::limLoudness, pid::limCeiling, pid::limLookahead, pid::limRelease, pid::limSaturation, pid::limColor,   // 2026-09-02
            pid::filterOn, pid::filterType, pid::filterFreq, pid::filterRes,   // 2026-09-02 — master filter is a mix decision, not a patch one
            pid::tuneMode, pid::tune, pid::fineTune, pid::velSensitivity,
            pid::macroPunch, pid::macroBody, pid::macroCrush, pid::macroTail,
            pid::synthEnable, pid::sampleEnable,
            pid::sampleLevel, pid::sampleStart, pid::sampleEnd, pid::sampleReverse,
            pid::sampleTune, pid::sampleFine, pid::sampleMidiTrack, pid::sampleAttack,
            pid::sampleDecay, pid::sampleHP, pid::sampleLP, pid::sampleCrush
        };
        return excluded.contains (pId);
    }

    //====================================================================== factory
    int PresetManager::getNumFactory() noexcept { return (int) kFactory.size(); }

    juce::StringArray PresetManager::getFactoryNames() const
    {
        juce::StringArray names;
        for (const auto& f : kFactory)
            names.add (f.name);
        return names;
    }

    void PresetManager::loadFactory (int index)
    {
        if (index < 0 || index >= (int) kFactory.size())
            return;

        const auto& f = kFactory[static_cast<size_t> (index)];

        auto ns = snapshotState().createCopy();

        // every param -> its default, then the genre overrides, then force synth-only.
        for (int i = 0; i < ns.getNumChildren(); ++i)
        {
            auto child = ns.getChild (i);
            const auto pId = child.getProperty ("id").toString();
            if (pId.isNotEmpty())
                child.setProperty ("value", defaultOf (pId), nullptr);
        }
        for (const auto& o : f.overrides)
            writeTreeParam (ns, o.id, o.value);

        writeTreeParam (ns, pid::synthEnable,  1.0f);
        writeTreeParam (ns, pid::sampleEnable, 0.0f);

        ns.setProperty ("currentPresetName", f.name, nullptr);
        ns.setProperty ("currentPresetPath", juce::String(), nullptr);
        // Factory presets are synth-only in SOUND (sampleEnable written 0 above), but they
        // keep whatever kick is currently loaded in the SAMPLE strip rather than clearing it
        // (bug-scan 2026-09-01, code-review CONFIRMED: writing "" here undid the fresh-
        // instance "first factory kick preloaded, section off" seeding the moment any factory
        // preset was loaded — the strip fell back to "N kicks in the bank" with nothing armed).
        ns.setProperty ("currentSampleName", getSampleName ? getSampleName() : juce::String(), nullptr);

        applyStateUndoable (std::move (ns), juce::String ("Preset: ") + f.name);
    }

    //======================================================================== user
    namespace { juce::File& testFolderRef() { static juce::File f; return f; } }

    void PresetManager::setUserFolderForTests (const juce::File& f)
    {
        testFolderRef() = f;
        if (f != juce::File())
            f.createDirectory();
    }

    juce::File PresetManager::userFolder()
    {
        if (testFolderRef() != juce::File())
            return testFolderRef();

        auto base = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
        if (base == juce::File() || base.getFullPathName().isEmpty())
            base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);

        auto folder = base.getChildFile ("KICKR").getChildFile ("Presets");
        if (! folder.createDirectory().wasOk() && ! folder.isDirectory())
            folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("KICKR").getChildFile ("Presets");
        folder.createDirectory();
        return folder;
    }

    juce::StringArray PresetManager::getUserNames() const
    {
        juce::StringArray names;
        for (const auto& f : juce::RangedDirectoryIterator (userFolder(), false, "*.kickrpreset",
                                                            juce::File::findFiles))
            names.add (f.getFile().getFileNameWithoutExtension());
        names.sortNatural();
        return names;
    }

    bool PresetManager::saveUser (const juce::String& name)
    {
        const auto safe = juce::File::createLegalFileName (name).trim();
        if (safe.isEmpty())
            return false;

        auto state = snapshotState();
        state.setProperty ("stateVersion", 2, nullptr);
        state.setProperty ("currentPresetName", safe, nullptr);
        state.setProperty ("currentSampleName", getSampleName ? getSampleName() : juce::String(), nullptr);

        const auto dest = userFolder().getChildFile (safe + ".kickrpreset");
        if (auto xml = std::unique_ptr<juce::XmlElement> (state.createXml()))
        {
            const bool ok = xml->writeTo (dest);
            if (ok)
                setPresetName (safe, dest);
            return ok;
        }
        return false;
    }

    bool PresetManager::loadUser (const juce::String& name)
    {
        const auto file = userFolder().getChildFile (name + ".kickrpreset");
        if (! file.existsAsFile())
            return false;

        auto xml = juce::XmlDocument::parse (file);
        if (xml == nullptr)
            return false;

        auto tree = juce::ValueTree::fromXml (*xml);
        if (! tree.isValid() || tree.getType() != apvts.state.getType())
            return false;

        tree.setProperty ("currentPresetName", name, nullptr);
        tree.setProperty ("currentPresetPath", file.getFullPathName(), nullptr);

        applyStateUndoable (std::move (tree), juce::String ("Preset: ") + name);
        return true;
    }

    bool PresetManager::loadByName (const juce::String& name)
    {
        const auto factory = getFactoryNames();
        const int  fi      = factory.indexOf (name);
        if (fi >= 0) { loadFactory (fi); return true; }
        return loadUser (name);
    }

    //================================================================ randomize/mutate
    void PresetManager::randomize()
    {
        auto ns = snapshotState().createCopy();

        for (const auto& r : kRandRanges)
        {
            if (juce::String (r.id) == pid::noiseLevel)
            {
                const float v = rng.nextFloat() < 0.60f ? 0.0f
                                                        : r.lo + rng.nextFloat() * (r.hi - r.lo);
                writeTreeParam (ns, r.id, v);
                continue;
            }
            writeTreeParam (ns, r.id, r.lo + rng.nextFloat() * (r.hi - r.lo));
        }
        writeTreeParam (ns, pid::noiseType, (float) rng.nextInt (3));

        // Light correlations.
        if (readTreeParam (ns, pid::drive) > 0.5f)
            writeTreeParam (ns, pid::character,
                            juce::jlimit (0.0f, 1.0f, readTreeParam (ns, pid::character) + rng.nextFloat() * 0.2f));
        if (readTreeParam (ns, pid::tailLength) > 400.0f)
            writeTreeParam (ns, pid::tailTone,
                            juce::jlimit (0.0f, 1.0f, readTreeParam (ns, pid::tailTone) - 0.15f));

        ns.setProperty ("currentPresetName", "Random", nullptr);
        applyStateUndoable (std::move (ns), "Randomize");
    }

    void PresetManager::mutate()
    {
        auto ns = snapshotState().createCopy();

        for (auto* p : apvts.processor.getParameters())
        {
            auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p);
            if (rp == nullptr || dynamic_cast<juce::AudioParameterChoice*> (p) != nullptr)
                continue;

            const auto pId = rp->getParameterID();
            if (isExcludedFromRandom (pId))
                continue;

            const auto& range = rp->getNormalisableRange();
            const float span  = range.end - range.start;
            const float cur   = readTreeParam (ns, pId);
            const float delta = (rng.nextFloat() - 0.5f) * 0.24f * span;   // ~+-12 %
            writeTreeParam (ns, pId, juce::jlimit (range.start, range.end, cur + delta));
        }

        const auto n = getCurrentPresetName();
        ns.setProperty ("currentPresetName",
                        n.endsWith (" *") ? n : (n.isEmpty() ? juce::String ("Mutated") : n + " *"),
                        nullptr);
        applyStateUndoable (std::move (ns), "Mutate");
    }
}
