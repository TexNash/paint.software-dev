#pragma once
#include "tool.h"
#include <QPolygonF>
#include <QImage>

class LassoTool : public Tool {
public:
    ToolType type() const override { return ToolType::LassoSelection; }
    QString name() const override { return "Lasso de sélection"; }
    QCursor cursor() const override { return Qt::CrossCursor; }

    void mousePressEvent(const QPointF &canvasPos, QMouseEvent *event, CanvasWidget &canvas) override;
    void mouseMoveEvent(const QPointF &canvasPos, QMouseEvent *event, CanvasWidget &canvas) override;
    void mouseReleaseEvent(const QPointF &canvasPos, QMouseEvent *event, CanvasWidget &canvas) override;
    void drawOverlay(QPainter &painter, const CanvasWidget &canvas) override;

    bool selectionGestureActive() const override { return m_selecting; }
    void cancelSelectionGesture(CanvasWidget &) override { m_selecting = false; }
    void deactivate(CanvasWidget &canvas) override { cancelSelectionGesture(canvas); }

private:
    bool m_selecting = false;
    QPolygonF m_lasso;
    // Resolve the toolbar mode and temporary modifiers when the gesture begins.
    SelectionMode m_gestureMode = SelectionMode::Replace;
    Qt::MouseButton m_button = Qt::LeftButton;   // button that started the lasso (combine mode)
};
