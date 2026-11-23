#include "canvas/active_canvas.h"

#include <QMutexLocker>

namespace fugue {

QString ActiveCanvas::id() const {
    const QMutexLocker lock(&m_lock);
    return m_id;
}

void ActiveCanvas::use(const QString &canvasId) {
    if (canvasId.isEmpty())
        return;
    {
        const QMutexLocker lock(&m_lock);
        if (m_id == canvasId)
            return;
        m_id = canvasId;
    }
    emit changed();
}

} // namespace fugue
