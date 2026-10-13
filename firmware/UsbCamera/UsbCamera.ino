// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Portable Camera contributors
// ESP32-S3 camera with an OLED viewfinder and a microSD shutter.
// See README.md for hardware connections, board settings, and serial commands.

#include <Arduino.h>
#include "esp32-hal-rgb-led.h"
#include "esp_camera.h"
#include "PreviewDecoder.h"
#include "esp_heap_caps.h"
#include "jpeg_decoder.h"
#include <Wire.h>
#include <U8g2lib.h>
#include <SD_MMC.h>
#include "CameraOrientation.h"
#include "PreviewFilter.h"
#include "ShutterGesture.h"
#include "PhotoCelebration.h"
#include "StartupAnimation.h"
#include "ErrorAnimation.h"
#include <errno.h>
#include "esp_timer.h"
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, 42, 41);
uint8_t oledI2cAddress = 0;
uint32_t totalPreviewFrames = 0;
void initializeOled();
void oledPreview();
bool startCameraPreview();
camera_fb_t* capturePhotoFrame();
void transferPhoto();

// Cumulative stage timings in microseconds; serial status reports per-frame averages.
struct PreviewTiming {
    uint32_t frameCount = 0;
    uint32_t errorCount = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;
    int decodedWidth = 0;
    int decodedHeight = 0;
    uint64_t frameWaitUs = 0;
    uint64_t decodeTimeUs = 0;
    uint64_t filterTimeUs = 0;
    uint64_t displayTimeUs = 0;
    uint64_t frameAgeUs = 0;
} previewTiming;

// Own a full-resolution frame and restore VGA preview on every return path.
struct ScopedStillCapture {
    camera_fb_t* frameBuffer;

    ScopedStillCapture() : frameBuffer(capturePhotoFrame()) {
    }

    ~ScopedStillCapture() {
        if (frameBuffer) {
            esp_camera_fb_return(frameBuffer);
        }
        startCameraPreview();
    }

    ScopedStillCapture(const ScopedStillCapture&) = delete;
    ScopedStillCapture& operator=(const ScopedStillCapture&) = delete;
};

bool cameraReady = false;
constexpr int kShutterPin = 14;
constexpr uint8_t kPhotoLedPin = 48;
constexpr uint32_t kPhotoLedDurationMs = 500;
bool photoLedActive = false;
uint32_t photoLedStartedAtMs = 0;

// Stop an active GPIO48 success pulse and log its elapsed time.
void stopPhotoLedPulse() {
    if (!photoLedActive) {
        return;
    }
    rgbLedWrite(kPhotoLedPin, 0, 0, 0);
    photoLedActive = false;
    Serial.printf("PHOTO_LED off elapsed_ms=%u\n", unsigned(millis() - photoLedStartedAtMs));
}

// Start the green success pulse after a photo passes SD readback verification.
void startPhotoLedPulse(uint32_t nowMs) {
    rgbLedWrite(kPhotoLedPin, 0, 32, 0);
    photoLedStartedAtMs = nowMs;
    photoLedActive = true;
    Serial.printf("PHOTO_LED gpio=%u green duration_ms=%u\n", unsigned(kPhotoLedPin),
                  unsigned(kPhotoLedDurationMs));
}

// End the success pulse without delaying button polling or screen updates.
void updatePhotoLedPulse(uint32_t nowMs) {
    if (photoLedActive && uint32_t(nowMs - photoLedStartedAtMs) >= kPhotoLedDurationMs) {
        stopPhotoLedPulse();
    }
}

PreviewFilter::Mode previewMode = PreviewFilter::Mode::Normal;
PreviewFilter::State previewFilter;
ShutterGesture shutterGesture;
PhotoCelebration photoCelebration;
StartupAnimation startupAnimation;
ErrorAnimation errorAnimation;
uint8_t nextErrorDemoIndex = 0;

// A timestamped change in the combined external-shutter and BOOT button state.
struct ButtonEdge {
    uint32_t timestampMs;
    bool isPressed;
};

constexpr uint8_t kButtonQueueCapacity = 32;
volatile ButtonEdge buttonEdges[kButtonQueueCapacity];
volatile uint8_t buttonReadIndex = 0;
volatile uint8_t buttonWriteIndex = 0;
volatile bool buttonQueueOverflowed = false;
volatile bool buttonIsPressed = false;
portMUX_TYPE buttonQueueMutex = portMUX_INITIALIZER_UNLOCKED;
uint32_t messageExpiresAtMs = 0;
bool sdCardReady = false;
uint32_t nextPhotoNumber = 1;
uint64_t storageTotalBytes = 0;
uint64_t storageUsedBytes = 0;
uint64_t storageFreeBytes = 0;
char storageLabel[24] = "SD unavailable";
void refreshStorageStatus();

// Queue button transitions in the interrupt handler; gesture processing runs in loop().
void IRAM_ATTR handleShutterInterrupt() {
    bool isPressed = digitalRead(kShutterPin) == LOW || digitalRead(0) == LOW;
    portENTER_CRITICAL_ISR(&buttonQueueMutex);
    if (isPressed != buttonIsPressed) {
        buttonIsPressed = isPressed;
        uint8_t nextWriteIndex = (buttonWriteIndex + 1) % kButtonQueueCapacity;
        if (nextWriteIndex == buttonReadIndex) {
            buttonQueueOverflowed = true;
        } else {
            buttonEdges[buttonWriteIndex].timestampMs = millis();
            buttonEdges[buttonWriteIndex].isPressed = isPressed;
            buttonWriteIndex = nextWriteIndex;
        }
    }
    portEXIT_CRITICAL_ISR(&buttonQueueMutex);
}

void resetShutterInput();
void setPreviewMode(PreviewFilter::Mode mode);
void cyclePreviewMode();
const char* previewModeName();
const char* previewFilterName();
void pollShutter();
void savePhoto();
void showError(ErrorAnimation::Kind kind);
bool mountCard();
void showMessage(const char* title, const char* detail);
bool writeOrientedJpeg(Print& output, const uint8_t* jpeg, size_t length);

// Initialize the OLED, shutter inputs, camera frame buffers, and microSD storage.
void setup() {
    Serial.begin(115200);
    rgbLedWrite(kPhotoLedPin, 0, 0, 0);
    delay(1500);
    initializeOled();
    pinMode(kShutterPin, INPUT_PULLUP);
    pinMode(0, INPUT_PULLUP);
    resetShutterInput();
    attachInterrupt(digitalPinToInterrupt(kShutterPin), handleShutterInterrupt, CHANGE);
    attachInterrupt(digitalPinToInterrupt(0), handleShutterInterrupt, CHANGE);
    SD_MMC.setPins(39, 38, 40);
    camera_config_t cameraConfig = {};
    cameraConfig.ledc_channel = LEDC_CHANNEL_0;
    cameraConfig.ledc_timer = LEDC_TIMER_0;
    cameraConfig.pin_d0 = 11;
    cameraConfig.pin_d1 = 9;
    cameraConfig.pin_d2 = 8;
    cameraConfig.pin_d3 = 10;
    cameraConfig.pin_d4 = 12;
    cameraConfig.pin_d5 = 18;
    cameraConfig.pin_d6 = 17;
    cameraConfig.pin_d7 = 16;
    cameraConfig.pin_xclk = 15;
    cameraConfig.pin_pclk = 13;
    cameraConfig.pin_vsync = 6;
    cameraConfig.pin_href = 7;
    cameraConfig.pin_sccb_sda = 4;
    cameraConfig.pin_sccb_scl = 5;
    cameraConfig.pin_pwdn = -1;
    cameraConfig.pin_reset = -1;
    cameraConfig.xclk_freq_hz = 20000000;
    cameraConfig.pixel_format = PIXFORMAT_JPEG;
    cameraConfig.frame_size = FRAMESIZE_UXGA;
    cameraConfig.jpeg_quality = 10;
    cameraConfig.fb_count = 2;
    cameraConfig.fb_location = CAMERA_FB_IN_PSRAM;
    cameraConfig.grab_mode = CAMERA_GRAB_LATEST;
    esp_err_t cameraInitResult = esp_camera_init(&cameraConfig);
    if (cameraInitResult != ESP_OK) {
        Serial.printf("ERROR camera 0x%x\n", cameraInitResult);
        showError(ErrorAnimation::Kind::CameraStartup);
        return;
    }
    cameraReady = true;
    Serial.printf("READY sensor=0x%x psram=%u\n", esp_camera_sensor_get()->id.PID,
                  ESP.getPsramSize());
    startCameraPreview();
    mountCard();
    if (oledI2cAddress) {
        startupAnimation.start(millis());
        Serial.printf("STARTUP moon_yeti duration_ms=%u\n", unsigned(StartupAnimation::DurationMs));
    }
}

// Service button gestures, project-defined serial commands, and OLED frames.
void loop() {
    updatePhotoLedPulse(millis());
    pollShutter();
    if (Serial.available()) {
        char command = Serial.read();
        if (command == '?') {
            Serial.printf("camera=%d OLED=0x%02x preview_frames=%u driver=U8g2_SSD1306\n",
                          cameraReady, oledI2cAddress, totalPreviewFrames);
            Serial.printf("orientation=90ccw mirror_horizontal=%d exif=%u preview=center_crop "
                          "mode=%s filter=%s edge_threshold=%d\n",
                          CameraOrientation::kMirrorHorizontal,
                          unsigned(CameraOrientation::kExifOrientation), previewModeName(),
                          previewFilterName(), PreviewFilter::EdgeThreshold);
            Serial.printf("preview_source=%dx%d decoded=%dx%d cpu_mhz=%u\n",
                          previewTiming.sourceWidth, previewTiming.sourceHeight,
                          previewTiming.decodedWidth, previewTiming.decodedHeight,
                          getCpuFrequencyMhz());
            if (previewTiming.frameCount) {
                double timingDivisor = previewTiming.frameCount * 1000.0;
                Serial.printf("preview_ms wait=%.1f decode=%.1f filter=%.1f oled=%.1f "
                              "frame_age=%.1f errors=%u heap=%u\n",
                              previewTiming.frameWaitUs / timingDivisor,
                              previewTiming.decodeTimeUs / timingDivisor,
                              previewTiming.filterTimeUs / timingDivisor,
                              previewTiming.displayTimeUs / timingDivisor,
                              previewTiming.frameAgeUs / timingDivisor, previewTiming.errorCount,
                              ESP.getFreeHeap());
            }
            Serial.printf("storage_total=%llu used=%llu free=%llu label=%s\n", storageTotalBytes,
                          storageUsedBytes, storageFreeBytes, storageLabel);
            Serial.printf("feedback=moon_yeti captions=%u startup_ms=%u photo_ms=%u\n",
                          unsigned(PhotoCelebration::CaptionCount),
                          unsigned(StartupAnimation::DurationMs),
                          unsigned(PhotoCelebration::DurationMs));
            Serial.printf("error_types=%u error_ms=%u\n", unsigned(ErrorAnimation::Count),
                          unsigned(ErrorAnimation::DurationMs));
            Serial.printf("photo_led gpio=%u pulse_ms=%u active=%d\n", unsigned(kPhotoLedPin),
                          unsigned(kPhotoLedDurationMs), photoLedActive);
        }
        if (command == 'e') {
            Serial.println("DEMO card-full animation; no file written");
            showError(ErrorAnimation::Kind::CardFull);
            resetShutterInput();
        }
        if (command == 'E') {
            Serial.printf("DEMO error=%u; no file written\n", unsigned(nextErrorDemoIndex));
            showError(ErrorAnimation::Kind(nextErrorDemoIndex));
            nextErrorDemoIndex = (nextErrorDemoIndex + 1) % ErrorAnimation::Count;
            resetShutterInput();
        }
        if (command == 's') {
            savePhoto();
            resetShutterInput();
        }
        if (command == '0') {
            setPreviewMode(PreviewFilter::Mode::Normal);
        }
        if (command == '1') {
            setPreviewMode(PreviewFilter::Mode::CombinedSobel);
        }
        if (command == '2') {
            setPreviewMode(PreviewFilter::Mode::SobelNms);
        }
        if (command == 'v') {
            cyclePreviewMode();
        }
        if (command == 'm') {
            bool cardMounted = mountCard();
            Serial.printf("SD ready=%d size_mb=%llu\n", cardMounted,
                          cardMounted ? SD_MMC.cardSize() / 1048576 : 0);
        }
        if (command == 't') {
            stopPhotoLedPulse();
            startupAnimation.cancel();
            photoCelebration.cancel();
            errorAnimation.cancel();
            oled.clearBuffer();
            oled.setFont(u8g2_font_6x12_tf);
            oled.drawFrame(0, 0, 128, 64);
            oled.drawStr(12, 24, "OLED TEST");
            oled.drawStr(12, 42, "128 x 64");
            oled.sendBuffer();
            delay(5000);
            resetShutterInput();
        }
        if (command == 'o') {
            Serial.println("OLED 1024");
            Serial.write(oled.getBufferPtr(), 1024);
            Serial.flush();
        }
        if (command == 'c' && cameraReady) {
            transferPhoto();
            resetShutterInput();
        }
    }
    oledPreview();
    delay(1);
}

uint8_t* previewGrayscaleBuffer = nullptr;

// Find the SSD1306 on I2C and allocate the grayscale preview buffer.
void initializeOled() {
    Wire.begin(41, 42, 400000);
    for (uint8_t candidateAddress = 0x3c; candidateAddress <= 0x3d; candidateAddress++) {
        Wire.beginTransmission(candidateAddress);
        if (Wire.endTransmission() == 0) {
            oledI2cAddress = candidateAddress;
            break;
        }
    }
    if (!oledI2cAddress) {
        Serial.println("OLED not found on SDA41 SCL42");
        return;
    }
    Serial.printf("OLED found 0x%02x; testing SSD1306 128x64\n", oledI2cAddress);
    oled.setI2CAddress(oledI2cAddress << 1);
    oled.setBusClock(400000);
    oled.begin();
    // Show the first welcome frame during hardware setup, then play the full
    // animation in loop() once initialization finishes.
    startupAnimation.start(millis());
    startupAnimation.update(oled, millis());
    previewGrayscaleBuffer =
        (uint8_t*)heap_caps_malloc(320 * 240, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!previewGrayscaleBuffer) {
        previewGrayscaleBuffer = (uint8_t*)ps_malloc(320 * 240);
    }
    if (!previewGrayscaleBuffer) {
        Serial.println("ERROR no memory for preview");
    }
}

// Allocate camera buffers at UXGA during initialization, then run the viewfinder
// at VGA. Switch back only while taking a photo; every exit restores preview.
bool startCameraPreview() {
    auto sensor = esp_camera_sensor_get();
    if (!sensor || sensor->set_framesize(sensor, FRAMESIZE_VGA) != 0) {
        Serial.println("ERROR preview size; using full-size fallback");
        return false;
    }
    return true;
}

// Switch to UXGA, discard stale frames, and return a verified 1600 x 1200 JPEG frame.
// The caller owns the returned camera frame and must restore preview afterward.
camera_fb_t* capturePhotoFrame() {
    auto sensor = esp_camera_sensor_get();
    if (!sensor || sensor->set_framesize(sensor, FRAMESIZE_UXGA) != 0) {
        return nullptr;
    }
    // Flush both queued frames plus a transition frame after changing the sensor.
    for (int frameIndex = 0; frameIndex < 3; frameIndex++) {
        auto staleFrame = esp_camera_fb_get();
        if (!staleFrame) {
            return nullptr;
        }
        esp_camera_fb_return(staleFrame);
    }
    for (int frameIndex = 0; frameIndex < 4; frameIndex++) {
        auto frameBuffer = esp_camera_fb_get();
        if (!frameBuffer) {
            return nullptr;
        }
        esp_jpeg_image_cfg_t jpegConfig = {};
        jpegConfig.indata = frameBuffer->buf;
        jpegConfig.indata_size = frameBuffer->len;
        esp_jpeg_image_output_t jpegInfo = {};
        if (esp_jpeg_get_image_info(&jpegConfig, &jpegInfo) == ESP_OK && jpegInfo.width == 1600 &&
            jpegInfo.height == 1200) {
            return frameBuffer;
        }
        esp_camera_fb_return(frameBuffer);
    }
    return nullptr;
}

// Send an oriented color JPEG using the JPEG-length / binary-data / END serial protocol.
void transferPhoto() {
    stopPhotoLedPulse();
    ScopedStillCapture stillCapture;
    auto frameBuffer = stillCapture.frameBuffer;
    if (!frameBuffer) {
        Serial.println("ERROR capture");
        return;
    }
    size_t jpegLength = CameraOrientation::jpegSize(frameBuffer->buf, frameBuffer->len);
    if (!jpegLength) {
        Serial.println("ERROR invalid JPEG");
        return;
    }
    Serial.printf("JPEG %u\n", unsigned(jpegLength));
    writeOrientedJpeg(Serial, frameBuffer->buf, frameBuffer->len);
    Serial.flush();
    Serial.println("\nEND");
}

// Decode one preview frame, run the selected filter, and send the packed pixels to the OLED.
// Active animations and temporary messages take priority over the live image.
void oledPreview() {
    if (!oledI2cAddress) {
        return;
    }
    if (startupAnimation.update(oled, millis())) {
        return;
    }
    if (photoCelebration.update(oled, millis(), storageLabel)) {
        return;
    }
    if (errorAnimation.update(oled, millis())) {
        return;
    }
    if (!cameraReady || !previewGrayscaleBuffer || (int32_t)(messageExpiresAtMs - millis()) > 0) {
        return;
    }
    const int64_t waitStartedAtUs = esp_timer_get_time();
    auto frameBuffer = esp_camera_fb_get();
    if (!frameBuffer) {
        previewTiming.errorCount++;
        return;
    }
    const int64_t frameAcquiredAtUs = esp_timer_get_time();
    const int64_t frameCapturedAtUs =
        int64_t(frameBuffer->timestamp.tv_sec) * 1000000 + frameBuffer->timestamp.tv_usec;
    static PreviewDecoder decoder;
    bool decodedSuccessfully =
        decoder.decode(frameBuffer->buf, frameBuffer->len, previewGrayscaleBuffer, 320 * 240);
    int decodedWidth = decoder.decodedWidth;
    int decodedHeight = decoder.decodedHeight;
    previewTiming.sourceWidth = decoder.sourceWidth;
    previewTiming.sourceHeight = decoder.sourceHeight;
    previewTiming.decodedWidth = decodedWidth;
    previewTiming.decodedHeight = decodedHeight;
    esp_camera_fb_return(frameBuffer); // Release promptly so capture can continue during I2C.
    const int64_t decodeFinishedAtUs = esp_timer_get_time();
    if (!decodedSuccessfully) {
        previewTiming.errorCount++;
        return;
    }
    if (!PreviewFilter::render(previewFilter, previewGrayscaleBuffer, decodedWidth, decodedHeight,
                               oled.getBufferPtr(), previewMode)) {
        previewTiming.errorCount++;
        return;
    }
    const int64_t filterFinishedAtUs = esp_timer_get_time();
    oled.sendBuffer();
    const int64_t displayFinishedAtUs = esp_timer_get_time();
    totalPreviewFrames++;
    previewTiming.frameCount++;
    previewTiming.frameWaitUs += frameAcquiredAtUs - waitStartedAtUs;
    previewTiming.decodeTimeUs += decodeFinishedAtUs - frameAcquiredAtUs;
    previewTiming.filterTimeUs += filterFinishedAtUs - decodeFinishedAtUs;
    previewTiming.displayTimeUs += displayFinishedAtUs - filterFinishedAtUs;
    if (displayFinishedAtUs >= frameCapturedAtUs) {
        previewTiming.frameAgeUs += displayFinishedAtUs - frameCapturedAtUs;
    }
}

// Replace success feedback with an error animation and log its recovery message.
void showError(ErrorAnimation::Kind kind) {
    stopPhotoLedPulse();
    startupAnimation.cancel();
    photoCelebration.cancel();
    uint32_t nowMs = millis();
    messageExpiresAtMs = nowMs;
    errorAnimation.start(nowMs, kind);
    Serial.printf("SHUTTER %s: %s (%s)\n", errorAnimation.title(), errorAnimation.detail(),
                  errorAnimation.action());
    if (oledI2cAddress) {
        errorAnimation.update(oled, nowMs);
    }
}

// Show a temporary status message and storage label while pausing the live preview.
void showMessage(const char* title, const char* detail) {
    startupAnimation.cancel();
    photoCelebration.cancel();
    errorAnimation.cancel();
    Serial.printf("SHUTTER %s: %s\n", title, detail);
    messageExpiresAtMs = millis() + 2000;
    if (!oledI2cAddress) {
        return;
    }
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x12_tf);
    oled.drawStr(5, 25, title);
    oled.drawStr(5, 44, detail);
    oled.drawStr(5, 60, storageLabel);
    oled.sendBuffer();
}

// Mount the card in one-bit mode and refresh available space; return the mount status.
bool mountCard() {
    if (sdCardReady) {
        return true;
    }
    // Never format a card on mount failure.
    sdCardReady = SD_MMC.begin("/sdcard", true, false, 10000, 5);
    if (sdCardReady && SD_MMC.cardType() == CARD_NONE) {
        SD_MMC.end();
        sdCardReady = false;
    }
    if (sdCardReady) {
        refreshStorageStatus();
    } else {
        storageTotalBytes = storageUsedBytes = storageFreeBytes = 0;
        snprintf(storageLabel, sizeof(storageLabel), "SD unavailable");
    }
    return sdCardReady;
}

// Return the serial status identifier for the selected preview mode.
const char* previewModeName() {
    switch (previewMode) {
        case PreviewFilter::Mode::CombinedSobel:
            return "combined_sobel";
        case PreviewFilter::Mode::SobelNms:
            return "sobel_nms";
        default:
            return "normal";
    }
}

// Return the serial status identifier for the selected image-processing algorithm.
const char* previewFilterName() {
    switch (previewMode) {
        case PreviewFilter::Mode::CombinedSobel:
            return "sobel_abs_gx_plus_abs_gy";
        case PreviewFilter::Mode::SobelNms:
            return "sobel_nms_4dir";
        default:
            return "unsharp_floyd_steinberg";
    }
}

// Select a mode, reset its contrast history and timing averages, and show its name.
void setPreviewMode(PreviewFilter::Mode mode) {
    startupAnimation.cancel();
    photoCelebration.cancel();
    errorAnimation.cancel();
    previewMode = mode;
    previewFilter.reset();
    // Stage timing averages describe the selected mode; total frame count continues.
    uint32_t errorCount = previewTiming.errorCount;
    previewTiming = {};
    previewTiming.errorCount = errorCount;
    Serial.printf("PREVIEW mode=%s threshold=%d\n", previewModeName(),
                  PreviewFilter::EdgeThreshold);
    messageExpiresAtMs = millis() + 700;
    if (!oledI2cAddress) {
        return;
    }
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x12_tf);
    oled.drawStr(8, 24, "PREVIEW MODE");
    const char* modeLabel = mode == PreviewFilter::Mode::Normal          ? "Normal"
                            : mode == PreviewFilter::Mode::CombinedSobel ? "Combined Sobel"
                                                                         : "Sobel + NMS";
    oled.drawStr(8, 44, modeLabel);
    oled.sendBuffer();
}

// Advance Normal -> Combined Sobel -> Sobel + NMS -> Normal.
void cyclePreviewMode() {
    setPreviewMode(PreviewFilter::nextMode(previewMode));
}

// Clear queued transitions and require a released button before accepting another press.
void resetShutterInput() {
    // Discard presses during capture/serial transfer and require a clean release.
    portENTER_CRITICAL(&buttonQueueMutex);
    buttonReadIndex = buttonWriteIndex = 0;
    buttonQueueOverflowed = false;
    buttonIsPressed = digitalRead(kShutterPin) == LOW || digitalRead(0) == LOW;
    shutterGesture.reset(millis(), buttonIsPressed);
    portEXIT_CRITICAL(&buttonQueueMutex);
}

// Drain timestamped button changes and dispatch a short press or one long-hold event.
void pollShutter() {
    ButtonEdge pendingEdges[kButtonQueueCapacity];
    uint8_t pendingEdgeCount = 0;
    portENTER_CRITICAL(&buttonQueueMutex);
    bool queueOverflowed = buttonQueueOverflowed;
    bool isPressed = buttonIsPressed;
    while (buttonReadIndex != buttonWriteIndex) {
        pendingEdges[pendingEdgeCount].timestampMs = buttonEdges[buttonReadIndex].timestampMs;
        pendingEdges[pendingEdgeCount++].isPressed = buttonEdges[buttonReadIndex].isPressed;
        buttonReadIndex = (buttonReadIndex + 1) % kButtonQueueCapacity;
    }
    // Take the clock snapshot with the level, so an intervening ISR cannot put a
    // queued edge before the time already processed by the gesture state machine.
    uint32_t nowMs = millis();
    portEXIT_CRITICAL(&buttonQueueMutex);
    if (queueOverflowed) {
        resetShutterInput();
        return;
    }
    uint8_t gestureEvents = ShutterGesture::None;
    for (uint8_t edgeIndex = 0; edgeIndex < pendingEdgeCount; edgeIndex++) {
        gestureEvents |= shutterGesture.update(pendingEdges[edgeIndex].timestampMs,
                                               pendingEdges[edgeIndex].isPressed);
    }
    gestureEvents |= shutterGesture.update(nowMs, isPressed);
    if (gestureEvents & ShutterGesture::TogglePreview) {
        cyclePreviewMode();
    } else if (gestureEvents & ShutterGesture::Photo) {
        savePhoto();
        resetShutterInput();
    }
}

// Capture a color JPEG into a temporary file, verify every byte, then rename it.
// Report success only after verification and rename; remove incomplete files on failure.
void savePhoto() {
    stopPhotoLedPulse(); // Clear an earlier success before attempting a new capture.
    if (!cameraReady) {
        showError(ErrorAnimation::Kind::CameraUnavailable);
        return;
    }
    showMessage("Taking photo...", "Hold still");
    if (!mountCard()) {
        showError(ErrorAnimation::Kind::CardUnavailable);
        return;
    }
    refreshStorageStatus();
    if (storageTotalBytes && !storageFreeBytes) {
        showError(ErrorAnimation::Kind::CardFull);
        return;
    }
    if (!SD_MMC.exists("/photos")) {
        errno = 0;
        if (!SD_MMC.mkdir("/photos")) {
            bool outOfSpace = errno == ENOSPC;
            refreshStorageStatus();
            if (outOfSpace || (storageTotalBytes && !storageFreeBytes)) {
                showError(ErrorAnimation::Kind::CardFull);
            } else {
                showError(ErrorAnimation::Kind::FolderCreate);
            }
            return;
        }
    }
    char photoPath[40];
    char temporaryPath[40];
    bool filenameAvailable = false;
    while (nextPhotoNumber <= 999999) {
        snprintf(photoPath, sizeof(photoPath), "/photos/IMG_%06lu.jpg",
                 (unsigned long)nextPhotoNumber);
        snprintf(temporaryPath, sizeof(temporaryPath), "/photos/IMG_%06lu.tmp",
                 (unsigned long)nextPhotoNumber++);
        if (!SD_MMC.exists(photoPath) && !SD_MMC.exists(temporaryPath)) {
            filenameAvailable = true;
            break;
        }
    }
    if (!filenameAvailable) {
        showError(ErrorAnimation::Kind::PhotoNamesFull);
        return;
    }
    ScopedStillCapture stillCapture;
    auto frameBuffer = stillCapture.frameBuffer;
    if (!frameBuffer) {
        showError(ErrorAnimation::Kind::CaptureFailed);
        return;
    }
    size_t jpegLength = CameraOrientation::jpegSize(frameBuffer->buf, frameBuffer->len);
    if (!jpegLength) {
        showError(ErrorAnimation::Kind::InvalidJpeg);
        return;
    }
    // JPEG size varies with the scene. Check this actual photo, not an estimate.
    refreshStorageStatus();
    if (storageTotalBytes && uint64_t(jpegLength) > storageFreeBytes) {
        showError(ErrorAnimation::Kind::CardFull);
        return;
    }
    errno = 0;
    File photoFile = SD_MMC.open(temporaryPath, FILE_WRITE);
    bool saveSucceeded = bool(photoFile);
    bool outOfSpace = errno == ENOSPC;
    if (saveSucceeded) {
        errno = 0;
        saveSucceeded = writeOrientedJpeg(photoFile, frameBuffer->buf, frameBuffer->len);
        outOfSpace |= errno == ENOSPC;
        errno = 0;
        photoFile.flush();
        outOfSpace |= errno == ENOSPC;
        errno = 0;
        photoFile.close();
        outOfSpace |= errno == ENOSPC;
        if (outOfSpace) {
            saveSucceeded = false;
        }
    }
    if (!saveSucceeded && !outOfSpace) {
        // Some filesystem failures do not preserve errno. Check before removing the
        // partial file, since cleanup releases its allocated space.
        refreshStorageStatus();
        outOfSpace = storageTotalBytes && !storageFreeBytes;
    }
    // Read back the entire new file before reporting success.
    if (saveSucceeded) {
        File verificationFile = SD_MMC.open(temporaryPath, FILE_READ);
        saveSucceeded = bool(verificationFile) && verificationFile.size() == jpegLength;
        uint8_t actualBytes[512];
        uint8_t expectedBytes[512];
        size_t verifiedBytes = 0;
        while (saveSucceeded && verifiedBytes < jpegLength) {
            size_t chunkSize = min(sizeof(actualBytes), jpegLength - verifiedBytes);
            saveSucceeded =
                CameraOrientation::jpegSlice(frameBuffer->buf, frameBuffer->len, verifiedBytes,
                                             expectedBytes, chunkSize) == chunkSize &&
                verificationFile.read(actualBytes, chunkSize) == chunkSize &&
                memcmp(actualBytes, expectedBytes, chunkSize) == 0;
            verifiedBytes += chunkSize;
        }
        verificationFile.close();
    }
    if (saveSucceeded) {
        errno = 0;
        saveSucceeded = SD_MMC.rename(temporaryPath, photoPath);
        if (!saveSucceeded) {
            outOfSpace |= errno == ENOSPC;
        }
    }
    if (!saveSucceeded) {
        bool partialFileRemoved = !SD_MMC.exists(temporaryPath) || SD_MMC.remove(temporaryPath);
        if (outOfSpace && partialFileRemoved) {
            refreshStorageStatus();
            showError(ErrorAnimation::Kind::CardFull);
            return;
        }
        SD_MMC.end();
        sdCardReady = false;
        snprintf(storageLabel, sizeof(storageLabel), "SD unavailable");
        showError(ErrorAnimation::Kind::CardWrite);
        return;
    }
    Serial.printf("SAVED %s bytes=%u verified=1 orientation=%u\n", photoPath, unsigned(jpegLength),
                  unsigned(CameraOrientation::kExifOrientation));
    refreshStorageStatus();
    // Success feedback begins only after full readback verification and rename.
    // Error messages and a new button action can replace it immediately.
    uint32_t nowMs = millis();
    startPhotoLedPulse(nowMs);
    photoCelebration.start(nowMs);
    messageExpiresAtMs = nowMs;
    Serial.printf("SHUTTER Photo saved!: %s caption=%s\n", photoPath + 8,
                  photoCelebration.caption());
}

// Read filesystem capacity and format the free-space label for the OLED.
void refreshStorageStatus() {
    storageTotalBytes = SD_MMC.totalBytes();
    storageUsedBytes = SD_MMC.usedBytes();
    storageFreeBytes =
        storageTotalBytes > storageUsedBytes ? storageTotalBytes - storageUsedBytes : 0;
    if (!storageTotalBytes) {
        snprintf(storageLabel, sizeof(storageLabel), "Storage unknown");
        return;
    }
    unsigned freePercent = unsigned(storageFreeBytes * 100 / storageTotalBytes);
    // Decimal MB, rounded down; percentage uses filesystem capacity, not raw card size.
    snprintf(storageLabel, sizeof(storageLabel), "FREE %lluMB %u%%", storageFreeBytes / 1000000,
             freePercent);
}

// Stream the small EXIF segment and original JPEG through the same path for SD/USB.
bool writeOrientedJpeg(Print& output, const uint8_t* jpegData, size_t inputLength) {
    const size_t orientedLength = CameraOrientation::jpegSize(jpegData, inputLength);
    if (!orientedLength) {
        return false;
    }
    uint8_t jpegChunk[512];
    size_t bytesWritten = 0;
    while (bytesWritten < orientedLength) {
        size_t chunkSize = CameraOrientation::jpegSlice(jpegData, inputLength, bytesWritten,
                                                        jpegChunk, sizeof(jpegChunk));
        if (!chunkSize || output.write(jpegChunk, chunkSize) != chunkSize) {
            return false;
        }
        bytesWritten += chunkSize;
    }
    return true;
}
