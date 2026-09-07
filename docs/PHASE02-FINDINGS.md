# Phase 02 findings — first recompile

**Gate met.** N64Recomp translates all **4,580** functions with no errors, and
the generated C compiles and archives cleanly: **66 objects, a 5.2 MB static
library, 3,267 exported functions** from ~20 MB of generated C.

```bash
scripts/build-recompiler.sh    # build N64Recomp + RSPRecomp from lib/
scripts/refine-syms.sh         # recover call targets splat missed (converges)
scripts/recompile.sh           # -> RecompiledFuncs/*.c
scripts/build-recompiled-lib.sh # the gate
```

## The theme of this phase

Every defect here was **silent**. Not one announced itself as "the symbol table
is wrong"; each surfaced as a puzzling downstream symptom, and in one case as no
symptom at all. That is what the plan meant by a wrong boundary producing code
that compiles and then corrupts state — the difference being that these were
caught by a gate rather than by a player.

## 1. The one that produced no error at all

After the first clean-looking run, N64Recomp reported `Function count: 4465` and
wrote **one** function. No error, no warning.

The cause was in our own `macro.inc`. splat announces every label with
`nonmatching <name>, <size>`, and for a function that size is its only
appearance anywhere in the output — `enddlabel` closes data labels, but nothing
closes a function. We had stubbed `nonmatching` as a no-op, so **every function
had `st_size == 0`**. N64Recomp emits a function only when its section words are
non-empty, derives those from the symbol size, and so quietly emitted nothing
but the entrypoint.

Making the macro emit `.size` fixed it. The lesson is not about the macro: a
pipeline stage that produces 1 of 4,465 outputs and exits successfully is a
pipeline stage with no output check, so `scripts/build-recompiled-lib.sh` now
reports counts at every step.

## 2. Absolute symbols silently shadowing real ones

`func_800FD9C4` is defined in `.aux`, called from `.main`, and N64Recomp
insisted "No function found for jal target" while `readelf` showed it present.

It was present as `FUNC GLOBAL **ABS**`. splat's `undefined_funcs_auto.txt`
emits `NAME = 0xADDR;` for references it cannot resolve within a segment, and a
linker-script assignment defines the symbol as *absolute*. Where the symbol is
also defined properly by another object — which is every cross-segment call —
the absolute definition wins and detaches it from its section.

**Nothing looks wrong when this happens.** The address is identical either way,
so the link succeeds and the bytes still verify byte-identical against the ROM.
Only a consumer that resolves symbols *by section* notices, and N64Recomp is
one. `tools/gen_link_syms.py` now emits an assignment only for a symbol no
object defines: **116 were suppressed**, which is 116 cross-segment calls that
were quietly broken.

Byte-identity passing throughout is worth dwelling on. Phase 01's gate is a good
gate, and it was not sufficient here.

## 3. Data decoded as code

With `disassemble_all`, splat puts a segment's data — jump tables, float pools,
pointer tables — in the same `.text` as the code, labelled with `glabel` exactly
as functions are. N64Recomp then decodes tables as instructions. It fails loudly
when the bytes are an unimplementable encoding (`D_800C8B30` decoded as `mfc0`
from a reserved register; `func_800C6FB4` was a single word that spimdisasm had
already flagged `/* invalid instruction */`), and produces wrong code silently
when they decode cleanly.

Two layers now handle it:

- **The split.** Each segment's trailing data is declared as a `data`
  subsegment. The boundaries were found by locating each segment's last
  `jr $ra` and confirming **zero function prologues after it**:

  | Segment | Code ends (ROM) | Trailing data |
  |---|---|---|
  | boot | `0x01A860` | 13.1 KB |
  | main | `0x0BEE10` | 27.5 KB |
  | aux | `0x0D0100` | 2.3 KB |

- **The ignore list.** `tools/gen_ignored_syms.py` keys on splat's naming
  convention (`D_<addr>` is data) and feeds N64Recomp's `ignored`. It must
  select on the *data* names, never on "not `func_`" — the inverse rule sweeps
  up hand-verified names like `osGetCount` and makes the recompiler reject the
  config outright.

## 4. A function split in two

`func_8000C944` branched backwards to `0x8000C8FC`, before its own start.
splat's control-flow analysis had cut one routine in half: the preceding
function was sized `0x574` and ended exactly where this one began, and
`0x574 + 0xF0 = 0x664` is the real extent. Declaring that size in
`recomp/symbol_addrs.txt` merges them.

Large handwritten assembly is where splat's heuristics are weakest, and this is
the failure mode the plan named as most expensive. It is worth noting that
N64Recomp caught it — a backwards branch out of a function is not something the
recompiler can express, so it refused rather than guessing.

## 5. libultra, identified rather than guessed

Phase 01 deferred libultra identification for want of a reference SDK build to
byte-match against. It turns out much of it can be read straight off the
instruction encodings, which is stronger evidence than a byte match anyway.

Eleven functions are now named in `recomp/symbol_addrs.txt`, each from its body:

- **Cache routines** from the MIPS cache-op encodings (low two bits select I or
  D cache, bits 4:2 the operation) and the size each compares against —
  `0x4000` is the VR4300's 16 KB icache, `0x2000` its 8 KB dcache. That
  separates `osInvalICache`, `osInvalDCache` (main loop `Hit_Invalidate_D`, with
  `Hit_Writeback_Invalidate_D` on the unaligned head and tail lines),
  `osWritebackDCache` (`Hit_Writeback_D`) and `osWritebackDCacheAll`.
- **Interrupt and register access** from cop0 register numbers: 9 is Count
  (`osGetCount`), 11 Compare (`__osSetCompare`), 12 Status (`__osGetSR`,
  `__osSetSR`, `__osDisableInt`, `__osRestoreInt`), and `cfc1`/`ctc1 $31` is
  the FPU control register (`osSetFpcCsr`).

Naming matters beyond readability: N64Recomp recognises these names and either
hands them to librecomp (`reimplemented_funcs`) or drops them
(`ignored_funcs`). Naming is the mechanism, not documentation.

Where a body did **not** prove an identity, the function kept its generated name
and was stubbed instead. Guessing a libultra name would silently redirect calls
to a runtime function with different semantics, and nothing would flag it.

## 6. What is stubbed, and why that is defensible

| Stub | Evidence | Why stubbing is sound |
|---|---|---|
| `func_80003D40`, `func_8000CF50`, `func_80014930` | cop0 EntryHi/EntryLo/Index, `tlbwi`/`tlbp` | TLB manipulation. The port has no MMU — librecomp addresses RDRAM flatly — and phase 00 established the game has no TLB-mapped code. |
| `func_8000C3D0` | reads cop0 Cause, saves/restores FPU control, indirect tail call | An exception handler. A recompiled port has no CPU exceptions to service. |
| `func_8000CB94` | writes cop0 EPC and Status; `func_8000CA34` tail-calls it | The matching return-from-exception path. |

## 7. Three functions the port owes the runtime

N64Recomp's `ignored_funcs` are dropped on the assumption that *something*
defines them. For `reimplemented_funcs` that something is librecomp; for
`ignored_funcs` it is nobody, and the port must supply them. Compiling the
generated C found exactly three that Rayman 2 actually calls:
`__osGetSR_recomp`, `__osSetSR_recomp`, `__osSetCompare_recomp`.

`include/port_runtime.h` declares them and is force-included (`-include`),
because generated sources must never be hand-edited. **Phase 03 must define
them**, and the definitions are not free choices — `__osSetCompare` schedules
the next counter interrupt, so it has to agree with however the VI and timer
are wired, rather than simply being dropped. The header records the reasoning.

## Carried forward

- **`__osGetSR` stubbed to 0** claims interrupts are disabled and no status bits
  are set. The routines the game relies on for critical sections
  (`__osDisableInt` / `__osRestoreInt`) are reimplemented by librecomp rather
  than stubbed, so this should not matter — but it is the first suspect if early
  execution goes astray in phase 04.
- **`__FILE__` partitioning** is still unexploited. 3,267 functions now carry
  generated names; the assert anchors would give most of them a module.
- **The `aux` tail** (`0x2A0` between the code end and the next ROM data) is
  still classified by `disassemble_all` rather than established.
