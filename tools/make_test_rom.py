#!/usr/bin/env python3
"""Build a tiny original NROM test cartridge for hardware bring-up."""

from pathlib import Path


class Assembler:
    def __init__(self, origin: int) -> None:
        self.origin = origin
        self.code = bytearray()
        self.labels: dict[str, int] = {}
        self.relative_fixups: list[tuple[int, str]] = []
        self.absolute_fixups: list[tuple[int, str]] = []

    @property
    def pc(self) -> int:
        return self.origin + len(self.code)

    def label(self, name: str) -> None:
        self.labels[name] = self.pc

    def emit(self, *values: int) -> None:
        self.code.extend(value & 0xFF for value in values)

    def branch(self, opcode: int, label: str) -> None:
        self.emit(opcode, 0)
        self.relative_fixups.append((len(self.code) - 1, label))

    def absolute(self, opcode: int, label: str) -> None:
        self.emit(opcode, 0, 0)
        self.absolute_fixups.append((len(self.code) - 2, label))

    def finish(self) -> bytearray:
        for offset, label in self.relative_fixups:
            target = self.labels[label]
            next_pc = self.origin + offset + 1
            displacement = target - next_pc
            if not -128 <= displacement <= 127:
                raise ValueError(f"branch to {label} is out of range")
            self.code[offset] = displacement & 0xFF
        for offset, label in self.absolute_fixups:
            target = self.labels[label]
            self.code[offset] = target & 0xFF
            self.code[offset + 1] = target >> 8
        return self.code


def build_program() -> bytearray:
    a = Assembler(0x8000)
    a.label("reset")
    a.emit(0x78, 0xD8)                    # SEI; CLD
    a.emit(0xA2, 0x40, 0x8E, 0x17, 0x40)  # disable APU frame IRQ
    a.emit(0xA2, 0xFF, 0x9A, 0xE8)        # stack; X=0
    a.emit(0x8E, 0x00, 0x20)              # disable NMI
    a.emit(0x8E, 0x01, 0x20)              # disable rendering
    a.emit(0x8E, 0x10, 0x40)              # disable DMC IRQ

    for label in ("vblank1", "vblank2"):
        a.label(label)
        a.emit(0x2C, 0x02, 0x20)          # BIT $2002
        a.branch(0x10, label)              # BPL label

    a.emit(0xA9, 0x3F, 0x8D, 0x06, 0x20)
    a.emit(0xA9, 0x00, 0x8D, 0x06, 0x20)
    a.emit(0xA2, 0x00)                    # X=0
    a.label("palette_loop")
    a.absolute(0xBD, "palette")           # LDA palette,X
    a.emit(0x8D, 0x07, 0x20, 0xE8, 0xE0, 0x20)
    a.branch(0xD0, "palette_loop")

    a.emit(0xA9, 0x20, 0x8D, 0x06, 0x20)
    a.emit(0xA9, 0x00, 0x8D, 0x06, 0x20)
    a.emit(0xA9, 0x00, 0xA0, 0x03)        # blank tile, three pages
    a.label("page_loop")
    a.emit(0xA2, 0x00)
    a.label("page_fill")
    a.emit(0x8D, 0x07, 0x20, 0xE8)
    a.branch(0xD0, "page_fill")
    a.emit(0x88)
    a.branch(0xD0, "page_loop")

    a.emit(0xA2, 0xC0)                    # remaining 192 tile bytes
    a.label("tail_fill")
    a.emit(0x8D, 0x07, 0x20, 0xCA)
    a.branch(0xD0, "tail_fill")
    a.emit(0xA9, 0x00, 0xA2, 0x40)        # 64 attribute bytes
    a.label("attribute_fill")
    a.emit(0x8D, 0x07, 0x20, 0xCA)
    a.branch(0xD0, "attribute_fill")

    # Put a recognizable "NES TEST" label near the center of the nametable.
    a.emit(0xA9, 0x21, 0x8D, 0x06, 0x20)
    a.emit(0xA9, 0xCC, 0x8D, 0x06, 0x20)
    for tile in (1, 2, 3, 0, 4, 2, 3, 4):
        a.emit(0xA9, tile, 0x8D, 0x07, 0x20)

    a.emit(0xA9, 0x00, 0x8D, 0x05, 0x20, 0x8D, 0x05, 0x20)
    a.emit(0xA9, 0x0A, 0x8D, 0x01, 0x20)  # show background
    a.label("forever")
    a.absolute(0x4C, "forever")

    a.label("palette")
    a.emit(
        0x0F, 0x30, 0x16, 0x27, 0x0F, 0x21, 0x11, 0x01,
        0x0F, 0x30, 0x10, 0x00, 0x0F, 0x27, 0x17, 0x07,
        0x0F, 0x30, 0x16, 0x27, 0x0F, 0x21, 0x11, 0x01,
        0x0F, 0x30, 0x10, 0x00, 0x0F, 0x27, 0x17, 0x07,
    )
    return a.finish()


def main() -> None:
    project = Path(__file__).resolve().parents[1]
    output = project / "assets" / "test.nes"
    program = build_program()
    prg = bytearray([0xEA] * 0x4000)
    prg[: len(program)] = program
    for vector_offset in (0x3FFA, 0x3FFC, 0x3FFE):
        prg[vector_offset : vector_offset + 2] = (0x00, 0x80)

    chr_rom = bytearray(0x2000)
    glyphs = {
        1: (0x42, 0x62, 0x52, 0x4A, 0x46, 0x42, 0x42, 0x00),  # N
        2: (0x7E, 0x40, 0x40, 0x7C, 0x40, 0x40, 0x7E, 0x00),  # E
        3: (0x3C, 0x42, 0x40, 0x3C, 0x02, 0x42, 0x3C, 0x00),  # S
        4: (0x7F, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x00),  # T
    }
    for tile, rows in glyphs.items():
        start = tile * 16
        chr_rom[start : start + 8] = bytes(rows)
    header = b"NES\x1A" + bytes((1, 1, 0, 0)) + bytes(8)
    output.write_bytes(header + prg + chr_rom)
    print(f"wrote {output} ({output.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
