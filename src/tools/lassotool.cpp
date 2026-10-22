#include "lassotool.h"
#include "../canvas/canvaswidget.h"
#include "../core/document.h"
#include <QPainter>
#include <QMouseEvent>
#include <QPainterPath>

void LassoTool::mousePressEvent(const QPointF &canvasPos, QMouseEvent *event, CanvasWidget &) {
    // Both mouse buttons create selections; modifiers temporarily override the toolbar mode.
    if (m_selecting) return;
    const bool left = event->button() == Qt::LeftButton;
    const bool right = event->button() == Qt::RightButton;
    if (left || right) {
        m_selecting = true;
        m_button = event->button();
        m_gestureMode = selectionModeFor(event->modifiers(), m_button, selectionMode());
        m_lasso.clear();
        m_lasso << canvasPos;
    }
}

void LassoTool::mouseMoveEvent(const QPointF &canvasPos, QMouseEvent *event, CanvasWidget &canvas) {
    if (m_selecting && (event->buttons() & (Qt::LeftButton | Qt::RightButton))) {
        m_lasso << canvasPos;
        canvas.update();
    }
}

void LassoTool::mouseReleaseEvent(const QPointF &canvasPos, QMouseEvent *event, CanvasWidget &canvas) {
    if (m_selecting && event->button() == m_button) {
        m_lasso << canvasPos;
        m_selecting = false;

        Document *doc = canvas.document();
        if (doc) {
            const Selection before = doc->selection();
            QPainterPath path;
            path.addPolygon(m_lasso);
            path.closeSubpath();
            if (m_lasso.boundingRect().width() < 1 && m_lasso.boundingRect().height() < 1)
                doc->selection().clear();
            else if (m_lasso.size() > 2)
                doc->selection().selectPath(path, m_gestureMode);
            doc->recordSelectionChange(before, "Lasso Select");
        }
        m_lasso.clear();
        canvas.update();
    }
}

void LassoTool::drawOverlay(QPainter &painter, const CanvasWidget &canvas) {
    if (!m_selecting || m_lasso.size() < 2) return;

    painter.save();
    int rs = canvas.rulerSize();
    painter.translate(rs + canvas.pan().x(), rs + canvas.pan().y());
    painter.scale(canvas.zoom(), canvas.zoom());

    QPen pen(Qt::white, 1.0 / canvas.zoom());
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPolyline(m_lasso);

    QPen dashPen(Qt::black, 1.0 / canvas.zoom(), Qt::DashLine);
    dashPen.setDashPattern({4, 4});
    painter.setPen(dashPen);
    painter.drawPolyline(m_lasso);
    painter.restore();
}
