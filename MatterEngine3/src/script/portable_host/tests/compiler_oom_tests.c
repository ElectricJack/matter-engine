/* Sweep public compilation after runtime/context initialization. Unlike the
 * parser-only regression, this also covers variable/label resolution and
 * function bytecode construction. Optional source-map allocation failures may
 * succeed, but the compiled program must still return the expected value. */
#include "quickjs.h"
#include "cutils.h"
#include "oom_allocator.h"

typedef struct Bytecode {
    uint8_t *data;
    size_t size;
} Bytecode;

static int failed_buffer_stays_stopped(void)
{
    Allocator a = {0};
    DynBuf bc;
    const uint8_t payload[32] = {0};
    int ok;
    dbuf_init2(&bc, &a, resize);
    if (dbuf_realloc(&bc, 8) || dbuf_putc(&bc, 42))
        return 0;
    a.armed = 1;
    a.fail_at = 1;
    ok = dbuf_put(&bc, payload, sizeof(payload)) < 0 && a.failed;
    /* All of these fit in the old capacity. None may extend or rewrite the
       truncated instruction after the first failed write. */
    ok = ok && dbuf_realloc(&bc, 2) < 0 && dbuf_putc(&bc, 1) < 0 &&
         dbuf_put_self(&bc, 0, 1) < 0 && dbuf_write(&bc, 0, payload, 1) < 0 &&
         bc.size == 1 && bc.buf[0] == 42;
    dbuf_free(&bc);
    return ok && a.live == 0;
}

static int compile(const char *source, int expected, int all_allocations,
                   size_t fail_at, size_t *calls, int *optional, Bytecode *baseline)
{
    const JSMallocFunctions functions = {
        zero_allocate, allocate, release, resize, allocation_size
    };
    Allocator a = {0};
    JSRuntime *rt = JS_NewRuntime2(&functions, &a);
    JSContext *ctx = rt ? JS_NewContext(rt) : NULL;
    JSValue result, exception;
    int ok, threw;
    int32_t value = 0;
    if (!ctx) {
        fprintf(stderr, "runtime/context setup failed\n");
        if (rt)
            JS_FreeRuntime(rt);
        return 0;
    }
    a.all_allocations = all_allocations;
    a.fail_at = fail_at;
    a.armed = 1;
    result = JS_Eval(ctx, source, strlen(source), "compiler-oom-unique-filename.js",
                     JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    a.armed = 0;
    *calls = a.calls;
    threw = JS_IsException(result);
    ok = fail_at ? a.failed : !threw;
    if (threw) {
        if (!JS_HasException(ctx))
            ok = 0;
        exception = JS_GetException(ctx);
        JS_FreeValue(ctx, exception);
    } else {
        if (JS_HasException(ctx))
            ok = 0;
        size_t size;
        uint8_t *bytes = JS_WriteObject(ctx, &size, result,
            JS_WRITE_OBJ_BYTECODE | JS_WRITE_OBJ_STRIP_SOURCE | JS_WRITE_OBJ_STRIP_DEBUG);
        if (!bytes) {
            ok = 0;
        } else if (!fail_at) {
            baseline->data = malloc(size);
            baseline->size = size;
            if (!baseline->data)
                ok = 0;
            else
                memcpy(baseline->data, bytes, size);
        } else if (size != baseline->size || memcmp(bytes, baseline->data, size)) {
            /* Optional debug failures may omit debug data, never executable
               code, constants or captured bindings. */
            ok = 0;
        }
        js_free(ctx, bytes);
        if (a.failed)
            ++*optional;
        result = JS_EvalFunction(ctx, result);
        if (JS_IsException(result) || JS_ToInt32(ctx, &value, result) || value != expected)
            ok = 0;
    }
    JS_FreeValue(ctx, result);
    if (JS_HasException(ctx)) {
        exception = JS_GetException(ctx);
        JS_FreeValue(ctx, exception);
    }
    result = JS_Eval(ctx, "1 + 2", 5, "recovery.js", JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsNumber(result) || JS_ToInt32(ctx, &value, result) || value != 3)
        ok = 0;
    JS_FreeValue(ctx, result);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    if (a.live)
        ok = 0;
    if (!ok)
        fprintf(stderr, "failed allocation %zu: injected=%d, exception=%d, value=%d, live=%zu\n",
                fail_at, a.failed, threw, (int)value, a.live);
    return ok;
}

int main(void)
{
    const struct { const char *name, *source; int expected; } cases[] = {
        {"branch", "if (1) { 2; } else { 3; }", 2},
        {"atoms", "let object = {field: 1}; object.field += 2; object['field'];", 3},
        {"labels", "let n = 0; while (n < 3) { if (n) n += 1; else ++n; }", 3},
        {"nested", "function f(x) { return x ? 1 : 2; } f(0);", 2},
        {"closure", "function outer(x) { return function inner(y) { return x + y; }; } outer(1)(2);", 3},
        {"dead-atoms", "function f(x) { if (x) return 'gone'; else return 3; return 'dead'; } f(0);", 3},
        {"with", "let obj = {answer: 2}; with(obj) { answer += 1; } obj.answer;", 3},
        {"private", "class C { #x = 3; get() { return this.#x; } } new C().get();", 3},
        {"eval", "function f(x) { eval(''); return arguments[0] + x; } f(2);", 4},
        {"switch", "function f(x) { switch(x) { case 1: return 1; default: return 3; case 2: return 2; } } f(0);", 3},
        {"finally", "function f(x) { try { if(x) return 1; return 2; } finally { x++; } } f(0);", 2}
    };
    size_t i, n, calls, ignored;
    int mode, optional;
    if (!failed_buffer_stays_stopped()) {
        fprintf(stderr, "failed DynBuf accepted a subsequent write\n");
        return 1;
    }
    for (mode = 0; mode < 2; ++mode) {
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            Bytecode baseline = {0};
            optional = 0;
            if (!compile(cases[i].source, cases[i].expected, mode, 0, &calls, &optional, &baseline)) {
                free(baseline.data);
                return 1;
            }
            for (n = 1; n <= calls; ++n) {
                fprintf(stderr, "compiler-oom %s %s allocation %zu/%zu\n",
                        mode ? "all" : "realloc", cases[i].name, n, calls);
                if (!compile(cases[i].source, cases[i].expected, mode, n, &ignored, &optional, &baseline)) {
                    free(baseline.data);
                    return 1;
                }
            }
            free(baseline.data);
            printf("PASS compiler-oom %s %s: %zu failures (%d optional successes)\n",
                   mode ? "all" : "realloc", cases[i].name, calls, optional);
        }
    }
    return 0;
}
