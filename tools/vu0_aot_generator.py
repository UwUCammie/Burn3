#!/usr/bin/env python3
"""Translate captured VU0 microprograms to native C++ (TAKEDOWN_LOCAL_VU0_AOT_MODULE).

Input is a TAKEDOWN_VU0_CAPTURE_DIR capture: the MicroMem images VCALLMS
launches started from (vu0-<hash>.bin) and manifest.tsv's (image, entry)
launches. For every captured entry the translator explores every control
path statically (VU0 microcode has no indirect jumps, and a microprogram
cannot write MicroMem) and emits one C++ function per image in which:

- VF/ACC/VI/I/Q, the four-deep MAC flag pipeline and STATUS live in locals,
  loaded at launch and spilled at exits; upper FMAC operations call the exact
  SSE2 kernels of vu0_simd_arithmetic.hpp with compile-time operands/masks.
- MAC flags are only formed where something can observe them (FMAND/FMOR/FMEQ
  four pairs later, a pipeline that is live at a join or exit); STATUS's sticky
  bits accumulate as lane vectors.
- Control flow is a graph of (pair, pending branch target, E-delay, recent VI
  writes) nodes joined by gotos; the integer-branch backup rule
  (branch_vi_source) is resolved statically from the recent-write pattern.
- The pair limit is checked once per join node against the longest run to the
  next join; when it could be reached, and at the entry when the limit is
  already short, the machine is handed to the interpreter at that exact pair
  with its full state (Q pipeline, MAC pipeline, VI write history).
- An undecodable pair on a reachable path returns the interpreter's
  UnsupportedInstruction result at that pair.

Every decode the translator relies on is checked against the interpreter's
own constexpr decoder with a static_assert. The identity of a program is
checked once per (MicroMem generation, entry) against the words of every pair
reachable from the entry.

The output is game-derived; write it under local-analysis/ only.

usage: vu0_aot_generator.py <capture-dir> <output.inc> [--min-launches N] [--listing]
"""
import argparse
import re
import struct
import sys
from pathlib import Path

PAIR_COUNT = 512
sys.setrecursionlimit(20000)
PAIR_MASK = 0x1FF
MAX_NODES_PER_IMAGE = 20000
DIV_SQRT_LATENCY = 7
RSQRT_LATENCY = 13


def sext(value, bits):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


# ---- Decode (mirrors decode_upper/decode_lower in vu0_micro_interpreter.cpp) ----

SPECIAL2 = {}
for i in range(4):
    SPECIAL2[0x00 + i] = ("AddaLane", i)
    SPECIAL2[0x08 + i] = ("MaddaLane", i)
    SPECIAL2[0x0C + i] = ("MsubaLane", i)
    SPECIAL2[0x18 + i] = ("MulaLane", i)
SPECIAL2.update({
    0x1E: ("MulaI", 0), 0x28: ("Adda", 0), 0x29: ("Madda", 0), 0x2A: ("Mula", 0),
    0x2C: ("Suba", 0), 0x2D: ("Msuba", 0), 0x2E: ("OpMula", 0), 0x2F: ("Nop", 0),
    0x20: ("AddaQ", 0), 0x21: ("MaddaQ", 0), 0x22: ("AddaI", 0), 0x23: ("MaddaI", 0),
    0x24: ("SubaQ", 0), 0x25: ("MsubaQ", 0), 0x26: ("SubaI", 0), 0x27: ("MsubaI", 0),
})
ITOF_FRACTIONS = {0x10: 0, 0x11: 4, 0x12: 12, 0x13: 15}
FTOI_FRACTIONS = {0x14: 0, 0x15: 4, 0x16: 12, 0x17: 15}
SPECIAL1 = {}
for i in range(4):
    SPECIAL1[0x00 + i] = ("AddLane", i)
    SPECIAL1[0x04 + i] = ("SubLane", i)
    SPECIAL1[0x08 + i] = ("MaddLane", i)
    SPECIAL1[0x0C + i] = ("MsubLane", i)
    SPECIAL1[0x10 + i] = ("MaxLane", i)
    SPECIAL1[0x14 + i] = ("MiniLane", i)
    SPECIAL1[0x18 + i] = ("MulLane", i)
SPECIAL1.update({
    0x1C: ("MulQ", 0), 0x1D: ("MaxI", 0), 0x1E: ("MulI", 0), 0x1F: ("MiniI", 0),
    0x20: ("AddQ", 0), 0x21: ("MaddQ", 0), 0x22: ("AddI", 0), 0x23: ("MaddI", 0),
    0x24: ("SubQ", 0), 0x25: ("MsubQ", 0), 0x26: ("SubI", 0), 0x27: ("MsubI", 0),
    0x28: ("Add", 0), 0x29: ("Madd", 0), 0x2A: ("Mul", 0), 0x2B: ("Max", 0),
    0x2C: ("Sub", 0), 0x2D: ("Msub", 0), 0x2E: ("Opmsub", 0), 0x2F: ("Mini", 0),
})
# Operand forms that use no FT register (decode leaves ft at 0).
NO_FT = {"MulQ", "MaxI", "MulI", "MiniI", "AddQ", "MaddQ", "AddI", "MaddI", "SubQ",
         "MsubQ", "SubI", "MsubI", "MulaI", "AddaQ", "MaddaQ", "AddaI", "MaddaI",
         "SubaQ", "MsubaQ", "SubaI", "MsubaI", "Nop"}


def decode_upper(word):
    if (word & 0x18000000) and not (word & 0x40000000):
        return None
    ft = (word >> 16) & 31
    fs = (word >> 11) & 31
    fd = (word >> 6) & 31
    op1 = word & 0x3F
    if (op1 & 0x3C) == 0x3C:
        op2 = (word & 3) | ((word >> 4) & 0x7C)
        if op2 in ITOF_FRACTIONS:
            return dict(op="Vitof", lane=0, ft=ft, fs=fs, fd=ft, fraction=ITOF_FRACTIONS[op2])
        if op2 in FTOI_FRACTIONS:
            return dict(op="Vftoi", lane=0, ft=ft, fs=fs, fd=ft, fraction=FTOI_FRACTIONS[op2])
        if op2 not in SPECIAL2:
            return None
        op, lane = SPECIAL2[op2]
        if op == "Nop":
            return dict(op=op, lane=0, ft=0, fs=0, fd=0, fraction=0)
        return dict(op=op, lane=lane, ft=0 if op in NO_FT else ft, fs=fs, fd=0, fraction=0)
    if op1 not in SPECIAL1:
        return None
    op, lane = SPECIAL1[op1]
    return dict(op=op, lane=lane, ft=0 if op in NO_FT else ft, fs=fs, fd=fd, fraction=0)


def L(op, **fields):
    base = dict(op=op, mask=0, it=0, is_=0, ft=0, fs=0, fd=0, imm=0)
    base.update(fields)
    return base


def decode_lower(word):
    primary = (word >> 25) & 0x7F
    mask = (word >> 21) & 0xF
    ft = (word >> 16) & 31
    it = (word >> 16) & 15
    fs = (word >> 11) & 31
    is_ = (word >> 11) & 15
    fd = (word >> 6) & 31
    imm = sext(word & 0x7FF, 11)
    if primary == 0x00:
        return L("Lq", mask=mask, is_=is_, ft=ft, imm=imm)
    if primary == 0x01:
        return L("Sq", mask=mask, it=it, fs=fs, imm=imm)
    if primary == 0x05:
        return L("Isw", mask=mask, it=it, is_=is_, imm=imm)
    if primary == 0x04:
        return L("Ilw", mask=mask, it=it, is_=is_, imm=imm)
    if primary == 0x08:
        return L("Iaddiu", it=it, is_=is_, imm=imm)
    if primary == 0x09:
        return L("Isubiu", it=it, is_=is_, imm=imm)
    if primary == 0x18:
        return L("Fmeq", it=it, is_=is_)
    if primary == 0x1A:
        return L("Fmand", it=it, is_=is_)
    if primary == 0x1B:
        return L("Fmor", it=it, is_=is_)
    if primary == 0x20:
        return L("B", imm=imm)
    if primary == 0x28:
        return L("Ibeq", it=it, is_=is_, imm=imm)
    if primary == 0x29:
        return L("Ibne", it=it, is_=is_, imm=imm)
    if primary in (0x2C, 0x2D, 0x2E, 0x2F):
        return L({0x2C: "Iblt", 0x2D: "Ibgt", 0x2E: "Ible", 0x2F: "Ibge"}[primary], is_=is_, imm=imm)
    if primary == 0x40:
        function = word & 0x3F
        vi_fd = fd & 15
        if function == 0x30:
            return L("Iadd", it=ft, is_=fs, fd=vi_fd)
        if function == 0x31:
            return L("Isub", it=ft, is_=fs, fd=vi_fd)
        if function == 0x32:
            return L("Iaddi", it=it, is_=is_, fd=fd & 31)
        if function == 0x34:
            return L("Iand", it=ft, is_=fs, fd=vi_fd)
        if function == 0x35:
            return L("Ior", it=ft, is_=fs, fd=vi_fd)
        if function == 0x3C:
            if fd == 12:
                return L("Nop" if mask == 0 else "Move", mask=mask, ft=ft, fs=fs)
            if fd == 14:
                return L("Div", it=(word >> 23) & 3, is_=(word >> 21) & 3, ft=ft, fs=fs)
            if fd == 15:
                return L("Mtir", it=it, is_=(word >> 21) & 3, fs=fs)
            if fd == 13:
                return L("Lqi", mask=mask, is_=is_, ft=ft)
            return None
        if function == 0x3D:
            if fd == 12:
                return L("Nop" if mask == 0 else "Mr32", mask=mask, ft=ft, fs=fs)
            if fd == 13:
                return L("Sqi", mask=mask, it=it, fs=fs)
            if fd == 14:
                return L("Sqrt", it=(word >> 23) & 3, ft=ft)
            if fd == 15:
                return L("Mfir", mask=mask, is_=is_, ft=ft)
            return None
        if function == 0x3E:
            if fd == 13:
                return L("Lqd", mask=mask, is_=is_, ft=ft)
            if fd == 14:
                return L("Rsqrt", it=(word >> 23) & 3, is_=(word >> 21) & 3, ft=ft, fs=fs)
            return None
        if function == 0x3F:
            if fd == 13:
                return L("Sqd", mask=mask, it=it, fs=fs)
            if fd == 14:
                return L("Waitq")
            return None
        return None
    return None


# ---- Pair semantics ----

FMAC = {
    # op: (kind, operation, operand, writes_acc)
}
for name, operation in (("Add", "Add"), ("Sub", "Subtract"), ("Madd", "MultiplyAdd"),
                        ("Msub", "MultiplySubtract"), ("Mul", "Multiply")):
    FMAC[name + "Lane"] = ("A", operation, "FtLane", False)
    FMAC[name] = ("A", operation, "Ft", False)
    FMAC[name + "aLane"] = ("A", operation, "FtLane", True)
    FMAC[name + "a"] = ("A", operation, "Ft", True)
    FMAC[name + "Q"] = ("A", operation, "Q", False)
    FMAC[name + "I"] = ("A", operation, "I", False)
    FMAC[name + "aQ"] = ("A", operation, "Q", True)
    FMAC[name + "aI"] = ("A", operation, "I", True)
FMAC["Opmsub"] = ("A", "MultiplySubtract", "Cross", False)
FMAC["OpMula"] = ("A", "Multiply", "Cross", True)
FMAC["MaxLane"] = ("Max", None, "FtLane", False)
FMAC["MiniLane"] = ("Min", None, "FtLane", False)
FMAC["MaxI"] = ("Max", None, "I", False)
FMAC["MiniI"] = ("Min", None, "I", False)
FMAC["Max"] = ("Max", None, "Ft", False)
FMAC["Mini"] = ("Min", None, "Ft", False)
# The decoder's enum has every name above except the ones it lacks; restrict
# to the decoder's set (e.g. there is no SubaLane/MaddQ-to-ACC naming clash).
UPPER_OPS = {"Nop", "AddLane", "SubLane", "MaddLane", "MsubLane", "MulLane", "MaxLane",
             "MiniLane", "MaxI", "MiniI", "MulQ", "MulI", "AddQ", "AddI", "SubI", "MaddQ",
             "MaddI", "SubQ", "MsubQ", "MsubI", "Add", "Madd", "Mul", "Max", "Sub", "Msub",
             "Opmsub", "Mini", "AddaLane", "MaddaLane", "MsubaLane", "MulaLane", "Mula",
             "OpMula", "Adda", "MulaI", "AddaQ", "MaddaQ", "AddaI", "MaddaI", "SubaQ",
             "MsubaQ", "SubaI", "MsubaI", "Madda", "Suba", "Msuba", "Vitof", "Vftoi"}

COND_BRANCHES = {"Ibeq", "Ibne", "Iblt", "Ibgt", "Ible", "Ibge"}
INDEX5_OPS = {"Iadd", "Isub", "Iand", "Ior"}


class Pair:
    def __init__(self, pc, lower_word, upper_word):
        self.pc = pc
        self.lower_word = lower_word
        self.upper_word = upper_word
        self.i_bit = bool(upper_word & 0x80000000)
        self.e_bit = bool(upper_word & 0x40000000)
        self.upper = decode_upper(upper_word)
        self.lower_decode = decode_lower(lower_word)
        self.lower = None if self.i_bit else self.lower_decode
        self.unsupported = None
        if self.upper is None:
            self.unsupported = "Upper"
        elif not self.i_bit and self.lower is None:
            self.unsupported = "Lower"
        # A 5-bit VI index makes the interpreter's read throw; never compile it.
        self.untranslatable = (self.lower is not None and self.lower["op"] in INDEX5_OPS and
                               (self.lower["it"] >= 16 or self.lower["is_"] >= 16))
        self.vi_write = None  # (reg, eligible)
        self.branch = None  # ("B", target) or ("C", target)
        if self.unsupported or self.lower is None:
            return
        low = self.lower
        op = low["op"]
        reg = None
        if op in ("Lqi", "Lqd"):
            reg = low["is_"]
        elif op in ("Sqi", "Sqd"):
            reg = low["it"]
        elif op == "Ilw":
            reg = low["it"] if low["mask"] else None
        elif op in ("Iaddiu", "Isubiu", "Fmand", "Fmor", "Fmeq", "Iaddi", "Mtir"):
            reg = low["it"]
        elif op in INDEX5_OPS:
            reg = low["fd"]
        if reg:
            self.vi_write = (reg, op not in ("Fmand", "Fmor", "Fmeq"))
        target = (pc + 1 + (low["imm"] & 0xFFFF)) & PAIR_MASK
        if op == "B":
            self.branch = ("B", target)
        elif op in COND_BRANCHES:
            self.branch = ("C", target)

    @property
    def cond_branch(self):
        return self.branch is not None and self.branch[0] == "C"

    def updates_mac(self):
        return self.upper is not None and self.upper["op"] in FMAC and FMAC[self.upper["op"]][0] == "A"

    def describe(self):
        text = f"{self.pc:3d}: {self.lower_word:08x} {self.upper_word:08x}"
        if self.unsupported:
            return text + f"  <unsupported {self.unsupported}>"
        up = self.upper
        dest = "".join(c for b, c in ((8, "x"), (4, "y"), (2, "z"), (1, "w")) if (self.upper_word >> 21) & b)
        text += f"  {up['op']}.{dest} fd{up['fd']} fs{up['fs']} ft{up['ft']}"
        if up["op"].endswith("Lane"):
            text += f" lane{up['lane']}"
        if self.i_bit:
            text += f" | I=0x{self.lower_word:08x}"
        else:
            low = self.lower
            text += f" | {low['op']} m{low['mask']} it{low['it']} is{low['is_']} ft{low['ft']} fs{low['fs']} fd{low['fd']} imm{low['imm']}"
        if self.e_bit:
            text += " [E]"
        return text


def successors(pair, pt):
    """(pc, pending target) after executing `pair` with pending target `pt`."""
    pc = pair.pc
    if pair.branch is None:
        return [(pt, None)] if pt is not None else [((pc + 1) & PAIR_MASK, None)]
    kind, target = pair.branch
    taken = ((pc + 1) & PAIR_MASK, target)
    if kind == "B":
        return [taken]
    not_taken = (pt, None) if pt is not None else ((pc + 1) & PAIR_MASK, None)
    return [taken, not_taken]


class TranslationError(Exception):
    pass


class Node:
    def __init__(self, ident, key):
        self.id = ident
        self.key = key
        self.pc, self.pt, self.ed, self.hist = key
        self.succ = []  # list of (label, node) ; label in ("taken", "next")
        self.preds = 0
        self.entry = False
        self.terminal = None  # "unsupported" / "complete"


class Image:
    def __init__(self, name, data, entries):
        self.name = name
        self.words = struct.unpack("<1024I", data)
        self.pairs = [Pair(pc, self.words[pc * 2], self.words[pc * 2 + 1]) for pc in range(PAIR_COUNT)]
        self.entries = sorted(entries)
        self.relevant = {}
        self.nodes = {}
        self.order = []
        self.saves = set()
        self.compiled_entries = []
        self.reach = {}

    # Nodes keyed (pc, pt, ed): does a conditional branch run within this
    # pair and the next three? Only then can recent VI writes be observed.
    def compute_relevance(self, entry):
        graph = {}
        work = [(entry, None, False)]
        while work:
            key = work.pop()
            if key in graph:
                continue
            pc, pt, ed = key
            pair = self.pairs[pc]
            nexts = []
            if not pair.unsupported and not ed:
                nexts = [(npc, npt, pair.e_bit) for npc, npt in successors(pair, pt)]
            graph[key] = nexts
            work.extend(nexts)
        for key in graph:
            frontier = {key}
            found = False
            for _ in range(4):
                if any(self.pairs[k[0]].cond_branch for k in frontier):
                    found = True
                    break
                frontier = {n for k in frontier for n in graph[k]}
            self.relevant[key] = self.relevant.get(key, False) or found

    def explore(self):
        for entry in self.entries:
            self.compute_relevance(entry)
        unknown = ("X",) * 4
        for entry in self.entries:
            try:
                before = dict(self.nodes)
                order_before = list(self.order)
                saves_before = set(self.saves)
                start = self.node((entry, None, False, (None,) * 4 if self.relevant[(entry, None, False)] else unknown))
                start.entry = True
                work = [start]
                seen = set()
                while work:
                    node = work.pop()
                    if node.id in seen:
                        continue
                    seen.add(node.id)
                    if len(self.nodes) > MAX_NODES_PER_IMAGE:
                        raise TranslationError("node budget exceeded")
                    pair = self.pairs[node.pc]
                    if pair.untranslatable:
                        raise TranslationError(f"pair {node.pc}: 5-bit VI index")
                    if node.succ or node.terminal:
                        continue
                    if pair.unsupported:
                        node.terminal = "unsupported"
                        continue
                    if node.ed:
                        node.terminal = "complete"
                        continue
                    record = None
                    if pair.vi_write:
                        record = ("w", node.pc, pair.vi_write[0], pair.vi_write[1])
                    hist = node.hist[1:] + (record,)
                    nexts = successors(pair, node.pt)
                    labels = ["taken", "next"] if len(nexts) == 2 else ["next"]
                    for label, (npc, npt) in zip(labels, nexts):
                        key0 = (npc, npt, pair.e_bit)
                        nhist = hist if self.relevant[key0] else unknown
                        succ = self.node((npc, npt, pair.e_bit, nhist))
                        node.succ.append((label, succ))
                        succ.preds += 1
                        work.append(succ)
                for node_id in seen:
                    self.check_backup(self.order[node_id])
                self.compiled_entries.append(entry)
                self.reach[entry] = sorted({self.order[i].pc for i in seen})
            except TranslationError as error:
                print(f"{self.name} entry {entry}: not translated ({error})", file=sys.stderr)
                self.nodes = before
                self.order = order_before
                self.saves = saves_before
                for node in self.order:
                    node.succ = [(l, s) for l, s in node.succ if s.id < len(self.order)]
                # Recount predecessors from the surviving graph.
                for node in self.order:
                    node.preds = 0
                for node in self.order:
                    for _, s in node.succ:
                        s.preds += 1

    def node(self, key):
        if key not in self.nodes:
            node = Node(len(self.order), key)
            self.nodes[key] = node
            self.order.append(node)
        return self.nodes[key]

    # branch_vi_source for one VI index at a conditional-branch node: None to
    # read the register, or the pc whose pre-write value the branch sees.
    def backup_source(self, node, reg):
        if reg == 0 or node.pt is not None:
            return None
        hist = node.hist
        newest = hist[3]
        if newest == "X":
            raise TranslationError(f"pair {node.pc}: unknown write history")
        if newest is None or newest[2] != reg or not newest[3]:
            return None
        source = None
        for age in range(1, 5):
            entry = hist[4 - age]
            if entry == "X":
                raise TranslationError(f"pair {node.pc}: unknown write history")
            if entry is None:
                continue
            if entry[2] == reg:
                if not entry[3]:
                    break
                source = (entry[1], age)
        pc, age = source
        if any(e not in (None, "X") and e[1] == pc for e in hist[4 - age + 1:]):
            raise TranslationError(f"pair {node.pc}: repeated writer in branch window")
        return pc

    def check_backup(self, node):
        pair = self.pairs[node.pc]
        if node.terminal or not pair.cond_branch:
            return
        low = pair.lower
        regs = [low["it"], low["is_"]] if low["op"] in ("Ibeq", "Ibne") else [low["is_"]]
        for reg in regs:
            pc = self.backup_source(node, reg)
            if pc is not None:
                self.saves.add(pc)

    def is_join(self, node):
        return node.entry or node.preds != 1

    # A handoff to the interpreter at a join rebuilds its VI write history
    # from the node's pattern; each recorded writer must have its value saved.
    def transfer_history(self, node):
        if all(e == "X" for e in node.hist):
            return []
        entries = []
        for age in range(4, 0, -1):
            entry = node.hist[4 - age]
            if entry in (None, "X"):
                continue
            entries.append((age, entry[1], entry[2], entry[3]))
        pcs = [e[1] for e in entries]
        if len(pcs) != len(set(pcs)):
            raise TranslationError(f"pair {node.pc}: repeated writer at a join")
        return entries


# ---- C++ emission ----

def vf(index):
    return "kVf0" if index == 0 else f"vf{index}"


def lane_shuffle(lane):
    return f"_MM_SHUFFLE({lane}, {lane}, {lane}, {lane})"


class Context:
    """Static knowledge carried along single-predecessor edges."""

    def __init__(self):
        self.off = 0
        self.pipe = ["p0", "p1", "p2", "p3"]
        self.lm = "lmac"
        self.pq = "dyn"
        self.lastpc = None  # None: the dynamic `lastpc`
        # Lanes of ACC that are clamp3 fixed points (finite normal or signed
        # zero), 4-bit in the same DEST order as the write masks. Reset to
        # unknown at every entry and join, like the rest of this context.
        self.acc = 0

    def copy(self):
        other = Context()
        other.off = self.off
        other.pipe = list(self.pipe)
        other.lm = self.lm
        other.pq = self.pq
        other.lastpc = self.lastpc
        other.acc = self.acc
        return other


def materialize(ctx, out, indent):
    pad = " " * indent
    for index, value in enumerate(ctx.pipe):
        if value != f"p{index}":
            out.append(f"{pad}p{index} = {value};")
    if ctx.lm != "lmac":
        out.append(f"{pad}lmac = {ctx.lm};")
    if ctx.pq == "none":
        out.append(f"{pad}pq_valid = false;")
    elif ctx.pq != "dyn":
        _, bits, ready = ctx.pq
        out.append(f"{pad}pq_valid = true;")
        out.append(f"{pad}pq_bits = {bits};")
        out.append(f"{pad}pq_ready = c + {ready}U;")
    if ctx.off:
        out.append(f"{pad}c += {ctx.off}U;")
    if ctx.lastpc is not None:
        out.append(f"{pad}lastpc = {ctx.lastpc};")


class Emitter:
    def __init__(self, image):
        self.image = image
        self.out = []
        self.emitted = set()
        self.mac_locals = []
        self.fq_locals = []
        self.reach_cache = {}

    def run_length(self, node, memo):
        # Longest run of pairs from `node` until the next join (exclusive).
        if node.id in memo:
            return memo[node.id]
        if node.terminal == "unsupported":
            result = 0
        elif node.terminal == "complete":
            result = 1
        else:
            result = 1 + max((0 if self.image.is_join(s) else self.run_length(s, memo))
                             for _, s in node.succ)
        memo[node.id] = result
        return result

    def emit(self):
        image = self.image
        out = self.out
        nodes = [n for n in image.order]
        memo = {}
        # Handoffs at joins rebuild the VI write history from saved values.
        self.transfers = {}
        for node in nodes:
            if image.is_join(node):
                self.transfers[node.id] = image.transfer_history(node)
                image.saves.update(pc for _, pc, _, _ in self.transfers[node.id])
        used_vf = set()
        used_vi = set()
        for node in nodes:
            pair = image.pairs[node.pc]
            if pair.unsupported:
                continue
            up = pair.upper
            used_vf.update((up["fs"], up["ft"], up["fd"]))
            if pair.lower:
                low = pair.lower
                if low["op"] in ("Lq", "Lqi", "Lqd", "Mfir", "Move", "Mr32", "Sqrt", "Rsqrt", "Div"):
                    used_vf.add(low["ft"])
                if low["op"] in ("Sq", "Sqi", "Sqd", "Move", "Mr32", "Mtir", "Rsqrt", "Div"):
                    used_vf.add(low["fs"])
                used_vi.update(r for r in (low["it"], low["is_"]) if r < 16)
                if low["op"] in INDEX5_OPS:
                    used_vi.add(low["fd"])
        used_vf.discard(0)
        used_vi.discard(0)
        used_vi = {r for r in used_vi if r < 16}
        self.body = []
        work = []
        for node in nodes:
            if node.entry:
                work.append((node, Context()))
        # Joins are emitted with the canonical context; single-predecessor
        # nodes with the context of their only incoming edge.
        for node in nodes:
            if self.image.is_join(node) and not node.entry:
                work.append((node, Context()))
        while work:
            node, ctx = work.pop(0)
            chain = [(node, ctx)]
            while chain:
                current, cctx = chain.pop()
                if current.id in self.emitted:
                    continue
                self.emitted.add(current.id)
                pending = self.emit_node(current, cctx, memo)
                # Emit the fall-through successor next, the rest later.
                for index, item in enumerate(pending):
                    if index == 0:
                        chain.append(item)
                    else:
                        work.insert(0, item)
        # A MAC value nothing reads (no FMAND/FMOR/FMEQ, join or exit sees it)
        # is never formed.
        text = "\n".join(self.body)
        counts = {}
        for match in re.finditer(r"\bmac\d+\b", text):
            counts[match.group(0)] = counts.get(match.group(0), 0) + 1
        dead = {local for local in self.mac_locals if counts.get(local, 0) <= 1}
        assignment = re.compile(r"^ *(mac[0-9]+) = vu0_native_mac")
        self.body = [line for line in self.body
                     if not ((found := assignment.match(line)) and found.group(1) in dead)]
        self.mac_locals = [local for local in self.mac_locals if local not in dead]
        name = f"vu0_native_{image.name}"
        out.append(f"// image {image.name}: entries {image.compiled_entries}, {len(nodes)} nodes")
        out.append(f"std::optional<Vu0MicroRunResult> {name}(Vu0Machine& m, const std::size_t limit) {{")
        out.append("    auto& state = m.state;")
        if "(d + " in text:
            out.append("    std::byte* const d = m.memory.vu0_data_local_bytes();")
        out.append("    const __m128i kVf0 = _mm_setr_epi32(0, 0, 0, 0x3f800000);")
        for r in sorted(used_vf):
            out.append(f"    __m128i vf{r} = vu0_detail::simd_load(state.read_vu0_vector({r}U));")
        out.append("    __m128i acc = vu0_detail::simd_load(state.vu0_accumulator());")
        for r in sorted(used_vi):
            out.append(f"    std::uint16_t vi{r} = state.read_vu0_integer({r}U);")
        out.append("    std::uint32_t ireg = state.read_vu0_control(kControlI);")
        out.append("    std::uint32_t q = state.read_vu0_control(kControlQ);")
        out.append("    std::uint32_t status = m.status;")
        out.append("    std::uint32_t p0 = m.mac_pipeline[0];")
        out.append("    std::uint32_t p1 = m.mac_pipeline[1];")
        out.append("    std::uint32_t p2 = m.mac_pipeline[2];")
        out.append("    std::uint32_t p3 = m.mac_pipeline[3];")
        out.append("    std::uint16_t lmac = m.logical_mac;")
        out.append("    bool mac_touched = false;")
        out.append("    Vu0NativeSticky sticky{};")
        out.append("    bool pq_valid = false;")
        out.append("    std::uint32_t pq_bits = 0U;")
        out.append("    std::size_t pq_ready = 0U;")
        out.append("    std::size_t c = 0U;")
        out.append("    int lastpc = -1;")
        out.append("    int xpc = 0;")
        if "goto x_unsupported;" in text:
            out.append("    Vu0MicroSlot xslot = Vu0MicroSlot::None;")
        for pc in sorted(image.saves):
            out.append(f"    std::uint16_t save{pc} = 0U;")
        for local in self.mac_locals:
            out.append(f"    std::uint16_t {local} = 0U;")
        for local in self.fq_locals:
            out.append(f"    std::uint32_t {local} = 0U;")
        out.append("    switch (m.pc) {")
        for node in nodes:
            if node.entry:
                out.append(f"    case {node.pc}U: goto n{node.id};")
        out.append("    default: return std::nullopt;")
        out.append("    }")
        out.extend(self.body)
        # Exits.
        def spill():
            lines = []
            for r in sorted(used_vf):
                lines.append(f"    state.write_vu0_vector({r}U, vu0_native_gpr(vf{r}));")
            lines.append("    state.set_vu0_accumulator(vu0_native_gpr(acc));")
            for r in sorted(used_vi):
                lines.append(f"    state.write_vu0_integer({r}U, vi{r});")
            lines.append("    state.write_vu0_control(kControlI, ireg);")
            lines.append("    m.committed = c;")
            lines.append("    m.status = vu0_native_status(status, mac_touched, lmac, sticky);")
            lines.append("    if (lastpc >= 0) {")
            lines.append("        state.write_vu0_control(kControlTpc, static_cast<std::uint32_t>(lastpc));")
            lines.append(f"        m.last_lower = kVu0NativeWords_{image.name}[lastpc * 2];")
            lines.append(f"        m.last_upper = kVu0NativeWords_{image.name}[lastpc * 2 + 1];")
            lines.append("    }")
            return lines
        body_text = "\n".join(self.body)
        if "goto x_complete;" in body_text:
            out.append("x_complete:")
            out.extend(spill())
            out.append("    state.write_vu0_control(kControlQ, pq_valid ? pq_bits : q);")
            out.append("    m.mac_pipeline = {lmac, lmac, lmac, lmac};")
            out.append("    m.pc = static_cast<std::uint16_t>(lastpc);")
            out.append("    return vu0_native_complete(m);")
        if "goto x_unsupported;" in body_text:
            out.append("x_unsupported:")
            out.extend(spill())
            out.append("    state.write_vu0_control(kControlQ, pq_valid ? pq_bits : q);")
            out.append("    m.mac_pipeline = {p0, p1, p2, p3};")
            out.append(f"    return vu0_native_unsupported(m, xslot, static_cast<std::uint16_t>(xpc), "
                       f"kVu0NativeWords_{image.name}[xpc * 2], kVu0NativeWords_{image.name}[xpc * 2 + 1]);")
        out.append("x_transfer:")
        out.extend(spill())
        out.append("    state.write_vu0_control(kControlQ, q);")
        out.append("    if (pq_valid) {")
        out.append("        m.pending_q = PendingQ{.bits = pq_bits, .ready_cycle = pq_ready};")
        out.append("    }")
        out.append("    m.mac_pipeline = {p0, p1, p2, p3};")
        out.append("    m.logical_mac = lmac;")
        out.append("    m.pc = static_cast<std::uint16_t>(xpc);")
        out.append("    return std::nullopt;")
        out.append("}")
        out.append("")

    def emit_node(self, node, ctx, memo):
        image = self.image
        pair = image.pairs[node.pc]
        b = self.body
        b.append(f"n{node.id}: // {pair.describe()}  pt={node.pt} ed={int(node.ed)}")
        pending = []
        if image.is_join(node):
            assert ctx.off == 0 and ctx.pq == "dyn" and ctx.lastpc is None
            length = self.run_length(node, memo)
            b.append(f"    if (c + {length}U > limit) [[unlikely]] {{")
            b.append(f"        xpc = {node.pc};")
            if node.pt is not None:
                b.append(f"        m.pending_branch_target = static_cast<std::uint16_t>({node.pt});")
            if node.ed:
                b.append("        m.e_delay_pending = true;")
            history = self.transfers[node.id]
            for index, (age, pc, reg, eligible) in enumerate(history):
                b.append(f"        m.vi_history[{index}] = Vu0ViWrite{{.issue = c - {age}U, .reg = {reg}U, "
                         f".previous = save{pc}, .eligible = {'true' if eligible else 'false'}}};")
            if history:
                b.append(f"        m.vi_history_next = {len(history) & 3}U;")
                b.append(f"        m.vi_history_size = {len(history)}U;")
            b.append("        goto x_transfer;")
            b.append("    }")
        if node.terminal == "unsupported":
            b.append("    {")
            materialize(ctx, b, 8)
            b.append(f"        xpc = {node.pc};")
            b.append(f"        xslot = Vu0MicroSlot::{pair.unsupported};")
            b.append("        goto x_unsupported;")
            b.append("    }")
            return pending
        b.append("    {")
        cycle = ctx.off  # committed at this pair = c + cycle
        nctx = ctx.copy()
        up = pair.upper
        low = pair.lower
        dest = (pair.upper_word >> 21) & 0xF
        # ---- upper (staged) ----
        upper_target = None  # ("vf", index) / ("acc",)
        mac_local = None
        info = FMAC.get(up["op"])

        def read_q():
            if nctx.pq == "none" or nctx.pq == "dyn":
                if nctx.pq == "dyn":
                    b.append(f"        if (pq_valid && pq_ready <= c + {cycle}U) {{")
                    b.append("            q = pq_bits;")
                    b.append("            pq_valid = false;")
                    b.append("        }")
                return "q"
            _, bits, ready = nctx.pq
            if ready <= cycle:
                b.append(f"        q = {bits};")
                nctx.pq = "none"
            return "q"

        if info is not None:
            kind, operation, operand, writes_acc = info
            left = vf(up["fs"])
            if operand == "Ft":
                right = vf(up["ft"])
            elif operand == "FtLane":
                right = f"_mm_shuffle_epi32({vf(up['ft'])}, {lane_shuffle(up['lane'])})"
            elif operand == "Q":
                right = f"_mm_set1_epi32(static_cast<int>({read_q()}))"
            elif operand == "I":
                right = "_mm_set1_epi32(static_cast<int>(ireg))"
            else:
                left = f"_mm_shuffle_epi32({vf(up['fs'])}, _MM_SHUFFLE(3, 0, 2, 1))"
                right = f"_mm_shuffle_epi32({vf(up['ft'])}, _MM_SHUFFLE(3, 1, 0, 2))"
            mask = 0xE if operand == "Cross" else dest
            if writes_acc:
                base = "acc"
                upper_target = ("acc",)
            else:
                base = vf(up["fd"])
                upper_target = ("vf", up["fd"])
            if kind == "A":
                # MADD/MSUB consume the incoming ACC facts, never their own
                # newly written result facts, so this reads nctx before the
                # write below updates it.
                canonical_acc = (operation in ("MultiplyAdd", "MultiplySubtract")
                                 and nctx.acc == 0xF)
                entry = ("vu0_native_fmac_canonical_acc" if canonical_acc
                         else "vu0_native_fmac")
                b.append(f"        const auto u = {entry}<vu0_detail::Vu0Fmac::{operation}, {mask}U>("
                         f"{left}, {right}, acc, {base});")
                mac_local = f"mac{node.id}"
                self.mac_locals.append(mac_local)
            else:
                maximum = "true" if kind == "Max" else "false"
                b.append(f"        const auto u = vu0_native_min_max<{maximum}, {mask}U>({left}, {right}, {base});")
            if writes_acc:
                # The committed ACC lanes are this FMAC's result for the written
                # lanes (always chop output) and the untouched old ACC for the
                # rest, so the write mask adds exactly those facts.
                nctx.acc |= mask
        elif up["op"] in ("Vitof", "Vftoi"):
            function = "vu0_native_itof" if up["op"] == "Vitof" else "vu0_native_ftoi"
            b.append(f"        const auto u = {function}<{up['fraction']}U, {dest}U>({vf(up['fs'])}, {vf(up['fd'])});")
            upper_target = ("vf", up["fd"])
        else:
            assert up["op"] == "Nop", up["op"]
        if upper_target == ("vf", 0):
            upper_target = None  # VF0 ignores writes
        # ---- lower (staged against pre-pair state) ----
        lower_vf = None  # (index, expression)
        vi_write = None  # (reg, expression)
        stores = []
        branch_cond = None
        fdiv = None  # (expression computing bits with flags variable, latency)
        waitq = False
        if low is not None:
            op = low["op"]

            def vi(r):
                return "0" if r == 0 else f"vi{r}"

            def address(base_reg, imm):
                if base_reg == 0:
                    return f"{(imm * 16) & 0xFFF}U"
                return (f"((static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(vi{base_reg})) + "
                        f"({imm})) * 16U) & 0x0fffU)")

            def post_address(reg, decrement):
                base = vi(reg)
                if decrement:
                    eff = f"static_cast<std::uint16_t>({base} - 1)"
                else:
                    eff = f"static_cast<std::uint16_t>({base})"
                return eff, f"((static_cast<std::uint32_t>({eff}) * 16U) & 0x0fffU)"

            if op == "Nop":
                pass
            elif op in ("Lq", "Lqi", "Lqd"):
                if op == "Lq":
                    addr = address(low["is_"], low["imm"])
                else:
                    eff, addr = post_address(low["is_"], op == "Lqd")
                    if low["is_"] != 0:
                        b.append(f"        const std::uint16_t eff = {eff};")
                        addr = "((static_cast<std::uint32_t>(eff) * 16U) & 0x0fffU)"
                        vi_write = (low["is_"], "eff" if op == "Lqd" else "static_cast<std::uint16_t>(eff + 1U)")
                b.append(f"        const std::uint32_t la = {addr};")
                if low["ft"] != 0:
                    lower_vf = (low["ft"], f"vu0_native_load<{low['mask']}U>(d + la, {vf(low['ft'])})")
            elif op in ("Sq", "Sqi", "Sqd"):
                if op == "Sq":
                    addr = address(low["it"], low["imm"])
                else:
                    eff, addr = post_address(low["it"], op == "Sqd")
                    if low["it"] != 0:
                        b.append(f"        const std::uint16_t eff = {eff};")
                        addr = "((static_cast<std::uint32_t>(eff) * 16U) & 0x0fffU)"
                        vi_write = (low["it"], "eff" if op == "Sqd" else "static_cast<std::uint16_t>(eff + 1U)")
                b.append(f"        const std::uint32_t sa = {addr};")
                b.append(f"        const __m128i sv = {vf(low['fs'])};")
                stores.append(f"vu0_native_store<{low['mask']}U>(d + sa, sv);")
            elif op == "Isw":
                b.append(f"        const std::uint32_t sa = {address(low['is_'], low['imm'])};")
                b.append(f"        const std::uint32_t sw = {vi(low['it'])};")
                stores.append(f"vu0_native_store_words<{low['mask']}U>(d + sa, sw);")
            elif op == "Ilw":
                if low["mask"] and low["it"]:
                    last_lane = max(l for l in range(4) if low["mask"] & (8 >> l))
                    b.append(f"        const std::uint32_t la = {address(low['is_'], low['imm'])};")
                    vi_write = (low["it"], f"vu0_native_u16(d + la + {last_lane * 4}U)")
            elif op in ("Iaddiu", "Isubiu"):
                imm15 = ((pair.lower_word >> 21) & 0xF) << 11 | (pair.lower_word & 0x7FF)
                sign = "+" if op == "Iaddiu" else "-"
                vi_write = (low["it"], f"static_cast<std::uint16_t>(static_cast<std::uint32_t>({vi(low['is_'])}) {sign} {imm15}U)")
            elif op in ("Fmand", "Fmor", "Fmeq"):
                front = f"static_cast<std::uint16_t>({ctx.pipe[0]})"
                if op == "Fmand":
                    expr = f"static_cast<std::uint16_t>({vi(low['is_'])} & {front})"
                elif op == "Fmor":
                    expr = f"static_cast<std::uint16_t>({vi(low['is_'])} | {front})"
                else:
                    expr = f"static_cast<std::uint16_t>({vi(low['is_'])} == {front} ? 1U : 0U)"
                vi_write = (low["it"], expr)
            elif op in ("Iadd", "Isub", "Iand", "Ior"):
                symbol = {"Iadd": "+", "Isub": "-", "Iand": "&", "Ior": "|"}[op]
                vi_write = (low["fd"], f"static_cast<std::uint16_t>({vi(low['is_'])} {symbol} {vi(low['it'])})")
            elif op == "Iaddi":
                imm5 = sext(low["fd"], 5)
                vi_write = (low["it"], f"static_cast<std::uint16_t>({vi(low['is_'])} + ({imm5}))")
            elif op == "Mtir":
                vi_write = (low["it"], f"static_cast<std::uint16_t>(vu0_native_lane<{low['is_']}>({vf(low['fs'])}) & 0xffffU)")
            elif op == "Mfir":
                if low["ft"] != 0:
                    lower_vf = (low["ft"], f"vu0_native_select<{low['mask']}U>(_mm_set1_epi32(static_cast<std::int16_t>({vi(low['is_'])})), {vf(low['ft'])})")
            elif op in ("Move", "Mr32"):
                if low["ft"] != 0:
                    source = vf(low["fs"])
                    if op == "Mr32":
                        source = f"_mm_shuffle_epi32({source}, _MM_SHUFFLE(0, 3, 2, 1))"
                    lower_vf = (low["ft"], f"vu0_native_select<{low['mask']}U>({source}, {vf(low['ft'])})")
            elif op == "Sqrt":
                fdiv = (f"vu0_square_root_bits(vu0_native_lane<{low['it']}>({vf(low['ft'])}), fflags)", DIV_SQRT_LATENCY)
            elif op == "Rsqrt":
                fdiv = (f"vu0_reciprocal_square_root_bits(vu0_native_lane<{low['is_']}>({vf(low['fs'])}), "
                        f"vu0_native_lane<{low['it']}>({vf(low['ft'])}), fflags)", RSQRT_LATENCY)
            elif op == "Div":
                fdiv = (f"vu0_divide_bits(vu0_native_lane<{low['is_']}>({vf(low['fs'])}), "
                        f"vu0_native_lane<{low['it']}>({vf(low['ft'])}), fflags)", DIV_SQRT_LATENCY)
            elif op == "Waitq":
                waitq = True
            elif op == "B" or (op in COND_BRANCHES and node.terminal == "complete"):
                pass
            elif op in COND_BRANCHES:
                def source(reg):
                    if reg == 0:
                        return "std::uint16_t{0U}"
                    pc = image.backup_source(node, reg)
                    return f"save{pc}" if pc is not None else f"vi{reg}"
                if op in ("Ibeq", "Ibne"):
                    relation = "==" if op == "Ibeq" else "!="
                    branch_cond = f"{source(low['it'])} {relation} {source(low['is_'])}"
                else:
                    relation = {"Iblt": "<", "Ibgt": ">", "Ible": "<=", "Ibge": ">="}[op]
                    branch_cond = f"static_cast<std::int16_t>({source(low['is_'])}) {relation} 0"
            else:
                raise AssertionError(op)
        if vi_write is not None and vi_write[0] == 0:
            vi_write = None
        if fdiv is not None:
            fq = f"fq{node.id}"
            self.fq_locals.append(fq)
            b.append("        std::uint32_t fflags = 0U;")
            b.append(f"        {fq} = {fdiv[0]};")
        if branch_cond is not None:
            b.append(f"        const bool taken = {branch_cond};")
        if lower_vf is not None:
            b.append(f"        const __m128i lv = {lower_vf[1]};")
        if vi_write is not None:
            b.append(f"        const std::uint16_t lw = {vi_write[1]};")
        # ---- commit ----
        if upper_target == ("acc",):
            b.append("        acc = u.value;" if info and info[0] == "A" else "        acc = u;")
        elif upper_target is not None:
            value = "u.value" if info and info[0] == "A" else "u"
            b.append(f"        vf{upper_target[1]} = {value};")
        if lower_vf is not None and not (upper_target is not None and upper_target[0] == "vf" and upper_target[1] == lower_vf[0]):
            b.append(f"        vf{lower_vf[0]} = lv;")
        if vi_write is not None:
            if pair.pc in image.saves:
                b.append(f"        save{pair.pc} = vi{vi_write[0]};")
            b.append(f"        vi{vi_write[0]} = lw;")
        for store in stores:
            b.append(f"        {store}")
        if pair.i_bit:
            b.append(f"        ireg = 0x{pair.lower_word:08x}U;")
        if mac_local is not None:
            mask = 0xE if info[2] == "Cross" else dest
            b.append(f"        {mac_local} = vu0_native_mac<{mask}U>(u);")
            b.append(f"        vu0_native_sticky<{mask}U>(sticky, u);")
            b.append("        mac_touched = true;")
            nctx.lm = mac_local
        if fdiv is not None:
            b.append("        status = (status & ~0x0030U) | fflags | (fflags << 6U);")
        nctx.pipe = nctx.pipe[1:] + [nctx.lm]
        if waitq:
            if nctx.pq == "dyn":
                b.append("        if (pq_valid) {")
                b.append("            q = pq_bits;")
                b.append("            pq_valid = false;")
                b.append("        }")
            elif nctx.pq != "none":
                b.append(f"        q = {nctx.pq[1]};")
            nctx.pq = "none"
        if fdiv is not None:
            if nctx.pq == "dyn":
                b.append("        if (pq_valid) {")
                b.append("            q = pq_bits;")
                b.append("        }")
            elif nctx.pq != "none":
                b.append(f"        q = {nctx.pq[1]};")
            nctx.pq = ("st", f"fq{node.id}", cycle + fdiv[1])
        nctx.off = cycle + 1
        nctx.lastpc = node.pc
        if node.terminal == "complete":
            materialize(nctx, b, 8)
            b.append("        goto x_complete;")
            b.append("    }")
            return pending
        edges = []
        for label, succ in node.succ:
            edges.append((label, succ))
        # Taken edge first under its condition, then the fall-through.
        for label, succ in edges:
            conditional = len(edges) == 2 and label == "taken"
            pad = 12 if conditional else 8
            if conditional:
                b.append("        if (taken) {")
            if image.is_join(succ):
                materialize(nctx, b, pad)
                b.append(" " * pad + f"goto n{succ.id};")
            else:
                b.append(" " * pad + f"goto n{succ.id};")
                pending.append((succ, nctx.copy()))
            if conditional:
                b.append("        }")
        b.append("    }")
        # The fall-through successor (last edge) first.
        pending.reverse()
        return pending


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--min-launches", type=int, default=1,
                        help="skip (image, entry) launches with fewer completed launches in the manifest")
    parser.add_argument("--listing", action="store_true", help="print the executed pairs of each image")
    options = parser.parse_args(argv)

    entries = {}
    manifest = options.capture / "manifest.tsv"
    for line in manifest.read_text().splitlines()[1:]:
        image, entry, count, _pairs = line.split("\t")
        if int(count) >= options.min_launches:
            entries.setdefault(image, set()).add(int(entry))

    out = ["// Generated by tools/vu0_aot_generator.py from captured VU0 MicroMem images.",
           "// Game-derived: local use only, never distribute.", ""]
    table = []
    total_nodes = 0
    for image_name in sorted(entries):
        data = (options.capture / f"vu0-{image_name}.bin").read_bytes()
        if len(data) != PAIR_COUNT * 8:
            sys.exit(f"{image_name}: MicroMem image is not 4 KiB")
        image = Image(image_name, data, entries[image_name])
        if options.listing:
            executed = sorted({int(x) for x in (options.capture / f"pairs-{image_name}.txt").read_text().split()})
            print(f"== {image_name} entries {image.entries}")
            for pc in executed:
                print(image.pairs[pc].describe())
        image.explore()
        if not image.compiled_entries:
            continue
        # Compile-time proof that this decoder matches the interpreter's.
        used = sorted({pc for e in image.compiled_entries for pc in image.reach[e]})
        out.append(f"constexpr std::uint32_t kVu0NativeWords_{image_name}[{PAIR_COUNT * 2}] = {{")
        for row in range(0, PAIR_COUNT * 2, 8):
            out.append("    " + ", ".join(f"0x{w:08x}U" for w in image.words[row:row + 8]) + ",")
        out.append("};")
        for pc in used:
            pair = image.pairs[pc]
            up = pair.upper
            low = pair.lower_decode
            if up is None:
                upper_check = "false"
            else:
                upper_check = (f"true, UpperOp::{up['op']}, {up['lane']}, {up['ft']}, {up['fs']}, "
                               f"{up['fd']}, {up['fraction']}")
            if low is None:
                lower_check = "false"
            else:
                lower_check = (f"true, LowerOp::{low['op']}, {low['mask']}, {low['it']}, {low['is_']}, "
                               f"{low['ft']}, {low['fs']}, {low['fd']}, {low['imm']}")
            if up is None:
                up_args = "false, UpperOp::Nop, 0, 0, 0, 0, 0"
            else:
                up_args = upper_check
            if low is None:
                low_args = "false, LowerOp::Nop, 0, 0, 0, 0, 0, 0, 0"
            else:
                low_args = lower_check
            out.append(f"static_assert(vu0_native_decode_matches(0x{pair.lower_word:08x}U, 0x{pair.upper_word:08x}U, "
                       f"{up_args}, {low_args}));")
        emitter = Emitter(image)
        emitter.emit()
        out.extend(emitter.out)
        total_nodes += len(image.order)
        for entry in image.compiled_entries:
            ranges = []
            for pc in image.reach[entry]:
                if ranges and ranges[-1][0] + ranges[-1][1] == pc:
                    ranges[-1][1] += 1
                else:
                    ranges.append([pc, 1])
            out.append(f"constexpr std::uint16_t kVu0NativeRanges_{image_name}_{entry}[] = {{"
                       + ", ".join(f"{s}, {n}" for s, n in ranges) + "};")
            table.append(f"    {{0x{image_name}ULL, {entry}U, vu0_native_{image_name}, kVu0NativeWords_{image_name}, "
                         f"kVu0NativeRanges_{image_name}_{entry}, {len(ranges)}U}},")
        print(f"{image_name}: entries {image.compiled_entries} of {image.entries}, {len(image.order)} nodes, "
              f"{len(used)} pairs, {len(image.saves)} saved writers", file=sys.stderr)
    out.append("constexpr Vu0NativeEntry kVu0NativeEntries[] = {")
    out.extend(table)
    if not table:
        out.append("    {0U, 0U, nullptr, nullptr, nullptr, 0U},")
    out.append("};")
    options.output.parent.mkdir(parents=True, exist_ok=True)
    options.output.write_text("\n".join(out) + "\n")
    print(f"entries={len(table)} nodes={total_nodes}")


if __name__ == "__main__":
    main()
