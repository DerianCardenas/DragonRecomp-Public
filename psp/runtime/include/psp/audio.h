#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

namespace dragon::psp {

class PspAudio {
public:
    static constexpr int kMaxChannels = 8;
    static constexpr uint32_t kSampleRate = 44100U;

    PspAudio();
    void reset();

    int reserve_channel(int requested_ch, int sample_count, int format);
    int release_channel(int ch);
    int set_channel_sample_count(int ch, int sample_count);
    int set_channel_format(int ch, int format);
    int set_channel_volume(int ch, int vol_left, int vol_right);
    int get_channel_rest_len(int ch) const;

    // Submit stereo/mono PCM16 samples from guest memory; mixes into output ring buffer.
    int output_samples(int ch, int vol_left, int vol_right, const int16_t* pcm, int sample_count);

    // SRC channel (variable frequency)
    int reserve_src(int sample_count, int freq, int channels);
    int release_src();
    int output_src(int vol, const int16_t* pcm);

    // Pull mixed stereo PCM16 frames for SDL3 audio stream (fills with silence on underrun).
    void pop_stereo(int16_t* dst, size_t stereo_frames);
    size_t queued_frames() const;

private:
    struct Channel {
        bool reserved = false;
        int sample_count = 1024;
        int format = 0;  // 0 = stereo, 0x10 = mono
        int vol_left = 0x8000;
        int vol_right = 0x8000;
        int pending_samples = 0;
    };

    std::array<Channel, kMaxChannels> channels_{};
    bool src_reserved_ = false;
    int src_sample_count_ = 1024;
    int src_freq_ = 44100;
    int src_channels_ = 2;

    mutable std::mutex mutex_;
    std::vector<int16_t> ring_;
    size_t read_pos_ = 0;
    size_t write_pos_ = 0;
    size_t count_ = 0;  // interleaved int16 samples

    void push_mixed_stereo(const int16_t* pcm, int sample_count, bool mono, int vol_l, int vol_r);
};

}  // namespace dragon::psp
