#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

namespace dragon::ps1 {

struct LoadedPs1Image;

class Ps1Cdrom {
public:
    Ps1Cdrom();
    void reset();
    void attach_image(const LoadedPs1Image* img) noexcept { image_ = img; }

    uint8_t read8(uint32_t addr) noexcept;
    void write8(uint32_t addr, uint8_t val) noexcept;
    uint32_t dma_read32() noexcept;

    bool has_pending_irq() const noexcept { return (irq_flags_ & irq_enable_ & 0x1FU) != 0U; }

private:
    void execute_command(uint8_t cmd) noexcept;
    void push_response(uint8_t b) noexcept;
    void trigger_irq(uint8_t code) noexcept;
    void load_current_sector() noexcept;

    const LoadedPs1Image* image_ = nullptr;
    uint8_t index_ = 0;
    uint8_t irq_enable_ = 0;
    uint8_t irq_flags_ = 0;
    uint8_t mode_ = 0;
    uint8_t stat_ = 0x02;  // Motor on

    uint32_t setloc_lba_ = 0;
    uint32_t read_lba_ = 0;
    bool reading_ = false;

    std::deque<uint8_t> param_fifo_;
    std::deque<uint8_t> resp_fifo_;
    std::vector<uint8_t> data_buf_;
    size_t data_pos_ = 0;
};

}  // namespace dragon::ps1
