#include "history.h"
#include "document.h"
#include "layer.h"
#include <algorithm>

FloatingPixelSessionCommand::FloatingPixelSessionCommand(Document *doc, int layerIndex,
                                                         const QString &description)
    : m_doc(doc), m_layerIndex(layerIndex), m_description(description), m_before(capture()) {
    if (m_before.valid) m_target = m_doc->m_layers[layerIndex];
}

FloatingPixelSessionCommand::State FloatingPixelSessionCommand::capture() const {
    State state;
    if (!m_doc) return state;
    const auto *layer = m_doc->layerAt(m_layerIndex);
    if (!layer) return state;
    const auto *session = m_doc->floatingPixelSession();
    // A checkpoint may start or end a session, but must not retarget one.
    if (session && !session->targets(layer)) return state;
    state.image = layer->image();
    state.offset = layer->offset();
    state.retained = layer->retainsOffCanvasPixels();
    state.selection = m_doc->selection();
    if (session) state.session = *session;
    state.valid = true;
    return state;
}

bool FloatingPixelSessionCommand::captureAfter() {
    if (m_hasAfter || !m_before.valid || m_target.lock().get() != m_doc->layerAt(m_layerIndex)) return false;
    State after = capture();
    if (!after.valid) return false;
    m_after = std::move(after);
    m_hasAfter = true;
    return true;
}

void FloatingPixelSessionCommand::apply(const State &state) {
    auto *layer = m_doc->layerAt(m_layerIndex);
    if (!state.valid || !layer) return;
    layer->setImage(state.image);
    layer->setOffset(state.offset);
    layer->setRetainsOffCanvasPixels(state.retained);
    m_doc->restoreFloatingPixelSession(state.session ? &*state.session : nullptr, m_layerIndex);
    m_doc->selection() = state.selection;
    m_doc->setModified(true);
    emit m_doc->selectionChanged();
    m_doc->notifyContentChanged();
}

void FloatingPixelSessionCommand::undo() { if (m_hasAfter) apply(m_before); }
void FloatingPixelSessionCommand::redo() { if (m_hasAfter) apply(m_after); }

MultiImageEditCommand::MultiImageEditCommand(Document *doc, std::vector<ImageEditState> edits, QString description)
    : m_doc(doc), m_edits(std::move(edits)), m_description(std::move(description)) {}
void MultiImageEditCommand::apply(bool after) {
    for (const auto &edit : m_edits)
        if (auto *layer = m_doc->layerAt(edit.index)) layer->setImage(after ? edit.after : edit.before);
    m_doc->notifyContentChanged();
}
void MultiImageEditCommand::undo() { apply(false); }
void MultiImageEditCommand::redo() { apply(true); }

// ImageEditCommand
ImageEditCommand::ImageEditCommand(Document *doc, int layerIndex, const QImage &before, const QImage &after, const QString &desc)
    : m_doc(doc), m_layerIndex(layerIndex), m_before(before), m_after(after), m_description(desc) {}

void ImageEditCommand::undo() {
    if (m_layerIndex >= 0 && m_layerIndex < m_doc->layerCount()) {
        m_doc->layerAt(m_layerIndex)->setImage(m_before);
        m_doc->notifyContentChanged();
    }
}

void ImageEditCommand::redo() {
    if (m_layerIndex >= 0 && m_layerIndex < m_doc->layerCount()) {
        m_doc->layerAt(m_layerIndex)->setImage(m_after);
        m_doc->notifyContentChanged();
    }
}

LayerOffsetCommand::LayerOffsetCommand(Document *doc, int index, QPoint before, QPoint after)
    : m_doc(doc), m_index(index), m_before(before), m_after(after) {}
void LayerOffsetCommand::apply(QPoint offset) {
    if (auto *layer = m_doc->layerAt(m_index)) {
        layer->setOffset(offset);
        m_doc->setModified(true);
        m_doc->notifyContentChanged();
    }
}
void LayerOffsetCommand::undo() { apply(m_before); }
void LayerOffsetCommand::redo() { apply(m_after); }

// LayerAddCommand
LayerAddCommand::LayerAddCommand(Document *doc, int index, const QString &desc)
    : m_doc(doc), m_index(index), m_description(desc) {}

void LayerAddCommand::undo() {
    m_savedLayer = std::make_shared<Layer>(*m_doc->layerAt(m_index));
    m_doc->removeLayerDirect(m_index);
}

void LayerAddCommand::redo() {
    if (m_savedLayer)
        m_doc->insertLayerDirect(m_index, std::make_shared<Layer>(*m_savedLayer));
}

// LayerRemoveCommand
LayerRemoveCommand::LayerRemoveCommand(Document *doc, int index, const QString &desc)
    : m_doc(doc), m_index(index), m_description(desc) {
    m_savedLayer = std::make_shared<Layer>(*m_doc->layerAt(index));
}

void LayerRemoveCommand::undo() {
    m_doc->insertLayerDirect(m_index, std::make_shared<Layer>(*m_savedLayer));
}

void LayerRemoveCommand::redo() {
    m_doc->removeLayerDirect(m_index);
}

// LayerMoveCommand
LayerMoveCommand::LayerMoveCommand(Document *doc, int fromIndex, int toIndex, const QString &desc)
    : m_doc(doc), m_fromIndex(fromIndex), m_toIndex(toIndex), m_description(desc) {}

void LayerMoveCommand::undo() {
    m_doc->moveLayerDirect(m_toIndex, m_fromIndex);
}

void LayerMoveCommand::redo() {
    m_doc->moveLayerDirect(m_fromIndex, m_toIndex);
}

// DocumentSnapshotCommand
DocumentSnapshotCommand::DocumentSnapshotCommand(Document *doc, const QString &desc)
    : m_doc(doc), m_description(desc) {
    m_before = capture(doc);
}

void DocumentSnapshotCommand::captureAfter() {
    m_after = capture(m_doc);
    m_hasAfter = true;
}

DocumentSnapshotCommand::State DocumentSnapshotCommand::capture(Document *doc) {
    State s;
    s.width = doc->width();
    s.height = doc->height();
    s.activeLayer = doc->activeLayerIndex();
    s.layers = doc->snapshotLayers();
    s.selection = doc->selection();
    if (const auto *session = doc->floatingPixelSession()) {
        s.session = *session;
        for (int i = 0; i < doc->layerCount(); ++i)
            if (session->targets(doc->layerAt(i))) { s.sessionLayer = i; break; }
    }
    return s;
}

void DocumentSnapshotCommand::restore(Document *doc, const State &state) {
    std::vector<std::shared_ptr<Layer>> copy;
    copy.reserve(state.layers.size());
    for (const auto &l : state.layers)
        copy.push_back(std::make_shared<Layer>(*l));
    doc->restoreFromSnapshot(std::move(copy), state.width, state.height, state.activeLayer,
                             state.session ? &*state.session : nullptr, state.sessionLayer);
    doc->selection() = state.selection;
    emit doc->selectionChanged();
}

void DocumentSnapshotCommand::undo() {
    restore(m_doc, m_before);
}

void DocumentSnapshotCommand::redo() {
    if (m_hasAfter)
        restore(m_doc, m_after);
}

// LayerPropertyCommand
LayerPropertyCommand::LayerPropertyCommand(Document *doc, int index,
                                           float beforeOpacity, bool beforeVisible, int beforeBlend,
                                           const QString &beforeName, bool beforeLocked,
                                           const QString &desc)
    : m_doc(doc), m_index(index), m_description(desc) {
    m_before = {beforeOpacity, beforeVisible, beforeBlend, beforeName, beforeLocked};
}

void LayerPropertyCommand::captureAfter() {
    if (auto *l = m_doc->layerAt(m_index)) {
        m_after = {l->opacity(), l->isVisible(), static_cast<int>(l->blendMode()),
                   l->name(), l->isLocked()};
        m_hasAfter = true;
    }
}

void LayerPropertyCommand::apply(const Props &p) {
    if (auto *l = m_doc->layerAt(m_index)) {
        l->setOpacity(p.opacity);
        l->setVisible(p.visible);
        l->setBlendMode(static_cast<BlendMode>(p.blend));
        l->setName(p.name);
        l->setLocked(p.locked);
        m_doc->notifyContentChanged();
    }
}

void LayerPropertyCommand::undo() { apply(m_before); }
void LayerPropertyCommand::redo() { if (m_hasAfter) apply(m_after); }

SelectionChangeCommand::SelectionChangeCommand(Document *doc, const Selection &before,
                                               const Selection &after, QString description)
    : m_doc(doc), m_before(before), m_after(after), m_description(std::move(description)) {}
void SelectionChangeCommand::apply(const Selection &selection) {
    m_doc->selection() = selection;
    emit m_doc->selectionChanged();
}
void SelectionChangeCommand::undo() { apply(m_before); }
void SelectionChangeCommand::redo() { apply(m_after); }

// HistoryManager
HistoryManager::HistoryManager(QObject *parent) : QObject(parent) {}

void HistoryManager::push(std::unique_ptr<HistoryCommand> cmd) {
    m_redoStack.clear();
    m_undoStack.push_back(std::move(cmd));
    // Cap the undo depth to keep memory bounded (each image edit stores full
    // before/after frames).
    const size_t kMaxUndo = 100;
    if (m_undoStack.size() > kMaxUndo)
        m_undoStack.erase(m_undoStack.begin(),
                          m_undoStack.begin() + (m_undoStack.size() - kMaxUndo));
    emit historyChanged();
}

void HistoryManager::undo() {
    emit historyAboutToChange();
    if (m_undoStack.empty()) return;
    auto cmd = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    cmd->undo();
    m_redoStack.push_back(std::move(cmd));
    emit historyChanged();
}

void HistoryManager::redo() {
    emit historyAboutToChange();
    if (m_redoStack.empty()) return;
    auto cmd = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    cmd->redo();
    m_undoStack.push_back(std::move(cmd));
    emit historyChanged();
}

void HistoryManager::goToIndex(int index) {
    const int total = static_cast<int>(m_undoStack.size() + m_redoStack.size());
    index = std::max(0, std::min(index, total));
    // currentIndex() == m_undoStack.size(); undo to shrink it, redo to grow it.
    while (static_cast<int>(m_undoStack.size()) > index) undo();
    while (static_cast<int>(m_undoStack.size()) < index) redo();
}

QString HistoryManager::undoDescription() const {
    if (m_undoStack.empty()) return QString();
    return m_undoStack.back()->description();
}

QString HistoryManager::redoDescription() const {
    if (m_redoStack.empty()) return QString();
    return m_redoStack.back()->description();
}

QStringList HistoryManager::historyList() const {
    QStringList list;
    for (const auto &cmd : m_undoStack)
        list.append(cmd->description());
    return list;
}

QStringList HistoryManager::fullHistoryList() const {
    QStringList list;
    for (const auto &cmd : m_undoStack)
        list.append(cmd->description());
    // Redo stack is in reverse chronological order (back() = next to redo).
    for (auto it = m_redoStack.rbegin(); it != m_redoStack.rend(); ++it)
        list.append((*it)->description());
    return list;
}

void HistoryManager::clear() {
    m_undoStack.clear();
    m_redoStack.clear();
    emit historyChanged();
}
