#pragma once

#include <QString>
#include <QVector>
#include <variant>

namespace demod {

// ---------------------------------------------------------------------------
// Parameter descriptors — metadata for UI auto-build.
//
// SpinParam  → QDoubleSpinBox.  internal_value = UI_value * scale.
// ComboParam → QComboBox.       internal_value = selected option's value.
// ---------------------------------------------------------------------------

struct SpinParam {
    QString name;
    double  min;
    double  max;
    double  defaultVal;    // in UI units (e.g. kHz)
    QString suffix;        // e.g. " kHz"
    double  step;
    double  scale{1.0};    // UI_value * scale = internal_value (Hz, etc.)
};

struct ComboParam {
    QString name;
    struct Option { QString label; double value; };
    QVector<Option> options;
    int defaultIndex{0};
};

using ParamDesc = std::variant<SpinParam, ComboParam>;

} // namespace demod
