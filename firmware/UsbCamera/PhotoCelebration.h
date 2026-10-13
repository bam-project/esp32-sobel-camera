// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <U8g2lib.h>

// OLED-only feedback. Start this after the JPEG has been verified and renamed.
class PhotoCelebration {
public:
    static constexpr uint32_t DurationMs = 3000;
    static constexpr uint8_t CaptionCount = 8;

    // Start success feedback and advance the rotating caption once per saved photo.
    void start(uint32_t nowMs) {
        startedAtMs = nowMs;
        active = true;
        lastFrameIndex = 255;
        currentCaptionIndex = nextCaptionIndex;
        nextCaptionIndex = (nextCaptionIndex + 1) % CaptionCount;
    }

    // Release the display so another animation or the live preview can draw.
    void cancel() {
        active = false;
    }

    // Return the caption selected for the current successful save.
    const char* caption() const {
        static const char* const captions[] = {
            "Yeti approved!",   "Peak moment!",     "Looking sharp!",     "One for the moon",
            "Caught a memory!", "Proof I was here", "Stay a little wild", "Another tiny story"};
        return captions[currentCaptionIndex];
    }

    // True reserves the OLED; false lets the live preview draw again. There are
    // no delays: button polling continues, and only changed frames use the bus.
    bool update(U8G2& display, uint32_t nowMs, const char* storageLabel) {
        if (!active) {
            return false;
        }
        uint32_t elapsedMs = nowMs - startedAtMs; // Unsigned subtraction also handles millis wrap.
        if (elapsedMs >= DurationMs) {
            active = false;
            return false;
        }
        // Scale every pose with the duration, including time to read the caption.
        uint32_t phasePermille = elapsedMs * 1000 / DurationMs;
        uint8_t frameIndex = phasePermille < 120   ? 0
                             : phasePermille < 240 ? 1
                             : phasePermille < 360 ? 2
                             : phasePermille < 520 ? 3
                                                   : 4;
        if (frameIndex == lastFrameIndex) {
            return true;
        }
        lastFrameIndex = frameIndex;
        drawFrame(display, frameIndex, storageLabel);
        display.sendBuffer();
        return true;
    }

private:
    bool active = false;
    uint32_t startedAtMs = 0;
    uint8_t nextCaptionIndex = 0;
    uint8_t currentCaptionIndex = 0;
    uint8_t lastFrameIndex = 255;

    // Center text within the 128-pixel display width.
    static void drawCenteredText(U8G2& display, uint8_t baseline, const char* text) {
        int textWidth = display.getStrWidth(text);
        display.drawStr(textWidth < 128 ? (128 - textWidth) / 2 : 0, baseline, text);
    }

    // Render a Yeti camera pose, the current caption, and the available storage label.
    void drawFrame(U8G2& display, uint8_t frameIndex, const char* storageLabel) const {
        display.clearBuffer();
        display.setDrawColor(1);
        display.setFont(u8g2_font_6x12_tf);
        drawCenteredText(display, 11, "Photo saved!");

        // White fur, a dark face and little feet. Keep the graphic below the OLED's
        // fixed yellow strip; the caption and free-space line stay clear underneath.
        display.drawRBox(54, 18, 20, 20, 6);
        display.drawBox(53, 24, 22, 12);
        display.drawTriangle(54, 22, 51, 26, 56, 26);
        display.drawTriangle(73, 22, 77, 26, 71, 26);
        display.drawRBox(53, 35, 9, 6, 2);
        display.drawRBox(66, 35, 9, 6, 2);
        display.setDrawColor(0);
        display.drawRBox(58, 21, 12, 9, 3);
        display.setDrawColor(1);
        display.drawPixel(61, 24);
        display.drawPixel(67, 24);
        display.drawHLine(62, 27, 4);

        // Lift the camera, flash for two short frames, then lower it.
        const int cameraY = (frameIndex == 0 || frameIndex == 4) ? 31 : 25;
        display.drawRBox(49, cameraY, 8, 6, 2);
        display.drawRBox(71, cameraY, 8, 6, 2);
        display.drawBox(57, cameraY - 1, 4, 2);
        display.drawRBox(55, cameraY, 18, 9, 2);
        display.setDrawColor(0);
        display.drawBox(57, cameraY + 2, 14, 5);
        display.setDrawColor(1);
        display.drawCircle(64, cameraY + 4, 2);
        if (frameIndex == 2 || frameIndex == 3) {
            display.drawDisc(82, 22, 1);
            if (frameIndex == 2) {
                display.drawVLine(82, 17, 3);
                display.drawVLine(82, 25, 3);
                display.drawHLine(77, 22, 3);
                display.drawHLine(85, 22, 3);
            }
        }

        // Small crescent beside the photographer, drawn with the same OLED library.
        display.drawDisc(41, 22, 4);
        display.setDrawColor(0);
        display.drawDisc(43, 20, 4);
        display.setDrawColor(1);
        display.setFont(u8g2_font_5x7_tf);
        drawCenteredText(display, 51, caption());
        drawCenteredText(display, 62, storageLabel);
    }
};
