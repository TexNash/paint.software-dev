#include "movetool.h"
#include "canvas/canvaswidget.h"
#include "core/document.h"
#include <QPainter>
#include <QTransform>
#include <QLineF>
#include <cmath>
#include <limits>

MoveTool::MoveTool() = default;
MoveTool::~MoveTool() { QObject::disconnect(m_historyConnection); }

static QRect floatingFrame(const FloatingPixelSession *session) {
    const QRectF bounds = session->sourceBounds();
    const double left = std::floor(bounds.left()), top = std::floor(bounds.top());
    const double right = std::ceil(bounds.right()), bottom = std::ceil(bounds.bottom());
    if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) || !std::isfinite(bottom)
        || left < std::numeric_limits<int>::min() || top < std::numeric_limits<int>::min()
        || right > std::numeric_limits<int>::max() || bottom > std::numeric_limits<int>::max()
        || right <= left || bottom <= top || right - left > std::numeric_limits<int>::max()
        || bottom - top > std::numeric_limits<int>::max()) return {};
    return QRect(int(left), int(top), int(right - left), int(bottom - top));
}

void MoveTool::finishSessionGesture() {
    if (!m_sessionGesture) return;
    QObject::disconnect(m_historyConnection);
    m_sessionGesture = false;
    m_moving = m_resizing = m_rotating = m_pivotGesture = false;
    auto command = std::move(m_sessionCommand);
    auto *doc = m_sessionDocument.data();
    m_sessionDocument.clear();
    if (!doc || !command) return;
    command->captureAfter();
    const auto *session = doc->floatingPixelSession();
    if (session && (session->transform() != m_sessionStartTransform
                    || session->rotationPivot() != m_sessionStartPivot)) {
        doc->history().push(std::move(command));
        doc->setModified(true);
        // The final preview already invalidated its footprints. Closing a
        // gesture changes history/overlay state, not the entire layer raster.
        emit doc->floatingPixelsChanged(QRegion());
    } else {
        // A click is not a lift/duplicate or a new history branch.
        command->undo();
        doc->setModified(m_sessionWasModified);
    }
}

void MoveTool::deactivate(CanvasWidget &canvas) {
    canvas.flushFloatingMove();
    if (m_sessionGesture) finishSessionGesture();
    else if (m_moving && canvas.document()) mouseReleaseEvent(QPointF(), nullptr, canvas);
}

void MoveTool::keyPressEvent(QKeyEvent *event, CanvasWidget &canvas) {
    auto *doc = canvas.document();
    if (!doc || (!doc->floatingPixelSession() && !doc->selection().hasSelection())) return;
    QPoint delta;
    switch (event->key()) {
    case Qt::Key_Left: delta = QPoint(-1, 0); break;
    case Qt::Key_Right: delta = QPoint(1, 0); break;
    case Qt::Key_Up: delta = QPoint(0, -1); break;
    case Qt::Key_Down: delta = QPoint(0, 1); break;
    default: return;
    }
    finishSessionGesture();
    if (event->modifiers() & Qt::ControlModifier) delta *= 10;
    const bool wasModified = doc->isModified();
    auto command = std::make_unique<DocumentSnapshotCommand>(doc, "Move Selected Pixels");
    if (!doc->prepareFloatingPixelGesture(false,m_sampling)) return;
    const QTransform moved = doc->floatingPixelSession()->transform() * QTransform::fromTranslate(delta.x(), delta.y());
    if (!doc->setFloatingPixelTransform(moved)) {
        command->captureAfter(); command->undo(); doc->setModified(wasModified);
        return;
    }
    command->captureAfter();
    doc->history().push(std::move(command));
    doc->setModified(true);
    event->accept();
}

void MoveTool::mousePressEvent(const QPointF &canvasPos, QMouseEvent *event, CanvasWidget &canvas) {
    finishSessionGesture();
    // Left drags move/resize; a right drag rotates the lifted content (Paint.NET).
    if (event->button() != Qt::LeftButton && event->button() != Qt::RightButton) return;
    auto *doc = canvas.document();
    auto *layer = doc->activeLayer();
    if (!layer || layer->isLocked()) return;

    if (doc->floatingPixelSession() || doc->selection().hasSelection()) {
        const QRect beforeFrame = doc->floatingPixelSession()
            ? floatingFrame(doc->floatingPixelSession()) : doc->selection().boundingRect();
        const auto handle = SelHandles::at(beforeFrame, canvasPos, canvas.zoom());
        const QPointF pivot = doc->floatingPixelSession() ? doc->floatingPixelSession()->rotationPivot()
                                                         : QRectF(beforeFrame).center();
        const bool pivotDrag = event->button() == Qt::LeftButton && handle == SelHandles::Handle::None
            && QLineF(canvasPos, pivot).length() <= SelHandles::pivotRadius(beforeFrame, canvas.zoom());
        const bool rotation = event->button() == Qt::RightButton
            || (handle == SelHandles::Handle::None && SelHandles::inRotationZone(beforeFrame, canvasPos, canvas.zoom()));
        auto command = std::make_unique<DocumentSnapshotCommand>(doc, pivotDrag ? "Move Rotation Pivot" : rotation ? "Rotate Selection"
            : (handle != SelHandles::Handle::None ? "Resize Selected Pixels" : "Move Selected Pixels"));
        const bool wasModified = doc->isModified();
        if (!doc->prepareFloatingPixelGesture(!pivotDrag && (event->modifiers() & Qt::ControlModifier),m_sampling)) return;
        const auto *session = doc->floatingPixelSession();
        const QRect frame = floatingFrame(session);
        if (frame.isEmpty()) {
            command->captureAfter(); command->undo(); doc->setModified(wasModified);
            return;
        }
        m_sessionCommand = std::move(command);
        m_sessionDocument = doc;
        m_sessionWasModified = wasModified;
        m_sessionStartTransform = session->transform();
        m_sessionStartPivot = session->rotationPivot();
        m_pivotGesture = pivotDrag;
        m_sessionGesture = m_moving = true;
        m_startPos = canvasPos;
        m_originalRect = m_previewRect = frame;
        m_rotCenter = session->rotationPivot();
        m_angle = 0;
        m_handle = SelHandles::at(frame, canvasPos, canvas.zoom());
        m_rotating = !pivotDrag && (event->button() == Qt::RightButton
            || (m_handle == SelHandles::Handle::None && SelHandles::inRotationZone(frame, canvasPos, canvas.zoom())));
        m_resizing = !pivotDrag && !m_rotating && m_handle != SelHandles::Handle::None;
        m_rotStartAngle = std::atan2(canvasPos.y() - m_rotCenter.y(), canvasPos.x() - m_rotCenter.x()) * 180.0 / M_PI;
        m_historyConnection = QObject::connect(&doc->history(), &HistoryManager::historyChanged, &canvas, [this]() {
            // Undo/history navigation has already restored the authoritative state.
            // Do not let a later mouse release write a stale preview over it.
            QObject::disconnect(m_historyConnection);
            m_sessionGesture = m_moving = m_resizing = m_rotating = m_pivotGesture = false;
            m_sessionCommand.reset();
            m_sessionDocument.clear();
        });
        return;
    }

    m_moving = true;
    m_startPos = canvasPos;
    // Whole-layer movement remains independent of selected-pixel sessions.
    m_resizing = m_rotating = false;
    m_originalOffset = layer->offset();
}

void MoveTool::mouseMoveEvent(const QPointF &canvasPos, QMouseEvent *event, CanvasWidget &canvas) {
    if (m_sessionGesture) {
        auto *doc = m_sessionDocument.data();
        if (!doc || doc != canvas.document() || !doc->floatingPixelSession()) { finishSessionGesture(); return; }
        if (m_pivotGesture) {
            doc->setFloatingRotationPivot(m_sessionStartPivot + canvasPos - m_startPos);
            canvas.update();
            return;
        }
        QTransform change;
        if (m_rotating) {
            m_angle = std::atan2(canvasPos.y() - m_rotCenter.y(), canvasPos.x() - m_rotCenter.x()) * 180.0 / M_PI - m_rotStartAngle;
            if (event->modifiers() & Qt::ShiftModifier) m_angle = std::round(m_angle / 15.0) * 15.0;
            change = SelHandles::rotationAbout(m_rotCenter, m_angle);
        } else if (m_resizing) {
            if (canvasPos != m_startPos)
                change = SelHandles::resizeTransform(m_originalRect, m_handle, canvasPos, event->modifiers());
            m_previewRect = change.mapRect(QRectF(m_originalRect)).toAlignedRect();
        } else {
            const QPoint delta = (canvasPos - m_startPos).toPoint();
            change.translate(delta.x(), delta.y());
        }
        doc->setFloatingPixelTransform(m_sessionStartTransform * change);
        return;
    }
    if (!m_moving) return;
    auto *doc = canvas.document();
    auto *layer = doc->activeLayer();
    if (!layer) return;

    const QPoint delta = (canvasPos - m_startPos).toPoint();
    layer->setOffset(m_originalOffset + delta);
    canvas.updateCanvas();
}

void MoveTool::mouseReleaseEvent(const QPointF &, QMouseEvent *, CanvasWidget &canvas) {
    if (m_sessionGesture) { finishSessionGesture(); return; }
    if (!m_moving) return;
    m_moving = false;
    auto *doc = canvas.document();
    auto *layer = doc->activeLayer();
    if (!layer) return;

    // Ordinary whole-layer moves bake the delta into the bitmap and restore its
    // original origin. Explicit retained state is the only nondestructive path.
    const QPoint finalOffset = layer->offset();
    if (finalOffset == m_originalOffset) return;   // no real move

    // Keep the full bitmap when the layer has content outside the canvas. Baking
    // into a fixed buffer here would permanently discard it on the first drag.
    if (layer->retainsOffCanvasPixels()) {
        doc->history().push(std::make_unique<LayerOffsetCommand>(doc, doc->activeLayerIndex(),
                                                               m_originalOffset, finalOffset));
        doc->setModified(true);
        emit doc->documentChanged();
        return;
    }
    QImage before = layer->image().copy();
    QImage moved(before.size(), QImage::Format_ARGB32_Premultiplied);
    moved.fill(Qt::transparent);
    {
        QPainter p(&moved);
        p.drawImage(finalOffset - m_originalOffset, before);
    }
    layer->setOffset(m_originalOffset);
    layer->setImage(moved);
    doc->pushImageEdit(doc->activeLayerIndex(), before, "Move Layer");
    emit doc->documentChanged();
}

void MoveTool::drawOverlay(QPainter &painter, const CanvasWidget &canvas) {
    auto *doc = canvas.document();
    if (!doc || (!doc->selection().hasSelection() && !doc->floatingPixelSession())) return;

    if (m_rotating) {
        // Draw the original box rotated about its centre, so the pivot and current
        // angle are visible while dragging.
        if (m_originalRect.isEmpty()) return;
        const QTransform rot = SelHandles::rotationAbout(m_rotCenter, m_angle);
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        QPolygonF box;
        for (const QPointF &c : { QPointF(m_originalRect.topLeft()), QPointF(m_originalRect.topRight()),
                                  QPointF(m_originalRect.bottomRight()), QPointF(m_originalRect.bottomLeft()) })
            box << canvas.canvasToWidget(rot.map(c));
        painter.setPen(QPen(QColor(35, 120, 255), 1, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawPolygon(box);
        // Pivot dot.
        painter.setPen(QPen(Qt::white, 1));
        painter.setBrush(QColor(35, 120, 255));
        painter.drawEllipse(canvas.canvasToWidget(m_rotCenter), 4, 4);
        painter.restore();
        return;
    }

    const QRect rect = m_resizing ? m_previewRect : (doc->floatingPixelSession()
        ? floatingFrame(doc->floatingPixelSession()) : doc->selection().boundingRect());
    if (rect.isEmpty()) return;

    // Show the same handles as Move Selection, so it is visible that a corner can
    // be grabbed to stretch the artwork — without them the tool looked move-only.
    painter.save();
    painter.setPen(QPen(Qt::white, 1));
    painter.setBrush(QColor(35, 120, 255));
    for (const QRectF &handle : SelHandles::rects(rect, canvas.zoom())) {
        const QPointF tl = canvas.canvasToWidget(handle.topLeft());
        const QPointF br = canvas.canvasToWidget(handle.bottomRight());
        painter.drawRect(QRectF(tl, br).normalized());
    }
    const QPointF pivot = canvas.canvasToWidget(doc->floatingPixelSession()
        ? doc->floatingPixelSession()->rotationPivot() : QRectF(rect).center());
    painter.setBrush(Qt::NoBrush);
    const double radius = SelHandles::pivotRadius(rect, canvas.zoom()) * canvas.zoom();
    painter.drawEllipse(pivot, radius, radius);
    painter.drawLine(pivot + QPointF(-radius*0.6,0), pivot + QPointF(radius*0.6,0));
    painter.drawLine(pivot + QPointF(0,-radius*0.6), pivot + QPointF(0,radius*0.6));
    painter.restore();
}
