#include "helper_notice.h"

#include <QClipboard>
#include <QGuiApplication>

namespace fugue {

HelperNotice::HelperNotice(QObject *parent) : QObject(parent), m_shown(CudaHelperNotice::due()) {}

void HelperNotice::copy() { QGuiApplication::clipboard()->setText(command()); }

void HelperNotice::dismiss() {
    CudaHelperNotice::dismiss();
    m_shown = false;
    emit shownChanged();
}

} // namespace fugue
