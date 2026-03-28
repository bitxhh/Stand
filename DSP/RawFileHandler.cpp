#include "RawFileHandler.h"
#include "Logger.h"

RawFileHandler::RawFileHandler(const QString& path)
    : path_(path)
{}

RawFileHandler::~RawFileHandler() {
    onStreamStopped();
}

void RawFileHandler::onStreamStarted(double /*sampleRateHz*/) {
    file_.open(path_.toStdString(), std::ios::binary);
    if (!file_.is_open())
        LOG_ERROR("RawFileHandler: cannot open: " + path_.toStdString());
    else
        LOG_INFO("RawFileHandler: writing to " + path_.toStdString());
}

void RawFileHandler::onStreamStopped() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
        LOG_INFO("RawFileHandler: closed " + path_.toStdString());
    }
}

void RawFileHandler::processBlock(const int16_t* iq, int count, double /*sampleRateHz*/) {
    if (file_.is_open())
        file_.write(reinterpret_cast<const char*>(iq),
                    static_cast<std::streamsize>(count) * 2 * sizeof(int16_t));
}
