#include "paint_document.h"

#include <algorithm>
#include <cmath>

namespace fugue::mask {

namespace {

constexpr size_t maximumSteps = 300;

double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

uint8_t byteOf(double v) { return uint8_t(std::lround(clamp01(v) * 255.0)); }

// A one-channel buffer as opaque gray color.
MaskPaint asColor(const MaskPaint &in) {
    if (in.channels() == 4)
        return in;
    std::vector<uint8_t> pixels(in.pixels().size() * 4);
    for (size_t i = 0; i < in.pixels().size(); ++i) {
        pixels[i * 4] = pixels[i * 4 + 1] = pixels[i * 4 + 2] = in.pixels()[i];
        pixels[i * 4 + 3] = 255;
    }
    return MaskPaint(in.width(), in.height(), pixels, 4);
}

// A color buffer as a mask: its first channel.
MaskPaint asMask(const MaskPaint &in) {
    if (in.channels() == 1)
        return in;
    std::vector<uint8_t> pixels(in.pixels().size() / 4);
    for (size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = in.pixels()[i * 4];
    return MaskPaint(in.width(), in.height(), pixels, 1);
}

struct BlendName {
    Blend blend;
    const char *id;
    const char *op;
};

constexpr BlendName kBlends[] = {
    {Blend::Normal, "normal", "svg:src-over"},       {Blend::Multiply, "multiply", "svg:multiply"},
    {Blend::Screen, "screen", "svg:screen"},         {Blend::Overlay, "overlay", "svg:overlay"},
    {Blend::SoftLight, "soft-light", "svg:soft-light"}, {Blend::Darken, "darken", "svg:darken"},
    {Blend::Lighten, "lighten", "svg:lighten"},      {Blend::Add, "add", "svg:plus"},
};

} // namespace

Blend blendOfOp(const QString &compositeOp, bool *known) {
    if (known)
        *known = true;
    if (compositeOp.isEmpty())
        return Blend::Normal;
    for (const BlendName &b : kBlends)
        if (compositeOp == QLatin1String(b.op))
            return b.blend;
    if (known)
        *known = false;
    return Blend::Normal;
}

QString opOfBlend(Blend blend) {
    for (const BlendName &b : kBlends)
        if (b.blend == blend)
            return QString::fromLatin1(b.op);
    return QStringLiteral("svg:src-over");
}

QString blendId(Blend blend) {
    for (const BlendName &b : kBlends)
        if (b.blend == blend)
            return QString::fromLatin1(b.id);
    return QStringLiteral("normal");
}

bool blendFromId(const QString &id, Blend *blend) {
    for (const BlendName &b : kBlends)
        if (id == QLatin1String(b.id)) {
            *blend = b.blend;
            return true;
        }
    return false;
}

double blendChannel(Blend blend, double cb, double cs) {
    switch (blend) {
    case Blend::Normal: return cs;
    case Blend::Multiply: return cb * cs;
    case Blend::Screen: return cb + cs - cb * cs;
    case Blend::Overlay: // hard light with the layers swapped
        return cb <= 0.5 ? 2 * cb * cs : 1 - 2 * (1 - cb) * (1 - cs);
    case Blend::SoftLight: {
        if (cs <= 0.5)
            return cb - (1 - 2 * cs) * cb * (1 - cb);
        const double d = cb <= 0.25 ? ((16 * cb - 12) * cb + 4) * cb : std::sqrt(cb);
        return cb + (2 * cs - 1) * (d - cb);
    }
    case Blend::Darken: return std::min(cb, cs);
    case Blend::Lighten: return std::max(cb, cs);
    case Blend::Add: return std::min(1.0, cb + cs);
    }
    return cs;
}

PaintDocument::PaintDocument(int width, int height) : m_width(std::max(1, width)), m_height(std::max(1, height)) {}

PaintDocument::Layer *PaintDocument::find(int id) {
    for (Layer &l : m_layers)
        if (l.info.id == id)
            return &l;
    return nullptr;
}

const PaintDocument::Layer *PaintDocument::find(int id) const {
    for (const Layer &l : m_layers)
        if (l.info.id == id)
            return &l;
    return nullptr;
}

PaintDocument::Layer *PaintDocument::paintable() {
    Layer *l = find(m_active);
    return l && l->info.kind != LayerKind::Picture ? l : nullptr;
}

bool PaintDocument::canPaint() const {
    const Layer *l = find(m_active);
    return l && l->info.kind != LayerKind::Picture;
}

LayerInfo PaintDocument::layer(int position) const {
    if (position >= 0 && position < layerCount())
        return m_layers[size_t(position)].info;
    LayerInfo none;
    none.visible = false;
    none.opacity = 0;
    return none;
}

int PaintDocument::positionOf(int id) const {
    for (size_t i = 0; i < m_layers.size(); ++i)
        if (m_layers[i].info.id == id)
            return int(i);
    return -1;
}

const MaskPaint *PaintDocument::pixels(int id) const {
    const Layer *l = find(id);
    return l ? l->pixels.get() : nullptr;
}

// ---- history ------------------------------------------------------------------------------------

void PaintDocument::push(Step step) {
    m_redo.clear();
    for (Layer &l : m_layers)
        l.pixels->forgetRedo();
    m_undo.push_back(std::move(step));
    if (m_undo.size() > maximumSteps)
        m_undo.erase(m_undo.begin());
}

bool PaintDocument::canUndo() const { return !m_undo.empty() && !m_stroking; }
bool PaintDocument::canRedo() const { return !m_redo.empty() && !m_stroking; }

void PaintDocument::undo() {
    while (canUndo()) {
        Step step = std::move(m_undo.back());
        m_undo.pop_back();
        if (apply(step, true)) {
            m_redo.push_back(std::move(step));
            return;
        }
    }
}

void PaintDocument::redo() {
    if (!canRedo())
        return;
    Step step = std::move(m_redo.back());
    m_redo.pop_back();
    apply(step, false);
    m_undo.push_back(std::move(step));
}

// Detaches the layer of a step into the step (what undoing an Add and redoing a Remove do).
void PaintDocument::detach(Step &step) {
    const int at = positionOf(step.id);
    if (at < 0)
        return;
    step.kept = m_layers[size_t(at)].pixels;
    step.keptMask = m_layers[size_t(at)].mask;
    step.info = m_layers[size_t(at)].info;
    step.from = at;
    m_layers.erase(m_layers.begin() + at);
    m_active = m_layers.empty() ? 0 : m_layers.back().info.id;
}

void PaintDocument::attach(Step &step) {
    if (!step.kept)
        return;
    const int at = std::clamp(step.from, 0, layerCount());
    m_layers.insert(m_layers.begin() + at, Layer{step.info, step.kept, step.keptMask});
    step.kept.reset();
    step.keptMask.reset();
}

// Does a step either way; false when it was stale (its layer's buffer had dropped the step) and so did nothing.
bool PaintDocument::apply(Step &step, bool undoing) {
    switch (step.kind) {
    case Step::Kind::Pixels: {
        Layer *l = find(step.id);
        if (!l)
            return false;
        if (undoing) {
            if (!l->pixels->canUndo())
                return false;
            l->pixels->undo();
        } else {
            l->pixels->redo();
        }
        return true;
    }
    case Step::Kind::Add:
    case Step::Kind::Remove: {
        const bool removing = (step.kind == Step::Kind::Remove) == !undoing;
        if (removing) {
            detach(step);
            if (undoing && find(step.active))
                m_active = step.active;
        } else {
            attach(step);
            m_active = undoing ? step.active : step.id;
        }
        return true;
    }
    case Step::Kind::Move: {
        const int at = positionOf(step.id);
        if (at < 0)
            return false;
        Layer l = std::move(m_layers[size_t(at)]);
        m_layers.erase(m_layers.begin() + at);
        m_layers.insert(m_layers.begin() + (undoing ? step.from : step.to), std::move(l));
        return true;
    }
    }
    return false;
}

// ---- layers -------------------------------------------------------------------------------------

int PaintDocument::addLayer(LayerKind kind, const QString &name, const MaskPaint &picture) {
    if (m_stroking)
        return 0;
    Layer l;
    l.info.id = m_nextId++;
    l.info.kind = kind;
    l.info.name = name.isEmpty() ? QStringLiteral("Layer %1").arg(l.info.id) : name;
    if (kind == LayerKind::Mask)
        l.pixels = std::make_shared<MaskPaint>(m_width, m_height, 1);
    else if (kind == LayerKind::Paint)
        l.pixels = std::make_shared<MaskPaint>(m_width, m_height, 4);
    else
        l.pixels = std::make_shared<MaskPaint>(asColor(picture).scaled(m_width, m_height));
    l.pixels->forgetHistory();
    Step step;
    step.kind = Step::Kind::Add;
    step.id = l.info.id;
    step.active = m_active;
    step.from = layerCount();
    m_layers.push_back(l);
    m_active = l.info.id;
    push(std::move(step));
    return l.info.id;
}

int PaintDocument::loadLayer(LayerKind kind, const QString &name, bool visible, double opacity, const MaskPaint &pixels) {
    if (m_stroking)
        return 0;
    Layer l;
    l.info.id = m_nextId++;
    l.info.kind = kind;
    l.info.name = name.isEmpty() ? QStringLiteral("Layer %1").arg(l.info.id) : name;
    l.info.visible = visible;
    l.info.opacity = clamp01(opacity);
    l.pixels = std::make_shared<MaskPaint>((kind == LayerKind::Mask ? asMask(pixels) : asColor(pixels)).scaled(m_width, m_height));
    l.pixels->forgetHistory();
    m_layers.push_back(l);
    m_active = l.info.id;
    return l.info.id;
}

int PaintDocument::loadLayer(const LayerInfo &info, const MaskPaint &pixels, const MaskPaint *mask) {
    const int id = loadLayer(info.kind, info.name, info.visible, info.opacity, pixels);
    if (id == 0)
        return 0;
    Layer *l = find(id);
    setCompositeOp(id, info.compositeOp);
    l->info.producer = info.producer;
    l->info.created = info.created;
    l->info.locked = info.locked;
    if (mask && info.kind != LayerKind::Mask) {
        setLayerMask(id, *mask);
        l->info.maskEnabled = info.maskEnabled;
    }
    return id;
}

bool PaintDocument::setPicture(int id, const MaskPaint &picture) {
    Layer *l = find(id);
    if (!l || l->info.kind != LayerKind::Picture)
        return false;
    l->pixels = std::make_shared<MaskPaint>(asColor(picture).scaled(m_width, m_height));
    l->pixels->forgetHistory();
    return true;
}

PaintDocument PaintDocument::scaledTo(int width, int height) const {
    PaintDocument out(width, height);
    for (const Layer &l : m_layers)
        out.loadLayer(l.info, *l.pixels, l.mask.get());
    return out;
}

void PaintDocument::forgetHistory() {
    m_undo.clear();
    m_redo.clear();
    for (Layer &l : m_layers)
        l.pixels->forgetHistory();
}

bool PaintDocument::removeLayer(int id) {
    const int at = positionOf(id);
    if (at < 0 || m_stroking)
        return false;
    Step step;
    step.kind = Step::Kind::Remove;
    step.id = id;
    step.active = m_active;
    detach(step);
    push(std::move(step));
    return true;
}

bool PaintDocument::moveLayer(int id, int position) {
    const int at = positionOf(id);
    if (at < 0 || m_stroking)
        return false;
    position = std::clamp(position, 0, layerCount() - 1);
    if (position == at)
        return true;
    Step step;
    step.kind = Step::Kind::Move;
    step.id = id;
    step.from = at;
    step.to = position;
    Layer l = std::move(m_layers[size_t(at)]);
    m_layers.erase(m_layers.begin() + at);
    m_layers.insert(m_layers.begin() + position, std::move(l));
    push(std::move(step));
    return true;
}

IRect PaintDocument::takeDirty() {
    IRect all;
    for (Layer &l : m_layers)
        all = all.united(l.pixels->takeDirty());
    return all;
}

bool PaintDocument::setActiveLayer(int id) {
    if (!find(id) || m_stroking)
        return false;
    m_active = id;
    return true;
}

bool PaintDocument::setVisible(int id, bool visible) {
    Layer *l = find(id);
    if (l)
        l->info.visible = visible;
    return l;
}

bool PaintDocument::setOpacity(int id, double opacity) {
    Layer *l = find(id);
    if (l)
        l->info.opacity = clamp01(opacity);
    return l;
}

bool PaintDocument::setName(int id, const QString &name) {
    Layer *l = find(id);
    if (l)
        l->info.name = name;
    return l;
}

bool PaintDocument::setBlend(int id, Blend blend) { return setCompositeOp(id, blend == Blend::Normal ? QString() : opOfBlend(blend)); }

bool PaintDocument::setCompositeOp(int id, const QString &compositeOp) {
    Layer *l = find(id);
    if (l)
        l->info.compositeOp = compositeOp == QLatin1String("svg:src-over") ? QString() : compositeOp;
    return l;
}

bool PaintDocument::setOrigin(int id, const QString &producer, const QString &created) {
    Layer *l = find(id);
    if (l) {
        l->info.producer = producer;
        l->info.created = created;
    }
    return l;
}

bool PaintDocument::setLayerMask(int id, const MaskPaint &mask) {
    Layer *l = find(id);
    if (!l || l->info.kind == LayerKind::Mask)
        return false;
    l->mask = std::make_shared<MaskPaint>(asMask(mask).scaled(m_width, m_height));
    l->mask->forgetHistory();
    l->info.hasMask = true;
    return true;
}

bool PaintDocument::removeLayerMask(int id) {
    Layer *l = find(id);
    if (!l || !l->info.hasMask)
        return false;
    l->mask.reset();
    l->info.hasMask = false;
    l->info.maskEnabled = true;
    return true;
}

bool PaintDocument::setMaskEnabled(int id, bool enabled) {
    Layer *l = find(id);
    if (!l || !l->info.hasMask)
        return false;
    l->info.maskEnabled = enabled;
    return true;
}

const MaskPaint *PaintDocument::maskPixels(int id) const {
    const Layer *l = find(id);
    return l && l->info.hasMask ? l->mask.get() : nullptr;
}

// ---- painting -----------------------------------------------------------------------------------

bool PaintDocument::beginStroke() {
    Layer *l = paintable();
    if (!l || m_stroking)
        return false;
    m_stroking = true;
    m_strokeLayer = l->info.id;
    m_revisionBefore = l->pixels->revision();
    l->pixels->beginStroke(mode, brush);
    return true;
}

void PaintDocument::strokeTo(Point point, double pressure) {
    if (!m_stroking)
        return;
    if (Layer *l = find(m_strokeLayer))
        l->pixels->strokeTo(point, pressure);
}

void PaintDocument::endStroke() {
    if (!m_stroking)
        return;
    m_stroking = false;
    Layer *l = find(m_strokeLayer);
    if (l)
        l->pixels->endStroke();
    recordIfChanged(l);
}

// A step for what the layer's buffer just committed (nothing when it changed nothing).
void PaintDocument::recordIfChanged(Layer *l) {
    if (!l || l->pixels->revision() == m_revisionBefore)
        return;
    Step step;
    step.id = l->info.id;
    push(std::move(step));
}

bool PaintDocument::fillPolygon(const std::vector<Point> &outline) {
    Layer *l = paintable();
    if (!l || m_stroking)
        return false;
    m_revisionBefore = l->pixels->revision();
    l->pixels->fillPolygon(outline, mode, brush.opacity, brush.color);
    recordIfChanged(l);
    return true;
}

bool PaintDocument::fillRect(double x, double y, double width, double height) {
    Layer *l = paintable();
    if (!l || m_stroking)
        return false;
    m_revisionBefore = l->pixels->revision();
    l->pixels->fillRect(x, y, width, height, mode, brush.opacity, brush.color);
    recordIfChanged(l);
    return true;
}

bool PaintDocument::clearLayer() {
    Layer *l = paintable();
    if (!l || m_stroking)
        return false;
    m_revisionBefore = l->pixels->revision();
    l->pixels->clear();
    recordIfChanged(l);
    return true;
}

bool PaintDocument::fillLayer() {
    Layer *l = paintable();
    if (!l || m_stroking)
        return false;
    m_revisionBefore = l->pixels->revision();
    Rgba color = brush.color;
    color.a = byteOf(color.a / 255.0 * brush.opacity);
    l->pixels->fill(color);
    recordIfChanged(l);
    return true;
}

bool PaintDocument::invertLayer() {
    Layer *l = paintable();
    if (!l || m_stroking)
        return false;
    m_revisionBefore = l->pixels->revision();
    l->pixels->invert();
    recordIfChanged(l);
    return true;
}

// ---- the result ---------------------------------------------------------------------------------

MaskPaint PaintDocument::flatten() const { return flattenArea({0, 0, m_width, m_height}); }

MaskPaint PaintDocument::flattenMask() const { return flattenMaskArea({0, 0, m_width, m_height}); }

// The part of the canvas inside `area` (cut to the canvas), as a buffer of its own size.
MaskPaint PaintDocument::flattenArea(const IRect &want) const {
    const int x0 = std::clamp(want.x, 0, m_width), y0 = std::clamp(want.y, 0, m_height);
    const int w = std::clamp(want.x + want.width, x0, m_width) - x0, h = std::clamp(want.y + want.height, y0, m_height) - y0;
    const size_t count = size_t(w) * size_t(h);
    std::vector<double> color(count * 4, 0.0); // straight, 0 to 1
    for (const Layer &l : m_layers) {
        if (!l.info.visible || l.info.kind == LayerKind::Mask || l.info.opacity <= 0)
            continue;
        const std::vector<uint8_t> &src = l.pixels->pixels();
        const std::vector<uint8_t> *mask = l.info.hasMask && l.info.maskEnabled && l.mask ? &l.mask->pixels() : nullptr;
        const Blend blend = l.info.blend();
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * size_t(w) + size_t(x);
                const size_t p = size_t(y0 + y) * size_t(m_width) + size_t(x0 + x);
                const size_t s = p * 4;
                double a = src[s + 3] / 255.0 * l.info.opacity;
                if (mask)
                    a *= (*mask)[p] / 255.0;
                if (a <= 0)
                    continue;
                double *d = &color[i * 4];
                const double outA = a + d[3] * (1 - a);
                if (blend == Blend::Normal) {
                    for (int k = 0; k < 3; ++k)
                        d[k] = (src[s + size_t(k)] / 255.0 * a + d[k] * d[3] * (1 - a)) / outA;
                } else {
                    // W3C Compositing: Cs' = (1 - ab) Cs + ab B(Cb, Cs), then source-over with Cs'.
                    for (int k = 0; k < 3; ++k) {
                        const double cs = src[s + size_t(k)] / 255.0;
                        const double mixed = (1 - d[3]) * cs + d[3] * blendChannel(blend, d[k], cs);
                        d[k] = (mixed * a + d[k] * d[3] * (1 - a)) / outA;
                    }
                }
                d[3] = outA;
            }
    }
    std::vector<uint8_t> out(count * 4);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = byteOf(color[i]);
    return MaskPaint(std::max(1, w), std::max(1, h), count == 0 ? std::vector<uint8_t>(4, 0) : out, 4);
}

MaskPaint PaintDocument::flattenMaskArea(const IRect &want) const {
    const int x0 = std::clamp(want.x, 0, m_width), y0 = std::clamp(want.y, 0, m_height);
    const int w = std::clamp(want.x + want.width, x0, m_width) - x0, h = std::clamp(want.y + want.height, y0, m_height) - y0;
    std::vector<uint8_t> out(size_t(w) * size_t(h), 0);
    for (const Layer &l : m_layers) {
        if (!l.info.visible || l.info.kind != LayerKind::Mask)
            continue;
        const std::vector<uint8_t> &src = l.pixels->pixels();
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t &o = out[size_t(y) * size_t(w) + size_t(x)];
                o = std::max(o, byteOf(src[size_t(y0 + y) * size_t(m_width) + size_t(x0 + x)] / 255.0 * l.info.opacity));
            }
    }
    return MaskPaint(std::max(1, w), std::max(1, h), out.empty() ? std::vector<uint8_t>(1, 0) : out, 1);
}

} // namespace fugue::mask
