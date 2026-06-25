#include <cstdio>
#include <cstdint>
#include <cstring>
extern "C" {
#include "quickjs.h"
}

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

static void test_embed_eval_1_plus_1() {
    JSRuntime* rt = JS_NewRuntime();
    CHECK(rt != nullptr, "runtime created");
    JSContext* ctx = JS_NewContext(rt);
    CHECK(ctx != nullptr, "context created");
    JSValue v = JS_Eval(ctx, "1+1", 3, "<test>", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(v), "eval did not throw");
    int32_t out = -1;
    CHECK(JS_ToInt32(ctx, &out, v) == 0, "result convertible to int");
    CHECK(out == 2, "1+1 == 2");
    JS_FreeValue(ctx, v);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

int main() {
    test_embed_eval_1_plus_1();
    if (failures == 0) printf("ALL PASS\n");
    return failures ? 1 : 0;
}
