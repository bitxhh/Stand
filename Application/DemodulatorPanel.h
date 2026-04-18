#pragma once

#include <QWidget>
#include <QString>

class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QSlider;
class QLabel;
class QPushButton;
class BaseDemodHandler;
class FmAudioOutput;
class CombinedRxController;
class BandpassHandler;
class AudioFileHandler;

class DemodulatorPanel : public QWidget {
    Q_OBJECT

public:
    explicit DemodulatorPanel(int slotIndex, QWidget* parent = nullptr);
    ~DemodulatorPanel() override;

    void attachToController(CombinedRxController* ctrl);
    void detachFromController();

    void onStreamStarted();
    void onStreamStopped();
    void updateMetrics();

    void setCenterFreqMHz(double mhz);
    void setSampleRateHz(double sr);
    void tuneToMHz(double mhz);
    [[nodiscard]] double vfoFreqMHz() const;
    [[nodiscard]] int slotIndex() const { return slotIndex_; }
    [[nodiscard]] QString currentMode() const;
    [[nodiscard]] double currentBwMHz() const;

    // Supplies everything needed to build recording filenames. Called by
    // RadioMonitorPage at stream start / settings change.
    //   dir            — output directory (empty disables recording)
    //   timestamp      — session prefix (YYYYMMDD_HHMMSS)
    //   combinedSource — rx0 / dualrx / triplerx / quadrorx
    //   centerFreqHz   — LO at time of session
    //   filteredAllowed/audioAllowed — master enables from RecordingSettings
    void setRecordingContext(const QString& dir,
                             const QString& timestamp,
                             const QString& combinedSource,
                             double         centerFreqHz,
                             bool           filteredAllowed,
                             bool           audioAllowed);

signals:
    void removeRequested(int slotIndex);
    void vfoChanged(int slotIndex, double freqMHz, double bwMHz);

private:
    void onModeChanged(int index);
    void applyDemod();
    void teardownDemod();
    void buildUi();
    void emitVfoChanged();

    void updateFilteredRecording();
    void updateAudioRecording();
    void teardownFilteredRecording();
    void teardownAudioRecording();
    [[nodiscard]] bool recordingDirValid() const;

    int slotIndex_;
    double centerFreqMHz_{102.0};
    double sampleRateHz_{0.0};
    CombinedRxController* ctrl_{nullptr};

    BaseDemodHandler* demodHandler_{nullptr};
    FmAudioOutput* audioOut_{nullptr};
    float volume_{0.8f};

    QComboBox*      modeCombo_{nullptr};
    QDoubleSpinBox* vfoSpin_{nullptr};
    QSlider*        volumeSlider_{nullptr};
    QLabel*         volumeLabel_{nullptr};
    QLabel*         statusLabel_{nullptr};
    QLabel*         levelLabel_{nullptr};
    QPushButton*    removeButton_{nullptr};

    QLabel*         fmBwLabel_{nullptr};
    QDoubleSpinBox* fmBwSpin_{nullptr};
    QLabel*         fmDeemphLabel_{nullptr};
    QComboBox*      fmDeemphCombo_{nullptr};

    QLabel*         amBwLabel_{nullptr};
    QDoubleSpinBox* amBwSpin_{nullptr};

    // ── Recording ───────────────────────────────────────────────────────────
    QCheckBox*        filteredCheck_{nullptr};
    QCheckBox*        audioCheck_   {nullptr};
    BandpassHandler*  filteredHandler_{nullptr};   // owned, attached via addExtraHandler
    AudioFileHandler* audioHandler_   {nullptr};   // owned
    QString           recordingDir_;
    QString           recordingTimestamp_;
    QString           combinedSource_;
    double            recordingCenterHz_{0.0};
    bool              filteredAllowed_{false};
    bool              audioAllowed_   {false};
};
