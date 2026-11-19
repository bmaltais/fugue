#pragma once

// The command registry and the keymap (spec 12.4, 12.5). Every action of the window is a
// command with a string id (`file.save`). The menus, the `?` overlay and the settings panel
// read this one registry, and later tickets only register their commands and set their
// handlers. A command with no handler yet is listed but marked not available, and its keys
// are not taken from anything else.
//
// The user's keymap is a diff against the defaults, read from keybindings.json: "add" binds
// a key to a command and "unset" removes a default key. A file that cannot be used never
// stops the app: the defaults stay and the reason is listed in problems(), which the window
// shows.

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <functional>

namespace fugue {

struct CommandSpec {
    QString id;          // `group.name`, lowercase letters, digits and `-`
    QString description; // English; translated when listed
    QStringList keys;    // default key bindings, as QKeySequence portable text ("Ctrl+S")
    QStringList gestures; // bindings that are not keys, shown but not rebindable ("Alt+drag")
    // Where the keys apply: app (everywhere), canvas, group-selected, group (inside one) or
    // run (a run is in progress). The more specific context owns a key (12.4).
    QString context = QStringLiteral("app");
    // How it compares to ComfyUI (12.4): "same", or what differs ("added", "Omarchy").
    QString versus = QStringLiteral("same");

    bool deviates() const { return versus != QStringLiteral("same"); }
};

class CommandRegistry : public QObject {
    Q_OBJECT
    // The keys that have an owner right now. The window binds one shortcut to each.
    Q_PROPERTY(QStringList activeKeys READ activeKeys NOTIFY changed)
    // True while a text field has focus: it owns bare-letter and Shift-only keys (12.4).
    Q_PROPERTY(bool textFieldFocus READ textFieldFocus WRITE setTextFieldFocus NOTIFY changed)
    Q_PROPERTY(bool unsavedWork READ unsavedWork WRITE setUnsavedWork NOTIFY changed)
    Q_PROPERTY(bool workflowOpen READ workflowOpen WRITE setWorkflowOpen NOTIFY changed)
    // What was wrong with the keymap file and the keymap it holds, one line each.
    Q_PROPERTY(QStringList problems READ problems NOTIFY changed)
    // Where the keymap file is (the last one loaded, else the default), with the home folder
    // as ~, for the `?` overlay's footer.
    Q_PROPERTY(QString keymapFile READ keymapFile NOTIFY changed)

public:
    using Handler = std::function<void()>;

    explicit CommandRegistry(QObject *parent = nullptr);

    // Registers a command, with its handler when it has one. Returns what is wrong with the
    // spec (a bad or repeated id, an unknown context, a key that is not one chord), empty
    // when it was registered; a spec with a problem is not registered.
    [[nodiscard]] QString add(const CommandSpec &spec, Handler handler = {});
    // Sets or (with an empty handler) removes the handler of a registered command. False
    // when the id is not registered.
    bool setHandler(const QString &id, Handler handler);

    bool contains(const QString &id) const { return m_index.contains(id); }
    QStringList ids() const;
    const CommandSpec *spec(const QString &id) const;
    bool hasHandler(const QString &id) const;
    // The keys now bound to the command: its defaults, less the user's unsets, plus the
    // user's adds. Portable text, as QKeySequence prints it.
    QStringList keys(const QString &id) const;

    // Runs the handler of the command, whatever its context. False when there is none.
    bool run(const QString &id);
    // The command that owns the key in the active contexts, when it has a handler; empty
    // when none does. Among commands bound to one key, the most specific context wins, and
    // the user's own binding wins over a default in the same context.
    QString ownerOf(const QString &key) const;
    // Runs the owner of the key. False when nothing owns it.
    Q_INVOKABLE bool triggerKey(const QString &key);

    // `app` is always active. A ticket that owns a context turns it on and off.
    void setContextActive(const QString &context, bool active);
    bool contextActive(const QString &context) const;
    static QStringList contexts();

    // While a text field has focus, bare-letter and Shift-only keys and Ctrl+A, C, V, X, Z, Y
    // belong to the field, and so does Backspace, Delete, Tab and Enter. F-keys, Escape and
    // other Ctrl or Alt chords stay with the commands (12.4).
    Q_INVOKABLE static bool belongsToTextField(const QString &key);
    // True when the object is a text input (a TextField, a TextArea or an editable control).
    Q_INVOKABLE static bool isTextInput(QObject *object);

    // Applies a keymap diff (the content of keybindings.json); `source` names it in
    // messages. Whatever cannot be used is skipped and listed in problems().
    void applyKeymap(const QByteArray &json, const QString &source = QStringLiteral("keybindings.json"));
    // Reads the file (missing is fine: the defaults stay). A file that is too big, cannot be
    // read or is not valid JSON is a problem, never a crash.
    void loadKeymap(const QString &path);
    // ~/.config/fugue/keybindings.json
    static QString keymapPath();
    QString keymapFile() const;

    // Every command, in the order registered (the keymap table's, spec 12.5), as maps for QML: id, description (translated), keys,
    // gestures, context, versus, deviates, available.
    Q_INVOKABLE QVariantList list() const;

    QStringList activeKeys() const;
    bool textFieldFocus() const { return m_textFieldFocus; }
    void setTextFieldFocus(bool focus);
    bool unsavedWork() const { return m_unsavedWork; }
    void setUnsavedWork(bool unsaved);
    bool workflowOpen() const { return m_workflowOpen; }
    void setWorkflowOpen(bool open);
    QStringList problems() const;

    // The app-level commands. Quit asks first when there is unsaved work; close closes the
    // open workflow, or quits the same way when none is open.
    Q_INVOKABLE void quitNow();
    void requestQuit();
    void requestClose();

signals:
    void changed();
    // The `?` command: show the overlay, or hide it when it is shown.
    void shortcutsToggleRequested();
    // Quit was asked while there is unsaved work: the window asks first, then calls quitNow().
    void quitConfirmationRequested();
    // Close was asked with a workflow open: the window (or the workflows panel) closes it.
    void closeWorkflowRequested();

private:
    struct Entry {
        CommandSpec spec;
        Handler handler;
        QStringList keys; // effective, canonical
        QStringList userKeys; // the subset the user added
    };
    struct Edit {
        QString command, key;
    };

    static QString canonicalKey(const QString &key, QString *problem = nullptr);
    void rebuild();

    QList<Entry> m_entries;
    QHash<QString, int> m_index;
    QSet<QString> m_activeContexts;
    QList<Edit> m_adds, m_unsets;
    QStringList m_fileProblems, m_keymapProblems;
    QString m_keymapFile;
    bool m_textFieldFocus = false;
    bool m_unsavedWork = false;
    bool m_workflowOpen = false;
};

// Registers every command of the default keymap (12.4) with its id, keys and context, and
// the handlers of the app-level ones: app.quit, file.close and app.shortcuts. The rest
// have no handler until the ticket that builds them sets one.
void registerDefaultCommands(CommandRegistry &registry);

} // namespace fugue
