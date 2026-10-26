import json
import subprocess
from pathlib import Path

Import("env")

def get_firmware_version():
    force_version_file = Path("force_version.txt")
    if force_version_file.exists():
        build_version = force_version_file.read_text(encoding="utf-8").strip()
        if not build_version:
            raise ValueError("force_version.txt exists but is empty")
    else:
        ret = subprocess.run(
            ["git", "describe", "--tags", "--dirty"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )  # Uses any tags
        if ret.returncode == 0 and ret.stdout.strip():
            build_version = ret.stdout.strip()
        else:
            # Fallback for shallow/tagless CI checkouts
            ret = subprocess.run(
                ["git", "describe", "--always", "--dirty"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                check=True,
            )
            build_version = ret.stdout.strip()
    print("Firmware Revision: " + build_version)
    return build_version


# Keep Git metadata out of BUILD_FLAGS: global flag changes invalidate all
# ESP-IDF and dependency objects, even though only the app displays it.
output = Path(env.subst("$PROJECT_DIR")) / "include/system/firmware_version_gen.hpp"
content = ("#pragma once\n"
           "// Auto-generated; do not edit.\n"
           "inline constexpr char firmware_revision[] = " +
           json.dumps(get_firmware_version()) + ";\n")
if not output.exists() or output.read_text(encoding="utf-8") != content:
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(content, encoding="utf-8")
