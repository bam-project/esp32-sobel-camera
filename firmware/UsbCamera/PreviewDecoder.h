// SPDX-License-Identifier: MIT
#pragma once
#include <JPEGDEC.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// JPEGDEC supplies packed grayscale tiles. Copy bounded rows into a small image;
// use tile callbacks so decoder padding never changes the destination stride.
class PreviewDecoder {
    JPEGDEC jpegDecoder;
    uint8_t* grayscaleOutput = nullptr;
    bool tileCopyFailed = false;

    // Copy each JPEGDEC tile into the row-major output buffer, excluding tile padding.
    // Return zero if a tile would exceed the decoded image bounds.
    static int copyGrayscaleTile(JPEGDRAW* tile) {
        auto decoder = static_cast<PreviewDecoder*>(tile->pUser);
        if (!decoder || tile->iBpp != 8 || tile->x < 0 || tile->y < 0 || tile->iWidthUsed <= 0 ||
            tile->iHeight <= 0 || tile->iWidthUsed > tile->iWidth ||
            tile->x + tile->iWidthUsed > decoder->decodedWidth ||
            tile->y + tile->iHeight > decoder->decodedHeight) {
            if (decoder) {
                decoder->tileCopyFailed = true;
            }
            return 0;
        }
        auto tilePixels = reinterpret_cast<const uint8_t*>(tile->pPixels);
        for (int tileRow = 0; tileRow < tile->iHeight; tileRow++) {
            memcpy(decoder->grayscaleOutput + (tile->y + tileRow) * decoder->decodedWidth + tile->x,
                   tilePixels + tileRow * tile->iWidth, tile->iWidthUsed);
        }
        return 1;
    }

public:
    int sourceWidth = 0;
    int sourceHeight = 0;
    int decodedWidth = 0;
    int decodedHeight = 0;

    // Decode VGA at half scale or UXGA at one-eighth scale using JPEG luminance only.
    // Return true only when both JPEG decoding and all tile copies succeed.
    bool decode(uint8_t* jpegData, size_t jpegLength, uint8_t* outputPixels,
                size_t outputCapacity) {
        if (!jpegData || !outputPixels || jpegLength > 0x7fffffff) {
            return false;
        }
        if (!jpegDecoder.openRAM(jpegData, int(jpegLength), copyGrayscaleTile)) {
            return false;
        }
        sourceWidth = jpegDecoder.getWidth();
        sourceHeight = jpegDecoder.getHeight();
        int scaleDivisor = 0;
        int scaleOption = 0;
        if (sourceWidth == 640 && sourceHeight == 480) {
            scaleDivisor = 2;
            scaleOption = JPEG_SCALE_HALF;
        } else if (sourceWidth == 1600 && sourceHeight == 1200) {
            scaleDivisor = 8;
            scaleOption = JPEG_SCALE_EIGHTH;
        }
        if (!scaleDivisor) {
            jpegDecoder.close();
            return false;
        }
        decodedWidth = sourceWidth / scaleDivisor;
        decodedHeight = sourceHeight / scaleDivisor;
        if (size_t(decodedWidth) * decodedHeight > outputCapacity) {
            jpegDecoder.close();
            return false;
        }
        grayscaleOutput = outputPixels;
        tileCopyFailed = false;
        jpegDecoder.setPixelType(EIGHT_BIT_GRAYSCALE);
        jpegDecoder.setUserPointer(this);
        bool decodedSuccessfully =
            jpegDecoder.decode(0, 0, scaleOption | JPEG_LUMA_ONLY) == 1 && !tileCopyFailed;
        jpegDecoder.close();
        grayscaleOutput = nullptr;
        return decodedSuccessfully;
    }
};
