#!/usr/bin/env python3
"""Record, and compare, exactly what the recompilation pipeline consumed and produced.

The pipeline turns a ROM into C through several stages, most of whose outputs
are git-ignored build artifacts. That is the right call -- they are derived from
the builder's own cartridge -- but it leaves a gap: if two runs disagree, there
is nothing to diff, and no way to tell whether the difference came from an input
that changed or from the pipeline itself being non-deterministic.

Phase 04 is a bisecting exercise. Bisecting requires that the same inputs give
the same outputs, and requires being *told* when they do not. So every run
writes a manifest of hashes, and a later run can check itself against it.

    python tools/manifest.py write            # record the current state
    python tools/manifest.py check            # compare against the record
    python tools/manifest.py check --update   # compare, then re-record

`check` exits non-zero if anything an input did not explain has changed, and
prints what moved. Inputs are listed separately from outputs precisely so the
distinction is visible: outputs changing while inputs did not is the interesting
case, and the one that cost this project a known-good build once already.
"""
import hashlib
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "recomp" / "pipeline.manifest.json"

# Things a human edits, or that the cartridge supplies. If one of these moved,
# the outputs are expected to move with it.
INPUT_FILES = [
    "rom.z64",
    "recomp/rayman2.us.yaml",
    "recomp/rayman2.us.toml",
    "recomp/symbol_addrs.txt",
    "recomp/macro.inc",
]

# Things the pipeline derives. Two of these are also fed back in on the next
# split (auto_funcs.txt is read by splat; ignored_syms.txt is spliced into the
# recompiler config), which is why they are tracked rather than assumed inert.
DERIVED_FILES = [
    "recomp/auto_funcs.txt",
    "recomp/ignored_syms.txt",
    "recomp/undefined_syms_auto.txt",
    "recomp/undefined_funcs_auto.txt",
    "recomp/rayman2.us.ld",
    "elf/rayman2.us.elf",
]

# Whole trees, hashed as one value over their sorted contents.
OUTPUT_TREES = ["asm", "RecompiledFuncs"]


def hash_file(path: pathlib.Path) -> str | None:
    if not path.is_file():
        return None
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()[:16]


def hash_tree(path: pathlib.Path) -> tuple[str | None, int]:
    """One hash over a directory: every file's relative path and content."""
    if not path.is_dir():
        return None, 0
    files = sorted(p for p in path.rglob("*") if p.is_file())
    h = hashlib.sha256()
    for p in files:
        h.update(str(p.relative_to(path)).replace("\\", "/").encode())
        h.update(b"\0")
        h.update(hash_file(p).encode())
        h.update(b"\0")
    return h.hexdigest()[:16], len(files)


def collect() -> dict:
    state = {"inputs": {}, "derived": {}, "outputs": {}}
    for rel in INPUT_FILES:
        state["inputs"][rel] = hash_file(ROOT / rel)
    for rel in DERIVED_FILES:
        state["derived"][rel] = hash_file(ROOT / rel)
    for rel in OUTPUT_TREES:
        digest, count = hash_tree(ROOT / rel)
        state["outputs"][rel] = {"hash": digest, "files": count}
    return state


def render(state: dict) -> None:
    for section in ("inputs", "derived"):
        print(f"  [{section}]")
        for rel, digest in state[section].items():
            print(f"    {digest or '(absent)':16}  {rel}")
    print("  [outputs]")
    for rel, info in state["outputs"].items():
        print(f"    {info['hash'] or '(absent)':16}  {rel}  ({info['files']} files)")


def diff(old: dict, new: dict) -> tuple[list[str], list[str]]:
    changed_inputs, changed_outputs = [], []
    for rel in INPUT_FILES:
        if old.get("inputs", {}).get(rel) != new["inputs"][rel]:
            changed_inputs.append(rel)
    for rel in DERIVED_FILES:
        if old.get("derived", {}).get(rel) != new["derived"][rel]:
            changed_outputs.append(rel)
    for rel in OUTPUT_TREES:
        o = (old.get("outputs", {}).get(rel) or {}).get("hash")
        n = new["outputs"][rel]["hash"]
        if o != n:
            changed_outputs.append(rel)
    return changed_inputs, changed_outputs


def main() -> int:
    action = sys.argv[1] if len(sys.argv) > 1 else "write"
    update = "--update" in sys.argv
    state = collect()

    if action == "write":
        MANIFEST.parent.mkdir(parents=True, exist_ok=True)
        MANIFEST.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8", newline="\n")
        print(f"wrote {MANIFEST.relative_to(ROOT)}")
        render(state)
        return 0

    if action != "check":
        raise SystemExit(f"usage: {sys.argv[0]} [write|check] [--update]")

    if not MANIFEST.is_file():
        print(f"no manifest at {MANIFEST.relative_to(ROOT)}; run 'write' first.")
        render(state)
        return 0

    old = json.loads(MANIFEST.read_text(encoding="utf-8"))
    changed_inputs, changed_outputs = diff(old, state)

    if not changed_inputs and not changed_outputs:
        print("REPRODUCIBLE: every input and output matches the recorded manifest.")
        return 0

    if changed_inputs:
        print("inputs changed (outputs are expected to follow):")
        for rel in changed_inputs:
            print(f"    {rel}")
    if changed_outputs:
        print("outputs changed:")
        for rel in changed_outputs:
            print(f"    {rel}")

    if changed_outputs and not changed_inputs:
        print()
        print("NOT REPRODUCIBLE: outputs moved while every input stayed identical.")
        print("Re-running the pipeline is not giving the same result, so a build")
        print("cannot be reasoned about by comparing it to a previous one. Fix this")
        print("before bisecting anything.")
        if update:
            MANIFEST.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8", newline="\n")
        return 1

    if update:
        MANIFEST.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8", newline="\n")
        print("manifest updated.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
