from collections import deque
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys


MACHO_MAGICS = {bytes.fromhex(value) for value in (
    'feedface', 'cefaedfe', 'feedfacf', 'cffaedfe',
    'cafebabe', 'bebafeca', 'cafebabf', 'bfbafeca',
)}
LIB_COMMANDS = {
    'LC_LOAD_DYLIB', 'LC_LOAD_WEAK_DYLIB', 'LC_REEXPORT_DYLIB',
    'LC_LOAD_UPWARD_DYLIB', 'LC_LAZY_LOAD_DYLIB',
}
SYSTEM_PREFIXES = ('/usr/lib/', '/System/Library/')


def run(*command):
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode:
        sys.stderr.write(result.stderr or result.stdout)
        result.check_returncode()
    return result.stdout


def is_macho(path):
    if path.is_symlink() or not path.is_file():
        return False
    with path.open('rb') as stream:
        return stream.read(4) in MACHO_MAGICS


def load_commands(binary):
    output = run('/usr/bin/otool', '-l', binary)
    paths, dependencies, versions = set(), set(), set()
    for block in re.split(r'^Load command \d+\s*$', output, flags=re.M)[1:]:
        command = re.search(r'^\s*cmd (LC_\w+)$', block, re.M)
        if not command:
            continue
        name = command[1]
        if name == 'LC_RPATH':
            paths.add(re.search(r'^\s*path (.+) \(offset ', block, re.M)[1])
        elif name in LIB_COMMANDS:
            dependencies.add(re.search(r'^\s*name (.+) \(offset ', block, re.M)[1])
        elif name in {'LC_BUILD_VERSION', 'LC_VERSION_MIN_MACOSX'}:
            version = re.search(r'^\s*(?:minos|version) (\d+(?:\.\d+)*)$', block, re.M)
            if version:
                versions.add(tuple(map(int, version[1].split('.'))))
    return paths, dependencies, versions


def expand(name, binary, main):
    if name.startswith('@loader_path/'):
        return binary.parent / name[len('@loader_path/'):]
    if name.startswith('@executable_path/'):
        return main.parent / name[len('@executable_path/'):]
    return Path(name)


def dependency_exists(candidates, app):
    for candidate in candidates:
        if str(candidate).startswith(SYSTEM_PREFIXES):
            return True
        resolved = candidate.resolve()
        if resolved.is_relative_to(app) and resolved.is_file():
            return True
    return False


def embed_dependency(dependency, frameworks, brew_prefix):
    if dependency.startswith('@rpath/'):
        relative = Path(dependency[len('@rpath/'):])
        source = brew_prefix / 'lib' / relative
    elif dependency.startswith(str(brew_prefix) + '/'):
        source = Path(dependency)
        relative = Path(source.name)
        for parent in source.parents:
            if parent.suffix == '.framework':
                relative = Path(parent.name) / source.relative_to(parent)
                break
    else:
        return None, []

    if relative.is_absolute() or '..' in relative.parts:
        return None, []
    target = frameworks / relative
    if target.is_file() and is_macho(target.resolve()):
        return target, []
    source = source.resolve()
    if not source.is_relative_to(brew_prefix) or not is_macho(source):
        return None, []

    if relative.parts[0].endswith('.framework'):
        source_bundle = next(parent for parent in source.parents if parent.suffix == '.framework')
        target_bundle = frameworks / relative.parts[0]
        if target_bundle.exists():
            return None, []
        run('/usr/bin/ditto', '--norsrc', '--noextattr', source_bundle, target_bundle)
        copied = [path for path in target_bundle.rglob('*') if is_macho(path)]
    else:
        shutil.copyfile(source, target)
        target.chmod(0o755)
        copied = [target]
    if not target.is_file():
        return None, []
    install_name = '@executable_path/../Frameworks/' + relative.as_posix()
    run('/usr/bin/install_name_tool', '-id', install_name, target.resolve())
    return target, copied


def finalize(app, entitlements, brew_prefix):
    main = app / 'Contents/MacOS/kukagram'
    info = app / 'Contents/Info.plist'
    data = plistlib.loads(info.read_bytes())
    if data['CFBundleIdentifier'] != 'io.kukagram.desktop' or data['CFBundleExecutable'] != 'kukagram':
        raise SystemExit('Unexpected app identity')

    files = list(app.rglob('*'))
    for path in files:
        if path.is_symlink() and not path.resolve().is_relative_to(app):
            raise SystemExit(f'External symlink: {path.relative_to(app)}')
    binaries = [path for path in files if is_macho(path)]
    if main not in binaries:
        raise SystemExit('Main executable is not Mach-O')

    minimum_versions = set()
    frameworks = app / 'Contents/Frameworks'
    main_paths = {path for path in load_commands(main)[0] if not path.startswith('/')
                  or path.startswith(SYSTEM_PREFIXES)}
    pending = deque(binaries)
    while pending:
        binary = pending.popleft()
        paths, dependencies, versions = load_commands(binary)
        minimum_versions.update(versions)
        for path in sorted(paths):
            if path.startswith('/') and not path.startswith(SYSTEM_PREFIXES):
                run('/usr/bin/install_name_tool', '-delete_rpath', path, binary)
                paths.remove(path)
        roots = [expand(path, binary, main) for path in paths]
        roots.extend(expand(path, main, main) for path in main_paths)
        for dependency in sorted(dependencies):
            candidates = (
                [root / dependency[len('@rpath/'):] for root in roots]
                if dependency.startswith('@rpath/')
                else [expand(dependency, binary, main)]
            )
            if dependency_exists(candidates, app):
                continue
            target, copied = embed_dependency(dependency, frameworks, brew_prefix)
            if target is None:
                raise SystemExit(f'Unresolved dependency for {binary.relative_to(app)}: {dependency}')
            replacement = '@executable_path/../Frameworks/' + target.relative_to(frameworks).as_posix()
            run('/usr/bin/install_name_tool', '-change', dependency, replacement, binary)
            pending.extend(copied)
            binaries.extend(copied)

    if minimum_versions:
        data['LSMinimumSystemVersion'] = '.'.join(map(str, max(minimum_versions)))
    if revision := os.environ.get('KUKAGRAM_BUILD_REVISION'):
        data['KukagramBuildRevision'] = revision
    info.write_bytes(plistlib.dumps(data, sort_keys=False))

    for binary in binaries:
        if binary != main:
            run('/usr/bin/codesign', '--force', '--sign', '-', binary)
    nested = [path for path in app.rglob('*') if path.is_dir() and not path.is_symlink()
              and path.suffix in {'.app', '.framework', '.xpc', '.appex'}]
    for bundle in sorted(nested, key=lambda path: len(path.parts), reverse=True):
        run('/usr/bin/codesign', '--force', '--sign', '-', bundle)
    run('/usr/bin/codesign', '--force', '--sign', '-', '--entitlements', entitlements, app)
    print(f'Checked and signed {len(binaries)} Mach-O files')


if __name__ == '__main__':
    finalize(*(Path(argument).resolve() for argument in sys.argv[1:]))
