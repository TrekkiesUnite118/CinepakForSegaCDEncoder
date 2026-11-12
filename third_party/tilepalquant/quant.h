#pragma once

#include <string>
#include <cstring>

#include "options.h"
#include "image.h"

using namespace std;

struct Candidate {
    int       colorIndex;
    double    colorDistance;
    rgbColor  comparedColor;
    double    brightness;
};

// scdcpk: result is returned in *result instead of being written to a PNG
int quantizeImage(quantOptions & quantizationOptions, Image & image, Image * result);

// scdcpk: map an image onto existing palettes (paletteData: numPalettes x colorsPerPalette)
int quantizeImageWithPalettes(quantOptions & quantizationOptions, Image & image,
                              const vector <rgbColorU8> & paletteData, Image * result);
