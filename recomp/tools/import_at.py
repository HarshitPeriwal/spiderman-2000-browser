"""Names the Windows import called just before a return address (for "call to an unimplemented Windows
import" traps, which report the caller's return address).

Usage: import_at.py <exe> <return address hex> [...]
"""
import re
import sys

import capstone
import pefile

pe = pefile.PE(sys.argv[1])
base = pe.OPTIONAL_HEADER.ImageBase
iat = {i.address: f"{imp.dll.decode()}!{(i.name or b'#%d' % i.ordinal).decode()}"
       for imp in pe.DIRECTORY_ENTRY_IMPORT for i in imp.imports}
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)


def target(op):
    m = re.search(r"\[0x([0-9a-f]+)\]", op)
    return int(m.group(1), 16) if m else None


def follow(addr):
    """call [iat] directly, or call thunk -> jmp [iat]"""
    off = pe.get_offset_from_rva(addr - base)
    ins = next(md.disasm(pe.__data__[off:off + 16], addr, count=1), None)
    if ins and ins.mnemonic == "jmp" and target(ins.op_str) in iat:
        return iat[target(ins.op_str)]
    return None


for ret in sys.argv[2:]:
    ret = int(ret, 16)
    found = None
    for size in (6, 5, 3, 2):   # call [mem] = 6 bytes, call rel32 = 5, call [reg+d8] = 3, call reg = 2
        off = pe.get_offset_from_rva(ret - size - base)
        ins = next(md.disasm(pe.__data__[off:off + size], ret - size, count=1), None)
        if not ins or ins.mnemonic != "call" or ins.address + ins.size != ret:
            continue
        t = target(ins.op_str)
        if t in iat:
            found = iat[t]
        elif ins.op_str.startswith("0x"):
            found = follow(int(ins.op_str, 16))
        found = found or f"{ins.mnemonic} {ins.op_str} (not an import)"
        break
    print(f"{ret:08x}: {found or 'no call found before this address'}")
