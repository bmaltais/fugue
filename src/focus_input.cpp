#include "focus_input.h"

#include <QCoreApplication>
#include <QKeyEvent>

namespace fugue {

FocusInput::FocusInput(QObject *parent) : QObject(parent) {
    if (QCoreApplication::instance())
        QCoreApplication::instance()->installEventFilter(this);
}

void FocusInput::setKeyboard(bool keyboard) {
    if (m_keyboard == keyboard)
        return;
    m_keyboard = keyboard;
    emit keyboardChanged();
}

bool FocusInput::eventFilter(QObject *watched, QEvent *event) {
    switch (event->type()) {
    case QEvent::KeyPress: {
        // A bare modifier is not navigation; Tab, arrows, Space and the rest are.
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key != Qt::Key_Shift && key != Qt::Key_Control && key != Qt::Key_Alt && key != Qt::Key_Meta)
            setKeyboard(true);
        break;
    }
    case QEvent::MouseButtonPress:
    case QEvent::TouchBegin:
    case QEvent::TabletPress:
        setKeyboard(false);
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

} // namespace fugue
