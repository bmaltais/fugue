#include "canvas/selection_contour.h"

#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

namespace fugue {

int contourCell(double zoom, double minScreen) {
    int cell = 1;
    while (cell < (1 << 14) && cell * zoom < minScreen)
        cell *= 2;
    return cell;
}

namespace {

constexpr double level = 127.5;

struct Grid {
    int x0, y0; // the first node's index (in cells)
    int w, h;   // nodes across and down
    int cell, width, height;
    std::vector<uint8_t> v;
    uint8_t at(int i, int j) const { return v[size_t(j) * size_t(w) + size_t(i)]; }
    // A node sits at the center of the picture pixel it samples, so a hard edge lands on the pixel boundary.
    QPointF pos(int i, int j) const { return {(x0 + i) * double(cell) + 0.5, (y0 + j) * double(cell) + 0.5}; }
};

// Edge ids: horizontal (node i,j to i+1,j) = 2 * n, vertical (i,j to i,j+1) = 2 * n + 1, n = j * w + i.
QPointF edgePoint(const Grid &g, int id) {
    const int n = id / 2, i = n % g.w, j = n / g.w;
    const bool horizontal = id % 2 == 0;
    const int i2 = horizontal ? i + 1 : i, j2 = horizontal ? j : j + 1;
    const double a = g.at(i, j), b = g.at(i2, j2);
    const double t = a == b ? 0.5 : std::clamp((level - a) / (b - a), 0.0, 1.0);
    const QPointF p = g.pos(i, j), q = g.pos(i2, j2);
    return p + (q - p) * t;
}

} // namespace

std::vector<ContourPath> selectionContour(int width, int height, const std::function<uint8_t(int, int)> &value,
                                          const QRect &window, int cell) {
    std::vector<ContourPath> paths;
    cell = std::max(1, cell);
    const QRect clipped = window.intersected(QRect(0, 0, width, height));
    if (clipped.isEmpty())
        return paths;
    Grid g;
    g.cell = cell;
    g.width = width;
    g.height = height;
    // One node of margin each side, so an edge on the window's border still closes in it.
    g.x0 = clipped.left() / cell - 1;
    g.y0 = clipped.top() / cell - 1;
    g.w = clipped.right() / cell + 3 - g.x0;
    g.h = clipped.bottom() / cell + 3 - g.y0;
    g.v.resize(size_t(g.w) * size_t(g.h));
    for (int j = 0; j < g.h; ++j)
        for (int i = 0; i < g.w; ++i) {
            const int x = (g.x0 + i) * cell, y = (g.y0 + j) * cell;
            g.v[size_t(j) * size_t(g.w) + size_t(i)] = x < 0 || y < 0 || x >= width || y >= height ? 0 : value(x, y);
        }

    std::vector<std::array<int, 2>> segments;
    const auto inside = [&](int i, int j) { return g.at(i, j) >= 128; };
    for (int j = 0; j + 1 < g.h; ++j)
        for (int i = 0; i + 1 < g.w; ++i) {
            const int n = j * g.w + i;
            const int T = 2 * n, B = 2 * (n + g.w), L = 2 * n + 1, R = 2 * (n + 1) + 1;
            const int tl = inside(i, j), tr = inside(i + 1, j), br = inside(i + 1, j + 1), bl = inside(i, j + 1);
            const int code = tl | tr << 1 | br << 2 | bl << 3;
            if (code == 0 || code == 15)
                continue;
            const auto add = [&](int a, int b) { segments.push_back({a, b}); };
            const bool center = (g.at(i, j) + g.at(i + 1, j) + g.at(i + 1, j + 1) + g.at(i, j + 1)) / 4.0 >= level;
            switch (code) {
            case 1: case 14: add(L, T); break;
            case 2: case 13: add(T, R); break;
            case 3: case 12: add(L, R); break;
            case 4: case 11: add(R, B); break;
            case 6: case 9: add(T, B); break;
            case 7: case 8: add(L, B); break;
            case 5:
                if (center) { add(T, R); add(L, B); } else { add(L, T); add(R, B); }
                break;
            case 10:
                if (center) { add(L, T); add(R, B); } else { add(T, R); add(L, B); }
                break;
            }
        }

    // Chain the segments that share an edge into polylines.
    std::unordered_map<int, std::array<int, 2>> at; // edge -> the (up to two) segments on it, -1 for none
    for (size_t s = 0; s < segments.size(); ++s)
        for (int e : segments[s]) {
            auto it = at.try_emplace(e, std::array<int, 2>{-1, -1}).first;
            it->second[it->second[0] < 0 ? 0 : 1] = int(s);
        }
    std::vector<char> used(segments.size(), 0);
    const auto next = [&](int edge, int from) {
        for (int s : at[edge])
            if (s >= 0 && s != from && !used[size_t(s)])
                return s;
        return -1;
    };
    for (size_t start = 0; start < segments.size(); ++start) {
        if (used[start])
            continue;
        used[start] = 1;
        std::vector<int> edges{segments[start][0], segments[start][1]};
        for (int s = int(start), tail = edges.back(); (s = next(tail, s)) >= 0;) {
            used[size_t(s)] = 1;
            tail = segments[size_t(s)][0] == tail ? segments[size_t(s)][1] : segments[size_t(s)][0];
            edges.push_back(tail);
        }
        if (edges.front() != edges.back()) { // open: grow the other end too
            std::vector<int> before;
            for (int s = int(start), head = edges.front(); (s = next(head, s)) >= 0;) {
                used[size_t(s)] = 1;
                head = segments[size_t(s)][0] == head ? segments[size_t(s)][1] : segments[size_t(s)][0];
                before.push_back(head);
            }
            edges.insert(edges.begin(), before.rbegin(), before.rend());
        }
        ContourPath path;
        path.reserve(edges.size());
        for (int e : edges)
            path.push_back(edgePoint(g, e));
        paths.push_back(std::move(path));
    }
    return paths;
}

} // namespace fugue
