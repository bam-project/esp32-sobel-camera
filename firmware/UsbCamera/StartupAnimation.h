// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <U8g2lib.h>

// A short moonrise over the Himalayas before the live viewfinder starts.
class StartupAnimation {
public:
    static constexpr uint32_t DurationMs = 3000;

    // Restart the six-frame moonrise and invalidate the previous rendered frame.
    void start(uint32_t nowMs) {
        startedAtMs = nowMs;
        active = true;
        lastFrameIndex = 255;
    }

    // Release the display so another animation or the live preview can draw.
    void cancel() {
        active = false;
    }

    // Draw only changed frames; return true while the animation owns the display.
    bool update(U8G2& display, uint32_t nowMs) {
        if (!active) {
            return false;
        }
        uint32_t elapsedMs = nowMs - startedAtMs;
        if (elapsedMs >= DurationMs) {
            active = false;
            return false;
        }
        uint8_t frameIndex = elapsedMs * 6 / DurationMs;
        if (frameIndex == lastFrameIndex) {
            return true;
        }
        lastFrameIndex = frameIndex;
        drawFrame(display, frameIndex);
        display.sendBuffer();
        return true;
    }

private:
    bool active = false;
    uint32_t startedAtMs = 0;
    uint8_t lastFrameIndex = 255;

    // Draw a point or a bright cross for the twinkling star effect.
    static void drawStar(U8G2& display, int x, int y, bool bright) {
        display.drawPixel(x, y);
        if (bright) {
            display.drawHLine(x - 2, y, 5);
            display.drawVLine(x, y - 2, 5);
        }
    }

    // Render the moonrise and mountain silhouettes into the OLED buffer.
    static void drawFrame(U8G2& display, uint8_t frameIndex) {
        display.clearBuffer();
        display.setDrawColor(1);
        // Draw the sky first, then mask the mountain interiors so the moon emerges
        // from behind the ridge. All coordinates fit the 128 x 64 OLED.
        int moonY = 36 - frameIndex * 3;
        display.drawDisc(90, moonY, 6);
        display.setDrawColor(0);
        display.drawDisc(93, moonY - 2, 6);
        display.setDrawColor(1);
        drawStar(display, 26, 23, frameIndex % 2 == 0);
        drawStar(display, 108, 20, frameIndex % 2 != 0);
        display.setDrawColor(0);
        display.drawTriangle(8, 44, 37, 23, 67, 44);
        display.drawTriangle(41, 44, 68, 28, 99, 44);
        display.drawTriangle(76, 44, 99, 33, 119, 44);
        display.setDrawColor(1);
        display.drawLine(8, 44, 37, 23);
        display.drawLine(37, 23, 67, 44);
        display.drawLine(51, 38, 68, 28);
        display.drawLine(68, 28, 99, 44);
        display.drawLine(86, 39, 99, 33);
        display.drawLine(99, 33, 119, 44);
        display.drawLine(29, 29, 34, 31);
        display.drawLine(34, 31, 38, 28);
        display.drawLine(38, 28, 44, 29);
    }
};
