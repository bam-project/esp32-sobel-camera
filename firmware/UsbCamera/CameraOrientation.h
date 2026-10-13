// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Rotate 90 degrees CCW, then correct the assembled view's left/right reversal.
// JPEG data stays untouched; EXIF-aware viewers apply the equivalent transform.
namespace CameraOrientation {
constexpr bool kMirrorHorizontal = true;
// EXIF 7 is a transverse reflection: 90 degrees CCW, then mirror horizontally.
constexpr uint8_t kExifOrientation = kMirrorHorizontal ? 7 : 8;
constexpr uint8_t kExifSegment[] = {0xff,
                                    0xe1,
                                    0x00,
                                    0x22,
                                    'E',
                                    'x',
                                    'i',
                                    'f',
                                    0,
                                    0,
                                    'I',
                                    'I',
                                    0x2a,
                                    0,
                                    8,
                                    0,
                                    0,
                                    0,
                                    1,
                                    0,
                                    0x12,
                                    0x01,
                                    3,
                                    0,
                                    1,
                                    0,
                                    0,
                                    0,
                                    kExifOrientation,
                                    0,
                                    0,
                                    0,
                                    0,
                                    0,
                                    0,
                                    0};

// Return the JPEG length including the inserted EXIF segment; reject a missing SOI marker.
inline size_t jpegSize(const uint8_t* jpegData, size_t jpegLength) {
    return jpegData && jpegLength >= 2 && jpegData[0] == 0xff && jpegData[1] == 0xd8
               ? jpegLength + sizeof(kExifSegment)
               : 0;
}

// Read a slice of SOI + EXIF + the remaining sensor JPEG, without a second frame.
inline size_t jpegSlice(const uint8_t* jpegData, size_t jpegLength, size_t offset,
                        uint8_t* outputBuffer, size_t outputCapacity) {
    const size_t orientedLength = jpegSize(jpegData, jpegLength);
    if (!outputBuffer || offset >= orientedLength) {
        return 0;
    }
    size_t requestedBytes = orientedLength - offset;
    if (requestedBytes > outputCapacity) {
        requestedBytes = outputCapacity;
    }
    size_t copiedBytes = 0;
    while (copiedBytes < requestedBytes) {
        const uint8_t* sourceBytes;
        size_t availableBytes;
        if (offset < 2) {
            sourceBytes = jpegData + offset;
            availableBytes = 2 - offset;
        } else if (offset < 2 + sizeof(kExifSegment)) {
            size_t exifOffset = offset - 2;
            sourceBytes = kExifSegment + exifOffset;
            availableBytes = sizeof(kExifSegment) - exifOffset;
        } else {
            size_t jpegOffset = offset - sizeof(kExifSegment);
            sourceBytes = jpegData + jpegOffset;
            availableBytes = jpegLength - jpegOffset;
        }
        size_t chunkSize = requestedBytes - copiedBytes;
        if (chunkSize > availableBytes) {
            chunkSize = availableBytes;
        }
        memcpy(outputBuffer + copiedBytes, sourceBytes, chunkSize);
        offset += chunkSize;
        copiedBytes += chunkSize;
    }
    return copiedBytes;
}
} // namespace CameraOrientation
