# PlatformIO post-link validation; no dependency source files are modified.
from pathlib import Path
import subprocess
import sys

Import("env")
PROJECT = Path(env.subst("$PROJECT_DIR"))

def check_isr_placement(source, target, env):
    defines = {}
    for define in env.get("CPPDEFINES", []):
        if isinstance(define, (tuple, list)):
            defines[define[0]] = define[1] if len(define) > 1 else 1
        else:
            defines[define] = 1
    if "EBUS_INTERNAL" not in defines or defines.get("EBUS_SIMULATION", 0):
        return
    toolchain = env.PioPlatform().get_package_dir("toolchain-riscv32-esp")
    objdump = Path(toolchain) / "bin" / "riscv32-esp-elf-objdump"
    if sys.platform == "win32":
        objdump = objdump.with_suffix(".exe")
    subprocess.run([sys.executable, str(PROJECT / "scripts/check_ebus_iram.py"),
                    str(target[0]), "--objdump", str(objdump)], check=True)

env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", check_isr_placement)
