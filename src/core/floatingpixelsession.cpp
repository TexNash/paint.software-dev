#include "floatingpixelsession.h"
#include "layer.h"
#include <cmath>
#include <QPainter>
#include <cstring>
#include <limits>
#include <vector>

namespace {
bool integerTranslation(const QTransform &transform) {
    return transform.type() <= QTransform::TxTranslate
        && transform.dx() == std::floor(transform.dx()) && transform.dy() == std::floor(transform.dy())
        && transform.dx() >= std::numeric_limits<int>::min() && transform.dx() <= std::numeric_limits<int>::max()
        && transform.dy() >= std::numeric_limits<int>::min() && transform.dy() <= std::numeric_limits<int>::max();
}
}


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
    session->m_fullCoverage = true;
    for (int y = 0; y < coverage.height() && session->m_fullCoverage; ++y)
        for (int x = 0; x < coverage.width(); ++x)
            if (coverage.constScanLine(y)[x] != 255) { session->m_fullCoverage = false; break; }
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
    const QRectF bounds = transform.mapRect(QRectF(QPointF(), m_source.size()))
                              .united(QRectF(m_baseOffset, m_base.size()));
    for (double value : {bounds.x(), bounds.y(), bounds.width(), bounds.height(),
                         bounds.right(), bounds.bottom()})
        if (!std::isfinite(value)) return false;
    m_transform = transform;
    m_inverse = inverse;
    return true;
}

QRectF FloatingPixelSession::documentBounds() const {
    return m_transform.mapRect(QRectF(QPointF(), m_source.size()))
        .united(QRectF(m_baseOffset, m_base.size()));
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

QImage FloatingPixelSession::renderBase(const QRect &documentRegion) const {
    if (documentRegion.isEmpty()) return {};
    QImage result(documentRegion.size(), QImage::Format_ARGB32_Premultiplied);
    if (result.isNull()) return {};
    const bool originalLiftPose = !m_liftOriginal.isNull() && m_transform == m_liftTransform;
    const QImage &base = originalLiftPose ? m_liftOriginal : m_base;
    result.fill(Qt::transparent);
    {
        QPainter painter(&result);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(QPointF(double(m_baseOffset.x()) - documentRegion.x(),
                                 double(m_baseOffset.y()) - documentRegion.y()), base);
    }
    return result;
}

QImage FloatingPixelSession::render(const QRect &documentRegion) const {
    QImage result = renderBase(documentRegion);
    if (result.isNull() || (!m_liftOriginal.isNull() && m_transform == m_liftTransform)) return result;
    // Intersect before narrowing to QRect: distant off-canvas transforms need
    // no sampling and must not overflow integer coordinates.
    const QRect affected = sourceBounds().intersected(QRectF(documentRegion)).toAlignedRect().intersected(documentRegion);
    if (!affected.isEmpty() && integerTranslation(m_transform)) {
        const int sx = int(double(affected.x()) - m_transform.dx());
        const int sy = int(double(affected.y()) - m_transform.dy());
        for (int y = 0; y < affected.height(); ++y) {
            auto *destination = reinterpret_cast<QRgb *>(result.scanLine(affected.y() - documentRegion.y() + y))
                + affected.x() - documentRegion.x();
            const auto *source = reinterpret_cast<const QRgb *>(m_source.constScanLine(sy + y)) + sx;
            const uchar *coverage = m_coverage.constScanLine(sy + y) + sx;
            if (m_fullCoverage) std::memcpy(destination, source, size_t(affected.width()) * 4);
            else for (int x = 0; x < affected.width(); ++x) {
                if (coverage[x] == 255) destination[x] = source[x];
                else if (coverage[x]) destination[x] = replaceCoveredPixel(destination[x], source[x], coverage[x]);
            }
        }
        return result;
    }
    const auto *pixels = reinterpret_cast<const QRgb *>(m_source.constBits());
    const uchar *mask = m_coverage.constBits();
    const qsizetype pixelStride = m_source.bytesPerLine() / 4, maskStride = m_coverage.bytesPerLine();
    // Match QTransform::map's classified affine paths, including its fuzzy
    // identity/axis classification, while keeping getters out of pixel loops.
    const auto type = m_inverse.type();
    const double a = type <= QTransform::TxTranslate ? 1 : m_inverse.m11();
    const double d = type <= QTransform::TxTranslate ? 1 : m_inverse.m22();
    const double b = type <= QTransform::TxScale ? 0 : m_inverse.m12();
    const double c = type <= QTransform::TxScale ? 0 : m_inverse.m21();
    const double tx = m_inverse.dx(), ty = m_inverse.dy();
    const int sourceWidth = m_source.width(), sourceHeight = m_source.height();
    if (b == 0 && c == 0 && !affected.isEmpty()) {
        std::vector<int> columns(size_t(affected.width()), -1);
        for (int x = 0; x < affected.width(); ++x) {
            const double sx = (double(affected.x()) + x + 0.5) * a + tx;
            if (sx >= 0 && sx < sourceWidth) columns[x] = int(sx);
        }
        for (int y = 0; y < affected.height(); ++y) {
            const double sy = (double(affected.y()) + y + 0.5) * d + ty;
            if (!(sy >= 0 && sy < sourceHeight)) continue;
            const auto *source = pixels + int(sy) * pixelStride;
            const auto *coverage = mask + int(sy) * maskStride;
            auto *destination = reinterpret_cast<QRgb *>(result.scanLine(affected.y() - documentRegion.y() + y))
                + affected.x() - documentRegion.x();
            for (int x = 0; x < affected.width(); ++x) {
                const int sx = columns[x];
                if (sx < 0) continue;
                const int value = m_fullCoverage ? 255 : coverage[sx];
                if (value == 255) destination[x] = source[sx];
                else if (value) destination[x] = replaceCoveredPixel(destination[x], source[sx], value);
            }
        }
        return result;
    }
    for (int y = 0; y < affected.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(result.scanLine(affected.y() - documentRegion.y() + y));
        const double py = double(affected.y()) + y + 0.5, yc = py * c, yd = py * d;
        for (int x = 0; x < affected.width(); ++x) {
            const double px = double(affected.x()) + x + 0.5;
            const double sx = px * a + yc + tx, sy = px * b + yd + ty;
            if (sx >= 0 && sy >= 0 && sx < sourceWidth && sy < sourceHeight) {
                const int ix = int(sx), iy = int(sy);
                const int coverage = mask[iy * maskStride + ix];
                auto &pixel = line[affected.x() - documentRegion.x() + x];
                const QRgb source = pixels[iy * pixelStride + ix];
                if (coverage == 255) pixel = source;
                else if (coverage) pixel = replaceCoveredPixel(pixel, source, coverage);
            }
        }
    }
    return result;
}

QImage FloatingPixelSession::renderCoverage(const QRect &documentRegion) const {
    return sample(documentRegion, nullptr);
}

QImage FloatingPixelSession::sample(const QRect &documentRegion, QImage *pixels) const {
    if (documentRegion.isEmpty()) return {};
    QImage result(documentRegion.size(), QImage::Format_Grayscale8);
    if (result.isNull()) return {};
    result.fill(0);
    if (pixels) {
        *pixels = QImage(documentRegion.size(), QImage::Format_ARGB32_Premultiplied);
        if (pixels->isNull()) return {};
        pixels->fill(0);
    }
    const auto *sourcePixels = reinterpret_cast<const QRgb *>(m_source.constBits());
    const qsizetype pixelStride = m_source.bytesPerLine() / 4;
    const QRect affected = sourceBounds().intersected(QRectF(documentRegion)).toAlignedRect().intersected(documentRegion);
    if (!affected.isEmpty() && integerTranslation(m_transform)) {
        const int sx = int(double(affected.x()) - m_transform.dx());
        const int sy = int(double(affected.y()) - m_transform.dy());
        for (int y = 0; y < affected.height(); ++y) {
            if (pixels) std::memcpy(pixels->scanLine(affected.y() - documentRegion.y() + y) + qsizetype(affected.x() - documentRegion.x()) * 4,
                                    m_source.constScanLine(sy + y) + qsizetype(sx) * 4, size_t(affected.width()) * 4);
            std::memcpy(result.scanLine(affected.y() - documentRegion.y() + y) + affected.x() - documentRegion.x(),
                        m_coverage.constScanLine(sy + y) + sx, size_t(affected.width()));
        }
        return result;
    }
    const uchar *mask = m_coverage.constBits();
    const qsizetype stride = m_coverage.bytesPerLine();
    // Match QTransform::map's classified affine paths, including its fuzzy
    // identity/axis classification, while keeping getters out of pixel loops.
    const auto type = m_inverse.type();
    const double a = type <= QTransform::TxTranslate ? 1 : m_inverse.m11();
    const double d = type <= QTransform::TxTranslate ? 1 : m_inverse.m22();
    const double b = type <= QTransform::TxScale ? 0 : m_inverse.m12();
    const double c = type <= QTransform::TxScale ? 0 : m_inverse.m21();
    const double tx = m_inverse.dx(), ty = m_inverse.dy();
    const int sourceWidth = m_source.width(), sourceHeight = m_source.height();
    if (b == 0 && c == 0 && !affected.isEmpty()) {
        std::vector<int> columns(size_t(affected.width()), -1);
        int first = affected.width(), last = -1;
        for (int x = 0; x < affected.width(); ++x) {
            const double sx = (double(affected.x()) + x + 0.5) * a + tx;
            if (sx >= 0 && sx < sourceWidth) { columns[x] = int(sx); first = qMin(first, x); last = x; }
        }
        for (int y = 0; y < affected.height(); ++y) {
            const double sy = (double(affected.y()) + y + 0.5) * d + ty;
            if (!(sy >= 0 && sy < sourceHeight)) continue;
            auto *destination = result.scanLine(affected.y() - documentRegion.y() + y) + affected.x() - documentRegion.x();
            const auto *source = mask + int(sy) * stride;
            if (pixels) {
                auto *out = reinterpret_cast<QRgb *>(pixels->scanLine(affected.y() - documentRegion.y() + y)) + affected.x() - documentRegion.x();
                const auto *in = sourcePixels + int(sy) * pixelStride;
                for (int x = first; x <= last; ++x) out[x] = in[columns[x]];
            }
            if (m_fullCoverage && last >= first) std::memset(destination + first, 255, size_t(last - first + 1));
            else for (int x = first; x <= last; ++x) destination[x] = source[columns[x]];
        }
        return result;
    }
    for (int y = 0; y < affected.height(); ++y) {
        uchar *line = result.scanLine(affected.y() - documentRegion.y() + y);
        auto *out = pixels ? reinterpret_cast<QRgb *>(pixels->scanLine(affected.y() - documentRegion.y() + y)) : nullptr;
        const double py = double(affected.y()) + y + 0.5, yc = py * c, yd = py * d;
        for (int x = 0; x < affected.width(); ++x) {
            const double px = double(affected.x()) + x + 0.5;
            const double sx = px * a + yc + tx, sy = px * b + yd + ty;
            if (sx >= 0 && sy >= 0 && sx < sourceWidth && sy < sourceHeight) {
                line[affected.x() - documentRegion.x() + x] = mask[int(sy) * stride + int(sx)];
                if (out) out[affected.x() - documentRegion.x() + x] = sourcePixels[int(sy) * pixelStride + int(sx)];
            }
        }
    }
    return result;
}

QImage FloatingPixelSession::transformedSource(const QRect &documentRegion) const {
    if (documentRegion.isEmpty()) return {};
    QImage result(documentRegion.size(), QImage::Format_ARGB32_Premultiplied);
    if (result.isNull()) return {};
    result.fill(Qt::transparent);
    for (int y = 0; y < result.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            QPoint source;
            if (sourcePoint(QPointF(double(documentRegion.x()) + x + 0.5,
                                   double(documentRegion.y()) + y + 0.5), source)
                && m_coverage.constScanLine(source.y())[source.x()]) line[x] = m_source.pixel(source);
        }
    }
    return result;
}

QImage FloatingPixelSession::renderScaled(const QRect &region, QSize outputSize) const {
    if (region.isEmpty() || outputSize.isEmpty()) return {};
    QImage result(outputSize, QImage::Format_ARGB32_Premultiplied);
    if (result.isNull()) return {};
    const bool originalLiftPose = !m_liftOriginal.isNull() && m_transform == m_liftTransform;
    const QImage &base = originalLiftPose ? m_liftOriginal : m_base;
    for (int y = 0; y < result.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            const QPointF point(region.x() + (x + 0.5) * region.width() / outputSize.width(),
                                region.y() + (y + 0.5) * region.height() / outputSize.height());
            const double bx = std::floor(point.x()) - m_baseOffset.x();
            const double by = std::floor(point.y()) - m_baseOffset.y();
            QRgb pixel = bx >= 0 && by >= 0 && bx < base.width() && by < base.height()
                ? base.pixel(int(bx), int(by)) : 0;
            QPoint source;
            if (!originalLiftPose && sourcePoint(point, source))
                pixel = replaceCoveredPixel(pixel, m_source.pixel(source), m_coverage.constScanLine(source.y())[source.x()]);
            line[x] = pixel;
        }
    }
    return result;
}
