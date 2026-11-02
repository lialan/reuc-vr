"""Marks a SPIR-V vertex module's position output Invariant, in place.

The game draws a surface in layers (base, dirt, decals) over the same vertices and tests them EQUAL or LEQUAL against the depth the first
layer wrote. The driver compiles the vertex stage once per pipeline, and without the Invariant decoration Vulkan does not require the
position to come out bit-identical across pipelines: on the Quest the layers' depths differed in the last bits and their pixels passed or
failed the test at random, sparkling with every head movement (the headset's per-eye reprojection lengthens the position math). Slang has
no qualifier for it, so the build adds `OpDecorate <position> Invariant` after the module's `OpDecorate <position> BuiltIn Position`.

  python -I tools/spirv_invariant_position.py MODULE.spv
"""
import struct
import sys

OP_DECORATE = 71
DECORATION_BUILTIN, DECORATION_INVARIANT = 11, 18
BUILTIN_POSITION = 0


def main():
    path = sys.argv[1]
    data = open(path, "rb").read()
    if len(data) % 4 or len(data) < 20:
        sys.exit(f"{path}: not a SPIR-V module")
    words = list(struct.unpack(f"<{len(data) // 4}I", data))
    if words[0] != 0x07230203:
        sys.exit(f"{path}: not a little-endian SPIR-V module")
    out, i, position, marked = words[:5], 5, None, set()
    while i < len(words):
        count, opcode = words[i] >> 16, words[i] & 0xFFFF
        if count == 0:
            sys.exit(f"{path}: malformed instruction at word {i}")
        inst = words[i:i + count]
        out += inst
        if opcode == OP_DECORATE and count >= 3 and inst[2] == DECORATION_INVARIANT:
            marked.add(inst[1])
        if opcode == OP_DECORATE and count == 4 and inst[2] == DECORATION_BUILTIN and inst[3] == BUILTIN_POSITION:
            position = inst[1]
            out += [(3 << 16) | OP_DECORATE, position, DECORATION_INVARIANT]
        i += count
    if position is None:
        sys.exit(f"{path}: no BuiltIn Position output")
    if position in marked:   # already invariant: leave the module as it was
        return
    open(path, "wb").write(struct.pack(f"<{len(out)}I", *out))


if __name__ == "__main__":
    main()
