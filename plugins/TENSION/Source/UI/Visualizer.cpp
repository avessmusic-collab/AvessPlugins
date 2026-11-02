#include "Visualizer.h"

using namespace TensionUI;

namespace
{
    constexpr float twoPi = juce::MathConstants<float>::twoPi;
    constexpr int kSpikes = 72;

    constexpr int kStaticTile = 128;       // px, tiled across the 500x500 window
    constexpr int kStaticEveryNTicks = 5;  // 60 Hz / 5 = 12 Hz frame flips

    float easeOutCubic (float x) { const float u = 1.0f - x; return 1.0f - u * u * u; }
}

Visualizer::Visualizer (const ThemeState& themeState) : theme (themeState)
{
    setInterceptsMouseClicks (false, false);
    setOpaque (false);
    rings.reserve (16);
    buildStaticFrames();
    lastTickMs = juce::Time::getMillisecondCounterHiRes();
    startTimerHz (60);
}

Visualizer::~Visualizer() { stopTimer(); }

// One-time: pre-render the static tiles (white speckles, alpha baked in).
// Cubic bias keeps most pixels near-invisible with sparse brighter flecks, so
// the overall effect stays subtle on the near-black backdrop.
void Visualizer::buildStaticFrames()
{
    for (auto& img : staticFrames)
    {
        img = juce::Image (juce::Image::ARGB, kStaticTile, kStaticTile, true);
        juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < kStaticTile; ++y)
            for (int x = 0; x < kStaticTile; ++x)
            {
                const float v = random.nextFloat();
                const float alpha = v * v * v * 0.085f;   // mean ~2 %, peak 8.5 %
                bd.setPixelColour (x, y, juce::Colours::white.withAlpha (alpha));
            }
    }
}

void Visualizer::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    const float dt = juce::jlimit (0.0f, 0.1f, (float) ((now - lastTickMs) * 0.001));
    lastTickMs = now;

    // TV-static: flip to the next pre-rendered tile at 12 Hz with a fresh
    // random offset. This animates regardless of tension, so the idle path
    // below still repaints (at 12 Hz instead of the old full stop).
    bool staticFlipped = false;
    if (++staticTick >= kStaticEveryNTicks)
    {
        staticTick = 0;
        staticFrame = (staticFrame + 1) % (int) staticFrames.size();
        staticOffset = { random.nextInt (kStaticTile), random.nextInt (kStaticTile) };
        staticFlipped = true;
    }

    const bool active = state.tension > 0.0005f || smoothedI > 0.0005f;
    if (! active)
    {
        // Field fully transparent at 0 %: finish the fade-out, then only the
        // static keeps repainting (12 Hz).
        if (idleFrames < 3)
        {
            ++idleFrames;
            rings.clear();
            for (auto& p : particles) p.alive = false;
            smoothedI = smoothedTension = gate = 0.0f;
            repaint();
        }
        else if (staticFlipped)
            repaint (10, 10, 500, 500);   // static only changes inside the window
        return;
    }

    idleFrames = 0;
    step (dt);
    repaint();
}

void Visualizer::step (float dt)
{
    smoothedTension += 0.12f * (state.tension - smoothedTension);
    const float targetI = std::pow (juce::jlimit (0.0f, 1.0f, state.tension), intensityGamma (riseMode));
    smoothedI += 0.12f * (targetI - smoothedI);
    gate = smoothstep (0.0f, 0.04f, smoothedTension);

    const float i = smoothedI;

    // spike motion
    const float rate = 0.15f + 2.6f * i;
    spikePhase += dt * rate * twoPi;
    rotation += dt * juce::degreesToRadians (2.0f + 22.0f * i);
    breathPhase += dt * twoPi * (0.5f + 1.5f * i);

    // rings
    if (gate > 0.001f)
    {
        ringAccum += dt * (0.2f + 2.8f * i);
        while (ringAccum >= 1.0f)
        {
            ringAccum -= 1.0f;
            if (rings.size() < 12)
                rings.push_back ({});
        }
    }
    const float lifetime = juce::jmax (1.3f, 3.2f - 1.9f * i);
    for (auto& r : rings) r.age += dt;
    rings.erase (std::remove_if (rings.begin(), rings.end(),
                                 [lifetime] (const Ring& r) { return r.age >= lifetime; }),
                 rings.end());

    // particles
    const int target = juce::roundToInt (gate * (6.0f + 130.0f * std::pow (i, 1.1f)));
    int alive = 0;
    for (auto& p : particles)
    {
        if (! p.alive) continue;
        p.age += dt;
        if (p.age >= p.life) { p.alive = false; continue; }
        p.radius += p.speed * dt;
        p.angle += (p.drift / juce::jmax (1.0f, p.radius)) * dt;
        ++alive;
    }
    for (int spawned = 0; alive < target && spawned < 6; ++spawned, ++alive)
        spawnParticle();
}

void Visualizer::spawnParticle()
{
    const float i = smoothedI;
    for (auto& p : particles)
    {
        if (p.alive) continue;
        p.alive = true;
        p.age = 0.0f;
        p.angle = random.nextFloat() * twoPi;
        p.radius = 100.0f + random.nextFloat() * 10.0f;
        p.speed = (25.0f + 140.0f * i) * (0.6f + 0.8f * random.nextFloat());
        p.life = (1.6f - 0.8f * i) * (0.7f + 0.6f * random.nextFloat());
        p.size = (1.0f + 1.6f * i) * (0.6f + 0.6f * random.nextFloat());
        p.drift = (6.0f + 30.0f * i) * (random.nextBool() ? 1.0f : -1.0f);
        return;
    }
}

void Visualizer::paint (juce::Graphics& g)
{
    paintBackdrop (g);

    const auto& th = theme.current();
    const float i = smoothedI;
    const float gt = gate;
    const juce::Colour accent = accentRamp (th, smoothedTension);
    const float cx = kFieldCentreX, cy = kFieldCentreY;

    {
        juce::Graphics::ScopedSaveState s (g);
        juce::Path window;
        window.addRoundedRectangle (10.0f, 10.0f, 500.0f, 500.0f, 6.0f);
        g.reduceClipRegion (window);

        // faint moving TV-static over the backdrop (alpha baked into the tiles)
        g.setTiledImageFill (staticFrames[(size_t) staticFrame],
                             -staticOffset.x, -staticOffset.y, 1.0f);
        g.fillRect (10, 10, 500, 500);

        if (gt > 0.001f)
        {
            // halo: radial r 90 -> 230, mid alpha at r=130
            const float haloMid = juce::jlimit (0.0f, 1.0f, gt * 0.14f * std::pow (i, 1.3f) * th.glowK
                                                              * (0.85f + 0.3f * juce::jlimit (0.0f, 1.0f, state.level)));
            g.setGradientFill (makeRadial (cx, cy, 90.0f, 230.0f,
                                           { { 0.0f, accent.withAlpha (0.0f) },
                                             { 40.0f / 140.0f, accent.withAlpha (haloMid) },
                                             { 1.0f, accent.withAlpha (0.0f) } }));
            g.fillEllipse (cx - 230.0f, cy - 230.0f, 460.0f, 460.0f);

            // pulse rings
            const float lifetime = juce::jmax (1.3f, 3.2f - 1.9f * i);
            for (const auto& r : rings)
            {
                const float an = juce::jlimit (0.0f, 1.0f, r.age / lifetime);
                const float radius = 100.0f + 115.0f * easeOutCubic (an);
                const float alpha = gt * (0.08f + 0.4f * i) * std::pow (1.0f - an, 1.5f);
                g.setColour (accent.withAlpha (juce::jlimit (0.0f, 1.0f, alpha)));
                g.drawEllipse (cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, 2.2f - 1.2f * an);
            }

            // radial spikes
            const float breath = 1.0f + 0.015f * i * std::sin (breathPhase);
            const float inner = 104.0f * breath;
            const float baseLen = 2.0f + 6.0f * i;
            const float maxExtra = 70.0f * std::pow (i, 1.2f);
            const float width = 1.2f + 1.3f * i;
            for (int k = 0; k < kSpikes; ++k)
            {
                const float kf = (float) k;
                const float act = 0.5f * ((0.5f + 0.5f * std::sin (state.phase + kf * 0.7f + spikePhase))
                                         + (0.5f + 0.5f * std::sin (kf * 1.9f - spikePhase * 1.37f)));
                const float len = baseLen + maxExtra * act;
                const float a = rotation + twoPi * kf / (float) kSpikes;
                const float ca = std::cos (a), sa = std::sin (a);
                const float alpha = gt * (0.10f + 0.75f * i) * (0.35f + 0.65f * act);
                g.setColour (accent.withAlpha (juce::jlimit (0.0f, 1.0f, alpha)));
                g.drawLine (cx + ca * inner, cy + sa * inner, cx + ca * (inner + len), cy + sa * (inner + len), width);
            }

            // particles
            for (const auto& p : particles)
            {
                if (! p.alive) continue;
                const float an = p.age / p.life;
                const float alpha = gt * (0.15f + 0.6f * i) * (0.8f + 0.4f * state.noise) * (1.0f - an);
                g.setColour (accent.withAlpha (juce::jlimit (0.0f, 1.0f, alpha)));
                const float px = cx + std::cos (p.angle) * p.radius;
                const float py = cy + std::sin (p.angle) * p.radius;
                g.fillEllipse (px - p.size * 0.5f, py - p.size * 0.5f, p.size, p.size);
            }
        }

        // accent hairline at 13.5 px, drawn inside the window over the field
        g.setGradientFill (makeLinear ({ 13.0f, 13.0f }, { 507.0f, 507.0f },
                                       { { 0.0f, th.stopLow.withAlpha (0.22f + 0.2f * i) },
                                         { 1.0f, th.stopHigh.withAlpha (0.18f + 0.2f * i) } }));
        g.drawRoundedRectangle (13.5f, 13.5f, 493.0f, 493.0f, 4.0f, 1.0f);
    }
}
