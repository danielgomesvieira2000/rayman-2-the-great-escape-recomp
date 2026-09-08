// The joybus wire: __osSiRawStartDma, and the Controller Pak transactions on it.
//
// This is the layer the game's own Controller Pak filesystem stands on. Nothing
// here knows what a note or an inode is; it knows two commands, read a block
// and write a block, plus the status query that says an accessory is there at
// all. Everything above -- the ID checksums, the free-page table, the note
// directory, the bank select -- is the cartridge's own recompiled libultra,
// which is why recomp/symbol_addrs.txt deliberately leaves osPfsInitPak
// unnamed. Everything below is a flat 32 KiB of bytes in
// src/controller_pak.cpp.
//
// Why the port has to supply this function at all: __osSiRawStartDma is in
// N64Recomp's `ignored_funcs` and not in `reimplemented_funcs`, so the
// recompiler drops the game's copy and librecomp does not replace it. That is
// the correct division -- the original writes to SI_DRAM_ADDR and the PIF RAM
// address registers, which no recompilation can carry over -- and it is also
// the seam the port wants, because the SI is where a Controller Pak actually
// lives.
//
// The wire format was read out of this ROM rather than assumed, because the two
// commands the game uses are packed in two different shapes:
//
//   The status query (func_8000A790) writes a SHORT block with no leading dummy
//   byte -- txsize at 0, rxsize at 1, cmd at 2 -- and its reply is three bytes
//   at 3, 4 and 5. func_8000A820 reads them back as
//   type = reply[0] | (reply[1] << 8), status = reply[2], and takes the channel
//   error from the top two bits of rxsize.
//
//   The pak read (func_80011A40) writes the LONG block libultra is usually
//   documented with -- dummy 0xFF at 0, txsize at 1, rxsize at 2, cmd at 3, the
//   address halfword at 4, thirty-two data bytes at 6, and the data CRC at
//   0x26. The pak write is the same shape with txsize 0x23 and rxsize 1.
//
// Reading offset 3 for the command byte, which works for the long block, finds
// 0xFF in the short one. A handler that only knows one of the two shapes
// answers half the conversation and the game concludes the pak is unusable.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "recomp.h"
#include "ultramodern/ultramodern.hpp"

#include "controller_pak.h"

namespace {

// Joybus commands. Only these three ever reach a Controller Pak.
constexpr unsigned kCmdRequestStatus = 0;
constexpr unsigned kCmdReadPak       = 2;
constexpr unsigned kCmdWritePak      = 3;

// The controller reports its type in the status reply. 0x0005 is a standard
// controller; the status byte beside it is a bitfield whose bit 0 (CONT_CARD_ON)
// means an accessory is present.
constexpr unsigned kContTypeNormal = 0x0005;
constexpr unsigned kContCardOn     = 0x01;

// The pak's address space, in 32-byte blocks. Blocks below 0x400 are the 32 KiB
// of storage. Block 0x400 (address 0x8000) is the accessory identify and bank
// select register, which behaves as memory: whatever is written there reads
// back, which is exactly what libultra's __osPfsSelectBank expects.
constexpr int kBlockDetect = 0x400;

uint8_t g_detect_cell[4][rayman2::pak::kBlockSize] = {};

bool trace_enabled() {
    static const bool on = std::getenv("RAYMAN2_PAKTRACE") != nullptr;
    return on;
}

// __osContDataCrc, matching func_800117B8 in this ROM instruction for
// instruction: a bitwise CRC over 33 iterations, the last of which shifts in
// nothing. The game compares its own result against the byte returned here on
// both reads and writes, and calls the transfer failed if they differ, so this
// has to be the same variant the cartridge shipped with rather than any other
// libultra revision's.
uint8_t data_crc(const uint8_t* data) {
    uint8_t crc = 0;
    for (int i = 0; i <= 32; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            const uint8_t xor_term = (crc & 0x80) ? 0x85 : 0x00;
            crc <<= 1;
            if (i != 32) {
                crc |= (data[i] >> bit) & 1;
            }
            crc ^= xor_term;
        }
    }
    return crc;
}

// Answer one READ_PAK or WRITE_PAK block. `base` is the sign-extended address of
// the block within PIF RAM, so the offsets below are the long-block layout.
void handle_pak_block(uint8_t* rdram, int64_t base, int port, unsigned cmd) {
    const unsigned address = (static_cast<unsigned>(MEM_BU(4, base)) << 8) |
                              static_cast<unsigned>(MEM_BU(5, base));
    // The low five bits of the address halfword are its CRC, not address bits.
    const int block = static_cast<int>(address >> 5);
    const bool is_write = (cmd == kCmdWritePak);

    uint8_t data[rayman2::pak::kBlockSize];
    if (is_write) {
        for (int i = 0; i < rayman2::pak::kBlockSize; i++) {
            data[i] = static_cast<uint8_t>(MEM_BU(6 + i, base));
        }
    }
    else {
        std::memset(data, 0, sizeof(data));
    }

    if (block == kBlockDetect) {
        // Identify / bank select. libultra writes a byte here and reads it back
        // to decide the pak is present and to choose a bank; a cell that simply
        // remembers what it was given satisfies both.
        if (is_write) {
            std::memcpy(g_detect_cell[port], data, sizeof(data));
        }
        else {
            std::memcpy(data, g_detect_cell[port], sizeof(data));
        }
    }
    else if (block < kBlockDetect) {
        if (is_write) {
            rayman2::pak::write_block(port, block, data);
        }
        else {
            rayman2::pak::read_block(port, block, data);
        }
    }
    // Anything above the identify register is unmapped on a Controller Pak --
    // 0xC000 is the Rumble Pak's motor, and this port presents a Controller Pak
    // rather than both. A read of it returns zeroes, which is what a slot with
    // no motor in it gives.

    if (!is_write) {
        for (int i = 0; i < rayman2::pak::kBlockSize; i++) {
            MEM_B(6 + i, base) = static_cast<int8_t>(data[i]);
        }
    }
    // Both directions echo the data CRC; the game checks it either way.
    MEM_B(0x26, base) = static_cast<int8_t>(data_crc(data));

    if (trace_enabled()) {
        std::fprintf(stderr, "[rayman2] pak %s port=%d block=0x%03X data=%02X%02X%02X%02X\n",
                     is_write ? "write" : "read ", port, block,
                     data[0], data[1], data[2], data[3]);
    }
}

// Fill in the reply to a short-format status query at `base`.
void handle_status(uint8_t* rdram, int64_t base, int port) {
    const bool has_pak = rayman2::pak::present(port);

    MEM_B(3, base) = static_cast<int8_t>(kContTypeNormal & 0xFF);          // type, low byte
    MEM_B(4, base) = static_cast<int8_t>((kContTypeNormal >> 8) & 0xFF);   // type, high byte
    MEM_B(5, base) = static_cast<int8_t>(has_pak ? kContCardOn : 0x00);

    if (trace_enabled()) {
        std::fprintf(stderr, "[rayman2] pak status port=%d -> %s\n",
                     port, has_pak ? "card present" : "empty slot");
    }
}

// Walk PIF RAM looking for the one command block the game just packed.
//
// A pak transaction is single-channel: `channel` filler bytes of zero, then one
// block. Requiring the filler to be zero is what makes the search unambiguous,
// because a block at channel 2 and a block at channel 0 whose first bytes
// happen to look like a command are otherwise indistinguishable.
bool service_frame(uint8_t* rdram, int64_t pif) {
    for (int channel = 0; channel < 4; channel++) {
        const int64_t base = pif + channel;

        bool filler_ok = true;
        for (int i = 0; i < channel; i++) {
            if (static_cast<unsigned>(MEM_BU(0, pif + i)) != 0x00) {
                filler_ok = false;
                break;
            }
        }
        if (!filler_ok) {
            break;
        }

        // Short block: no dummy byte. Only the status query uses this shape.
        const unsigned short_tx  = static_cast<unsigned>(MEM_BU(0, base));
        const unsigned short_rx  = static_cast<unsigned>(MEM_BU(1, base));
        const unsigned short_cmd = static_cast<unsigned>(MEM_BU(2, base));
        if (short_tx == 0x01 && short_rx == 0x03 && short_cmd == kCmdRequestStatus) {
            handle_status(rdram, base, channel);
            return true;
        }

        // Long block: dummy, then the command triple.
        const unsigned dummy = static_cast<unsigned>(MEM_BU(0, base));
        const unsigned tx    = static_cast<unsigned>(MEM_BU(1, base));
        const unsigned rx    = static_cast<unsigned>(MEM_BU(2, base));
        const unsigned cmd   = static_cast<unsigned>(MEM_BU(3, base));
        if (dummy == 0xFF &&
            ((cmd == kCmdReadPak  && tx == 0x03 && rx == 0x21) ||
             (cmd == kCmdWritePak && tx == 0x23 && rx == 0x01))) {
            handle_pak_block(rdram, base, channel, cmd);
            return true;
        }
    }
    return false;
}

} // namespace

extern "C" void __osSiRawStartDma_recomp(uint8_t* rdram, recomp_context* ctx) {
    const int32_t direction = static_cast<int32_t>(ctx->r4);   // OS_READ 0, OS_WRITE 1
    // MEM_* subtract a sign-extended KSEG0 base, so the address has to arrive
    // sign-extended too. A plain uint32_t 0x800255B0 zero-extends and turns the
    // subtraction into a 4 GiB out-of-bounds offset.
    const int64_t pif = static_cast<int32_t>(ctx->r5);

    // Only touch the buffer when it is a plausible RDRAM address. The game hands
    // this function a pointer it built itself, and a bad one should not become
    // an access violation inside the runtime.
    const uint32_t linear = static_cast<uint32_t>(ctx->r5) - 0x80000000u;
    const bool usable = (linear < 0x00800000u);

    // Serve the transaction on the read half. The game writes the request,
    // waits, then reads the reply back out of the same buffer, so answering
    // here means the reply is in place exactly when it is fetched, and a write
    // that is never followed by a read costs nothing.
    if (direction == 0 && usable) {
        if (!service_frame(rdram, pif) && trace_enabled()) {
            std::fprintf(stderr, "[rayman2] pak: unrecognised SI frame:");
            for (int i = 0; i < 12; i++) {
                std::fprintf(stderr, " %02X", static_cast<unsigned>(MEM_BU(0, pif + i)));
            }
            std::fprintf(stderr, "\n");
        }
    }

    // Post the SI completion on BOTH halves. The caller blocks on its queue
    // after each of the two transfers, so a missing completion is not a wrong
    // answer -- it is a hang, with the SI semaphore still held.
    ultramodern::send_si_message();

    ctx->r2 = 0;   // $v0: success
}
