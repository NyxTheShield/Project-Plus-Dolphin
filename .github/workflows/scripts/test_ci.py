"""CI configuration regression checks. Requires PyYAML; PowerShell tests use pwsh.

These check workflow contracts/command construction, not a hosted MSVC or macOS
build. Run actionlint separately for GitHub expression and workflow validation.
"""
import base64
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
import unittest

import yaml


ROOT = Path(__file__).resolve().parents[3]


def load(relative):
    return yaml.load((ROOT / relative).read_text(), Loader=yaml.BaseLoader)


CI = load('.github/workflows/ci.yml')
DEPENDENCIES = load('.github/workflows/dependencies.yml')
BUNDLE = load('.github/actions/dependencies/action.yml')
NATIVE_JOBS = ('build_windows_x86_64', 'build_macos', 'build_appimage')


def step(job, name):
    return next(s for s in job['steps'] if s.get('name') == name)


class WorkflowTests(unittest.TestCase):
    def test_native_jobs_build_the_workflow_checkout(self):
        for name in NATIVE_JOBS:
            checkout = CI['jobs'][name]['steps'][0]
            self.assertTrue(checkout['uses'].startswith('actions/checkout@'))
            self.assertNotIn('repository', checkout['with'])
            self.assertNotIn('ref', checkout['with'])
            self.assertEqual(checkout['with']['persist-credentials'], 'false')

    def test_compiler_cache_restore_save_and_failure_contracts(self):
        for name in NATIVE_JOBS:
            with self.subTest(job=name):
                job = CI['jobs'][name]
                restore = step(job, 'Restore compiled objects')
                save = step(job, 'Save compiled objects')
                stats = step(job, 'Show compiler cache statistics')
                build = next(s for s in job['steps'] if s.get('id') == 'build')
                self.assertIn('cmake --build', build['run'])
                self.assertNotIn('cmake -S', build['run'])
                key = restore['with']['key']
                self.assertIn('${{ github.sha }}', key)
                self.assertIn('${{ github.run_attempt }}', key)
                self.assertNotIn('github.run_id', key)
                self.assertTrue(key.startswith(restore['with']['restore-keys']))
                self.assertEqual(restore['with']['path'], save['with']['path'])
                self.assertEqual(save['with']['key'],
                                 '${{ steps.' + restore['id'] + '.outputs.cache-primary-key }}')
                self.assertIn('steps.' + restore['id'] + ".outputs.cache-hit != 'true'", save['if'])
                self.assertIn('steps.' + restore['id'] + ".outcome == 'success'", save['if'])
                for guarded in (save, stats):
                    self.assertIn('!cancelled()', guarded['if'])
                    self.assertIn("steps.build.outcome == 'success'", guarded['if'])
                    self.assertIn("steps.build.outcome == 'failure'", guarded['if'])
                self.assertIn('GITHUB_STEP_SUMMARY', stats['run'])

    def test_windows_ninja_uses_msvc_and_compiler_launchers(self):
        job = CI['jobs']['build_windows_x86_64']
        configure = step(job, 'Configure CMake')['run']
        for flag in ('-G Ninja', '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl',
                     '-DCMAKE_C_COMPILER_LAUNCHER=sccache',
                     '-DCMAKE_CXX_COMPILER_LAUNCHER=sccache', '-DCMAKE_BUILD_TYPE=Release'):
            self.assertIn(flag, configure)
        self.assertNotIn('-A x64', configure)
        self.assertEqual(job['env']['SCCACHE_CACHE_SIZE'], '2G')
        self.assertEqual(step(job, 'Upload Build Artifact')['with']['path'], 'Binary/x64/')
        self.assertIn('/Z7', (ROOT / 'CMake/FlagsOverride.cmake').read_text())

    def test_dependency_warmer_uses_same_windows_runner_and_setup(self):
        job = CI['jobs']['build_windows_x86_64']
        warmer = DEPENDENCIES['jobs']['dependencies']
        self.assertIn(job['runs-on'], warmer['strategy']['matrix']['runner'])
        for key in ('uses', 'with'):
            self.assertEqual(step(job, 'Setup MSVC for Ninja')[key],
                             step(warmer, 'Setup MSVC for Ninja')[key])
        self.assertEqual(step(warmer, 'Setup MSVC for Ninja')['if'], "runner.os == 'Windows'")
        commands = [s['run'] for s in BUNDLE['runs']['steps'] if 'run' in s]
        self.assertEqual(sum('Windows) args+=(-G Ninja -DCMAKE_C_COMPILER=cl '
                             '-DCMAKE_CXX_COMPILER=cl)' in command for command in commands), 2)

    def test_binary_dependency_cache_has_exact_toolchain_and_recipe_key(self):
        restore = step(BUNDLE['runs'], 'Restore prebuilt libraries')['with']
        self.assertNotIn('restore-keys', restore)
        for fingerprint in ('runner.os', 'runner.arch', 'steps.configure.outputs.image',
                            'CMakeCache.txt', 'bundle-toolchain.txt',
                            '.github/actions/dependencies/action.yml', 'CMake/DependencyBundle/**'):
            self.assertIn(fingerprint, restore['key'])

    def test_existing_release_optimization_and_qt_binary_cache_are_retained(self):
        mac = CI['jobs']['build_macos']
        self.assertIn('-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON',
                      step(mac, 'Generate CMake Files')['run'])
        for name in ('build_macos', 'build_appimage'):
            self.assertEqual(CI['jobs'][name]['env']['CCACHE_MAXSIZE'], '2G')
            self.assertEqual(step(CI['jobs'][name], 'Install cached Qt binaries')['with']['cache'], 'true')

    def test_macos_has_native_intel_and_apple_silicon_jobs(self):
        mac = CI['jobs']['build_macos']
        self.assertEqual(mac['strategy']['fail-fast'], 'false')
        self.assertEqual(mac['runs-on'], '${{ matrix.runner }}')
        self.assertEqual(mac['env']['MACOS_ARCH'], '${{ matrix.arch }}')
        self.assertEqual(mac['strategy']['matrix']['include'], [
            {'runner': 'macos-15-intel', 'arch': 'x86_64'},
            {'runner': 'macos-15', 'arch': 'arm64'},
        ])
        warmer = DEPENDENCIES['jobs']['dependencies']['strategy']['matrix']['runner']
        for entry in mac['strategy']['matrix']['include']:
            self.assertIn(entry['runner'], warmer)
        self.assertIn('$MACOS_ARCH', step(mac, 'Prepare Build Artifacts')['run'])

    def test_macos_caches_are_separate_and_preserve_intel_keys(self):
        mac = CI['jobs']['build_macos']
        for name, field in (('Install cached Qt binaries', 'cache-key-prefix'),
                            ('Cache MoltenVK', 'key'), ('Restore compiled objects', 'key'),
                            ('Restore compiled objects', 'restore-keys')):
            with self.subTest(step=name, field=field):
                key = step(mac, name)['with'][field]
                self.assertTrue(key.startswith('${{ matrix.runner }}-'))
                intel = key.replace('${{ matrix.runner }}', 'macos-15-intel')
                arm = key.replace('${{ matrix.runner }}', 'macos-15')
                self.assertNotEqual(intel, arm)
                self.assertTrue(intel.startswith(('macos-15-intel-qt-v1',
                                                 'macos-15-intel-moltenvk-v2-',
                                                 'macos-15-intel-ccache-v2-')))

    def test_macos_signing_and_verification_precede_upload(self):
        mac = CI['jobs']['build_macos']
        configure = step(mac, 'Generate CMake Files')['run']
        self.assertIn('-DMACOS_CODE_SIGNING=ON', configure)
        self.assertIn('-DMACOS_CODE_SIGNING_IDENTITY=-', configure)
        entitlements = plistlib.loads(
            (ROOT / 'Source/Core/DolphinQt/DolphinEmu.entitlements').read_bytes())
        self.assertTrue(entitlements['com.apple.security.cs.allow-jit'])
        names = [s['name'] for s in mac['steps']]
        self.assertLess(names.index('Verify Qt architecture'), names.index('Generate CMake Files'))
        self.assertLess(names.index('Verify MoltenVK architecture'), names.index('Save MoltenVK'))
        self.assertLess(names.index('Verify macOS bundle'), names.index('Prepare Build Artifacts'))
        self.assertLess(names.index('Prepare Build Artifacts'), names.index('Upload Artifact'))
        for name in ('Verify Qt architecture', 'Verify MoltenVK architecture'):
            command = step(mac, name)['run']
            self.assertTrue(command.startswith('lipo "'))
            self.assertTrue(command.endswith('-verify_arch "$MACOS_ARCH"'))

    def test_bash_step_syntax(self):
        jobs = [CI['jobs']['build_macos'], CI['jobs']['build_appimage'], BUNDLE['runs']]
        for job in jobs:
            for item in job['steps']:
                if 'run' in item:
                    with self.subTest(step=item['name']):
                        result = subprocess.run(['bash', '-n'], input=item['run'], text=True,
                                                capture_output=True)
                        self.assertEqual(result.returncode, 0, result.stderr)


class MacOSCommandTests(unittest.TestCase):
    def run_bash(self, script, **extra_env):
        return subprocess.run(['bash', '-euo', 'pipefail', '-c', script], text=True,
                              capture_output=True, env={**os.environ, **extra_env})

    @unittest.skipUnless(shutil.which('clang') and (shutil.which('lipo') or shutil.which('llvm-lipo')),
                         'Clang and lipo/llvm-lipo are required for Mach-O checks')
    def test_qt_architecture_check_with_real_macho_files(self):
        # Compile SDK-independent objects; no macOS host or Qt download is needed.
        lipo = shutil.which('lipo') or shutil.which('llvm-lipo')
        command = step(CI['jobs']['build_macos'], 'Verify Qt architecture')['run']
        with tempfile.TemporaryDirectory(prefix='dolphin-ci-macho-') as directory:
            qt_prefix = Path(directory) / 'Qt prefix with spaces'
            library = qt_prefix / 'lib/QtCore.framework/QtCore'
            library.parent.mkdir(parents=True)
            objects = {}
            for arch in ('x86_64', 'arm64'):
                objects[arch] = Path(directory) / (arch + '.o')
                result = subprocess.run(
                    ['clang', '-target', arch + '-apple-macos11', '-x', 'c', '-c',
                     '-o', str(objects[arch]), '-'], input='int ci_architecture_probe;\n',
                    text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([lipo, '-create', *(str(p) for p in objects.values()),
                                     '-output', str(library)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for arch in objects:
                with self.subTest(universal=True, arch=arch):
                    result = self.run_bash('lipo() { "$CI_LIPO" "$@"; }\n' + command,
                                           CI_LIPO=lipo, QT_ROOT_DIR=str(qt_prefix), MACOS_ARCH=arch)
                    self.assertEqual(result.returncode, 0, result.stderr)
            for present, missing in (('x86_64', 'arm64'), ('arm64', 'x86_64')):
                with self.subTest(universal=False, missing=missing):
                    shutil.copyfile(objects[present], library)
                    result = self.run_bash('lipo() { "$CI_LIPO" "$@"; }\n' + command,
                                           CI_LIPO=lipo, QT_ROOT_DIR=str(qt_prefix), MACOS_ARCH=missing)
                    self.assertNotEqual(result.returncode, 0)

    def test_runner_guard_rejects_rosetta_or_wrong_runner(self):
        command = step(CI['jobs']['build_macos'], 'Verify native runner')['run']
        for expected in ('arm64', 'x86_64'):
            for actual in ('arm64', 'x86_64'):
                with self.subTest(expected=expected, actual=actual):
                    result = self.run_bash('uname() { echo "$TEST_HOST_ARCH"; }\n' + command,
                                           MACOS_ARCH=expected, TEST_HOST_ARCH=actual)
                    self.assertEqual(result.returncode, int(expected != actual), result.stderr)

    def test_application_and_dependencies_use_matching_native_architectures(self):
        commands = [step(CI['jobs']['build_macos'], 'Generate CMake Files')['run'],
                    step(BUNDLE['runs'], 'Configure dependency bundle and record toolchain')['run'],
                    step(BUNDLE['runs'], 'Verify installed dependency libraries')['run']]
        mocks = '''
            uname() { echo "$MACOS_ARCH"; }
            cmake() { printf '%s\\n' "$@"; }
            ctest() { :; }
        '''
        for arch in ('arm64', 'x86_64'):
            for index, command in enumerate(commands):
                with self.subTest(arch=arch, command=index):
                    result = self.run_bash(mocks + command, MACOS_ARCH=arch, RUNNER_OS='macOS',
                                           QT_ROOT_DIR='/qt prefix', DEPENDENCY_PREFIX='/deps prefix',
                                           GITHUB_WORKSPACE='/ci workspace', GITHUB_OUTPUT=os.devnull)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    args = result.stdout.splitlines()
                    self.assertIn('-DCMAKE_OSX_ARCHITECTURES=' + arch, args)
                    self.assertIn('-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0', args)
                    self.assertIn('-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON', args)
                    if index == 0:
                        self.assertIn('-DDOLPHIN_DEPENDENCY_PREFIX=/deps prefix', args)
                    if index == 2:
                        self.assertIn('-DDOLPHIN_DEPENDENCY_PREFIX=/ci workspace/build-dependency-prefix',
                                      args)

    def test_bundle_checks_reject_wrong_architecture_signature_or_startup(self):
        command = step(CI['jobs']['build_macos'], 'Verify macOS bundle')['run']
        mocks = '''
            lipo() {
                if [ "$1" = -archs ]; then
                    echo "$TEST_MAIN_ARCH"
                else
                    test "$2" = -verify_arch
                    test "$3" = "$MACOS_ARCH"
                    if [[ "$1" == *QtCore* ]]; then
                        [[ " $TEST_LIBRARY_ARCHS " == *" $3 "* ]] || return 41
                    fi
                fi
            }
            find() { printf '%s\\0' "$1/Contents/MacOS/DolphinQt" \
                "$1/Contents/Frameworks/QtCore.framework/QtCore" "$1/Contents/Resources/icon.png"; }
            file() { if [[ "$2" == *.png ]]; then echo PNG; else echo Mach-O; fi; }
            codesign() { echo SIGNATURE; return "$TEST_SIGN_EXIT"; }
            function build/Binaries/DolphinQt.app/Contents/MacOS/DolphinQt() {
                test "$1" = --version
                echo STARTUP
                return "$TEST_STARTUP_EXIT"
            }
        '''
        for arch in ('arm64', 'x86_64'):
            wrong = 'x86_64' if arch == 'arm64' else 'arm64'
            cases = ((arch, 'arm64 x86_64', 0, 0, 0), (wrong, arch, 0, 0, 1),
                     (arch, wrong, 0, 0, 41), (arch, arch, 42, 0, 42),
                     (arch, arch, 0, 43, 43))
            for main, libraries, sign, startup, expected in cases:
                with self.subTest(arch=arch, main=main, libraries=libraries, exit=expected):
                    result = self.run_bash(mocks + command, MACOS_ARCH=arch, TEST_MAIN_ARCH=main,
                                           TEST_LIBRARY_ARCHS=libraries, TEST_SIGN_EXIT=str(sign),
                                           TEST_STARTUP_EXIT=str(startup))
                    self.assertEqual(result.returncode, expected, result.stderr)
                    if expected in (1, 41, 42):
                        self.assertNotIn('STARTUP', result.stdout)

    def test_bundle_startup_does_not_inherit_sdk_plugin_paths(self):
        command = step(CI['jobs']['build_macos'], 'Verify macOS bundle')['run']
        plugin_paths = ('QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH',
                        'QML2_IMPORT_PATH', 'QML_IMPORT_PATH')
        mocks = '''
            lipo() { if [ "$1" = -archs ]; then echo "$MACOS_ARCH"; fi; }
            find() { :; }
            codesign() { :; }
            function build/Binaries/DolphinQt.app/Contents/MacOS/DolphinQt() {
                test "$1" = --version
                for qt_variable in QT_PLUGIN_PATH QT_QPA_PLATFORM_PLUGIN_PATH \
                                   QML2_IMPORT_PATH QML_IMPORT_PATH; do
                    if printenv "$qt_variable" >/dev/null; then
                        echo "$qt_variable leaked into the packaged app" >&2
                        return 44
                    fi
                done
                # The startup test still uses the normal environment otherwise.
                test "$QT_ROOT_DIR" = '/Qt SDK/macos'
                command -v bash >/dev/null
                echo STARTUP
            }
        '''
        for arch in ('arm64', 'x86_64'):
            for value in (None, '', '/Qt SDK/macos/plugins:/another SDK/plugins'):
                with self.subTest(arch=arch, plugin_path=value):
                    # Remove any paths from this test process before simulating CI.
                    setup = 'unset ' + ' '.join(plugin_paths) + '\n'
                    if value is not None:
                        setup += ''.join(f'export {name}="$TEST_SDK_PATH"\n'
                                         for name in plugin_paths)
                    # Clearing paths must be scoped to the app, not later steps.
                    parent_check = ''
                    for name in plugin_paths:
                        if value is None:
                            parent_check += f'test -z "${{{name}+set}}"\n'
                        else:
                            parent_check += f'test "${{{name}}}" = "$TEST_SDK_PATH"\n'
                    result = self.run_bash(setup + mocks + command + '\n' + parent_check,
                                           MACOS_ARCH=arch, QT_ROOT_DIR='/Qt SDK/macos',
                                           TEST_SDK_PATH=value or '')
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn('STARTUP', result.stdout)


@unittest.skipUnless(shutil.which('pwsh'), 'PowerShell is not installed')
class WindowsCommandTests(unittest.TestCase):
    def run_powershell(self, script, **extra_env):
        return subprocess.run(['pwsh', '-NoProfile', '-NonInteractive', '-Command', script],
                              text=True, capture_output=True, env={**os.environ, **extra_env})

    def test_all_windows_steps_parse(self):
        for item in CI['jobs']['build_windows_x86_64']['steps']:
            if 'run' not in item:
                continue
            with self.subTest(step=item['name']):
                result = self.run_powershell('''
                    $source = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($env:CI_SOURCE))
                    $tokens = $null
                    $errors = $null
                    $null = [Management.Automation.Language.Parser]::ParseInput($source, [ref]$tokens, [ref]$errors)
                    if ($errors.Count) { $errors | Out-String | Write-Error; exit 1 }
                ''', CI_SOURCE=base64.b64encode(item['run'].encode()).decode())
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_configure_preserves_dependency_path_as_one_argument(self):
        command = step(CI['jobs']['build_windows_x86_64'], 'Configure CMake')['run']
        result = self.run_powershell('function cmake { ConvertTo-Json -InputObject @($args) -Compress }\n' +
                                     command, DEPENDENCY_PREFIX='C:/CI workspace/dependency prefix')
        self.assertEqual(result.returncode, 0, result.stderr)
        args = json.loads(result.stdout)
        self.assertIn('-DDOLPHIN_DEPENDENCY_PREFIX=C:/CI workspace/dependency prefix', args)
        self.assertEqual(args[args.index('-G') + 1], 'Ninja')

    def test_build_propagates_cache_start_and_compiler_failure(self):
        command = step(CI['jobs']['build_windows_x86_64'], 'Build')['run']
        for cache_exit, build_exit, expected in ((0, 0, 0), (0, 23, 23), (17, 0, 17)):
            with self.subTest(cache_exit=cache_exit, build_exit=build_exit):
                result = self.run_powershell(f'''
                    function sccache {{ $global:LASTEXITCODE = {cache_exit} }}
                    function cmake {{ $global:LASTEXITCODE = {build_exit} }}
                ''' + command + '\nexit $LASTEXITCODE', NUMBER_OF_PROCESSORS='4')
                self.assertEqual(result.returncode, expected, result.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
