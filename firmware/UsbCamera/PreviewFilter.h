// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "CameraOrientation.h"

// Small integer-only viewfinder pipeline. The output is SSD1306 page order.
namespace PreviewFilter {
constexpr int Width = 128;
constexpr int Height = 64;
enum class Mode : uint8_t { Normal, CombinedSobel, SobelNms };
constexpr int EdgeThreshold = 75;

// Return the next mode in the three-mode shutter-button cycle.
inline Mode nextMode(Mode mode) {
    switch (mode) {
        case Mode::Normal:
            return Mode::CombinedSobel;
        case Mode::CombinedSobel:
            return Mode::SobelNms;
        default:
            return Mode::Normal;
    }
}

// Limit an integer to an inclusive range.
inline int clamp(int value, int minimum, int maximum) {
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

// Reuse fixed-size buffers across frames; only Normal mode keeps contrast history.
// Q8 tone limits store intensity multiplied by 256 for fractional smoothing.
struct State {
    uint8_t grayscalePixels[Width * Height];
    uint32_t scratchBuffer[(Width + 1) * (Height + 1)];
    int16_t diffusionErrors[2][Width + 2];
    int blackPointQ8 = -1;
    int whitePointQ8 = 0;

    void reset() {
        blackPointQ8 = -1;
    }
};

// Use four summed-area-table entries to average a clipped square neighborhood.
inline int boxMean(const State& state, int x, int y, int radius) {
    int left = clamp(x - radius, 0, Width);
    int right = clamp(x + radius + 1, 0, Width);
    int top = clamp(y - radius, 0, Height);
    int bottom = clamp(y + radius + 1, 0, Height);
    const uint32_t* summedAreaTable = state.scratchBuffer;
    return int(summedAreaTable[bottom * (Width + 1) + right] +
               summedAreaTable[top * (Width + 1) + left] -
               summedAreaTable[top * (Width + 1) + right] -
               summedAreaTable[bottom * (Width + 1) + left]) /
           ((right - left) * (bottom - top));
}

// Sobel 3x3 gradients on grayscale, before dithering adds artificial edges.
// Gx = [-1 0 1; -2 0 2; -1 0 1], Gy = [-1 -2 -1; 0 0 0; 1 2 1].
// |Gx| + |Gy| approximates gradient magnitude without floating-point square roots.
inline void renderEdges(const uint8_t* grayscalePixels, uint8_t* oledBuffer) {
    memset(oledBuffer, 0, Width * Height / 8);
    for (int y = 0; y < Height; y++) {
        for (int x = 0; x < Width; x++) {
            int leftX = clamp(x - 1, 0, Width - 1);
            int rightX = clamp(x + 1, 0, Width - 1);
            int topY = clamp(y - 1, 0, Height - 1);
            int bottomY = clamp(y + 1, 0, Height - 1);
            int topLeft = grayscalePixels[topY * Width + leftX];
            int topCenter = grayscalePixels[topY * Width + x];
            int topRight = grayscalePixels[topY * Width + rightX];
            int middleLeft = grayscalePixels[y * Width + leftX];
            int middleRight = grayscalePixels[y * Width + rightX];
            int bottomLeft = grayscalePixels[bottomY * Width + leftX];
            int bottomCenter = grayscalePixels[bottomY * Width + x];
            int bottomRight = grayscalePixels[bottomY * Width + rightX];
            int gx =
                -topLeft + topRight - 2 * middleLeft + 2 * middleRight - bottomLeft + bottomRight;
            int gy =
                -topLeft - 2 * topCenter - topRight + bottomLeft + 2 * bottomCenter + bottomRight;
            int magnitude = (gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy);
            if (magnitude >= EdgeThreshold) {
                oledBuffer[(y / 8) * Width + x] |= uint8_t(1U << (y & 7));
            }
        }
    }
}

// Quantize the gradient (across the edge) to 0, 45, 90 or 135 degrees.
// tan(22.5 degrees) is approximately 13573/32768. Integer comparisons avoid
// atan2; reversing both gradient signs keeps the same comparison axis.
// Return 0: left/right, 1: upper-left/lower-right, 2: above/below,
// or 3: upper-right/lower-left. Image Y increases downward.
inline uint8_t gradientDirection(int gx, int gy) {
    int absGx = gx < 0 ? -gx : gx;
    int absGy = gy < 0 ? -gy : gy;
    if (absGy * 32768 <= absGx * 13573) {
        return 0;
    }
    if (absGx * 32768 <= absGy * 13573) {
        return 2;
    }
    return (gx < 0) == (gy < 0) ? 1 : 3;
}

// Compute Sobel strengths, then retain local maxima along four gradient axes.
// The low 16 bits store magnitude; the high bits store the comparison direction.
inline void renderSobelNms(State& state, uint8_t* oledBuffer) {
    // Normal's integral-image buffer is unused in this mode. Reuse it for packed
    // magnitude/direction values, adding no frame buffer or per-frame allocation.
    static_assert((Width + 1) * (Height + 1) >= Width * Height, "Edge scratch capacity");
    uint32_t* packedEdges = state.scratchBuffer;
    const uint8_t* grayscalePixels = state.grayscalePixels;
    for (int y = 0; y < Height; y++) {
        for (int x = 0; x < Width; x++) {
            int leftX = clamp(x - 1, 0, Width - 1);
            int rightX = clamp(x + 1, 0, Width - 1);
            int topY = clamp(y - 1, 0, Height - 1);
            int bottomY = clamp(y + 1, 0, Height - 1);
            int topLeft = grayscalePixels[topY * Width + leftX];
            int topCenter = grayscalePixels[topY * Width + x];
            int topRight = grayscalePixels[topY * Width + rightX];
            int middleLeft = grayscalePixels[y * Width + leftX];
            int middleRight = grayscalePixels[y * Width + rightX];
            int bottomLeft = grayscalePixels[bottomY * Width + leftX];
            int bottomCenter = grayscalePixels[bottomY * Width + x];
            int bottomRight = grayscalePixels[bottomY * Width + rightX];
            int gx =
                -topLeft + topRight - 2 * middleLeft + 2 * middleRight - bottomLeft + bottomRight;
            int gy =
                -topLeft - 2 * topCenter - topRight + bottomLeft + 2 * bottomCenter + bottomRight;
            int magnitude = (gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy);
            packedEdges[y * Width + x] =
                uint32_t(magnitude) | (uint32_t(gradientDirection(gx, gy)) << 16);
        }
    }
    memset(oledBuffer, 0, Width * Height / 8);
    constexpr int neighborOffsets[4] = {1, Width + 1, Width, Width - 1};
    // Keep the outermost border dark: each candidate needs two real neighbors.
    for (int y = 1; y < Height - 1; y++) {
        for (int x = 1; x < Width - 1; x++) {
            int pixelIndex = y * Width + x;
            uint32_t packedEdge = packedEdges[pixelIndex];
            int magnitude = int(packedEdge & 0xffff);
            if (magnitude < EdgeThreshold) {
                continue;
            }
            int neighborOffset = neighborOffsets[packedEdge >> 16];
            int previousMagnitude = int(packedEdges[pixelIndex - neighborOffset] & 0xffff);
            int nextMagnitude = int(packedEdges[pixelIndex + neighborOffset] & 0xffff);
            // Resolve equal-strength plateaus consistently, independent of edge polarity.
            if (magnitude > previousMagnitude && magnitude >= nextMagnitude) {
                oledBuffer[(y / 8) * Width + x] |= uint8_t(1U << (y & 7));
            }
        }
    }
}

// Rotate, mirror, and crop grayscale input to the OLED grid, then apply the selected mode.
// Write a 1024-byte SSD1306 page buffer. Return false for invalid input dimensions.
inline bool render(State& state, const uint8_t* sourcePixels, int sourceWidth, int sourceHeight,
                   uint8_t* oledBuffer, Mode mode = Mode::Normal) {
    if (!sourcePixels || !oledBuffer || sourceWidth < 2 || sourceHeight < 2) {
        return false;
    }
    // Rotate 90 degrees CCW, mirror left/right, then fill a centered 2:1 crop.
    const int cropWidth = sourceHeight;
    const int cropHeight = sourceHeight / 2;
    const int cropTop = (sourceWidth - cropHeight) / 2;
    if (cropHeight < 1 || cropTop < 0) {
        return false;
    }
    uint16_t histogram[256] = {};
    for (int y = 0; y < Height; y++) {
        for (int x = 0; x < Width; x++) {
            // Two samples per axis cover the pixel footprint instead of adding blur
            // outside it. They work for both VGA preview and the full-size fallback.
            int sampleRow0 = clamp((4 * x + 1) * cropWidth / (4 * Width), 0, sourceHeight - 1);
            int sampleRow1 = clamp((4 * x + 3) * cropWidth / (4 * Width), 0, sourceHeight - 1);
            int sampleColumn0 =
                clamp(cropTop + (4 * y + 1) * cropHeight / (4 * Height), 0, sourceWidth - 1);
            int sampleColumn1 =
                clamp(cropTop + (4 * y + 3) * cropHeight / (4 * Height), 0, sourceWidth - 1);
            int averagedGray =
                (sourcePixels[sampleRow0 * sourceWidth + sourceWidth - 1 - sampleColumn0] +
                 sourcePixels[sampleRow1 * sourceWidth + sourceWidth - 1 - sampleColumn0] +
                 sourcePixels[sampleRow0 * sourceWidth + sourceWidth - 1 - sampleColumn1] +
                 sourcePixels[sampleRow1 * sourceWidth + sourceWidth - 1 - sampleColumn1] + 2) /
                4;
            // Match the JPEG orientation before either Normal or Sobel processing.
            int displayX = CameraOrientation::kMirrorHorizontal ? Width - 1 - x : x;
            state.grayscalePixels[y * Width + displayX] = averagedGray;
            histogram[averagedGray]++;
        }
    }
    if (mode == Mode::CombinedSobel) {
        renderEdges(state.grayscalePixels, oledBuffer);
        return true;
    }
    if (mode == Mode::SobelNms) {
        renderSobelNms(state, oledBuffer);
        return true;
    }
    // Clip only the extreme 2% and limit gain in flat/dark scenes. Smooth the
    // tone limits, not the picture, so movement does not acquire trailing ghosts.
    int blackPoint = 0;
    int whitePoint = 255;
    int histogramCount = 0;
    for (int intensity = 0; intensity < 256; intensity++) {
        histogramCount += histogram[intensity];
        if (histogramCount > Width * Height / 50) {
            blackPoint = intensity;
            break;
        }
    }
    histogramCount = 0;
    for (int intensity = 255; intensity >= 0; intensity--) {
        histogramCount += histogram[intensity];
        if (histogramCount > Width * Height / 50) {
            whitePoint = intensity;
            break;
        }
    }
    int contrastRange = whitePoint - blackPoint;
    if (contrastRange < 80) {
        int midpoint = (blackPoint + whitePoint) / 2;
        blackPoint = clamp(midpoint - 40, 0, 175);
        whitePoint = blackPoint + 80;
    }
    if (state.blackPointQ8 < 0) {
        state.blackPointQ8 = blackPoint * 256;
        state.whitePointQ8 = whitePoint * 256;
    } else {
        state.blackPointQ8 += (blackPoint * 256 - state.blackPointQ8) / 4;
        state.whitePointQ8 += (whitePoint * 256 - state.whitePointQ8) / 4;
    }
    blackPoint = state.blackPointQ8 / 256;
    whitePoint = state.whitePointQ8 / 256;
    contrastRange = clamp(whitePoint - blackPoint, 80, 255);
    memset(state.scratchBuffer, 0, (Width + 1) * sizeof(uint32_t));
    for (int y = 0; y < Height; y++) {
        uint32_t rowSum = 0;
        state.scratchBuffer[(y + 1) * (Width + 1)] = 0;
        for (int x = 0; x < Width; x++) {
            rowSum += state.grayscalePixels[y * Width + x];
            state.scratchBuffer[(y + 1) * (Width + 1) + x + 1] =
                rowSum + state.scratchBuffer[y * (Width + 1) + x + 1];
        }
    }
    memset(oledBuffer, 0, Width * Height / 8);
    memset(state.diffusionErrors, 0, sizeof(state.diffusionErrors));
    for (int y = 0; y < Height; y++) {
        int16_t* currentRowErrors = state.diffusionErrors[y & 1];
        int16_t* nextRowErrors = state.diffusionErrors[(y + 1) & 1];
        memset(nextRowErrors, 0, sizeof(state.diffusionErrors[0]));
        const int scanDirection = (y & 1) ? -1 : 1;
        for (int x = (scanDirection > 0 ? 0 : Width - 1); x >= 0 && x < Width; x += scanDirection) {
            int rawGray = state.grayscalePixels[y * Width + x];
            int globalTone = clamp((rawGray - blackPoint) * 255 / contrastRange, 0, 255);
            int localContrast =
                clamp((rawGray - boxMean(state, x, y, 8)) * 128 / contrastRange, -32, 32);
            // Preserve solid blacks/whites; add bounded local contrast to show detail.
            // Thresholded unsharp masking emphasizes edges without boosting tiny noise.
            int detailDifference = rawGray - boxMean(state, x, y, 1);
            detailDifference = detailDifference > 3
                                   ? detailDifference - 3
                                   : (detailDifference < -3 ? detailDifference + 3 : 0);
            int sharpeningBoost = clamp(detailDifference * 192 / contrastRange, -36, 36);
            int adjustedTone = clamp(globalTone + localContrast + sharpeningBoost, 0, 255);
            // Serpentine Floyd-Steinberg: spread quantization error 7/16,3/16,5/16,1/16.
            int diffusedTone = clamp(adjustedTone + currentRowErrors[x + 1] / 16, 0, 255);
            int outputTone = diffusedTone >= 128 ? 255 : 0;
            int quantizationError = diffusedTone - outputTone;
            if (outputTone) {
                oledBuffer[(y / 8) * Width + x] |= uint8_t(1U << (y & 7));
            }
            currentRowErrors[x + 1 + scanDirection] += quantizationError * 7;
            nextRowErrors[x + 1 - scanDirection] += quantizationError * 3;
            nextRowErrors[x + 1] += quantizationError * 5;
            nextRowErrors[x + 1 + scanDirection] += quantizationError;
        }
    }
    return true;
}
} // namespace PreviewFilter
