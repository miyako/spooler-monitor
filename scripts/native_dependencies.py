"""Audit directly imported system libraries on each native build host."""
from pathlib import Path
import re
import subprocess


def validate_windows_imports(imports):
    if not isinstance(imports, list) or not imports or any(not isinstance(name, str) for name in imports):
        raise ValueError("unreviewed or missing Windows native import inventory")
    system = {"kernel32.dll", "kernelbase.dll", "ntdll.dll", "advapi32.dll",
              "user32.dll", "winspool.drv", "msvcrt.dll"}
    unexpected = [name for name in imports if name.lower() not in system
                  and not name.lower().startswith(("api-ms-win-", "ext-ms-win-"))]
    if unexpected:
        raise ValueError("unreviewed or missing Windows native import inventory: " + repr(unexpected))
    return imports


def inspect(executable, target, compiler):
    if target.startswith("macos-"):
        result = subprocess.run(["otool", "-L", str(executable)], check=True,
                                capture_output=True, text=True, timeout=30)
        imports = [line.strip().split(" (", 1)[0] for line in result.stdout.splitlines()
                   if " (compatibility version " in line]
        if Path(executable).suffix == ".dylib":
            identity = subprocess.run(["otool", "-D", str(executable)], check=True,
                                      capture_output=True, text=True, timeout=30)
            names = identity.stdout.splitlines()[1:]
            if len(names) != 1 or not names[0].strip():
                raise ValueError("missing dylib install-name identity")
            # otool -L includes LC_ID_DYLIB as well as actual imports.
            imports = [name for name in imports if name != names[0].strip()]
        if not imports or any(not name.startswith(("/usr/lib/", "/System/Library/")) for name in imports):
            raise ValueError("unbundled non-system macOS native dependency: " + repr(imports))
        return imports
    dumpbin = Path(compiler["CMAKE_CXX_COMPILER"]).with_name("dumpbin.exe")
    if not dumpbin.is_file():
        raise ValueError("native Windows dependency audit requires the configured MSVC dumpbin.exe")
    result = subprocess.run([str(dumpbin), "/DEPENDENTS", str(executable)], check=True,
                            capture_output=True, text=True, timeout=30)
    imports = re.findall(r"^\s+([A-Za-z0-9_.-]+\.(?:dll|drv))\s*$", result.stdout,
                         flags=re.MULTILINE | re.IGNORECASE)
    return validate_windows_imports(imports)
