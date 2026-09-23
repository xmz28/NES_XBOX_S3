"""PlatformIO pre-script: build and flash the raw NES ROM partition."""

from pathlib import Path
import os
import sys

Import("env")  # type: ignore[name-defined]  # Provided by PlatformIO/SCons.

project_dir = Path(env.subst("$PROJECT_DIR"))  # type: ignore[name-defined]
build_dir = Path(env.subst("$BUILD_DIR"))  # type: ignore[name-defined]
sys.path.insert(0, str(project_dir / "tools"))

from build_rom_pack import build_pack  # noqa: E402

pack_path = build_dir / "roms.bin"


def project_path(variable: str, default: str) -> Path:
    value = Path(os.environ.get(variable, default))
    return value if value.is_absolute() else project_dir / value


build_pack(
    project_path("NES_ROM_MANIFEST", "roms/manifest.csv"),
    project_path("NES_ROM_ROOT", "."),
    pack_path,
    0xE70000,
)
env.Append(FLASH_EXTRA_IMAGES=[("0x190000", str(pack_path))])  # type: ignore[name-defined]
