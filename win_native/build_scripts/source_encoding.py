"""Owned source files as UTF-8 with BOM and CRLF line endings.

usage: python -I source_encoding.py check|convert <file>...
       python -I source_encoding.py copy <source> <target>   (converted copy)

check   prints every file that is not valid UTF-8 + BOM + CRLF-only and exits 1
        when there is one.
convert rewrites such files in place (content unchanged apart from the BOM
        and the line endings); invalid UTF-8 is an error and nothing is written.
"""
import sys

BOM = b"\xef\xbb\xbf"


def converted(raw, path):
    body = raw[len(BOM):] if raw.startswith(BOM) else raw
    try:
        text = body.decode("utf-8")
    except UnicodeDecodeError as error:
        sys.exit(f"{path}: not valid UTF-8 ({error})")
    text = text.replace("\r\n", "\n").replace("\r", "\n").replace("\n", "\r\n")
    return BOM + text.encode("utf-8")


def main():
    action = sys.argv[1]
    if action == "copy":
        source, target = sys.argv[2:4]
        with open(source, "rb") as handle:
            raw = handle.read()
        with open(target, "wb") as handle:
            handle.write(converted(raw, source))
        return
    wrong = 0
    changed = 0
    for path in sys.argv[2:]:
        with open(path, "rb") as handle:
            raw = handle.read()
        wanted = converted(raw, path)
        if wanted == raw:
            continue
        if action == "check":
            wrong += 1
            print(f"not UTF-8+BOM+CRLF: {path}")
        else:
            with open(path, "wb") as handle:
                handle.write(wanted)
            changed += 1
    if action == "check":
        print(f"{len(sys.argv) - 2} files checked, {wrong} to convert")
        sys.exit(1 if wrong else 0)
    print(f"{len(sys.argv) - 2} files, {changed} converted")


if __name__ == "__main__":
    main()
