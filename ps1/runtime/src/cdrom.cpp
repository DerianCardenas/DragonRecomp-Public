#include "ps1/cdrom.h"

#include "ps1/loader.h"

namespace dragon::ps1 {

namespace {
inline uint8_t bcd_to_bin(uint8_t b) noexcept {
    return static_cast<uint8_t>(((b >> 4U) * 10U) + (b & 0x0FU));
}
}  // namespace

Ps1Cdrom::Ps1Cdrom() : data_buf_(2048U, 0U) {
    reset();
}

void Ps1Cdrom::reset() {
    index_ = 0;
    irq_enable_ = 0;
    irq_flags_ = 0;
    mode_ = 0;
    stat_ = 0x02;
    setloc_lba_ = 0;
    read_lba_ = 0;
    reading_ = false;
    param_fifo_.clear();
    resp_fifo_.clear();
    data_pos_ = data_buf_.size();
}

void Ps1Cdrom::push_response(uint8_t b) noexcept {
    if (resp_fifo_.size() < 16U)
        resp_fifo_.push_back(b);
}

void Ps1Cdrom::trigger_irq(uint8_t code) noexcept {
    irq_flags_ = (irq_flags_ & 0xE0U) | (code & 0x1FU);
}

void Ps1Cdrom::load_current_sector() noexcept {
    if (image_ && image_->has_disc()) {
        image_->read_sector_2048(read_lba_, data_buf_.data());
    } else {
        std::fill(data_buf_.begin(), data_buf_.end(), uint8_t{0});
    }
    data_pos_ = 0U;
    ++read_lba_;
}

uint8_t Ps1Cdrom::read8(uint32_t addr) noexcept {
    const uint32_t port = addr & 3U;
    if (port == 0U) {
        uint8_t status = index_ & 3U;
        if (param_fifo_.empty())
            status |= (1U << 3U);  // PRMEMPT
        if (param_fifo_.size() < 16U)
            status |= (1U << 4U);  // PRMWRDY
        if (!resp_fifo_.empty())
            status |= (1U << 5U);  // RSLRRDY
        if (data_pos_ < data_buf_.size())
            status |= (1U << 6U);  // DRQSTS
        return status;
    }
    if (port == 1U) {
        if (resp_fifo_.empty())
            return 0U;
        const uint8_t v = resp_fifo_.front();
        resp_fifo_.pop_front();
        return v;
    }
    if (port == 2U) {
        if (data_pos_ < data_buf_.size())
            return data_buf_[data_pos_++];
        return 0U;
    }
    if (port == 3U) {
        if (index_ == 0U || index_ == 2U)
            return static_cast<uint8_t>(0xE0U | irq_enable_);
        return static_cast<uint8_t>(0xE0U | (irq_flags_ & 0x1FU));
    }
    return 0U;
}

void Ps1Cdrom::write8(uint32_t addr, uint8_t val) noexcept {
    const uint32_t port = addr & 3U;
    if (port == 0U) {
        index_ = val & 3U;
        return;
    }
    if (port == 1U) {
        if (index_ == 0U)
            execute_command(val);
        return;
    }
    if (port == 2U) {
        if (index_ == 0U) {
            if (param_fifo_.size() < 16U)
                param_fifo_.push_back(val);
        } else if (index_ == 1U) {
            irq_enable_ = val & 0x1FU;
        }
        return;
    }
    if (port == 3U) {
        if (index_ == 0U) {
            if ((val & 0x80U) != 0U && reading_ && data_pos_ >= data_buf_.size()) {
                load_current_sector();
            }
        } else if (index_ == 1U) {
            irq_flags_ &= static_cast<uint8_t>(~(val & 0x1FU));
            if ((val & 0x40U) != 0U)
                param_fifo_.clear();
            if (reading_ && (irq_flags_ & 7U) == 0U) {
                load_current_sector();
                push_response(stat_);
                trigger_irq(1U);  // INT1 data ready
            }
        }
    }
}

uint32_t Ps1Cdrom::dma_read32() noexcept {
    uint32_t w = 0U;
    for (uint32_t i = 0; i < 4U; ++i) {
        const uint8_t b = (data_pos_ < data_buf_.size()) ? data_buf_[data_pos_++] : 0U;
        w |= static_cast<uint32_t>(b) << (i * 8U);
    }
    return w;
}

void Ps1Cdrom::execute_command(uint8_t cmd) noexcept {
    resp_fifo_.clear();
    switch (cmd) {
        case 0x01:  // Getstat
            push_response(stat_);
            trigger_irq(3U);
            break;
        case 0x02: {  // Setloc(mm, ss, ff) in BCD
            if (param_fifo_.size() >= 3U) {
                const uint32_t mm = bcd_to_bin(param_fifo_[0]);
                const uint32_t ss = bcd_to_bin(param_fifo_[1]);
                const uint32_t ff = bcd_to_bin(param_fifo_[2]);
                const uint32_t total = (mm * 60U + ss) * 75U + ff;
                setloc_lba_ = (total >= 150U) ? (total - 150U) : 0U;
            }
            param_fifo_.clear();
            push_response(stat_);
            trigger_irq(3U);
            break;
        }
        case 0x06:  // ReadN
        case 0x1B:  // ReadS
            read_lba_ = setloc_lba_;
            reading_ = true;
            stat_ = 0x22;  // Motor + Reading
            load_current_sector();
            push_response(stat_);
            trigger_irq(1U);
            break;
        case 0x08:  // Stop
        case 0x09:  // Pause
            reading_ = false;
            stat_ = 0x02;
            push_response(stat_);
            trigger_irq(2U);
            break;
        case 0x0A:  // Init
            reading_ = false;
            mode_ = 0;
            stat_ = 0x02;
            push_response(stat_);
            trigger_irq(2U);
            break;
        case 0x0E:  // Setmode
            if (!param_fifo_.empty()) {
                mode_ = param_fifo_.front();
                param_fifo_.pop_front();
            }
            push_response(stat_);
            trigger_irq(3U);
            break;
        case 0x15:  // SeekL
            read_lba_ = setloc_lba_;
            push_response(stat_);
            trigger_irq(2U);
            break;
        case 0x19:  // Test
            if (!param_fifo_.empty() && param_fifo_.front() == 0x20U) {
                // Version date (YY, MM, DD, Ver)
                push_response(0x95);
                push_response(0x05);
                push_response(0x16);
                push_response(0xC1);
            } else {
                push_response(stat_);
            }
            param_fifo_.clear();
            trigger_irq(3U);
            break;
        case 0x1A:  // GetID
            push_response(0x02);
            push_response(0x00);
            push_response(0x20);
            push_response(0x00);
            push_response('S');
            push_response('C');
            push_response('E');
            push_response('A');
            trigger_irq(2U);
            break;
        default:
            param_fifo_.clear();
            push_response(stat_);
            trigger_irq(3U);
            break;
    }
}

}  // namespace dragon::ps1
