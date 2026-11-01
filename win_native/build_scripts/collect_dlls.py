"""Copy the non-system DLL closure of an executable next to it.

usage: python -I collect_dlls.py <objdump.exe> <exe> <search_dir> <dest_dir>
System DLLs (present in C:\\Windows\\System32 or api-ms-win-*) are skipped.
"""
import os
import shutil
import subprocess
import sys


def imports(objdump, path):
    output = subprocess.run([objdump, "-p", path], capture_output=True, text=True, check=True).stdout
    return [line.split("DLL Name:")[1].strip() for line in output.splitlines() if "DLL Name:" in line]


def main():
    objdump, exe, search_dir, dest_dir = sys.argv[1:]
    system32 = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32")
    pending = [exe]
    seen = set()
    copied = []
    while pending:
        current = pending.pop()
        for name in imports(objdump, current):
            key = name.lower()
            if key in seen:
                continue
            seen.add(key)
            if key.startswith("api-ms-win-") or key.startswith("ext-ms-") or os.path.exists(os.path.join(system32, name)):
                continue
            source = os.path.join(search_dir, name)
            if not os.path.exists(source):
                print(f"MISSING {name} (needed by {os.path.basename(current)})")
                continue
            destination = os.path.join(dest_dir, name)
            shutil.copy2(source, destination)
            copied.append(name)
            pending.append(source)
    for name in sorted(copied):
        print(f"copied {name}")


if __name__ == "__main__":
    main()
