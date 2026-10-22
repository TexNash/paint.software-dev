#include "tool.h"
#include "core/document.h"
#include "core/selection.h"
#include <QPainter>
#include <QImage>

void Tool::clipToSelection(QPainter &painter, Document *doc) {
    if (!doc) return;
    if (auto *layer = doc->activeLayer()) {
        painter.translate(-layer->offset());
        if (layer->image().size() != doc->size() || !layer->offset().isNull())
            painter.setClipRect(QRect(QPoint(), doc->size()));
    }
    const Selection &sel = doc->selection();
    if (!sel.hasSelection()) return;
    QRegion region = sel.region(true);
    painter.setClipRegion(region, Qt::IntersectClip);
}

QPointF Tool::imagePoint(const QPointF &documentPoint, Document *doc) {
    return documentPoint - (doc && doc->activeLayer() ? doc->activeLayer()->offset() : QPoint());
}

bool Tool::imageSelectionAllows(Document *doc, int x, int y) {
    if (!doc) return true;
    const QPoint offset = doc->activeLayer() ? doc->activeLayer()->offset() : QPoint();
    const QPoint point = QPoint(x, y) + offset;
    return QRect(QPoint(), doc->size()).contains(point) && selectionAllows(doc, point.x(), point.y());
}

QPainter::CompositionMode Tool::compositionModeFor(int blendIndex) {
    // Maps the blend-mode combo (Layer::allBlendModes order) to a Qt mode.
    switch (blendIndex) {
    case 1:  return QPainter::CompositionMode_Multiply;   // Multiply
    case 2:  return QPainter::CompositionMode_Plus;        // Additive
    case 3:  return QPainter::CompositionMode_ColorBurn;
    case 4:  return QPainter::CompositionMode_ColorDodge;
    case 5:  return QPainter::CompositionMode_Darken;
    case 6:  return QPainter::CompositionMode_Difference;
    case 7:  return QPainter::CompositionMode_ColorDodge;  // Glow    (approximation)
    case 8:  return QPainter::CompositionMode_Lighten;
    case 9:  return QPainter::CompositionMode_Difference;  // Negation (approximation)
    case 10: return QPainter::CompositionMode_Overlay;
    case 11: return QPainter::CompositionMode_Screen;      // Reflect (approximation)
    case 12: return QPainter::CompositionMode_Screen;
    case 13: return QPainter::CompositionMode_Xor;         // Xor
    case 14: return QPainter::CompositionMode_Source;      // Overwrite (callers replace per-pixel)
    default: return QPainter::CompositionMode_SourceOver;  // Normal + unsupported
    }
}

bool Tool::selectionAllows(Document *doc, int x, int y) {
    if (!doc) return true;
    const Selection &sel = doc->selection();
    if (!sel.hasSelection()) return true;
    return sel.selectionValue(x, y) != 0;
}

int Tool::imageSelectionCoverage(Document *doc, int x, int y) {
    if (!doc) return 255;
    const QPoint offset = doc->activeLayer() ? doc->activeLayer()->offset() : QPoint();
    return doc->selection().selectionValue(x + offset.x(), y + offset.y());
}

void Tool::clipImageEdit(Document *doc, const QImage &before, QImage &after, QRect dirty) {
    if (!doc || !doc->selection().hasSelection()) return;
    if (dirty.isNull()) dirty = after.rect();
    dirty = dirty.intersected(after.rect()).intersected(before.rect());
    // Both buffers are premultiplied storage: interpolate colour and alpha
    // together so clipping cannot introduce a colour fringe at transparency.
    for (int y = dirty.top(); !dirty.isEmpty() && y <= dirty.bottom(); ++y) {
        const auto *base = reinterpret_cast<const QRgb *>(before.constScanLine(y));
        auto *out = reinterpret_cast<QRgb *>(after.scanLine(y));
        for (int x = dirty.left(); x <= dirty.right(); ++x) {
            const int coverage = imageSelectionCoverage(doc, x, y), inverse = 255 - coverage;
            if (coverage == 255) continue;
            if (!coverage) { out[x] = base[x]; continue; }
            out[x] = qRgba((qRed(base[x])*inverse + qRed(out[x])*coverage)/255,
                           (qGreen(base[x])*inverse + qGreen(out[x])*coverage)/255,
                           (qBlue(base[x])*inverse + qBlue(out[x])*coverage)/255,
                           (qAlpha(base[x])*inverse + qAlpha(out[x])*coverage)/255);
        }
    }
}

QImage Tool::maskEditToSelection(Document *doc, const QImage &before, const QImage &after) {
    QImage result = after.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    clipImageEdit(doc, before.convertToFormat(QImage::Format_ARGB32_Premultiplied), result);
    return result;
}
