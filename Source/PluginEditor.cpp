#include "PluginEditor.h"
#include "BinaryData.h"

namespace mimic
{
using namespace juce;

namespace
{
// Tout est placé dans le repère de la fenêtre à sa taille d'origine (943 x 512), puis mis à l'échelle.
const Rectangle<float> kCavity { 234.0f, 111.0f, 483.0f, 180.0f };
const Rectangle<float> kMawGlow { 178.5f, 84.0f, 585.0f, 232.0f };
const Rectangle<float> kSourceSlot { 109.0f, 28.0f, 146.0f, 28.0f };
const Rectangle<float> kWaveSlot { 109.0f, 64.0f, 112.0f, 24.0f };
const Rectangle<float> kFreezeSlot { 689.0f, 28.0f, 146.0f, 28.0f };
const Rectangle<float> kPill { 347.5f, 471.0f, 248.0f, 24.0f };
constexpr float kMeterLeft = 55.0f, kMeterRight = 849.0f;
constexpr float kKnobY = 401.0f, kKnobRadius = 60.0f;
constexpr int kDashes = 28, kMeterSegments = 26;
constexpr int kFreezeButton = 7;

const Colour kNight { 0xff0d0809 }, kEmber { 0xffe23444 }, kBone { 0xffffecee };

float knobX (int index) { return 131.0f + 136.0f * (float) index; }

Rectangle<float> knobArea (int index)
{
    // Le sceau, ses tirets et l'étiquette de valeur au-dessus.
    return Rectangle<float> (knobX (index) - 68.0f, kKnobY - 92.0f, 136.0f, 160.0f);
}

// Les boutons des fentes n'ont pas tous la même largeur (VOIX, PERCU, TEXTURE ; les quatre formes d'onde) : leurs
// bords sont ceux du décor, relevés dans la page d'origine (ui/mimic_ui.html) et ramenés au repère de la fenêtre.
// Un découpage en parts égales allumait un bout du bouton voisin.
constexpr float kSourceEdges[] = { 109.0f, 154.3f, 200.0f, 255.0f };
constexpr float kWaveEdges[] = { 109.0f, 136.8f, 164.9f, 193.2f, 221.0f };

Rectangle<float> buttonArea (int button)
{
    if (button < 3)
        return { kSourceEdges[button], kSourceSlot.getY(), kSourceEdges[button + 1] - kSourceEdges[button], kSourceSlot.getHeight() };

    if (button < kFreezeButton)
        return { kWaveEdges[button - 3], kWaveSlot.getY(), kWaveEdges[button - 2] - kWaveEdges[button - 3], kWaveSlot.getHeight() };

    return kFreezeSlot;
}

Rectangle<float> meterArea (float x0) { return { x0, 139.0f, 44.0f, 142.0f }; }

/** Bande i de l'affichage (50 Hz à 12 kHz, 72 bandes) lue dans les 64 bandes du moteur (30 Hz à 18 kHz). */
float bandAt (const float* bands, int i)
{
    const auto f = std::exp (std::log (50.0f) + ((float) i / 71.0f) * std::log (240.0f));
    const auto p = jlimit (0.0f, 63.0f, std::log (f / 30.0f) / std::log (600.0f) * 64.0f - 0.5f);
    const auto i0 = (int) p;
    return bands[i0] + (bands[jmin (63, i0 + 1)] - bands[i0]) * (p - (float) i0);
}

float sampleBands (const float* bands, int count, float u)
{
    const auto pos = jlimit (0.0f, 1.0f, u) * (float) (count - 1);
    const auto i0 = (int) pos;
    return bands[i0] + (bands[jmin (count - 1, i0 + 1)] - bands[i0]) * (pos - (float) i0);
}

//==============================================================================
// Bruit de valeur à plusieurs octaves, pour la fumée de la gueule.
float hash2 (int x, int y, int seed)
{
    auto n = (uint32) x * 374761393u + (uint32) y * 668265263u + (uint32) seed * 1442695041u;
    n = (n ^ (n >> 13)) * 1274126177u;
    return (float) ((n ^ (n >> 16)) & 0xffffffu) / 16777216.0f;
}

float valueNoise (float x, float y, int seed)
{
    const auto xi = (int) std::floor (x), yi = (int) std::floor (y);
    auto fx = x - (float) xi, fy = y - (float) yi;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    const auto a = hash2 (xi, yi, seed), b = hash2 (xi + 1, yi, seed);
    const auto c = hash2 (xi, yi + 1, seed), d = hash2 (xi + 1, yi + 1, seed);
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

float fbm (float x, float y, int seed, int octaves)
{
    float value = 0.0f, amplitude = 0.5f, frequency = 1.0f, total = 0.0f;

    for (int i = 0; i < octaves; ++i)
    {
        value += amplitude * valueNoise (x * frequency, y * frequency, seed + i * 17);
        total += amplitude;
        frequency *= 2.0f;
        amplitude *= 0.5f;
    }

    return value / total;
}

Image makeSmoke()
{
    constexpr int w = 256, h = 128;
    Image image (Image::ARGB, w, h, true);
    const Image::BitmapData data (image, Image::BitmapData::writeOnly);

    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const auto px = (float) x * 2.0f, py = (float) y * 2.0f;
            const auto warp = fbm (px * 0.012f, py * 0.02f, 201, 3) * 40.0f;
            const auto v = fbm ((px + warp) * 0.008f, py * 0.016f, 203, 5);
            const auto v2 = fbm (px * 0.03f, py * 0.05f, 205, 3);
            auto k = jlimit (0.0f, 1.0f, (v - 0.38f) * 2.2f);
            k = jlimit (0.0f, 1.0f, k * k * (0.55f + 0.7f * v2));
            data.setPixelColour (x, y, Colour::fromFloatRGBA (1.0f, 0.36f, 0.2f, k));
        }

    return image;
}

void strokeLine (Graphics& g, float x1, float y1, float x2, float y2, float thickness)
{
    g.drawLine (x1, y1, x2, y2, thickness);
}

void star4 (Graphics& g, float x, float y, float r, float alpha)
{
    g.setColour (Colour (0xffffe6c8).withAlpha (jlimit (0.0f, 1.0f, alpha)));
    strokeLine (g, x - r, y, x + r, y, 1.0f);
    strokeLine (g, x, y - r, x, y + r, 1.0f);
    const auto q = r * 0.42f;
    g.setColour (Colour (0xffffe6c8).withAlpha (jlimit (0.0f, 1.0f, alpha * 0.5f)));
    strokeLine (g, x - q, y - q, x + q, y + q, 1.0f);
    strokeLine (g, x + q, y - q, x - q, y + q, 1.0f);
    g.setColour (Colours::white.withAlpha (jlimit (0.0f, 1.0f, alpha)));
    g.fillRect (x - 1.0f, y - 1.0f, 2.0f, 2.0f);
}

/** La couleur d'un segment de vumètre allumé : de la braise sombre en bas au blanc chaud en haut. */
Colour meterColour (int segment)
{
    const auto f = (float) segment / (float) (kMeterSegments - 1);

    if (f < 0.55f)
    {
        const auto t = f / 0.55f;
        return Colour::fromFloatRGBA (jmin (1.0f, 0.55f + 0.95f * t), 0.04f + 0.28f * t, 0.05f + 0.17f * t, 1.0f);
    }

    const auto t = (f - 0.55f) / 0.45f;
    return Colour::fromFloatRGBA (1.0f, jmin (1.0f, 0.32f + 0.78f * t), jmin (1.0f, 0.22f + 0.73f * t), 1.0f);
}
}

//==============================================================================
MimicEditor::MimicEditor (MimicProcessor& p)
    : AudioProcessorEditor (p), processor (p),
      knobs { { { "morph", 0.62f }, { "carve", 0.38f }, { "groove", 0.50f }, { "wet", 0.75f }, { "blend", 1.0f }, { "output", 0.889f } } },
      serif (FontOptions (Font::getDefaultSerifFontName(), 11.0f, Font::bold)),
      mono (FontOptions (Font::getDefaultMonospacedFontName(), 11.0f, Font::bold))
{
    for (auto& knob : knobs)
        knob.parameter = processor.apvts.getParameter (knob.id);

    freezeParameter = processor.apvts.getParameter ("freeze");
    sourceParameter = processor.apvts.getParameter ("source");
    inputParameter = processor.apvts.getParameter ("input");
    waveParameter = processor.apvts.getParameter ("wave");

    underSource = ImageCache::getFromMemory (BinaryData::under_png, BinaryData::under_pngSize);
    overSource = ImageCache::getFromMemory (BinaryData::over_png, BinaryData::over_pngSize);
    onSource = ImageCache::getFromMemory (BinaryData::over_on_png, BinaryData::over_on_pngSize);
    smoke = makeSmoke();

    for (auto& ember : embers)
        ember = { random.nextFloat(), random.nextFloat(), 0.02f + random.nextFloat() * 0.06f,
                  0.6f + random.nextFloat() * 1.6f, random.nextFloat() * 6.0f };

    peakHold.assign (161, 0.0f);
    frozen = freezeParameter->getValue() > 0.5f;

    setOpaque (true);
    setWantsKeyboardFocus (true);

    // La licence passe au-dessus de tout : ajoutée en dernier.
    licenseOverlay = std::make_unique<onirique::license::Overlay> ("MIMIC", processor.license);
    addAndMakeVisible (*licenseOverlay);

    setResizable (true, true);
    getConstrainer()->setFixedAspectRatio ((double) kBaseWidth / (double) kBaseHeight);
    setResizeLimits (kBaseWidth * 3 / 4, kBaseHeight * 3 / 4, kBaseWidth * 17 / 10, kBaseHeight * 17 / 10);
    setSize (kBaseWidth, kBaseHeight);

    lastTick = Time::getMillisecondCounter();
    startTimerHz (30);
}

MimicEditor::~MimicEditor() { stopTimer(); }

void MimicEditor::resized()
{
    const auto s = scale();
    // The badge's place is given at the base size: the overlay applies the zoom itself.
    licenseOverlay->fit (getLocalBounds(), s, kPill.toNearestInt());
}

void MimicEditor::repaintBase (Rectangle<float> area)
{
    repaint ((area * scale()).getSmallestIntegerContainer().expanded (1));
}

//==============================================================================
void MimicEditor::rebuildLayers()
{
    const auto w = jmax (1, roundToInt ((float) kBaseWidth * layerScale));
    const auto h = jmax (1, roundToInt ((float) kBaseHeight * layerScale));

    auto fitted = [w, h] (const Image& source)
    {
        return source.getWidth() == w && source.getHeight() == h ? source
                                                                  : source.rescaled (w, h, Graphics::highResamplingQuality);
    };

    under = fitted (underSource);
    over = fitted (overSource);
    on = fitted (onSource);
}

void MimicEditor::paint (Graphics& g)
{
    const auto s = scale();

    // Le décor est tenu à la taille exacte de la fenêtre en pixels réels, sans dépasser celle des images.
    const auto wanted = jlimit (0.25f, 2.0f, s * (float) g.getInternalContext().getPhysicalPixelScaleFactor());

    if (std::abs (wanted - layerScale) > 0.004f || under.isNull())
    {
        layerScale = wanted;
        rebuildLayers();
    }

    const auto bounds = getLocalBounds().toFloat();
    const auto clip = g.getClipBounds().toFloat() / s;
    g.setImageResamplingQuality (Graphics::lowResamplingQuality);
    g.drawImage (under, bounds);

    {
        const Graphics::ScopedSaveState state (g);
        g.addTransform (AffineTransform::scale (s));

        if (clip.intersects (kCavity))
            drawCavity (g);
    }

    g.drawImage (over, bounds);

    // Les boutons allumés viennent de la même image que le décor, photographiée tous boutons allumés.
    for (int button = 0; button <= kFreezeButton; ++button)
    {
        if (! buttonIsOn (button) || ! clip.intersects (buttonArea (button)))
            continue;

        const auto dest = (buttonArea (button) * s).toNearestInt();
        const auto src = (buttonArea (button) * layerScale).toNearestInt();
        g.drawImage (on, dest.getX(), dest.getY(), dest.getWidth(), dest.getHeight(),
                     src.getX(), src.getY(), src.getWidth(), src.getHeight());
    }

    const Graphics::ScopedSaveState state (g);
    g.addTransform (AffineTransform::scale (s));

    // La lueur de la gueule suit le signal, le gel et le fichier qu'on approche.
    if (clip.intersects (kMawGlow))
    {
        const auto strength = jlimit (0.0f, 1.0f, 0.2f + 0.6f * energy + 0.5f * flash + 0.45f * hover) * 0.5f;
        const auto centre = Point<float> (kMawGlow.getCentreX(), kMawGlow.getY() + kMawGlow.getHeight() * 0.52f);
        ColourGradient glow (kEmber.withAlpha (0.55f * strength), centre,
                             kEmber.withAlpha (0.0f), centre.translated (kMawGlow.getWidth() * 0.52f, 0.0f), true);
        glow.addColour (0.55 / 0.8, kEmber.withAlpha (0.18f * strength));
        FillType fill (glow);
        fill.transform = AffineTransform::scale (1.0f, (kMawGlow.getHeight() * 0.55f) / (kMawGlow.getWidth() * 0.52f), centre.x, centre.y);
        g.setFillType (fill);
        g.fillRect (kMawGlow);
    }

    drawMeters (g);

    for (int i = 0; i < kNumKnobs; ++i)
        if (clip.intersects (knobArea (i)))
            drawKnob (g, i);
}

//==============================================================================
void MimicEditor::drawCavity (Graphics& g)
{
    const Graphics::ScopedSaveState state (g);
    g.reduceClipRegion (kCavity.getSmallestIntegerContainer());
    g.setOrigin (kCavity.getPosition().toInt());

    const auto W = kCavity.getWidth(), H = kCavity.getHeight();
    const auto cx = W * 0.5f, cy = H * 0.5f;
    const auto t = (float) time;

    g.setColour (kNight);
    g.fillRect (0.0f, 0.0f, W, H);

    // Fumée qui dérive et volutes de braise.
    {
        const auto ox = std::fmod (t * 9.0f, 512.0f), oy = std::sin (t * 0.3f) * 6.0f;
        g.setOpacity (jlimit (0.0f, 1.0f, 0.13f + 0.3f * energy));
        g.drawImage (smoke, Rectangle<float> (-ox, oy - 20.0f, 665.6f, H * 1.5f), RectanglePlacement::stretchToFit);
        g.drawImage (smoke, Rectangle<float> (665.6f - ox, oy - 20.0f, 665.6f, H * 1.5f), RectanglePlacement::stretchToFit);
        g.setOpacity (jlimit (0.0f, 1.0f, 0.08f + 0.2f * energy));
        const auto shift = std::fmod (t * 5.0f, W);
        g.drawImageTransformed (smoke, AffineTransform::scale (-W * 1.4f / (float) smoke.getWidth(), H * 1.2f / (float) smoke.getHeight())
                                           .translated (W + shift, -10.0f - oy));
        g.drawImageTransformed (smoke, AffineTransform::scale (-W * 1.4f / (float) smoke.getWidth(), H * 1.2f / (float) smoke.getHeight())
                                           .translated (W + shift - W * 1.4f, -10.0f - oy));
        g.setOpacity (1.0f);
    }

    for (int q = 0; q < 4; ++q)
    {
        Path wave;

        for (int c = 0; c <= 40; ++c)
        {
            const auto x = (float) c / 40.0f * W;
            const auto y = cy + std::sin ((float) c * 0.33f + t * (0.5f + (float) q * 0.17f) + (float) q * 2.0f) * H * (0.1f + 0.05f * (float) q) * (0.6f + energy)
                           + ((float) q - 1.5f) * H * 0.13f;
            if (c == 0) wave.startNewSubPath (x, y); else wave.lineTo (x, y);
        }

        g.setColour (kEmber.withAlpha (0.05f + 0.1f * energy));
        g.strokePath (wave, PathStrokeType (6.0f - (float) q));
    }

    {
        const Point<float> centre (cx, cy);
        ColourGradient glow (Colour (255, 92, 76).withAlpha (jmin (1.0f, 0.62f + 0.3f * energy)), centre,
                             kNight.withAlpha (0.0f), centre.translated (W * 0.46f, 0.0f), true);
        glow.addColour (0.18, kEmber.withAlpha (jmin (1.0f, 0.5f + 0.3f * energy)));
        glow.addColour (0.55, Colour (120, 14, 26).withAlpha (0.32f + 0.15f * energy));
        FillType fill (glow);
        fill.transform = AffineTransform::scale (1.0f, 0.62f, cx, cy);
        g.setFillType (fill);
        g.fillRect (0.0f, 0.0f, W, H);
    }

    for (size_t q = 0; q < embers.size(); ++q)
    {
        const auto& ember = embers[q];
        g.setColour ((q % 3 != 0 ? Colour (0xffff7a3c) : Colour (0xffffc060)).withAlpha (jlimit (0.0f, 1.0f, 0.25f + 0.3f * std::sin (t * 2.0f + ember.phase))));
        const auto r = jmax (0.6f, ember.radius * 0.7f);
        g.fillEllipse (ember.x * W - r, ember.y * H - r, 2.0f * r, 2.0f * r);
    }

    // Réticule.
    {
        const auto px = std::round (cx) + 0.5f, py = std::round (cy) + 0.5f;
        g.setColour (Colour (255, 214, 190).withAlpha (0.26f));

        for (const auto radius : { std::round (H * 0.36f) + 0.5f, std::round (H * 0.22f) + 0.5f, std::round (H * 0.07f) + 0.5f })
            g.drawEllipse (px - radius, py - radius, 2.0f * radius, 2.0f * radius, 1.0f);

        strokeLine (g, px - H * 0.46f, py, px + H * 0.46f, py, 1.0f);
        strokeLine (g, px, py - H * 0.46f, px, py + H * 0.46f, 1.0f);
        g.setColour (Colour (255, 214, 190).withAlpha (0.18f));

        for (int tick = 0; tick < 48; ++tick)
        {
            const auto angle = (float) tick * MathConstants<float>::pi / 24.0f;
            const auto length = tick % 4 != 0 ? 0.025f : 0.05f;
            strokeLine (g, px + std::cos (angle) * H * 0.36f, py + std::sin (angle) * H * 0.36f,
                        px + std::cos (angle) * H * (0.36f + length), py + std::sin (angle) * H * (0.36f + length), 1.0f);
        }
    }

    // L'onde : le timbre de la source en miroir autour du centre, grave au milieu.
    const auto cols = (int) peakHold.size();
    const auto amp = H * 0.47f;
    std::array<float, 161> env {}, raw {};

    for (int c = 0; c < cols; ++c)
    {
        const auto u = std::abs ((float) c / (float) (cols - 1) * 2.0f - 1.0f);
        raw[(size_t) c] = jlimit (0.0f, 1.2f, sampleBands (timbre.data(), kBands, u));
        const auto shimmer = frozen ? 1.0f : 0.9f + random.nextFloat() * 0.1f + 0.1f * std::sin ((float) c * 0.45f + t * 5.5f + u * 6.0f);
        env[(size_t) c] = jmax (2.2f + 2.0f * (1.0f - u), (0.12f + raw[(size_t) c] * 1.25f) * amp * shimmer * (1.0f - 0.4f * u));
    }

    auto envelope = [&] (float k)
    {
        Path path;

        for (int c = 0; c < cols; ++c)
        {
            const auto x = (float) c / (float) (cols - 1) * W;
            if (c == 0) path.startNewSubPath (x, cy - env[(size_t) c] * k); else path.lineTo (x, cy - env[(size_t) c] * k);
        }

        for (int c = cols - 1; c >= 0; --c)
            path.lineTo ((float) c / (float) (cols - 1) * W, cy + env[(size_t) c] * k);

        path.closeSubPath();
        return path;
    };

    {
        ColourGradient body (kEmber.withAlpha (0.0f), 0.0f, cy - amp, kEmber.withAlpha (0.0f), 0.0f, cy + amp, false);
        body.addColour (0.2, kEmber.withAlpha (0.6f));
        body.addColour (0.4, Colour (240, 110, 50).withAlpha (0.7f));
        body.addColour (0.5, Colour (255, 196, 110).withAlpha (0.82f));
        body.addColour (0.6, Colour (240, 110, 50).withAlpha (0.7f));
        body.addColour (0.8, kEmber.withAlpha (0.6f));

        g.setGradientFill (body);
        g.setOpacity (0.22f);
        g.fillPath (envelope (1.35f)); // le halo, à la place du flou de la page
        g.setOpacity (1.0f);
        g.setGradientFill (body);
        g.fillPath (envelope (1.0f));
    }

    for (int c = 0; c < cols; c += 2)
    {
        const auto x = std::round ((float) c / (float) (cols - 1) * W) + 0.5f;
        const auto height = env[(size_t) c] * (1.12f + 0.2f * std::sin ((float) c * 0.9f + t * 4.5f));
        g.setColour (c % 6 == 0 ? Colour (255, 200, 110).withAlpha (0.55f) : kEmber.withAlpha (0.4f));
        strokeLine (g, x, cy - height, x, cy + height, 1.0f);
    }

    // Crêtes : les maxima du timbre, avec une retenue qui retombe.
    for (int c = 1; c < cols - 1; ++c)
    {
        const auto isPeak = raw[(size_t) c] > raw[(size_t) c - 1] && raw[(size_t) c] >= raw[(size_t) c + 1] && raw[(size_t) c] > 0.08f;
        auto& hold = peakHold[(size_t) c];
        hold = jmax (isPeak ? raw[(size_t) c] : 0.0f, hold - 0.012f);

        if (! isPeak && hold < 0.1f)
            continue;

        const auto u = std::abs ((float) c / (float) (cols - 1) * 2.0f - 1.0f);
        const auto x = std::round ((float) c / (float) (cols - 1) * W) + 0.5f;
        const auto top = (0.12f + hold * 1.25f) * amp * (1.0f - 0.4f * u) * 1.12f;
        g.setColour (Colour (255, 206, 130).withAlpha (jlimit (0.0f, 0.95f, 0.35f + hold)));
        strokeLine (g, x, cy - top, x, cy + top, 1.2f);
        g.setColour (Colour (255, 240, 210).withAlpha (0.9f));
        g.fillRect (x - 1.0f, cy - top - 1.0f, 3.0f, 2.0f);
        g.fillRect (x - 1.0f, cy + top - 1.0f, 3.0f, 2.0f);
    }

    g.setColour (Colour (240, 120, 50).withAlpha (0.7f));
    strokeLine (g, 0.0f, cy, W, cy, 2.6f);
    g.setColour (Colour (255, 220, 160).withAlpha (0.8f));
    strokeLine (g, 0.0f, cy, W, cy, 1.0f);

    // Le contour du synthé, en ambre : ce qui sort après MORPH et CARVE.
    {
        const auto wetAmount = knobs[3].parameter->getValue();
        g.setColour (Colour (255, 190, 100).withAlpha (0.3f + 0.65f * wetAmount));

        for (const auto side : { -1.0f, 1.0f })
        {
            Path outline;

            for (int c = 0; c < cols; ++c)
            {
                const auto u = std::abs ((float) c / (float) (cols - 1) * 2.0f - 1.0f);
                const auto x = (float) c / (float) (cols - 1) * W;
                const auto y = jlimit (0.0f, 1.3f, sampleBands (synth.data(), kBands, u)) * H * 0.36f * (1.0f - 0.35f * u);
                if (c == 0) outline.startNewSubPath (x, cy + side * y); else outline.lineTo (x, cy + side * y);
            }

            g.strokePath (outline, PathStrokeType (1.5f, PathStrokeType::curved));
        }
    }

    static constexpr float starAt[6][2] = { { 0.3f, 0.3f }, { 0.7f, 0.3f }, { 0.3f, 0.7f }, { 0.7f, 0.7f }, { 0.42f, 0.2f }, { 0.58f, 0.8f } };

    for (int i = 0; i < 6; ++i)
        star4 (g, std::round (starAt[i][0] * W), std::round (starAt[i][1] * H),
               (i < 4 ? 7.0f : 4.5f) + 2.0f * std::sin (t * 3.0f + (float) i * 1.7f) + 3.0f * energy,
               0.6f + 0.4f * std::sin (t * 2.0f + (float) i));

    if (flash > 0.0f)
    {
        g.setColour (Colour (255, 226, 200).withAlpha (jmin (1.0f, flash * 0.3f)));
        g.fillRect (0.0f, 0.0f, W, H);
    }

    {
        const Point<float> centre (cx, cy);
        ColourGradient vignette (Colours::transparentBlack, centre, Colours::black.withAlpha (0.7f), centre.translated (W * 0.6f, 0.0f), true);
        vignette.addColour (1.0 / 3.0, Colours::transparentBlack);
        g.setGradientFill (vignette);
        g.fillRect (0.0f, 0.0f, W, H);
    }

    // Le fichier déposé, ou ce qu'il faut en faire.
    String caption;

    if (dragOver)
        caption = String::fromUTF8 ("DÉPOSER LE FICHIER DANS LA GUEULE");
    else if (messageTime > 0.0f)
        caption = message;
    else if (clip.loaded && inputParameter->getValue() > 0.5f)
        caption = clip.name.upToLastOccurrenceOf (".", false, false).toUpperCase();

    if (caption.isNotEmpty())
    {
        g.setFont (serif);
        const auto area = Rectangle<float> (W * 0.2f, H - 48.0f, W * 0.6f, 14.0f);
        g.setColour (Colours::black.withAlpha (0.7f));
        g.drawText (caption, area.translated (0.0f, 1.0f), Justification::centred, true);
        g.setColour (kBone.withAlpha (0.92f));
        g.drawText (caption, area, Justification::centred, true);
    }
}

//==============================================================================
void MimicEditor::drawKnob (Graphics& g, int index) const
{
    const auto value = knobs[(size_t) index].parameter->getValue();
    const Point<float> centre (knobX (index), kKnobY);
    const auto lit = roundToInt (value * (float) (kDashes - 1)) + 1;

    // Les tirets s'allument de -135 degrés jusqu'à l'aiguille.
    for (int k = 0; k < kDashes; ++k)
    {
        const auto angle = degreesToRadians (-135.0f + (float) k * 270.0f / (float) (kDashes - 1));
        const Point<float> direction (std::sin (angle), -std::cos (angle));
        const Line<float> dash (centre + direction * 55.0f, centre + direction * 61.0f);

        if (k < lit)
        {
            g.setColour (kEmber.withAlpha (0.35f));
            g.drawLine (dash, 5.0f);
            g.setColour (kEmber);
            g.drawLine (dash, 2.2f);
        }
        else
        {
            g.setColour (Colour (0xff34363c));
            g.drawLine (dash, 2.2f);
        }
    }

    // L'aiguille : une lame d'acier avec son trait de braise.
    {
        const Graphics::ScopedSaveState state (g);
        g.addTransform (AffineTransform::rotation (degreesToRadians (-135.0f + 270.0f * value)).translated (centre.x, centre.y));

        Path blade;
        blade.startNewSubPath (0.0f, -50.0f);
        blade.lineTo (6.0f, -44.9f);
        blade.lineTo (6.0f, -27.0f);
        blade.lineTo (-6.0f, -27.0f);
        blade.lineTo (-6.0f, -44.9f);
        blade.closeSubPath();

        g.setColour (Colours::black.withAlpha (0.55f));
        g.fillPath (blade, AffineTransform::translation (1.0f, 2.5f));

        ColourGradient steel (Colour (0xff17120f), -6.0f, 0.0f, Colour (0xff17120f), 6.0f, 0.0f, false);
        steel.addColour (0.38, Colour (0xffa7a9b3));
        steel.addColour (0.48, Colour (0xffeceef4));
        steel.addColour (0.58, Colour (0xff5a5b64));
        g.setGradientFill (steel);
        g.fillPath (blade);

        g.setColour (kEmber.withAlpha (0.4f));
        g.fillRect (-2.5f, -45.0f, 5.0f, 14.0f);
        g.setColour (kEmber);
        g.fillRect (-1.0f, -44.0f, 2.0f, 12.0f);
    }

    // La valeur, tant que la souris est sur le sceau ou le tient.
    if (index == hovered || index == dragging)
    {
        const auto text = knobs[(size_t) index].parameter->getCurrentValueAsText();
        const auto width = GlyphArrangement::getStringWidth (mono, text) + 18.0f;
        const Rectangle<float> pill (centre.x - width * 0.5f, kKnobY - kKnobRadius - 4.0f - 22.0f, width, 22.0f);

        g.setColour (kEmber.withAlpha (0.25f));
        g.fillRoundedRectangle (pill.expanded (3.0f), 5.0f);
        g.setColour (Colour (0xff070304));
        g.fillRoundedRectangle (pill, 3.0f);
        g.setColour (kEmber);
        g.drawRoundedRectangle (pill, 3.0f, 1.0f);
        g.setColour (kBone);
        g.setFont (mono);
        g.drawText (text, pill, Justification::centred, false);
    }
}

void MimicEditor::drawMeters (Graphics& g) const
{
    const auto clip = g.getClipBounds().toFloat();

    auto draw = [&] (float x0, const float* levels)
    {
        if (! clip.intersects (meterArea (x0)))
            return;

        for (int c = 0; c < 2; ++c)
            for (int j = 0; j < kMeterSegments; ++j)
            {
                if (((float) j + 0.5f) / (float) kMeterSegments > levels[c])
                    break;

                const Rectangle<float> segment (x0 + 6.0f + (float) c * 18.0f, 275.0f - ((float) j + 0.5f) * 5.0f - 1.6f, 14.0f, 3.2f);
                g.setColour (meterColour (j).withAlpha (0.3f));
                g.fillRect (segment.expanded (1.5f, 1.0f));
                g.setColour (meterColour (j));
                g.fillRect (segment);
            }
    };

    draw (kMeterLeft, meterIn);
    draw (kMeterRight, meterOut);

    // Le témoin de saturation, sous le mot OUTPUT.
    if (clipHold > 0.0f)
    {
        const Graphics::ScopedSaveState state (g);
        g.addTransform (AffineTransform::rotation (MathConstants<float>::pi * 0.25f, 871.0f, 128.0f));
        g.setColour (kEmber.withAlpha (0.45f));
        g.fillRect (871.0f - 8.0f, 128.0f - 8.0f, 16.0f, 16.0f);
        g.setColour (kBone);
        g.fillRect (871.0f - 5.0f, 128.0f - 5.0f, 10.0f, 10.0f);
    }
}

//==============================================================================
void MimicEditor::timerCallback()
{
    const auto now = Time::getMillisecondCounter();
    const auto dt = jlimit (0.0f, 0.05f, (float) (now - lastTick) / 1000.0f);
    lastTick = now;
    time += dt;

    const auto vis = processor.getVisual();

    // Le timbre de la source et le spectre du synthé, comme la page d'origine les lisait.
    const auto decay = std::exp (-dt * 10.0f);

    for (int i = 0; i < kBands; ++i)
    {
        const auto s = jlimit (0.0f, 1.0f, bandAt (vis.src, i));
        live[(size_t) i] = jmax (s * s, live[(size_t) i] * decay);
        const auto y = jlimit (0.0f, 1.0f, bandAt (vis.synth, i));
        synth[(size_t) i] = y * y;
    }

    energy += (vis.energy - energy) * (1.0f - std::exp (-dt * 14.0f));

    const auto nowFrozen = freezeParameter->getValue() > 0.5f;

    if (nowFrozen && ! frozen)
    {
        frozenBands = live;
        flash = 1.0f;
    }

    frozen = nowFrozen;
    timbre = frozen ? frozenBands : live;

    for (auto& ember : embers)
    {
        ember.y -= ember.speed * 0.016f * (0.6f + energy) * (dt * 30.0f);

        if (ember.y < -0.05f)
        {
            ember.y = 1.05f;
            ember.x = random.nextFloat();
        }
    }

    const auto oldGlow = 0.2f + 0.6f * energy + 0.5f * flash + 0.45f * hover;
    flash = jmax (0.0f, flash - dt * 2.4f);
    bite = jmax (0.0f, bite - dt);
    hover += ((dragOver ? 1.0f : 0.0f) - hover) * (1.0f - std::exp (-dt * 12.0f));
    messageTime = jmax (0.0f, messageTime - dt);

    // Vumètres : montée immédiate, descente régulière.
    auto follow = [dt] (float& shown, float target) { shown = jmax (jmin (1.0f, target), shown - dt * 1.4f); };

    auto litSegments = [] (const float* levels)
    {
        return (int) (levels[0] * (float) kMeterSegments + 0.5f) * 64 + (int) (levels[1] * (float) kMeterSegments + 0.5f);
    };

    const auto inBefore = litSegments (meterIn), outBefore = litSegments (meterOut);
    follow (meterIn[0], vis.inPk[0]);
    follow (meterIn[1], vis.inPk[1]);
    follow (meterOut[0], vis.outPk[0]);
    follow (meterOut[1], vis.outPk[1]);

    if (litSegments (meterIn) != inBefore)
        repaintBase (meterArea (kMeterLeft));

    if (litSegments (meterOut) != outBefore)
        repaintBase (meterArea (kMeterRight));

    const auto clipBefore = clipHold > 0.0f;
    clipHold = vis.clip > 0.5f ? 0.4f : jmax (0.0f, clipHold - dt);

    if ((clipHold > 0.0f) != clipBefore)
        repaintBase ({ 858.0f, 115.0f, 26.0f, 26.0f });

    // Le visualiseur bouge sans cesse ; la lueur autour n'est redessinée que si elle change.
    if (std::abs (oldGlow - (0.2f + 0.6f * energy + 0.5f * flash + 0.45f * hover)) > 0.004f)
        repaintBase (kMawGlow);
    else
        repaintBase (kCavity);

    // Paramètres changés par l'hôte ou par un autre geste.
    for (int i = 0; i < kNumKnobs; ++i)
    {
        const auto value = knobs[(size_t) i].parameter->getValue();

        if (! approximatelyEqual (value, knobs[(size_t) i].shown))
        {
            knobs[(size_t) i].shown = value;
            repaintBase (knobArea (i));
        }
    }

    int states = 0;

    for (int button = 0; button <= kFreezeButton; ++button)
        states |= buttonIsOn (button) ? 1 << button : 0;

    if (states != buttonStates)
    {
        buttonStates = states;
        repaintBase (kSourceSlot.getUnion (kWaveSlot));
        repaintBase (kFreezeSlot);
    }

    if (const auto serial = processor.getClipSerial(); serial != clipSerial)
    {
        const auto first = clipSerial < 0;
        clipSerial = serial;
        clip = processor.getClipInfo();

        if (! first)
        {
            if (clip.error.isNotEmpty())
            {
                message = ("FICHIER REFUSÉ : " + clip.error).toUpperCase();
                messageTime = 3.5f;
            }
            else if (clip.loaded)
            {
                bite = 0.32f;
                flash = 1.0f;
            }
        }
    }
}

//==============================================================================
int MimicEditor::knobAt (Point<float> base) const
{
    for (int i = 0; i < kNumKnobs; ++i)
        if (base.getDistanceFrom ({ knobX (i), kKnobY }) <= kKnobRadius)
            return i;

    return -1;
}

int MimicEditor::buttonAt (Point<float> base) const
{
    for (int button = 0; button <= kFreezeButton; ++button)
        if (buttonArea (button).contains (base))
            return button;

    return -1;
}

bool MimicEditor::buttonIsOn (int button) const
{
    if (button < 3)
        return inputParameter->getValue() < 0.5f && roundToInt (sourceParameter->convertFrom0to1 (sourceParameter->getValue())) == button;

    if (button < kFreezeButton)
        return roundToInt (waveParameter->convertFrom0to1 (waveParameter->getValue())) == button - 3;

    return freezeParameter->getValue() > 0.5f;
}

void MimicEditor::setChoice (const char* id, int index)
{
    if (auto* parameter = processor.apvts.getParameter (id))
    {
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost (parameter->convertTo0to1 ((float) index));
        parameter->endChangeGesture();
    }
}

void MimicEditor::toggleFreeze()
{
    freezeParameter->beginChangeGesture();
    freezeParameter->setValueNotifyingHost (freezeParameter->getValue() > 0.5f ? 0.0f : 1.0f);
    freezeParameter->endChangeGesture();
}

void MimicEditor::pressButton (int button)
{
    if (button < 3)
    {
        // Choisir une source rend la main au sidechain si un fichier jouait.
        setChoice ("source", button);

        if (inputParameter->getValue() > 0.5f)
            setChoice ("input", MimicProcessor::kInputSidechain);
    }
    else if (button < kFreezeButton)
        setChoice ("wave", button - 3);
    else
        toggleFreeze();
}

void MimicEditor::setKnob (int index, float value)
{
    knobs[(size_t) index].parameter->setValueNotifyingHost (jlimit (0.0f, 1.0f, value));
}

void MimicEditor::mouseDown (const MouseEvent& e)
{
    const auto base = toBase (e.position);

    if (const auto button = buttonAt (base); button >= 0)
    {
        pressButton (button);
        return;
    }

    dragging = knobAt (base);

    if (dragging >= 0)
    {
        dragStartValue = knobs[(size_t) dragging].parameter->getValue();
        knobs[(size_t) dragging].parameter->beginChangeGesture();
        repaintBase (knobArea (dragging));
    }
}

void MimicEditor::mouseDrag (const MouseEvent& e)
{
    if (dragging >= 0)
        setKnob (dragging, dragStartValue - (float) e.getDistanceFromDragStartY() / scale() / (e.mods.isShiftDown() ? 700.0f : 200.0f));
}

void MimicEditor::mouseUp (const MouseEvent& e)
{
    if (dragging >= 0)
    {
        knobs[(size_t) dragging].parameter->endChangeGesture();
        repaintBase (knobArea (dragging));
        dragging = -1;
        mouseMove (e);
    }
}

void MimicEditor::mouseMove (const MouseEvent& e)
{
    const auto base = toBase (e.position);
    const auto now = knobAt (base);

    if (now != hovered)
    {
        if (hovered >= 0) repaintBase (knobArea (hovered));
        if (now >= 0) repaintBase (knobArea (now));
        hovered = now;
    }

    setMouseCursor (now >= 0 ? MouseCursor::UpDownResizeCursor
                             : (buttonAt (base) >= 0 ? MouseCursor::PointingHandCursor : MouseCursor::NormalCursor));
}

void MimicEditor::mouseExit (const MouseEvent&)
{
    if (hovered >= 0 && dragging < 0)
    {
        repaintBase (knobArea (hovered));
        hovered = -1;
    }
}

void MimicEditor::mouseDoubleClick (const MouseEvent& e)
{
    if (const auto index = knobAt (toBase (e.position)); index >= 0)
    {
        auto& knob = knobs[(size_t) index];
        knob.parameter->beginChangeGesture();
        setKnob (index, knob.defaultValue);
        knob.parameter->endChangeGesture();
    }
}

void MimicEditor::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& wheel)
{
    if (const auto index = knobAt (toBase (e.position)); index >= 0 && ! approximatelyEqual (wheel.deltaY, 0.0f))
    {
        auto& knob = knobs[(size_t) index];
        knob.parameter->beginChangeGesture();
        setKnob (index, knob.parameter->getValue() + (wheel.deltaY > 0.0f ? 0.02f : -0.02f));
        knob.parameter->endChangeGesture();
    }
}

bool MimicEditor::keyPressed (const KeyPress& key)
{
    if (key == KeyPress::spaceKey)
    {
        toggleFreeze();
        return true;
    }

    // Les flèches règlent le sceau sous la souris.
    if (hovered < 0)
        return false;

    auto& knob = knobs[(size_t) hovered];
    const auto step = key.getModifiers().isShiftDown() ? 0.005f : 0.02f;
    auto value = knob.parameter->getValue();

    if (key.isKeyCode (KeyPress::upKey) || key.isKeyCode (KeyPress::rightKey))       value += step;
    else if (key.isKeyCode (KeyPress::downKey) || key.isKeyCode (KeyPress::leftKey)) value -= step;
    else if (key.isKeyCode (KeyPress::homeKey))                                      value = 0.0f;
    else if (key.isKeyCode (KeyPress::endKey))                                       value = 1.0f;
    else return false;

    knob.parameter->beginChangeGesture();
    setKnob (hovered, value);
    knob.parameter->endChangeGesture();
    return true;
}

//==============================================================================
bool MimicEditor::isInterestedInFileDrag (const StringArray& files)
{
    return files.size() > 0 && File (files[0]).hasFileExtension ("wav;wave;flac;mp3");
}

void MimicEditor::fileDragEnter (const StringArray& files, int x, int y) { fileDragMove (files, x, y); }

void MimicEditor::fileDragMove (const StringArray&, int x, int y)
{
    dragOver = kCavity.expanded (10.0f).contains (toBase (Point<int> (x, y).toFloat()));
}

void MimicEditor::fileDragExit (const StringArray&) { dragOver = false; }

void MimicEditor::filesDropped (const StringArray& files, int x, int y)
{
    const auto inside = kCavity.expanded (10.0f).contains (toBase (Point<int> (x, y).toFloat()));
    dragOver = false;

    if (inside && files.size() > 0)
        processor.loadFile (File (files[0]));
}

} // namespace mimic
