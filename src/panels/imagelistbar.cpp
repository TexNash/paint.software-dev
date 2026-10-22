#include "imagelistbar.h"
#include "../i18n.h"
#include "core/document.h"

#include <QHBoxLayout>
#include <QToolButton>
#include <QPainter>
#include <QFileInfo>
#include <QMouseEvent>
#include <QEvent>
#include <QEnterEvent>

namespace {
constexpr int kThumb = 38;   // thumbnail button size
constexpr int kImage = 30;   // rendered image size inside it

class ImageThumbnailButton : public QToolButton {
public:
    explicit ImageThumbnailButton(QWidget *parent) : QToolButton(parent) {
        closeButton = new QToolButton(this);
        closeButton->setObjectName("imageCloseButton");
        closeButton->setText(QString::fromUtf8("×"));
        closeButton->setGeometry(kThumb - 17, 1, 16, 16);
        closeButton->setToolTip(TR("Fermer"));
        closeButton->setAccessibleName(TR("Fermer"));
        closeButton->setStyleSheet(
            "QToolButton { color: white; background: #555; border: 1px solid white;"
            " border-radius: 2px; padding: 0; font-size: 14px; }"
            "QToolButton:hover { background: #c42b1c; }"
            "QToolButton:pressed { background: #8f1f14; }");
        closeButton->hide();
    }

    QToolButton *closeButton;

protected:
    void enterEvent(QEnterEvent *event) override {
        closeButton->show();
        QToolButton::enterEvent(event);
    }
    void leaveEvent(QEvent *event) override {
        closeButton->hide();
        QToolButton::leaveEvent(event);
    }
};
}

ImageListBar::ImageListBar(QWidget *parent) : QWidget(parent) {
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(2, 1, 2, 1);
    m_layout->setSpacing(2);
}

void ImageListBar::setDocuments(const QVector<Document*> &docs, int activeIndex) {
    const bool sameDocuments = m_docs == docs && m_layout->count() == docs.size();
    m_docs = docs;
    m_active = activeIndex;
    if (sameDocuments) {
        for (int i = 0; i < m_docs.size(); ++i)
            if (auto *button = qobject_cast<QToolButton *>(m_layout->itemAt(i)->widget())) updateButton(button, m_docs[i], i);
    } else rebuild();
}

void ImageListBar::rebuild() {
    // Clear existing buttons.
    while (QLayoutItem *item = m_layout->takeAt(0)) {
        if (QWidget *w = item->widget()) w->deleteLater();
        delete item;
    }

    for (int i = 0; i < m_docs.size(); ++i) {
        Document *doc = m_docs[i];
        if (!doc) continue;

        auto *btn = new ImageThumbnailButton(this);
        btn->setFixedSize(kThumb, kThumb);
        btn->setCheckable(true);
        btn->setChecked(i == m_active);
        btn->setAutoRaise(true);

        updateButton(btn, doc, i);

        connect(btn, &QToolButton::clicked, this, [this, i]() { emit imageSelected(i); });
        btn->setProperty("imageIndex", i);
        btn->installEventFilter(this);   // middle-click closes the image
        btn->closeButton->setProperty("imageIndex", i);
        btn->closeButton->installEventFilter(this);
        connect(btn->closeButton, &QToolButton::clicked, this, [this, i]() { emit imageCloseRequested(i); });
        m_layout->addWidget(btn);
    }
}

void ImageListBar::updateButton(QToolButton *button, Document *document, int index) {
    if (!document) return;
    button->setChecked(index == m_active);
    // Render the flattened image onto a checkerboard, scaled to fit.
    QPixmap pm(kImage, kImage);
    pm.fill(Qt::transparent);
    {
        QPainter p(&pm);
        for (int y = 0; y < kImage; y += 6)
            for (int x = 0; x < kImage; x += 6)
                p.fillRect(x, y, 6, 6,
                           ((x / 6 + y / 6) % 2 == 0) ? QColor(255, 255, 255) : QColor(205, 205, 205));
        QImage thumb = document->thumbnail(QSize(kImage, kImage));
        p.drawImage((kImage - thumb.width()) / 2, (kImage - thumb.height()) / 2, thumb);
        p.setPen(QPen(QColor(120, 120, 120), 1));
        p.drawRect(0, 0, kImage - 1, kImage - 1);
    }
    button->setIcon(QIcon(pm));
    button->setIconSize(QSize(kImage, kImage));

    QString name = document->filePath().isEmpty() ? TR("Sans titre")
                                             : QFileInfo(document->filePath()).fileName();
    if (document->isModified()) name += " *";
    button->setToolTip(QString(TR("%1\n%2 × %3\n(clic milieu pour fermer)"))
                        .arg(name).arg(document->width()).arg(document->height()));

}

bool ImageListBar::eventFilter(QObject *obj, QEvent *event) {
    if (event->type() == QEvent::MouseButtonPress
        && static_cast<QMouseEvent *>(event)->button() == Qt::MiddleButton)
        return true;
    if (event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::MiddleButton) {
            bool ok = false;
            int index = obj->property("imageIndex").toInt(&ok);
            if (ok) {
                emit imageCloseRequested(index);
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}
