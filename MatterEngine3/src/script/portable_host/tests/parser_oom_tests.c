/* Fail each growing realloc during parsing, after runtime/context setup.
 * Bytecode buffers, label tables and variable tables use realloc (including
 * realloc(NULL, size)). Keep malloc/calloc available for tokens and exceptions
 * to isolate emitter failures from unrelated VM initialization/OOM behavior. */
/* Include the production VM translation unit to stop fault injection at the
 * parser boundary. Later compiler passes and optional debug allocations have
 * separate failure contracts; no test hooks are added to the shipped VM. */
#include "quickjs.c"
#include "oom_allocator.h"

/* The global-eval setup used by __JS_EvalInternal, with allocation failures
 * armed only around the real parser. js_create_function still consumes and
 * validates the resulting function tree, with allocations restored. */
static JSValue parse_and_compile(JSContext *ctx, Allocator *a, const char *source)
{
    JSParseState s;
    JSFunctionDef *fd;
    int err;
    js_parse_init(ctx, &s, source, strlen(source), "parser-oom.js", 1);
    fd = js_new_function_def(ctx, NULL, true, false, s.filename, 1, 1);
    if (!fd)
        return JS_EXCEPTION;
    s.cur_func = fd;
    s.allow_html_comments = true;
    fd->eval_type = JS_EVAL_TYPE_GLOBAL;
    fd->has_this_binding = true;
    fd->arguments_allowed = true;
    fd->func_name = JS_DupAtom(ctx, JS_ATOM__eval_);
    a->armed = 1;
    push_scope(&s);
    fd->body_scope = fd->scope_level;
    err = js_parse_program(&s);
    a->armed = 0;
    if (err) {
        free_token(&s, &s.token);
        js_free_function_def(ctx, fd);
        return JS_EXCEPTION;
    }
    return js_create_function(ctx, fd);
}

static int compile(const char *source, size_t fail_at, size_t *calls)
{
    const JSMallocFunctions functions = {
        zero_allocate, allocate, release, resize, allocation_size
    };
    Allocator a = {0};
    JSRuntime *rt = JS_NewRuntime2(&functions, &a);
    JSContext *ctx = rt ? JS_NewContext(rt) : NULL;
    JSValue result, exception;
    int ok, threw;
    if (!ctx) {
        fprintf(stderr, "runtime/context setup failed\n");
        if (rt)
            JS_FreeRuntime(rt);
        return 0;
    }
    a.fail_at = fail_at;
    result = parse_and_compile(ctx, &a, source);
    *calls = a.calls;
    threw = JS_IsException(result);
    ok = (!fail_at || a.failed) && (a.failed ? threw : !threw);
    exception = JS_GetException(ctx);
    if (a.failed && JS_IsNull(exception))
        ok = 0;
    JS_FreeValue(ctx, exception);
    if (!threw) {
        /* Successful compilation must also produce executable bytecode. */
        result = JS_EvalFunction(ctx, result);
        if (JS_IsException(result))
            ok = 0;
    }
    JS_FreeValue(ctx, result);
    /* The same runtime must remain usable after the failed compilation. */
    result = JS_Eval(ctx, "1 + 2", 5, "recovery.js", JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsNumber(result) || JS_VALUE_GET_INT(result) != 3)
        ok = 0;
    JS_FreeValue(ctx, result);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    if (a.live)
        ok = 0;
    if (!ok)
        fprintf(stderr, "failed allocation %zu: injected=%d, exception=%d, live=%zu\n",
                fail_at, a.failed, threw, a.live);
    return ok;
}

int main(void)
{
    const struct { const char *name, *source; } cases[] = {
        {"branch", "if (1) { 2; } else { 3; }"},
        {"atoms", "let object = {field: 1}; object.field += 2; object['field'];"},
        {"labels", "let n = 0; while (n < 3) { if (n) n += 1; else ++n; }"},
        {"nested", "function f(x) { return x ? 1 : 2; } f(0);"}
    };
    size_t i, n, calls, ignored;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (!compile(cases[i].source, 0, &calls))
            return 1;
        for (n = 1; n <= calls; ++n) {
            fprintf(stderr, "parser-oom %s allocation %zu/%zu\n", cases[i].name, n, calls);
            if (!compile(cases[i].source, n, &ignored))
                return 1;
        }
        printf("PASS parser-oom %s: %zu allocation failures\n", cases[i].name, calls);
    }
    return 0;
}
