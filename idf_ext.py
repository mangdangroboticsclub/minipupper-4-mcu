"""Project level idf.py extension: keep the embedded pwa-claw web app in sync
*before* the build system is started.

Why this exists
---------------
Every file in ``pwa-claw/www/`` is embedded into the firmware as binary data
(``target_add_binary_data`` in ``main/CMakeLists.txt``).  The build is driven by
file timestamps, and ``npm run build`` rewrites all of ``www/``, so a web build
regenerates ~5 MB of embedded assembler sources and relinks the application
(~1m40s on this project).

Ninja decides what is out of date once, when it starts.  A web build that runs
*inside* the build graph therefore cannot trigger the relink for the very build
it runs in -- the relink would only happen on the next invocation, which means
the firmware you flash embeds the previous web app.

Running the incremental check here, before ninja is started, gives us both:

* nothing happens (~20 ms) when the web app did not change, so ``idf.py build``
  and ``idf.py flash`` stay quick;
* when it did change, ninja sees the fresh timestamps at startup and rebuilds
  the embedded assets plus the application in the same run, so stale web assets
  are never flashed.

The same check is also wired into CMake (target ``mpx_build_pwa``) for people
who build with plain ``cmake --build build`` / ``ninja``, where this extension
does not run.  See DEV_CONTAINER.md section 4.
"""

import os
import shutil
import subprocess

try:
    from idf_py_actions.errors import FatalError
except ImportError:  # pragma: no cover - keep the extension importable on its own
    class FatalError(RuntimeError):
        pass

# Tasks that (transitively) build the firmware application.
PWA_ACTIONS = ('all', 'build', 'app', 'flash', 'app-flash')

PWA_SCRIPT = os.path.join('tools', 'pwa_build.cmake')
CACHE_VAR = 'MPX_BUILD_PWA'


def _pwa_disabled(global_args, build_dir):
    """Honour -DMPX_BUILD_PWA=OFF (command line or CMake cache)."""
    # An explicit -D on the command line wins over the cached value.
    for entry in getattr(global_args, 'define_cache_entry', []) or []:
        key, _, value = entry.partition('=')
        if key == CACHE_VAR:
            return value.strip().upper() in ('OFF', '0', 'FALSE', 'NO')

    try:
        with open(os.path.join(build_dir, 'CMakeCache.txt'), encoding='utf-8', errors='replace') as cache:
            for line in cache:
                if line.startswith(CACHE_VAR + ':BOOL='):
                    return line.split('=', 1)[1].strip().upper() == 'OFF'
    except OSError:
        pass

    return False


def action_extensions(base_actions, project_path):
    project_dir = project_path

    def host_test_callback(action, ctx, global_args, **kwargs):
        """`idf.py host-test` -- build and run tools/host_tests.

        The identity tests (robot ID format, key derivation) do not need a
        board or even ESP-IDF, so they run as an ordinary CMake project with
        its own build directory. CI runs the same commands directly; this is
        the discoverable local shortcut.
        """
        source_dir = os.path.join(project_dir, 'tools', 'host_tests')
        if not os.path.isfile(os.path.join(source_dir, 'CMakeLists.txt')):
            raise FatalError('host tests: {} not found'.format(source_dir))

        build_dir = os.path.join(
            getattr(global_args, 'build_dir', None) or os.path.join(project_dir, 'build'),
            'host_tests',
        )
        cmake = shutil.which('cmake') or 'cmake'

        for command in (
            [cmake, '-S', source_dir, '-B', build_dir],
            [cmake, '--build', build_dir],
            [cmake, '-E', 'chdir', build_dir, shutil.which('ctest') or 'ctest',
             '--output-on-failure'],
        ):
            print('host-test:', ' '.join(command), flush=True)
            try:
                result = subprocess.run(command, cwd=project_dir)
            except OSError as error:
                raise FatalError('host tests: could not run cmake: {}'.format(error))
            if result.returncode != 0:
                raise FatalError('host tests: failed ({}).'.format(' '.join(command)))

    # In a dict, the key is the command name; an extra 'names' key would be
    # passed straight to click and rejected.
    host_test_action = {
        'help': 'Build and run the host unit tests in tools/host_tests.',
        'callback': host_test_callback,
    }

    def pwa_sync_callback(ctx, global_args, tasks):
        # Nothing to do for 'idf.py -n ...', menuconfig, monitor, clean, ...
        if getattr(global_args, 'dry_run', False):
            return
        if not any(getattr(task, 'name', '') in PWA_ACTIONS for task in tasks):
            return

        script = os.path.join(project_dir, PWA_SCRIPT)
        if not os.path.isfile(script):
            return

        build_dir = getattr(global_args, 'build_dir', None) or os.path.join(project_dir, 'build')

        if _pwa_disabled(global_args, build_dir):
            return

        npm = shutil.which('npm')
        if not npm:
            print('pwa-claw: npm not found in PATH, embedding www/ as it is.')
            return

        cmake = shutil.which('cmake') or 'cmake'
        command = [
            cmake,
            '-DMPX_PWA_DIR=' + os.path.join(project_dir, 'pwa-claw'),
            '-DMPX_NPM_EXECUTABLE=' + npm,
            '-DMPX_PWA_STAMP=' + os.path.join(build_dir, 'mpx_pwa.stamp'),
            # The CMake target repeats this check inside ninja and prints the
            # "nothing to do" line; keep the pre-build run quiet when idle.
            '-DMPX_PWA_QUIET=ON',
            '-P',
            script,
        ]

        try:
            result = subprocess.run(command, cwd=project_dir)
        except OSError as error:
            raise FatalError('pwa-claw: could not run the web app build: {}'.format(error))

        if result.returncode != 0:
            raise FatalError('pwa-claw: web app build failed, aborting.')

    return {
        'actions': {'host-test': host_test_action},
        'global_action_callbacks': [pwa_sync_callback],
    }
