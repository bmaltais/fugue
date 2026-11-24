#pragma once

// What a tool of the Canvas view's strip paints (#380), the model behind CanvasView's `paintTarget`: the SELECTION (what the next
// action affects: Quick Mask, the Selection Brush, the lasso) or the MASK of the selected layer (what the layer hides: the Brush and
// the Eraser). Photoshop's split: the Brush paints masks only; Quick Mask turns any brush into a selection painter while it is on.
// QML gives it the tool, the Quick Mask state and the layer the mask target is; it answers the same for a person and for a test.

#include <QObject>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

namespace fugue {

class PaintTarget : public QObject {
    Q_OBJECT
    QML_ELEMENT
    // "hand", "zoom", "move", "region", "lasso", "selbrush" (the Selection Brush), "brush", "eraser".
    Q_PROPERTY(QString tool MEMBER m_tool NOTIFY changed)
    Q_PROPERTY(bool quickMask MEMBER m_quickMask NOTIFY changed)
    // The position of the layer whose mask is painted (CanvasViewModel::maskTarget); negative: no layer mask is targeted.
    Q_PROPERTY(int maskTarget MEMBER m_maskTarget NOTIFY changed)
    // The tool paints a brush stroke (Brush, Eraser or Selection Brush).
    Q_PROPERTY(bool brushing READ brushing NOTIFY changed)
    // The Selection Brush is the tool.
    Q_PROPERTY(bool selectionBrush READ selectionBrush NOTIFY changed)
    // The stroke or shape goes into the selection / into the layer's mask.
    Q_PROPERTY(bool paintsSelection READ paintsSelection NOTIFY changed)
    Q_PROPERTY(bool paintsMask READ paintsMask NOTIFY changed)
    // "selection", "mask" or "none" (a brush with no layer mask to paint).
    Q_PROPERTY(QString target READ target NOTIFY changed)

public:
    explicit PaintTarget(QObject *parent = nullptr) : QObject(parent) {}

    bool brushing() const { return m_tool == "brush" || m_tool == "eraser" || m_tool == "selbrush"; }
    bool selectionBrush() const { return m_tool == "selbrush"; }
    // Quick Mask makes every brush paint the selection; the Selection Brush and the lasso always do.
    bool paintsSelection() const { return m_quickMask || m_tool == "selbrush" || m_tool == "lasso"; }
    // The mask is painted by the Brush and Eraser (and the lasso's overlay is the mask's while one is targeted); never while the
    // selection is being painted by a brush.
    bool paintsMask() const { return !m_quickMask && m_tool != "selbrush" && m_maskTarget >= 0; }
    QString target() const {
        if (paintsSelection())
            return QStringLiteral("selection");
        return paintsMask() ? QStringLiteral("mask") : QStringLiteral("none");
    }

signals:
    void changed();

private:
    QString m_tool = QStringLiteral("region");
    bool m_quickMask = false;
    int m_maskTarget = -1;
};

} // namespace fugue
