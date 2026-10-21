#include "selectionhandles.h"
#include "../core/selection.h"

#include <QPainter>
#include <algorithm>
#include <cmath>

namespace SelHandles {

double radius(const QRect &rect, double zoom) {
    const double screenRadius = 4.0;
    return std::min({screenRadius / std::max(zoom, 0.01),
                     rect.width() / 4.0,
                     rect.height() / 4.0});
}

// The eight anchors, in the same order as the Handle enum values below.
double pivotRadius(const QRect &rect, double zoom) {
    return std::min({5.0 / std::max(zoom, 0.01), rect.width() / 8.0, rect.height() / 8.0});
}

static QVector<QPointF> anchors(const QRect &rect) {
    return {
        rect.topLeft(),
        QPointF(rect.center().x(), rect.top()),
        rect.topRight(),
        QPointF(rect.right(), rect.center().y()),
        rect.bottomRight(),
        QPointF(rect.center().x(), rect.bottom()),
        rect.bottomLeft(),
        QPointF(rect.left(), rect.center().y())
    };
}

Handle at(const QRect &rect, const QPointF &canvasPos, double zoom) {
    if (rect.isEmpty()) return Handle::None;

    const double r = radius(rect, zoom);
    static const Handle order[] = {
        Handle::TopLeft, Handle::Top, Handle::TopRight, Handle::Right,
        Handle::BottomRight, Handle::Bottom, Handle::BottomLeft, Handle::Left
    };

    const QVector<QPointF> pts = anchors(rect);
    for (int i = 0; i < pts.size(); ++i) {
        const QRectF hit(pts[i].x() - r, pts[i].y() - r, r * 2.0, r * 2.0);
        if (hit.contains(canvasPos)) return order[i];
    }
    return Handle::None;
}

QRect resized(const QRect &rect, Handle handle, const QPointF &canvasPos) {
    QRect result = rect;
    const QPoint pos = canvasPos.toPoint();

    switch (handle) {
    case Handle::TopLeft:     result.setTopLeft(pos); break;
    case Handle::Top:         result.setTop(pos.y()); break;
    case Handle::TopRight:    result.setTopRight(pos); break;
    case Handle::Right:       result.setRight(pos.x()); break;
    case Handle::BottomRight: result.setBottomRight(pos); break;
    case Handle::Bottom:      result.setBottom(pos.y()); break;
    case Handle::BottomLeft:  result.setBottomLeft(pos); break;
    case Handle::Left:        result.setLeft(pos.x()); break;
    case Handle::None:        break;
    }

    QRect normalized = result.normalized();
    if (normalized.width() < 2) normalized.setWidth(2);
    if (normalized.height() < 2) normalized.setHeight(2);
    return normalized;
}

QRect resized(const QRect &rect, Handle handle, const QPointF &canvasPos,
              Qt::KeyboardModifiers mods) {
    // No modifier: keep the exact original path (edge-anchored setTop/setRight…).
    if (!(mods & (Qt::ShiftModifier | Qt::AltModifier)) || handle == Handle::None)
        return resized(rect, handle, canvasPos);

    // Which edges the handle moves, per axis.
    int sx = 0, sy = 0;
    switch (handle) {
    case Handle::TopLeft:     sx = -1; sy = -1; break;
    case Handle::Top:         sx =  0; sy = -1; break;
    case Handle::TopRight:    sx =  1; sy = -1; break;
    case Handle::Right:       sx =  1; sy =  0; break;
    case Handle::BottomRight: sx =  1; sy =  1; break;
    case Handle::Bottom:      sx =  0; sy =  1; break;
    case Handle::BottomLeft:  sx = -1; sy =  1; break;
    case Handle::Left:        sx = -1; sy =  0; break;
    default: return resized(rect, handle, canvasPos);
    }

    const QRectF r(rect);
    const double oldW = r.width(), oldH = r.height();
    const QPointF centre = r.center();
    const double minSize = 2.0;

    double newW = oldW, newH = oldH;
    QPointF newCentre = centre;

    if (mods & Qt::AltModifier) {
        // Symmetric about the centre: half-extents follow the mouse.
        if (sx) newW = 2.0 * std::abs(canvasPos.x() - centre.x());
        if (sy) newH = 2.0 * std::abs(canvasPos.y() - centre.y());
        if ((mods & Qt::ShiftModifier) && sx && sy) {   // keep aspect (corners only)
            const double f = std::max(newW / std::max(oldW, minSize),
                                      newH / std::max(oldH, minSize));
            newW = oldW * f;
            newH = oldH * f;
        }
    } else {
        // Anchor the opposite edge/corner; the dragged handle follows the mouse.
        const QPointF anchor(centre.x() - sx * oldW / 2.0, centre.y() - sy * oldH / 2.0);
        if (sx) newW = std::abs(canvasPos.x() - anchor.x());
        if (sy) newH = std::abs(canvasPos.y() - anchor.y());
        if ((mods & Qt::ShiftModifier) && sx && sy) {   // keep aspect (corners only)
            const double f = std::max(newW / std::max(oldW, minSize),
                                      newH / std::max(oldH, minSize));
            newW = oldW * f;
            newH = oldH * f;
        }
        newW = std::max(newW, minSize);
        newH = std::max(newH, minSize);
        newCentre = QPointF(anchor.x() + sx * newW / 2.0, anchor.y() + sy * newH / 2.0);
    }

    newW = std::max(newW, minSize);
    newH = std::max(newH, minSize);
    QRect out = QRectF(newCentre.x() - newW / 2.0, newCentre.y() - newH / 2.0,
                       newW, newH).toRect();
    if (out.width() < 2) out.setWidth(2);
    if (out.height() < 2) out.setHeight(2);
    return out;
}

QTransform resizeTransform(const QRect &rect, Handle handle, const QPointF &pos,
                           Qt::KeyboardModifiers mods) {
    if (rect.isEmpty()) return {};
    int sx = 0, sy = 0;
    switch (handle) {
    case Handle::TopLeft: sx = -1; sy = -1; break;
    case Handle::Top: sy = -1; break;
    case Handle::TopRight: sx = 1; sy = -1; break;
    case Handle::Right: sx = 1; break;
    case Handle::BottomRight: sx = 1; sy = 1; break;
    case Handle::Bottom: sy = 1; break;
    case Handle::BottomLeft: sx = -1; sy = 1; break;
    case Handle::Left: sx = -1; break;
    default: return {};
    }
    const QRectF r(rect);
    const bool symmetric = mods & Qt::AltModifier;
    const QPointF anchor(symmetric || !sx ? r.center().x() : (sx > 0 ? r.left() : r.right()),
                         symmetric || !sy ? r.center().y() : (sy > 0 ? r.top() : r.bottom()));
    // Unmodified handles use QRect's inclusive right/bottom pixel coordinates.
    const bool inclusive = !(mods & (Qt::AltModifier | Qt::ShiftModifier));
    double x = sx ? (pos.x() + (inclusive && sx > 0 ? 1 : 0) - anchor.x())
                         / (sx * r.width() / (symmetric ? 2 : 1)) : 1;
    double y = sy ? (pos.y() + (inclusive && sy > 0 ? 1 : 0) - anchor.y())
                         / (sy * r.height() / (symmetric ? 2 : 1)) : 1;
    if ((mods & Qt::ShiftModifier) && sx && sy) {
        const double magnitude = std::max({std::abs(x), std::abs(y), 2.0/r.width(), 2.0/r.height()});
        x = std::copysign(magnitude, x);
        y = std::copysign(magnitude, y);
    }
    if (sx) x = std::copysign(std::max(std::abs(x), 2.0 / r.width()), x);
    if (sy) y = std::copysign(std::max(std::abs(y), 2.0 / r.height()), y);
    QTransform result;
    result.translate(anchor.x(), anchor.y());
    result.scale(x, y);
    result.translate(-anchor.x(), -anchor.y());
    return result;
}

bool inRotationZone(const QRect &rect, const QPointF &canvasPos, double zoom) {
    if (rect.isEmpty()) return false;
    if (QRectF(rect).contains(canvasPos)) return false;   // inside → move
    // A band a fixed number of screen pixels wide, beyond the handles.
    const double band = std::max(28.0 / std::max(zoom, 0.01), radius(rect, zoom) * 3.0);
    const QRectF outer = QRectF(rect).adjusted(-band, -band, band, band);
    return outer.contains(canvasPos);
}

QTransform rotationAbout(const QPointF &center, double angleDeg) {
    QTransform t;
    t.translate(center.x(), center.y());
    t.rotate(angleDeg);
    t.translate(-center.x(), -center.y());
    return t;
}

QVector<QRectF> rects(const QRect &rect, double zoom) {
    const qreal r = radius(rect, zoom);
    QVector<QRectF> out;
    for (const QPointF &p : anchors(rect))
        out.append(QRectF(p.x() - r, p.y() - r, r * 2.0, r * 2.0));
    return out;
}

void setScaledMask(Selection &sel, const QImage &originalMask,
                   const QRect &from, const QRect &to, bool smooth) {
    if (originalMask.isNull() || from.isEmpty() || to.isEmpty()) return;

    QImage newMask(originalMask.size(), QImage::Format_Grayscale8);
    newMask.fill(0);
    const QImage scaled = originalMask.copy(from)
                              .scaled(to.size(), Qt::IgnoreAspectRatio,
                                      smooth ? Qt::SmoothTransformation : Qt::FastTransformation)
                              .convertToFormat(QImage::Format_Grayscale8);
    QPainter p(&newMask);
    p.drawImage(to.topLeft(), scaled);
    p.end();
    sel.setMaskImage(newMask);
}

void setRotatedMask(Selection &sel, const QImage &originalMask,
                    const QPointF &center, double angleDeg, bool smooth) {
    if (originalMask.isNull()) return;

    QImage newMask(originalMask.size(), QImage::Format_Grayscale8);
    newMask.fill(0);
    QPainter p(&newMask);
    if (smooth) p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setTransform(rotationAbout(center, angleDeg));
    p.drawImage(0, 0, originalMask);
    p.end();
    sel.setMaskImage(newMask);
}

} // namespace SelHandles
