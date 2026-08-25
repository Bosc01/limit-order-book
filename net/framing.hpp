#pragma once
#include <cstdint>
#include <cstring>
#include <functional>

#include "protocol.hpp"

namespace lobnet {

// TCP delivers a byte stream, not messages: one recv() can hold half a
// frame, three frames, or a frame and a half. This class owns that
// reassembly, and NOTHING else -- no sockets -- so the parser can be unit
// tested and fuzzed by feeding bytes in adversarial chunkings.
//
// Security posture: the peer is untrusted. A frame longer than
// kMaxFrameLen, a zero-length frame, or a buffer overflow attempt is a
// protocol violation -- the connection is poisoned and the caller must drop
// it (that is what real exchange gateways do; there is no "resync" on a
// corrupted binary stream).
class FrameBuffer {
public:
    static constexpr std::size_t kCapacity = 4096; // frames are <= 64 + 2 bytes

    // Feed raw bytes; invokes sink(body, len) for each complete frame, where
    // body starts at the type byte. Returns false on protocol violation
    // (caller should disconnect the peer). After false, the buffer is
    // poisoned: every later call also returns false.
    bool feed(const std::uint8_t* data, std::size_t n,
              const std::function<void(const std::uint8_t*, std::size_t)>& sink) {
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

            // If the buffer is still full after draining, the peer sent a
            // frame that cannot fit -- with kCapacity >> max frame size this
            // only happens on garbage input.
            if (fill_ == kCapacity) {
                poisoned_ = true;
                return false;
            }
        }
        return true;
    }

private:
    bool drain(const std::function<void(const std::uint8_t*, std::size_t)>& sink) {
        std::size_t pos = 0;
        while (fill_ - pos >= kHeaderLen) {
            std::uint16_t len;
            std::memcpy(&len, buf_ + pos, sizeof(len));
            if (len == 0 || len > kMaxFrameLen) { // attacker-controlled length
                poisoned_ = true;
                return false;
            }
            if (fill_ - pos < kHeaderLen + len) break; // incomplete: wait for more
            sink(buf_ + pos + kHeaderLen, len);
            pos += kHeaderLen + len;
        }
        // Shift the partial remainder to the front. Frames are tiny, so this
        // memmove is a few dozen bytes at worst.
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
