#include "RawFileHandler.h"
#include "Logger.h"

RawFileHandler::RawFileHandler(const QString& path, Format format)
    : path_(path)
    , format_(format)
{}

RawFileHandler::~RawFileHandler() {
    onStreamStopped();
}

void RawFileHandler::onStreamStarted(double /*sampleRateHz*/) {
    file_.open(path_.toStdString(), std::ios::binary);
    if (!file_.is_open()) {
        LOG_ERROR("RawFileHandler: cannot open: " + path_.toStdString());
        return;
    }
    LOG_INFO(std::string("RawFileHandler: writing ")
             + (format_ == Format::Float64 ? "float64" : "float32")
             + " to " + path_.toStdString());
}

void RawFileHandler::onStreamStopped() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
        LOG_INFO("RawFileHandler: closed " + path_.toStdString());
    }
    promoteBuf_.clear();
    promoteBuf_.shrink_to_fit();
}

void RawFileHandler::processBlock(const float* iq, int count, double /*sampleRateHz*/) {
    if (!file_.is_open() || count <= 0) return;

    const std::size_t n = static_cast<std::size_t>(count) * 2;

    if (format_ == Format::Float32) {
        file_.write(reinterpret_cast<const char*>(iq),
                    static_cast<std::streamsize>(n * sizeof(float)));
        return;
    }

    // Float64: promote to double before writing.
    if (promoteBuf_.size() < n) promoteBuf_.resize(n);
    for (std::size_t i = 0; i < n; ++i)
        promoteBuf_[i] = static_cast<double>(iq[i]);
    file_.write(reinterpret_cast<const char*>(promoteBuf_.data()),
                static_cast<std::streamsize>(n * sizeof(double)));
}
