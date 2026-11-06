# Static recompiler: translates arm64 functions from the game's il2cpp section
# into C++ that runs on recomp::Ctx (see recomp/runtime/recomp_ctx.h).
#
# usage: python3 -I arm64_recomp.py <UnityFramework> <function_table.json> <out_dir>
#            [--only addr,addr,...] [--split N]
#
# The function table comes from tools/analysis/function_table.py.
import sys, json, argparse, collections, os, re
import lief, capstone
from capstone import arm64 as A

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import abi

REG, IMM, MEM, FP = A.ARM64_OP_REG, A.ARM64_OP_IMM, A.ARM64_OP_MEM, A.ARM64_OP_FP

COND = {
    A.ARM64_CC_EQ: "c.z", A.ARM64_CC_NE: "!c.z",
    A.ARM64_CC_HS: "c.c", A.ARM64_CC_LO: "!c.c",
    A.ARM64_CC_MI: "c.n", A.ARM64_CC_PL: "!c.n",
    A.ARM64_CC_VS: "c.vf", A.ARM64_CC_VC: "!c.vf",
    A.ARM64_CC_HI: "(c.c && !c.z)", A.ARM64_CC_LS: "(!c.c || c.z)",
    A.ARM64_CC_GE: "(c.n == c.vf)", A.ARM64_CC_LT: "(c.n != c.vf)",
    A.ARM64_CC_GT: "(!c.z && c.n == c.vf)", A.ARM64_CC_LE: "(c.z || c.n != c.vf)",
    A.ARM64_CC_AL: "true", A.ARM64_CC_NV: "true",
}

SFT_LSL, SFT_LSR, SFT_ASR, SFT_ROR = A.ARM64_SFT_LSL, A.ARM64_SFT_LSR, A.ARM64_SFT_ASR, A.ARM64_SFT_ROR
SFT_MSL = A.ARM64_SFT_MSL
EXT = {
    A.ARM64_EXT_UXTB: ("uint8_t", False), A.ARM64_EXT_UXTH: ("uint16_t", False),
    A.ARM64_EXT_UXTW: ("uint32_t", False), A.ARM64_EXT_UXTX: ("uint64_t", False),
    A.ARM64_EXT_SXTB: ("int8_t", True), A.ARM64_EXT_SXTH: ("int16_t", True),
    A.ARM64_EXT_SXTW: ("int32_t", True), A.ARM64_EXT_SXTX: ("int64_t", True),
}


class Unsupported(Exception):
    pass


class Reg:
    """A decoded register operand."""

    def __init__(self, name):
        self.name = name
        if name in ("sp", "wsp"):
            self.kind, self.num, self.bits = "sp", 31, 64 if name == "sp" else 32
        elif name in ("xzr", "wzr"):
            self.kind, self.num, self.bits = "zr", 31, 64 if name == "xzr" else 32
        elif name == "fp":
            self.kind, self.num, self.bits = "x", 29, 64
        elif name == "lr":
            self.kind, self.num, self.bits = "x", 30, 64
        elif name[0] in "xw":
            self.kind, self.num, self.bits = "x", int(name[1:]), 64 if name[0] == "x" else 32
        elif name[0] in "bhsdqv":
            self.kind, self.num = "v", int(name[1:])
            self.bits = {"b": 8, "h": 16, "s": 32, "d": 64, "q": 128, "v": 128}[name[0]]
        else:
            raise Unsupported("register " + name)

    @property
    def is64(self):
        return self.bits == 64


class Emitter:
    def __init__(self, md, func_addrs, image_end):
        self.md = md
        self.func_addrs = func_addrs
        self.image_end = image_end
        self.unsupported = collections.Counter()

    # ---- operand helpers -------------------------------------------------

    def reg(self, op):
        return Reg(self.md.reg_name(op.reg))

    def read(self, r):
        """C++ expression reading an integer register with its natural width."""
        if r.kind == "zr":
            return "0ull" if r.is64 else "0u"
        if r.kind == "sp":
            return "c.sp" if r.is64 else "uint32_t(c.sp)"
        if r.kind != "x":
            raise Unsupported("int read of " + r.name)
        return f"c.x[{r.num}]" if r.is64 else f"uint32_t(c.x[{r.num}])"

    def write(self, r, expr):
        if r.kind == "zr":
            return f"(void)({expr});"
        if r.kind == "sp":
            return f"c.sp = uint64_t({expr});" if r.is64 else f"c.sp = uint32_t({expr});"
        if r.kind != "x":
            raise Unsupported("int write of " + r.name)
        return f"c.x[{r.num}] = uint64_t({expr});" if r.is64 else f"c.x[{r.num}] = uint32_t({expr});"

    def utype(self, r):
        return "uint64_t" if r.is64 else "uint32_t"

    def stype(self, r):
        return "int64_t" if r.is64 else "int32_t"

    def shifted(self, op, width64):
        """Second source operand: register (shifted/extended) or immediate."""
        ut = "uint64_t" if width64 else "uint32_t"
        st = "int64_t" if width64 else "int32_t"
        bits = 64 if width64 else 32
        if op.type == IMM:
            v = op.imm
            if op.shift.type == SFT_LSL and op.shift.value:
                v <<= op.shift.value
            v &= (1 << bits) - 1
            return f"{ut}({v:#x}ull)"
        if op.type != REG:
            raise Unsupported("operand type")
        r = self.reg(op)
        if op.ext:
            t, _ = EXT[op.ext]
            e = f"{ut}({st}({t}({self.read(r)})))" if EXT[op.ext][1] else f"{ut}({t}({self.read(r)}))"
            amount = op.shift.value if op.shift.type == SFT_LSL else 0
            return f"({e} << {amount})" if amount else e
        e = f"{ut}({self.read(r)})"
        s, amount = op.shift.type, op.shift.value
        if not s or not amount:
            return e
        if s == SFT_LSL:
            return f"({e} << {amount})"
        if s == SFT_LSR:
            return f"({e} >> {amount})"
        if s == SFT_ASR:
            return f"{ut}({st}({e}) >> {amount})"
        if s == SFT_ROR:
            return f"Ror{bits}({e}, {amount})"
        raise Unsupported("shift type")

    def address(self, ins, mem_index):
        """Returns (setup_lines, addr_expr, post_lines) for a memory operand."""
        ops = ins.operands
        op = ops[mem_index]
        base = Reg(self.md.reg_name(op.mem.base))
        base_expr = self.read(base)
        parts = [base_expr]
        if op.mem.index:
            idx = Reg(self.md.reg_name(op.mem.index))
            if op.ext:
                t, signed = EXT[op.ext]
                e = f"uint64_t(int64_t({t}({self.read(idx)})))" if signed else f"uint64_t({t}({self.read(idx)}))"
            else:
                e = f"uint64_t({self.read(idx)})"
            if op.shift.type == SFT_LSL and op.shift.value:
                e = f"({e} << {op.shift.value})"
            parts.append(e)
        disp = op.mem.disp
        post = mem_index + 1 < len(ops) and ops[mem_index + 1].type == IMM
        setup, after = [], []
        if post:
            # post-index: access at base, then base += imm
            imm = ops[mem_index + 1].imm
            setup.append(f"uint64_t ea = {base_expr};")
            after.append(self.write(base, f"ea + uint64_t({imm}ll)"))
        else:
            if disp:
                parts.append(f"uint64_t({disp}ll)")
            setup.append(f"uint64_t ea = {' + '.join(parts)};")
            if ins.writeback:
                after.append(self.write(base, "ea"))
        return setup, "ea", after

    def vreg(self, r):
        return f"c.v[{r.num}]"

    def fread(self, r):
        if r.bits == 32:
            return f"c.v[{r.num}].f32[0]"
        if r.bits == 64:
            return f"c.v[{r.num}].f64[0]"
        raise Unsupported("fp width " + r.name)

    def fwrite(self, r, expr):
        if r.bits == 32:
            return f"SetS(c.v[{r.num}], float({expr}));"
        if r.bits == 64:
            return f"SetD(c.v[{r.num}], double({expr}));"
        raise Unsupported("fp width " + r.name)

    # ---- instruction translation ------------------------------------------

    def translate(self, ins, func):
        m = ins.mnemonic
        ops = ins.operands
        pc = ins.address
        nxt = pc + ins.size

        def r(i):
            return self.reg(ops[i])

        def target_stmt(t):
            if func["start"] <= t < func["end"]:
                return f"goto L_{t:x};"
            if t in self.func_addrs:
                return f"f_{t:x}(c); return;"
            return f"CallImport(c, {t:#x}); return;"

        # -- control flow
        if m == "nop":
            return []
        if m == "b":
            return [target_stmt(ops[0].imm)]
        if m.startswith("b.") or (m == "b" and ins.cc):
            return [f"if ({COND[ins.cc]}) {{ {target_stmt(ops[0].imm)} }}"]
        if m == "bl":
            t = ops[0].imm
            call = f"f_{t:x}(c);" if t in self.func_addrs else f"CallImport(c, {t:#x});"
            return [f"c.x[30] = g_imageBase + {nxt:#x};", call]
        if m == "blr":
            if pc in func["native_calls"]:
                # icall / P/Invoke target resolved by the runtime: typed native call
                ret, params = func["native_calls"][pc]
                body = abi.native_call(ret, params, "t")
                return ["{ uint64_t t = " + self.read(r(0)) + "; " + " ".join(body) + " }"]
            return [f"{{ uint64_t t = {self.read(r(0))}; c.x[30] = g_imageBase + {nxt:#x}; CallIndirect(c, t); }}"]
        if m == "br" and pc in func["native_calls"]:
            # tail call into the resolved native function
            ret, params = func["native_calls"][pc]
            body = abi.native_call(ret, params, "t")
            return ["{ uint64_t t = " + self.read(r(0)) + "; " + " ".join(body) + " return; }"]
        if m == "br":
            if pc in func.get("table_branches", ()):
                func["has_br"] = True
                return [f"{{ uint64_t t = {self.read(r(0))}; JUMP_INDIRECT(t); }}"]
            # tail call through a register (vtable, delegate, interface)
            return [f"{{ uint64_t t = {self.read(r(0))}; CallIndirect(c, t); return; }}"]
        if m == "ret":
            if ops and r(0).num != 30:
                raise Unsupported("ret to non-lr")
            return ["return;"]
        if m in ("cbz", "cbnz"):
            cmp = "==" if m == "cbz" else "!="
            return [f"if ({self.read(r(0))} {cmp} 0) {{ {target_stmt(ops[1].imm)} }}"]
        if m in ("tbz", "tbnz"):
            cmp = "==" if m == "tbz" else "!="
            reg = r(0)
            return [f"if ((({self.utype(reg)}({self.read(reg)}) >> {ops[1].imm}) & 1) {cmp} 0) {{ {target_stmt(ops[2].imm)} }}"]
        if m in ("brk", "udf"):
            return [f"Trap(c, {pc:#x}, {ops[0].imm if ops else 0});"]

        simd = self.simd(ins, m, ops)
        if simd is not None:
            return simd

        # -- addresses and moves
        if m == "adrp" or m == "adr":
            return [self.write(r(0), f"g_imageBase + {ops[1].imm:#x}")]
        if m == "mov":
            return self.mov(ins, ops)
        if m == "movk":
            d = r(0)
            sh = ops[1].shift.value if ops[1].shift.type else 0
            mask = ((0xFFFF << sh) ^ ((1 << d.bits) - 1)) & ((1 << d.bits) - 1)
            return [self.write(d, f"({self.read(d)} & {mask:#x}ull) | ({ops[1].imm:#x}ull << {sh})")]
        if m in ("movz", "movn"):
            d = r(0)
            v = ops[1].imm << (ops[1].shift.value if ops[1].shift.type else 0)
            if m == "movn":
                v = ~v
            return [self.write(d, f"{v & ((1 << d.bits) - 1):#x}ull")]

        # -- integer arithmetic
        if m in ("add", "sub", "adds", "subs") and ops[0].type == REG and Reg(self.md.reg_name(ops[0].reg)).kind != "v":
            d = r(0)
            a = self.read(r(1))
            b = self.shifted(ops[2], d.is64)
            op = "+" if m.startswith("add") else "-"
            out = []
            if m.endswith("s"):
                kind = "Add" if op == "+" else "Sub"
                out.append(f"Flags{kind}{64 if d.is64 else 32}(c, {a}, {b});")
            out.append(self.write(d, f"{self.utype(d)}({a}) {op} {b}"))
            return out
        if m in ("cmp", "cmn"):
            a_r = r(0)
            kind = "Sub" if m == "cmp" else "Add"
            return [f"Flags{kind}{64 if a_r.is64 else 32}(c, {self.read(a_r)}, {self.shifted(ops[1], a_r.is64)});"]
        if (m == "neg" or m == "negs") and not ops[0].vas:
            d = r(0)
            b = self.shifted(ops[1], d.is64)
            out = []
            if m == "negs":
                out.append(f"FlagsSub{64 if d.is64 else 32}(c, 0, {b});")
            out.append(self.write(d, f"{self.utype(d)}(0) - {b}"))
            return out
        if m in ("and", "orr", "eor", "bic", "orn", "ands", "bics", "eon") and Reg(self.md.reg_name(ops[0].reg)).kind != "v":
            d = r(0)
            a = self.read(r(1))
            b = self.shifted(ops[2], d.is64)
            base = m.rstrip("s") if m in ("ands", "bics") else m
            ut = self.utype(d)
            expr = {
                "and": f"{ut}({a}) & {b}", "orr": f"{ut}({a}) | {b}", "eor": f"{ut}({a}) ^ {b}",
                "bic": f"{ut}({a}) & ~{b}", "orn": f"{ut}({a}) | ~{b}", "eon": f"{ut}({a}) ^ ~{b}",
            }[base]
            out = []
            if m in ("ands", "bics"):
                out.append(f"{{ {ut} res = {expr}; FlagsLogic{64 if d.is64 else 32}(c, res); {self.write(d, 'res')} }}")
            else:
                out.append(self.write(d, expr))
            return out
        if m == "tst":
            a_r = r(0)
            return [f"FlagsLogic{64 if a_r.is64 else 32}(c, {self.utype(a_r)}({self.read(a_r)}) & {self.shifted(ops[1], a_r.is64)});"]
        if m == "mvn":
            d = r(0)
            return [self.write(d, f"~{self.shifted(ops[1], d.is64)}")]
        if m in ("lsl", "lsr", "asr", "ror"):
            d = r(0)
            ut, st, bits = self.utype(d), self.stype(d), d.bits
            a = f"{ut}({self.read(r(1))})"
            if ops[2].type == IMM:
                s = str(ops[2].imm)
            else:
                s = f"(unsigned({self.read(r(2))}) & {bits - 1})"
            expr = {
                "lsl": f"{a} << {s}", "lsr": f"{a} >> {s}",
                "asr": f"{ut}({st}({a}) >> {s})", "ror": f"Ror{bits}({a}, {s})",
            }[m]
            return [self.write(d, expr)]
        if m in ("mul", "madd", "msub", "mneg"):
            d = r(0)
            ut = self.utype(d)
            prod = f"{ut}({self.read(r(1))}) * {ut}({self.read(r(2))})"
            if m == "mul":
                return [self.write(d, prod)]
            if m == "mneg":
                return [self.write(d, f"{ut}(0) - {prod}")]
            acc = f"{ut}({self.read(r(3))})"
            return [self.write(d, f"{acc} + {prod}" if m == "madd" else f"{acc} - {prod}")]
        if m in ("smaddl", "umaddl", "smull", "umull", "smsubl", "umsubl", "smnegl"):
            d = r(0)
            signed = m[0] == "s"
            cast = (lambda e: f"uint64_t(int64_t(int32_t({e})))") if signed else (lambda e: f"uint64_t(uint32_t({e}))")
            prod = f"{cast(self.read(r(1)))} * {cast(self.read(r(2)))}"
            if m in ("smull", "umull"):
                return [self.write(d, prod)]
            if m == "smnegl":
                return [self.write(d, f"uint64_t(0) - {prod}")]
            acc = self.read(r(3))
            return [self.write(d, f"{acc} + {prod}" if "add" in m else f"{acc} - {prod}")]
        if m in ("smulh", "umulh"):
            d = r(0)
            if m == "smulh":
                return [self.write(d, f"Smulh(int64_t({self.read(r(1))}), int64_t({self.read(r(2))}))")]
            return [self.write(d, f"Umulh({self.read(r(1))}, {self.read(r(2))})")]
        if m in ("crc32b", "crc32h", "crc32w", "crc32x", "crc32cb", "crc32ch", "crc32cw", "crc32cx"):
            count = {"b": 1, "h": 2, "w": 4, "x": 8}[m[-1]]
            polynomial = "0x82f63b78u" if m.startswith("crc32c") else "0xedb88320u"
            body = f"uint32_t crc = uint32_t({self.read(r(1))}); uint64_t value = uint64_t({self.read(r(2))}); "
            body += f"for (unsigned byte = 0; byte < {count}; ++byte) {{ crc ^= uint8_t(value); value >>= 8; for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1u) ? {polynomial} : 0u); }} "
            return ["{ " + body + self.write(r(0), "crc") + " }"]
        if m in ("sdiv", "udiv"):
            d = r(0)
            bits = d.bits
            if m == "sdiv":
                st = self.stype(d)
                return [self.write(d, f"Sdiv{bits}({st}({self.read(r(1))}), {st}({self.read(r(2))}))")]
            return [self.write(d, f"Udiv{bits}({self.read(r(1))}, {self.read(r(2))})")]
        if m in ("sxtw", "sxth", "sxtb", "uxtb", "uxth", "uxtw"):
            d = r(0)
            t = {"sxtw": "int32_t", "sxth": "int16_t", "sxtb": "int8_t", "uxtb": "uint8_t", "uxth": "uint16_t", "uxtw": "uint32_t"}[m]
            if m[0] == "s":
                return [self.write(d, f"{self.utype(d)}({self.stype(d)}({t}({self.read(r(1))})))")]
            return [self.write(d, f"{self.utype(d)}({t}({self.read(r(1))}))")]
        if m in ("ubfx", "sbfx", "ubfiz", "sbfiz", "bfi", "bfxil"):
            return self.bitfield(m, r(0), r(1), ops[2].imm, ops[3].imm)
        if m in ("rbit", "rev", "rev16", "rev32", "rev64", "clz") and Reg(self.md.reg_name(ops[0].reg)).kind == "x":
            d = r(0)
            a = self.read(r(1))
            bits = d.bits
            if m == "rbit":
                return [self.write(d, f"Rbit{bits}({a})")]
            if m == "clz":
                return [self.write(d, f"({a}) ? __builtin_clzll(uint64_t({a})) - {64 - bits} : {bits}")]
            if m == "rev" or (m == "rev64" and bits == 64) or (m == "rev32" and bits == 32):
                return [self.write(d, f"__builtin_bswap{bits}({a})")]
            if m == "rev16":
                mask = 0x00FF00FF00FF00FF & ((1 << bits) - 1)
                ut = self.utype(d)
                return [self.write(d, f"((({ut}({a}) & {mask:#x}ull) << 8) | (({ut}({a}) >> 8) & {mask:#x}ull))")]
            if m == "rev32" and bits == 64:
                return [self.write(d, f"(uint64_t(__builtin_bswap32(uint32_t({a}))) | (uint64_t(__builtin_bswap32(uint32_t(uint64_t({a}) >> 32))) << 32))")]
            raise Unsupported(m)
        if m == "extr":
            d = r(0)
            ut, bits, lsb = self.utype(d), d.bits, ops[3].imm
            hi, lo = f"{ut}({self.read(r(1))})", f"{ut}({self.read(r(2))})"
            if lsb == 0:
                return [self.write(d, lo)]
            return [self.write(d, f"({lo} >> {lsb}) | ({hi} << {bits - lsb})")]

        # -- conditional select / compare
        if m in ("csel", "csinc", "csinv", "csneg"):
            d = r(0)
            ut = self.utype(d)
            a, b = f"{ut}({self.read(r(1))})", f"{ut}({self.read(r(2))})"
            alt = {"csel": b, "csinc": f"{b} + 1", "csinv": f"~{b}", "csneg": f"{ut}(0) - {b}"}[m]
            return [self.write(d, f"{COND[ins.cc]} ? {a} : {ut}({alt})")]
        if m in ("cset", "csetm"):
            d = r(0)
            v = "1" if m == "cset" else f"{self.utype(d)}(~{self.utype(d)}(0))"
            return [self.write(d, f"{COND[ins.cc]} ? {v} : 0")]
        if m in ("cinc", "cinv", "cneg"):
            d = r(0)
            ut = self.utype(d)
            a = f"{ut}({self.read(r(1))})"
            alt = {"cinc": f"{a} + 1", "cinv": f"~{a}", "cneg": f"{ut}(0) - {a}"}[m]
            return [self.write(d, f"{COND[ins.cc]} ? {ut}({alt}) : {a}")]
        if m in ("ccmp", "ccmn"):
            a_r = r(0)
            kind = "Sub" if m == "ccmp" else "Add"
            b = self.shifted(ops[1], a_r.is64)
            nzcv = ops[2].imm
            return [f"if ({COND[ins.cc]}) Flags{kind}{64 if a_r.is64 else 32}(c, {self.read(a_r)}, {b}); else SetNzcv(c, {nzcv});"]

        if m in ("adc", "adcs", "sbc", "sbcs"):
            d = r(0)
            ut, bits = self.utype(d), d.bits
            a = f"{ut}({self.read(r(1))})"
            b = f"{ut}({self.read(r(2))})" if m.startswith("adc") else f"{ut}(~{ut}({self.read(r(2))}))"
            out = [f"{ut} cin = c.c;"]
            if m.endswith("s"):
                out.append(f"FlagsAdc{bits}(c, {a}, {b}, cin);")
            out.append(self.write(d, f"{ut}({a} + {b} + cin)"))
            return ["{ " + " ".join(out) + " }"]

        # -- barriers and atomics (host atomics are at least as strong)
        if m in ("dmb", "dsb"):
            return ["__atomic_thread_fence(__ATOMIC_SEQ_CST);"]
        if m in ("isb", "clrex", "nop", "yield", "hint"):
            return []
        atomic = self.atomic(ins, m, ops)
        if atomic is not None:
            return atomic

        # -- loads and stores
        if m in LOADS or m in STORES:
            return self.load_store(ins, m, ops)

        # -- floating point
        return self.fp(ins, m, ops)

    # ---- AdvSIMD (see recomp/runtime/recomp_simd.h) -------------------------

    def lanes(self, vas, signed=False):
        bits, count = vas_lane_bits(vas), vas_count(vas)
        return (f"{'int' if signed else 'uint'}{bits}_t", count, bits)

    def simd(self, ins, m, ops):
        if not ops or ops[0].type != REG:
            return None
        d = self.reg(ops[0])
        if m in ("ld1", "ld2", "ld3", "ld4", "st1", "st2", "st3", "st4", "ld1r"):
            return self.simd_mem(ins, m, ops)
        if m in ("umov", "smov"):
            return self.vmov(ins, ops, signed=(m == "smov"))
        if d.kind != "v":
            return None
        V = lambda o: f"c.v[{self.reg(o).num}]"

        # scalar results from vectors
        if ops[0].vas == 0 and ops[0].vector_index == -1 and len(ops) == 2 and ops[1].type == REG and ops[1].vas:
            t, n, bits = self.lanes(ops[1].vas)
            if m == "addv":
                return [f"Addv<{t}, {n}>({V(ops[0])}, {V(ops[1])});"]
            if m == "uaddlv":
                total = " + ".join(f"uint64_t({V(ops[1])}.u{bits}[{i}])" for i in range(n))
                return [f"{{ uint64_t t = {total}; c.v[{self.reg(ops[0]).num}] = V128{{}}; c.v[{self.reg(ops[0]).num}].u{bits * 2}[0] = uint{bits * 2}_t(t); }}"]
            if m in ("umaxv", "uminv", "smaxv", "sminv"):
                t, n, bits = self.lanes(ops[1].vas, signed=m[0] == "s")
                return [f"MinMaxV<{t}, {n}, {'true' if 'max' in m else 'false'}>({V(ops[0])}, {V(ops[1])});"]
            if m == "addp":
                return [f"{{ uint64_t t = {V(ops[1])}.u64[0] + {V(ops[1])}.u64[1]; SetD64({V(ops[0])}, t); }}"]
            if m == "faddp":
                if bits == 32:
                    return [f"SetS({V(ops[0])}, {V(ops[1])}.f32[0] + {V(ops[1])}.f32[1]);"]
                return [f"SetD({V(ops[0])}, {V(ops[1])}.f64[0] + {V(ops[1])}.f64[1]);"]
            return None

        if not ops[0].vas or ops[0].vector_index != -1:
            return None
        t, n, bits = self.lanes(ops[0].vas)
        st = f"int{bits}_t"
        dst = V(ops[0])

        def map2(expr):
            return [f"Map2<{t}, {n}>({dst}, {V(ops[1])}, {V(ops[2])}, []({t} x, {t} y) {{ return {expr}; }});"]

        def map1(expr):
            return [f"Map1<{t}, {n}>({dst}, {V(ops[1])}, []({t} x) {{ return {expr}; }});"]

        zero_form = len(ops) == 3 and ops[2].type in (IMM, FP)
        int_cmp = {"cmeq": "x == y", "cmtst": "(x & y) != 0", "cmgt": f"{st}(x) > {st}(y)", "cmge": f"{st}(x) >= {st}(y)",
                   "cmhi": "x > y", "cmhs": "x >= y"}
        int_cmp0 = {"cmeq": "x == 0", "cmgt": f"{st}(x) > 0", "cmge": f"{st}(x) >= 0", "cmlt": f"{st}(x) < 0", "cmle": f"{st}(x) <= 0"}
        if m in int_cmp0 and zero_form:
            return map1(f"({int_cmp0[m]}) ? AllOnes<{t}>() : {t}(0)")
        if m in int_cmp and not zero_form:
            return map2(f"({int_cmp[m]}) ? AllOnes<{t}>() : {t}(0)")
        f_cmp = {"fcmeq": "==", "fcmgt": ">", "fcmge": ">=", "fcmlt": "<", "fcmle": "<="}
        if m in f_cmp:
            if zero_form:
                return map1(f"(AsFloat(x) {f_cmp[m]} 0) ? AllOnes<{t}>() : {t}(0)")
            return map2(f"(AsFloat(x) {f_cmp[m]} AsFloat(y)) ? AllOnes<{t}>() : {t}(0)")
        if m in ("shl", "ushr", "sshr") and ops[2].type == IMM:
            fn = {"shl": "Lsl", "ushr": "Lsr", "sshr": "Asr"}[m]
            return map1(f"{fn}(x, {ops[2].imm})")
        if m in ("ushl", "sshl"):
            return map2(f"{'Ushl' if m == 'ushl' else 'Sshl'}(x, y)")
        if m in ("bsl", "bit", "bif", "bcax", "eor3"):
            words = 2 if bits * n == 128 else 1
            expr = {"bsl": "(a & b) | (~a & c)", "bit": "(b & c) | (a & ~c)", "bif": "(a & c) | (b & ~c)"}
            if m in expr:
                args = f"{dst}, {V(ops[1])}, {V(ops[2])}"
                return [f"Map3<uint64_t, {words}>({dst}, {args}, [](uint64_t a, uint64_t b, uint64_t c) {{ return {expr[m]}; }});"]
            e = "a ^ (b & ~c)" if m == "bcax" else "a ^ b ^ c"
            return [f"Map3<uint64_t, {words}>({dst}, {V(ops[1])}, {V(ops[2])}, {V(ops[3])}, [](uint64_t a, uint64_t b, uint64_t c) {{ return {e}; }});"]
        if m in ("mvn", "not") and len(ops) == 2:
            return map1("~x")
        if m in ("mvni", "movi") and ops[1].type == IMM:
            imm = ops[1].imm << (ops[1].shift.value if ops[1].shift.type else 0)
            if ops[1].shift.type == SFT_MSL:  # "shifting ones"
                imm |= (1 << ops[1].shift.value) - 1
            value = (~imm if m == "mvni" else imm) & ((1 << bits) - 1)
            return [f"Fill<{t}, {n}>({dst}, {t}({value:#x}ull));"]
        if m == "mul":
            if ops[2].vector_index != -1:
                return [f"Map2Elem<{t}, {n}>({dst}, {V(ops[1])}, {V(ops[2])}, {ops[2].vector_index}, []({t} x, {t} y) {{ return {t}(x * y); }});"]
            return map2(f"{t}(x * y)")
        if m == "fabd":
            return map2(f"FromFloat<{t}>(std::fabs(AsFloat(x) - AsFloat(y)))")
        if m == "addp" and len(ops) == 3:
            return [f"Addp<{t}, {n}>({dst}, {V(ops[1])}, {V(ops[2])});"]
        if m == "faddp" and len(ops) == 3:
            return [f"Faddp<{t}, {n}>({dst}, {V(ops[1])}, {V(ops[2])});"]
        perm = {"uzp1": ("Uzp", 0), "uzp2": ("Uzp", 1), "zip1": ("Zip", 0), "zip2": ("Zip", 1), "trn1": ("Trn", 0), "trn2": ("Trn", 1)}
        if m in perm:
            fn, part = perm[m]
            return [f"{fn}<{t}, {n}, {part}>({dst}, {V(ops[1])}, {V(ops[2])});"]
        if m == "tbl":
            regs = [self.reg(o).num for o in ops[1:-1]]
            if regs != list(range(regs[0], regs[0] + len(regs))):
                raise Unsupported("tbl register list wraps")
            return [f"Tbl<{n}>({dst}, &c.v[{regs[0]}], {len(regs)}, {V(ops[-1])});"]
        if m in ("fcvtl", "fcvtl2"):
            return [f"Fcvtl<2, {1 if m.endswith('2') else 0}>({dst}, {V(ops[1])});"]
        if m in ("sshll", "sshll2", "ushll", "ushll2", "sxtl", "sxtl2", "uxtl", "uxtl2"):
            signed = m[0] == "s"
            to_t = f"{'int' if signed else 'uint'}{bits}_t"
            from_t = f"{'int' if signed else 'uint'}{bits // 2}_t"
            shift = ops[2].imm if len(ops) > 2 else 0
            return [f"Widen<{to_t}, {from_t}, {n}, {1 if m.endswith('2') else 0}>({dst}, {V(ops[1])}, {shift});"]
        if m in ("saddw", "saddw2", "uaddw", "uaddw2"):
            signed = m[0] == "s"
            to_t = f"{'int' if signed else 'uint'}{bits}_t"
            from_t = f"{'int' if signed else 'uint'}{bits // 2}_t"
            return [f"AddWide<{to_t}, {from_t}, {n}, {1 if m.endswith('2') else 0}>({dst}, {V(ops[1])}, {V(ops[2])});"]
        if m == "udot":
            return [f"Udot<{n}>({dst}, {V(ops[1])}, {V(ops[2])}, {ops[2].vector_index});"]
        return None

    def simd_mem(self, ins, m, ops):
        mem_i = next(i for i, o in enumerate(ops) if o.type == MEM)
        regs = ops[:mem_i]
        base = Reg(self.md.reg_name(ops[mem_i].mem.base))
        lines = [f"uint64_t ea = {self.read(base)};"]
        first = self.reg(regs[0]).num
        if ops[mem_i].mem.disp:
            raise Unsupported("structure access with offset")
        if regs[0].vector_index != -1:
            # single lane: ld1/st1 {v.s}[i]
            t, _, bits = self.lanes(regs[0].vas)
            lane = regs[0].vector_index
            if m.startswith("ld"):
                lines.append(f"c.v[{first}].u{bits}[{lane}] = Load<{t}>(ea);")
            else:
                lines.append(f"Store<{t}>(ea, c.v[{first}].u{bits}[{lane}]);")
            total = bits // 8
        elif m == "ld1r":
            t, n, bits = self.lanes(regs[0].vas)
            lines.append(f"Fill<{t}, {n}>(c.v[{first}], Load<{t}>(ea));")
            total = bits // 8
        else:
            t, n, bits = self.lanes(regs[0].vas)
            k = len(regs)
            structure = int(m[2])
            width = n * bits // 8
            if structure == 1:
                for i in range(k):
                    reg = (first + i) % 32
                    if width == 16:
                        lines.append(f"c.v[{reg}] = Load<V128>(ea + {16 * i});" if m.startswith("ld") else f"Store<V128>(ea + {16 * i}, c.v[{reg}]);")
                    else:
                        lines.append(f"SetD64(c.v[{reg}], Load<uint64_t>(ea + {8 * i}));" if m.startswith("ld") else f"Store<uint64_t>(ea + {8 * i}, c.v[{reg}].u64[0]);")
            else:
                fn = "LdN" if m.startswith("ld") else "StN"
                lines.append(f"{fn}<{t}, {n}>(c, {first}, {k}, ea);")
            total = width * k
        if ins.writeback:
            import re as _re
            mm = _re.search(r",\s*#(0x[0-9a-f]+|\d+)$", ins.op_str)
            rm = _re.search(r",\s*(x\d+)$", ins.op_str)
            if mm:
                lines.append(self.write(base, f"ea + {int(mm.group(1), 0)}"))
            elif rm:
                lines.append(self.write(base, f"ea + {self.read(Reg(rm.group(1)))}"))
            else:
                lines.append(self.write(base, f"ea + {total}"))
        return ["{ " + " ".join(lines) + " }"]

    def mov(self, ins, ops):
        d = self.reg(ops[0])
        if d.kind == "v" or (ops[1].type == REG and self.reg(ops[1]).kind == "v"):
            return self.vmov(ins, ops)
        if ops[1].type == IMM:
            return [self.write(d, f"{ops[1].imm & ((1 << d.bits) - 1):#x}ull")]
        return [self.write(d, self.read(self.reg(ops[1])))]

    def vmov(self, ins, ops, signed=False):
        d, s = self.reg(ops[0]), self.reg(ops[1])
        if d.kind == "v" and s.kind == "v" and ops[0].vector_index == -1 and ops[1].vector_index == -1:
            if ops[0].vas == 0 and ops[1].vas == 0 and d.bits != s.bits:
                raise Unsupported("scalar mov width")
            if ops[0].vas in (A.ARM64_VAS_8B, A.ARM64_VAS_4H, A.ARM64_VAS_2S, A.ARM64_VAS_1D):
                return [f"{{ uint64_t t = c.v[{s.num}].u64[0]; c.v[{d.num}] = V128{{}}; c.v[{d.num}].u64[0] = t; }}"]
            return [f"c.v[{d.num}] = c.v[{s.num}];"]
        if d.kind == "x" and s.kind == "v" and ops[1].vector_index != -1:
            lane = ops[1].vector_index
            size = vas_lane_bits(ops[1].vas)
            if signed:
                return [self.write(d, f"{self.utype(d)}({self.stype(d)}(int{size}_t(c.v[{s.num}].u{size}[{lane}])))")]
            return [self.write(d, f"c.v[{s.num}].u{size}[{lane}]")]
        if d.kind == "v" and ops[0].vector_index == -1 and ops[0].vas == 0 and s.kind == "v" and ops[1].vector_index != -1:
            # scalar from lane (dup s1, v0.s[1])
            size = d.bits
            return [f"{{ auto t = c.v[{s.num}].u{size}[{ops[1].vector_index}]; c.v[{d.num}] = V128{{}}; c.v[{d.num}].u{size}[0] = t; }}"]
        if d.kind == "v" and ops[0].vector_index != -1:
            lane = ops[0].vector_index
            size = vas_lane_bits(ops[0].vas)
            src = self.read(s) if s.kind != "v" else f"c.v[{s.num}].u{size}[{ops[1].vector_index}]"
            return [f"c.v[{d.num}].u{size}[{lane}] = uint{size}_t({src});"]
        raise Unsupported("vector mov form")

    def bitfield(self, m, d, s, lsb, width):
        ut, st, bits = self.utype(d), self.stype(d), d.bits
        a = f"{ut}({self.read(s)})"
        mask = (1 << width) - 1
        if m == "ubfx":
            return [self.write(d, f"({a} >> {lsb}) & {mask:#x}ull")]
        if m == "sbfx":
            return [self.write(d, f"{ut}({st}({a} << {bits - lsb - width}) >> {bits - width})")]
        if m == "ubfiz":
            return [self.write(d, f"({a} & {mask:#x}ull) << {lsb}")]
        if m == "sbfiz":
            return [self.write(d, f"{ut}({st}({a} << {bits - width}) >> {bits - width}) << {lsb}")]
        if m == "bfi":
            keep = ((1 << bits) - 1) ^ (mask << lsb)
            return [self.write(d, f"({ut}({self.read(d)}) & {keep:#x}ull) | (({a} & {mask:#x}ull) << {lsb})")]
        if m == "bfxil":
            keep = ((1 << bits) - 1) ^ mask
            return [self.write(d, f"({ut}({self.read(d)}) & {keep:#x}ull) | (({a} >> {lsb}) & {mask:#x}ull)")]
        raise Unsupported(m)

    def atomic(self, ins, m, ops):
        """Acquire/release loads and stores and the LSE atomics; None if m is none of them."""
        import re as _re
        mm = _re.fullmatch(r"(ldapr|ldar|stlr|ldlar|stllr)([bh]?)", m)
        if mm:
            kind, sz = mm.groups()
            r0 = self.reg(ops[0])
            t = {"b": "uint8_t", "h": "uint16_t"}.get(sz, "uint64_t" if r0.is64 else "uint32_t")
            p = f"reinterpret_cast<{t}*>({self.read(Reg(self.md.reg_name(ops[1].mem.base)))})"
            if kind.startswith("st"):
                return [f"__atomic_store_n({p}, {t}({self.read(r0)}), __ATOMIC_SEQ_CST);"]
            return [self.write(r0, f"__atomic_load_n({p}, __ATOMIC_SEQ_CST)")]
        mm = _re.fullmatch(r"(cas|swp|ldadd|ldclr|ldeor|ldset|stadd|stclr|steor|stset)(a|l|al)?([bh]?)", m)
        if not mm:
            return None
        kind, _, sz = mm.groups()
        rs = self.reg(ops[0])
        t = {"b": "uint8_t", "h": "uint16_t"}.get(sz, "uint64_t" if rs.is64 else "uint32_t")
        mem = ops[-1]
        p = f"reinterpret_cast<{t}*>({self.read(Reg(self.md.reg_name(mem.mem.base)))})"
        if kind == "cas":
            rt = self.reg(ops[1])
            return ["{ " + f"{t} e = {t}({self.read(rs)}); __atomic_compare_exchange_n({p}, &e, {t}({self.read(rt)}), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); " + self.write(rs, "e") + " }"]
        if kind == "swp":
            rt = self.reg(ops[1])
            return ["{ " + f"{t} old = __atomic_exchange_n({p}, {t}({self.read(rs)}), __ATOMIC_SEQ_CST); " + self.write(rt, "old") + " }"]
        op = kind[2:] if kind.startswith("ld") else kind[2:]
        fn = {"add": "__atomic_fetch_add", "clr": "__atomic_fetch_and", "eor": "__atomic_fetch_xor", "set": "__atomic_fetch_or"}[op]
        val = f"{t}(~{t}({self.read(rs)}))" if op == "clr" else f"{t}({self.read(rs)})"
        call = f"{fn}({p}, {val}, __ATOMIC_SEQ_CST)"
        if kind.startswith("st") or len(ops) < 3:
            return [f"{call};"]
        rt = self.reg(ops[1])
        return ["{ " + f"{t} old = {call}; " + self.write(rt, "old") + " }"]

    def load_store(self, ins, m, ops):
        pair = m in ("ldp", "stp", "ldpsw")
        mem_index = 2 if pair else 1
        if ops[mem_index].type == IMM and m.startswith("ldr"):
            # literal load
            r0 = self.reg(ops[0])
            addr = f"g_imageBase + {ops[mem_index].imm:#x}"
            return [self.reg_load(m, r0, addr)]
        setup, ea, after = self.address(ins, mem_index)
        body = []
        if pair:
            r0, r1 = self.reg(ops[0]), self.reg(ops[1])
            size = 4 if m == "ldpsw" else r0.bits // 8
            if m == "stp":
                body.append(self.reg_store("str", r0, ea))
                body.append(self.reg_store("str", r1, f"{ea} + {size}"))
            else:
                lm = "ldrsw" if m == "ldpsw" else "ldr"
                # load into temps first: the base may be one of the destinations
                body.append(self.reg_load(lm, r0, ea, temp="t0"))
                body.append(self.reg_load(lm, r1, f"{ea} + {size}", temp="t1"))
                body.append(self.temp_commit(r0, "t0"))
                body.append(self.temp_commit(r1, "t1"))
        else:
            r0 = self.reg(ops[0])
            if m in STORES:
                body.append(self.reg_store(m, r0, ea))
            else:
                body.append(self.reg_load(m, r0, ea))
        return ["{ " + " ".join(setup + body + after) + " }"]

    def reg_load(self, m, r0, addr, temp=None):
        base = m.replace("ldur", "ldr")
        if r0.kind == "v":
            t = {8: "uint8_t", 16: "uint16_t", 32: "uint32_t", 64: "uint64_t", 128: "V128"}[r0.bits]
            if temp:
                return f"auto {temp} = Load<{t}>({addr});"
            if r0.bits == 128:
                return f"c.v[{r0.num}] = Load<V128>({addr});"
            return f"c.v[{r0.num}] = V128{{}}; c.v[{r0.num}].u{r0.bits}[0] = Load<{t}>({addr});"
        sizes = {"ldrb": "uint8_t", "ldrh": "uint16_t", "ldrsb": "int8_t", "ldrsh": "int16_t", "ldrsw": "int32_t"}
        if base == "ldr":
            t = "uint64_t" if r0.is64 else "uint32_t"
        else:
            t = sizes[base]
        expr = f"Load<{t}>({addr})"
        if t.startswith("int"):
            expr = f"{self.utype(r0)}({self.stype(r0)}({expr}))"
        if temp:
            return f"uint64_t {temp} = uint64_t({expr});"
        return self.write(r0, expr)

    def temp_commit(self, r0, temp):
        if r0.kind == "v":
            if r0.bits == 128:
                return f"c.v[{r0.num}] = {temp};"
            return f"c.v[{r0.num}] = V128{{}}; c.v[{r0.num}].u{r0.bits}[0] = {temp};"
        return self.write(r0, temp if r0.is64 else f"uint32_t({temp})")

    def reg_store(self, m, r0, addr):
        base = m.replace("stur", "str")
        if r0.kind == "v":
            t = {8: "uint8_t", 16: "uint16_t", 32: "uint32_t", 64: "uint64_t", 128: "V128"}[r0.bits]
            val = f"c.v[{r0.num}]" if r0.bits == 128 else f"c.v[{r0.num}].u{r0.bits}[0]"
            return f"Store<{t}>({addr}, {val});"
        t = {"strb": "uint8_t", "strh": "uint16_t"}.get(base, "uint64_t" if r0.is64 else "uint32_t")
        return f"Store<{t}>({addr}, {t}({self.read(r0)}));"

    def fp(self, ins, m, ops):
        def R(i):
            return self.reg(ops[i])

        scalar_ops = {"fadd": "+", "fsub": "-", "fmul": "*", "fdiv": "/"}
        if m in scalar_ops and not ops[0].vas:
            d = R(0)
            if ops[2].vector_index != -1:
                ft = "f32" if d.bits == 32 else "f64"
                rhs = f"c.v[{R(2).num}].{ft}[{ops[2].vector_index}]"
            else:
                rhs = self.fread(R(2))
            return [self.fwrite(d, f"{self.fread(R(1))} {scalar_ops[m]} {rhs}")]
        if m in ("fmla", "fmls") and not ops[0].vas and ops[2].vector_index != -1:
            d = R(0)
            ft = "f32" if d.bits == 32 else "f64"
            sign = "-" if m == "fmls" else ""
            lanev = f"c.v[{R(2).num}].{ft}[{ops[2].vector_index}]"
            return [self.fwrite(d, f"std::fma({sign}{self.fread(R(1))}, {lanev}, {self.fread(d)})")]
        if m == "fnmul" and not ops[0].vas:
            d = R(0)
            return [self.fwrite(d, f"-({self.fread(R(1))} * {self.fread(R(2))})")]
        if m in ("fmadd", "fmsub", "fnmadd", "fnmsub"):
            d = R(0)
            a, b, acc = self.fread(R(1)), self.fread(R(2)), self.fread(R(3))
            fma = "std::fma"
            expr = {"fmadd": f"{fma}({a}, {b}, {acc})", "fmsub": f"{fma}(-{a}, {b}, {acc})",
                    "fnmadd": f"{fma}(-{a}, {b}, -{acc})", "fnmsub": f"{fma}({a}, {b}, -{acc})"}[m]
            return [self.fwrite(d, expr)]
        if m in ("fneg", "fabs", "fsqrt", "frintm", "frintp", "frintz", "frinta", "frintn", "frintx", "frinti") and not ops[0].vas:
            d = R(0)
            a = self.fread(R(1))
            expr = {"fneg": f"-{a}", "fabs": f"std::fabs({a})", "fsqrt": f"std::sqrt({a})",
                    "frintm": f"std::floor({a})", "frintp": f"std::ceil({a})", "frintz": f"std::trunc({a})",
                    "frinta": f"std::round({a})", "frintn": f"std::nearbyint({a})",
                    "frintx": f"std::nearbyint({a})", "frinti": f"std::nearbyint({a})"}[m]
            return [self.fwrite(d, expr)]
        if m in ("fmax", "fmin", "fmaxnm", "fminnm") and not ops[0].vas:
            d = R(0)
            a, b = self.fread(R(1)), self.fread(R(2))
            if m in ("fmax", "fmin"):
                return [self.fwrite(d, f"F{m[1:]}({a}, {b})")]
            return [self.fwrite(d, f"F{m[1:4]}nm({a}, {b})")]
        if m == "fabd" and not ops[0].vas:
            d = R(0)
            return [self.fwrite(d, f"std::fabs({self.fread(R(1))} - {self.fread(R(2))})")]
        if m in ("fcmp", "fcmpe"):
            a = R(0)
            b = self.fread(R(1)) if ops[1].type == REG else ("0.0f" if a.bits == 32 else "0.0")
            return [f"FlagsFcmp(c, {self.fread(a)}, {b});"]
        if m in ("fccmp", "fccmpe"):
            a, b = R(0), R(1)
            return [f"if ({COND[ins.cc]}) FlagsFcmp(c, {self.fread(a)}, {self.fread(b)}); else SetNzcv(c, {ops[2].imm});"]
        if m == "fcsel":
            d = R(0)
            return [self.fwrite(d, f"{COND[ins.cc]} ? {self.fread(R(1))} : {self.fread(R(2))}")]
        if m == "fmov":
            return self.fmov(ops)
        if m in ("scvtf", "ucvtf") and not ops[0].vas:
            d, s = R(0), R(1)
            ft = "float" if d.bits == 32 else "double"
            if s.kind == "v":
                src = f"c.v[{s.num}].{'s' if m == 'scvtf' else 'u'}{s.bits}[0]"
            else:
                src = f"{self.stype(s) if m == 'scvtf' else self.utype(s)}({self.read(s)})"
            if len(ops) > 2:
                return [self.fwrite(d, f"{ft}({src}) / {ft}({1 << ops[2].imm}ull)")]
            return [self.fwrite(d, f"{ft}({src})")]
        if m in ("fcvtzs", "fcvtzu", "fcvtms", "fcvtmu", "fcvtps", "fcvtpu", "fcvtas", "fcvtau", "fcvtns", "fcvtnu") and not ops[0].vas:
            d, s = R(0), R(1)
            rnd = {"z": "", "m": "std::floor", "p": "std::ceil", "a": "std::round", "n": "std::nearbyint"}[m[4]]
            fsrc = self.fread(s)
            if len(ops) > 2 and ops[2].type == IMM:  # fixed point: scale by 2^fbits first
                fsrc = f"({fsrc} * {'float' if s.bits == 32 else 'double'}({float(1 << ops[2].imm)!r}))"
            src = f"{rnd}({fsrc})"
            signed = m[-1] == "s"
            if d.kind == "v":
                it = ("int" if signed else "uint") + str(d.bits) + "_t"
                return [f"c.v[{d.num}] = V128{{}}; c.v[{d.num}].u{d.bits}[0] = uint{d.bits}_t(FcvtSat<{it}>({src}));"]
            it = self.stype(d) if signed else self.utype(d)
            return [self.write(d, f"FcvtSat<{it}>({src})")]
        if m == "fcvt":
            d, s = R(0), R(1)
            if d.bits in (32, 64) and s.bits in (32, 64):
                return [self.fwrite(d, self.fread(s))]
            raise Unsupported("fcvt half")
        if m == "movi":
            d = R(0)
            imm = ops[1].imm
            if ops[1].shift.type and ops[1].shift.value:
                imm <<= ops[1].shift.value
                if ops[1].shift.type == SFT_MSL:
                    imm |= (1 << ops[1].shift.value) - 1
            if d.bits == 64 and not ops[0].vas:
                return [f"SetD64(c.v[{d.num}], {imm & 0xFFFFFFFFFFFFFFFF:#x}ull);"]
            lane = vas_lane_bits(ops[0].vas)
            count = vas_count(ops[0].vas)
            fill = " ".join(f"c.v[{d.num}].u{lane}[{i}] = uint{lane}_t({imm:#x}ull);" for i in range(count))
            return [f"c.v[{d.num}] = V128{{}}; {fill}"]
        if m == "dup":
            d, s = R(0), R(1)
            lane = vas_lane_bits(ops[0].vas)
            count = vas_count(ops[0].vas)
            src = self.read(s) if s.kind == "x" else f"c.v[{s.num}].u{lane}[{ops[1].vector_index}]"
            fill = " ".join(f"tmp.u{lane}[{i}] = uint{lane}_t(val);" for i in range(count))
            return [f"{{ uint64_t val = uint64_t({src}); V128 tmp{{}}; {fill} c.v[{d.num}] = tmp; }}"]
        if ops and ops[0].vas:
            return self.vector(ins, m, ops)
        raise Unsupported(m)

    def fmov(self, ops):
        d = self.reg(ops[0])
        if ops[1].type == FP:
            if d.kind == "v" and ops[0].vas:
                lane = vas_lane_bits(ops[0].vas)
                count = vas_count(ops[0].vas)
                ft = "f32" if lane == 32 else "f64"
                cast = "float" if lane == 32 else "double"
                fill = " ".join(f"c.v[{d.num}].{ft}[{i}] = {cast}({ops[1].fp!r});" for i in range(count))
                return [f"c.v[{d.num}] = V128{{}}; {fill}"]
            return [self.fwrite(d, repr(ops[1].fp))]
        s = self.reg(ops[1])
        if d.kind == "v" and s.kind == "v":
            if ops[0].vector_index != -1 or ops[1].vector_index != -1:
                raise Unsupported("fmov lane")
            return [self.fwrite(d, self.fread(s))]
        if d.kind == "v" and s.kind in ("x", "zr"):
            if ops[0].vector_index == 1:
                return [f"c.v[{d.num}].u64[1] = {self.read(s)};"]
            if d.bits == 32:
                return [f"SetS32(c.v[{d.num}], uint32_t({self.read(s)}));"]
            return [f"SetD64(c.v[{d.num}], uint64_t({self.read(s)}));"]
        if d.kind == "x" and s.kind == "v":
            if ops[1].vector_index == 1:
                return [self.write(d, f"c.v[{s.num}].u64[1]")]
            return [self.write(d, f"c.v[{s.num}].u{s.bits}[0]")]
        raise Unsupported("fmov form")

    def vector(self, ins, m, ops):
        """Element-wise vector operations used by the game (small set)."""
        d = self.reg(ops[0])
        lane = vas_lane_bits(ops[0].vas)
        count = vas_count(ops[0].vas)
        fops = {"fadd": "+", "fsub": "-", "fmul": "*", "fdiv": "/"}
        if m in fops and len(ops) == 3:
            a, b = self.reg(ops[1]), self.reg(ops[2])
            ft = "f32" if lane == 32 else "f64"
            if ops[2].vector_index != -1:
                bi = ops[2].vector_index
                body = " ".join(f"t.{ft}[{i}] = c.v[{a.num}].{ft}[{i}] {fops[m]} c.v[{b.num}].{ft}[{bi}];" for i in range(count))
            else:
                body = " ".join(f"t.{ft}[{i}] = c.v[{a.num}].{ft}[{i}] {fops[m]} c.v[{b.num}].{ft}[{i}];" for i in range(count))
            return [f"{{ V128 t{{}}; {body} c.v[{d.num}] = t; }}"]
        if m in ("fmla", "fmls") and len(ops) == 3:
            # fused per lane: d = d + n * m (fmls: d - n * m), by vector or by element
            a, b = self.reg(ops[1]), self.reg(ops[2])
            ft = "f32" if lane == 32 else "f64"
            sign = "-" if m == "fmls" else ""
            bi = ops[2].vector_index
            body = " ".join(f"t.{ft}[{i}] = std::fma({sign}c.v[{a.num}].{ft}[{i}], c.v[{b.num}].{ft}[{bi if bi != -1 else i}], c.v[{d.num}].{ft}[{i}]);" for i in range(count))
            return [f"{{ V128 t{{}}; {body} c.v[{d.num}] = t; }}"]
        if m in ("fminnm", "fmaxnm") and len(ops) == 3:
            a, b = self.reg(ops[1]), self.reg(ops[2])
            ft = "f32" if lane == 32 else "f64"
            fn = "Fminnm" if m == "fminnm" else "Fmaxnm"
            body = " ".join(f"t.{ft}[{i}] = {fn}(c.v[{a.num}].{ft}[{i}], c.v[{b.num}].{ft}[{i}]);" for i in range(count))
            return [f"{{ V128 t{{}}; {body} c.v[{d.num}] = t; }}"]
        if m == "bic" and len(ops) == 2 and ops[1].type == IMM:
            imm = ops[1].imm << (ops[1].shift.value if ops[1].shift.type else 0)
            mask = (~imm) & ((1 << lane) - 1)
            body = " ".join(f"t.u{lane}[{i}] = uint{lane}_t(c.v[{d.num}].u{lane}[{i}] & {mask:#x}u);" for i in range(count))
            return [f"{{ V128 t{{}}; {body} c.v[{d.num}] = t; }}"]
        if m in ("ssra", "usra") and len(ops) == 3 and ops[2].type == IMM:
            a = self.reg(ops[1])
            sg = "s" if m == "ssra" else "u"
            sh = ops[2].imm
            body = " ".join(f"t.{sg}{lane}[{i}] = {'int' if sg == 's' else 'uint'}{lane}_t(c.v[{d.num}].{sg}{lane}[{i}] + {'int' if sg == 's' else 'uint'}{lane}_t(c.v[{a.num}].{sg}{lane}[{i}] >> {min(sh, lane - 1) if sg == 's' else sh}));" for i in range(count))
            if sg == "u" and sh >= lane:
                body = " ".join(f"t.u{lane}[{i}] = c.v[{d.num}].u{lane}[{i}];" for i in range(count))
            return [f"{{ V128 t{{}}; {body} c.v[{d.num}] = t; }}"]
        if m == "xtn2":
            s = self.reg(ops[1])
            half = count // 2
            body = " ".join(f"t.u{lane}[{half + i}] = uint{lane}_t(c.v[{s.num}].u{lane * 2}[{i}]);" for i in range(half))
            return [f"{{ V128 t = c.v[{d.num}]; {body} c.v[{d.num}] = t; }}"]
        if m == "cnt":
            s = self.reg(ops[1])
            body = " ".join(f"t.u8[{i}] = uint8_t(Popcount64(c.v[{s.num}].u8[{i}]));" for i in range(count))
            return [f"{{ V128 t{{}}; {body} c.v[{d.num}] = t; }}"]
        if m == "mla" and len(ops) == 3 and ops[2].type == REG:
            a, b = self.reg(ops[1]), self.reg(ops[2])
            bi = ops[2].vector_index
            body = " ".join(f"t.u{lane}[{i}] = uint{lane}_t(c.v[{d.num}].u{lane}[{i}] + c.v[{a.num}].u{lane}[{i}] * c.v[{b.num}].u{lane}[{bi if bi != -1 else i}]);" for i in range(count))
            return [f"{{ V128 t{{}}; {body} c.v[{d.num}] = t; }}"]
        if m == "neg" and len(ops) == 2:
            s = self.reg(ops[1])
            body = " ".join(f"t.u{lane}[{i}] = uint{lane}_t(0u - c.v[{s.num}].u{lane}[{i}]);" for i in range(count))
            return [f"{{ V128 t{{}}; {body} c.v[{d.num}] = t; }}"]
        iops = {"add": "+", "sub": "-", "mul": "*", "and": "&", "orr": "|", "eor": "^", "bic": "& ~"}
        if m in iops and len(ops) == 3 and ops[2].type == REG:
            a, b = self.reg(ops[1]), self.reg(ops[2])
            body = " ".join(f"t.u{lane}[{i}] = uint{lane}_t(c.v[{a.num}].u{lane}[{i}] {iops[m]} c.v[{b.num}].u{lane}[{i}]);" for i in range(count))
            return [f"{{ V128 t{{}}; {body} c.v[{d.num}] = t; }}"]
        minmax = {"umin": ("u", "<"), "umax": ("u", ">"), "smin": ("s", "<"), "smax": ("s", ">")}
        if m in minmax and len(ops) == 3 and ops[2].type == REG:
            sg, cmp = minmax[m]
            a, b = self.reg(ops[1]), self.reg(ops[2])
            body = " ".join(f"{{ auto x = c.v[{a.num}].{sg}{lane}[{i}], y = c.v[{b.num}].{sg}{lane}[{i}]; t.{sg}{lane}[{i}] = x {cmp} y ? x : y; }}" for i in range(count))
            return [f"{{ V128 t{{}}; {body} c.v[{d.num}] = t; }}"]
        if m in ("uqadd", "uqsub", "sqadd", "sqsub") and len(ops) == 3 and ops[2].type == REG and lane <= 32:
            a, b = self.reg(ops[1]), self.reg(ops[2])
            sg = m[0]
            wide = "int64_t"
            lo, hi = (f"-(int64_t(1) << {lane - 1})", f"(int64_t(1) << {lane - 1}) - 1") if sg == "s" else ("0", f"(int64_t(1) << {lane}) - 1")
            op = "+" if "add" in m else "-"
            body = " ".join(f"{{ {wide} r = {wide}(c.v[{a.num}].{sg}{lane}[{i}]) {op} {wide}(c.v[{b.num}].{sg}{lane}[{i}]); t.{sg}{lane}[{i}] = {"int" if sg == "s" else "uint"}{lane}_t(r < {lo} ? {lo} : r > {hi} ? {hi} : r); }}" for i in range(count))
            return [f"{{ V128 t{{}}; {body} c.v[{d.num}] = t; }}"]
        if m == "xtn":
            s = self.reg(ops[1])
            body = " ".join(f"t.u{lane}[{i}] = uint{lane}_t(c.v[{s.num}].u{lane * 2}[{i}]);" for i in range(count))
            return [f"{{ V128 t{{}}; {body} c.v[{d.num}] = t; }}"]
        if m == "ext":
            a, b = self.reg(ops[1]), self.reg(ops[2])
            n = ops[3].imm
            total = count * lane // 8
            body = " ".join(f"t.u8[{i}] = {'c.v[%d].u8[%d]' % (a.num, i + n) if i + n < total else 'c.v[%d].u8[%d]' % (b.num, i + n - total)};" for i in range(total))
            return [f"{{ V128 t{{}}; {body} c.v[{d.num}] = t; }}"]
        if m == "rev64":
            s = self.reg(ops[1])
            per = 64 // lane
            body = " ".join(f"t.u{lane}[{i}] = c.v[{s.num}].u{lane}[{(i // per) * per + per - 1 - i % per}];" for i in range(count))
            return [f"{{ V128 t{{}}; {body} c.v[{d.num}] = t; }}"]
        if m == "sshll":
            s = self.reg(ops[1])
            sh = ops[2].imm
            body = " ".join(f"t.s{lane}[{i}] = int{lane}_t(c.v[{s.num}].s{lane // 2}[{i}]) << {sh};" for i in range(count))
            return [f"{{ V128 t{{}}; {body} c.v[{d.num}] = t; }}"]
        if m in ("scvtf", "ucvtf", "fcvtzs", "fcvtzu", "fneg", "fabs"):
            s = self.reg(ops[1])
            ft, it = ("f32", "32") if lane == 32 else ("f64", "64")
            fty = "float" if lane == 32 else "double"
            fbits = ops[2].imm if len(ops) > 2 and ops[2].type == IMM else 0
            scale = f" * {fty}({1.0 / (1 << fbits)!r})" if fbits else ""
            if m == "scvtf":
                body = " ".join(f"t.{ft}[{i}] = {fty}(c.v[{s.num}].s{it}[{i}]){scale};" for i in range(count))
            elif m == "ucvtf":
                body = " ".join(f"t.{ft}[{i}] = {fty}(c.v[{s.num}].u{it}[{i}]){scale};" for i in range(count))
            elif m == "fneg":
                body = " ".join(f"t.{ft}[{i}] = -c.v[{s.num}].{ft}[{i}];" for i in range(count))
            elif m == "fabs":
                body = " ".join(f"t.{ft}[{i}] = std::fabs(c.v[{s.num}].{ft}[{i}]);" for i in range(count))
            else:
                sig = "int" if m == "fcvtzs" else "uint"
                mul = f" * {fty}({float(1 << fbits)!r})" if fbits else ""
                body = " ".join(f"t.{'s' if sig == 'int' else 'u'}{it}[{i}] = FcvtSat<{sig}{it}_t>(c.v[{s.num}].{ft}[{i}]{mul});" for i in range(count))
            return [f"{{ V128 t{{}}; {body} c.v[{d.num}] = t; }}"]
        raise Unsupported(f"vector {m} {ins.op_str}")


LOADS = {"ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "ldrsw", "ldur", "ldurb", "ldurh", "ldursb", "ldursh", "ldursw", "ldp", "ldpsw"}
STORES = {"str", "strb", "strh", "stur", "sturb", "sturh", "stp"}


def vas_lane_bits(vas):
    names = {A.ARM64_VAS_8B: 8, A.ARM64_VAS_16B: 8, A.ARM64_VAS_4H: 16, A.ARM64_VAS_8H: 16,
             A.ARM64_VAS_2S: 32, A.ARM64_VAS_4S: 32, A.ARM64_VAS_1D: 64, A.ARM64_VAS_2D: 64,
             A.ARM64_VAS_1B: 8, A.ARM64_VAS_1H: 16, A.ARM64_VAS_1S: 32}
    if vas not in names:
        raise Unsupported(f"vas {vas}")
    return names[vas]


def vas_count(vas):
    counts = {A.ARM64_VAS_8B: 8, A.ARM64_VAS_16B: 16, A.ARM64_VAS_4H: 4, A.ARM64_VAS_8H: 8,
              A.ARM64_VAS_2S: 2, A.ARM64_VAS_4S: 4, A.ARM64_VAS_1D: 1, A.ARM64_VAS_2D: 2,
              A.ARM64_VAS_1B: 1, A.ARM64_VAS_1H: 1, A.ARM64_VAS_1S: 1}
    if vas not in counts:
        raise Unsupported(f"vas {vas}")
    return counts[vas]


def protect(body, site, wrapper_typeinfo):
    """Wraps a call in try/catch that enters the original landing pad."""
    lp = f"goto L_{site['landing_pad']:x};"
    wrapper_sel = next((x["selector"] for x in site["catches"] if x["type"] == wrapper_typeinfo), None)
    catch_all_sel = next((x["selector"] for x in site["catches"] if x["type"] == 0), None)
    if wrapper_sel is not None:
        on_wrapper = f"LandingCatch(c, e, {wrapper_sel}); {lp}"
    elif catch_all_sel is not None:
        on_wrapper = f"LandingCatchAll(c, {catch_all_sel}); {lp}"
    elif site["cleanup"]:
        on_wrapper = f"LandingCleanup(c); {lp}"
    else:
        on_wrapper = "throw;"
    if catch_all_sel is not None:
        on_other = f"LandingCatchAll(c, {catch_all_sel}); {lp}"
    elif site["cleanup"]:
        on_other = f"LandingCleanup(c); {lp}"
    else:
        on_other = "throw;"
    restore = "cs_.Restore(c); "
    on_wrapper = on_wrapper if on_wrapper == "throw;" else restore + on_wrapper
    on_other = on_other if on_other == "throw;" else restore + on_other
    return ["{ CalleeSaved cs_(c); try { " + " ".join(body) + " }",
            f"catch (const Il2CppExceptionWrapper& e) {{ (void)e; {on_wrapper} }}",
            f"catch (...) {{ {on_other} }} }}"]


def signature_keys(em, signature):
    ret, params = abi.split_signature(signature)
    if params and params[-1].startswith("const MethodInfo*"):
        params = params[:-1]
    return abi.normalize(ret, em.types), [abi.normalize(p, em.types) for p in params]


def icall_signature(em, name):
    """Native signature of the extern method an icall string names, from the
    method's own entry in script.json (overloads told apart by arity)."""
    if name in em.icall_cache:
        return em.icall_cache[name]
    result = None
    if "::" in name:
        cls, rest = name.split("::", 1)
        if "(" in rest:
            meth, args = rest.split("(", 1)
            nargs = len([a for a in args.rstrip(")").split(",") if a.strip()])
        else:
            meth, nargs = rest, None
        cands = []
        for sig in em.sig_by_name.get(cls.replace("/", ".") + "$$" + meth, []):
            _, params = abi.split_signature(sig)
            params = [p for p in params if not p.startswith("const MethodInfo*")]
            has_this = bool(re.search(r"\([^,)]*\b__this\b", sig))
            if (nargs is None or len(params) - has_this == nargs) and sig not in cands:
                cands.append(sig)
        if len({tuple(signature_keys(em, c)[1]) + (signature_keys(em, c)[0],) for c in cands if _keys_ok(em, c)}) == 1:
            result = signature_keys(em, next(c for c in cands if _keys_ok(em, c)))
    em.icall_cache[name] = result
    return result


def _keys_ok(em, sig):
    try:
        signature_keys(em, sig)
        return True
    except KeyError:
        return False


def native_call_sites(em, code, gva, func, insns):
    """Maps each blr/br that calls a pointer returned by a native resolver to
    the native signature to call it with.

    IL2CPP caches each resolved pointer in a static: resolve, store x0 to the
    cache, and later load the cache into the register the blr uses. Registers
    are followed linearly, which is enough for these local patterns."""
    start = func["start"]
    resolver_calls = [i for i in insns if i.mnemonic == "bl" and i.operands and i.operands[0].imm in em.native_resolvers]
    if not resolver_calls:
        return {}
    own = None
    if start in em.signatures:
        try:
            own = signature_keys(em, em.signatures[start])
        except KeyError:
            pass

    def cstring(va):
        off = va - gva
        if not 0 <= off < len(code):
            return None
        end = code.find(b"\0", off, off + 1024)
        return code[off:end].decode("utf-8", "replace") if end > 0 else None

    def pinvoke_signature(args_off):
        # PInvokeArguments on the stack: {moduleName, entryPoint} string views
        # first. The wrapper is often inlined into a caller, so the method
        # being translated is not necessarily the extern itself.
        entry = cstring(stack_addrs[args_off + 16]) if args_off is not None and args_off + 16 in stack_addrs else None
        if not entry:
            return None
        cls, _, meth = func["name"].rpartition("$$")
        if meth == entry:
            return own
        cands = [c for c in em.sig_by_name.get(cls + "$$" + entry, []) if _keys_ok(em, c)]
        keys = {tuple(signature_keys(em, c)[1]) + (signature_keys(em, c)[0],) for c in cands}
        return signature_keys(em, cands[0]) if len(keys) == 1 else None

    caches = {}
    out = {}
    for pass_no in (0, 1):
        regs = {}
        stack_addrs = {}
        for ins in insns:
            m, ops = ins.mnemonic, ins.operands
            dst = em.md.reg_name(ops[0].reg) if ops and ops[0].type == REG else None
            if m == "stp" and len(ops) == 3 and ops[2].type == MEM and em.md.reg_name(ops[2].mem.base) == "sp":
                for n in (0, 1):
                    r = em.md.reg_name(ops[n].reg) if ops[n].type == REG else None
                    if r in regs and regs[r][0] == "addr":
                        stack_addrs[ops[2].mem.disp + 8 * n] = regs[r][1]
                    else:
                        stack_addrs.pop(ops[2].mem.disp + 8 * n, None)
                continue
            if m == "add" and dst and len(ops) > 2 and ops[1].type == REG and em.md.reg_name(ops[1].reg) == "sp" and ops[2].type == IMM:
                regs[dst] = ("sp", ops[2].imm)
                continue
            if m == "mov" and dst and len(ops) == 2 and ops[1].type == REG and em.md.reg_name(ops[1].reg) == "sp":
                regs[dst] = ("sp", 0)
                continue
            if m == "adrp" and dst:
                regs[dst] = ("page", ops[1].imm)
                continue
            if m in ("add", "adr") and dst:
                if m == "adr":
                    regs[dst] = ("addr", ops[1].imm)
                    continue
                src = em.md.reg_name(ops[1].reg) if ops[1].type == REG else None
                if src in regs and regs[src][0] == "page" and len(ops) > 2 and ops[2].type == IMM:
                    regs[dst] = ("addr", regs[src][1] + ops[2].imm)
                    continue
            if m == "ldr" and dst and ops[1].type == MEM:
                base = em.md.reg_name(ops[1].mem.base)
                if base in regs and regs[base][0] == "page":
                    va = regs[base][1] + ops[1].mem.disp
                    if va in caches:
                        regs[dst] = caches[va]
                        continue
            if m == "str" and ops and ops[0].type == REG and ops[1].type == MEM:
                src = em.md.reg_name(ops[0].reg)
                base = em.md.reg_name(ops[1].mem.base)
                if src in regs and regs[src][0] == "native" and base in regs and regs[base][0] == "page":
                    caches[regs[base][1] + ops[1].mem.disp] = regs[src]
                continue
            if m == "mov" and dst and len(ops) == 2 and ops[1].type == REG:
                src = em.md.reg_name(ops[1].reg)
                if src in regs:
                    regs[dst] = regs[src]
                else:
                    regs.pop(dst, None)
                continue
            if m == "bl":
                target = ops[0].imm
                if target in em.native_resolvers:
                    sig = None
                    if target == em.icall_resolver and regs.get("x0", ("",))[0] == "addr":
                        name = cstring(regs["x0"][1])
                        sig = icall_signature(em, name) if name else None
                        if sig is None and name and "::" in name:
                            cls, rest = name.split("::", 1)
                            if cls.replace("/", ".") + "$$" + rest.split("(")[0] == func["name"]:
                                sig = own  # the extern method's own body
                    else:
                        x0 = regs.get("x0", ("",))
                        sig = pinvoke_signature(x0[1] if x0[0] == "sp" else None)
                    for k in list(regs):
                        if k.startswith("x") and k[1:].isdigit() and int(k[1:]) <= 18:
                            del regs[k]
                    if sig is not None:
                        regs["x0"] = ("native", sig)
                    continue
                for k in list(regs):
                    if k.startswith("x") and k[1:].isdigit() and int(k[1:]) <= 18:
                        del regs[k]
                continue
            if m in ("blr", "br") and pass_no == 1:
                reg = em.md.reg_name(ops[0].reg)
                if reg in regs and regs[reg][0] == "native":
                    out[ins.address] = regs[reg][1]
                if m == "blr":
                    for k in list(regs):
                        if k.startswith("x") and k[1:].isdigit() and int(k[1:]) <= 18:
                            del regs[k]
                continue
            if dst:
                regs.pop(dst, None)
                if dst.startswith("w"):
                    regs.pop("x" + dst[1:], None)
    if len(out) < len(resolver_calls):
        em.unsupported["native call site without a signature"] += len(resolver_calls) - len(out)
    return out


def translate_function(em, code, gva, func):
    start, end = func["start"], func["end"]
    insns = list(em.md.disasm(code[start - gva:end - gva], start))
    func["native_calls"] = native_call_sites(em, code, gva, func, insns) if em.native_resolvers else {}
    # Jump-table branches (switch): the target is computed from a table as
    # base + entry << 2. Other br instructions are tail calls.
    tables = set()
    for k, ins in enumerate(insns):
        if ins.mnemonic != "br" or not ins.operands:
            continue
        reg = ins.operands[0].reg
        for prev in reversed(insns[max(0, k - 8):k]):
            if prev.mnemonic == "adr":
                tables.add(ins.address)
                break
            if prev.operands and prev.operands[0].type == REG and prev.operands[0].reg == reg:
                if prev.mnemonic == "add" and re.search(r"(lsl|sxtw|uxtw|sxth|uxth|sxtb|uxtb) #2", prev.op_str):
                    tables.add(ins.address)
                break
    func["table_branches"] = tables
    targets = set()
    for ins in insns:
        for o in ins.operands:
            if o.type == IMM and ins.mnemonic in BRANCHY and start <= o.imm < end:
                targets.add(o.imm)
    sites = em.lsda.get(str(start), [])
    for site in sites:
        if start <= site["landing_pad"] < end:
            targets.add(site["landing_pad"])
    lines = []
    func["has_br"] = False
    ok = True
    for ins in insns:
        try:
            body = em.translate(ins, func)
        except (Unsupported, KeyError, ValueError, IndexError) as e:
            em.unsupported[f"{ins.mnemonic} {e}"] += 1
            body = [f'Unimplemented(c, {ins.address:#x}, "{ins.mnemonic} {ins.op_str}");']
            ok = False
        if sites and ins.mnemonic in ("bl", "blr"):
            site = next((x for x in sites if x["start"] <= ins.address < x["end"]), None)
            if site and start <= site["landing_pad"] < end:
                body = protect(body, site, em.wrapper_typeinfo)
        lines.append((ins.address, ins, body))
    if func["has_br"]:
        targets.update(a for a, _, _ in lines)

    out = [f"// {func['name']}", f"void f_{start:x}(Ctx& c)", "{"]
    if func["has_br"]:
        cases = " ".join(f"case {t:#x}: goto L_{t:x};" for t in sorted(targets))
        out.append(f"#define JUMP_INDIRECT(t) switch ((t) - g_imageBase) {{ {cases} default: CallIndirect(c, (t)); return; }}")
    for addr, ins, body in lines:
        if addr in targets:
            out.append(f"L_{addr:x}:")
        out.append(f"    // {addr:x}: {ins.mnemonic} {ins.op_str}")
        out.extend("    " + b for b in body)
    if func["has_br"]:
        out.append("#undef JUMP_INDIRECT")
    # falling off the end (function boundary approximation) continues in the next function
    out.append(f"    {'f_%x(c);' % end if end in em.func_addrs else 'Trap(c, %#x, 0);' % end}")
    out.append("}")
    return out, ok, len(insns)


BRANCHY = {"b", "b.eq", "b.ne", "b.hs", "b.lo", "b.mi", "b.pl", "b.vs", "b.vc", "b.hi", "b.ls", "b.ge", "b.lt", "b.gt", "b.le", "b.al",
           "cbz", "cbnz", "tbz", "tbnz"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("table")
    ap.add_argument("out_dir")
    ap.add_argument("--only", help="comma separated function addresses (hex)")
    ap.add_argument("--only-file", help="file with one hex address per line")
    ap.add_argument("--split", type=int, default=1)
    ap.add_argument("--signatures", help="Il2CppDumper script.json (enables typed icall/P/Invoke calls)")
    ap.add_argument("--types", help="type_classes.json from classify_types.py")
    ap.add_argument("--native-resolvers", default="", help="comma separated helper addresses that return native function pointers")
    ap.add_argument("--lsda", help="lsda.json from lsda.py (enables exception landing pads)")
    ap.add_argument("--wrapper-typeinfo", default="0", help="address of Il2CppExceptionWrapper's typeinfo")
    ap.add_argument("--override", action="append", default=[],
                    help="addr=symbol: the function's body becomes a call to native recomp::symbol(Ctx&)")
    ap.add_argument("--hook", action="append", default=[],
                    help="addr=symbol: the function stays recompiled as f_<addr>_orig and f_<addr> calls native "
                         "recomp::symbol(Ctx&) first, which calls f_<addr>_orig itself")
    ap.add_argument("--extra-table", action="store_true",
                    help="emit kExtraFunctionTable instead of kFunctionTable (a second, separately built set)")
    args = ap.parse_args()

    macho = lief.parse(args.binary)
    # Functions may live in the il2cpp section or in __text (generated
    # invokers, attribute generators, thunks), so read the whole segment.
    seg = next(s for s in macho.segments if s.name == "__TEXT")
    gva, code = seg.virtual_address, bytes(seg.content)
    table = json.load(open(args.table))["functions"]
    funcs = [{"start": f["addr"], "end": f["addr"] + f["size"], "name": f["name"]} for f in table]
    all_addrs = {f["start"] for f in funcs}

    selected = funcs
    wanted = None
    if args.only:
        wanted = {int(a, 16) for a in args.only.split(",")}
    if args.only_file:
        wanted = {int(l.strip(), 16) for l in open(args.only_file) if l.strip()}
    if wanted is not None:
        selected = [f for f in funcs if f["start"] in wanted]

    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.detail = True
    # Calls to functions outside the selection go through CallImport, so a
    # partial build links on its own.
    em = Emitter(md, {f["start"] for f in selected}, None)
    em.signatures, em.types, em.native_resolvers = {}, {}, set()
    em.sig_by_name, em.icall_cache, em.icall_resolver = {}, {}, None
    if args.signatures and args.types:
        for m in json.load(open(args.signatures))["ScriptMethod"]:
            em.signatures.setdefault(m["Address"], m["Signature"])
            em.sig_by_name.setdefault(m["Name"], []).append(m["Signature"])
        em.types = json.load(open(args.types))
        resolvers = [int(a, 16) for a in args.native_resolvers.split(",") if a]
        em.native_resolvers = set(resolvers)
        em.icall_resolver = resolvers[0] if resolvers else None  # the first one takes an icall name
    em.lsda = json.load(open(args.lsda))["functions"] if args.lsda else {}
    em.wrapper_typeinfo = int(args.wrapper_typeinfo, 16)

    overrides = {int(a, 16): sym for a, sym in (o.split("=", 1) for o in args.override)}
    hooks = {int(a, 16): sym for a, sym in (o.split("=", 1) for o in args.hook)}
    os.makedirs(args.out_dir, exist_ok=True)
    chunks = [selected[i::args.split] for i in range(args.split)]
    total_ins = total_ok = 0
    clean = 0
    for n, chunk in enumerate(chunks):
        with open(os.path.join(args.out_dir, f"recomp_{n:03d}.cpp"), "w") as out:
            out.write('#include "recomp_ctx.h"\n#include "recomp_bridge.h"\n#include "recomp_eh.h"\n#include "recomp_simd.h"\n#include "recomp_decls.h"\n\nnamespace recomp\n{\n')
            for f in chunk:
                if f["start"] in overrides:
                    sym = overrides[f["start"]]
                    out.write(f"void {sym}(Ctx& c);\n// {f['name']} (native override)\nvoid f_{f['start']:x}(Ctx& c)\n{{\n    {sym}(c);\n}}\n\n")
                    clean += 1
                    continue
                lines, ok, count = translate_function(em, code, gva, f)
                total_ins += count
                clean += ok
                if f["start"] in hooks:
                    sym, name = hooks[f["start"]], f"f_{f['start']:x}"
                    head = f"void {name}(Ctx& c)"
                    i = next(k for k, l in enumerate(lines) if l.startswith(head))
                    lines[i] = lines[i].replace(head, f"void {name}_orig(Ctx& c)", 1)
                    out.write(f"void {sym}(Ctx& c);\n// {f['name']} (native hook)\nvoid {name}(Ctx& c)\n{{\n    {sym}(c);\n}}\n\n")
                out.write("\n".join(lines) + "\n\n")
            out.write("}\n")
    with open(os.path.join(args.out_dir, "recomp_decls.h"), "w") as out:
        out.write('#pragma once\n#include "recomp_ctx.h"\n\nnamespace recomp\n{\n')
        for f in selected:
            out.write(f"void f_{f['start']:x}(Ctx& c);\n")
        out.write("}\n")
    with open(os.path.join(args.out_dir, "recomp_table.cpp"), "w") as out:
        out.write('#include "recomp_decls.h"\n\nnamespace recomp\n{\n')
        table = "kExtraFunction" if args.extra_table else "kFunction"
        out.write(f"extern const FunctionEntry {table}Table[] = {{\n")
        for f in sorted(selected, key=lambda f: f["start"]):
            out.write(f"    {{ {f['start']:#x}, f_{f['start']:x} }},\n")
        out.write("};\n")
        out.write(f"extern const size_t {table}Count = {len(selected)};\n}}\n")

    print(f"functions: {len(selected)}  fully translated: {clean}  instructions: {total_ins}")
    for k, v in em.unsupported.most_common(40):
        print(f"  unsupported {v:7d}  {k}")


if __name__ == "__main__":
    main()
