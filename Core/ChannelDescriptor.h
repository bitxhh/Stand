#pragma once

// ---------------------------------------------------------------------------
// ChannelDescriptor — identifies a single stream endpoint on an SDR device.
//
// direction    — RX (receive) or TX (transmit)
// channelIndex — hardware channel index (0 or 1 for LimeSDR)
//
// Used by IDevice channel-aware methods, StreamWorker, and BlockMeta.
// ---------------------------------------------------------------------------
struct ChannelDescriptor {
    enum Direction { RX, TX };
    Direction direction   { RX };
    int       channelIndex{ 0  };

    bool operator==(const ChannelDescriptor& o) const noexcept {
        return direction == o.direction && channelIndex == o.channelIndex;
    }
    bool operator<(const ChannelDescriptor& o) const noexcept {
        if (direction != o.direction) return direction < o.direction;
        return channelIndex < o.channelIndex;
    }
};
