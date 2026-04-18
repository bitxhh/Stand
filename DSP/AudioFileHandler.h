#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <cstdint>
#include <cstdio>
#include <functional>

// ---------------------------------------------------------------------------
// AudioFileHandler — appends mono float32 audio to a WAV file.
//
// Listens to BaseDemodHandler::audioReady(QVector<float>, double) and writes
// incoming samples to a RIFF/WAVE IEEE-float PCM file. The output rate comes
// from the first push() and is frozen for the remainder of the session; if a
// subsequent block arrives at a different rate, it is discarded with a warning
// (re-opening mid-stream would corrupt the WAV header).
//
// Filename: because the audio sample rate is only known when the first block
// arrives, callers pass a `PathBuilder` closure that composes the final path
// from that rate. Example:
//   auto builder = [dir, ts, src, centerHz](double sr) {
//       return FileNaming::composeWithSuffix(dir, ts, src, "fm0",
//                                            centerHz, sr, ".wav");
//   };
// ---------------------------------------------------------------------------
class AudioFileHandler : public QObject {
    Q_OBJECT
public:
    using PathBuilder = std::function<QString(double sampleRateHz)>;

    explicit AudioFileHandler(PathBuilder builder, QObject* parent = nullptr);
    ~AudioFileHandler() override;

    // Flush WAV header with final sample count and close the file.
    void close();

    [[nodiscard]] bool    isOpen() const { return file_ != nullptr; }
    [[nodiscard]] QString path()   const { return path_; }

public slots:
    void push(QVector<float> samples, double sampleRateHz);

private:
    bool openFile(double sampleRateHz);
    void writeWavHeader(uint32_t sampleRate, uint32_t dataBytes);
    void patchWavHeader();

    PathBuilder builder_;
    QString     path_;
    FILE*       file_{nullptr};
    double      openedSampleRate_{0.0};
    uint64_t    samplesWritten_{0};
    bool        overflowLogged_{false};
};
