"""CPU-only ELF loader regression; no Vulkan or real LLVM compilation."""
import os
from pathlib import Path
import resource
import subprocess

import pytest

HERE = Path(__file__).parent


@pytest.mark.parametrize('visibility,original', [
    ('local', False), ('global', False), ('local', True),
])
def test_forwarding(tmp_path, visibility, original):
    sources = {
        'llvm.c': r'''
#include <stddef.h>
#include <stdint.h>
#include <string.h>
void *LLVMRunPasses(void *m, const char *p, void *t, void *o) {
    return (uintptr_t)m == 11 && !strcmp(p, "sentinel") &&
           (uintptr_t)t == 22 && (uintptr_t)o == 33 ? (void *)44 : 0;
}
void *LLVMGetPointerToGlobal(void *e, void *v) {
    return (uintptr_t)e == 55 && (uintptr_t)v == 66 ? (void *)77 : 0;
}
const char *LLVMGetModuleIdentifier(void *m, size_t *n) {
    (void)m; *n = 6; return "module";
}
const char *LLVMGetValueName2(void *v, size_t *n) {
    (void)v; *n = 8; return "function";
}
''',
        'driver.c': r'''
#include <stdint.h>
extern void *LLVMRunPasses(void *, const char *, void *, void *);
extern void *LLVMGetPointerToGlobal(void *, void *);
int exercise(void) {
    return (uintptr_t)LLVMRunPasses((void *)11, "sentinel", (void *)22,
                                  (void *)33) != 44 ||
           (uintptr_t)LLVMGetPointerToGlobal((void *)55, (void *)66) != 77;
}
''',
        'host.c': r'''
#include <dlfcn.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    void *driver = dlopen(argv[1], RTLD_NOW |
        (!strcmp(argv[2], "global") ? RTLD_GLOBAL : RTLD_LOCAL));
    if (!driver) return 3;
    int (*exercise)(void) = (int (*)(void))dlsym(driver, "exercise");
    return exercise ? exercise() : 4;
}
''',
    }
    for name, source in sources.items():
        (tmp_path / name).write_text(source)
    commands = [
        ['cc', '-shared', '-fPIC', 'llvm.c', '-Wl,-soname,libLLVM.so.20.1',
         '-o', 'libLLVM.so.20.1'],
        ['cc', '-shared', '-fPIC', 'driver.c', '-L.', '-l:libLLVM.so.20.1',
         '-Wl,-rpath,$ORIGIN', '-o', 'driver.so'],
        ['cc', 'host.c', '-ldl', '-o', 'host'],
        ['cc', '-shared', '-fPIC', '-O2', '-Wall', '-Wextra', '-Werror',
         str(HERE / ('executed-trace_llvm.c' if original else 'trace_llvm.c')),
         '-ldl', '-o', 'trace.so'],
    ]
    for command in commands:
        subprocess.run(command, cwd=tmp_path, check=True, capture_output=True,
                       text=True, timeout=30)
    # The original interposer deliberately aborts; do not leave a core file.
    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    result = subprocess.run(
        [str(tmp_path / 'host'), str(tmp_path / 'driver.so'), visibility],
        env=dict(os.environ, LD_PRELOAD=str(tmp_path / 'trace.so')),
        capture_output=True, text=True, timeout=10, preexec_fn=no_core)
    if original:
        assert result.returncode == -6, result.stderr
        assert 'missing LLVMRunPasses' in result.stderr
    else:
        assert result.returncode == 0, result.stderr
        for call, name in [('LLVMRunPasses', 'module'),
                           ('LLVMGetPointerToGlobal', 'function')]:
            for phase in ['enter', 'exit']:
                assert f'phase={call}.{phase} name={name}' in result.stderr
