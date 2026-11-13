#!/usr/bin/env python3
# Random AArch64 memory-access programs for the JIT harness. usage: gen.py <seed> <count> > tests.s
import random, sys
seed, count = int(sys.argv[1]), int(sys.argv[2])
rnd = random.Random(seed)
GPR = [f"x{i}" for i in range(6, 28)]          # data registers (never bases)
BASES = ["x1", "x2", "x3", "sp"]
def w(x): return "w" + x[1:]
def d(x): return "d" + x[1:]
def s(x): return "s" + x[1:]
def q(x): return "q" + x[1:]
def vreg(): return rnd.randrange(0, 24)
def base(): return rnd.choice(BASES)
def pick(): return rnd.choice(GPR)
def off(sz, mx=256):
    return rnd.randrange(0, mx) * sz
def soff(): return rnd.randrange(-256, 256)
def pair_off(sz): return rnd.randrange(-64, 64) * sz
def op():
    k = rnd.random()
    b = base()
    r = pick()
    if k < 0.16:
        n = rnd.choice(["ldr", "str"]); return f"{n} {r}, [{b}, #{off(8, 64)}]"
    if k < 0.26:
        n = rnd.choice(["ldr", "str"]); return f"{n} {w(r)}, [{b}, #{off(4, 64)}]"
    if k < 0.32:
        n = rnd.choice(["ldrh", "strh", "ldrb", "strb"]); sz = 2 if "h" in n else 1; return f"{n} {w(r)}, [{b}, #{off(sz, 64)}]"
    if k < 0.36:
        n = rnd.choice(["ldrsw", "ldrsh", "ldrsb"]); sz = {"ldrsw": 4, "ldrsh": 2, "ldrsb": 1}[n]; return f"{n} {r}, [{b}, #{off(sz, 64)}]"
    if k < 0.42:
        n = rnd.choice(["ldr", "str"]); t = rnd.choice(["q", "d", "s"]); v = vreg()
        sz = {"q": 16, "d": 8, "s": 4}[t]; return f"{n} {t}{v}, [{b}, #{off(sz, 64)}]"
    if k < 0.50:
        n = rnd.choice(["ldur", "stur"]); return f"{n} {r}, [{b}, #{soff()}]"
    if k < 0.62:
        n = rnd.choice(["ldp", "stp"]); r2 = pick()
        if n == "ldp" and r2 == r: r2 = GPR[(GPR.index(r) + 1) % len(GPR)]
        t = rnd.choice(["x", "x", "w"])
        if t == "x": return f"{n} {r}, {r2}, [{b}, #{pair_off(8)}]"
        return f"{n} {w(r)}, {w(r2)}, [{b}, #{pair_off(4)}]"
    if k < 0.66:
        n = rnd.choice(["ldp", "stp"]); v1 = vreg(); v2 = (v1 + 1) % 24; return f"{n} q{v1}, q{v2}, [{b}, #{pair_off(16)}]"
    if k < 0.70:
        n = rnd.choice(["ldr", "str"]); mode = rnd.choice(["pre", "post"])
        i = rnd.choice([-64, -32, -16, -8, 8, 16, 32, 64])
        return f"{n} {r}, [{b}, #{i}]!" if mode == "pre" else f"{n} {r}, [{b}], #{i}"
    if k < 0.73:
        n = rnd.choice(["ldp", "stp"]); r2 = pick()
        if n == "ldp" and r2 == r: r2 = GPR[(GPR.index(r) + 1) % len(GPR)]
        i = rnd.choice([-64, -32, -16, 16, 32, 64]); mode = rnd.choice(["pre", "post"])
        return f"{n} {r}, {r2}, [{b}, #{i}]!" if mode == "pre" else f"{n} {r}, {r2}, [{b}], #{i}"
    if k < 0.80:
        return rnd.choice([f"add {b}, {b}, #{rnd.randrange(1, 512)}", f"sub {b}, {b}, #{rnd.randrange(1, 512)}"]) if b != "sp" else f"add sp, sp, #{rnd.choice([16, 32, 64, 128])}"
    if k < 0.85:
        return rnd.choice([f"add {pick()}, {pick()}, #{rnd.randrange(1, 4096)}", f"mov {pick()}, #{rnd.randrange(0, 65535)}",
                           f"eor {pick()}, {pick()}, {pick()}", f"add {pick()}, {pick()}, {pick()}"])
    if k < 0.89:
        n = rnd.choice(["ldr", "str"]); return f"{n} {r}, [{b}, x4]" if rnd.random() < 0.5 else f"{n} {w(r)}, [{b}, w4, uxtw #2]"
    if k < 0.93:
        return rnd.choice([f"ldar {r}, [x5]", f"stlr {r}, [x5]", f"ldaxr {r}, [x5]", f"stlxr w28, {r}, [x5]", "dmb ish", f"ldxr {w(r)}, [x5, #0]"])
    if k < 0.95:
        return rnd.choice(["dmb ish", "isb", "dsb sy", "nop"])
    # a run of fields of one struct: the shape the page reuse is for
    n = rnd.choice(["ldr", "str"]); return f"{n} {r}, [{b}, #{off(8, 4)}]"
out = [".text", ".balign 4096"]
for i in range(count):
    out.append(f"test{i}:")
    # prologue like a function: save pairs
    nops = rnd.randrange(20, 140)
    if rnd.random() < 0.5:
        out.append("    stp x29, x30, [sp, #-48]!")
        out.append("    stp x19, x20, [sp, #16]")
        out.append("    stp x21, x22, [sp, #32]")
    for _ in range(nops):
        out.append("    " + op())
        if rnd.random() < 0.04:
            out.append("    add x4, x4, #8")
    out.append("    svc #0")
    out.append(".balign 4096")
print("\n".join(out))
