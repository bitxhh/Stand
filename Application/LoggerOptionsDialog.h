#pragma once

#include <QCheckBox>
#include <QDialog>
#include <QList>

class LoggerOptionsDialog : public QDialog {
    Q_OBJECT
public:
    explicit LoggerOptionsDialog(QWidget* parent = nullptr);

private:
    void applyAndSave();
    QList<QCheckBox*> checkboxes_;
};
