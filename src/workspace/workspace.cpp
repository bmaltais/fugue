#include "workspace/workspace.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>

#include "commands/command_registry.h"
#include "engine/atomic_file.h"

namespace fugue {

namespace {

bool isDefault(const AreaTree &tree) {
    return tree.root().leaf && tree.root().area.view == ViewRegistry::kGraph;
}

bool readAxis(const QString &text, AreaAxis *axis) {
    if (text == QLatin1String("row"))
        *axis = AreaAxis::Row;
    else if (text == QLatin1String("column"))
        *axis = AreaAxis::Column;
    else
        return false;
    return true;
}

AreaTree::Path toPath(const QVariantList &list) {
    AreaTree::Path path;
    for (const QVariant &index : list)
        path << index.toInt();
    return path;
}

} // namespace

Workspace::Workspace(QString path, QObject *parent) : QObject(parent), m_path(std::move(path)) {
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(kSaveDelayMs);
    connect(&m_saveTimer, &QTimer::timeout, this, &Workspace::save);
}

Workspace::~Workspace() { flush(); }

QVariantMap Workspace::tree() const { return m_tree.toJson().toVariantMap(); }

void Workspace::setActive(int id) {
    if (id == m_active || !m_tree.area(id))
        return;
    m_active = id;
    emit activeChanged();
}

int Workspace::graphArea() const {
    for (const int id : m_tree.areaIds())
        if (m_tree.area(id)->view == ViewRegistry::kGraph)
            return id;
    return 0;
}

void Workspace::setBodySize(const QSizeF &size) {
    if (size == m_bodySize)
        return;
    m_bodySize = size;
    emit bodySizeChanged();
}

AreaTree::MinimumSize Workspace::minimum() const {
    return [this](const QString &view) { return m_views.minimumSize(view); };
}

QString Workspace::newViewFor(int id) const {
    const Area *area = m_tree.area(id);
    if (!area)
        return {};
    const ViewRegistry::ViewType *own = m_views.find(area->view);
    if (own && own->multiplicity == ViewRegistry::Multiplicity::Many)
        return own->id;
    for (const QString &type : m_views.ids())
        if (m_views.find(type)->multiplicity == ViewRegistry::Multiplicity::Many)
            return type;
    return {};
}

void Workspace::keepGraph(AreaTree &tree) const {
    // One Graph per window. A layout without one stays as it is: the Graph is then hidden, not forced into an area.
    bool seen = false;
    for (const int id : tree.areaIds()) {
        if (tree.area(id)->view != ViewRegistry::kGraph)
            continue;
        if (seen)
            tree.setView(id, ViewRegistry::kCanvas);
        seen = true;
    }
}

int Workspace::fallbackArea() const {
    if (const int graph = graphArea())
        return graph;
    const QList<int> ids = m_tree.areaIds();
    return ids.isEmpty() ? 0 : ids.first();
}

void Workspace::commit(const AreaTree &next) {
    m_tree = next;
    if (!m_tree.area(m_active)) {
        m_active = fallbackArea();
        emit activeChanged();
    }
    emit changed();
    if (m_path.isEmpty())
        return;
    if (isDefault(m_tree)) {
        // The default window keeps no file: the next start reads nothing.
        m_saveTimer.stop();
        if (QFile::exists(m_path) && !QFile::remove(m_path))
            qWarning().noquote() << "workspace: could not remove" << m_path;
        return;
    }
    m_saveTimer.start();
}

void Workspace::save() {
    m_saveTimer.stop();
    if (m_path.isEmpty() || isDefault(m_tree))
        return;
    if (const Errors errors = replaceFile(m_path, m_tree.save(), QStringLiteral("the layout")); !errors.isEmpty())
        qWarning().noquote() << "workspace:" << errors.first().message;
}

void Workspace::flush() {
    if (m_saveTimer.isActive())
        save();
}

void Workspace::restore() {
    if (m_path.isEmpty() || !QFileInfo(m_path).isFile())
        return;
    QFile file(m_path);
    QString problem;
    AreaTree read;
    if (file.size() > kMaxFileSize)
        problem = QStringLiteral("larger than %1 bytes").arg(kMaxFileSize);
    else if (!file.open(QIODevice::ReadOnly))
        problem = file.errorString();
    else
        read = AreaTree::load(file.readAll(), &problem);
    file.close();
    if (!problem.isEmpty()) {
        const QString bad = m_path + QStringLiteral(".bad");
        QFile::remove(bad);
        if (!QFile::rename(m_path, bad))
            qWarning().noquote() << "workspace: could not keep" << m_path << "as" << bad;
        qWarning().noquote() << "workspace:" << m_path << problem;
        emit message(QCoreApplication::translate(
            "Workspace", "The saved layout could not be read, so the window has one area again. The file is kept as %1.")
                         .arg(QFileInfo(bad).fileName()));
        return;
    }
    keepGraph(read);
    m_tree = read;
    m_active = fallbackArea();
    emit changed();
    emit activeChanged();
}

bool Workspace::canSplit(int id, const QString &axis) const {
    AreaAxis along;
    if (!m_tree.area(id) || !readAxis(axis, &along))
        return false;
    const QString view = newViewFor(id);
    if (view.isEmpty())
        return false;
    AreaTree next = m_tree;
    if (!next.split(id, along, view))
        return false;
    return m_bodySize.isEmpty() || next.fits(m_bodySize, minimum());
}

int Workspace::splitArea(int id, const QString &axis) {
    if (!canSplit(id, axis))
        return 0;
    AreaAxis along;
    readAxis(axis, &along);
    AreaTree next = m_tree;
    const int made = next.split(id, along, newViewFor(id));
    commit(next);
    setActive(made);
    return made;
}

bool Workspace::closeArea(int id) {
    const Area *area = m_tree.area(id);
    if (!area || m_tree.count() < 2)
        return false;
    AreaTree next = m_tree;
    const int taker = next.close(id);
    if (!taker)
        return false;
    commit(next);
    setActive(taker);
    return true;
}

bool Workspace::canShow(int id, const QString &view) const {
    return m_tree.area(id) && m_views.contains(view);
}

bool Workspace::showView(int id, const QString &view) {
    if (!canShow(id, view) || m_tree.area(id)->view == view)
        return false;
    AreaTree next = m_tree;
    const ViewRegistry::ViewType *type = m_views.find(view);
    int elsewhere = 0;
    if (type->multiplicity == ViewRegistry::Multiplicity::OnePerWindow)
        for (const int other : m_tree.areaIds())
            if (other != id && m_tree.area(other)->view == view)
                elsewhere = other;
    if (elsewhere)
        next.swap(id, elsewhere);
    else
        next.setView(id, view);
    commit(next);
    return true;
}

QVariantList Workspace::viewChoices(int id) const {
    QVariantList choices;
    const Area *area = m_tree.area(id);
    if (!area)
        return choices;
    for (const QString &type : m_views.ids()) {
        bool moves = false;
        if (m_views.find(type)->multiplicity == ViewRegistry::Multiplicity::OnePerWindow)
            for (const int other : m_tree.areaIds())
                moves = moves || (other != id && m_tree.area(other)->view == type);
        choices << QVariantMap{{"id", type},
                               {"name", m_views.name(type)},
                               {"glyph", m_views.glyph(type)},
                               {"current", area->view == type},
                               {"enabled", canShow(id, type)},
                               {"moves", moves}};
    }
    return choices;
}

QString Workspace::viewOf(int id) const {
    const Area *area = m_tree.area(id);
    return area ? area->view : QString();
}

int Workspace::areaOf(const QString &view) const {
    for (const int id : m_tree.areaIds())
        if (m_tree.area(id)->view == view)
            return id;
    return 0;
}

bool Workspace::toggleApp() {
    const int id = m_tree.area(m_active) ? m_active : fallbackArea();
    const QString view = viewOf(id);
    if (view == ViewRegistry::kApp)
        return showView(id, ViewRegistry::kGraph);
    if (view != ViewRegistry::kGraph)
        return showView(id, ViewRegistry::kApp);
    if (const int app = areaOf(ViewRegistry::kApp))
        return showView(app, ViewRegistry::kGraph);
    for (const int other : m_tree.areaIds())
        if (other != id)
            return showView(other, ViewRegistry::kApp);
    return false;
}

bool Workspace::requestApp(bool app) {
    const int id = m_tree.area(m_active) ? m_active : fallbackArea();
    if (!app)
        return viewOf(id) == ViewRegistry::kApp && showView(id, ViewRegistry::kGraph);
    if (areaOf(ViewRegistry::kApp))
        return false;
    return toggleApp();
}

bool Workspace::resizeBorder(const QVariantList &path, int border, double delta, double length) {
    AreaTree next = m_tree;
    if (!next.resize(toPath(path), border, delta, length, minimum()))
        return false;
    commit(next);
    return true;
}

bool Workspace::equalizeBorder(const QVariantList &path, int border) {
    AreaTree next = m_tree;
    if (!next.equalize(toPath(path), border))
        return false;
    commit(next);
    return true;
}

bool Workspace::setAreaState(int id, const QVariantMap &state) {
    const Area *area = m_tree.area(id);
    const QJsonObject json = QJsonObject::fromVariantMap(state);
    if (!area || area->state.value(area->view).toObject() == json)
        return false;
    AreaTree next = m_tree;
    next.setState(id, json);
    commit(next);
    return true;
}

QVariantMap Workspace::areaState(int id) const {
    const Area *area = m_tree.area(id);
    return area ? area->state.value(area->view).toObject().toVariantMap() : QVariantMap();
}

void Workspace::reset() {
    if (isDefault(m_tree) && m_tree.area(1))
        return;
    commit(AreaTree());
    setActive(1);
}

void addAreaCommands(CommandRegistry &commands) {
    struct Row {
        const char *id;
        const char *description;
        const char *label;
        QStringList keys;
    };
    const QList<Row> rows = {
        {"area.split-row", QT_TRANSLATE_NOOP("Command", "Split the area side by side"),
         QT_TRANSLATE_NOOP("Command", "Split side by side"), {"Ctrl+\\"}},
        {"area.split-column", QT_TRANSLATE_NOOP("Command", "Split the area top and bottom"),
         QT_TRANSLATE_NOOP("Command", "Split top and bottom"), {"Ctrl+Alt+\\"}},
        {"area.close", QT_TRANSLATE_NOOP("Command", "Close the area (the one beside it takes its place)"),
         QT_TRANSLATE_NOOP("Command", "Close area"), {"Ctrl+Alt+W"}},
        {"area.view-graph", QT_TRANSLATE_NOOP("Command", "Show the Graph in the area"),
         QT_TRANSLATE_NOOP("Command", "Graph"), {"Ctrl+Alt+G"}},
        {"area.view-canvas", QT_TRANSLATE_NOOP("Command", "Show a Canvas in the area"),
         QT_TRANSLATE_NOOP("Command", "Canvas"), {"Ctrl+Alt+C"}},
        {"view.reset-layout", QT_TRANSLATE_NOOP("Command", "Reset the layout to one area showing the Graph"),
         QT_TRANSLATE_NOOP("Command", "Reset layout"), {}},
    };
    for (const Row &row : rows) {
        CommandSpec spec;
        spec.id = QString::fromLatin1(row.id);
        if (commands.contains(spec.id))
            continue;
        spec.description = QString::fromUtf8(row.description);
        spec.label = QString::fromUtf8(row.label);
        spec.keys = row.keys;
        spec.versus = QStringLiteral("added");
        if (const QString problem = commands.add(spec); !problem.isEmpty())
            qCritical().noquote() << "area command:" << problem;
    }
}

void Workspace::registerCommands(CommandRegistry &commands) {
    addAreaCommands(commands);
    // Each acts on the active area, or the Graph's (else the first) when none is (the window has not said yet).
    const auto target = [this] { return m_tree.area(m_active) ? m_active : fallbackArea(); };
    for (const auto &[id, axis] : {std::pair{"area.split-row", "row"}, std::pair{"area.split-column", "column"}}) {
        const QString along = QString::fromLatin1(axis);
        commands.setHandler(QString::fromLatin1(id), [this, target, along] { splitArea(target(), along); });
        commands.setEnabledWhen(QString::fromLatin1(id), [this, target, along] { return canSplit(target(), along); });
    }
    commands.setHandler("area.close", [this, target] { closeArea(target()); });
    commands.setEnabledWhen("area.close", [this] { return m_tree.count() > 1; });
    for (const QString &view : {ViewRegistry::kGraph, ViewRegistry::kCanvas}) {
        const QString id = QStringLiteral("area.view-") + view;
        commands.setHandler(id, [this, target, view] { showView(target(), view); });
        commands.setEnabledWhen(id, [this, target, view] { return canShow(target(), view); });
        commands.setCheckedWhen(id, [this, target, view] { return viewOf(target()) == view; });
    }
    commands.setHandler("view.reset-layout", [this] { reset(); });
    commands.setEnabledWhen("view.reset-layout", [this] { return !isDefault(m_tree); });
}

} // namespace fugue
