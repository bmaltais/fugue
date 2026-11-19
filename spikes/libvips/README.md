# Spike 03 benchmark (libvips)

`bench.cpp` measures libvips on 1, 2 and 4 megapixel float32 RGB images, checks parity with
ComfyUI's formulas, and tests thread and cache settings. Results and the decision are in
[docs/spikes/03-libvips.md](../../docs/spikes/03-libvips.md).

```
g++ -O2 -std=c++20 -Wall -Wextra bench.cpp -o bench $(pkg-config --cflags --libs vips-cpp)
./bench all 15          # or: bench | parity | contention | chain  [repetitions]
```
