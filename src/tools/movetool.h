#pragma once
#include "tool.h"
#include "selectionhandles.h"
#include "core/floatingpixelsession.h"
#include <QRect>
#include <QPointer>
#include <memory>

class DocumentSnapshotCommand;

class MoveTool : public Tool {
public:
    MoveTool();
    ~MoveTool() override;
    ToolType type() const override { return ToolType::Move; }
    QString name() const override { return "Déplacer les pixels sélectionnés"; }
    QCursor cursor() const override { return Qt::SizeAllCursor; }

    void mousePressEvent(const QPointF &canvasPos, QMouseEvent *event, CanvasWidget &canvas) override;
    void mouseMoveEvent(const QPointF &canvasPos, QMouseEvent *event, CanvasWidget &canvas) override;
    void mouseReleaseEvent(const QPointF &canvasPos, QMouseEvent *event, CanvasWidget &canvas) override;
    void drawOverlay(QPainter &painter, const CanvasWidget &canvas) override;
    void keyPressEvent(QKeyEvent *event, CanvasWidget &canvas) override;
    void deactivate(CanvasWidget &canvas) override;
    FloatingPixelSession::Sampling sampling() const { return m_sampling; }
    void setSampling(FloatingPixelSession::Sampling sampling) {
        if (FloatingPixelSession::validSampling(sampling)) m_sampling = sampling;
    }

private:
    void finishSessionGesture();
    FloatingPixelSession::Sampling m_sampling = FloatingPixelSession::Sampling::Bicubic;
    bool m_sessionGesture = false;
    bool m_pivotGesture = false;
    bool m_sessionWasModified = false;
    QPointer<Document> m_sessionDocument;
    QTransform m_sessionStartTransform;
    QPointF m_sessionStartPivot;
    std::unique_ptr<DocumentSnapshotCommand> m_sessionCommand;
    QMetaObject::Connection m_historyConnection;
    bool m_moving = false;
    QPointF m_startPos;
    QPoint m_originalOffset;

    // Dragging a handle rescales the artwork itself, not just the marquee: the
    // selected pixels are stretched or squashed onto the new bounds.
    bool m_resizing = false;
    SelHandles::Handle m_handle = SelHandles::Handle::None;
    QRect m_originalRect;     // selection bounds at press
    QRect m_previewRect;      // bounds being dragged to

    // Dragging just outside the box (or with the right button) rotates the lifted
    // pixels and the marquee about the session pivot. Shift snaps to 15°.
    bool m_rotating = false;
    QPointF m_rotCenter;      // rotation pivot at press, canvas space
    double m_rotStartAngle = 0.0;  // pointer angle at press, degrees
    double m_angle = 0.0;     // rotation applied so far, degrees

};
