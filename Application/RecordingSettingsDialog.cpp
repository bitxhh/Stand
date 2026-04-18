#include "RecordingSettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

RecordingSettingsDialog::RecordingSettingsDialog(const RecordingSettings& initial,
                                                 QWidget* parent)
    : QDialog(parent)
    , initial_(initial)
{
    setWindowTitle(tr("Recording settings"));
    setModal(true);
    buildUi();
}

void RecordingSettingsDialog::buildUi() {
    auto* outer = new QVBoxLayout(this);

    // ── Output directory ────────────────────────────────────────────────────
    {
        auto* row  = new QHBoxLayout;
        auto* lbl  = new QLabel(tr("Output directory:"), this);
        dirEdit_   = new QLineEdit(initial_.outputDir, this);
        auto* btn  = new QPushButton(tr("Browse\u2026"), this);
        btn->setFixedWidth(90);

        row->addWidget(lbl);
        row->addWidget(dirEdit_, 1);
        row->addWidget(btn);
        outer->addLayout(row);

        connect(btn, &QPushButton::clicked, this, &RecordingSettingsDialog::browseDir);
    }

    outer->addSpacing(8);

    auto* header = new QLabel(tr("What to record:"), this);
    header->setStyleSheet("font-weight: 600;");
    outer->addWidget(header);

    // ── Raw per-channel ─────────────────────────────────────────────────────
    rawFormatCombo_ = new QComboBox(this);
    rawFormatCombo_->addItem(QStringLiteral(".cf32 (float32)"),
                             static_cast<int>(RecordingSettings::RawFormat::Float32));
    rawFormatCombo_->addItem(QStringLiteral(".cf64 (float64)"),
                             static_cast<int>(RecordingSettings::RawFormat::Float64));
    rawFormatCombo_->setCurrentIndex(static_cast<int>(initial_.rawFormat));

    rawPerChannelCheck_ = new QCheckBox(tr("Raw I/Q per channel (before combining)"), this);
    rawPerChannelCheck_->setChecked(initial_.recordRawPerChannel);

    combinedCheck_ = new QCheckBox(tr("Combined I/Q"), this);
    combinedCheck_->setChecked(initial_.recordCombined);

    filteredCheck_ = new QCheckBox(tr("Filtered I/Q (per demodulator)"), this);
    filteredCheck_->setChecked(initial_.recordFiltered);

    audioCheck_ = new QCheckBox(tr("Audio (demodulated)"), this);
    audioCheck_->setChecked(initial_.recordAudio);

    auto rowWithFormat = [this](QCheckBox* check, QWidget* right) {
        auto* host = new QWidget(this);
        auto* h    = new QHBoxLayout(host);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(check, 1);
        if (right) {
            auto* fmtLbl = new QLabel(tr("Format:"), host);
            h->addWidget(fmtLbl);
            h->addWidget(right);
        }
        return host;
    };

    outer->addWidget(rowWithFormat(rawPerChannelCheck_, rawFormatCombo_));
    outer->addWidget(rowWithFormat(combinedCheck_, nullptr));
    outer->addWidget(rowWithFormat(filteredCheck_, nullptr));
    outer->addWidget(rowWithFormat(audioCheck_,    nullptr));

    outer->addStretch();

    // ── OK / Cancel ─────────────────────────────────────────────────────────
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         Qt::Horizontal, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(buttons);
}

void RecordingSettingsDialog::browseDir() {
    const QString start = dirEdit_->text().isEmpty()
                              ? QDir::homePath()
                              : dirEdit_->text();
    const QString dir = QFileDialog::getExistingDirectory(this,
                            tr("Select output directory"), start);
    if (!dir.isEmpty())
        dirEdit_->setText(dir);
}

RecordingSettings RecordingSettingsDialog::settings() const {
    RecordingSettings out = initial_;
    out.outputDir           = dirEdit_->text();
    out.recordRawPerChannel = rawPerChannelCheck_->isChecked();
    out.recordCombined      = combinedCheck_->isChecked();
    out.recordFiltered      = filteredCheck_->isChecked();
    out.recordAudio         = audioCheck_->isChecked();
    out.rawFormat = static_cast<RecordingSettings::RawFormat>(
        rawFormatCombo_->currentData().toInt());
    return out;
}
