#pragma once

#include "../Core/RecordingSettings.h"

#include <QDialog>

class QLineEdit;
class QCheckBox;
class QComboBox;

// ---------------------------------------------------------------------------
// RecordingSettingsDialog — modal dialog exposing RecordingSettings fields:
//   • output directory (line edit + Browse button)
//   • what to record (4 checkboxes)
//   • raw format selector (.cf32 / .cf64)
//
// UI matches the mock in the v2 refactor plan. The caller supplies an initial
// RecordingSettings snapshot and retrieves the edited copy via settings()
// once exec() returns QDialog::Accepted.
// ---------------------------------------------------------------------------
class RecordingSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit RecordingSettingsDialog(const RecordingSettings& initial,
                                     QWidget* parent = nullptr);

    [[nodiscard]] RecordingSettings settings() const;

private:
    void buildUi();
    void browseDir();

    RecordingSettings  initial_;

    QLineEdit*         dirEdit_{nullptr};
    QCheckBox*         rawPerChannelCheck_{nullptr};
    QCheckBox*         combinedCheck_{nullptr};
    QCheckBox*         filteredCheck_{nullptr};
    QCheckBox*         audioCheck_{nullptr};
    QComboBox*         rawFormatCombo_{nullptr};
};
