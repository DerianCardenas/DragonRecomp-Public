#include "sms/opcodes.h"

#include <array>

namespace dragon::sms {

namespace {

// Base Z80 opcode table (0x00..0xFF). Prefix bytes (CB, DD, ED, FD) have length=0.
constexpr std::array<OpcodeInfo, 256> kBaseTable = {{
    // 0x00 - 0x0F
    {1, 4, 4},   {3, 10, 10}, {1, 7, 7},   {1, 6, 6},   {1, 4, 4},   {1, 4, 4},   {2, 7, 7},   {1, 4, 4},
    {1, 4, 4},   {1, 11, 11}, {1, 7, 7},   {1, 6, 6},   {1, 4, 4},   {1, 4, 4},   {2, 7, 7},   {1, 4, 4},
    // 0x10 - 0x1F
    {2, 8, 13},  {3, 10, 10}, {1, 7, 7},   {1, 6, 6},   {1, 4, 4},   {1, 4, 4},   {2, 7, 7},   {1, 4, 4},
    {2, 12, 12}, {1, 11, 11}, {1, 7, 7},   {1, 6, 6},   {1, 4, 4},   {1, 4, 4},   {2, 7, 7},   {1, 4, 4},
    // 0x20 - 0x2F
    {2, 7, 12},  {3, 10, 10}, {3, 16, 16}, {1, 6, 6},   {1, 4, 4},   {1, 4, 4},   {2, 7, 7},   {1, 4, 4},
    {2, 7, 12},  {1, 11, 11}, {3, 16, 16}, {1, 6, 6},   {1, 4, 4},   {1, 4, 4},   {2, 7, 7},   {1, 4, 4},
    // 0x30 - 0x3F
    {2, 7, 12},  {3, 10, 10}, {3, 13, 13}, {1, 6, 6},   {1, 11, 11}, {1, 11, 11}, {2, 10, 10}, {1, 4, 4},
    {2, 7, 12},  {1, 11, 11}, {3, 13, 13}, {1, 6, 6},   {1, 4, 4},   {1, 4, 4},   {2, 7, 7},   {1, 4, 4},
    // 0x40 - 0x4F
    {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 7, 7},   {1, 4, 4},
    {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 7, 7},   {1, 4, 4},
    // 0x50 - 0x5F
    {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 7, 7},   {1, 4, 4},
    {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 7, 7},   {1, 4, 4},
    // 0x60 - 0x6F
    {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 7, 7},   {1, 4, 4},
    {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 7, 7},   {1, 4, 4},
    // 0x70 - 0x7F (0x76 = HALT)
    {1, 7, 7},   {1, 7, 7},   {1, 7, 7},   {1, 7, 7},   {1, 7, 7},   {1, 7, 7},   {1, 4, 4},   {1, 7, 7},
    {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 7, 7},   {1, 4, 4},
    // 0x80 - 0x8F
    {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 7, 7},   {1, 4, 4},
    {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 7, 7},   {1, 4, 4},
    // 0x90 - 0x9F
    {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 7, 7},   {1, 4, 4},
    {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 7, 7},   {1, 4, 4},
    // 0xA0 - 0xAF
    {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 7, 7},   {1, 4, 4},
    {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 7, 7},   {1, 4, 4},
    // 0xB0 - 0xBF
    {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 7, 7},   {1, 4, 4},
    {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 4, 4},   {1, 7, 7},   {1, 4, 4},
    // 0xC0 - 0xCF (0xCB = prefix)
    {1, 5, 11},  {1, 10, 10}, {3, 10, 10}, {3, 10, 10}, {3, 10, 17}, {1, 11, 11}, {2, 7, 7},   {1, 11, 11},
    {1, 5, 11},  {1, 10, 10}, {3, 10, 10}, {0, 4, 4},   {3, 10, 17}, {3, 17, 17}, {2, 7, 7},   {1, 11, 11},
    // 0xD0 - 0xDF (0xDD = prefix)
    {1, 5, 11},  {1, 10, 10}, {3, 10, 10}, {2, 11, 11}, {3, 10, 17}, {1, 11, 11}, {2, 7, 7},   {1, 11, 11},
    {1, 5, 11},  {1, 4, 4},   {3, 10, 10}, {2, 11, 11}, {3, 10, 17}, {0, 4, 4},   {2, 7, 7},   {1, 11, 11},
    // 0xE0 - 0xEF (0xED = prefix)
    {1, 5, 11},  {1, 10, 10}, {3, 10, 10}, {1, 19, 19}, {3, 10, 17}, {1, 11, 11}, {2, 7, 7},   {1, 11, 11},
    {1, 5, 11},  {1, 4, 4},   {3, 10, 10}, {1, 4, 4},   {3, 10, 17}, {0, 4, 4},   {2, 7, 7},   {1, 11, 11},
    // 0xF0 - 0xFF (0xFD = prefix)
    {1, 5, 11},  {1, 10, 10}, {3, 10, 10}, {1, 4, 4},   {3, 10, 17}, {1, 11, 11}, {2, 7, 7},   {1, 11, 11},
    {1, 5, 11},  {1, 6, 6},   {3, 10, 10}, {1, 4, 4},   {3, 10, 17}, {0, 4, 4},   {2, 7, 7},   {1, 11, 11},
}};

constexpr std::array<OpcodeInfo, 256> make_cb_table() {
    std::array<OpcodeInfo, 256> t{};
    for (int i = 0; i < 256; ++i) {
        const bool is_hl = ((i & 0x07) == 6);
        const bool is_bit = ((i & 0xC0) == 0x40);
        const uint8_t cyc = is_hl ? (is_bit ? 12U : 15U) : 8U;
        t[static_cast<size_t>(i)] = {2, cyc, cyc};
    }
    return t;
}

constexpr std::array<OpcodeInfo, 256> make_ed_table() {
    std::array<OpcodeInfo, 256> t{};
    for (int i = 0; i < 256; ++i) {
        t[static_cast<size_t>(i)] = {2, 8, 8};  // Unassigned ED ops act as 2-byte 8-cycle NOP
    }
    // 0x40 - 0x7F
    for (int r = 0; r < 8; ++r) {
        t[0x40 + r * 8 + 0] = {2, 12, 12};  // IN r,(C)
        t[0x40 + r * 8 + 1] = {2, 12, 12};  // OUT (C),r
    }
    t[0x42] = t[0x52] = t[0x62] = t[0x72] = {2, 15, 15};  // SBC HL,rr
    t[0x4A] = t[0x5A] = t[0x6A] = t[0x7A] = {2, 15, 15};  // ADC HL,rr
    t[0x43] = t[0x53] = t[0x63] = t[0x73] = {4, 20, 20};  // LD (nn),rr
    t[0x4B] = t[0x5B] = t[0x6B] = t[0x7B] = {4, 20, 20};  // LD rr,(nn)
    t[0x44] = t[0x4C] = t[0x54] = t[0x5C] = t[0x64] = t[0x6C] = t[0x74] = t[0x7C] = {2, 8, 8};  // NEG
    t[0x45] = t[0x4D] = t[0x55] = t[0x5D] = t[0x65] = t[0x6D] = t[0x75] = t[0x7D] = {2, 14, 14}; // RETN/RETI
    t[0x46] = t[0x4E] = t[0x56] = t[0x5E] = t[0x66] = t[0x6E] = t[0x76] = t[0x7E] = {2, 8, 8};   // IM
    t[0x47] = t[0x4F] = {2, 9, 9};   // LD I,A / LD R,A
    t[0x57] = t[0x5F] = {2, 9, 9};   // LD A,I / LD A,R
    t[0x67] = t[0x6F] = {2, 18, 18}; // RRD / RLD

    // Block operations 0xA0..0xBB
    t[0xA0] = t[0xA8] = {2, 16, 16};  // LDI / LDD
    t[0xA1] = t[0xA9] = {2, 16, 16};  // CPI / CPD
    t[0xA2] = t[0xAA] = {2, 16, 16};  // INI / IND
    t[0xA3] = t[0xAB] = {2, 16, 16};  // OUTI / OUTD
    t[0xB0] = t[0xB8] = {2, 16, 21};  // LDIR / LDDR
    t[0xB1] = t[0xB9] = {2, 16, 21};  // CPIR / CPDR
    t[0xB2] = t[0xBA] = {2, 16, 21};  // INIR / INDR
    t[0xB3] = t[0xBB] = {2, 16, 21};  // OTIR / OTDR
    return t;
}

constexpr auto kCbTable = make_cb_table();
constexpr auto kEdTable = make_ed_table();

}  // namespace

const OpcodeInfo& opcode_info_base(uint8_t op) noexcept {
    return kBaseTable[op];
}

const OpcodeInfo& opcode_info_cb(uint8_t op) noexcept {
    return kCbTable[op];
}

const OpcodeInfo& opcode_info_ed(uint8_t op) noexcept {
    return kEdTable[op];
}

OpcodeInfo opcode_info_index(uint8_t op) noexcept {
    if (op == 0xCB) {
        return {4, 23, 23};  // DD/FD CB <d> <op> (BIT is 20 cycles, handled in interpreter)
    }
    switch (op) {
        case 0x09:
        case 0x19:
        case 0x29:
        case 0x39:
            return {2, 15, 15};  // ADD IX/IY, rr
        case 0x21:
            return {4, 14, 14};  // LD IX/IY, nn
        case 0x22:
        case 0x2A:
            return {4, 20, 20};  // LD (nn), IX/IY / LD IX/IY, (nn)
        case 0x23:
        case 0x2B:
            return {2, 10, 10};  // INC/DEC IX/IY
        case 0x24:
        case 0x25:
        case 0x2C:
        case 0x2D:
            return {2, 8, 8};    // INC/DEC IXH/IXL
        case 0x26:
        case 0x2E:
            return {3, 11, 11};  // LD IXH/IXL, n
        case 0x34:
        case 0x35:
            return {3, 23, 23};  // INC/DEC (IX+d)
        case 0x36:
            return {4, 19, 19};  // LD (IX+d), n
        case 0xE1:
            return {2, 14, 14};  // POP IX/IY
        case 0xE3:
            return {2, 23, 23};  // EX (SP), IX/IY
        case 0xE5:
            return {2, 15, 15};  // PUSH IX/IY
        case 0xE9:
            return {2, 8, 8};    // JP (IX/IY)
        case 0xF9:
            return {2, 10, 10};  // LD SP, IX/IY
        default:
            break;
    }
    // Uses (IX+d) when dst or src is 6 (excluding 0x76 HALT)
    const bool is_ld_block = (op >= 0x40 && op <= 0x7F && op != 0x76);
    const bool is_alu_block = (op >= 0x80 && op <= 0xBF);
    if ((is_ld_block && (((op >> 3) & 7) == 6 || (op & 7) == 6)) ||
        (is_alu_block && (op & 7) == 6)) {
        return {3, 19, 19};
    }
    // Half-index register ops (IXH/IXL) or fallback to base op + 4 prefix T-cycles
    const OpcodeInfo base = kBaseTable[op];
    return {static_cast<uint8_t>(base.length + 1U),
            static_cast<uint8_t>(base.cycles + 4U),
            static_cast<uint8_t>(base.cycles_taken + 4U)};
}

}  // namespace dragon::sms
