#pragma once

#include <QByteArray>
#include <QCache>
#include <QImage>
#include <QMutex>

// Full-resolution immutable samples (with coverage), or rendered source-footprint
// rectangles (null coverage). Keys for rendered rectangles include destination
// base identity. Neither form owns documents/layers or includes layer properties.
// One shared LRU bounds storage across documents; history never contains this cache.
class FloatingRenderCache {
public:
    struct Tile {
        QImage pixels;
        QImage coverage;
        QPoint origin; // rendered rectangle origin; source-tile coordinates are supplied by the visitor
        bool opaqueCoverage = false;
        bool isNull() const { return pixels.isNull(); }
    };
    struct Stats { quint64 hits, misses; int costKiB, limitKiB, entries; };
    explicit FloatingRenderCache(int limitKiB = 64 * 1024);
    static FloatingRenderCache &shared();
    Tile find(const QByteArray &key);
    void insert(const QByteArray &key, const Tile &tile);
    Stats stats() const;
    void clear();

private:
    mutable QMutex m_mutex;
    QCache<QByteArray, Tile> m_tiles;
    quint64 m_hits = 0, m_misses = 0;
};
