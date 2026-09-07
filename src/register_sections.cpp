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

// Register the two sections the runtime cannot place by itself.
//
// librecomp fills its address -> function map in exactly one place at startup:
// init() calls load_overlays(0x1000, entrypoint, 1 MB) to model the IPL3 boot
// DMA. That helper derives each section's RAM address from its ROM offset by
// assuming the whole window landed contiguously at the entrypoint --
// section.rom_addr - 0x1000 + 0x80000400 -- which is true of the boot section
// and of nothing else here:
//
//   boot  rom 0x001000 -> 0x80000400   correct
//   main  rom 0x01DCC0 -> 0x8001D0C0   wrong; it lives at 0x80025C50
//   aux   rom 0x0C5BF0 -> 0x800C4FF0   wrong; it lives at 0x800F64A0
//
// Those two are not overlays. The game DMAs them itself, once, from its boot
// thread, to fixed addresses, and they stay there -- so nothing in librecomp's
// overlay machinery ever corrects the guess.
//
// The symptom was quiet and late. Direct calls between recompiled functions are
// ordinary C calls and do not consult the map at all, so the entire main
// segment ran correctly right up to the first *indirect* call into it, which
// died on "Failed to find function at 0x800AF120" -- an address that was in the
// table all along, filed under 0x800A6590 because of the offset above.
//
// Registering by the declared RAM address fixes it. Doing so before the game
// has actually copied the bytes is not a problem and not a race: the map holds
// native function pointers, which exist from link time; the RDRAM contents it
// is keyed against are irrelevant to the lookup.
extern "C" void rayman2_register_static_sections() {
    load_overlays(0x0001DCC0, 0x80025C50, 0x000A7F30);   // main
    load_overlays(0x000C5BF0, 0x800F64A0, 0x0000AE30);   // aux
}
