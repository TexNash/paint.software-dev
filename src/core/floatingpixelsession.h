#pragma once

#include <QImage>
#include <QRect>
#include <QTransform>
#include <memory>

class Layer;

// Pixel state only: does not modify its target layer. Document/history own lifecycle.
// Source pixels are UNMASKED premultiplied pixels; coverage is independent alpha.
class FloatingPixelSession {
public:
    static std::unique_ptr<FloatingPixelSession> create(
        const std::shared_ptr<Layer> &target, const QImage &source,
        const QImage &coverage, QPoint sourceOrigin);

    bool targets(const Layer *layer) const;
    // Return values use QImage copy-on-write. No mutable source references escape.
    QImage sourcePixels() const { return m_source; }
    QImage sourceCoverage() const { return m_coverage; }
    QImage basePixels() const { return m_base; }
    QPoint baseOffset() const { return m_baseOffset; }
    QTransform transform() const { return m_transform; }
    bool isSuspended() const { return m_suspended; }
    QRectF sourceBounds() const { return m_transform.mapRect(QRectF(QPointF(), m_source.size())); }
    // Absolute source-local -> document transform. Invalid updates leave state intact.
    bool setTransform(const QTransform &transform);
    // Finite raster domain enclosing the captured base and transformed source.
    // Keep this domain when compositing modes where transparent pixels matter.
    QRectF documentBounds() const;

    // Explicit document-space region, independent of canvas bounds. Nearest-neighbour
    // sampling is a foundation policy, not a Paint.NET resampling implementation.
    QImage render(const QRect &documentRegion) const;
    // Bounded preview sampling; never materializes the full document.
    QImage renderScaled(const QRect &documentRegion, QSize outputSize) const;
    QImage renderCoverage(const QRect &documentRegion) const;
    QImage transformedSource(const QRect &documentRegion) const;

private:
    friend class Document; // Rebind checkpoint copies to restored layer instances.
    FloatingPixelSession() = default;
    bool sourcePoint(QPointF documentPoint, QPoint &sourcePoint) const;
    static QRgb replaceCoveredPixel(QRgb destination, QRgb source, int coverage);

    std::weak_ptr<Layer> m_target;
    QImage m_source, m_coverage, m_base;
    // A context boundary starts a new lift from the edited raster. At the lift's
    // original pose, preserve that raster exactly, including fractional edges.
    QImage m_liftOriginal;
    QTransform m_liftTransform;
    bool m_suspended = false;
    bool m_fullCoverage = false;
    QPoint m_baseOffset;
    QTransform m_transform, m_inverse;
};
