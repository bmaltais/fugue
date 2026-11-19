#pragma once

// Editing a node's input values in place (spec 6.2 widgets, 3.2, 2.5), the C++ half of the
// widgets in src/canvas/widgets/. The QML only draws and forwards what was typed or picked;
// everything that decides is here: which widget an input has and its constraints, turning
// typed text into a value, checking that value with the engine's own rules (checkLiteral,
// never the QML's), and writing it to the document through the canvas model's undo stack.
//
// Each commit is one undo step. A value set at once (a toggle, a choice, a chevron step, a
// picked file or model) is one CanvasModel::commit(). Typing and dragging are a session:
// begin() opens one edit on the model, preview() puts each valid value in the document as it
// is typed (so a save or a copy meanwhile has it), and finish() ends the edit, one step for
// the whole session; cancel() puts the value back and leaves no step. An invalid value is
// never written: preview() and the set calls return why, for the field to show, and leave the
// document as it was. A session ends by itself, as its own step, when any other edit begins
// (a command run while the field has focus, an undo), so the two never merge into one step;
// and without a step when the document is replaced (another tab, a file opened). Either way
// sessionEnded tells the field. A value is never written to a node or input that is gone.
//
// Seeds (6.2): "fixed" stores a number, "random" stores {"auto": "random"}, and "increment"
// stores a number and lists the input under layout.nodes.<id>.increment, so the file keeps
// the choice and a run still sees a plain number; advanceSeeds() adds one to each of them
// after a run starts (#19 calls it).
//
// Resource inputs show the model picker (2.5). Its list comes from a model source: by
// default each enabled provider instance (providers.json) is asked to describe() itself when
// a picker opens, and every model it offers of the input's resource type is listed. Models
// on disk (8.9) are not listed yet: nothing finds them until the component pack (M5). A name
// typed into the picker is stored as a handle served by name, {"resource": type, "name": …}.

#include "engine/document.h"
#include "engine/node_type.h"

#include <QColor>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

class CanvasModel;

namespace fugue {
class Registry;
}

class WidgetEdit : public QObject {
    Q_OBJECT
    // Bumped whenever a model list arrives, so the pickers re-read models().
    Q_PROPERTY(int modelsRevision READ modelsRevision NOTIFY modelsChanged)

public:
    WidgetEdit(const fugue::Registry *registry, CanvasModel *canvas, QObject *parent = nullptr);

    // What the widget of an input needs: {widget, type, label, min, max, step, integer,
    // choices, optional, default (when there is one)}. `widget` is a 6.2 name, or "model" for
    // a resource input. Empty for an input that cannot be edited here: a placeholder's (no
    // known type), a list, a media or data input.
    Q_INVOKABLE QVariantMap spec(const QString &node, const QString &input) const;
    // The stored value: a number, text or boolean, {"auto": "random"} or a resource handle as
    // a map; undefined when the input is unset (it uses its default) or linked.
    Q_INVOKABLE QVariant value(const QString &node, const QString &input) const;
    // Where a linked input's value comes from: "<node title> · <output label>".
    Q_INVOKABLE QString source(const QString &node, const QString &input) const;

    // Why this text cannot be the input's value, in one sentence; empty when it can. Numbers
    // take a dot or the language's decimal separator. Empty text unsets a number, and an
    // optional text input with no default; a required number with no default needs one.
    Q_INVOKABLE QString check(const QString &node, const QString &input, const QString &text) const;
    // Sets the input from typed text, as one undo step. Returns check()'s answer; nothing is
    // written when it is not empty.
    Q_INVOKABLE QString setText(const QString &node, const QString &input, const QString &text);
    // Sets the input to a value (a boolean, a choice, a number, a handle), one undo step.
    Q_INVOKABLE QString setValue(const QString &node, const QString &input, const QVariant &value);
    // A file or folder from the picker (a file: URL or a path), stored as a local path.
    Q_INVOKABLE QString setPath(const QString &node, const QString &input, const QVariant &url);
    // A color from the color picker, stored as #rrggbb, or #rrggbbaa when not opaque.
    Q_INVOKABLE QString setColor(const QString &node, const QString &input, const QColor &color);
    // Takes the stored value out, so the default applies (or, for an optional input, nothing).
    Q_INVOKABLE void unset(const QString &node, const QString &input);

    // --- a typing or dragging session: one undo step in all ---
    Q_INVOKABLE void begin(const QString &node, const QString &input);
    // Writes the text (previewValue: the value) when it is valid; returns why not otherwise.
    Q_INVOKABLE QString preview(const QString &text);
    Q_INVOKABLE QString previewValue(const QVariant &value);
    Q_INVOKABLE void finish();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE bool editing() const { return !m_session.node.isEmpty(); }
    // Ends the session as finish() does and tells the field (sessionEnded). The canvas model
    // calls it when another edit begins.
    void endSession();
    // True while the session writes its own value (not another edit).
    bool writing() const { return m_writing; }

    // --- numbers (spin, slider, seed) ---
    // `from` moved by `steps` of the input's step (1 for a whole number, else 0.1 when it has
    // none), held inside min and max and rounded to the step.
    Q_INVOKABLE double stepped(const QString &node, const QString &input, double from, int steps) const;
    // The value at a share (0 to 1) of a slider's range, rounded to the step; and back.
    Q_INVOKABLE double valueAt(const QString &node, const QString &input, double fraction) const;
    Q_INVOKABLE double fraction(const QString &node, const QString &input, double value) const;
    // How the number shows in a field: whole numbers without a fraction.
    Q_INVOKABLE static QString numberText(double value);

    // --- seeds ---
    // "fixed", "random" or "increment".
    Q_INVOKABLE QString seedMode(const QString &node, const QString &input) const;
    // One undo step. From random to fixed or increment, a new number is drawn.
    Q_INVOKABLE void setSeedMode(const QString &node, const QString &input, const QString &mode);
    // Every incrementing seed gains one (back to its minimum past the maximum), one undo step.
    Q_INVOKABLE void advanceSeeds();

    // --- the model picker ---
    // One model: {name, handle, status ("ready", "loading", "unavailable"), provider}.
    struct Model {
        QString name;
        QJsonObject handle;
        QString status;
        QString provider;
    };
    using ModelSource = std::function<void(const QString &resourceType, std::function<void(QList<Model>)> done)>;
    // Replaces where models come from (tests pass a fixed list).
    void setModelSource(ModelSource source) { m_source = std::move(source); }
    // Asks the source for the models of the input's resource type; modelsChanged when they come.
    Q_INVOKABLE void requestModels(const QString &node, const QString &input);
    // The models last listed for the input's type, as maps; empty before any arrive.
    Q_INVOKABLE QVariantList models(const QString &node, const QString &input) const;
    Q_INVOKABLE bool modelsLoading(const QString &node, const QString &input) const;
    int modelsRevision() const { return m_modelsRevision; }
    // Picks a listed model (its handle as a map), one undo step.
    Q_INVOKABLE QString setModel(const QString &node, const QString &input, const QVariantMap &handle);
    // A model served by this name, as typed.
    Q_INVOKABLE QString setModelName(const QString &node, const QString &input, const QString &name);

signals:
    void modelsChanged();
    // A session ended without finish() or cancel(): another edit began, or the document was
    // replaced. The field closes.
    void sessionEnded();

private:
    struct Target {
        const fugue::InputSpec *spec = nullptr;
        fugue::PortKind kind = fugue::PortKind::Unknown;
        QString widget;
    };
    // The input's spec when it can be edited here.
    Target target(const QString &node, const QString &input) const;
    // Parses typed text: the value, `unset` for empty text that unsets, or why not.
    QString parse(const Target &target, const QString &text, QJsonValue *value, bool *unsetIt) const;
    QString checkValue(const Target &target, const QJsonValue &value) const;
    // Writes the value (undefined: unset) as one step, or into the open session.
    void write(const QString &node, const QString &input, const QJsonValue &value, const QString &text);
    QString stepText(const QString &node, const QString &input) const;
    double randomSeed(const fugue::InputSpec &spec) const;

    const fugue::Registry *m_registry;
    CanvasModel *m_canvas;
    struct Session {
        QString node, input;
        fugue::InputValue before;
        bool open = false; // holds an edit open on the model
    } m_session;
    bool m_writing = false;
    ModelSource m_source;
    QHash<QString, QList<Model>> m_models; // by resource type
    QSet<QString> m_loading;
    int m_modelsRevision = 0;
};
