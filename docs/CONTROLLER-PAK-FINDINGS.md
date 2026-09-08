# The Controller Pak

Rayman 2 has no EEPROM, SRAM or Flash. Everything it saves -- the chosen
language, the slot names, the progress -- goes to a Controller Pak in the
controller's expansion slot. Until this work the port answered every enquiry
with "no pak", which is a legitimate console answer and which the game handles
gracefully: it shows "No Controller Pak found. The game will not be saved." and
plays on. That was the right placeholder and the wrong destination.

## Which layer to emulate

There were two places to put a Controller Pak, and the choice decided how much
had to be written.

The **high** seam is the `osPfs*` API. N64Recomp lists those in
`reimplemented_funcs`, so naming `osPfsInitPak` in `symbol_addrs.txt` swaps in
librecomp's implementation -- which answers `PFS_ERR_NOPACK` to everything. To
save through that seam the port would have to implement the pak *filesystem*:
the ID block and its two checksums, the inode table and its backup, the note
directory, allocation, the bank select. All of it exists already, compiled into
the cartridge.

The **low** seam is `__osSiRawStartDma`, the routine that moves the 64-byte PIF
RAM to and from the controller bus. That name is in `ignored_funcs` and *not* in
`reimplemented_funcs`, which means naming it drops the game's copy and hands the
job to the port with nothing standing in the way.

So: stop naming `osPfsInitPak`, and name `__osSiRawStartDma` instead. The game's
own libultra filesystem is recompiled and runs; the port answers two joybus
commands underneath it. The surface shrinks from a filesystem to a pair of block
transfers, and -- because the bytes are laid out by the code that shipped on the
cartridge -- what lands on disk is a real Controller Pak image rather than an
approximation of one. `controller_pak_1.pak` can be opened by anything else that
reads them.

This is the same layer the sibling Beetle Adventure Racing port emulates, for a
different reason: that game drives the SI directly and never calls `osPfs*` at
all. The wire format below is shared between them; the choice of seam is not.

## The wire, read out of this ROM

Rayman 2 packs its two commands in **two different shapes**, and a handler that
knows only one answers half the conversation. Both were read from the game's own
packing routines rather than assumed from libultra documentation.

`func_8000A790`, the status query, writes a **short** block -- no leading dummy
byte:

    offset 0     txsize    1
    offset 1     rxsize    3
    offset 2     cmd       0   (request status)
    offset 3-5             the three reply bytes

and `func_8000A820` reads the reply back as `type = reply[0] | (reply[1] << 8)`,
`status = reply[2]`, taking the channel error from the top two bits of rxsize.
So a standard controller is `05 00`, and bit 0 of the status byte is the "an
accessory is present" flag.

`func_80011A40`, the pak read, writes the **long** block libultra is usually
documented with:

    offset 0     dummy     0xFF
    offset 1     txsize    0x03      (0x23 for a write)
    offset 2     rxsize    0x21      (0x01 for a write)
    offset 3     cmd       2 read / 3 write
    offset 4-5   address   block << 5 | address CRC
    offset 6..   32 data bytes
    offset 0x26  data CRC

Reading the command byte at offset 3 -- correct for the long block -- finds 0xFF
in the short one. That is the whole reason `service_frame` matches on the
(txsize, rxsize, cmd) triple in both shapes rather than on the command alone.

The data CRC matters more than it looks. The game computes it over the 32 bytes
and compares it with the byte returned at 0x26, on **both** reads and writes,
and calls the transfer failed when they differ. There is more than one revision
of `__osContDataCrc` in the wild; the one in `src/si_pak.cpp` matches
`func_800117B8` in this ROM instruction for instruction.

## An empty pak is not a blank pak

A file of 32,768 zero bytes is not an empty Controller Pak, it is a broken one:
`osPfsInitPak` reads the ID block, verifies a checksum and its complement over
the first 28 bytes, and reports a fatal ID error when they do not agree. The
player's remedy for that is to format the pak on a console they do not have.

So a pak that has never been used is written out already formatted, exactly as
the console's own menu would leave it -- four copies of a valid ID block, an
inode table with every data page marked free, its backup, and an empty note
directory. `format_empty()` in `src/controller_pak.cpp` has the layout.

## Both halves post an SI completion

`__osPfsGetStatus` does `__osSiRawStartDma(OS_WRITE)`, `osRecvMesg`,
`__osSiRawStartDma(OS_READ)`, `osRecvMesg`. The transaction is served on the read
half, because that is when the game fetches the reply out of the same buffer it
wrote the request into -- but the completion has to be posted on **both**, or the
first `osRecvMesg` never returns and the game hangs holding the SI semaphore.
That is a deadlock rather than a wrong answer, which makes it the more expensive
mistake of the two.

## What it cost: the Rumble Pak

The controller has one accessory slot, and ultramodern's `osMotorInit` answers
`PFS_ERR_DEVICE` unless the reported pak is a Rumble Pak. Reporting a Controller
Pak therefore turns rumble off, exactly as taking the Rumble Pak out of a real
controller would. Nothing is lost against the previous state -- the port reported
no pak at all, so rumble was already inert -- but it is a real choice and not an
oversight. Serving both from the joybus layer is possible, because the pak data
area (below 0x8000) and the motor register (0xC000) do not overlap; it is a
deliberate deviation from hardware, and it was not needed for saving.

## Verified

`RAYMAN2_PAKTRACE=1` traces every transaction. From an empty configuration
directory:

  * the status query is answered "card present", and the "No Controller Pak
    found" screen is gone;
  * the game reads the ID block, the label and the inode page, writes the inode
    backup, and reaches the first-boot language screen -- which is what a console
    with a valid, empty pak shows;
  * choosing a language and starting a slot allocates a note and writes one data
    page. On disk, the note table entry carries game code `4E593245` -- "NY2E",
    this cartridge -- company code "41", start page 5, and the note name
    "RAYMAN2 AAA" in the N64 note-name encoding, "AAA" being the slot name
    offered at the prompt. The inode marks page 5 as a one-page file;
  * relaunching skips the language screen and reads the note table and page 5
    back. The game remembered.

Writes are flushed once a second from the event pump rather than from the game
thread, so a disk write never lands in the middle of a transfer the game is
timing, and again on the way out.
