#!/usr/bin/env python3
"""Issue 86: name the frames of a trap inside an ok image.

An ok image (clangd.okb) is a zip of one static-pie ELF per architecture, not stripped. okrun maps it
itself, so a Windows debugger sees no module there: only raw addresses. This script takes what cdb
printed at the trap (capture.cdb) and the image's ELF, finds where the image was mapped by matching
the bytes around the instruction pointer against the ELF's text, and names each return address from
the ELF's own symbol table.

    symbolize.py --elf x86_64/clangd.oke --cdb-output run.cdb.txt [--out stack.txt]
"""
import argparse
import bisect
import re
import shutil
import struct
import subprocess
import sys


class Elf:
    def __init__(self, path):
        with open(path, "rb") as handle:
            self.data = handle.read()
        data = self.data
        if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
            raise ValueError(f"{path}: not a little-endian ELF64")
        (self.phoff, self.shoff) = struct.unpack_from("<QQ", data, 0x20)
        (self.phentsize, self.phnum, self.shentsize, self.shnum, self.shstrndx) = struct.unpack_from("<HHHHH", data, 0x36)
        self.loads = []   # (vaddr, offset, filesz, memsz, flags)
        for index in range(self.phnum):
            at = self.phoff + index * self.phentsize
            p_type, p_flags, p_offset, p_vaddr, _p_paddr, p_filesz, p_memsz, _align = struct.unpack_from("<IIQQQQQQ", data, at)
            if p_type == 1:
                self.loads.append((p_vaddr, p_offset, p_filesz, p_memsz, p_flags))
        self.span = max((vaddr + memsz for vaddr, _o, _f, memsz, _fl in self.loads), default=0)
        self.symbols = []   # (value, size, name)
        sections = []
        for index in range(self.shnum):
            at = self.shoff + index * self.shentsize
            sections.append(struct.unpack_from("<IIQQQQIIQQ", data, at))
        for section in sections:
            _name, sh_type, _flags, _addr, sh_offset, sh_size, sh_link, _info, _align, sh_entsize = section
            if sh_type != 2 or sh_entsize == 0:   # SHT_SYMTAB
                continue
            strtab = sections[sh_link]
            str_off, str_size = strtab[4], strtab[5]
            strings = data[str_off:str_off + str_size]
            for i in range(sh_size // sh_entsize):
                st_name, st_info, _other, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", data, sh_offset + i * sh_entsize)
                if (st_info & 0xF) != 2 or st_shndx == 0:   # STT_FUNC, defined
                    continue
                end = strings.find(b"\0", st_name)
                self.symbols.append((st_value, st_size, strings[st_name:end].decode("utf-8", "replace")))
        self.symbols.sort()
        self.starts = [value for value, _size, _name in self.symbols]

    def executable(self, vaddr):
        return any(start <= vaddr < start + memsz and (flags & 1) for start, _o, _f, memsz, flags in self.loads)

    def find_bytes(self, needle):
        """Every virtual address at which `needle` lies in an executable segment's file bytes."""
        found = []
        for vaddr, offset, filesz, _memsz, flags in self.loads:
            if not flags & 1:
                continue
            segment = self.data[offset:offset + filesz]
            at = segment.find(needle)
            while at != -1:
                found.append(vaddr + at)
                at = segment.find(needle, at + 1)
        return found

    def name(self, vaddr):
        index = bisect.bisect_right(self.starts, vaddr) - 1
        if index < 0:
            return None
        value, size, name = self.symbols[index]
        if size and vaddr >= value + size + 16:
            return None
        return name, vaddr - value


def number(text):
    return int(text.replace("`", ""), 16)


def parse_capture(text):
    capture = {"rip": None, "rbp": None, "rsp": None, "bytes": {}, "frames": [], "stack": [], "exception": None}
    for match in re.finditer(r"\b(rip|rbp|rsp)=([0-9a-fA-F`]{8,17})", text):
        if capture[match.group(1)] is None:
            capture[match.group(1)] = number(match.group(2))
    match = re.search(r"ExceptionCode:\s*([0-9a-fA-F]+)", text)
    if match:
        capture["exception"] = match.group(1)
    section = None
    for line in text.splitlines():
        marker = re.match(r"^===([A-Z]+)===\s*$", line.strip())
        if marker:
            section = marker.group(1)
            continue
        if section == "BYTES":
            match = re.match(r"^([0-9a-fA-F`]{8,17})\s+((?:[0-9a-fA-F]{2}[ -])+)", line)
            if match:
                address = number(match.group(1))
                for offset, byte in enumerate(re.findall(r"[0-9a-fA-F]{2}", match.group(2))[:16]):
                    capture["bytes"][address + offset] = int(byte, 16)
        elif section == "CHAIN":
            match = re.match(r"^FRAME\s+([0-9a-fA-F`]+)\s+RET\s+([0-9a-fA-F`]+)", line.strip())
            if match:
                capture["frames"].append((number(match.group(1)), number(match.group(2))))
        elif section == "STACK":
            match = re.match(r"^([0-9a-fA-F`]{8,17})\s+([0-9a-fA-F`]{8,17})", line)
            if match:
                capture["stack"].append((number(match.group(1)), number(match.group(2))))
    return capture


def locate_base(elf, capture):
    """The address the image's virtual address 0 was mapped at: the bytes around rip, found in the ELF."""
    rip = capture["rip"]
    if rip is None or not capture["bytes"]:
        return None, "no instruction pointer or bytes in the capture"
    addresses = sorted(capture["bytes"])
    for length in (64, 48, 32, 24, 16):
        # A window that ends just past the trapping instruction.
        start = max(addresses[0], rip + 2 - length)
        window = bytes(capture["bytes"].get(start + i, -1) & 0xFF for i in range(length)) if all((start + i) in capture["bytes"] for i in range(length)) else None
        if window is None:
            continue
        found = elf.find_bytes(window)
        if len(found) == 1:
            return start - found[0], f"{length} bytes around rip match the image's text once"
        if len(found) > 1:
            bases = sorted({start - vaddr for vaddr in found})
            aligned = [base for base in bases if base % 0x1000 == 0]
            if len(aligned) == 1:
                return aligned[0], f"{length} bytes around rip match {len(found)} places, one page-aligned"
            return None, f"{length} bytes around rip match {len(found)} places in the image"
    return None, "the bytes around rip are nowhere in the image's text (the trap is outside the image)"


def demangle(names):
    tool = shutil.which("llvm-cxxfilt") or shutil.which("c++filt")
    if not tool or not names:
        return {}
    try:
        done = subprocess.run([tool], input="\n".join(names), capture_output=True, text=True, timeout=60)
        lines = done.stdout.splitlines()
        if len(lines) == len(names):
            return dict(zip(names, lines))
    except (OSError, subprocess.SubprocessError):
        pass
    return {}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", required=True)
    parser.add_argument("--cdb-output", required=True)
    parser.add_argument("--out")
    parser.add_argument("--base", help="the image's base, when it is known (hex)")
    args = parser.parse_args()

    with open(args.cdb_output, "r", encoding="utf-8", errors="replace") as handle:
        capture = parse_capture(handle.read())
    elf = Elf(args.elf)
    lines = []
    emit = lines.append
    emit(f"exception {capture['exception']}  rip {capture['rip'] and hex(capture['rip'])}  rsp {capture['rsp'] and hex(capture['rsp'])}  rbp {capture['rbp'] and hex(capture['rbp'])}")
    if capture["rip"] is not None and capture["rip"] in capture["bytes"]:
        at = capture["rip"]
        emit("bytes at rip: " + " ".join(f"{capture['bytes'].get(at + i, 0):02x}" for i in range(8) if (at + i) in capture["bytes"]))
    if args.base:
        base, how = int(args.base, 16), "given"
    else:
        base, how = locate_base(elf, capture)
    emit(f"image base: {hex(base) if base is not None else 'unknown'} ({how}); image span {hex(elf.span)}; {len(elf.symbols)} function symbols")
    if base is None:
        text = "\n".join(lines) + "\n"
        sys.stdout.write(text)
        if args.out:
            open(args.out, "w", encoding="utf-8").write(text)
        return 1

    def inside(address):
        return base <= address < base + elf.span and elf.executable(address - base)

    wanted = []

    def describe(address):
        if not inside(address):
            return None
        named = elf.name(address - base)
        if not named:
            return f"image+{address - base:#x}"
        wanted.append(named[0])
        return (named[0], named[1], address - base)

    rows = []
    rows.append(("TRAP", capture["rip"], describe(capture["rip"])))
    for frame, ret in capture["frames"]:
        rows.append((f"fp {frame:#x}", ret, describe(ret)))
    scan = []
    for slot, value in capture["stack"]:
        described = describe(value)
        if described:
            scan.append((f"sp {slot:#x}", value, described))
    pretty = demangle(sorted(set(wanted)))

    def show(row):
        where, address, described = row
        if described is None:
            return f"  {where:<22} {address:#018x}  (outside the image)"
        if isinstance(described, str):
            return f"  {where:<22} {address:#018x}  {described}"
        name, offset, image_offset = described
        return f"  {where:<22} {address:#018x}  image+{image_offset:#x}  {pretty.get(name, name)}+{offset:#x}"

    emit("")
    emit("== the trap and its frame-pointer chain (innermost first) ==")
    for row in rows:
        emit(show(row))
    emit("")
    emit(f"== every stack slot that points into the image's text ({len(scan)} of {len(capture['stack'])} slots; a superset of the frames) ==")
    for row in scan[:400]:
        emit(show(row))
    text = "\n".join(lines) + "\n"
    sys.stdout.write(text)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as handle:
            handle.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
