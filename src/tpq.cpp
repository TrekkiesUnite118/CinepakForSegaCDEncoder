#include <cstdlib>
#include <cstring>
#include <cstdint>

#include "options.h"
#include "image.h"
#include "quant.h"
#include "tpq.h"

static const uint8_t ditherPatterns[Opts::ditherPatternsCount][DITHER_WIDTH][DITHER_HEIGHT] = {
    {{0, 2}, {3, 1}},
    {{0, 3}, {1, 2}},
    {{0, 1}, {3, 2}},
    {{0, 1}, {1, 0}},
    {{0, 1}, {0, 1}},
    {{0, 0}, {1, 1}}
};
static const uint8_t ditherPixelsSz[Opts::ditherPatternsCount] = { 4, 4, 4, 2, 2, 2 };

static void setup(quantOptions &q, const tpq_opts *o)
{
    q.tileWidth = 8;
    q.tileHeight = 8;
    q.numPalettes = (unsigned)o->npal;
    q.colorsPerPalette = 16;
    q.bitsPerChannel = 3;
    q.fractionOfPixels = o->fraction > 0 ? o->fraction : (float)FRACTION_OF_PIXELS_DEFAULT;
    q.colorZeroBehaviour = Opts::indexZeroShared;
    q.colorZeroValue = COLOR_ZERO_RGB_DEFAULT;
    q.sharedColorInput = SHARED_COLOR_RGB_DEFAULT;
    q.transparentColorInput = TRANSPARENT_COLOR_RGB_DEFAULT;
    q.ditherMethod = (unsigned)o->dither;
    q.ditherPatternType = (o->pattern >= 0 && o->pattern < Opts::ditherPatternsCount) ? (unsigned)o->pattern : 0;
    q.ditherWeight = o->weight > 0 ? o->weight : (float)DITHER_WEIGHT_DEFAULT;
    memcpy(q.ditherPattern, ditherPatterns[q.ditherPatternType], DITHER_PATTERN_AR_SZ);
    q.ditherPixels = ditherPixelsSz[q.ditherPatternType];
    q.totalPaletteColors = q.numPalettes * q.colorsPerPalette;
    q.randomSeed = o->seed;
    q.use_metafile = false;
    q.verbose = false;
    q.verboseDebug = false;
    q.exportPreviews = false;
}

static void toImage(Image &img, const uint8_t *rgb, int w, int h)
{
    img.width = (unsigned)w;
    img.height = (unsigned)h;
    img.data.resize((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        img.data[i * 4 + 0] = rgb[i * 3 + 0];
        img.data[i * 4 + 1] = rgb[i * 3 + 1];
        img.data[i * 4 + 2] = rgb[i * 3 + 2];
        img.data[i * 4 + 3] = 255;
    }
}

extern "C" int tpq_quantize(const tpq_opts *o, const uint8_t *rgb, int w, int h,
                            uint8_t *pal, uint8_t *idx)
{
    quantOptions q;
    Image img, res;
    setup(q, o);
    toImage(img, rgb, w, h);
    srand(q.randomSeed);
    if (quantizeImage(q, img, &res) != EXIT_SUCCESS)
        return -1;
    if (res.paletteData.size() < (size_t)o->npal * 16 || res.colorIndexes.size() != (size_t)w * h)
        return -1;
    for (int i = 0; i < o->npal * 16; i++) {
        pal[i * 3 + 0] = (i & 15) ? res.paletteData[i].ch.r : 0;
        pal[i * 3 + 1] = (i & 15) ? res.paletteData[i].ch.g : 0;
        pal[i * 3 + 2] = (i & 15) ? res.paletteData[i].ch.b : 0;
    }
    memcpy(idx, res.colorIndexes.data(), (size_t)w * h);
    return 0;
}

extern "C" int tpq_remap(const tpq_opts *o, const uint8_t *rgb, int w, int h,
                         const uint8_t *pal, uint8_t *idx)
{
    quantOptions q;
    Image img, res;
    vector<rgbColorU8> p((size_t)o->npal * 16);
    setup(q, o);
    toImage(img, rgb, w, h);
    for (size_t i = 0; i < p.size(); i++) {
        p[i].ch.r = pal[i * 3 + 0];
        p[i].ch.g = pal[i * 3 + 1];
        p[i].ch.b = pal[i * 3 + 2];
    }
    if (quantizeImageWithPalettes(q, img, p, &res) != EXIT_SUCCESS)
        return -1;
    memcpy(idx, res.colorIndexes.data(), (size_t)w * h);
    return 0;
}
