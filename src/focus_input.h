#pragma once

// Which input the person used last, so a focus ring shows for the keyboard only (docs/ui-ergonomics.md, a browser's
// :focus-visible). Item has no focusReason in QML (only Control has), and a ring must also stay off for the focus a dialog
// gives its default control when a mouse click opened it. This singleton watches the application's events: a key press sets
// `keyboard`, a mouse press, touch or tablet event clears it. It starts true (nothing has been pointed yet).
// A control draws its ring as `activeFocus && FocusInput.keyboard`.

#include <QEvent>
#include <QObject>
#include <QtQmlIntegration/qqmlintegration.h>

namespace fugue {

class FocusInput : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool keyboard READ keyboard NOTIFY keyboardChanged)

public:
    explicit FocusInput(QObject *parent = nullptr);

    bool keyboard() const { return m_keyboard; }

    // The same switch for code and tests without a real event.
    Q_INVOKABLE void setKeyboard(bool keyboard);

signals:
    void keyboardChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    bool m_keyboard = true;
};

} // namespace fugue
