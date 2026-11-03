"""Pre-specialized TEV fragment shaders for the headset.

The Quest's driver specializes the 600 KB TEV fragment module per pipeline but leaves most of it in: a one-stage quad ran 160-200
instructions a fragment. spirv-opt with the key's constants frozen leaves about 5% of the module (one key: 30748 -> 1521 instructions).
That takes about 0.4 s a key on a desktop, so it is done here, at build time, for the keys the game is known to use (pipeline key
files); the renderer uses a folded module when one matches a pipeline's constants and the driver's specialization otherwise.

  python -I tools/fold_tev_shaders.py --spirv-opt PATH --shaders DIR --out DIR KEYFILE...

Writes OUT/<fnv1a64 of the 68 constants and the variant>.spv; the variant is the module: smoke.stereo.early.frag.spv for keys whose alpha
test cannot fail (key word 0 bit 16 clear), smoke.stereo.frag.spv otherwise (renderer/vulkan_renderer.cpp make_pipeline, folded_module);
variants 2 and 3 are the same two with the texture table (smoke.stereo.tex.frag.spv, smoke.stereo.tex.early.frag.spv, UC_TEX_MERGE), folded
for every key when those modules are in --shaders.
"""
import argparse
import concurrent.futures
import os
import struct
import subprocess
import sys
import tempfile

HEADER = 12          # pipeline_keys.h kPipelineKeysHeader: three words
KEY_WORDS = 70       # PipelineKey


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


DIRECT_STAGE = 1 << 16   # pipeline_keys.h kTevDirectStage: order-word bit of a stage without indirect texturing (the headset's keys)


def headset_key(key):
    """pipeline_keys.h headset_direct_key: a key recorded before the headset marked its direct stages is read as all-direct."""
    stages = min(key[1], 16)
    if any(key[5 + 4 * i] & DIRECT_STAGE for i in range(stages)):
        return key
    key = list(key)
    for i in range(stages):
        key[5 + 4 * i] |= DIRECT_STAGE
    return tuple(key)


def stripped_key(key):
    key = list(key)
    for i in range(min(key[1], 16)):
        key[5 + 4 * i] &= ~DIRECT_STAGE
    return tuple(key)


def constants(key):
    """The fragment stage's specialization constants (make_pipeline): 0..66 = key words 1..67, 67 = key word 0 bit 16."""
    return list(key[1:68]) + [(key[0] >> 16) & 1]


def variant(key):
    return 0 if (key[0] >> 16) & 1 else 1   # 1: early-Z module (no alpha test)


def name(values, var):
    return "%016x.spv" % fnv1a64(struct.pack("<68I", *values) + bytes([var]))


def read_keys(path):
    data = open(path, "rb").read()[HEADER:]
    size = KEY_WORDS * 4
    return [struct.unpack_from("<%dI" % KEY_WORDS, data, i) for i in range(0, len(data) - size + 1, size)]


def fold(job):
    spirv_opt, source, values, target = job
    spec = " ".join("%d:%d" % (i, v) for i, v in enumerate(values))
    with tempfile.TemporaryDirectory() as tmp:
        staged = os.path.join(tmp, "set.spv")
        subprocess.run([spirv_opt, source, "--set-spec-const-default-value", spec, "-o", staged], check=True)
        # fold-spec-const-op-composite: -O alone left the OpSpecConstantOp conditions (stage count, selectors) unfolded, so every unused
        # stage stayed in the module behind a constant branch (one key: 41 -> 17 KB).
        subprocess.run([spirv_opt, staged, "--freeze-spec-const", "--fold-spec-const-op-composite", "-O", "-o", target + ".tmp"], check=True)
    os.replace(target + ".tmp", target)
    return os.path.getsize(target)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--spirv-opt", required=True)
    parser.add_argument("--shaders", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("keys", nargs="+")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    modules = {0: os.path.join(args.shaders, "smoke.stereo.frag.spv"), 1: os.path.join(args.shaders, "smoke.stereo.early.frag.spv"),
               2: os.path.join(args.shaders, "smoke.stereo.tex.frag.spv"), 3: os.path.join(args.shaders, "smoke.stereo.tex.early.frag.spv")}
    table = os.path.exists(modules[2]) and os.path.exists(modules[3])   # the texture-table modules (variant + 2)
    wanted = {}
    for path in args.keys:
        if os.path.exists(path):
            for key in read_keys(path):
                # both forms: the headset's (direct stages marked, their indirect path compiled out) and the one with every stage able to
                # read an indirect coordinate, which the renderer falls back to for a key whose own form is not listed (a stage that does
                # read one, in a scene no recording covered): never slower than before the marks
                for form in (headset_key(key), stripped_key(key)):
                    values, var = constants(form), variant(form)
                    for v in (var, var + 2) if table else (var,):
                        wanted[name(values, v)] = (values, v)
    jobs = []
    for file, (values, var) in sorted(wanted.items()):
        target = os.path.join(args.out, file)
        if not os.path.exists(target) or os.path.getmtime(target) < os.path.getmtime(modules[var]):
            jobs.append((args.spirv_opt, modules[var], values, target))
    for stale in set(os.listdir(args.out)) - set(wanted):   # keys no longer listed
        if stale.endswith(".spv"):
            os.remove(os.path.join(args.out, stale))
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        total = sum(pool.map(fold, jobs))
    print("fold_tev_shaders: %d keys, %d folded now (%d KB)" % (len(wanted), len(jobs), total // 1024))
    return 0


if __name__ == "__main__":
    sys.exit(main())
