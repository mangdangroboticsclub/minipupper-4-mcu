#!/usr/bin/env python3
"""Pre-build sanity checks for the repo.

Every component ESP-Claw ships carries `path:` dependencies written for its
position inside the ESP-Claw tree. When one of those components is copied into
this repo it lands at a different depth, and a path that used to reach the repo
root now reaches somewhere on the filesystem outside it -- where it either does
not exist, or, worse, does.

That is a configure-time error with a confusing message (the component manager
reports the resolved path, not the manifest that produced it), so it is worth
catching here instead.

    python tools/check_paths.py
"""
import os
import re
import sys

try:
    import yaml
except ImportError:
    sys.exit("PyYAML is required: python -m pip install pyyaml")

ROOT = os.path.abspath(os.path.dirname(os.path.dirname(__file__)))
SKIP = {'third-party', 'build', 'managed_components', '.git', 'node_modules'}


def walk():
    for dirpath, dirnames, files in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames if d not in SKIP]
        yield dirpath, files


def main():
    problems = []

    for dirpath, files in walk():
        if 'idf_component.yml' in files:
            manifest = os.path.join(dirpath, 'idf_component.yml')
            try:
                data = yaml.safe_load(open(manifest, encoding='utf-8')) or {}
            except Exception as exc:                       # noqa: BLE001
                problems.append(f'{manifest}: cannot parse ({exc})')
                continue
            for name, spec in (data.get('dependencies') or {}).items():
                if not isinstance(spec, dict) or 'path' not in spec:
                    continue
                target = os.path.abspath(os.path.join(dirpath, spec['path']))
                where = os.path.relpath(manifest, ROOT)
                if not target.startswith(ROOT + os.sep):
                    problems.append(
                        f'{where}: dependency "{name}" escapes the repo -> {target}')
                elif not os.path.isdir(target):
                    problems.append(
                        f'{where}: dependency "{name}" -> {os.path.relpath(target, ROOT)} '
                        f'(does not exist)')

        for name in files:
            if not name.endswith(('.txt', '.cmake')):
                continue
            path = os.path.join(dirpath, name)
            text = open(path, encoding='utf-8', errors='ignore').read()
            for match in re.findall(r'"((?:\.\./)+[^"]*)"', text):
                target = os.path.abspath(os.path.join(dirpath, match))
                if not target.startswith(ROOT + os.sep):
                    problems.append(
                        f'{os.path.relpath(path, ROOT)}: relative path escapes '
                        f'the repo -> {match}')

    # Empty source files.
    #
    # This exists because it happened, twice: a file-normalising one-liner
    # written as open(p, "wb").write(open(p, "rb").read()) truncates the file
    # before the read runs, so every file it touches becomes zero bytes. An
    # empty .cc still compiles cleanly as an empty translation unit, so the
    # failure surfaces much later as "namespace has not been declared" in a
    # completely different file.
    for dirpath, files in walk():
        for name in files:
            if not name.endswith(('.c', '.cc', '.h', '.yaml', '.yml')):
                continue
            path = os.path.join(dirpath, name)
            if os.path.getsize(path) == 0:
                problems.append(f'{os.path.relpath(path, ROOT)}: file is empty')

    if problems:
        print('Path problems found:\n')
        for problem in problems:
            print(f'  {problem}')
        print(f'\n{len(problems)} problem(s).')
        return 1

    print('All component paths resolve inside the repo.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
