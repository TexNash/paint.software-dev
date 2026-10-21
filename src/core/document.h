#pragma once

#include <QObject>
#include <QImage>
#include <QColor>
#include <QSize>
#include <memory>
#include <vector>
#include <functional>

#include "layer.h"
#include "selection.h"
#include "history.h"
#include "floatingpixelsession.h"

class Document : public QObject {
    Q_OBJECT
public:
    explicit Document(int width = 800, int height = 600, QObject *parent = nullptr);
    explicit Document(const QString &filePath, QObject *parent = nullptr);

    // Size
    int width() const { return m_width; }
    int height() const { return m_height; }
    QSize size() const { return QSize(m_width, m_height); }
    void resize(int width, int height);
    void resizeCanvas(int width, int height, int anchorX = 0, int anchorY = 0);

    // Layers
    int layerCount() const { return static_cast<int>(m_layers.size()); }
    Layer *layerAt(int index);
    const Layer *layerAt(int index) const;
    Layer *activeLayer();
    int activeLayerIndex() const { return m_activeLayer; }
    void setActiveLayer(int index);

    int addLayer(const QString &name = QString());
    int addLayer(const QImage &image, const QString &name = QString());
    // One undoable clipboard paste; canvas growth does not resample artwork.
    int pasteImage(const QImage &image, const QString &name, bool expandCanvas);
    void removeLayer(int index);
    void moveLayer(int fromIndex, int toIndex);
    void duplicateLayer(int index);
    void mergeLayerDown(int index, bool keepArtwork = false);
    void flattenImage();

    // Direct layer manipulation (for undo/redo)
    void insertLayerDirect(int index, std::shared_ptr<Layer> layer);
    void removeLayerDirect(int index);
    void moveLayerDirect(int fromIndex, int toIndex);

    // Full-document snapshot support (for undo/redo of structural ops)
    std::vector<std::shared_ptr<Layer>> snapshotLayers() const;
    // Value snapshots for preview/export workers, including the effective target.
    std::vector<std::shared_ptr<Layer>> snapshotEffectiveLayers() const;
    void restoreFromSnapshot(std::vector<std::shared_ptr<Layer>> layers,
                             int width, int height, int activeLayer,
                             const FloatingPixelSession *session = nullptr, int sessionLayer = -1);

    // Undoable structural operations
    void cropTo(const QRect &rect);
    // Whole-image geometric transforms: rotate every layer and (for 90°/270°)
    // swap the document dimensions. quarterTurns is clockwise 90° steps.
    void rotate(int quarterTurns, const QString &description);
    void flip(bool horizontal, const QString &description);
    // Reorder the whole stack. newOrder holds current layer indices in their new
    // bottom-to-top position. Undoable. keepActive = the layer to stay active.
    void reorderLayers(const std::vector<int> &newOrder, int keepActiveOriginalIndex);

    // Layer property mutation (emits documentChanged so the canvas repaints)
    // Publish property-only changes without rebuilding the layer stack.
    void notifyLayerAppearanceChanged(int index);
    void setLayerOpacity(int index, float opacity);
    void setLayerVisibility(int index, bool visible);
    void setLayerBlendMode(int index, BlendMode mode);
    void setLayerName(int index, const QString &name);
    void setLayerLocked(int index, bool locked);

    // Selection
    Selection &selection() { return m_selection; }
    const Selection &selection() const { return m_selection; }

    // Low-level initialization: no UI activation or automatic history entry.
    // Captures a base snapshot; does not lift/erase pixels or mutate the selection.
    bool beginFloatingPixelSession(int layerIndex, const QImage &source,
                                   const QImage &coverage, QPoint sourceOrigin);
    // Atomic, undoable paste initialization. Placement/canvas decisions are made
    // by the caller. A previous session is materialized in the same history entry.
    bool pasteFloatingPixels(const QImage &source, const QImage &coverage, QPoint origin,
                             bool intoNewLayer = false, QSize canvasSize = QSize());
    // Read-only access ensures transform previews cannot bypass cache invalidation.
    const FloatingPixelSession *floatingPixelSession() const;
    // Preview an absolute transform and synchronize visible selection coverage.
    // History callers capture before a gesture and after its final preview.
    bool setFloatingPixelTransform(const QTransform &transform);
    // Context changes materialize pixels but preserve selection/editing intent.
    bool suspendFloatingPixels(bool recordHistory = true);
    bool finishFloatingPixels(bool clearSelection = true);
    // Prepare a source from the current active layer after suspension, or a copy.
    // Caller owns the gesture's before/after history checkpoint.
    bool prepareFloatingPixelGesture(bool copy = false);
    // Releases temporary state only; never rolls back or commits layer pixels.
    // Required before starting another session, including after target removal.
    void discardFloatingPixelSession();

    // History
    HistoryManager &history() { return m_history; }
    void pushImageEdit(int layerIndex, const QImage &before, const QString &desc);
    // Emitted by history commands so the canvas and panels refresh after undo/redo.
    void notifyContentChanged() { emit documentChanged(); emit layersChanged(); }

    // Rendering
    static QImage compositeLayers(const std::vector<Layer> &layers, QSize size);
    bool commitImageEdits(const std::vector<ImageEditState> &edits, const QString &description);
    QImage flatten() const;
    QImage thumbnail(QSize maximumSize) const;
    QImage flattenVisible() const;
    // Same composition as flattenVisible(), restricted to a document-space tile.
    QImage flattenVisible(const QRect &region) const;

    // File I/O
    // quality: 0..100 for lossy formats (JPEG, WebP). -1 means "reuse the last
    // chosen quality" (so a plain Save after Save As keeps the picked setting,
    // like Paint.NET remembering its Save Configuration).
    bool save(const QString &filePath, int quality = -1);
    bool load(const QString &filePath);
    // Native layered format (.psw) — preserves every layer, not just a flattened
    // image. Used automatically when the path ends in ".psw".
    bool saveNative(const QString &filePath);
    bool loadNative(const QString &filePath);
    static bool isNativeFormat(const QString &filePath);
    QString filePath() const { return m_filePath; }
    bool isModified() const { return m_modified; }
    void setModified(bool modified) { m_modified = modified; }
    bool editAllLayers() const { return m_editAllLayers; }
    void setEditAllLayers(bool editAllLayers);

    // Colors
    QColor primaryColor() const { return m_primaryColor; }
    QColor secondaryColor() const { return m_secondaryColor; }
    void setPrimaryColor(const QColor &color);
    void setSecondaryColor(const QColor &color);
    void swapColors();

signals:
    // Finalize a pending gesture before checkpointing a context transition.
    void floatingContextChanging();
    void layersChanged();
    void layerAppearanceChanged(int index);
    void activeLayerChanged(int index);
    void selectionChanged();
    void documentChanged();
    // Transform-only preview: raster dirtiness is confined to these canvas regions.
    void floatingPixelsChanged(const QRegion &regions);
    void editScopeChanged(bool editAllLayers);
    void primaryColorChanged(const QColor &color);
    void secondaryColorChanged(const QColor &color);
    void sizeChanged(int width, int height);

private:
    qint64 m_floatingSelectionKey = -1;
    friend class FloatingPixelSessionCommand;
    // Restores checkpoint state without signals; callers publish the complete state.
    void restoreFloatingPixelSession(const FloatingPixelSession *session, int layerIndex);
    std::optional<Layer> floatingPixelRaster() const;
    using FloatingTileVisitor = std::function<void(QRect, const QImage &, const QImage &, bool)>;
    bool visitFloatingTiles(const FloatingPixelSession &session, const QRect &region, const FloatingTileVisitor &visitor) const;
    QImage floatingCoverage(const FloatingPixelSession &session, const QRect &region) const;
    QImage floatingRaster(const FloatingPixelSession &session, const QRect &region) const;
    int m_width;
    int m_height;
    std::vector<std::shared_ptr<Layer>> m_layers;
    int m_activeLayer = 0;
    Selection m_selection;
    std::unique_ptr<FloatingPixelSession> m_floatingPixelSession;
    HistoryManager m_history;
    QString m_filePath;
    bool m_modified = false;
    int m_saveQuality = -1;   // last lossy-format quality chosen via Save As
    bool m_editAllLayers = false;
    QColor m_primaryColor = Qt::black;
    QColor m_secondaryColor = Qt::white;
};
