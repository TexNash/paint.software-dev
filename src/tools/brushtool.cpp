#include "brushtool.h"
#include "canvas/canvaswidget.h"
#include "core/document.h"
#include "core/hatchpatterns.h"
#include "core/layer.h"
#include <QPainter>
#include <cmath>

void BrushTool::mousePressEvent(const QPointF &canvasPos, QMouseEvent *event, CanvasWidget &canvas) {
    if (event->button() != Qt::LeftButton && event->button() != Qt::RightButton) return;
    auto *doc = canvas.document();
    auto *layer = doc->activeLayer();
    if (!layer || layer->isLocked()) return;

    const QColor col = (event->button() == Qt::LeftButton) ? doc->primaryColor() : doc->secondaryColor();

    // A second draw-button pressed mid-stroke switches the colour without breaking
    // the line, rather than stopping or restarting the stroke (issue #17).
    if (m_drawing) {
        if (col != m_strokeColor) switchColour(doc, layer, col);
        return;
    }

    m_drawing = true;
    m_lastPos = canvasPos;
    m_currentPos = canvasPos;
    m_beforeImage = layer->image().copy();
    m_baseImage = m_beforeImage;

    // The stroke is accumulated at full opacity in its own buffer, then
    // composited onto the layer once at the tool opacity. This keeps the whole
    // stroke at a uniform opacity instead of darkening where dabs overlap.
    m_strokeBuffer = QImage(layer->image().size(), QImage::Format_ARGB32_Premultiplied);
    m_strokeBuffer.fill(Qt::transparent);

    m_dirtyRect = QRect();
    m_segmentBounds = QRect();
    m_compositedOpacity = -1;
    m_selectionKey = -1;
    m_strokeColor = col;
    drawBrushDab(canvasPos - layer->offset(), m_strokeColor);
    canvas.updateCanvasRegion(compositeStroke(doc, layer, m_strokeColor).translated(layer->offset()));
}

void BrushTool::switchColour(Document *doc, Layer *layer, const QColor &color) {
    // Bake the coverage drawn so far into the base, then continue in the new
    // colour from the current point (no gap).
    m_baseImage = layer->image().copy();
    QPainter clear(&m_strokeBuffer);
    clear.setCompositionMode(QPainter::CompositionMode_Source);
    clear.fillRect(m_segmentBounds, Qt::transparent);
    m_segmentBounds = QRect();
    m_strokeColor = color;
}

void BrushTool::mouseMoveEvent(const QPointF &canvasPos, QMouseEvent *event, CanvasWidget &canvas) {
    m_currentPos = canvasPos;
    if (!m_drawing) return;
    auto *doc = canvas.document();
    auto *layer = doc->activeLayer();
    if (!layer) return;

    Q_UNUSED(event);
    drawBrushStroke(m_lastPos - layer->offset(), canvasPos - layer->offset(), m_strokeColor);
    m_lastPos = canvasPos;
    canvas.updateCanvasRegion(compositeStroke(doc, layer, m_strokeColor).translated(layer->offset()));
}

void BrushTool::mouseReleaseEvent(const QPointF &, QMouseEvent *event, CanvasWidget &canvas) {
    if (!m_drawing) return;
    auto *doc = canvas.document();
    auto *layer = doc ? doc->activeLayer() : nullptr;
    // Releasing one of two held buttons must not cut the stroke — keep drawing in
    // the still-held button's colour (issue #17).
    if (event->buttons() & (Qt::LeftButton | Qt::RightButton)) {
        if (layer) {
            const QColor col = (event->buttons() & Qt::LeftButton) ? doc->primaryColor()
                                                                   : doc->secondaryColor();
            if (col != m_strokeColor) switchColour(doc, layer, col);
        }
        return;
    }
    m_drawing = false;
    if (layer && layer->image() != m_beforeImage)   // skip no-op strokes (no empty undo step)
        doc->pushImageEdit(doc->activeLayerIndex(), m_beforeImage, "Paintbrush");
    m_strokeBuffer = QImage();
    m_baseImage = QImage();
}

void BrushTool::drawBrushStroke(const QPointF &from, const QPointF &to, const QColor &color) {
    double dx = to.x() - from.x();
    double dy = to.y() - from.y();
    double dist = std::sqrt(dx * dx + dy * dy);
    double spacing = std::max(1.0, m_brushSize * (m_spacing / 100.0));
    int steps = std::max(1, static_cast<int>(dist / spacing));

    for (int i = 1; i <= steps; ++i) {
        double t = (double)i / steps;
        QPointF pos(from.x() + dx * t, from.y() + dy * t);
        drawBrushDab(pos, color);
    }
}

void BrushTool::drawBrushDab(const QPointF &pos, const QColor &color) {
    QPainter painter(&m_strokeBuffer);
    if (m_antialiased) painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);

    // Draw the dab opaque; opacity is applied when the buffer is composited.
    QColor c = color;
    c.setAlphaF(1.0);

    // Paint.NET model: stylus pressure varies the dab SIZE (0..brush size), not
    // the opacity. A mouse reports full pressure, so it stamps the full size.
    const double p = m_pressureSensitivity ? m_pressure : 1.0;
    const double radius = (m_brushSize / 2.0) * p;
    if (radius < 0.4) return;   // effectively no pressure = no mark

    // Include the rasterizer's antialias fringe, including fractional centres.
    m_dirtyRect = m_dirtyRect.united(QRectF(pos.x() - radius - 2, pos.y() - radius - 2,
                                          2 * radius + 4, 2 * radius + 4).toAlignedRect()
                                    .intersected(m_strokeBuffer.rect()));

    if (m_fillStyle > 0) {
        // Patterned brush (Fill Style): stamp the hatch — pattern lines in the
        // draw colour, transparent between them — with a hard edge.
        painter.setBrush(Hatch::brush(m_fillStyle, c, QColor(Qt::transparent)));
        painter.drawEllipse(pos, radius, radius);
        return;
    }

    if (m_hardness >= 95) {
        painter.setBrush(c);
        painter.drawEllipse(pos, radius, radius);
    } else {
        QRadialGradient gradient(pos, radius);
        double hardRatio = m_hardness / 100.0;
        gradient.setColorAt(0, c);
        gradient.setColorAt(hardRatio, c);
        QColor transparent = c;
        transparent.setAlpha(0);
        gradient.setColorAt(1, transparent);
        painter.setBrush(gradient);
        painter.drawEllipse(pos, radius, radius);
    }
}

QRect BrushTool::compositeStroke(Document *doc, Layer *layer, const QColor &color) {
    m_segmentBounds = m_segmentBounds.united(m_dirtyRect);
    const qint64 selectionKey = doc->selection().mask().cacheKey() * 2 + int(doc->selectionClippingMode());
    if (selectionKey != m_selectionKey) {
        m_selectionRegion = doc->selection().region(true).translated(-layer->offset());
        if (layer->image().size() != doc->size() || !layer->offset().isNull()) {
            const QRegion visible(QRect(QPoint(), doc->size()).translated(-layer->offset()));
            m_selectionRegion = doc->selection().hasSelection() ? m_selectionRegion.intersected(visible) : visible;
        }
    }
    // Option/selection changes used to recomposite the whole colour segment.
    // Preserve that behaviour, but ordinary pointer moves only touch new damage.
    if (m_compositedOpacity != m_opacity || m_compositedBlendMode != m_blendMode
        || m_selectionKey != selectionKey)
        m_dirtyRect = m_segmentBounds;
    m_compositedOpacity = m_opacity;
    m_compositedBlendMode = m_blendMode;
    m_selectionKey = selectionKey;
    const QRect dirty = m_dirtyRect;
    m_dirtyRect = QRect();
    if (dirty.isEmpty()) return dirty;
    if (m_blendMode == 14) {
        compositeOverwrite(doc, layer, color, dirty);
        return dirty;
    }
    // Restore from the immutable segment base before applying accumulated
    // coverage once. Blending onto the previous preview would darken overlaps.
    QPainter painter(&layer->image());
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(dirty, m_baseImage, dirty);
    if (doc->selection().hasSelection() || !m_selectionRegion.isEmpty()) painter.setClipRegion(m_selectionRegion);
    painter.setOpacity((m_opacity / 100.0) * color.alphaF());
    painter.setCompositionMode(brushCompositionMode());
    painter.drawImage(dirty, m_strokeBuffer, dirty);
    painter.end();
    clipImageEdit(doc, m_baseImage, layer->image(), dirty);
    return dirty;
}

void BrushTool::compositeOverwrite(Document *doc, Layer *layer, const QColor &color, const QRect &dirty) {
    // Keep the original straight-alpha interpolation (including rounding), but
    // convert and scan only the damaged tile, not the entire layer every move.
    QImage result = m_baseImage.copy(dirty).convertToFormat(QImage::Format_ARGB32);
    const double strength = m_opacity / 100.0;
    const int tr = color.red(), tg = color.green(), tb = color.blue();
    const double ta = color.alphaF() * 255.0;
    for (int y = 0; y < result.height(); ++y) {
        const QRgb *buf = reinterpret_cast<const QRgb*>(m_strokeBuffer.constScanLine(y + dirty.y()));
        QRgb *dst = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            double cov = (qAlpha(buf[x + dirty.x()]) / 255.0) * strength;
            if (cov <= 0.0 || !imageSelectionAllows(doc, x + dirty.x(), y + dirty.y())) continue;
            const double ic = 1.0 - cov;
            QRgb d = dst[x];
            dst[x] = qRgba(
                qBound(0, int(qRed(d)   * ic + tr * cov + 0.5), 255),
                qBound(0, int(qGreen(d) * ic + tg * cov + 0.5), 255),
                qBound(0, int(qBlue(d)  * ic + tb * cov + 0.5), 255),
                qBound(0, int(qAlpha(d) * ic + ta * cov + 0.5), 255));
        }
    }
    QPainter painter(&layer->image());
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(dirty.topLeft(), result.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    painter.end();
    clipImageEdit(doc, m_baseImage, layer->image(), dirty);
}

QPainter::CompositionMode BrushTool::brushCompositionMode() const {
    // Shared mapping (Layer::allBlendModes order). 14 (Overwrite) is handled
    // separately in compositeOverwrite().
    return Tool::compositionModeFor(m_blendMode);
}

void BrushTool::drawOverlay(QPainter &painter, const CanvasWidget &canvas) {
    QPointF widgetPos = canvas.canvasToWidget(m_currentPos);
    double radius = m_brushSize / 2.0 * canvas.zoom();
    painter.setPen(QPen(Qt::black, 1, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(widgetPos, radius, radius);
}
