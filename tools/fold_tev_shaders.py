"""Pre-specialized TEV fragment shaders for the headset.

The Quest's driver specializes the 600 KB TEV fragment module per pipeline but leaves most of it in: a one-stage quad ran 160-200
instructions a fragment. spirv-opt with the key's constants frozen leaves about 5% of the module (one key: 30748 -> 1521 instructions).
That takes about 0.4 s a key on a desktop, so it is done here, at build time, for the keys the game is known to use (pipeline key
files); the renderer uses a folded module when one matches a pipeline's constants and the driver's specialization otherwise.

  python -I tools/fold_tev_shaders.py --spirv-opt PATH --shaders DIR --out DIR KEYFILE...

Writes OUT/<fnv1a64 of the 68 constants and the variant>.spv; the variant is the module: smoke.stereo.early.frag.spv for keys whose alpha
test cannot fail (key word 0 bit 16 clear), smoke.stereo.frag.spv otherwise (renderer/vulkan_renderer.cpp make_pipeline, folded_module).
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
    modules = {0: os.path.join(args.shaders, "smoke.stereo.frag.spv"), 1: os.path.join(args.shaders, "smoke.stereo.early.frag.spv")}
    wanted = {}
    for path in args.keys:
        if os.path.exists(path):
            for key in read_keys(path):
                values, var = constants(key), variant(key)
                wanted[name(values, var)] = (values, var)
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
