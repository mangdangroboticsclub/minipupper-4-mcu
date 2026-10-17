#!/usr/bin/env python3
"""Find every '/*' that sits inside an open block comment.

GCC builds this project with -Werror=comment, so one of these is a hard build
failure -- and the message points at the comment, not at the code, which makes
it read like a spurious warning. It has cost this repo three separate build
rounds. Run it before committing:

    python tools/check_comments.py

Exit status is the number of offences. Skips build/, third-party/ and
managed_components/: nothing there is ours to fix.
"""
import os
import sys

SKIP = {'build', 'third-party', 'managed_components', '.git', 'node_modules'}
EXTS = ('.c', '.h', '.cc', '.cpp', '.hpp')


def scan(path):
    """Walk the file as a character stream, tracking lexical state."""
    text = open(path, encoding='utf-8', errors='replace').read()
    hits, i, n = [], 0, len(text)
    state = 'code'          # code | block | line | str | chr
    while i < n:
        two = text[i:i + 2]
        if state == 'code':
            if two == '/*':
                state, i = 'block', i + 2
                continue
            if two == '//':
                state, i = 'line', i + 2
                continue
            if text[i] == '"':
                state = 'str'
            elif text[i] == "'":
                state = 'chr'
        elif state == 'block':
            if two == '/*':
                hits.append(text.count('\n', 0, i) + 1)
                i += 2
                continue
            if two == '*/':
                state, i = 'code', i + 2
                continue
        elif state == 'line':
            if text[i] == '\n':
                state = 'code'
        elif state in ('str', 'chr'):
            if text[i] == '\\':
                i += 2
                continue
            if text[i] == ('"' if state == 'str' else "'"):
                state = 'code'
        i += 1
    return hits


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    total = 0
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP]
        for name in filenames:
            if not name.endswith(EXTS):
                continue
            full = os.path.join(dirpath, name)
            for line in scan(full):
                rel = os.path.relpath(full, root).replace(os.sep, '/')
                print('%s:%d: "/*" inside a block comment '
                      '(-Werror=comment)' % (rel, line))
                total += 1
    print('%d offence(s)' % total)
    return total


if __name__ == '__main__':
    sys.exit(1 if main() else 0)
