"""Runs Flashback's PC tests (see README.md) against the recorder's sources in
sysmodules/rosalina/source, and says which passed. Exits with 1 if any failed.

usage: python run_tests.py [--cc COMPILER] [--skip-e2e] [--quick]
  --cc        a C compiler for this PC (default: gcc); on Windows, a MinGW gcc
  --skip-e2e  leave out the end-to-end test (it needs Windows, and a PC that
              isn't busy: it runs in real time)
  --quick     fewer rows and conversions for the assembly checks
"""
import argparse
import os
import platform
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.normpath(os.path.join(HERE, '..', '..', 'sysmodules', 'rosalina', 'source'))
BUILD = os.path.join(HERE, 'build')
EXE = '.exe' if platform.system() == 'Windows' else ''


def run(cmd, cwd=None, quiet=False):
    """Runs a command; returns (ok, output)."""
    p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if not quiet and p.stdout:
        print(p.stdout.rstrip())
    return p.returncode == 0, p.stdout


def compile_c(cc, out, sources, flags=(), libs=()):
    ok, text = run([cc, '-O2', *flags, '-o', os.path.join(BUILD, out + EXE), *sources, *libs], quiet=True)
    if not ok:
        print(text.rstrip() or f'{cc} failed, with no output')
    return ok


def extract_drawlines():
    """flashbackDrawLines and flashbackBlankRows (the Flashback screen's redraw),
    for screentest.c: from FLASHBACK_SCREEN_COLS up to FlashbackMenu_Show."""
    for name in sorted(os.listdir(SOURCE)):
        if name.startswith('flashback') and name.endswith('.c'):
            s = open(os.path.join(SOURCE, name), encoding='utf-8').read().replace('\r\n', '\n')
            if '#define FLASHBACK_SCREEN_COLS' in s:
                a = s.index('#define FLASHBACK_SCREEN_COLS')
                b = s.index('void FlashbackMenu_Show(void)', a)
                open(os.path.join(BUILD, 'drawlines_extracted.c'), 'w', encoding='utf-8').write(s[a:b])
                return True
    print('FLASHBACK_SCREEN_COLS not found in the sources')
    return False


def main():
    ap = argparse.ArgumentParser(description='Runs Flashback\'s PC tests.')
    ap.add_argument('--cc', default='gcc')
    ap.add_argument('--skip-e2e', action='store_true')
    ap.add_argument('--quick', action='store_true')
    args = ap.parse_args()
    cc = shutil.which(args.cc)
    if cc is None:
        sys.exit(f'No C compiler "{args.cc}" found: pass one with --cc (see README.md).')
    # (A MinGW gcc needs its own folder on the PATH to find its DLLs, and so
    # do the programs it builds.)
    os.environ['PATH'] = os.path.dirname(cc) + os.pathsep + os.environ.get('PATH', '')
    args.cc = cc
    os.makedirs(os.path.join(BUILD, 'out'), exist_ok=True)

    results = []

    def step(name, fn):
        print(f'\n=== {name}')
        t0 = time.time()
        ok = fn()
        print(f'--- {name}: {"passed" if ok else "FAILED"} ({time.time() - t0:.0f} s)')
        results.append((name, ok))
        return ok

    py = sys.executable
    inc = ['-I', BUILD, '-I', HERE]
    built = step('extract the recorder code', lambda: run([py, '-I', os.path.join(HERE, 'extract.py'), SOURCE,
                                                           os.path.join(BUILD, 'recorder_extracted.c')])[0]
                 and extract_drawlines())
    if built:
        step('fuzz test', lambda: compile_c(args.cc, 'fuzz', [os.path.join(HERE, 'fuzz.c'), os.path.join(HERE, 'decode.c')], inc)
             and run([os.path.join(BUILD, 'fuzz' + EXE)], cwd=BUILD)[0])
        if args.skip_e2e:
            print('\n(end-to-end test left out)')
        elif platform.system() != 'Windows':
            print('\n(end-to-end test left out: it needs Windows)')
        else:
            step('end-to-end test', lambda: compile_c(args.cc, 'e2e', [os.path.join(HERE, 'e2e.c')], inc, ['-lwinmm'])
                 and run([os.path.join(BUILD, 'e2e' + EXE)], cwd=BUILD)[0])
        step('screen redraw test', lambda: compile_c(args.cc, 'screentest', [os.path.join(HERE, 'screentest.c')],
                                                     inc + ['-Wno-unused-function'])
             and run([os.path.join(BUILD, 'screentest' + EXE)], cwd=BUILD)[0])

    rows, convs = ('300', '100') if args.quick else ('3000', '600')
    step('packer assembly check', lambda: run([py, '-I', os.path.join(HERE, 'asmcheck.py'), 'pack',
                                               os.path.join(SOURCE, 'flashback_pack.s'), rows])[0])
    step('capture assembly check', lambda: run([py, '-I', os.path.join(HERE, 'asmcheck.py'), 'capture',
                                                os.path.join(SOURCE, 'flashback_capture.s'), convs])[0])
    pc = os.path.join(HERE, 'sdtest', 'pc')
    step('SD test app logic', lambda: compile_c(args.cc, 'sdtest_pc', [os.path.join(pc, 'pctest.c')],
                                                ['-w', '-I', pc, '-include', os.path.join(pc, '3ds.h')])
         and run([os.path.join(BUILD, 'sdtest_pc' + EXE)], cwd=BUILD, quiet=True)[0])

    print('\n=== summary')
    for name, ok in results:
        print(f'  {"passed" if ok else "FAILED"}  {name}')
    failed = [n for n, ok in results if not ok]
    print('all passed' if not failed else f'{len(failed)} FAILED')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
