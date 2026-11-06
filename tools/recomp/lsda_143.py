# Parses the exception tables of the game binary: compact unwind's LSDA index
# (__unwind_info) and the GCC-style LSDAs in __gcc_except_tab.
#
# usage: python3 -I lsda.py <GameAssembly arm64> <out.json>
#
# Output: {"functions": {"<func addr>": [{"start", "end", "landing_pad",
#           "catches": [type addr or 0 for catch-all...], "cleanup": bool}]},
#          "types": {"<type addr>": count}}
# Addresses are image virtual addresses. Type entries are typeinfo addresses
# (0 = catch(...)); "catches" lists them in action order with their selector.
import sys, json, struct, collections
import lief

binary, out_path = sys.argv[1], sys.argv[2]
b = lief.parse(binary)
data = open(binary, "rb").read()
segments = [(s.virtual_address, s.virtual_size, s.file_offset, s.file_size) for s in b.segments]

def fo(va):
    for vaddr, vsize, foff, fsize in segments:
        if vaddr <= va < vaddr + fsize:
            return foff + (va - vaddr)
    raise ValueError(hex(va))

unwind = b.get_section("__unwind_info")
uw = bytes(unwind.content)
version, common_off, common_count, pers_off, pers_count, index_off, index_count = struct.unpack_from("<7I", uw, 0)

# First-level index: {functionOffset, secondLevelPagesOffset, lsdaIndexArrayOffset}
index = [struct.unpack_from("<3I", uw, index_off + 12 * i) for i in range(index_count)]
lsda_start = index[0][2]
lsda_end = index[-1][2]
lsda_entries = [struct.unpack_from("<2I", uw, lsda_start + 8 * i) for i in range((lsda_end - lsda_start) // 8)]

def uleb(buf, p):
    r = s = 0
    while True:
        byte = buf[p]; p += 1
        r |= (byte & 0x7f) << s; s += 7
        if not byte & 0x80:
            return r, p

def sleb(buf, p):
    r = s = 0
    while True:
        byte = buf[p]; p += 1
        r |= (byte & 0x7f) << s; s += 7
        if not byte & 0x80:
            if byte & 0x40:
                r -= 1 << s
            return r, p

def read_encoded(enc, va):
    """Reads a DW_EH_PE value at virtual address va; returns (value, size)."""
    if enc == 0xff:
        return 0, 0
    fmt = enc & 0x0f
    off = fo(va)
    if fmt == 0x01:
        v, p = uleb(data, off); size = p - off
    elif fmt == 0x0b:
        v = struct.unpack_from("<i", data, off)[0]; size = 4
    elif fmt == 0x03:
        v = struct.unpack_from("<I", data, off)[0]; size = 4
    elif fmt in (0x00, 0x04):
        v = struct.unpack_from("<Q", data, off)[0]; size = 8
    else:
        raise ValueError(f"encoding {enc:#x}")
    # DW_EH_PE encoded null remains null, including pcrel|indirect.
    if v == 0:
        return 0, size
    if enc & 0x70 == 0x10:  # pcrel
        v += va
    if enc & 0x80:          # indirect
        v = struct.unpack_from("<Q", data, fo(v))[0]
    return v, size

functions = {}
type_counts = collections.Counter()
for func_off, lsda_va in lsda_entries:
    func = func_off
    p = lsda_va
    lp_enc = data[fo(p)]; p += 1
    lp_start = func
    if lp_enc != 0xff:
        lp_start, n = read_encoded(lp_enc, p); p += n
    tt_enc = data[fo(p)]; p += 1
    tt_base = None
    if tt_enc != 0xff:
        off, q = uleb(data, fo(p))
        tt_base = p + (q - fo(p)) + off
        p += q - fo(p)
    cs_enc = data[fo(p)]; p += 1
    cs_len, q = uleb(data, fo(p)); p += q - fo(p)
    cs_end = p + cs_len
    action_base = cs_end
    sites = []
    while p < cs_end:
        start, n = read_encoded(cs_enc & 0x0f, p); p += n
        length, n = read_encoded(cs_enc & 0x0f, p); p += n
        lp, n = read_encoded(cs_enc & 0x0f, p); p += n
        action, q = uleb(data, fo(p)); p += q - fo(p)
        if not lp:
            continue
        catches, cleanup = [], False
        if action == 0:
            cleanup = True
        else:
            a = action_base + action - 1
            while True:
                filt, q = sleb(data, fo(a))
                next_va = a + (q - fo(a))
                disp, _ = sleb(data, fo(next_va))
                if filt == 0:
                    cleanup = True
                elif filt > 0:
                    size = {0x03: 4, 0x0b: 4, 0x00: 8, 0x04: 8}[tt_enc & 0x0f]
                    tinfo, _ = read_encoded(tt_enc, tt_base - filt * size)
                    catches.append({"selector": filt, "type": tinfo})
                    type_counts[tinfo] += 1
                if disp == 0:
                    break
                a = next_va + disp
        sites.append({"start": func + start, "end": func + start + length, "landing_pad": lp_start + lp,
                      "catches": catches, "cleanup": cleanup})
    if sites:
        functions[str(func)] = sites

json.dump({"functions": functions, "types": {str(k): v for k, v in type_counts.items()}}, open(out_path, "w"))
print(f"functions with landing pads: {len(functions)}, call sites: {sum(len(v) for v in functions.values())}")
syms = {s.value: s.name for s in b.symbols if s.value}
for t, n in type_counts.most_common(10):
    print(f"  catches {t:#x} {syms.get(t, '?')}: {n}")
