#include "psp/audio.h"

#include <algorithm>
#include <cstring>

namespace dragon::psp {

namespace {
constexpr size_t kRingCapacitySamples = 44100U * 2U;  // 1.0 sec stereo ring buffer
}

PspAudio::PspAudio() : ring_(kRingCapacitySamples, 0) {}

void PspAudio::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& ch : channels_)
        ch = Channel{};
    src_reserved_ = false;
    read_pos_ = 0;
    write_pos_ = 0;
    count_ = 0;
    std::fill(ring_.begin(), ring_.end(), int16_t{0});
}

int PspAudio::reserve_channel(int requested_ch, int sample_count, int format) {
    int ch = requested_ch;
    if (ch < 0) {
        for (int i = 0; i < kMaxChannels; ++i) {
            if (!channels_[static_cast<size_t>(i)].reserved) {
                ch = i;
                break;
            }
        }
    }
    if (ch < 0 || ch >= kMaxChannels)
        return -1;
    auto& slot = channels_[static_cast<size_t>(ch)];
    slot.reserved = true;
    slot.sample_count = (sample_count > 0) ? sample_count : 1024;
    slot.format = format;
    slot.vol_left = 0x8000;
    slot.vol_right = 0x8000;
    slot.pending_samples = 0;
    return ch;
}

int PspAudio::release_channel(int ch) {
    if (ch < 0 || ch >= kMaxChannels)
        return -1;
    channels_[static_cast<size_t>(ch)].reserved = false;
    return 0;
}

int PspAudio::set_channel_sample_count(int ch, int sample_count) {
    if (ch < 0 || ch >= kMaxChannels)
        return -1;
    if (sample_count > 0)
        channels_[static_cast<size_t>(ch)].sample_count = sample_count;
    return 0;
}

int PspAudio::set_channel_format(int ch, int format) {
    if (ch < 0 || ch >= kMaxChannels)
        return -1;
    channels_[static_cast<size_t>(ch)].format = format;
    return 0;
}

int PspAudio::set_channel_volume(int ch, int vol_left, int vol_right) {
    if (ch < 0 || ch >= kMaxChannels)
        return -1;
    auto& slot = channels_[static_cast<size_t>(ch)];
    if (vol_left >= 0)
        slot.vol_left = vol_left;
    if (vol_right >= 0)
        slot.vol_right = vol_right;
    return 0;
}

int PspAudio::get_channel_rest_len(int ch) const {
    if (ch < 0 || ch >= kMaxChannels)
        return 0;
    return channels_[static_cast<size_t>(ch)].pending_samples;
}

int PspAudio::output_samples(int ch, int vol_left, int vol_right, const int16_t* pcm,
                             int sample_count) {
    if (ch < 0 || ch >= kMaxChannels || !pcm)
        return -1;
    auto& slot = channels_[static_cast<size_t>(ch)];
    if (!slot.reserved)
        slot.reserved = true;
    const int n = (sample_count > 0) ? sample_count : slot.sample_count;
    const int vl = (vol_left >= 0) ? vol_left : slot.vol_left;
    const int vr = (vol_right >= 0) ? vol_right : slot.vol_right;
    const bool mono = (slot.format == 0x10);
    push_mixed_stereo(pcm, n, mono, vl, vr);
    return n;
}

int PspAudio::reserve_src(int sample_count, int freq, int channels) {
    src_reserved_ = true;
    src_sample_count_ = (sample_count > 0) ? sample_count : 1024;
    src_freq_ = (freq > 0) ? freq : 44100;
    src_channels_ = (channels == 1) ? 1 : 2;
    return 0;
}

int PspAudio::release_src() {
    src_reserved_ = false;
    return 0;
}

int PspAudio::output_src(int vol, const int16_t* pcm) {
    if (!src_reserved_ || !pcm)
        return -1;
    push_mixed_stereo(pcm, src_sample_count_, src_channels_ == 1, vol, vol);
    return src_sample_count_;
}

void PspAudio::push_mixed_stereo(const int16_t* pcm, int sample_count, bool mono, int vol_l,
                                 int vol_r) {
    if (sample_count <= 0)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < sample_count; ++i) {
        const int32_t raw_l = mono ? pcm[i] : pcm[i * 2];
        const int32_t raw_r = mono ? pcm[i] : pcm[i * 2 + 1];
        const int32_t scaled_l = std::clamp((raw_l * vol_l) >> 15, -32768, 32767);
        const int32_t scaled_r = std::clamp((raw_r * vol_r) >> 15, -32768, 32767);

        if (count_ + 2U > kRingCapacitySamples) {
            // Drop oldest stereo frame if ring is full
            read_pos_ = (read_pos_ + 2U) % kRingCapacitySamples;
            count_ -= 2U;
        }
        ring_[write_pos_] = static_cast<int16_t>(scaled_l);
        write_pos_ = (write_pos_ + 1U) % kRingCapacitySamples;
        ring_[write_pos_] = static_cast<int16_t>(scaled_r);
        write_pos_ = (write_pos_ + 1U) % kRingCapacitySamples;
        count_ += 2U;
    }
}

void PspAudio::pop_stereo(int16_t* dst, size_t stereo_frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t samples_wanted = stereo_frames * 2U;
    const size_t avail = std::min(count_, samples_wanted);
    for (size_t i = 0; i < avail; ++i) {
        dst[i] = ring_[read_pos_];
        read_pos_ = (read_pos_ + 1U) % kRingCapacitySamples;
    }
    count_ -= avail;
    if (avail < samples_wanted) {
        std::memset(dst + avail, 0, (samples_wanted - avail) * sizeof(int16_t));
    }
}

size_t PspAudio::queued_frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_ / 2U;
}

}  // namespace dragon::psp
