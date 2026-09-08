// The Controller Pak's backing store: 32 KiB per port, persisted to disk.
//
// See include/controller_pak.h for how this sits under the joybus layer in
// src/si_pak.cpp and the game's own recompiled PFS above that.
//
// The one piece of real N64 knowledge in this file is format_empty(). A pak
// that is all zeroes is not an empty pak -- it is a broken one. libultra's
// osPfsInitPak reads the ID block, verifies two checksums over it, and returns
// PFS_ERR_ID_FATAL if they do not agree; a game then has nothing to offer the
// player except a message telling them to format the pak on a console they do
// not have. So a pak that has never been used is written out already formatted,
// exactly as the console's own memory-card menu would leave it.

#include "controller_pak.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::mutex g_mutex;
fs::path g_dir;
std::array<std::vector<uint8_t>, 4> g_pak;
std::array<bool, 4> g_loaded{};
std::array<bool, 4> g_dirty{};

// libultra's __osSumcalc: a plain 16-bit sum of the bytes. Used for the inode
// page's own checksum, which lives in the low byte of its first entry.
uint16_t sum_bytes(const uint8_t* p, int len) {
    uint32_t sum = 0;
    for (int i = 0; i < len; i++) {
        sum = (sum + p[i]) & 0xFFFF;
    }
    return static_cast<uint16_t>(sum);
}

// Write a valid formatted-empty Controller Pak: one bank, 128 pages, every data
// page free, no notes.
//
// The layout, from libultra's os_pfs.h:
//
//     page 0    blocks 1, 3, 4 and 6   four copies of the 32-byte ID block
//               block 7                the label, which may be blank
//     page 1    the inode table (the FAT): 128 big-endian entries
//     page 2    its backup copy, byte for byte
//     pages 3-4 the note table: 16 directory entries, all empty here
//     pages 5+  the data pages
//
// The ID block is checksummed twice, and both have to agree or the pak reads as
// fatally broken: a straight 16-bit sum of the first 28 bytes as big-endian
// halfwords, and the sum of their complements. Four copies exist because the
// pak is battery-backed and libultra will repair the block from whichever copy
// still verifies.
//
// The free-page marker is 3, and the first data page is 5 -- one ID page, two
// note-table pages, and two inode pages (one per bank, doubled for the backup).
void format_empty(std::vector<uint8_t>& m) {
    m.assign(rayman2::pak::kPakSize, 0);

    uint8_t id[32] = {};
    id[0x18] = 0x00;
    id[0x19] = 0x01;   // deviceid: bit 0 set marks the pak valid
    id[0x1A] = 0x01;   // banks: one, so 32 KiB
    id[0x1B] = 0x00;   // version

    uint16_t checksum = 0;
    uint16_t inverted = 0;
    for (int i = 0; i < 0x1C; i += 2) {
        const uint16_t halfword = static_cast<uint16_t>((id[i] << 8) | id[i + 1]);
        checksum += halfword;
        inverted += static_cast<uint16_t>(~halfword);
    }
    id[0x1C] = static_cast<uint8_t>(checksum >> 8);
    id[0x1D] = static_cast<uint8_t>(checksum);
    id[0x1E] = static_cast<uint8_t>(inverted >> 8);
    id[0x1F] = static_cast<uint8_t>(inverted);

    for (int block : {1, 3, 4, 6}) {
        std::memcpy(&m[block * 32], id, sizeof(id));
    }

    uint8_t inode[256] = {};
    for (int page = 5; page < 128; page++) {
        inode[page * 2 + 0] = 0x00;
        inode[page * 2 + 1] = 0x03;   // free
    }
    // Entry 0 is not a page: its low byte carries the checksum of the entries
    // that follow the first data page.
    inode[0] = 0x00;
    inode[1] = static_cast<uint8_t>(sum_bytes(&inode[5 * 2], (128 - 5) * 2) & 0xFF);

    std::memcpy(&m[1 * 256], inode, sizeof(inode));
    std::memcpy(&m[2 * 256], inode, sizeof(inode));
}

fs::path pak_path(int port) {
    return g_dir / ("controller_pak_" + std::to_string(port + 1) + ".pak");
}

// Caller holds g_mutex.
void ensure_loaded(int port) {
    if (g_loaded[port]) {
        return;
    }

    bool read_from_disk = false;
    if (!g_dir.empty()) {
        std::error_code ec;
        if (fs::exists(pak_path(port), ec)) {
            std::ifstream in(pak_path(port), std::ios::binary);
            if (in) {
                g_pak[port].assign(rayman2::pak::kPakSize, 0);
                in.read(reinterpret_cast<char*>(g_pak[port].data()), rayman2::pak::kPakSize);
                // A short file is a truncated write from a previous run, not a
                // pak. Formatting over it loses nothing that was readable.
                read_from_disk = (in.gcount() == rayman2::pak::kPakSize);
            }
        }
    }

    if (!read_from_disk) {
        format_empty(g_pak[port]);
        if (!g_dir.empty()) {
            std::ofstream out(pak_path(port), std::ios::binary | std::ios::trunc);
            if (out) {
                out.write(reinterpret_cast<const char*>(g_pak[port].data()), rayman2::pak::kPakSize);
                std::fprintf(stderr, "[rayman2] created Controller Pak \"%s\"\n",
                             pak_path(port).string().c_str());
            }
        }
    }

    g_loaded[port] = true;
    g_dirty[port] = false;
}

} // namespace

namespace rayman2::pak {

void set_storage_directory(const std::filesystem::path& dir) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_dir = dir;
}

bool present(int port) {
    return port == 0;
}

bool read_block(int port, int block, uint8_t out[kBlockSize]) {
    if (port < 0 || port >= 4 || block < 0 || block >= kBlockCount) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    ensure_loaded(port);
    std::memcpy(out, &g_pak[port][block * kBlockSize], kBlockSize);
    return true;
}

bool write_block(int port, int block, const uint8_t in[kBlockSize]) {
    if (port < 0 || port >= 4 || block < 0 || block >= kBlockCount) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    ensure_loaded(port);
    if (std::memcmp(&g_pak[port][block * kBlockSize], in, kBlockSize) != 0) {
        std::memcpy(&g_pak[port][block * kBlockSize], in, kBlockSize);
        g_dirty[port] = true;
    }
    return true;
}

void flush() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_dir.empty()) {
        return;
    }
    for (int port = 0; port < 4; port++) {
        if (!g_loaded[port] || !g_dirty[port]) {
            continue;
        }
        std::ofstream out(pak_path(port), std::ios::binary | std::ios::trunc);
        if (out) {
            out.write(reinterpret_cast<const char*>(g_pak[port].data()), kPakSize);
            g_dirty[port] = false;
        }
    }
}

} // namespace rayman2::pak
