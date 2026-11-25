# Third-party code in the nodes library

- `stb_truetype.h` v1.28 (https://github.com/nothings/stb): public domain (or MIT, the file's choice), unchanged. It rasterizes
  text with no GUI and no system font, for Draw Text (`src/nodes/text_raster.*`). Compiled only in `text_raster.cpp`.
