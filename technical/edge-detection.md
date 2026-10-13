# Sobel and Sobel + NMS

The camera has two edge-preview modes:

| Mode | Rule for lighting a pixel |
| --- | --- |
| Combined Sobel | Its edge strength is at least **75**. |
| Sobel + NMS | Its strength is at least **75**, and it is a local peak across the edge. |

Both modes use the same Sobel kernels and threshold. NMS adds a comparison with neighboring edge strengths to make thick responses thinner.

## Where the code lives

| File and function | Responsibility |
| --- | --- |
| [UsbCamera.ino — `oledPreview()`](../firmware/UsbCamera/UsbCamera.ino) | Gets a camera frame, decodes it, applies the selected filter, and updates the OLED. |
| [PreviewDecoder.h — `PreviewDecoder::decode()`](../firmware/UsbCamera/PreviewDecoder.h) | Decodes JPEG luminance into a flat grayscale array. |
| [PreviewFilter.h — `render()`](../firmware/UsbCamera/PreviewFilter.h) | Rotates, mirrors, crops, and resizes the grayscale image to 128 × 64. Selects the filter. |
| [PreviewFilter.h — `renderEdges()`](../firmware/UsbCamera/PreviewFilter.h) | Calculates Sobel strength and applies the threshold. |
| [PreviewFilter.h — `gradientDirection()`](../firmware/UsbCamera/PreviewFilter.h) | Selects the pair of neighbors used by NMS. |
| [PreviewFilter.h — `renderSobelNms()`](../firmware/UsbCamera/PreviewFilter.h) | Calculates strengths and directions, then keeps local peaks. |

The kernels are implemented directly in C++. JPEGDEC handles decoding, and U8g2 handles the screen buffer and display transfer.

## 1. Prepare the preview image

```text
640 × 480 camera JPEG
          ↓ JPEGDEC: half-size luminance decoding
320 × 240 grayscale array
          ↓ rotate 90° counterclockwise and mirror horizontally
          ↓ center crop and resize
128 × 64 grayscale array
          ↓ Combined Sobel or Sobel + NMS
1024-byte on/off OLED buffer
          ↓ U8g2 over I2C
OLED display
```

Decoding turns compressed JPEG data into pixel values. JPEGDEC also reduces the resolution during this step. Each grayscale value is a byte: **0 is black and 255 is white**.

The orientation and crop are calculated while sampling the source array. No separate rotated image is allocated. Each output pixel averages four source samples, with integer rounding.

If a full-size 1600 × 1200 frame reaches the preview decoder, it uses one-eighth scaling instead. That produces 200 × 150 grayscale pixels before the same crop and resize step.

The edge modes run before Normal mode's contrast adjustment and dithering. They use the prepared grayscale pixels directly.

## 2. Store the image as a flat array

The image is a grid, but the code stores its rows in one continuous array:

```cpp
uint8_t grayscalePixels[128 * 64];
```

Pixel coordinates use `x` from left to right and `y` from top to bottom:

```cpp
int pixelIndex = y * Width + x;
int grayValue = grayscalePixels[pixelIndex];
```

For example, this 3 × 3 neighborhood:

```text
10   10   200
10   10   200
10   10   200
```

would be stored as:

```cpp
uint8_t example[9] = {10, 10, 200, 10, 10, 200, 10, 10, 200};
```

Its center is at `1 * 3 + 1`, or index **4**. For the real preview, the row width is **128**, not 3.

## 3. Calculate the Sobel gradients

At each pixel, read the surrounding 3 × 3 neighborhood:

```text
topLeft       topCenter       topRight
middleLeft    center          middleRight
bottomLeft    bottomCenter    bottomRight
```

Multiply those values by two sets of weights and add the results:

```text
Horizontal-change kernel     Vertical-change kernel

 -1   0   1                   -1  -2  -1
 -2   0   2                    0   0   0
 -1   0   1                    1   2   1
```

The implementation expands the multiplication into these expressions:

```cpp
int gx = -topLeft + topRight
         - 2 * middleLeft + 2 * middleRight
         - bottomLeft + bottomRight;

int gy = -topLeft - 2 * topCenter - topRight
         + bottomLeft + 2 * bottomCenter + bottomRight;
```

- `gx` measures brightness change from left to right. A large value often indicates a **vertical edge**.
- `gy` measures brightness change from top to bottom. A large value often indicates a **horizontal edge**.
- The center has a zero weight in both kernels, so its value does not appear in these sums.

The gradient points across an edge toward increasing brightness. It does not point along the edge. Sobel detects brightness boundaries; it is not a dedicated corner detector.

For the example above:

```text
gx = (-10 + 200) + (-20 + 400) + (-10 + 200) = 760
gy = (-10 - 20 - 200) + (10 + 20 + 200)      =   0
```

The brightness changes from left to right, so this is a vertical boundary.

## 4. Combined Sobel: apply the threshold

Combine the two gradients using their absolute values:

```cpp
int magnitude = (gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy);
```

This is `abs(gx) + abs(gy)`. It is a fast approximation of gradient magnitude, rather than the Euclidean length `sqrt(gx * gx + gy * gy)`.

The decision is:

```text
magnitude >= 75  → pixel ON
magnitude < 75   → pixel OFF
```

For the example, `760 + 0 = 760`, so the center output pixel is ON.

**75 is a project setting, not a standard Sobel value.** It is defined by `EdgeThreshold` in `PreviewFilter.h`. Lower values keep weaker edges and more noise. Higher values keep stronger edges. A strength can exceed 255 because it is a weighted sum, not an original grayscale value.

At the image border, the Sobel calculation clamps sample coordinates to the nearest valid pixel. This repeats the outer pixel values instead of reading outside the array.

## 5. Sobel + NMS: choose a comparison direction

Non-maximum suppression (NMS) keeps local peaks in the Sobel response. It compares the current strength with two strengths **across the edge**.

`gradientDirection(gx, gy)` chooses one of four axes:

| Returned value | Neighbors | Flat-array offset |
| --- | --- | --- |
| 0 | Left and right | `1` |
| 1 | Upper-left and lower-right | `Width + 1` |
| 2 | Above and below | `Width` |
| 3 | Upper-right and lower-left | `Width - 1` |

The function first compares the absolute sizes of `gx` and `gy`:

```cpp
int absGx = gx < 0 ? -gx : gx;
int absGy = gy < 0 ? -gy : gy;

if (absGy * 32768 <= absGx * 13573) {
    return 0;
}
if (absGx * 32768 <= absGy * 13573) {
    return 2;
}
return (gx < 0) == (gy < 0) ? 1 : 3;
```

Why these numbers? The axes are 45° apart, so the boundary between two axes is halfway between them: **22.5°**. The ratio `tan(22.5°)` is approximately **0.4142**, represented here by `13573 / 32768`. Multiplying both sides avoids division and calculating an angle.

Comparing only `absGx > absGy` would not reserve a range for diagonal gradients.

When neither component is small enough, the signs choose a diagonal. Image `y` increases downward:

| `gx` | `gy` | Gradient direction | Neighbor pair |
| --- | --- | --- | --- |
| 760 | 0 | Right | Left / right |
| 0 | 760 | Down | Above / below |
| 200 | 200 | Down-right | Upper-left / lower-right |
| 200 | -200 | Up-right | Upper-right / lower-left |

Reversing both signs points the gradient in the opposite direction but keeps the same comparison axis. A zero gradient returns 0, but its strength is below the threshold, so it is not retained.

## 6. Compare neighboring strengths

NMS uses two passes:

1. Calculate a Sobel magnitude and direction for every pixel.
2. Compare each interior pixel with its selected neighbors.

The code stores magnitude and direction together:

```cpp
packedEdges[pixelIndex] = uint32_t(magnitude)
                       | (uint32_t(gradientDirection(gx, gy)) << 16);
```

The low 16 bits hold magnitude. The upper bits hold the direction label. The array reuses `State::scratchBuffer`, which Normal mode uses for its summed-area table. NMS does not allocate a new frame buffer on each update.

The second pass reads two **Sobel magnitudes**, not grayscale values:

```cpp
constexpr int neighborOffsets[4] = {1, Width + 1, Width, Width - 1};
int neighborOffset = neighborOffsets[packedEdge >> 16];
int previousMagnitude = int(packedEdges[pixelIndex - neighborOffset] & 0xffff);
int nextMagnitude = int(packedEdges[pixelIndex + neighborOffset] & 0xffff);
```

After checking `magnitude >= EdgeThreshold`, it keeps the pixel only when:

```cpp
magnitude > previousMagnitude && magnitude >= nextMagnitude
```

The strict `>` on one side breaks equal-strength ties consistently. It prevents a two-pixel-wide plateau from keeping both pixels. NMS leaves the outermost row and column OFF because those pixels do not have two neighbors inside the image.

### Worked example: reduce a two-pixel edge to one

Consider five columns through a vertical boundary. Each shown row has the same values:

```text
Grayscale values:

10   10   10   200   200
10   10   10   200   200
10   10   10   200   200
10   10   10   200   200
10   10   10   200   200
```

For the three middle positions on the center row, their overlapping neighborhoods are:

```text
Left position       Middle position     Right position

10  10  10          10  10  200         10  200  200
10  10  10          10  10  200         10  200  200
10  10  10          10  10  200         10  200  200

gx = 0              gx = 760             gx = 760
gy = 0              gy = 0               gy = 0
strength = 0        strength = 760       strength = 760
```

Combined Sobel lights both positions with strength 760.

For the middle position, the direction is 0, so NMS compares left and right:

```text
760 >= 75       threshold passes
760 > 0         stronger than the left neighbor
760 >= 760      at least as strong as the right neighbor

Result: ON
```

The right position has a left neighbor of strength 760. Its strict comparison is `760 > 760`, which is false, so it becomes OFF.

These three positions therefore change from **OFF, ON, ON** to **OFF, ON, OFF**. Every compared strength was calculated from that neighbor's own 3 × 3 grayscale neighborhood.

## 7. Pack the result for the OLED

The screen uses one bit per pixel. It does not receive the original magnitude as a shade.

```cpp
oledBuffer[(y / 8) * Width + x] |= uint8_t(1U << (y & 7));
```

Each byte stores eight vertically stacked pixels in one OLED page. The buffer is cleared before rendering, so rejected pixels stay OFF. The full buffer uses `128 * 64 / 8 = 1024` bytes.

`oledPreview()` sends that buffer with `oled.sendBuffer()`, then repeats with another camera frame. The loop is limited by capture, JPEG decoding, filtering, and I2C transfer. It does not process a new image every millisecond.

## What NMS changes

NMS usually makes edge responses thinner. It does not smooth the source image or automatically remove noise. Weak edges still depend on the threshold, and some edges can become broken or jagged on a 128 × 64 screen.
