#pragma once
#include <QDialog>
class QImage;

// Rejected (including Escape/window close) is deliberately the Cancel outcome.
class PasteDialog : public QDialog {
public:
    enum Choice { Cancel = QDialog::Rejected, Expand = QDialog::Accepted, Keep = 2 };
    explicit PasteDialog(const QImage &image, QWidget *parent = nullptr);
};
