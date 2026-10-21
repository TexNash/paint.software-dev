#include "movetool.h"
#include "canvas/canvaswidget.h"
#include "core/document.h"
#include <QPainter>
#include <QTransform>
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
    m_moving = m_resizing = m_rotating = false;
    auto command = std::move(m_sessionCommand);
    auto *doc = m_sessionDocument.data();
    m_sessionDocument.clear();
    if (!doc || !command) return;
    command->captureAfter();
    const auto *session = doc->floatingPixelSession();
    if (session && session->transform() != m_sessionStartTransform) {
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
    if (!doc || !doc->floatingPixelSession()) return;
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
    if (!doc->prepareFloatingPixelGesture()) return;
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

    if (doc->floatingPixelSession()) {
        const QRect beforeFrame = floatingFrame(doc->floatingPixelSession());
        const auto handle = SelHandles::at(beforeFrame, canvasPos, canvas.zoom());
        const bool rotation = event->button() == Qt::RightButton
            || (handle == SelHandles::Handle::None && SelHandles::inRotationZone(beforeFrame, canvasPos, canvas.zoom()));
        auto command = std::make_unique<DocumentSnapshotCommand>(doc, rotation ? "Rotate Selection"
            : (handle != SelHandles::Handle::None ? "Resize Selected Pixels" : "Move Selected Pixels"));
        const bool wasModified = doc->isModified();
        if (!doc->prepareFloatingPixelGesture(event->modifiers() & Qt::ControlModifier)) return;
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
        m_sessionGesture = m_moving = true;
        m_startPos = canvasPos;
        m_originalRect = m_previewRect = frame;
        m_rotCenter = QRectF(frame).center();
        m_angle = 0;
        m_handle = SelHandles::at(frame, canvasPos, canvas.zoom());
        m_rotating = event->button() == Qt::RightButton
            || (m_handle == SelHandles::Handle::None && SelHandles::inRotationZone(frame, canvasPos, canvas.zoom()));
        m_resizing = !m_rotating && m_handle != SelHandles::Handle::None;
        m_rotStartAngle = std::atan2(canvasPos.y() - m_rotCenter.y(), canvasPos.x() - m_rotCenter.x()) * 180.0 / M_PI;
        m_historyConnection = QObject::connect(&doc->history(), &HistoryManager::historyChanged, &canvas, [this]() {
            // Undo/history navigation has already restored the authoritative state.
            // Do not let a later mouse release write a stale preview over it.
            QObject::disconnect(m_historyConnection);
            m_sessionGesture = m_moving = m_resizing = m_rotating = false;
            m_sessionCommand.reset();
            m_sessionDocument.clear();
        });
        return;
    }

    m_moving = true;
    m_startPos = canvasPos;
    m_lastDelta = QPoint(0, 0);
    m_resizing = false;
    m_rotating = false;
    m_angle = 0.0;
    m_handle = SelHandles::Handle::None;

    if (doc->selection().hasSelection()) {
        // Move ONLY the selected pixels: lift them onto a floating copy and leave a
        // transparent hole behind (Paint.NET's "Move Selected Pixels").
        m_movingSelection = true;
        m_copyMode = (event->modifiers() & Qt::ControlModifier);   // Ctrl = move a copy
        m_originalImage = layer->image().copy();
        m_floating = doc->selection().getMaskedImage(m_originalImage, layer->offset());   // selected pixels only
        m_hole = m_originalImage.copy();
        // Leaving the hole intact keeps the originals in place, so only the copy moves.
        if (!m_copyMode) doc->selection().eraseFromImage(m_hole, layer->offset());

        m_originalRect = doc->selection().boundingRect();
        m_originalMask = doc->selection().mask().copy();
        m_rotCenter = QRectF(m_originalRect).center();

        // A handle rescales the lifted pixels; the corridor just outside the box (or
        // a right drag) rotates them; anywhere else inside moves them.
        m_handle = SelHandles::at(m_originalRect, canvasPos, canvas.zoom());
        if (m_handle != SelHandles::Handle::None) {
            m_resizing = true;
            m_previewRect = m_originalRect;
            m_floatingCrop = m_floating.copy(m_originalRect.translated(-layer->offset()));
        } else if (event->button() == Qt::RightButton
                   || SelHandles::inRotationZone(m_originalRect, canvasPos, canvas.zoom())) {
            m_rotating = true;
            m_rotStartAngle = std::atan2(canvasPos.y() - m_rotCenter.y(),
                                         canvasPos.x() - m_rotCenter.x()) * 180.0 / M_PI;
        }
    } else {
        // Preview a whole-layer move via its offset; ordinary layers bake the
        // delta on release, retained layers keep their full bitmap and offset.
        m_movingSelection = false;
        m_copyMode = false;
        m_originalOffset = layer->offset();
    }
}

void MoveTool::mouseMoveEvent(const QPointF &canvasPos, QMouseEvent *event, CanvasWidget &canvas) {
    if (m_sessionGesture) {
        auto *doc = m_sessionDocument.data();
        if (!doc || doc != canvas.document() || !doc->floatingPixelSession()) { finishSessionGesture(); return; }
        QTransform change;
        if (m_rotating) {
            m_angle = std::atan2(canvasPos.y() - m_rotCenter.y(), canvasPos.x() - m_rotCenter.x()) * 180.0 / M_PI - m_rotStartAngle;
            if (event->modifiers() & Qt::ShiftModifier) m_angle = std::round(m_angle / 15.0) * 15.0;
            change = SelHandles::rotationAbout(m_rotCenter, m_angle);
        } else if (m_resizing) {
            m_previewRect = SelHandles::resized(m_originalRect, m_handle, canvasPos, event->modifiers());
            change.translate(m_previewRect.x(), m_previewRect.y());
            change.scale(double(m_previewRect.width()) / m_originalRect.width(), double(m_previewRect.height()) / m_originalRect.height());
            change.translate(-m_originalRect.x(), -m_originalRect.y());
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

    if (m_rotating) {
        double deg = std::atan2(canvasPos.y() - m_rotCenter.y(),
                                canvasPos.x() - m_rotCenter.x()) * 180.0 / M_PI - m_rotStartAngle;
        if (event->modifiers() & Qt::ShiftModifier)
            deg = std::round(deg / 15.0) * 15.0;   // snap to 15°
        m_angle = deg;

        // Redraw the layer = hole + the lifted pixels rotated about the box centre.
        QImage result = m_hole.copy();
        {
            QPainter p(&result);
            p.setTransform(SelHandles::rotationAbout(m_rotCenter - layer->offset(), m_angle));
            p.drawImage(0, 0, m_floating);
        }
        layer->setImage(result);

        // Rotate the marquee to match (fast while dragging).
        SelHandles::setRotatedMask(doc->selection(), m_originalMask, m_rotCenter, m_angle, false);
        emit doc->selectionChanged();
        canvas.updateCanvas();
        return;
    }

    if (m_resizing) {
        m_previewRect = SelHandles::resized(m_originalRect, m_handle, canvasPos, event->modifiers());

        // Redraw the layer = hole + the lifted pixels stretched onto the new bounds.
        // Fast scaling while dragging; the release does one smooth pass.
        QImage result = m_hole.copy();
        if (!m_floatingCrop.isNull()) {
            const QImage scaled = m_floatingCrop.scaled(m_previewRect.size(), Qt::IgnoreAspectRatio,
                                                        Qt::FastTransformation);
            QPainter p(&result);
            p.drawImage(m_previewRect.topLeft() - layer->offset(), scaled);
            p.end();
        }
        layer->setImage(result);

        // Keep the marquee wrapped around the artwork as it is stretched.
        SelHandles::setScaledMask(doc->selection(), m_originalMask, m_originalRect, m_previewRect, false);
        emit doc->selectionChanged();
        canvas.updateCanvas();
        return;
    }

    // A movement delta, not an absolute position: round to nearest, not floor.
    // floor biases sub-pixel drags (a -0.001px nudge would jump a whole pixel up/left).
    const QPoint delta = (canvasPos - m_startPos).toPoint();

    if (m_movingSelection) {
        // Recompose the layer = hole + floating pixels shifted by delta.
        QImage result = m_hole.copy();
        QPainter p(&result);
        p.drawImage(delta, m_floating);
        p.end();
        layer->setImage(result);

        // Drag the marquee along with the pixels (incremental keeps it exact).
        if (delta != m_lastDelta) {
            doc->selection().translate(delta - m_lastDelta);
            m_lastDelta = delta;
            emit doc->selectionChanged();
        }
        canvas.updateCanvas();
    } else {
        layer->setOffset(m_originalOffset + delta);
        canvas.updateCanvas();
    }
}

void MoveTool::mouseReleaseEvent(const QPointF &, QMouseEvent *, CanvasWidget &canvas) {
    if (m_sessionGesture) { finishSessionGesture(); return; }
    if (!m_moving) return;
    m_moving = false;
    auto *doc = canvas.document();
    auto *layer = doc->activeLayer();
    if (!layer) return;

    if (m_rotating) {
        m_rotating = false;
        m_movingSelection = false;
        const bool changed = (std::abs(m_angle) > 1e-6);
        if (changed) {
            // Redo the rotation smoothly now the angle is final.
            QImage result = m_hole.copy();
            {
                QPainter p(&result);
                p.setRenderHint(QPainter::SmoothPixmapTransform);
                p.setTransform(SelHandles::rotationAbout(m_rotCenter - layer->offset(), m_angle));
                p.drawImage(0, 0, m_floating);
            }
            layer->setImage(result);
            SelHandles::setRotatedMask(doc->selection(), m_originalMask, m_rotCenter, m_angle, true);
            doc->pushImageEdit(doc->activeLayerIndex(), m_originalImage,
                               m_copyMode ? "Rotate Copy" : "Rotate Selection");
            emit doc->selectionChanged();
            emit doc->documentChanged();
        } else {
            layer->setImage(m_originalImage);   // no real rotation: put the pixels back
        }
        m_floating = m_hole = m_originalImage = m_floatingCrop = m_originalMask = QImage();
        m_handle = SelHandles::Handle::None;
        return;
    }

    if (m_resizing) {
        m_resizing = false;
        m_movingSelection = false;
        const bool changed = (m_previewRect != m_originalRect);
        if (changed) {
            // Redo the scale smoothly now the size is final — the drag used a fast,
            // blocky scale to stay responsive.
            QImage result = m_hole.copy();
            if (!m_floatingCrop.isNull()) {
                const QImage scaled = m_floatingCrop.scaled(m_previewRect.size(), Qt::IgnoreAspectRatio,
                                                            Qt::SmoothTransformation);
                QPainter p(&result);
                p.drawImage(m_previewRect.topLeft() - layer->offset(), scaled);
                p.end();
            }
            layer->setImage(result);
            SelHandles::setScaledMask(doc->selection(), m_originalMask, m_originalRect, m_previewRect, true);
            doc->pushImageEdit(doc->activeLayerIndex(), m_originalImage, "Resize Selected Pixels");
            emit doc->selectionChanged();
            emit doc->documentChanged();
        } else {
            layer->setImage(m_originalImage);   // no real resize: put the pixels back
        }
        m_floating = m_hole = m_originalImage = m_floatingCrop = m_originalMask = QImage();
        m_handle = SelHandles::Handle::None;
        return;
    }

    if (m_movingSelection) {
        m_movingSelection = false;
        const bool moved = !m_lastDelta.isNull();
        // The layer already holds hole + floating@delta from the last move.
        if (moved)
            doc->pushImageEdit(doc->activeLayerIndex(), m_originalImage, "Move Selection");
        m_floating = QImage();
        m_hole = QImage();
        m_originalImage = QImage();
        if (moved) emit doc->documentChanged();
        return;
    }

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
    painter.restore();
}
