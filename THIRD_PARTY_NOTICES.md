# Third-party notices

The project uses the MIT License for its original code, case design, print files, and documentation. External libraries and reference material keep their own licenses.

Vendor libraries, tutorial files, datasheets, and third-party 3D models are not bundled with this release.

## Libraries

| Library | Use | Source |
| --- | --- | --- |
| Arduino-ESP32 3.3.11 | Board support, Wire, SD_MMC, and the RGB LED helper | [Espressif repository and license](https://github.com/espressif/arduino-esp32) |
| esp32-camera | Camera driver, included with the ESP32 board package | [Espressif camera repository](https://github.com/espressif/esp32-camera) |
| U8g2 2.36.19 | OLED drawing and fonts | [U8g2 repository and license](https://github.com/olikraus/u8g2) |
| JPEGDEC 1.8.4 | JPEG decoding for the grayscale preview | [Larry Bank / BitBank Software repository](https://github.com/bitbank2/JPEGDEC), [Apache-2.0 license](https://github.com/bitbank2/JPEGDEC/blob/master/LICENSE) |

The ESP32 package also supplies the JPEG metadata helper. Library code and font data retain their upstream notices. Install the versions listed in the README.

## Hardware and shape references

- **Freenove FNK0085:** [board documentation](https://docs.freenove.com/projects/fnk0085/en/latest/) and [hardware repository](https://github.com/Freenove/Freenove_ESP32_S3_WROOM_Board). The board drawings and ESP32S3_EYE pin assignments informed the wiring and case. The vendor repository lists CC BY-NC-SA 3.0 for its files. Those files are not included here.
- **PartReel, 0.96-inch SSD1306 I2C OLED Module:** [reference model](https://partreel.com/p/ssd1306_module_096/), under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). The unmodified model was consulted for dimensions and hole spacing. The reference STEP model is not included. The case is a separate design. The [LCDwiki MC096VX page](https://www.lcdwiki.com/0.96inch_OLED_Module_MC096VX) is another screen reference.
- **Tactile switch:** [C&K PTS645 drawing](https://www.littelfuse.com/assetdocs/littelfuse-ck-tactile-pts645-series-datasheet?assetguid=d58fc8a8-f6e1-4527-ab68-27635e632e39) and [Adafruit 367](https://www.adafruit.com/product/367). These describe switch types. They do not confirm the fit of a particular kit switch.
- **Three-AAA holder:** [Pololu 1147 specifications](https://www.pololu.com/product/1147/specs) provide the 63 × 37 × 16 mm reference size. The [COMF SBH-431-1AS drawing](https://www.comf-hk.com/wp-content/uploads/SBH-431-1AS-Data-Sheet.pdf) describes a similar holder family. Neither is an exact drawing of the YIENLAO holder used during development. The CAD battery envelope was built from dimensions; it is not a vendor mesh.

Product names identify hardware and reference sources. They do not imply endorsement.

## Image processing references

- **Sobel operator:** developed by Irwin Sobel and Gary Feldman. See [Sobel's historical account and derivation](https://www.researchgate.net/publication/285159837_A_33_isotropic_gradient_operator_for_image_processing).
- **Non-maximum suppression:** the edge-thinning step is described in [John Canny's 1986 paper](https://www.researchgate.net/publication/224377985_A_Computational_Approach_to_Edge_Detection) and the [OpenCV explanation](https://docs.opencv.org/4.13.0/da/d22/tutorial_py_canny.html). This project implements Sobel and NMS in its own C++ code. It does not include OpenCV or the full Canny pipeline.
- **Photo orientation:** EXIF values follow the [ExifTool orientation reference](https://exiftool.org/TagNames/EXIF.html).
