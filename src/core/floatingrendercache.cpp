#include "floatingrendercache.h"
#include <QMutexLocker>
#include <limits>

FloatingRenderCache::FloatingRenderCache(int limitKiB) : m_tiles(qMax(0, limitKiB)) {}
FloatingRenderCache &FloatingRenderCache::shared() {
    static FloatingRenderCache cache;
    return cache;
}
FloatingRenderCache::Tile FloatingRenderCache::find(const QByteArray &key) {
    QMutexLocker lock(&m_mutex);
    if (const auto *tile = m_tiles.object(key)) { ++m_hits; return *tile; }
    ++m_misses;
    return {};
}
void FloatingRenderCache::insert(const QByteArray &key, const Tile &tile) {
    if (tile.isNull()) return;
    const qint64 cost = (tile.pixels.sizeInBytes() + tile.coverage.sizeInBytes() + key.size() + sizeof(Tile) + 1023) / 1024;
    QMutexLocker lock(&m_mutex);
    if (cost > m_tiles.maxCost() || cost > std::numeric_limits<int>::max()) return;
    m_tiles.insert(key, new Tile(tile), int(cost));
}
FloatingRenderCache::Stats FloatingRenderCache::stats() const {
    QMutexLocker lock(&m_mutex);
    return {m_hits, m_misses, int(m_tiles.totalCost()), int(m_tiles.maxCost()), int(m_tiles.size())};
}
void FloatingRenderCache::clear() {
    QMutexLocker lock(&m_mutex);
    m_tiles.clear(); m_hits = m_misses = 0;
}
