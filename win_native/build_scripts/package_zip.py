# SPDX-License-Identifier: MIT
"""Assemble the distributable zip of the Windows-native UxPlay build.

usage: python -I package_zip.py <build_dir> <source_dir> <msys2_clang64_dir> <deps_src_root> <output_zip>

Contents: stripped uxplay.exe (statically linked: it needs only Windows
system DLLs, so no DLL is bundled; the collector reports any that appear),
the Traditional Chinese user guide, licence texts of every statically linked
library and the complete corresponding source (LGPL relinking: rebuild from
it with win_native/build_scripts).
"""
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

# local tool state (.claude, .vs, .vscode) is not part of the corresponding source
SOURCE_EXCLUDES = {".git", ".claude", ".vs", ".vscode", "build", "dist", "__pycache__"}


def run(command):
    subprocess.run(command, check=True)


def add_tree(archive, root, prefix, excludes):
    for directory, subdirectories, files in os.walk(root):
        subdirectories[:] = [d for d in subdirectories if d not in excludes]
        for name in files:
            path = os.path.join(directory, name)
            relative = os.path.relpath(path, root)
            archive.write(path, os.path.join(prefix, relative))


def main():
    build_dir, source_dir, clang64, deps_src, output_zip = sys.argv[1:]
    objdump = os.path.join(clang64, "bin", "llvm-objdump.exe")
    strip = os.path.join(clang64, "bin", "llvm-strip.exe")
    staging = tempfile.mkdtemp(prefix="uxplay_package_")
    app = os.path.join(staging, "UxPlay")
    os.makedirs(os.path.join(app, "licenses"))

    exe = os.path.join(app, "uxplay.exe")
    shutil.copy2(os.path.join(build_dir, "uxplay.exe"), exe)
    run([strip, "--strip-all", exe])
    collector = os.path.join(os.path.dirname(os.path.abspath(__file__)), "collect_dlls.py")
    run([sys.executable, "-I", collector, objdump, exe, os.path.join(clang64, "bin"), app])

    shutil.copy2(os.path.join(source_dir, "win_native", "package", "user_guide_zh_tw.txt"), app)
    shutil.copy2(os.path.join(source_dir, "win_native", "package", "run_with_console_log.cmd"), app)
    shutil.copy2(os.path.join(source_dir, "win_native", "package", "install_apple_usb_support.cmd"), app)
    licenses = {
        "uxplay_gpl-3.0.txt": os.path.join(source_dir, "LICENSE"),
        "uxplay_native_engine_mit.txt": os.path.join(source_dir, "win_native", "LICENSE"),
        "ffmpeg_lgpl-2.1.txt": os.path.join(deps_src, "ffmpeg", "COPYING.LGPLv2.1"),
        "openssl_apache-2.0.txt": os.path.join(deps_src, "openssl", "LICENSE.txt"),
        "libplist_lgpl-2.1.txt": os.path.join(deps_src, "libplist", "COPYING.LESSER"),
        "winpthreads_mit.txt": os.path.join(clang64, "share", "licenses", "winpthreads", "COPYING"),
        "llvm_libcxx_apache-2.0-with-llvm-exception.txt": os.path.join(clang64, "share", "licenses", "libc++",
                                                                       "LICENSE"),
        "llhttp_mit.txt": os.path.join(source_dir, "lib", "llhttp", "LICENSE-MIT"),
        "playfair.txt": os.path.join(source_dir, "lib", "playfair", "LICENSE.md"),
    }
    missing = [path for path in licenses.values() if not os.path.exists(path)]
    if missing:
        shutil.rmtree(staging)
        sys.exit("licence text(s) not found, package not written:\n  " + "\n  ".join(missing))
    for target, path in licenses.items():
        shutil.copy2(path, os.path.join(app, "licenses", target))

    os.makedirs(os.path.dirname(os.path.abspath(output_zip)), exist_ok=True)
    with zipfile.ZipFile(output_zip, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        add_tree(archive, app, "UxPlay", set())
        add_tree(archive, source_dir, os.path.join("UxPlay", "source", "uxplay"), SOURCE_EXCLUDES)
    shutil.rmtree(staging)
    print(f"wrote {output_zip} ({os.path.getsize(output_zip) / 1e6:.1f} MB)")


if __name__ == "__main__":
    main()
