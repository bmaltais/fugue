// Spike 03: libvips speed on 1 to 4 megapixel images, and parity with ComfyUI's formulas.
// Images are float32 RGB in 0..1, as ComfyUI holds them. See README.md.
#include <vips/vips8>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>
using namespace vips;
using Clock = std::chrono::steady_clock;

static VImage makeImage(int w, int h, int seed) {
    VImage x = VImage::xyz(w, h);
    VImage a = (x.extract_band(0) * (0.0031 + seed * 0.0007)).sin() * 0.5 + 0.5;
    VImage b = (x.extract_band(1) * (0.0047 - seed * 0.0003)).cos() * 0.5 + 0.5;
    VImage c = ((x.extract_band(0) + x.extract_band(1)) * 0.0021).sin() * 0.5 + 0.5;
    return a.bandjoin(b).bandjoin(c).cast(VIPS_FORMAT_FLOAT).copy_memory();
}

static VImage clamp01(VImage r) {
    r = (r < 0.0).ifthenelse(0.0, r);
    return (r > 1.0).ifthenelse(1.0, r);
}
static VImage inv(VImage a) { return a * -1.0 + 1.0; }

// ComfyUI Blend.blend_mode and Blend.execute (nodes_post_processing.py).
static VImage g(VImage x) {
    VImage lo = (((x * 16.0 - 12.0) * x) + 4.0) * x;
    return (x <= 0.25).ifthenelse(lo, x.pow(0.5));
}
static VImage blendMode(VImage a, VImage b, const std::string &m) {
    if (m == "normal") return b;
    if (m == "multiply") return a * b;
    if (m == "screen") return inv(inv(a) * inv(b));
    if (m == "overlay") return (a <= 0.5).ifthenelse(a * b * 2.0, inv(inv(a) * inv(b) * 2.0));
    if (m == "soft_light")
        return (b <= 0.5).ifthenelse(a - inv(b * 2.0) * a * inv(a), a + (b * 2.0 - 1.0) * (g(a) - a));
    if (m == "difference") return a - b;
    fprintf(stderr, "bad mode\n");
    exit(2);
}
static VImage blend(VImage a, VImage b, const std::string &m, double f) {
    return clamp01(a * (1 - f) + blendMode(a, b, m) * f);
}

// ComfyUI Blur: a (2r+1)^2 kernel exp(-d^2/(2 sigma^2)) with d on linspace(-1,1), so it is a
// Gaussian of sigma*r pixels, truncated at the radius. Reflect padding.
static VImage gaussKernel(int r, double sigma) {
    int n = 2 * r + 1;
    std::vector<double> k(n * n);
    double sum = 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            double x = -1.0 + 2.0 * i / (n - 1), y = -1.0 + 2.0 * j / (n - 1);
            k[i * n + j] = std::exp(-(x * x + y * y) / (2.0 * sigma * sigma));
            sum += k[i * n + j];
        }
    for (auto &v : k) v /= sum;
    return VImage::new_matrix(n, n, k.data(), n * n);
}
// Separable form: the 2D kernel is a product of two 1D kernels, so two 1D passes equal it.
static VImage gauss1d(int r, double sigma, bool vertical) {
    int n = 2 * r + 1;
    std::vector<double> k(n);
    double sum = 0;
    for (int i = 0; i < n; i++) {
        double x = -1.0 + 2.0 * i / (n - 1);
        k[i] = std::exp(-(x * x) / (2.0 * sigma * sigma));
        sum += k[i];
    }
    for (auto &v : k) v /= sum;
    return vertical ? VImage::new_matrix(1, n, k.data(), n) : VImage::new_matrix(n, 1, k.data(), n);
}
// torch 'reflect' does not repeat the edge pixel (vips mirror does), so build it from flipped crops.
static VImage reflectPadExact(VImage a, int r) {
    int w = a.width(), h = a.height();
    VImage left = a.extract_area(1, 0, r, h).fliphor();
    VImage right = a.extract_area(w - 1 - r, 0, r, h).fliphor();
    VImage row = left.join(a, VIPS_DIRECTION_HORIZONTAL).join(right, VIPS_DIRECTION_HORIZONTAL);
    int W = row.width();
    VImage top = row.extract_area(0, 1, W, r).flipver();
    VImage bot = row.extract_area(0, h - 1 - r, W, r).flipver();
    return top.join(row, VIPS_DIRECTION_VERTICAL).join(bot, VIPS_DIRECTION_VERTICAL);
}
static VImage blurFull(VImage a, int r, double sigma) {  // exact ComfyUI, 2D kernel
    return reflectPadExact(a, r).conv(gaussKernel(r, sigma)).extract_area(r, r, a.width(), a.height());
}
static VImage blurSepTwoPass(VImage a, int r, double sigma) {
    VImage p = reflectPadExact(a, r).conv(gauss1d(r, sigma, false)).conv(gauss1d(r, sigma, true));
    return p.extract_area(r, r, a.width(), a.height());
}
// Native libvips Gaussian with the pixel sigma ComfyUI's kernel corresponds to.
static VImage blurNative(VImage a, int r, double sigma) {
    return a.gaussblur(sigma * r, VImage::option()->set("min_ampl", 0.001)->set("precision", VIPS_PRECISION_FLOAT));
}
// ComfyUI Sharpen = image + 10*alpha*(image - blur), clamped (derived from its kernel).
static VImage sharpenFull(VImage a, int r, double sigma, double alpha) {
    int n = 2 * r + 1;
    VImage gk = gaussKernel(r, sigma);
    std::vector<double> k(n * n);
    double sum = 0;
    for (int i = 0; i < n * n; i++) { k[i] = gk(i % n, i / n)[0] * -(alpha * 10); sum += k[i]; }
    k[r * n + r] = k[r * n + r] - sum + 1.0;
    VImage kern = VImage::new_matrix(n, n, k.data(), n * n);
    return clamp01(reflectPadExact(a, r).conv(kern).extract_area(r, r, a.width(), a.height()));
}
static VImage sharpenFast(VImage a, int r, double sigma, double alpha) {
    VImage b = blurNative(a, r, sigma);
    return clamp01(a + (a - b) * (alpha * 10));
}

static double ms(const std::function<void()> &f, int reps) {
    f();
    std::vector<double> t;
    for (int i = 0; i < reps; i++) {
        auto s = Clock::now();
        f();
        t.push_back(std::chrono::duration<double, std::milli>(Clock::now() - s).count());
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}
static void force(VImage v) { VImage m = v.copy_memory(); (void)m; }

struct Size { int w, h; const char *name; };
static const Size SIZES[] = {{1024, 1024, "1 MP"}, {1448, 1448, "2 MP"}, {2048, 2048, "4 MP"}};

static void bench(int reps) {
    printf("## Single node, one at a time (median of %d runs, ms; vips concurrency %d)\n\n", reps, vips_concurrency_get());
    printf("| Operation | 1 MP | 2 MP | 4 MP |\n| --- | ---: | ---: | ---: |\n");
    struct Op { const char *name; std::function<VImage(VImage, VImage, int, int)> fn; };
    std::vector<Op> ops = {
        {"resize to 50% lanczos3", [](VImage a, VImage, int, int) { return a.resize(0.5, VImage::option()->set("kernel", VIPS_KERNEL_LANCZOS3)); }},
        {"resize to 50% bicubic", [](VImage a, VImage, int, int) { return a.resize(0.5, VImage::option()->set("kernel", VIPS_KERNEL_CUBIC)); }},
        {"resize to 50% bilinear", [](VImage a, VImage, int, int) { return a.resize(0.5, VImage::option()->set("kernel", VIPS_KERNEL_LINEAR)); }},
        {"resize to 50% nearest", [](VImage a, VImage, int, int) { return a.resize(0.5, VImage::option()->set("kernel", VIPS_KERNEL_NEAREST)); }},
        {"resize to 50% area (reduce)", [](VImage a, VImage, int, int) { return a.reduce(2.0, 2.0); }},
        {"resize to 200% lanczos3", [](VImage a, VImage, int, int) { return a.resize(2.0, VImage::option()->set("kernel", VIPS_KERNEL_LANCZOS3)); }},
        {"crop to 50%", [](VImage a, VImage, int w, int h) { return a.extract_area(w / 4, h / 4, w / 2, h / 2); }},
        {"brightness/contrast", [](VImage a, VImage, int, int) { return clamp01(a.linear(1.2, 0.1)); }},
        {"blur r=8 sigma=1 (native gauss)", [](VImage a, VImage, int, int) { return blurNative(a, 8, 1.0); }},
        {"blur r=8 sigma=1 (exact kernel, 2 passes)", [](VImage a, VImage, int, int) { return blurSepTwoPass(a, 8, 1.0); }},
        {"blur r=8 sigma=1 (exact kernel, 2D conv)", [](VImage a, VImage, int, int) { return blurFull(a, 8, 1.0); }},
        {"blur r=31 sigma=1 (native gauss)", [](VImage a, VImage, int, int) { return blurNative(a, 31, 1.0); }},
        {"blur r=31 sigma=1 (exact kernel, 2 passes)", [](VImage a, VImage, int, int) { return blurSepTwoPass(a, 31, 1.0); }},
        {"sharpen r=3 sigma=1 alpha=1 (native)", [](VImage a, VImage, int, int) { return sharpenFast(a, 3, 1.0, 1.0); }},
        {"sharpen r=3 sigma=1 alpha=1 (exact 2D conv)", [](VImage a, VImage, int, int) { return sharpenFull(a, 3, 1.0, 1.0); }},
        {"blend normal", [](VImage a, VImage b, int, int) { return blend(a, b, "normal", 0.7); }},
        {"blend multiply", [](VImage a, VImage b, int, int) { return blend(a, b, "multiply", 0.7); }},
        {"blend screen", [](VImage a, VImage b, int, int) { return blend(a, b, "screen", 0.7); }},
        {"blend overlay", [](VImage a, VImage b, int, int) { return blend(a, b, "overlay", 0.7); }},
        {"blend soft light", [](VImage a, VImage b, int, int) { return blend(a, b, "soft_light", 0.7); }},
        {"blend difference", [](VImage a, VImage b, int, int) { return blend(a, b, "difference", 0.7); }},
    };
    std::vector<std::pair<VImage, VImage>> in;
    for (auto &s : SIZES) in.push_back({makeImage(s.w, s.h, 1), makeImage(s.w, s.h, 2)});
    for (auto &op : ops) {
        printf("| %s |", op.name);
        for (size_t i = 0; i < 3; i++) {
            VImage a = in[i].first, b = in[i].second;
            int w = SIZES[i].w, h = SIZES[i].h;
            double t = ms([&] { force(op.fn(a, b, w, h)); }, reps);
            printf(" %.1f |", t);
        }
        printf("\n");
        fflush(stdout);
    }
}

static void parity() {
    printf("\n## Parity with ComfyUI's formulas (max absolute difference, float 0..1)\n\n");
    VImage a = makeImage(512, 512, 1), b = makeImage(512, 512, 2);
    printf("| Check | Max diff |\n| --- | ---: |\n");
    for (const char *m : {"normal", "multiply", "screen", "overlay", "soft_light", "difference"}) {
        // Reference: scalar C++ of the same formulas, pixel by pixel.
        const float *pa = (const float *)a.data(), *pb = (const float *)b.data();
        VImage out = blend(a, b, m, 0.7);
        const float *po = (const float *)out.copy_memory().data();
        double worst = 0;
        std::string mm = m;
        size_t n = (size_t)512 * 512 * 3;
        VImage o = out.copy_memory();
        po = (const float *)o.data();
        for (size_t i = 0; i < n; i++) {
            double x = pa[i], y = pb[i], r;
            if (mm == "normal") r = y;
            else if (mm == "multiply") r = x * y;
            else if (mm == "screen") r = 1 - (1 - x) * (1 - y);
            else if (mm == "overlay") r = x <= 0.5 ? 2 * x * y : 1 - 2 * (1 - x) * (1 - y);
            else if (mm == "soft_light") {
                double gx = x <= 0.25 ? ((16 * x - 12) * x + 4) * x : std::sqrt(x);
                r = y <= 0.5 ? x - (1 - 2 * y) * x * (1 - x) : x + (2 * y - 1) * (gx - x);
            } else r = x - y;
            r = x * 0.3 + r * 0.7;
            r = std::min(1.0, std::max(0.0, r));
            worst = std::max(worst, std::fabs(r - po[i]));
        }
        printf("| blend %s | %.2e |\n", m, worst);
    }
    // Blur and sharpen: scalar reference of torch's reflect-pad plus conv2d.
    auto refConv = [&](VImage img, const std::vector<double> &k, int r) {
        int w = img.width(), h = img.height(), n = 2 * r + 1;
        VImage m = img.copy_memory();
        const float *p = (const float *)m.data();
        std::vector<double> out((size_t)w * h * 3);
        auto refl = [](int i, int len) { if (i < 0) i = -i; if (i >= len) i = 2 * (len - 1) - i; return i; };
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                for (int c = 0; c < 3; c++) {
                    double s = 0;
                    for (int j = 0; j < n; j++)
                        for (int i = 0; i < n; i++)
                            s += k[j * n + i] * p[((size_t)refl(y + j - r, h) * w + refl(x + i - r, w)) * 3 + c];
                    out[((size_t)y * w + x) * 3 + c] = s;
                }
        return out;
    };
    auto kernelOf = [&](int r, double sigma) {
        int n = 2 * r + 1;
        std::vector<double> k(n * n);
        double sum = 0;
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) {
                double x = -1.0 + 2.0 * i / (n - 1), y = -1.0 + 2.0 * j / (n - 1);
                k[i * n + j] = std::exp(-(x * x + y * y) / (2.0 * sigma * sigma));
                sum += k[i * n + j];
            }
        for (auto &v : k) v /= sum;
        return k;
    };
    auto cmp = [&](VImage got, const std::vector<double> &ref) {
        VImage m = got.copy_memory();
        const float *p = (const float *)m.data();
        double worst = 0;
        for (size_t i = 0; i < ref.size(); i++) worst = std::max(worst, std::fabs(ref[i] - p[i]));
        return worst;
    };
    VImage s = makeImage(128, 128, 1);
    for (auto [r, sg] : std::vector<std::pair<int, double>>{{3, 1.0}, {8, 1.0}, {8, 3.0}, {31, 1.0}}) {
        auto ref = refConv(s, kernelOf(r, sg), r);
        printf("| blur r=%d sigma=%.0f exact 2D conv | %.2e |\n", r, sg, cmp(blurFull(s, r, sg), ref));
        printf("| blur r=%d sigma=%.0f exact 2 passes | %.2e |\n", r, sg, cmp(blurSepTwoPass(s, r, sg), ref));
        printf("| blur r=%d sigma=%.0f native gaussblur (sigma*r px) | %.2e |\n", r, sg, cmp(blurNative(s, r, sg), ref));
    }
    {
        int r = 3; double sg = 1.0, al = 1.0;
        auto k = kernelOf(r, sg);
        int n = 2 * r + 1;
        double sum = 0;
        for (auto &v : k) { v *= -(al * 10); sum += v; }
        k[r * n + r] = k[r * n + r] - sum + 1.0;
        auto ref = refConv(s, k, r);
        for (auto &v : ref) v = std::min(1.0, std::max(0.0, v));
        printf("| sharpen r=3 sigma=1 alpha=1 exact 2D conv | %.2e |\n", cmp(sharpenFull(s, r, sg, al), ref));
        printf("| sharpen r=3 sigma=1 alpha=1 native | %.2e |\n", cmp(sharpenFast(s, r, sg, al), ref));
    }
}

// Executor interaction: N CPU worker threads each run a 2 MP blur+blend chain.
static void contention(int reps) {
    printf("\n## Executor interaction: 11 worker threads, each a 2 MP blur + blend + resize chain, 4 jobs per worker\n\n");
    printf("| vips concurrency | vips cache max | wall time (s) | jobs/s |\n| ---: | ---: | ---: | ---: |\n");
    VImage a = makeImage(1448, 1448, 1), b = makeImage(1448, 1448, 2);
    int workers = 11, perWorker = 4;
    for (int conc : {1, 2, 4, 12}) {
        for (int cache : {0, 100}) {
            vips_concurrency_set(conc);
            vips_cache_set_max(cache);
            std::vector<double> t;
            for (int rep = 0; rep < reps; rep++) {
                auto s = Clock::now();
                std::vector<std::thread> th;
                for (int w = 0; w < workers; w++)
                    th.emplace_back([&] {
                        for (int j = 0; j < perWorker; j++) {
                            VImage x = blurNative(a, 8, 1.0);
                            VImage y = blend(x, b, "soft_light", 0.5);
                            force(y.resize(0.5, VImage::option()->set("kernel", VIPS_KERNEL_LANCZOS3)));
                        }
                    });
                for (auto &x : th) x.join();
                t.push_back(std::chrono::duration<double>(Clock::now() - s).count());
            }
            std::sort(t.begin(), t.end());
            printf("| %d | %d | %.2f | %.1f |\n", conc, cache, t[t.size() / 2], workers * perWorker / t[t.size() / 2]);
            fflush(stdout);
        }
    }
}

// One large graph, evaluated once: does a chain of nodes fuse, and what does lazy evaluation cost?
static void chain(int reps) {
    printf("\n## A five-node chain on 4 MP (crop, brightness, blur, blend, resize)\n\n");
    VImage a = makeImage(2048, 2048, 1), b = makeImage(2048, 2048, 2);
    auto fused = [&] {
        VImage x = a.extract_area(100, 100, 1800, 1800);
        x = clamp01(x.linear(1.1, 0.05));
        x = blurNative(x, 4, 1.0);
        x = blend(x, b.extract_area(0, 0, 1800, 1800), "overlay", 0.6);
        force(x.resize(0.5, VImage::option()->set("kernel", VIPS_KERNEL_LANCZOS3)));
    };
    auto stepwise = [&] {  // every node materialises its result, as the engine's cache would
        VImage x = a.extract_area(100, 100, 1800, 1800).copy_memory();
        x = clamp01(x.linear(1.1, 0.05)).copy_memory();
        x = blurNative(x, 4, 1.0).copy_memory();
        x = blend(x, b.extract_area(0, 0, 1800, 1800), "overlay", 0.6).copy_memory();
        force(x.resize(0.5, VImage::option()->set("kernel", VIPS_KERNEL_LANCZOS3)));
    };
    printf("| Evaluation | ms |\n| --- | ---: |\n");
    printf("| lazy, one pipeline | %.1f |\n", ms(fused, reps));
    printf("| each node materialised | %.1f |\n", ms(stepwise, reps));
}

int main(int argc, char **argv) {
    if (VIPS_INIT(argv[0])) vips_error_exit(nullptr);
    std::string what = argc > 1 ? argv[1] : "all";
    int reps = argc > 2 ? atoi(argv[2]) : 15;
    vips_cache_set_max(0);
    if (what == "bench" || what == "all") bench(reps);
    if (what == "parity" || what == "all") parity();
    if (what == "contention" || what == "all") contention(5);
    if (what == "chain" || what == "all") chain(reps);
    vips_shutdown();
}
