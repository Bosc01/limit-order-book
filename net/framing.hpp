#pragma once
#include <cstdint>
#include <cstring>

#include "protocol.hpp"

namespace lobnet {

// Reassembles frames from a TCP byte stream. No sockets, so it can be fed
// arbitrary chunkings in tests. A zero or over-long length prefix poisons
// the buffer: there is no resynchronizing a binary stream, so the caller
// drops the connection.
class FrameBuffer {
public:
    static constexpr std::size_t kCapacity = 4096;
    // After drain() at most one partial frame remains, so the buffer can
    // never fill with an undeliverable frame.
    static_assert(kCapacity >= kHeaderLen + kMaxFrameLen);

    // Calls sink(body, len) for each complete frame, body starting at the
    // type byte. Returns false on a protocol violation; after that every
    // call returns false.
    template <class Sink>
    bool feed(const std::uint8_t* data, std::size_t n, Sink&& sink) {
        if (poisoned_) return false;
        while (n > 0) {
            // Top up the internal buffer.
            const std::size_t space = kCapacity - fill_;
            const std::size_t take  = n < space ? n : space;
            std::memcpy(buf_ + fill_, data, take);
            fill_ += take;
            data  += take;
            n     -= take;

            if (!drain(sink)) return false;
        }
        return true;
    }

private:
    template <class Sink>
    bool drain(Sink& sink) {
        std::size_t pos = 0;
        while (fill_ - pos >= kHeaderLen) {
            std::uint16_t len;
            std::memcpy(&len, buf_ + pos, sizeof(len));
            if (len == 0 || len > kMaxFrameLen) {
                poisoned_ = true;
                return false;
            }
            if (fill_ - pos < kHeaderLen + len) break; // incomplete: wait for more
            sink(buf_ + pos + kHeaderLen, len);
            pos += kHeaderLen + len;
        }
        // At most kHeaderLen + kMaxFrameLen - 1 bytes move.
        if (pos > 0) {
            std::memmove(buf_, buf_ + pos, fill_ - pos);
            fill_ -= pos;
        }
        return true;
    }

    std::uint8_t buf_[kCapacity];
    std::size_t  fill_     = 0;
    bool         poisoned_ = false;
};

} // namespace lobnet
