"""Marks a TEV fragment module's colour arithmetic RelaxedPrecision (mediump), in place.

The headset's GPU (Adreno) runs relaxed-precision float arithmetic at half precision: twice the ALU rate and half the registers. The GX
TEV works on 8-bit colours (values in -4..4, steps of 1/255), which half precision (11-bit mantissa) holds. What must stay full precision
is excluded: every value that feeds a texture coordinate (the backward slice of the image-sample coordinates: interpolated (s, t, q)
divided by q, the indirect texturing math) and every value derived from the fragment position (the fog's depth, 24-bit). Integer
operations are untouched.

  python -I tools/spirv_relaxed_precision.py MODULE.spv...           in place
  python -I tools/spirv_relaxed_precision.py --dir SOURCE TARGET      every .spv of SOURCE, relaxed, into TARGET
"""
import struct
import sys

OP_EXTINST, OP_TYPE_FLOAT, OP_TYPE_VECTOR, OP_VARIABLE, OP_LOAD, OP_DECORATE, OP_FUNCTION = 12, 22, 23, 59, 61, 71, 54
DECORATION_BUILTIN, DECORATION_RELAXED, BUILTIN_FRAGCOORD = 11, 0, 15
# image sample instructions: the coordinate is operand 3 (result type, result, sampled image, coordinate, ...)
SAMPLES = {87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100}   # OpImageSample*/OpImageFetch(95)/OpImageGather(96)/Dref
# float arithmetic worth relaxing (results of a float scalar or vector type)
ARITH = {
    127,  # OpFNegate
    129,  # OpFAdd
    131,  # OpFSub
    133,  # OpFMul
    136,  # OpFDiv
    142,  # OpVectorTimesScalar
    148,  # OpDot
    79,   # OpVectorShuffle
    80,   # OpCompositeConstruct
    81,   # OpCompositeExtract
    82,   # OpCompositeInsert
    169,  # OpSelect
    12,   # OpExtInst (GLSL.std.450: FMix, FClamp, FMin, FMax, ...)
}


def relax(path, target):
    data = open(path, "rb").read()
    words = list(struct.unpack("<%dI" % (len(data) // 4), data))
    if words[0] != 0x07230203:
        sys.exit(path + ": not SPIR-V")
    insts = []   # (offset, opcode, operands)
    i = 5
    while i < len(words):
        count, opcode = words[i] >> 16, words[i] & 0xFFFF
        insts.append((i, opcode, words[i + 1:i + count]))
        i += count
    float_types = set()
    for _, op, ops in insts:
        if op == OP_TYPE_FLOAT and ops[1] == 32: float_types.add(ops[0])
    for _, op, ops in insts:
        if op == OP_TYPE_VECTOR and ops[1] in float_types: float_types.add(ops[0])
    fragcoord = {ops[0] for _, op, ops in insts if op == OP_DECORATE and len(ops) >= 3 and ops[1] == DECORATION_BUILTIN and ops[2] == BUILTIN_FRAGCOORD}
    # result id -> (opcode, operand ids that are values)
    defs, uses = {}, {}
    for _, op, ops in insts:
        if op in (OP_DECORATE, 72) or len(ops) < 2: continue
        result = ops[1]
        args = ops[2:]
        if op == OP_EXTINST: args = ops[4:]
        defs[result] = (op, ops[0], args)
        for a in args: uses.setdefault(a, []).append(result)
    full = set()
    # backward slice of every sample coordinate
    stack = [ops[3] for _, op, ops in insts if op in SAMPLES and len(ops) > 3]
    while stack:
        v = stack.pop()
        if v in full or v not in defs: continue
        full.add(v); stack.extend(defs[v][2])
    # forward slice of the fragment position
    stack = [ops[1] for _, op, ops in insts if op == OP_LOAD and len(ops) > 2 and ops[2] in fragcoord]
    stack += [r for r, (op, t, args) in defs.items() if any(a in fragcoord for a in args)]   # access chains into gl_FragCoord
    while stack:
        v = stack.pop()
        if v in full: continue
        full.add(v); stack.extend(uses.get(v, []))
    marked = [r for r, (op, t, args) in defs.items() if op in ARITH and t in float_types and r not in full]
    marked += [r for r, (op, t, args) in defs.items() if op in SAMPLES and op not in (95, 96) and r not in full]   # sampled colours (vec4 float)
    annotations = [w for r in sorted(set(marked)) for w in ((3 << 16) | OP_DECORATE, r, DECORATION_RELAXED)]
    first_function = next(off for off, op, _ in insts if op == OP_FUNCTION)
    # decorations belong before the type declarations: insert after the last OpDecorate/OpMemberDecorate
    last_decoration = max(off + (words[off] >> 16) for off, op, _ in insts if op in (OP_DECORATE, 72) and off < first_function)
    out = words[:last_decoration] + annotations + words[last_decoration:]
    open(target, "wb").write(struct.pack("<%dI" % len(out), *out))
    return len(set(marked))


def main():
    if sys.argv[1] == "--dir":
        import os
        source, target = sys.argv[2], sys.argv[3]
        os.makedirs(target, exist_ok=True)
        names = [n for n in os.listdir(source) if n.endswith(".spv")]
        total = sum(relax(os.path.join(source, n), os.path.join(target, n)) for n in names)
        print(f"{len(names)} modules into {target}: {total} results relaxed")
    else:
        for path in sys.argv[1:]:
            print(f"{path}: {relax(path, path)} results relaxed")


if __name__ == "__main__":
    main()
