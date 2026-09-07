// Hand the recompiler's section tables to librecomp.
//
// N64Recomp emits recomp_overlays.inl describing where each code section came
// from in the ROM, where it lives in RDRAM, and which recompiled function
// covers each offset. librecomp needs that to turn a game-side address into a
// function pointer -- which is how any indirect call, jump table or function
// pointer in the recompiled code resolves.
//
// For this game the table is short and, notably, carries no relocations:
//
//   rom 0x001000 -> ram 0x80000400  size 0x01CCC0   boot
//   rom 0x01DCC0 -> ram 0x80025C50  size 0x0A7F30   main
//   rom 0x0C5BF0 -> ram 0x800F64A0  size 0x00AE30   aux
//
// That is the phase 01 segment map, arrived at independently by the recompiler,
// and `num_relocs = 0` on every entry is the recompiler agreeing that nothing
// here is relocatable. The overlay-index table is a single -1 because the game
// has no overlays at all, so the sibling Beetle port's module bridge has no
// counterpart here.

#include "librecomp/overlays.hpp"
#include "librecomp/sections.h"

// Defines section_table, num_sections and overlay_sections_by_index.
#include "recomp_overlays.inl"

void rayman2_register_sections() {
    recomp::overlays::overlay_section_table_data_t sections{};
    sections.code_sections     = section_table;
    sections.num_code_sections = ARRLEN(section_table);
    sections.total_num_sections = num_sections;

    recomp::overlays::overlays_by_index_t overlays{};
    overlays.table = overlay_sections_by_index;
    overlays.len   = ARRLEN(overlay_sections_by_index);

    recomp::overlays::register_overlays(sections, overlays);
}
