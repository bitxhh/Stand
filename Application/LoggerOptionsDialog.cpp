#include "LoggerOptionsDialog.h"
#include "../Core/LoggerConfig.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QPushButton>
#include <QVBoxLayout>

LoggerOptionsDialog::LoggerOptionsDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Logger settings"));
    setMinimumWidth(300);

    auto* layout = new QVBoxLayout(this);
    auto* group  = new QGroupBox(tr("Parameters to log"), this);
    auto* form   = new QFormLayout(group);

    for (const auto& p : LoggerConfig::instance().allParams()) {
        auto* cb = new QCheckBox(this);
        cb->setProperty("paramKey", p.key);
        cb->setChecked(LoggerConfig::instance().isEnabled(p.key));
        checkboxes_.append(cb);
        form->addRow(p.label, cb);
    }

    layout->addWidget(group);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Close, this);
    layout->addWidget(buttons);

    connect(buttons->button(QDialogButtonBox::Save), &QPushButton::clicked,
            this, &LoggerOptionsDialog::applyAndSave);
    connect(buttons->button(QDialogButtonBox::Close), &QPushButton::clicked,
            this, &QDialog::accept);
}

void LoggerOptionsDialog::applyAndSave() {
    for (QCheckBox* cb : checkboxes_) {
        LoggerConfig::instance().setEnabled(
            cb->property("paramKey").toString(), cb->isChecked());
    }
    LoggerConfig::instance().save();
}
