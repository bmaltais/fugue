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

} // namespace

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
    return position >= 0 && position < layerCount() ? m_layers[size_t(position)].info : LayerInfo{0, QString(), LayerKind::Paint, false, 0};
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
    step.info = m_layers[size_t(at)].info;
    step.from = at;
    m_layers.erase(m_layers.begin() + at);
    m_active = m_layers.empty() ? 0 : m_layers.back().info.id;
}

void PaintDocument::attach(Step &step) {
    if (!step.kept)
        return;
    const int at = std::clamp(step.from, 0, layerCount());
    m_layers.insert(m_layers.begin() + at, Layer{step.info, step.kept});
    step.kept.reset();
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

MaskPaint PaintDocument::flatten() const {
    const size_t count = size_t(m_width) * size_t(m_height);
    std::vector<double> color(count * 4, 0.0); // straight, 0 to 1
    for (const Layer &l : m_layers) {
        if (!l.info.visible || l.info.kind == LayerKind::Mask || l.info.opacity <= 0)
            continue;
        const std::vector<uint8_t> &src = l.pixels->pixels();
        for (size_t i = 0; i < count; ++i) {
            const double a = src[i * 4 + 3] / 255.0 * l.info.opacity;
            if (a <= 0)
                continue;
            double *d = &color[i * 4];
            const double outA = a + d[3] * (1 - a);
            for (int k = 0; k < 3; ++k)
                d[k] = (src[i * 4 + size_t(k)] / 255.0 * a + d[k] * d[3] * (1 - a)) / outA;
            d[3] = outA;
        }
    }
    std::vector<uint8_t> out(count * 4);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = byteOf(color[i]);
    return MaskPaint(m_width, m_height, out, 4);
}

MaskPaint PaintDocument::flattenMask() const {
    std::vector<uint8_t> out(size_t(m_width) * size_t(m_height), 0);
    for (const Layer &l : m_layers) {
        if (!l.info.visible || l.info.kind != LayerKind::Mask)
            continue;
        const std::vector<uint8_t> &src = l.pixels->pixels();
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = std::max(out[i], byteOf(src[i] / 255.0 * l.info.opacity));
    }
    return MaskPaint(m_width, m_height, out, 1);
}

} // namespace fugue::mask
