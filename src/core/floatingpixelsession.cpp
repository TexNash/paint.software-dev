#include "floatingpixelsession.h"
#include "layer.h"
#include <cmath>

std::unique_ptr<FloatingPixelSession> FloatingPixelSession::create(
    const std::shared_ptr<Layer> &target, const QImage &source,
    const QImage &coverage, QPoint sourceOrigin) {
    if (!target || source.isNull() || coverage.isNull()
        || source.size() != coverage.size()
        || coverage.format() != QImage::Format_Grayscale8 || target->image().isNull())
        return nullptr;
    bool selected = false;
    for (int y = 0; y < coverage.height() && !selected; ++y)
        for (int x = 0; x < coverage.width(); ++x)
            if (coverage.constScanLine(y)[x]) { selected = true; break; }
    if (!selected) return nullptr;

    auto session = std::unique_ptr<FloatingPixelSession>(new FloatingPixelSession);
    // Deep copies also isolate externally backed images and previously obtained
    // writable bits() pointers, which implicit sharing alone cannot protect against.
    session->m_source = source.convertToFormat(QImage::Format_ARGB32_Premultiplied).copy();
    session->m_coverage = coverage.copy();
    session->m_base = target->image().copy();
    if (session->m_source.isNull() || session->m_coverage.isNull() || session->m_base.isNull())
        return nullptr;
    session->m_target = target;
    session->m_baseOffset = target->offset();
    session->setTransform(QTransform::fromTranslate(sourceOrigin.x(), sourceOrigin.y()));
    return session;
}

bool FloatingPixelSession::targets(const Layer *layer) const {
    const auto target = m_target.lock();
    return target && target.get() == layer;
}

bool FloatingPixelSession::setTransform(const QTransform &transform) {
    if (!transform.isAffine()) return false;
    for (double value : {transform.m11(), transform.m12(), transform.m21(),
                         transform.m22(), transform.dx(), transform.dy()})
        if (!std::isfinite(value)) return false;
    bool invertible = false;
    const QTransform inverse = transform.inverted(&invertible);
    if (!invertible) return false;
    for (double value : {inverse.m11(), inverse.m12(), inverse.m21(),
                         inverse.m22(), inverse.dx(), inverse.dy()})
        if (!std::isfinite(value)) return false;
    m_transform = transform;
    m_inverse = inverse;
    return true;
}

bool FloatingPixelSession::sourcePoint(QPointF documentPoint, QPoint &sourcePoint) const {
    const QPointF point = m_inverse.map(documentPoint);
    // Check floating-point bounds before narrowing; huge off-canvas transforms
    // must not overflow integer coordinates.
    if (!(point.x() >= 0 && point.y() >= 0
          && point.x() < m_source.width() && point.y() < m_source.height())) return false;
    sourcePoint = QPoint(int(std::floor(point.x())), int(std::floor(point.y())));
    return true;
}

QRgb FloatingPixelSession::replaceCoveredPixel(QRgb destination, QRgb source, int coverage) {
    // Encapsulated coverage policy. Both operands are premultiplied; this is
    // coverage interpolation, NOT straight-colour math or source-over blending.
    // Apply selection coverage once, including to source alpha.
    auto channel = [coverage](int dst, int src) {
        return (src * coverage + dst * (255 - coverage) + 127) / 255;
    };
    return qRgba(channel(qRed(destination), qRed(source)),
                 channel(qGreen(destination), qGreen(source)),
                 channel(qBlue(destination), qBlue(source)),
                 channel(qAlpha(destination), qAlpha(source)));
}

QImage FloatingPixelSession::render(const QRect &documentRegion) const {
    if (documentRegion.isEmpty()) return {};
    QImage result(documentRegion.size(), QImage::Format_ARGB32_Premultiplied);
    if (result.isNull()) return {};
    for (int y = 0; y < result.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            const double dx = double(documentRegion.x()) + x;
            const double dy = double(documentRegion.y()) + y;
            const double bx = dx - m_baseOffset.x(), by = dy - m_baseOffset.y();
            QRgb pixel = 0;
            if (bx >= 0 && by >= 0 && bx < m_base.width() && by < m_base.height())
                pixel = m_base.pixel(int(bx), int(by));
            QPoint source;
            if (sourcePoint(QPointF(dx + 0.5, dy + 0.5), source))
                pixel = replaceCoveredPixel(pixel, m_source.pixel(source),
                                             m_coverage.constScanLine(source.y())[source.x()]);
            line[x] = pixel;
        }
    }
    return result;
}

QImage FloatingPixelSession::renderCoverage(const QRect &documentRegion) const {
    if (documentRegion.isEmpty()) return {};
    QImage result(documentRegion.size(), QImage::Format_Grayscale8);
    if (result.isNull()) return {};
    result.fill(0);
    for (int y = 0; y < result.height(); ++y) {
        uchar *line = result.scanLine(y);
        for (int x = 0; x < result.width(); ++x) {
            QPoint source;
            if (sourcePoint(QPointF(double(documentRegion.x()) + x + 0.5,
                                   double(documentRegion.y()) + y + 0.5), source))
                line[x] = m_coverage.constScanLine(source.y())[source.x()];
        }
    }
    return result;
}
