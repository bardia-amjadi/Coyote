// coyote-llvm: a separate native backend. It reads a *verified* ML program (the same input as the
// PVM), emits textual LLVM IR for the supported subset and, unless --emit-ll is given, asks clang to
// build an executable. It shares no code with the compiler; the only contract is docs/ml-spec.md.
//
// Supported subset: int, long, byte, double, char, boolean and string values; checked arithmetic;
// bitwise operators; conversions; comparisons; string concatenation, conversion, indexing and ordering;
// control flow; calls; console print/println; arrays and classes (with a native heap and GC).
// Everything else is rejected with a clear error (see docs/llvm.md).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "host.hpp"
#include "loader.hpp"
#include "ml.hpp"
#include "verifier.hpp"

#ifndef COYOTE_VERSION
#define COYOTE_VERSION "0.0.0"
#endif

namespace {

using pvm::Op;
using pvm::Type;

// ---- C runtime linked into every native program ----

const char* kRuntime = R"RT(#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define rt_getcwd _getcwd
#define rt_mkdir(p) _mkdir(p)
#define rt_rmdir(p) _rmdir(p)
#define rt_lstat stat
#else
#include <unistd.h>
#define rt_getcwd getcwd
#define rt_mkdir(p) mkdir(p, 0777)
#define rt_rmdir(p) rmdir(p)
#define rt_lstat lstat
#endif

void rt_fail(const char* file, int line, const char* msg) {
    fflush(stdout);
    if (line > 0) fprintf(stderr, "%s:%d: runtime error: %s\n", file, line, msg);
    else fprintf(stderr, "%s: runtime error: %s\n", file, msg);
    exit(1);
}
/* Stack trace, like the PVM: every call site records its function, file and line under the caller's depth. */
#define RT_TRACE_MAX 5002
static const char* rt_tr_fn[RT_TRACE_MAX];
static const char* rt_tr_file[RT_TRACE_MAX];
static int rt_tr_line[RT_TRACE_MAX];
extern int32_t rt_depth;
void rt_trace(int32_t depth, const char* fn, const char* file, int32_t line) {
    if (depth >= 0 && depth < RT_TRACE_MAX) { rt_tr_fn[depth] = fn; rt_tr_file[depth] = file; rt_tr_line[depth] = line; }
}
void rt_raise(const char* msg, const char* trace, void* payload, const char* fn, const char* file, int line);
void rt_fail_at(const char* fn, const char* file, int line, const char* msg) { rt_raise(msg, NULL, NULL, fn, file, line); }
/* A string value points at its characters; in front of them are the reference count (int64, just before) and the length in bytes (int64,
   before that). Strings may contain NUL characters, like the PVM's; a NUL byte also follows the characters for C interop. */
#define RT_LEN(s) (*((const int64_t*)(s) - 2))
void rt_print(const char* s) { fwrite(s, 1, (size_t)RT_LEN(s), stdout); }
void rt_println(const char* s) { fwrite(s, 1, (size_t)RT_LEN(s), stdout); fputc('\n', stdout); }
bool rt_streq(const char* a, const char* b) { return RT_LEN(a) == RT_LEN(b) && memcmp(a, b, (size_t)RT_LEN(a)) == 0; }
/* Call depth, like the PVM: the entry function is frame 1 and a call that would make it more than 5000 is a runtime error
   (the generated code checks rt_depth before every call). */
int32_t rt_depth = 1;
/* Strings are reference counted. A string value points at its characters; the count is the int64 just before them.
   Constants have a huge count (immortal). Every string on the operand stack or in a local owns one reference. */
#define RT_IMMORTAL 0x4000000000000000LL
static char* rt_alloc(size_t n) {  /* n bytes including the closing NUL; the length is n - 1 until the caller says otherwise */
    int64_t* h = (int64_t*)malloc(2 * sizeof(int64_t) + n);
    if (!h) rt_fail("native", 0, "out of memory");
    h[0] = (int64_t)n - 1;
    h[1] = 1;
    return (char*)(h + 2);
}
void rt_retain(const char* s) { int64_t* h = (int64_t*)s - 1; if (*h < RT_IMMORTAL) ++*h; }
void rt_release(const char* s) { int64_t* h = (int64_t*)s - 1; if (*h < RT_IMMORTAL && --*h == 0) free(h - 1); }
const char* rt_concat(const char* a, const char* b) {
    size_t la = (size_t)RT_LEN(a), lb = (size_t)RT_LEN(b);
    char* r = rt_alloc(la + lb + 1);
    memcpy(r, a, la);
    memcpy(r + la, b, lb + 1);
    return r;
}
const char* rt_str_int(int32_t v) {
    char* r = rt_alloc(16);
    snprintf(r, 16, "%d", (int)v);
    ((int64_t*)r)[-2] = (int64_t)strlen(r);
    return r;
}
const char* rt_str_lng(int64_t v) {
    char* r = rt_alloc(24);
    snprintf(r, 24, "%lld", (long long)v);
    ((int64_t*)r)[-2] = (int64_t)strlen(r);
    return r;
}
/* Same text as the PVM's formatDouble: the shortest of 15, 16 or 17 significant digits that reads back exactly. */
const char* rt_str_dbl(double v) {
    char buf[48];
    if (isnan(v)) strcpy(buf, "NaN");
    else if (isinf(v)) strcpy(buf, v < 0 ? "-Infinity" : "Infinity");
    else {
        for (int prec = 15; prec <= 17; ++prec) {
            snprintf(buf, sizeof buf, "%.*g", prec, v);
            if (strtod(buf, NULL) == v) break;
        }
        if (!strpbrk(buf, ".en")) strcat(buf, ".0");
    }
    size_t n = strlen(buf);
    char* r = rt_alloc(n + 1);
    memcpy(r, buf, n + 1);
    return r;
}
const char* rt_str_chr(int32_t cp) {
    char* r = rt_alloc(5);
    if (cp < 0x80) { r[0] = (char)cp; r[1] = 0; ((int64_t*)r)[-2] = 1; }
    else if (cp < 0x800) { r[0] = (char)(0xC0 | (cp >> 6)); r[1] = (char)(0x80 | (cp & 0x3F)); r[2] = 0; ((int64_t*)r)[-2] = 2; }
    else if (cp < 0x10000) { r[0] = (char)(0xE0 | (cp >> 12)); r[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); r[2] = (char)(0x80 | (cp & 0x3F)); r[3] = 0; ((int64_t*)r)[-2] = 3; }
    else { r[0] = (char)(0xF0 | (cp >> 18)); r[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); r[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); r[3] = (char)(0x80 | (cp & 0x3F)); r[4] = 0; }
    return r;
}
int32_t rt_str_len(const char* s) {
    int32_t n = 0;
    for (const char* e = s + RT_LEN(s); s < e; ++s) if (((unsigned char)*s & 0xC0) != 0x80) ++n;
    return n;
}
/* Code point at a code point index, or -1 when out of range. */
int32_t rt_str_at(const char* s, int32_t idx) {
    if (idx < 0) return -1;
    const unsigned char* p = (const unsigned char*)s;
    const unsigned char* end = p + RT_LEN(s);
    int32_t k = 0;
    while (p < end) {
        int len = *p < 0x80 ? 1 : *p < 0xE0 ? 2 : *p < 0xF0 ? 3 : 4;
        if (k == idx) {
            int32_t cp = len == 1 ? *p : len == 2 ? (*p & 0x1F) : len == 3 ? (*p & 0x0F) : (*p & 0x07);
            for (int j = 1; j < len; ++j) cp = (cp << 6) | (p[j] & 0x3F);
            return cp;
        }
        p += len;
        ++k;
    }
    return -1;
}
int32_t rt_strcmp(const char* a, const char* b) {
    int64_t la = RT_LEN(a), lb = RT_LEN(b);
    int c = memcmp(a, b, (size_t)(la < lb ? la : lb));
    if (c == 0) c = la < lb ? -1 : la > lb ? 1 : 0;
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}
/* Runtime errors whose message contains a value. kind: 7 array index, 8 negative array size, 1 long->int, 2 double->long, 3 double->int, 4 int->byte, 5 int->char, 6 string index. */
void rt_fail_cvt(const char* fn, const char* file, int line, int kind, int64_t a, double d, int64_t b) {
    char msg[160];
    char dbuf[48];
    if (kind == 2 || kind == 3) { const char* t = rt_str_dbl(d); snprintf(dbuf, sizeof dbuf, "%s", t); rt_release(t); }
    switch (kind) {
        case 1: snprintf(msg, sizeof msg, "cannot convert the long value %lld to int (out of range)", (long long)a); break;
        case 2: snprintf(msg, sizeof msg, "cannot convert %s to long (out of range)", dbuf); break;
        case 3: snprintf(msg, sizeof msg, "cannot convert %s to int (out of range)", dbuf); break;
        case 4: snprintf(msg, sizeof msg, "cannot convert %lld to byte (0..255)", (long long)a); break;
        case 5: snprintf(msg, sizeof msg, "invalid character code %lld", (long long)a); break;
        case 7: snprintf(msg, sizeof msg, "array index %lld is out of range for an array of length %lld", (long long)a, (long long)b); break;
        case 8: snprintf(msg, sizeof msg, "negative array size %lld", (long long)a); break;
        default: snprintf(msg, sizeof msg, "string index %lld is out of range for a string of length %lld", (long long)a, (long long)rt_str_len((const char*)b)); break;
    }
    rt_fail_at(fn, file, line, msg);
}
/* ---- heap: arrays with a precise, non-moving, stop-the-world mark-and-sweep collector (same trigger and limit as the PVM) ----
   Roots are the frame arrays that generated code links into rt_frames: every reference-typed local and operand-stack slot of every active
   function. Strings are not traced (they are reference counted); an array of strings owns one count per element. */
typedef struct RtObj {
    struct RtObj* next;   /* 0 */
    uint32_t mark;        /* 8 */
    uint32_t ek;          /* 12: element kind 'i' (32-bit slot), 'l', 'd', 's', 'r' or 'y' (compact bytes) */
    int64_t len;          /* 16 */
    int64_t cap;          /* 24 */
    int64_t weight;       /* 32: slots charged to the heap limit */
    void* data;           /* 40 */
} RtObj;
typedef struct RtFrame { struct RtFrame* prev; int64_t n; void** slots; } RtFrame;
extern const int32_t rt_class_count;
extern const char* const rt_class_kinds[];
extern const char* const rt_class_names[];
extern const unsigned char rt_isa[];
extern const int32_t rt_groots_n;
extern RtObj** const rt_groots[];
static RtFrame* rt_frames = NULL;
static RtObj* rt_head = NULL;
static int64_t rt_gc_count = 0, rt_gc_freed = 0;
static int64_t rt_objects = 0, rt_elements = 0, rt_obj_trig = 1024, rt_elem_trig = 1 << 20;
#define RT_MAX_ELEMENTS 8000000
const char* rt_loc_file = "native";
int32_t rt_loc_line = 0;
void rt_frame_push(RtFrame* f, void** slots, int64_t n) { f->prev = rt_frames; f->n = n; f->slots = slots; rt_frames = f; }
void rt_frame_pop(RtFrame* f) { rt_frames = f->prev; }
static size_t rt_stride(uint32_t ek) { return ek == 'y' ? 1 : 8; }
typedef struct RtMap { int64_t n, cap, live; int64_t* keys; int64_t* vals; char* alive; int32_t* tab; int64_t tabcap; } RtMap;
static int rt_mkk(const RtObj* o) { return (int)((o->cap >> 8) & 0xFF); }
static int rt_mvk(const RtObj* o) { return (int)(o->cap & 0xFF); }
static void rt_map_free(RtObj* o);
int32_t rt_map_len(RtObj* o);
int64_t rt_map_find(RtObj* o, int64_t key);
void* rt_map_slot(RtObj* o, int64_t idx);
int64_t rt_map_put(RtObj* o, int64_t key, const char* emptyStr);
bool rt_obj_eq(RtObj* a, RtObj* b);
const char* rt_concat(const char* a, const char* b);
void* rt_arr_push_slot(RtObj* o);
static size_t rt_cps(const char* s);
RtObj* rt_arr_copy(RtObj* src);
int32_t rt_map_del(RtObj* o, int64_t key);
RtObj* rt_map_collect(RtObj* o, int32_t which, uint32_t ek);
static RtObj* rt_tmp_root = NULL;  /* a value the runtime itself is building while it may collect */
static RtObj* rt_last_payload = NULL;  /* the object thrown by the most recent caught exception (mouth sys.exceptionObject) */
static void rt_collect(void) {
    ++rt_gc_count;
    size_t cap = 256, n = 0;
    RtObj** work = (RtObj**)malloc(cap * sizeof(RtObj*));
    if (!work) rt_fail(rt_loc_file, rt_loc_line, "out of memory");
    if (rt_tmp_root && !rt_tmp_root->mark) { rt_tmp_root->mark = 1; work[n++] = rt_tmp_root; }
    if (rt_last_payload && !rt_last_payload->mark) { rt_last_payload->mark = 1; work[n++] = rt_last_payload; }
    for (int32_t g = 0; g < rt_groots_n; ++g) {
        RtObj* o = *rt_groots[g];
        if (o && !o->mark) { o->mark = 1; if (n == cap) { cap *= 2; work = (RtObj**)realloc(work, cap * sizeof(RtObj*)); } work[n++] = o; }
    }
    for (RtFrame* f = rt_frames; f; f = f->prev)
        for (int64_t i = 0; i < f->n; ++i) {
            RtObj* o = (RtObj*)f->slots[i];
            if (o && !o->mark) { o->mark = 1; if (n == cap) { cap *= 2; work = (RtObj**)realloc(work, cap * sizeof(RtObj*)); } work[n++] = o; }
        }
    while (n > 0) {
        RtObj* o = work[--n];
        if (o->ek == 'm') {
            RtMap* mp = (RtMap*)o->data;
            if (rt_mvk(o) == 'r')
                for (int64_t i = 0; i < mp->n; ++i) {
                    RtObj* c = (RtObj*)mp->vals[i];
                    if (mp->alive[i] && c && !c->mark) { c->mark = 1; if (n == cap) { cap *= 2; work = (RtObj**)realloc(work, cap * sizeof(RtObj*)); } work[n++] = c; }
                }
            continue;
        }
        if (o->ek == 'x') {  /* a box of an array or a map keeps it alive */
            if (o->cap == 'A' || o->cap == 'M') {
                RtObj* c = (RtObj*)((int64_t*)o->data)[0];
                if (c && !c->mark) { c->mark = 1; if (n == cap) { cap *= 2; work = (RtObj**)realloc(work, cap * sizeof(RtObj*)); } work[n++] = c; }
            }
            continue;
        }
        if (o->ek != 'r' && o->ek != 'o') continue;
        RtObj** items = (RtObj**)o->data;
        const char* kinds = o->ek == 'o' ? rt_class_kinds[o->cap] : NULL;
        for (int64_t i = 0; i < o->len; ++i) {
            if (kinds && kinds[i] != 'r') continue;
            RtObj* c = items[i];
            if (c && !c->mark) { c->mark = 1; if (n == cap) { cap *= 2; work = (RtObj**)realloc(work, cap * sizeof(RtObj*)); } work[n++] = c; }
        }
    }
    free(work);
    RtObj** link = &rt_head;
    while (*link) {
        RtObj* o = *link;
        if (o->mark) { o->mark = 0; link = &o->next; continue; }
        *link = o->next;
        if (o->ek == 's' || (o->ek == 'x' && o->cap == 's')) for (int64_t i = 0; i < o->len; ++i) { const char* s = ((const char**)o->data)[i]; if (s) rt_release(s); }
        if (o->ek == 'o') { const char* kinds = rt_class_kinds[o->cap]; for (int64_t i = 0; i < o->len; ++i) if (kinds[i] == 's') { const char* s = ((const char**)o->data)[i]; if (s) rt_release(s); } }
        if (o->ek == 'm') { rt_map_free(o); o->data = NULL; }
        rt_elements -= o->weight;
        --rt_objects;
        ++rt_gc_freed;
        free(o->data);
        free(o);
    }
    rt_obj_trig = rt_objects * 2 > 1024 ? rt_objects * 2 : 1024;
    rt_elem_trig = rt_elements * 2 > (1 << 20) ? rt_elements * 2 : (1 << 20);
}
static void rt_ensure_room(int64_t count) {
    if (count > RT_MAX_ELEMENTS || rt_elements + count > RT_MAX_ELEMENTS) {
        rt_collect();
        if (count > RT_MAX_ELEMENTS || rt_elements + count > RT_MAX_ELEMENTS) {
            char msg[100];
            snprintf(msg, sizeof msg, "out of memory: the heap limit of %d array elements would be exceeded", RT_MAX_ELEMENTS);
            rt_fail(rt_loc_file, rt_loc_line, msg);
        }
    }
}
RtObj* rt_arr_new(uint32_t ek, int64_t n, const char* fill) {
    int64_t weight = ek == 'y' ? (n + 15) / 16 : n;
    if (rt_objects >= rt_obj_trig || rt_elements >= rt_elem_trig) rt_collect();
    rt_ensure_room(weight);
    RtObj* o = (RtObj*)calloc(1, sizeof(RtObj));
    if (!o) rt_fail(rt_loc_file, rt_loc_line, "out of memory");
    o->ek = ek;
    o->len = n;
    o->cap = n;
    o->weight = weight;
    if (n > 0) {
        o->data = calloc((size_t)n, rt_stride(ek));
        if (!o->data) rt_fail(rt_loc_file, rt_loc_line, "out of memory");
        if (ek == 's' && fill) for (int64_t i = 0; i < n; ++i) ((const char**)o->data)[i] = fill;
    }
    o->next = rt_head;
    rt_head = o;
    ++rt_objects;
    rt_elements += weight;
    return o;
}
/* A class instance: n 8-byte field slots; cap holds the class index; the class table says which fields hold references (r) or strings (s). */
RtObj* rt_obj_new(int32_t cls, int64_t n, const char* emptyStr) {
    if (rt_objects >= rt_obj_trig || rt_elements >= rt_elem_trig) rt_collect();
    rt_ensure_room(n);
    RtObj* o = (RtObj*)calloc(1, sizeof(RtObj));
    if (!o) rt_fail(rt_loc_file, rt_loc_line, "out of memory");
    o->ek = 'o';
    o->len = n;
    o->cap = cls;
    o->weight = n;
    if (n > 0) {
        o->data = calloc((size_t)n, 8);
        if (!o->data) rt_fail(rt_loc_file, rt_loc_line, "out of memory");
        const char* kinds = rt_class_kinds[cls];
        for (int64_t i = 0; i < n; ++i) if (kinds[i] == 's') ((const char**)o->data)[i] = emptyStr;
    }
    o->next = rt_head;
    rt_head = o;
    ++rt_objects;
    rt_elements += n;
    return o;
}
int32_t rt_isinst(RtObj* o, int32_t target) { return o && rt_isa[(size_t)o->cap * (size_t)rt_class_count + (size_t)target] ? 1 : 0; }
void rt_downcast(const char* fn, const char* file, int line, RtObj* o, int32_t target) {
    if (!o || rt_isinst(o, target)) return;
    char msg[200];
    snprintf(msg, sizeof msg, "invalid cast: %s is not a %s", rt_class_names[o->cap], rt_class_names[target]);
    rt_fail_at(fn, file, line, msg);
}
/* ---- exceptions: handlers are records in the frames of the functions that executed `try`; a throw unwinds to the innermost one with
   the LLVM sjlj intrinsics. The record restores the frame chain and the call depth; strings held by the abandoned frames are leaked. ---- */
typedef struct RtHandler { struct RtHandler* prev; void* jb[5]; RtFrame* frames; int32_t depth; } RtHandler;
static RtHandler* rt_handlers = NULL;
static const char* rt_last_msg = NULL;
static const char* rt_last_trace = NULL;
static int rt_has_err = 0;
void rt_try_push(void* hp) { RtHandler* h = (RtHandler*)hp; h->prev = rt_handlers; h->frames = rt_frames; h->depth = rt_depth; rt_handlers = h; }
void rt_try_pop(void) { rt_handlers = rt_handlers->prev; }
void* rt_handlers_get(void) { return rt_handlers; }
void rt_handlers_set(void* h) { rt_handlers = (RtHandler*)h; }
static const char* rt_copy_str(const char* src) { size_t n = strlen(src); char* r = rt_alloc(n + 1); memcpy(r, src, n + 1); return r; }
static void rt_app(char** b, size_t* len, size_t* cap, const char* t) {
    size_t n = strlen(t);
    while (*len + n + 1 > *cap) { *cap *= 2; *b = (char*)realloc(*b, *cap); }
    memcpy(*b + *len, t, n + 1);
    *len += n;
}
/* The call stack as the PVM prints it: innermost first, "  at fn (file:line)", at most 25 frames. */
static const char* rt_make_trace(const char* fn, const char* file, int line) {
    size_t len = 0, cap = 256;
    char* b = (char*)malloc(cap);
    b[0] = 0;
    char tmp[512];
    int n = rt_depth;
    for (int k = 0; k < n; ++k) {
        if (n > 25 && k == 20) { snprintf(tmp, sizeof tmp, "  ... (%d more frames)", n - 25); k = n - 6; }
        else if (k == 0) snprintf(tmp, sizeof tmp, "  at %s (%s:%d)", fn, file, line);
        else { int d = n - k; snprintf(tmp, sizeof tmp, "  at %s (%s:%d)", rt_tr_fn[d], rt_tr_file[d], rt_tr_line[d]); }
        if (len) rt_app(&b, &len, &cap, "\n");
        rt_app(&b, &len, &cap, tmp);
    }
    const char* r = rt_copy_str(b);
    free(b);
    return r;
}
void rt_raise(const char* msg, const char* trace, void* payload, const char* fn, const char* file, int line) {
    const char* tr = trace && *trace ? trace : rt_make_trace(fn, file, line);
    if (!rt_handlers) {
        fflush(stdout);
        if (line > 0) fprintf(stderr, "%s:%d: runtime error: %s\n", file, line, msg);
        else fprintf(stderr, "%s: runtime error: %s\n", file, msg);
        fputs(tr, stderr);
        if (*tr) fputc('\n', stderr);
        exit(1);
    }
    RtHandler* h = rt_handlers;
    rt_handlers = h->prev;
    rt_frames = h->frames;
    rt_depth = h->depth;
    rt_last_msg = rt_copy_str(msg);
    rt_last_trace = rt_copy_str(tr);
    rt_last_payload = (RtObj*)payload;
    rt_has_err = 1;
    __builtin_longjmp(h->jb, 1);
}
const char* rt_err_msg(void) { rt_retain(rt_last_msg); return rt_last_msg; }
/* ---- obj: null, a class instance itself, or a box (ek 'x', one 8-byte slot, `cap` = the type tag: i l y b c d s) ---- */
RtObj* rt_box(int32_t tag, int64_t bits) {
    RtObj* o = rt_arr_new('l', 1, NULL);
    o->ek = 'x';
    o->cap = tag;
    ((int64_t*)o->data)[0] = bits;
    return o;
}
RtObj* rt_box_str(const char* s) { return rt_box('s', (int64_t)(intptr_t)s); }
RtObj* rt_mouth_str(int32_t which) {
    if (!rt_has_err) return NULL;
    const char* s = which == 0 ? rt_last_msg : rt_last_trace;
    rt_retain(s);
    return rt_box_str(s);
}
RtObj* rt_mouth_payload(void) { return rt_last_payload; }
static int32_t rt_exit_code = 0;
RtObj* rt_mouth_sys(int32_t which, const char* text) {  /* 0 os, 1 version, 2 pid, 3 exitCode */
    if (which == 2) {
#ifdef _WIN32
        return rt_box('i', (int64_t)GetCurrentProcessId());
#else
        return rt_box('i', (int64_t)getpid());
#endif
    }
    if (which == 3) return rt_box('i', rt_exit_code);
    if (which == 1) { rt_retain(text); return rt_box_str(text); }
#ifdef _WIN32
    return rt_box_str(rt_copy_str("windows"));
#else
    return rt_box_str(rt_copy_str("linux"));
#endif
}
void rt_set_exit(const char* fn, const char* file, int line, RtObj* o) {
    if (!o || o->ek != 'x' || o->cap != 'i') rt_fail_at(fn, file, line, "mouth 'sys.exitCode' failed: sys.exitCode expects an int");
    rt_exit_code = (int32_t)((int64_t*)o->data)[0];
}
int32_t rt_exit_status(void) { return rt_exit_code & 0xFF; }
static const char* rt_tag_name(int tag) {
    switch (tag) { case 'i': return "int"; case 'l': return "long"; case 'y': return "byte"; case 'b': return "boolean"; case 'c': return "char"; case 'd': return "double"; default: return "string"; }
}
static const char* rt_obj_type(RtObj* o) {
    if (!o) return "null";
    if (o->ek != 'x') return rt_class_names[o->cap];
    if (o->cap == 'A' || o->cap == 'M') return (const char*)(intptr_t)((int64_t*)o->data)[1];
    return rt_tag_name((int)o->cap);
}
bool rt_istype(RtObj* o, int32_t tag, int32_t cls) {
    if (!o) return false;
    if (tag == 'o') return o->ek == 'o' && o->cap == cls;
    return o->ek == 'x' && o->cap == tag;
}
RtObj* rt_unbox_class(const char* fn, const char* file, int line, RtObj* o, int32_t target, int32_t nullable) {
    if (!o && nullable) return NULL;
    if (o && o->ek == 'o' && rt_isinst(o, target)) return o;
    char msg[300];
    snprintf(msg, sizeof msg, "cannot convert %s to %s", rt_obj_type(o), rt_class_names[target]);
    rt_fail_at(fn, file, line, msg);
    return NULL;
}
/* Unboxes a scalar or a string (want = its tag); an int widens to long or double like the PVM. A string comes back retained. */
int64_t rt_unbox_bits(const char* fn, const char* file, int line, RtObj* o, int32_t want, const char* tname) {
    if (o && o->ek == 'x') {
        const int tag = (int)o->cap;
        const int64_t bits = ((int64_t*)o->data)[0];
        if (tag == want) { if (want == 's') rt_retain((const char*)(intptr_t)bits); return bits; }
        if (tag == 'i' && want == 'd') { double d = (double)bits; int64_t r; memcpy(&r, &d, 8); return r; }
        if (tag == 'i' && want == 'l') return bits;
    }
    char msg[300];
    snprintf(msg, sizeof msg, "cannot convert %s to %s", rt_obj_type(o), tname);
    rt_fail_at(fn, file, line, msg);
    return 0;
}
const char* rt_unbox_str(const char* fn, const char* file, int line, RtObj* o) { return (const char*)(intptr_t)rt_unbox_bits(fn, file, line, o, 's', "string"); }
const char* rt_obj_str(RtObj* o) {
    if (!o) return rt_copy_str("null");
    if (o->ek == 'x') {
        const int64_t bits = ((int64_t*)o->data)[0];
        switch ((int)o->cap) {
            case 's': { const char* s = (const char*)(intptr_t)bits; rt_retain(s); return s; }
            case 'A': case 'M': {
                RtObj* c = (RtObj*)(intptr_t)bits;
                char msg[400];
                if (o->cap == 'A') snprintf(msg, sizeof msg, "%s[%lld items]", rt_obj_type(o), (long long)c->len);
                else snprintf(msg, sizeof msg, "%s[%lld entries]", rt_obj_type(o), (long long)rt_map_len(c));
                return rt_copy_str(msg);
            }
            case 'b': return rt_copy_str(bits ? "true" : "false");
            case 'c': return rt_str_chr((int32_t)bits);
            case 'd': { double d; memcpy(&d, &bits, 8); return rt_str_dbl(d); }
            default: return rt_str_lng(bits);
        }
    }
    char msg[300];
    snprintf(msg, sizeof msg, "%s object", rt_class_names[o->cap]);
    return rt_copy_str(msg);
}
bool rt_obj_eq(RtObj* a, RtObj* b) {
    if (!a || !b) return !a && !b;
    if (a->ek != b->ek) return false;
    if (a->ek != 'x') return a == b;
    if (a->cap != b->cap) return false;
    const int64_t x = ((int64_t*)a->data)[0], y = ((int64_t*)b->data)[0];
    if (a->cap == 's') return rt_streq((const char*)(intptr_t)x, (const char*)(intptr_t)y);
    if (a->cap == 'd') { double dx, dy; memcpy(&dx, &x, 8); memcpy(&dy, &y, 8); return dx == dy; }
    return x == y;
}
/* ---- boxes of arrays and maps (scalar/string elements) and the dyn operations ---- */
RtObj* rt_box_ref(int32_t tag, RtObj* p, const char* tname, int32_t tags) {
    RtObj* o = rt_arr_new('l', 3, NULL);
    o->ek = 'x';
    o->cap = tag;
    ((int64_t*)o->data)[0] = (int64_t)(intptr_t)p;
    ((int64_t*)o->data)[1] = (int64_t)(intptr_t)tname;
    ((int64_t*)o->data)[2] = tags;
    return o;
}
bool rt_istype_ref(RtObj* o, int32_t tag, const char* tname) { return o && o->ek == 'x' && o->cap == tag && strcmp(tname, rt_obj_type(o)) == 0; }
RtObj* rt_unbox_ref(const char* fn, const char* file, int line, RtObj* o, int32_t tag, const char* tname) {
    if (!rt_istype_ref(o, tag, tname)) {
        char msg[500];
        snprintf(msg, sizeof msg, "cannot convert %s to %s", rt_obj_type(o), tname);
        rt_fail_at(fn, file, line, msg);
    }
    return (RtObj*)(intptr_t)((int64_t*)o->data)[0];
}
static int rt_is_num(int tag) { return tag == 'i' || tag == 'l' || tag == 'y' || tag == 'd'; }
static int rt_is_whole(int tag) { return tag == 'i' || tag == 'l' || tag == 'y'; }
static double rt_bits_d(int64_t b) { double d; memcpy(&d, &b, 8); return d; }
static int64_t rt_d_bits(double d) { int64_t b; memcpy(&b, &d, 8); return b; }
static double rt_num(RtObj* o) { int64_t b = ((int64_t*)o->data)[0]; return o->cap == 'd' ? rt_bits_d(b) : (double)b; }
static int rt_is_box(RtObj* o, int tag) { return o && o->ek == 'x' && o->cap == tag; }
static void rt_dyn_bad(const char* fn, const char* file, int line, const char* op, RtObj* a, RtObj* b, int binary) {
    char msg[500];
    if (binary) snprintf(msg, sizeof msg, "operator '%s' cannot be applied to %s and %s", op, rt_obj_type(a), rt_obj_type(b));
    else snprintf(msg, sizeof msg, "operator '%s' cannot be applied to %s", op, rt_obj_type(a));
    rt_fail_at(fn, file, line, msg);
}
/* op: 0 add 1 sub 2 mul 3 div 4 rem 5 eq 6 ne 7 lt 8 le 9 gt 10 ge (same rules as the PVM's dynBinary) */
RtObj* rt_dyn_bin(const char* fn, const char* file, int line, int32_t op, RtObj* oa, RtObj* ob) {
    static const char* ops[] = {"+", "-", "*", "/", "%", "==", "!=", "<", "<=", ">", ">="};
    const char* ot = ops[op];
    const int ta = oa && oa->ek == 'x' ? (int)oa->cap : 0, tb = ob && ob->ek == 'x' ? (int)ob->cap : 0;
    if (op == 5 || op == 6) {
        int eq;
        if (ta && tb && rt_is_num(ta) && rt_is_num(tb) && ta != tb) eq = rt_num(oa) == rt_num(ob);
        else eq = rt_obj_eq(oa, ob);
        return rt_box('b', op == 5 ? eq : !eq);
    }
    if (op == 0 && (ta == 's' || tb == 's')) {
        const char* x = rt_obj_str(oa);
        const char* y = rt_obj_str(ob);
        const char* r = rt_concat(x, y);
        rt_release(x); rt_release(y);
        return rt_box('s', (int64_t)(intptr_t)r);
    }
    if (!oa || !ob) rt_dyn_bad(fn, file, line, ot, oa, ob, 1);
    const int64_t x = oa->ek == 'x' ? ((int64_t*)oa->data)[0] : 0, y = ob->ek == 'x' ? ((int64_t*)ob->data)[0] : 0;
    if (op >= 7) {
        int cmp;
        if (rt_is_num(ta) && rt_is_num(tb)) {
            double p = rt_num(oa), q = rt_num(ob);
            if (p != p || q != q) return rt_box('b', 0);
            cmp = p < q ? -1 : p > q ? 1 : 0;
        } else if (ta == 'c' && tb == 'c') cmp = x < y ? -1 : x > y ? 1 : 0;
        else if (ta == 's' && tb == 's') cmp = rt_strcmp((const char*)(intptr_t)x, (const char*)(intptr_t)y);
        else { rt_dyn_bad(fn, file, line, ot, oa, ob, 1); return NULL; }
        int r = op == 7 ? cmp < 0 : op == 8 ? cmp <= 0 : op == 9 ? cmp > 0 : cmp >= 0;
        return rt_box('b', r);
    }
    if (!rt_is_num(ta) || !rt_is_num(tb)) { rt_dyn_bad(fn, file, line, ot, oa, ob, 1); return NULL; }
    if (rt_is_whole(ta) && rt_is_whole(tb) && (ta == 'l' || tb == 'l')) {
        long long r = 0;
        switch (op) {
            case 0: if (__builtin_add_overflow((long long)x, (long long)y, &r)) rt_fail_at(fn, file, line, "long overflow"); break;
            case 1: if (__builtin_sub_overflow((long long)x, (long long)y, &r)) rt_fail_at(fn, file, line, "long overflow"); break;
            case 2: if (__builtin_mul_overflow((long long)x, (long long)y, &r)) rt_fail_at(fn, file, line, "long overflow"); break;
            case 3:
                if (y == 0) rt_fail_at(fn, file, line, "division by zero");
                if (x == INT64_MIN && y == -1) rt_fail_at(fn, file, line, "long overflow");
                r = x / y; break;
            default:
                if (y == 0) rt_fail_at(fn, file, line, "remainder by zero");
                r = y == -1 ? 0 : x % y; break;
        }
        return rt_box('l', r);
    }
    if (rt_is_whole(ta) && rt_is_whole(tb)) {
        long long r = 0;
        switch (op) {
            case 0: r = x + y; break; case 1: r = x - y; break; case 2: r = x * y; break;
            case 3: if (y == 0) rt_fail_at(fn, file, line, "division by zero"); r = x / y; break;
            default: if (y == 0) rt_fail_at(fn, file, line, "remainder by zero"); r = x % y; break;
        }
        if (r < -2147483648LL || r > 2147483647LL) rt_fail_at(fn, file, line, "integer overflow");
        return rt_box('i', r);
    }
    double p = rt_num(oa), q = rt_num(ob), r;
    switch (op) { case 0: r = p + q; break; case 1: r = p - q; break; case 2: r = p * q; break; case 3: r = p / q; break; default: r = fmod(p, q); break; }
    return rt_box('d', rt_d_bits(r));
}
RtObj* rt_dyn_un(const char* fn, const char* file, int line, int32_t op, RtObj* oa) {  /* 0 neg, 1 not */
    if (oa && oa->ek == 'x') {
        const int t = (int)oa->cap;
        const int64_t b = ((int64_t*)oa->data)[0];
        if (op == 0 && (t == 'i' || t == 'y')) { if (-b < -2147483648LL || -b > 2147483647LL) rt_fail_at(fn, file, line, "integer overflow"); return rt_box('i', -b); }
        if (op == 0 && t == 'l') { if (b == INT64_MIN) rt_fail_at(fn, file, line, "long overflow"); return rt_box('l', -b); }
        if (op == 0 && t == 'd') return rt_box('d', rt_d_bits(-rt_bits_d(b)));
        if (op == 1 && t == 'b') return rt_box('b', b == 0);
    }
    rt_dyn_bad(fn, file, line, op == 0 ? "-" : "!", oa, NULL, 0);
    return NULL;
}
/* converts a box to a value of tag `want` (an int widens to long or double); returns 0 when it does not fit */
static int rt_conv_tag(RtObj* o, int want, int64_t* out) {
    if (!o || o->ek != 'x') return 0;
    const int t = (int)o->cap;
    const int64_t b = ((int64_t*)o->data)[0];
    if (t == want) { *out = b; return 1; }
    if (t == 'i' && want == 'd') { *out = rt_d_bits((double)b); return 1; }
    if (t == 'i' && want == 'l') { *out = b; return 1; }
    return 0;
}
static const char* rt_key_text(int tag, int64_t bits) {
    switch (tag) {
        case 's': { rt_retain((const char*)(intptr_t)bits); return (const char*)(intptr_t)bits; }
        case 'b': return rt_copy_str(bits ? "true" : "false");
        case 'c': return rt_str_chr((int32_t)bits);
        default: return rt_str_lng(bits);
    }
}
static int64_t rt_slot_bits(int tag, const void* slot) {  /* a stored element as the bits of its box */
    switch (tag) {
        case 'l': return *(const int64_t*)slot;
        case 'd': return *(const int64_t*)slot;
        case 's': return *(const int64_t*)slot;
        case 'b': return *(const int32_t*)slot != 0;
        default: return *(const int32_t*)slot;  /* i, y, c */
    }
}
static void rt_store_slot(int tag, void* slot, int64_t bits) {
    if (tag == 'l' || tag == 'd' || tag == 's') *(int64_t*)slot = bits; else *(int32_t*)slot = (int32_t)bits;
}
static RtObj* rt_box_slot(int tag, const void* slot) {
    int64_t bits = rt_slot_bits(tag, slot);
    if (tag == 's') rt_retain((const char*)(intptr_t)bits);
    return rt_box(tag, bits);
}
static void rt_dyn_check(const char* fn, const char* file, int line, RtObj* c, RtObj* idx, int64_t* i) {
    if (!c) rt_fail_at(fn, file, line, "null reference: cannot index null");
    char msg[400];
    if (rt_is_box(c, 'A')) {
        RtObj* a = (RtObj*)(intptr_t)((int64_t*)c->data)[0];
        if (a->ek == 'y') rt_fail_at(fn, file, line, "an array<byte> cannot be indexed through dyn; use a variable of type array<byte>");
        if (!rt_is_box(idx, 'i')) { snprintf(msg, sizeof msg, "array index must be int but got %s", rt_obj_type(idx)); rt_fail_at(fn, file, line, msg); }
        *i = ((int64_t*)idx->data)[0];
        if (*i < 0 || *i >= a->len) { snprintf(msg, sizeof msg, "array index %lld is out of range for an array of length %lld", (long long)*i, (long long)a->len); rt_fail_at(fn, file, line, msg); }
    } else if (!rt_is_box(c, 'M')) {
        snprintf(msg, sizeof msg, "cannot index a value of type %s", rt_obj_type(c));
        rt_fail_at(fn, file, line, msg);
    }
}
RtObj* rt_dyn_index(const char* fn, const char* file, int line, RtObj* c, RtObj* idx) {
    int64_t i = 0;
    rt_dyn_check(fn, file, line, c, idx, &i);
    RtObj* a = (RtObj*)(intptr_t)((int64_t*)c->data)[0];
    const int64_t tags = ((int64_t*)c->data)[2];
    char msg[400];
    if (c->cap == 'A') return rt_box_slot((int)(tags & 0xFF), (char*)a->data + (size_t)i * 8);
    const int kt = (int)((tags >> 8) & 0xFF), vt = (int)(tags & 0xFF);
    int64_t key;
    if (!rt_conv_tag(idx, kt, &key)) { snprintf(msg, sizeof msg, "map key: cannot convert %s to %s", rt_obj_type(idx), rt_tag_name(kt)); rt_fail_at(fn, file, line, msg); }
    int64_t at = rt_map_find(a, key);
    if (at < 0) {
        const char* t = rt_key_text(kt, key);
        snprintf(msg, sizeof msg, "key not found: %s", t);
        rt_fail_at(fn, file, line, msg);
    }
    return rt_box_slot(vt, rt_map_slot(a, at));
}
void rt_dyn_setindex(const char* fn, const char* file, int line, RtObj* c, RtObj* idx, RtObj* val, const char* emptyStr) {
    int64_t i = 0, bits;
    rt_dyn_check(fn, file, line, c, idx, &i);
    RtObj* a = (RtObj*)(intptr_t)((int64_t*)c->data)[0];
    const int64_t tags = ((int64_t*)c->data)[2];
    char msg[400];
    const int vt = (int)(tags & 0xFF);
    if (c->cap == 'A') {
        if (!rt_conv_tag(val, vt, &bits)) { snprintf(msg, sizeof msg, "cannot convert %s to %s", rt_obj_type(val), rt_tag_name(vt)); rt_fail_at(fn, file, line, msg); }
        void* slot = (char*)a->data + (size_t)i * 8;
        if (vt == 's') { rt_retain((const char*)(intptr_t)bits); rt_release(*(const char**)slot); }
        rt_store_slot(vt, slot, bits);
        return;
    }
    const int kt = (int)((tags >> 8) & 0xFF);
    int64_t key, v;
    if (!rt_conv_tag(idx, kt, &key)) { snprintf(msg, sizeof msg, "cannot convert %s to %s", rt_obj_type(idx), rt_tag_name(kt)); rt_fail_at(fn, file, line, msg); }
    if (!rt_conv_tag(val, vt, &v)) { snprintf(msg, sizeof msg, "cannot convert %s to %s", rt_obj_type(val), rt_tag_name(vt)); rt_fail_at(fn, file, line, msg); }
    if (kt == 's') rt_retain((const char*)(intptr_t)key);  /* the map keeps its own reference to a new key */
    int64_t at = rt_map_put(a, key, emptyStr);
    void* slot = rt_map_slot(a, at);
    if (vt == 's') { rt_retain((const char*)(intptr_t)v); rt_release(*(const char**)slot); }
    rt_store_slot(vt, slot, v);
}
/* dyn member access and calls: the PVM only knows the built-in members of arrays, maps and strings (docs/language.md, "Obj and dyn") */
RtObj* rt_dyn_member(const char* fn, const char* file, int line, const char* name, RtObj* c) {
    char msg[500];
    if (!c) { snprintf(msg, sizeof msg, "null reference: cannot read property '%s' of null", name); rt_fail_at(fn, file, line, msg); }
    if (strcmp(name, "length") == 0 && c->ek == 'x') {
        if (c->cap == 'A') return rt_box('i', ((RtObj*)(intptr_t)((int64_t*)c->data)[0])->len);
        if (c->cap == 'M') return rt_box('i', rt_map_len((RtObj*)(intptr_t)((int64_t*)c->data)[0]));
        if (c->cap == 's') return rt_box('i', (int64_t)rt_cps((const char*)(intptr_t)((int64_t*)c->data)[0]));
    }
    snprintf(msg, sizeof msg, "%s has no property '%s'", rt_obj_type(c), name);
    rt_fail_at(fn, file, line, msg);
    return NULL;
}
static const char* rt_arr_tname(int tag) {
    static char names[128][24];
    if (!names[tag][0]) snprintf(names[tag], sizeof names[tag], "array<%s>", rt_tag_name(tag));
    return names[tag];
}
RtObj* rt_dyn_invoke(const char* fn, const char* file, int line, const char* name, int32_t argc, RtObj* c, RtObj* a0) {
    char msg[500];
    if (!c) { snprintf(msg, sizeof msg, "null reference: cannot call '%s' on null", name); rt_fail_at(fn, file, line, msg); }
    const int isA = rt_is_box(c, 'A'), isM = rt_is_box(c, 'M');
    RtObj* o = isA || isM ? (RtObj*)(intptr_t)((int64_t*)c->data)[0] : NULL;
    const int64_t tags = isA || isM ? ((int64_t*)c->data)[2] : 0;
    if (isA && o->ek == 'y') rt_fail_at(fn, file, line, "methods of an array<byte> cannot be called through dyn; use a variable of type array<byte>");
#define RT_EXPECT(n) do { if (argc != (n)) { snprintf(msg, sizeof msg, "method '%s' of %s expects %d argument(s) but got %d", name, rt_obj_type(c), (int)(n), (int)argc); rt_fail_at(fn, file, line, msg); } } while (0)
    const int vt = (int)(tags & 0xFF), kt = (int)((tags >> 8) & 0xFF);
    int64_t bits;
    if (isA && !strcmp(name, "push")) {
        RT_EXPECT(1);
        if (!rt_conv_tag(a0, vt, &bits)) { snprintf(msg, sizeof msg, "cannot convert %s to %s", rt_obj_type(a0), rt_tag_name(vt)); rt_fail_at(fn, file, line, msg); }
        void* slot = rt_arr_push_slot(o);
        if (vt == 's') { rt_retain((const char*)(intptr_t)bits); *(int64_t*)slot = bits; } else rt_store_slot(vt, slot, bits);
        return NULL;
    }
    if (isA && !strcmp(name, "copy")) {
        RT_EXPECT(0);
        RtObj* n = rt_arr_copy(o);
        rt_tmp_root = n;
        RtObj* r = rt_box_ref('A', n, (const char*)(intptr_t)((int64_t*)c->data)[1], (int32_t)tags);
        rt_tmp_root = NULL;
        return r;
    }
    if (isM && (!strcmp(name, "containsKey") || !strcmp(name, "get") || !strcmp(name, "remove"))) {
        RT_EXPECT(1);
        if (!rt_conv_tag(a0, kt, &bits)) { snprintf(msg, sizeof msg, "cannot convert %s to %s", rt_obj_type(a0), rt_tag_name(kt)); rt_fail_at(fn, file, line, msg); }
        if (!strcmp(name, "remove")) return rt_box('b', rt_map_del(o, bits) != 0);
        int64_t at = rt_map_find(o, bits);
        if (name[0] == 'c') return rt_box('b', at >= 0);
        return at < 0 ? NULL : rt_box_slot(vt, rt_map_slot(o, at));
    }
    if (isM && (!strcmp(name, "keys") || !strcmp(name, "values"))) {
        RT_EXPECT(0);
        const int keys = name[0] == 'k', et = keys ? kt : vt;
        const int ek = et == 'l' ? 'l' : et == 'd' ? 'd' : et == 's' ? 's' : et == 'y' ? 'y' : 'i';
        RtObj* n = rt_map_collect(o, keys ? 0 : 1, (uint32_t)ek);
        rt_tmp_root = n;
        RtObj* r = rt_box_ref('A', n, rt_arr_tname(et), et);
        rt_tmp_root = NULL;
        return r;
    }
    snprintf(msg, sizeof msg, "%s has no method '%s'", rt_obj_type(c), name);
    rt_fail_at(fn, file, line, msg);
    return NULL;
}
/* ---- host functions (the native subset): same results and error texts as the PVM's host table ---- */
static void rt_hfail(const char* fn, const char* file, int line, const char* host, const char* what) {
    char* msg = (char*)malloc(strlen(host) + strlen(what) + 40);
    sprintf(msg, "host function '%s' failed: %s", host, what);
    rt_fail_at(fn, file, line, msg);
}
static const char* rt_newstr(const char* p, size_t n) { char* r = rt_alloc(n + 1); memcpy(r, p, n); r[n] = 0; return r; }
static size_t rt_cps(const char* s) { size_t n = 0; for (const char* e = s + RT_LEN(s); s < e; ++s) if (((unsigned char)*s & 0xC0) != 0x80) ++n; return n; }
static const char* rt_cpat(const char* s, int64_t i) {  /* byte position of code point i (i == length gives the end) */
    for (const char* e = s + RT_LEN(s); s < e; ++s) if (((unsigned char)*s & 0xC0) != 0x80) { if (i == 0) return s; --i; }
    return s;
}
static const char* rt_find(const char* s, size_t sl, const char* f, size_t fl, size_t from) {  /* first occurrence at or after `from`, or NULL */
    if (fl > sl) return NULL;
    for (size_t i = from; i + fl <= sl; ++i) if (memcmp(s + i, f, fl) == 0) return s + i;
    return NULL;
}
int32_t rt_h_text_length(const char* s) { return (int32_t)rt_cps(s); }
const char* rt_h_text_substring(const char* fn, const char* file, int line, const char* s, int32_t start, int32_t len) {
    int64_t n = (int64_t)rt_cps(s);
    char m[160];
    if (start < 0 || start > n) { snprintf(m, sizeof m, "substring start %d is outside the text (length %lld)", start, (long long)n); rt_hfail(fn, file, line, "text.substring", m); }
    if (len < 0) { snprintf(m, sizeof m, "substring length %d is negative", len); rt_hfail(fn, file, line, "text.substring", m); }
    int64_t end = (int64_t)start + len > n ? n : (int64_t)start + len;
    const char* b = rt_cpat(s, start);
    const char* e = rt_cpat(s, end);
    return rt_newstr(b, (size_t)(e - b));
}
int32_t rt_h_text_charAt(const char* fn, const char* file, int line, const char* s, int32_t i) {
    int64_t n = (int64_t)rt_cps(s);
    if (i < 0 || i >= n) { char m[160]; snprintf(m, sizeof m, "charAt index %d is outside the text (length %lld)", i, (long long)n); rt_hfail(fn, file, line, "text.charAt", m); }
    const unsigned char* c = (const unsigned char*)rt_cpat(s, i);
    int k = *c < 0x80 ? 0 : (*c >> 5) == 6 ? 1 : (*c >> 4) == 14 ? 2 : (*c >> 3) == 30 ? 3 : 0;
    int32_t cp = k == 0 ? *c : (*c & (0x3F >> k));
    for (int j = 1; j <= k && c[j]; ++j) cp = (cp << 6) | (c[j] & 0x3F);
    return cp;
}
int32_t rt_h_text_indexOf(const char* s, const char* f) {
    const char* p = rt_find(s, (size_t)RT_LEN(s), f, (size_t)RT_LEN(f), 0);
    if (!p) return -1;
    size_t n = 0;
    for (const char* q = s; q < p; ++q) if (((unsigned char)*q & 0xC0) != 0x80) ++n;
    return (int32_t)n;
}
const char* rt_h_text_replace(const char* fn, const char* file, int line, const char* s, const char* f, const char* t) {
    size_t sl = (size_t)RT_LEN(s), fl = (size_t)RT_LEN(f), tl = (size_t)RT_LEN(t), cap = sl + 16, len = 0, pos = 0;
    if (fl == 0) rt_hfail(fn, file, line, "text.replace", "replace: the text to find is empty");
    char* out = (char*)malloc(cap);
    for (;;) {
        const char* q = rt_find(s, sl, f, fl, pos);
        size_t chunk = q ? (size_t)(q - s) - pos : sl - pos;
        while (len + chunk + tl + 1 > cap) { cap *= 2; out = (char*)realloc(out, cap); }
        memcpy(out + len, s + pos, chunk); len += chunk;
        if (!q) break;
        memcpy(out + len, t, tl); len += tl;
        pos = (size_t)(q - s) + fl;
        if (len > 64u * 1024u * 1024u) { free(out); rt_hfail(fn, file, line, "text.replace", "replace: the result is larger than 64 MiB"); }
    }
    const char* r = rt_newstr(out, len);
    free(out);
    return r;
}
const char* rt_h_text_upper(const char* s) { size_t n = (size_t)RT_LEN(s); char* r = rt_alloc(n + 1); for (size_t i = 0; i <= n; ++i) r[i] = (s[i] >= 'a' && s[i] <= 'z') ? (char)(s[i] - 32) : s[i]; return r; }
const char* rt_h_text_lower(const char* s) { size_t n = (size_t)RT_LEN(s); char* r = rt_alloc(n + 1); for (size_t i = 0; i <= n; ++i) r[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i]; return r; }
const char* rt_h_text_trim(const char* s) {
    const char* b = s;
    const char* e = s + RT_LEN(s);
    while (b < e && (*b == ' ' || *b == '\t' || *b == '\r' || *b == '\n')) ++b;
    while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) --e;
    return rt_newstr(b, (size_t)(e - b));
}
double rt_h_math(const char* fn, const char* file, int line, int32_t op, double x, double y) {
    static const char* names[] = {"sqrt", "sin", "cos", "tan", "asin", "acos", "atan", "exp", "log", "log10", "floor", "ceil", "round", "pow", "atan2"};
    double v = 0;
    switch (op) {
        case 0: v = sqrt(x); break; case 1: v = sin(x); break; case 2: v = cos(x); break; case 3: v = tan(x); break;
        case 4: v = asin(x); break; case 5: v = acos(x); break; case 6: v = atan(x); break; case 7: v = exp(x); break;
        case 8: v = log(x); break; case 9: v = log10(x); break; case 10: v = floor(x); break; case 11: v = ceil(x); break;
        case 12: v = round(x); break; case 13: v = pow(x, y); break; default: return atan2(x, y);
    }
    if (v != v) { char m[96]; snprintf(m, sizeof m, "%s produced a value that is not a number", names[op]); char hn[32]; snprintf(hn, sizeof hn, "math.%s", names[op]); rt_hfail(fn, file, line, hn, m); }
    return v;
}
int64_t rt_h_time_uptimeMs(void) {
#ifdef _WIN32
    static ULONGLONG start = 0;
    ULONGLONG now = GetTickCount64();
    if (!start) start = now;
    return (int64_t)(now - start);
#else
    static int64_t start = -1;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (start < 0) start = now;
    return now - start;
#endif
}
const char* rt_h_time_format(const char* fn, const char* file, int line, int64_t secs, const char* fmt, bool utc) {
    if (RT_LEN(fmt) == 0 || RT_LEN(fmt) > 200) rt_hfail(fn, file, line, "time.format", "the time format must be 1 to 200 characters (strftime codes such as %Y-%m-%d %H:%M:%S)");
    time_t t = (time_t)secs;
    struct tm tmv;
    memset(&tmv, 0, sizeof tmv);
#ifdef _WIN32
    if (utc) gmtime_s(&tmv, &t); else localtime_s(&tmv, &t);
#else
    if (utc) gmtime_r(&t, &tmv); else localtime_r(&t, &tmv);
#endif
    char buf[512];
    size_t n = strftime(buf, sizeof buf, fmt, &tmv);
    return rt_newstr(buf, n);
}
int64_t rt_h_time_unixSeconds(void) { return (int64_t)time(NULL); }
void rt_h_time_sleep(const char* fn, const char* file, int line, int32_t ms) {
    if (ms < 0 || ms > 60000) rt_hfail(fn, file, line, "time.sleep", "sleep takes 0 to 60000 milliseconds");
    fflush(stdout);
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts; ts.tv_sec = ms / 1000; ts.tv_nsec = (long)(ms % 1000) * 1000000L; nanosleep(&ts, NULL);
#endif
}
const char* rt_h_con_readLine(void) {
    fflush(stdout);
    size_t cap = 128, n = 0;
    char* b = (char*)malloc(cap);
    int c;
    while ((c = getc(stdin)) != EOF && c != '\n') { if (n + 2 > cap) { cap *= 2; b = (char*)realloc(b, cap); } b[n++] = (char)c; }
    if (n && b[n - 1] == '\r') --n;
    const char* r = rt_newstr(b, n);
    free(b);
    return r;
}
bool rt_h_con_hasInput(void) { int c = getc(stdin); if (c == EOF) return false; ungetc(c, stdin); return true; }
void rt_h_sys_exit(int32_t code) { fflush(stdout); exit(code & 0xFF); }
const char* rt_h_sys_cwd(void) { char b[4096]; if (!rt_getcwd(b, sizeof b)) b[0] = 0; return rt_copy_str(b); }
const char* rt_h_sys_os(void) {
#ifdef _WIN32
    return rt_copy_str("windows");
#else
    return rt_copy_str("linux");
#endif
}
const char* rt_h_env_get(const char* k) { const char* v = getenv(k); return rt_copy_str(v ? v : ""); }
bool rt_h_env_has(const char* k) { return getenv(k) != NULL; }
static void rt_pchk(const char* fn, const char* file, int line, const char* host, const char* p) {
    if (!RT_LEN(p)) rt_hfail(fn, file, line, host, "the path is empty");
    if ((int64_t)strlen(p) != RT_LEN(p)) rt_hfail(fn, file, line, host, "the path contains a NUL character");
}
static bool rt_is_file(const char* p) { struct stat st; return stat(p, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG; }
static bool rt_is_dir(const char* p) { struct stat st; return stat(p, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR; }
bool rt_h_file_exists(const char* fn, const char* file, int line, const char* p) { rt_pchk(fn, file, line, "file.exists", p); return rt_is_file(p); }
bool rt_h_dir_exists(const char* fn, const char* file, int line, const char* p) { rt_pchk(fn, file, line, "dir.exists", p); return rt_is_dir(p); }
RtObj* rt_h_encoding_toBytes(const char* s) {
    RtObj* o = rt_arr_new('y', RT_LEN(s), NULL);
    if (RT_LEN(s)) memcpy(o->data, s, (size_t)RT_LEN(s));
    return o;
}
/* strict UTF-8 decoding, every broken or overlong sequence, surrogate or value above U+10FFFF becomes U+FFFD (same as the PVM) */
const char* rt_h_encoding_fromBytes(const char* fn, const char* file, int line, RtObj* a) {
    if (!a) rt_fail_at(fn, file, line, "null array reference");
    const unsigned char* in = (const unsigned char*)a->data;
    size_t n = (size_t)a->len, i = 0, len = 0;
    char* out = (char*)malloc(n * 3 + 1);
    while (i < n) {
        const unsigned char b = in[i];
        size_t need = b < 0x80 ? 0 : (b >= 0xC2 && b < 0xE0) ? 1 : (b >= 0xE0 && b < 0xF0) ? 2 : (b >= 0xF0 && b < 0xF5) ? 3 : 99;
        if (need == 99) { memcpy(out + len, "\xEF\xBF\xBD", 3); len += 3; ++i; continue; }
        if (need == 0) { out[len++] = (char)b; ++i; continue; }
        if (i + need >= n) { memcpy(out + len, "\xEF\xBF\xBD", 3); len += 3; ++i; continue; }
        unsigned long cp = need == 1 ? (b & 0x1Fu) : need == 2 ? (b & 0x0Fu) : (b & 0x07u);
        int ok = 1;
        for (size_t k = 1; k <= need; ++k) {
            const unsigned char cb = in[i + k];
            if ((cb & 0xC0) != 0x80) { ok = 0; break; }
            cp = (cp << 6) | (cb & 0x3Fu);
        }
        if (ok && ((need == 2 && cp < 0x800) || (need == 3 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))) ok = 0;
        if (!ok) { memcpy(out + len, "\xEF\xBF\xBD", 3); len += 3; ++i; continue; }
        memcpy(out + len, in + i, need + 1);
        len += need + 1;
        i += need + 1;
    }
    const char* r = rt_newstr(out, len);
    free(out);
    return r;
}
static char* rt_read_all(const char* fn, const char* file, int line, const char* host, const char* p, size_t* outLen) {
    char m[4400];
    rt_pchk(fn, file, line, host, p);
    if (!rt_is_file(p)) { snprintf(m, sizeof m, "cannot read '%s': no such file", p); rt_hfail(fn, file, line, host, m); }
    struct stat st;
    if (stat(p, &st) == 0 && (uint64_t)st.st_size > 64u * 1024u * 1024u) { snprintf(m, sizeof m, "'%s' is larger than the 64 MiB read limit", p); rt_hfail(fn, file, line, host, m); }
    FILE* f = fopen(p, "rb");
    if (!f) { snprintf(m, sizeof m, "cannot open '%s' for reading", p); rt_hfail(fn, file, line, host, m); }
    size_t cap = (size_t)st.st_size + 1;
    char* b = (char*)malloc(cap);
    *outLen = fread(b, 1, cap - 1, f);
    fclose(f);
    return b;
}
const char* rt_h_file_readAll(const char* fn, const char* file, int line, const char* p) {
    size_t n;
    char* b = rt_read_all(fn, file, line, "file.readAll", p, &n);
    const char* r = rt_newstr(b, n);
    free(b);
    return r;
}
RtObj* rt_h_file_readBytes(const char* fn, const char* file, int line, const char* p) {
    size_t n;
    char* b = rt_read_all(fn, file, line, "file.readBytes", p, &n);
    RtObj* o = rt_arr_new('y', (int64_t)n, NULL);
    if (n) memcpy(o->data, b, n);
    free(b);
    return o;
}
void rt_h_file_writeBytes(const char* fn, const char* file, int line, const char* p, RtObj* a, int32_t append) {
    rt_pchk(fn, file, line, append ? "file.appendBytes" : "file.writeBytes", p);
    char m[4400];
    const char* host = append ? "file.appendBytes" : "file.writeBytes";
    if (!a) rt_fail_at(fn, file, line, "null array reference");
    if ((uint64_t)a->len > 64u * 1024u * 1024u) rt_hfail(fn, file, line, host, "refusing to write more than 64 MiB at once");
    FILE* f = fopen(p, append ? "ab" : "wb");
    if (!f) { snprintf(m, sizeof m, "cannot open '%s' for writing", p); rt_hfail(fn, file, line, host, m); }
    int ok = fwrite(a->data, 1, (size_t)a->len, f) == (size_t)a->len && fflush(f) == 0;
    fclose(f);
    if (!ok) { snprintf(m, sizeof m, "failed to write '%s'", p); rt_hfail(fn, file, line, host, m); }
}
int64_t rt_h_file_size(const char* fn, const char* file, int line, const char* p) {
    struct stat st;
    rt_pchk(fn, file, line, "file.size", p);
    if (stat(p, &st) != 0) { char m[4400]; snprintf(m, sizeof m, "cannot get the size of '%s': %s", p, strerror(errno)); rt_hfail(fn, file, line, "file.size", m); }
    return (int64_t)st.st_size;
}
bool rt_h_file_delete(const char* fn, const char* file, int line, const char* p) {
    rt_pchk(fn, file, line, "file.delete", p);
    if (!rt_is_file(p)) return false;
    if (remove(p) != 0) { char m[4400]; snprintf(m, sizeof m, "cannot delete '%s': %s", p, strerror(errno)); rt_hfail(fn, file, line, "file.delete", m); }
    return true;
}
/* random.*: xoshiro256** + splitmix64 + Lemire range reduction, identical to Rng in src/pvm/host.cpp */
static uint64_t rt_rs[4];
static int rt_rs_ready = 0;
static uint64_t rt_rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
static void rt_rand_seed(uint64_t v) {
    for (int i = 0; i < 4; ++i) {
        uint64_t z = (v += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        rt_rs[i] = z ^ (z >> 31);
    }
    rt_rs_ready = 1;
}
static uint64_t rt_rand_next(void) {
    if (!rt_rs_ready) rt_rand_seed(((uint64_t)time(NULL) << 32) ^ (uint64_t)clock() ^ ((uint64_t)(uintptr_t)&rt_rs << 7));
    const uint64_t result = rt_rotl(rt_rs[1] * 5, 7) * 9, t = rt_rs[1] << 17;
    rt_rs[2] ^= rt_rs[0]; rt_rs[3] ^= rt_rs[1]; rt_rs[1] ^= rt_rs[2]; rt_rs[0] ^= rt_rs[3]; rt_rs[2] ^= t; rt_rs[3] = rt_rotl(rt_rs[3], 45);
    return result;
}
static void rt_mul128(uint64_t a, uint64_t b, uint64_t* hi, uint64_t* lo) {
    const uint64_t a0 = a & 0xFFFFFFFFULL, a1 = a >> 32, b0 = b & 0xFFFFFFFFULL, b1 = b >> 32;
    const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFULL) + (p10 & 0xFFFFFFFFULL);
    *lo = (mid << 32) | (p00 & 0xFFFFFFFFULL);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}
void rt_h_random_seed(int32_t v) { rt_rand_seed((uint64_t)(int64_t)v); }
int32_t rt_h_random_nextInt(const char* fn, const char* file, int line, int32_t lo, int32_t hi) {
    if (lo > hi) { char m[100]; snprintf(m, sizeof m, "random range is empty (%d > %d)", lo, hi); rt_hfail(fn, file, line, "random.nextInt", m); }
    const uint64_t n = (uint64_t)((int64_t)hi - (int64_t)lo) + 1ULL;
    uint64_t h, l;
    rt_mul128(rt_rand_next(), n, &h, &l);
    if (l < n) { const uint64_t t = (0ULL - n) % n; while (l < t) rt_mul128(rt_rand_next(), n, &h, &l); }
    return (int32_t)((int64_t)((uint64_t)(int64_t)lo + h));
}
double rt_h_random_nextDouble(void) { return (double)(rt_rand_next() >> 11) * (1.0 / 9007199254740992.0); }
/* rt.*: the machine/user values are the PVM's; the GC and heap counters describe the native heap (different numbers by nature) */
const char* rt_h_rt_arch(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return rt_copy_str("x64");
#elif defined(__aarch64__) || defined(_M_ARM64)
    return rt_copy_str("arm64");
#elif defined(__i386__) || defined(_M_IX86)
    return rt_copy_str("x86");
#else
    return rt_copy_str("unknown");
#endif
}
int32_t rt_h_rt_cpuCount(void) {
#ifdef _WIN32
    SYSTEM_INFO si; GetSystemInfo(&si); return si.dwNumberOfProcessors ? (int32_t)si.dwNumberOfProcessors : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int32_t)n : 1;
#endif
}
int32_t rt_h_rt_memoryBytes(void) {
#ifdef _WIN32
    return -1;
#else
    FILE* f = fopen("/proc/self/statm", "r");
    long long pages = 0, resident = 0;
    int ok = f && fscanf(f, "%lld %lld", &pages, &resident) == 2;
    if (f) fclose(f);
    return ok ? (int32_t)(resident * (long long)sysconf(_SC_PAGESIZE)) : -1;
#endif
}
const char* rt_h_rt_machine(void) {
#ifdef _WIN32
    const char* n = getenv("COMPUTERNAME");
    return rt_copy_str(n ? n : "");
#else
    char buf[256] = {0};
    if (gethostname(buf, sizeof buf - 1) != 0) buf[0] = 0;
    return rt_copy_str(buf);
#endif
}
const char* rt_h_rt_user(void) { const char* n = getenv("USERNAME"); if (!n) n = getenv("USER"); return rt_copy_str(n ? n : ""); }
int32_t rt_h_rt_gcCollections(void) { return (int32_t)rt_gc_count; }
int32_t rt_h_rt_gcFreed(void) { return (int32_t)rt_gc_freed; }
int32_t rt_h_rt_heapObjects(void) { return (int32_t)rt_objects; }
int32_t rt_h_rt_heapElements(void) { return (int32_t)rt_elements; }
int32_t rt_h_proc_pid(void) {
#ifdef _WIN32
    return (int32_t)GetCurrentProcessId();
#else
    return (int32_t)getpid();
#endif
}
static int rt_readable(const char* path) { FILE* f = fopen(path, "rb"); if (!f) return 0; fclose(f); return 1; }
const char* rt_h_proc_which(const char* name) {
    if (!RT_LEN(name)) return rt_copy_str("");
    const char* path = getenv("PATH");
    const char* p = path ? path : "";
#ifdef _WIN32
    const char sep = ';';
    static const char* exts[] = {"", ".exe", ".cmd", ".bat", ".com"};
    const int nexts = 5;
#else
    const char sep = ':';
    static const char* exts[] = {""};
    const int nexts = 1;
#endif
    char* buf = (char*)malloc(strlen(p) + strlen(name) + 32);
    if (strchr(name, '/') || strchr(name, '\\')) {
        for (int i = 0; i < nexts; ++i) { sprintf(buf, "%s%s", name, exts[i]); if (rt_readable(buf)) { const char* r = rt_copy_str(buf); free(buf); return r; } }
        free(buf);
        return rt_copy_str("");
    }
    const char* pos = p;
    for (;;) {
        const char* e = strchr(pos, sep);
        size_t dl = e ? (size_t)(e - pos) : strlen(pos);
        if (dl > 0) {
            for (int i = 0; i < nexts; ++i) {
                memcpy(buf, pos, dl);
                sprintf(buf + dl, "/%s%s", name, exts[i]);
#ifdef _WIN32
                if (rt_readable(buf)) { const char* r = rt_copy_str(buf); free(buf); return r; }
#else
                struct stat st;
                if (access(buf, X_OK) == 0 && stat(buf, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG) { const char* r = rt_copy_str(buf); free(buf); return r; }
#endif
            }
        }
        if (!e) break;
        pos = e + 1;
    }
    free(buf);
    return rt_copy_str("");
}
static int rt_argc = 0;
static char** rt_argv = NULL;
void rt_set_args(int32_t argc, char** argv) { rt_argc = argc; rt_argv = argv; }
const char* rt_h_sys_homeDir(void) { const char* h = getenv("USERPROFILE"); if (!h || !*h) h = getenv("HOME"); return rt_copy_str(h ? h : ""); }
int32_t rt_h_sys_argCount(void) { return rt_argc > 0 ? rt_argc - 1 : 0; }
const char* rt_h_sys_arg(const char* fn, const char* file, int line, int32_t i) {
    if (i < 0 || i >= rt_h_sys_argCount()) { char m[100]; snprintf(m, sizeof m, "argument %d does not exist", i); rt_hfail(fn, file, line, "sys.arg", m); }
    return rt_copy_str(rt_argv[i + 1]);
}
void rt_h_env_set(const char* fn, const char* file, int line, const char* k, const char* v) {
    if (!RT_LEN(k) || strchr(k, '=') || (int64_t)strlen(k) != RT_LEN(k)) {
        char* m = (char*)malloc((size_t)RT_LEN(k) + 60);
        sprintf(m, "invalid environment variable name '%s'", k);
        rt_hfail(fn, file, line, "env.set", m);
    }
#ifdef _WIN32
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}
static void rt_efail(const char* fn, const char* file, int line, const char* host, const char* what, const char* p, int err) {
    char* m = (char*)malloc(strlen(what) + strlen(p) + 120);
    sprintf(m, "%s '%s': %s", what, p, strerror(err));
    rt_hfail(fn, file, line, host, m);
}
void rt_h_file_copy(const char* fn, const char* file, int line, const char* src, const char* dst) {
    rt_pchk(fn, file, line, "file.copy", src);
    rt_pchk(fn, file, line, "file.copy", dst);
    struct stat st;
    if (stat(src, &st) != 0) rt_efail(fn, file, line, "file.copy", "cannot copy", src, errno);
    if ((st.st_mode & S_IFMT) != S_IFREG) rt_efail(fn, file, line, "file.copy", "cannot copy", src, EINVAL);
    FILE* in = fopen(src, "rb");
    if (!in) rt_efail(fn, file, line, "file.copy", "cannot copy", src, errno);
    FILE* out = fopen(dst, "wb");
    if (!out) { int e = errno; fclose(in); rt_efail(fn, file, line, "file.copy", "cannot copy", src, e); }
    char buf[65536];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    int e = errno;
    fclose(in);
    if (fclose(out) != 0) ok = 0;
    if (!ok) rt_efail(fn, file, line, "file.copy", "cannot copy", src, e);
}
void rt_h_file_move(const char* fn, const char* file, int line, const char* src, const char* dst) {
    rt_pchk(fn, file, line, "file.move", src);
    rt_pchk(fn, file, line, "file.move", dst);
#ifdef _WIN32
    if (rt_is_file(dst)) remove(dst);
#endif
    if (rename(src, dst) != 0) rt_efail(fn, file, line, "file.move", "cannot move", src, errno);
}
int64_t rt_h_file_modified(const char* fn, const char* file, int line, const char* p) {
    struct stat st;
    rt_pchk(fn, file, line, "file.modified", p);
    if (stat(p, &st) != 0) rt_efail(fn, file, line, "file.modified", "cannot get the time of", p, errno);
    return (int64_t)st.st_mtime;
}
const char* rt_h_file_fullPath(const char* fn, const char* file, int line, const char* p) {
    rt_pchk(fn, file, line, "file.fullPath", p);
    char out[4200];
#ifdef _WIN32
    if (!_fullpath(out, p, sizeof out)) rt_efail(fn, file, line, "file.fullPath", "cannot resolve", p, errno);
    return rt_copy_str(out);
#else
    char abs[8400];
    if (p[0] == '/') snprintf(abs, sizeof abs, "%s", p);
    else { char cwd[4100]; if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0; snprintf(abs, sizeof abs, "%s/%s", cwd, p); }
    if (realpath(abs, out)) return rt_copy_str(out);
    /* weakly canonical: normalise ./.. lexically, resolve the longest existing prefix, keep the rest */
    char norm[8400];
    size_t nl = 0;
    norm[0] = 0;
    char* save = NULL;
    for (char* c = strtok_r(abs, "/", &save); c; c = strtok_r(NULL, "/", &save)) {
        if (!strcmp(c, ".")) continue;
        if (!strcmp(c, "..")) { while (nl > 0 && norm[nl - 1] != '/') --nl; if (nl > 0) --nl; norm[nl] = 0; continue; }
        nl += (size_t)sprintf(norm + nl, "/%s", c);
    }
    if (nl == 0) { strcpy(norm, "/"); nl = 1; }
    size_t cut = nl;
    char pre[8400];
    for (;;) {
        memcpy(pre, norm, cut); pre[cut] = 0;
        if (cut == 0) { strcpy(pre, "/"); }
        if (realpath(pre, out)) break;
        while (cut > 0 && norm[cut - 1] != '/') --cut;
        if (cut > 0) --cut;
        if (cut == 0) { strcpy(out, "/"); break; }
    }
    if (strcmp(out, "/") == 0) out[0] = 0;
    strcat(out, norm + cut);
    if (p[strlen(p) - 1] == '/' && out[strlen(out) - 1] != '/') strcat(out, "/");
    return rt_copy_str(out);
#endif
}
void rt_h_dir_create(const char* fn, const char* file, int line, const char* p) {
    rt_pchk(fn, file, line, "dir.create", p);
    char b[4200];
    snprintf(b, sizeof b, "%s", p);
    size_t n = strlen(b);
    while (n > 1 && b[n - 1] == '/') b[--n] = 0;
    for (size_t i = 1; i <= n; ++i) {
        if (b[i] != '/' && b[i] != 0) continue;
        char sv = b[i];
        b[i] = 0;
        if (rt_mkdir(b) != 0) { const int e = errno; if (!rt_is_dir(b)) rt_efail(fn, file, line, "dir.create", "cannot create the folder", p, (e == EEXIST && i < n) ? ENOTDIR : e); }
        b[i] = sv;
    }
}
static int rt_rmtree(const char* p) {
    DIR* d = opendir(p);
    if (!d) return errno;
    struct dirent* e;
    int err = 0;
    while (!err && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char* c = (char*)malloc(strlen(p) + strlen(e->d_name) + 2);
        sprintf(c, "%s/%s", p, e->d_name);
        struct stat st;
        if (rt_lstat(c, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR) err = rt_rmtree(c);
        else if (remove(c) != 0) err = errno;
        free(c);
    }
    closedir(d);
    if (!err && rt_rmdir(p) != 0) err = errno;
    return err;
}
bool rt_h_dir_delete(const char* fn, const char* file, int line, const char* p, bool recursive) {
    rt_pchk(fn, file, line, "dir.delete", p);
    if (!rt_is_dir(p)) return false;
    int err = recursive ? rt_rmtree(p) : (rt_rmdir(p) != 0 ? errno : 0);
    if (err) rt_efail(fn, file, line, "dir.delete", "cannot delete the folder", p, err);
    return true;
}
static int rt_cmpstr(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }
const char* rt_h_dir_list(const char* fn, const char* file, int line, const char* p) {
    rt_pchk(fn, file, line, "dir.list", p);
    DIR* d = rt_is_dir(p) ? opendir(p) : NULL;
    if (!d) { char* m = (char*)malloc(strlen(p) + 60); sprintf(m, "cannot list '%s': not a folder", p); rt_hfail(fn, file, line, "dir.list", m); }
    char** names = NULL;
    size_t n = 0, cap = 0, total = 0;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char* c = (char*)malloc(strlen(p) + strlen(e->d_name) + 3);
        sprintf(c, "%s/%s", p, e->d_name);
        const int isdir = rt_is_dir(c);
        free(c);
        char* nm = (char*)malloc(strlen(e->d_name) + 2);
        strcpy(nm, e->d_name);
        if (isdir) strcat(nm, "/");
        if (n == cap) { cap = cap ? cap * 2 : 16; names = (char**)realloc(names, cap * sizeof(char*)); }
        names[n++] = nm;
        total += strlen(nm) + 1;
    }
    closedir(d);
    if (n) qsort(names, n, sizeof(char*), rt_cmpstr);
    char* out = (char*)malloc(total + 1);
    size_t at = 0;
    for (size_t i = 0; i < n; ++i) { if (i) out[at++] = '\n'; size_t l = strlen(names[i]); memcpy(out + at, names[i], l); at += l; free(names[i]); }
    free(names);
    const char* r = rt_newstr(out, at);
    free(out);
    return r;
}
void rt_h_file_write(const char* fn, const char* file, int line, const char* p, const char* text, int32_t append) {
    char m[4400];
    const char* host = append ? "file.append" : "file.writeAll";
    rt_pchk(fn, file, line, host, p);
    if ((size_t)RT_LEN(text) > 64u * 1024u * 1024u) rt_hfail(fn, file, line, host, "refusing to write more than 64 MiB at once");
    FILE* f = fopen(p, append ? "ab" : "wb");
    if (!f) { snprintf(m, sizeof m, "cannot open '%s' for writing", p); rt_hfail(fn, file, line, host, m); }
    size_t n = (size_t)RT_LEN(text);
    int ok = fwrite(text, 1, n, f) == n && fflush(f) == 0;
    fclose(f);
    if (!ok) { snprintf(m, sizeof m, "failed to write '%s'", p); rt_hfail(fn, file, line, host, m); }
}
/* ---- maps: insertion-ordered entries plus an open-addressing index (same observable behavior as the PVM's map) ---- */
static uint64_t rt_hash(int kk, int64_t k) {
    uint64_t h = 1469598103934665603ULL;
    if (kk == 's') { const unsigned char* p = (const unsigned char*)(const char*)k; for (int64_t i = 0, n = RT_LEN((const char*)k); i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; } return h; }
    h = (uint64_t)k * 0x9E3779B97F4A7C15ULL;
    return h ^ (h >> 29);
}
static int rt_key_eq(int kk, int64_t a, int64_t b) { return kk == 's' ? rt_streq((const char*)a, (const char*)b) : a == b; }
static void rt_map_rebuild(RtObj* o) {
    RtMap* m = (RtMap*)o->data;
    int64_t want = 16;
    while (want < (m->n + 1) * 2) want *= 2;
    free(m->tab);
    m->tab = (int32_t*)malloc((size_t)want * sizeof(int32_t));
    if (!m->tab) rt_fail(rt_loc_file, rt_loc_line, "out of memory");
    m->tabcap = want;
    for (int64_t i = 0; i < want; ++i) m->tab[i] = -1;
    const int kk = rt_mkk(o);
    for (int64_t i = 0; i < m->n; ++i) {
        if (!m->alive[i]) continue;
        uint64_t h = rt_hash(kk, m->keys[i]) & (uint64_t)(want - 1);
        while (m->tab[h] != -1) h = (h + 1) & (uint64_t)(want - 1);
        m->tab[h] = (int32_t)i;
    }
}
RtObj* rt_map_new(int32_t kk, int32_t vk) {
    if (rt_objects >= rt_obj_trig || rt_elements >= rt_elem_trig) rt_collect();
    RtObj* o = (RtObj*)calloc(1, sizeof(RtObj));
    RtMap* m = (RtMap*)calloc(1, sizeof(RtMap));
    if (!o || !m) rt_fail(rt_loc_file, rt_loc_line, "out of memory");
    o->ek = 'm';
    o->cap = ((int64_t)kk << 8) | (int64_t)vk;
    o->data = m;
    o->next = rt_head;
    rt_head = o;
    ++rt_objects;
    return o;
}
static void rt_map_free(RtObj* o) {
    RtMap* m = (RtMap*)o->data;
    if (!m) return;
    const int kk = rt_mkk(o), vk = rt_mvk(o);
    for (int64_t i = 0; i < m->n; ++i) {
        if (!m->alive[i]) continue;
        if (kk == 's') rt_release((const char*)m->keys[i]);
        if (vk == 's' && m->vals[i]) rt_release((const char*)m->vals[i]);
    }
    free(m->keys); free(m->vals); free(m->alive); free(m->tab); free(m);
}
int64_t rt_map_find(RtObj* o, int64_t key) {
    RtMap* m = (RtMap*)o->data;
    if (m->tabcap == 0) return -1;
    const int kk = rt_mkk(o);
    uint64_t h = rt_hash(kk, key) & (uint64_t)(m->tabcap - 1);
    for (;;) {
        int32_t t = m->tab[h];
        if (t == -1) return -1;
        if (t >= 0 && m->alive[t] && rt_key_eq(kk, m->keys[t], key)) return t;
        h = (h + 1) & (uint64_t)(m->tabcap - 1);
    }
}
void* rt_map_slot(RtObj* o, int64_t idx) { return &((RtMap*)o->data)->vals[idx]; }
int32_t rt_map_len(RtObj* o) { return (int32_t)((RtMap*)o->data)->live; }
/* Finds or adds the entry for key and returns its index. A string key is passed with one reference that the map keeps (new entry) or releases (existing). */
int64_t rt_map_put(RtObj* o, int64_t key, const char* emptyStr) {
    RtMap* m = (RtMap*)o->data;
    const int kk = rt_mkk(o), vk = rt_mvk(o);
    int64_t found = rt_map_find(o, key);
    if (found >= 0) { if (kk == 's') rt_release((const char*)key); return found; }
    rt_ensure_room(1);  /* may collect: the map and the operands are still in their slots */
    rt_elements += 1;
    o->weight += 1;
    if (m->n == m->cap) {
        int64_t nc = m->cap < 8 ? 8 : m->cap * 2;
        int64_t* nk = (int64_t*)realloc(m->keys, (size_t)nc * 8);
        int64_t* nv = (int64_t*)realloc(m->vals, (size_t)nc * 8);
        char* na = (char*)realloc(m->alive, (size_t)nc);
        if (!nk || !nv || !na) rt_fail(rt_loc_file, rt_loc_line, "out of memory");
        m->keys = nk; m->vals = nv; m->alive = na; m->cap = nc;
    }
    int64_t i = m->n++;
    m->keys[i] = key;
    m->vals[i] = vk == 's' ? (int64_t)emptyStr : 0;
    m->alive[i] = 1;
    ++m->live;
    if ((m->n + 1) * 2 > m->tabcap) rt_map_rebuild(o);
    else {
        uint64_t h = rt_hash(kk, key) & (uint64_t)(m->tabcap - 1);
        while (m->tab[h] >= 0) h = (h + 1) & (uint64_t)(m->tabcap - 1);
        m->tab[h] = (int32_t)i;
    }
    return i;
}
int32_t rt_map_del(RtObj* o, int64_t key) {
    RtMap* m = (RtMap*)o->data;
    int64_t i = rt_map_find(o, key);
    if (i < 0) return 0;
    const int kk = rt_mkk(o), vk = rt_mvk(o);
    uint64_t h = rt_hash(kk, key) & (uint64_t)(m->tabcap - 1);
    while (m->tab[h] != (int32_t)i) h = (h + 1) & (uint64_t)(m->tabcap - 1);
    m->tab[h] = -2;
    if (kk == 's') rt_release((const char*)m->keys[i]);
    if (vk == 's' && m->vals[i]) rt_release((const char*)m->vals[i]);
    m->alive[i] = 0;
    m->vals[i] = 0;
    --m->live;
    if (m->n > 32 && m->n - m->live > m->live) {  /* compaction, as in the PVM */
        int64_t out = 0;
        for (int64_t j = 0; j < m->n; ++j) {
            if (!m->alive[j]) continue;
            m->keys[out] = m->keys[j];
            m->vals[out] = m->vals[j];
            m->alive[out] = 1;
            ++out;
        }
        rt_elements -= m->n - out;
        o->weight -= m->n - out;
        m->n = out;
        rt_map_rebuild(o);
    }
    return 1;
}
/* keys (which = 0) or values (which = 1) as a new array with element kind ek; the map is still rooted by the caller */
RtObj* rt_map_collect(RtObj* o, int32_t which, uint32_t ek) {
    RtMap* m = (RtMap*)o->data;
    RtObj* a = rt_arr_new(ek, m->live, NULL);
    int64_t k = 0;
    for (int64_t i = 0; i < m->n; ++i) {
        if (!m->alive[i]) continue;
        int64_t v = which == 0 ? m->keys[i] : m->vals[i];
        if (ek == 'y') ((unsigned char*)a->data)[k] = (unsigned char)v;
        else ((int64_t*)a->data)[k] = v;
        if (ek == 's') rt_retain((const char*)v);
        ++k;
    }
    return a;
}
/* shallow copy of a class instance or a map (op ocopy); the source stays rooted in the caller's slot */
RtObj* rt_ocopy(const char* fn, const char* file, int line, RtObj* src, const char* emptyStr) {
    if (!src) rt_fail_at(fn, file, line, "null reference: cannot copy a null value");
    if (src->ek == 'm') {
        RtObj* dst = rt_map_new(rt_mkk(src), rt_mvk(src));
        rt_tmp_root = dst;
        RtMap* sm = (RtMap*)src->data;
        const int kk = rt_mkk(src), vk = rt_mvk(src);
        for (int64_t i = 0; i < sm->n; ++i) {
            if (!sm->alive[i]) continue;
            if (kk == 's') rt_retain((const char*)sm->keys[i]);
            int64_t at = rt_map_put(dst, sm->keys[i], emptyStr);
            RtMap* dm = (RtMap*)dst->data;
            if (vk == 's') { rt_release((const char*)dm->vals[at]); rt_retain((const char*)sm->vals[i]); }
            dm->vals[at] = sm->vals[i];
        }
        rt_tmp_root = NULL;
        return dst;
    }
    if (src->ek == 'x') {
        int64_t bits = ((int64_t*)src->data)[0];
        if (src->cap == 's') rt_retain((const char*)(intptr_t)bits);
        return rt_box((int32_t)src->cap, bits);
    }
    RtObj* o = rt_obj_new((int32_t)src->cap, src->len, emptyStr);
    const char* kinds = rt_class_kinds[src->cap];
    for (int64_t i = 0; i < src->len; ++i) {
        int64_t v = ((int64_t*)src->data)[i];
        if (kinds[i] == 's') { rt_release(((const char**)o->data)[i]); rt_retain((const char*)v); }
        ((int64_t*)o->data)[i] = v;
    }
    return o;
}
void rt_fail_key(const char* fn, const char* file, int line, int kind, int64_t key) {
    char* msg = (char*)malloc(strlen(kind == 's' ? (const char*)key : "") + 64);
    if (kind == 's') sprintf(msg, "key not found: %s", (const char*)key);
    else if (kind == 'b') sprintf(msg, "key not found: %s", key ? "true" : "false");
    else if (kind == 'c') { const char* t = rt_str_chr((int32_t)key); sprintf(msg, "key not found: %s", t); }
    else sprintf(msg, "key not found: %lld", (long long)key);
    rt_fail_at(fn, file, line, msg);
}
/* ---- strict parsing, as in the PVM (ops.cpp) ---- */
int32_t rt_parse_int(const char* s, int32_t* ok) {
    size_t i = 0, n = (size_t)RT_LEN(s);
    int neg = 0;
    *ok = 0;
    if (i < n && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; ++i; }
    if (i == n || n - i > 10) return 0;
    long long v = 0;
    for (; i < n; ++i) { if (s[i] < '0' || s[i] > '9') return 0; v = v * 10 + (s[i] - '0'); }
    if (neg) v = -v;
    if (v < -2147483648LL || v > 2147483647LL) return 0;
    *ok = 1;
    return (int32_t)v;
}
int64_t rt_parse_lng(const char* s, int32_t* ok) {
    size_t i = 0, n = (size_t)RT_LEN(s);
    int neg = 0;
    *ok = 0;
    if (i < n && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; ++i; }
    if (i == n) return 0;
    unsigned long long v = 0;
    const unsigned long long limit = neg ? 9223372036854775808ULL : 9223372036854775807ULL;
    for (; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') return 0;
        const unsigned long long d = (unsigned long long)(s[i] - '0');
        if (v > (limit - d) / 10) return 0;
        v = v * 10 + d;
    }
    *ok = 1;
    return neg ? (int64_t)(0ULL - v) : (int64_t)v;
}
double rt_parse_dbl(const char* s, int32_t* ok) {
    size_t n = (size_t)RT_LEN(s);
    *ok = 0;
    if (n == 0) return 0;
    for (size_t i = 0; i < n; ++i) { char c = s[i]; if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E')) return 0; }
    char* end = NULL;
    double v = strtod(s, &end);
    if (end != s + n || isinf(v) || isnan(v)) return 0;
    *ok = 1;
    return v;
}
void rt_fail_parse(const char* fn, const char* file, int line, int kind, const char* text) {
    char* msg = (char*)malloc(strlen(text) + 40);
    sprintf(msg, "cannot convert \"%s\" to %s", text, kind == 1 ? "int" : kind == 2 ? "long" : "double");
    rt_fail_at(fn, file, line, msg);
}
/* Grows the array by one element and returns the address of the new slot; the caller stores the value (the array and the value stay rooted). */
void* rt_arr_push_slot(RtObj* o) {
    int64_t need = o->len + 1;
    if (o->ek == 'y') {
        int64_t w = (need + 15) / 16;
        if (w > o->weight) { rt_ensure_room(w - o->weight); rt_elements += w - o->weight; o->weight = w; }
    } else {
        rt_ensure_room(1);
        rt_elements += 1;
        o->weight += 1;
    }
    if (need > o->cap) {
        int64_t nc = o->cap < 4 ? 4 : o->cap * 2;
        void* nd = realloc(o->data, (size_t)nc * rt_stride(o->ek));
        if (!nd) rt_fail(rt_loc_file, rt_loc_line, "out of memory");
        o->data = nd;
        o->cap = nc;
    }
    o->len = need;
    return (char*)o->data + (size_t)(need - 1) * rt_stride(o->ek);
}
RtObj* rt_arr_copy(RtObj* src) {
    RtObj* o = rt_arr_new(src->ek, src->len, NULL);  /* src is still rooted by its operand-stack slot */
    if (src->len > 0) memcpy(o->data, src->data, (size_t)src->len * rt_stride(src->ek));
    if (src->ek == 's') for (int64_t i = 0; i < o->len; ++i) rt_retain(((const char**)o->data)[i]);
    return o;
}
int main_flush(void) { return fflush(stdout); }
)RT";

struct Unsupported {
    std::string message;
};

char kindOf(const Type& t, const std::string& what);
// Element kind of an array of `e`: 'i' (int, char, boolean: 32-bit slots), 'l', 'd', 's', 'r' (nested array) or 'y' (compact bytes).
char arrayElemKind(const Type& e, const std::string& what) {
    switch (e.kind) {
        case Type::Int: case Type::Char: case Type::Bool: return 'i';
        case Type::Long: return 'l';
        case Type::Double: return 'd';
        case Type::String: return 's';
        case Type::Byte: return 'y';
        case Type::Array: kindOf(e, what); return 'r';
        case Type::Class: return 'r';
        case Type::Nullable: case Type::Map: case Type::Fn: case Type::Obj: kindOf(e, what); return 'r';
        default: throw Unsupported{what + " has type array<" + pvm::typeName(e) + ">, which the LLVM backend does not support (use the PVM)"};
    }
}
char kindOf(const Type& t, const std::string& what) {
    switch (t.kind) {
        case Type::Int: return 'i';
        case Type::Bool: return 'b';
        case Type::String: return 's';
        case Type::Long: return 'l';
        case Type::Byte: return 'y';
        case Type::Double: return 'd';
        case Type::Char: return 'c';
        case Type::Array: arrayElemKind(*t.elem, what); return 'r';
        case Type::Class: return 'r';
        case Type::Obj: return 'r';  // a boxed string or a class instance (see docs/llvm.md); other boxes are refused where they are made
        case Type::Fn:
            for (const Type& p : *t.params) kindOf(p, what);
            if (t.elem->kind != Type::Void) kindOf(*t.elem, what);
            return 'r';
        case Type::Map: {
            const Type::Kind kk = t.key->kind;
            if (kk != Type::Int && kk != Type::Char && kk != Type::Bool && kk != Type::Long && kk != Type::Byte && kk != Type::String)
                throw Unsupported{what + " has the map type " + pvm::typeName(t) + ", whose key type the LLVM backend does not support (use the PVM)"};
            kindOf(*t.elem, what);
            return 'r';
        }
        case Type::Nullable:  // a reference (class, array, map) is null itself; a scalar or string is a one-element heap box (null = null pointer)
            if (t.elem->kind == Type::Obj || t.elem->kind == Type::Nullable || t.elem->kind == Type::Fn || t.elem->kind == Type::Void || t.elem->kind == Type::Guid)
                throw Unsupported{what + " has the nullable type " + pvm::typeName(t) + ", which the LLVM backend does not support yet (use the PVM)"};
            kindOf(*t.elem, what);
            return 'r';
        default: throw Unsupported{what + " has type " + pvm::typeName(t) + ", which the LLVM backend does not support (use the PVM)"};
    }
}
const char* llType(char k) { return k == 'i' || k == 'y' || k == 'c' ? "i32" : k == 'b' ? "i1" : k == 'l' ? "i64" : k == 'd' ? "double" : "ptr"; }
std::string dblLit(double v) {
    unsigned long long bits;
    std::memcpy(&bits, &v, sizeof bits);
    char b[32];
    std::snprintf(b, sizeof b, "0x%016llX", bits);
    return b;
}

// Tag of a scalar or string type as stored in an obj box (0 when the type cannot be boxed element-wise).
int boxTag(const Type& t) {
    switch (t.kind) {
        case Type::Int: return 'i'; case Type::Long: return 'l'; case Type::Byte: return 'y'; case Type::Bool: return 'b';
        case Type::Char: return 'c'; case Type::Double: return 'd'; case Type::String: return 's'; default: return 0;
    }
}
// 'A' / 'M' when t is an array or map whose elements (and keys) are scalars or strings: such a value can live in an obj box.
int refBoxTag(const Type& t) {
    if (t.kind == Type::Array && boxTag(*t.elem)) return 'A';
    if (t.kind == Type::Map && boxTag(*t.elem) && boxTag(*t.key) && t.key->kind != Type::Double) return 'M';
    return 0;
}

class Gen {
public:
    explicit Gen(const pvm::Module& m) : m_(m) {}

    std::string run(std::vector<std::string>& problems) {
        for (const pvm::Function& f : m_.funcs) {
            try {
                emitFunction(f);
            } catch (const Unsupported& u) {
                problems.push_back(u.message);
            }
        }
        if (!problems.empty()) return "";
        std::ostringstream o;
        o << "; generated by coyote-llvm " COYOTE_VERSION " from " << m_.source << "\n";
        o << "target triple = \"\"\n\n";
        o << "declare void @rt_fail(ptr, i32, ptr) noreturn\n"
          << "declare void @rt_raise(ptr, ptr, ptr, ptr, ptr, i32) noreturn\ndeclare void @rt_try_push(ptr)\ndeclare void @rt_try_pop()\ndeclare ptr @rt_handlers_get()\ndeclare void @rt_handlers_set(ptr)\n"
          << "declare ptr @rt_ocopy(ptr, ptr, i32, ptr, ptr)\ndeclare i32 @rt_h_text_length(ptr)\ndeclare ptr @rt_h_text_substring(ptr, ptr, i32, ptr, i32, i32)\ndeclare i32 @rt_h_text_charAt(ptr, ptr, i32, ptr, i32)\ndeclare i32 @rt_h_text_indexOf(ptr, ptr)\n"
          << "declare ptr @rt_h_text_replace(ptr, ptr, i32, ptr, ptr, ptr)\ndeclare ptr @rt_h_text_upper(ptr)\ndeclare ptr @rt_h_text_lower(ptr)\ndeclare ptr @rt_h_text_trim(ptr)\n"
          << "declare double @rt_h_math(ptr, ptr, i32, i32, double, double)\ndeclare i64 @rt_h_time_uptimeMs()\ndeclare i64 @rt_h_time_unixSeconds()\ndeclare void @rt_h_time_sleep(ptr, ptr, i32, i32)\n"
          << "declare ptr @rt_h_con_readLine()\ndeclare zeroext i1 @rt_h_con_hasInput()\ndeclare void @rt_h_sys_exit(i32)\ndeclare ptr @rt_h_sys_cwd()\ndeclare ptr @rt_h_sys_os()\n"
          << "declare ptr @rt_h_env_get(ptr)\ndeclare zeroext i1 @rt_h_env_has(ptr)\ndeclare zeroext i1 @rt_h_file_exists(ptr, ptr, i32, ptr)\ndeclare zeroext i1 @rt_h_dir_exists(ptr, ptr, i32, ptr)\ndeclare ptr @rt_h_file_readAll(ptr, ptr, i32, ptr)\n"
          << "declare void @rt_h_file_write(ptr, ptr, i32, ptr, ptr, i32)\ndeclare ptr @rt_box(i32, i64)\ndeclare zeroext i1 @rt_istype(ptr, i32, i32)\ndeclare i64 @rt_unbox_bits(ptr, ptr, i32, ptr, i32, ptr)\ndeclare zeroext i1 @rt_obj_eq(ptr, ptr)\ndeclare ptr @rt_mouth_sys(i32, ptr)\ndeclare void @rt_set_exit(ptr, ptr, i32, ptr)\ndeclare i32 @rt_exit_status()\ndeclare ptr @rt_h_file_readBytes(ptr, ptr, i32, ptr)\ndeclare void @rt_h_file_writeBytes(ptr, ptr, i32, ptr, ptr, i32)\ndeclare i64 @rt_h_file_size(ptr, ptr, i32, ptr)\ndeclare zeroext i1 @rt_h_file_delete(ptr, ptr, i32, ptr)\ndeclare ptr @rt_h_encoding_toBytes(ptr)\ndeclare ptr @rt_h_encoding_fromBytes(ptr, ptr, i32, ptr)\ndeclare ptr @rt_h_time_format(ptr, ptr, i32, i64, ptr, i1 zeroext)\ndeclare ptr @rt_err_msg()\ndeclare ptr @rt_box_str(ptr)\ndeclare ptr @rt_mouth_str(i32)\ndeclare ptr @rt_mouth_payload()\n"
          << "declare ptr @rt_unbox_class(ptr, ptr, i32, ptr, i32, i32)\ndeclare ptr @rt_unbox_str(ptr, ptr, i32, ptr)\ndeclare ptr @rt_obj_str(ptr)\n"
          << "declare i32 @llvm.eh.sjlj.setjmp(ptr)\ndeclare ptr @llvm.frameaddress.p0(i32)\ndeclare ptr @llvm.stacksave.p0()\n"
          << "declare void @rt_fail_at(ptr, ptr, i32, ptr) noreturn\ndeclare void @rt_trace(i32, ptr, ptr, i32)\n"
          << "declare void @rt_print(ptr)\ndeclare void @rt_println(ptr)\n"
          << "declare zeroext i1 @rt_streq(ptr, ptr)\ndeclare ptr @rt_concat(ptr, ptr)\ndeclare ptr @rt_str_int(i32)\n"
          << "declare void @rt_retain(ptr)\ndeclare void @rt_release(ptr)\n"
          << "@rt_depth = external global i32\n"
          << "declare i32 @main_flush()\n"
          << "declare {i32, i1} @llvm.sadd.with.overflow.i32(i32, i32)\n"
          << "declare {i32, i1} @llvm.ssub.with.overflow.i32(i32, i32)\n"
          << "declare {i32, i1} @llvm.smul.with.overflow.i32(i32, i32)\n"
          << "declare {i64, i1} @llvm.sadd.with.overflow.i64(i64, i64)\n"
          << "declare {i64, i1} @llvm.ssub.with.overflow.i64(i64, i64)\n"
          << "declare {i64, i1} @llvm.smul.with.overflow.i64(i64, i64)\n"
          << "declare ptr @rt_str_lng(i64)\ndeclare ptr @rt_str_dbl(double)\ndeclare ptr @rt_str_chr(i32)\n"
          << "declare i32 @rt_str_len(ptr)\ndeclare i32 @rt_str_at(ptr, i32)\ndeclare i32 @rt_strcmp(ptr, ptr)\n"
          << "declare void @rt_fail_cvt(ptr, ptr, i32, i32, i64, double, i64) noreturn\n"
          << "declare ptr @rt_arr_new(i32, i64, ptr)\ndeclare ptr @rt_arr_push_slot(ptr)\ndeclare ptr @rt_arr_copy(ptr)\n"
          << "declare void @rt_frame_push(ptr, ptr, i64)\ndeclare void @rt_frame_pop(ptr)\n"
          << "declare ptr @rt_dyn_member(ptr, ptr, i32, ptr, ptr)\ndeclare ptr @rt_dyn_invoke(ptr, ptr, i32, ptr, i32, ptr, ptr)\n"
          << "declare i32 @rt_h_proc_pid()\ndeclare ptr @rt_h_proc_which(ptr)\n"
          << "declare ptr @rt_h_rt_arch()\ndeclare i32 @rt_h_rt_cpuCount()\ndeclare i32 @rt_h_rt_memoryBytes()\ndeclare ptr @rt_h_rt_machine()\ndeclare ptr @rt_h_rt_user()\ndeclare i32 @rt_h_rt_gcCollections()\ndeclare i32 @rt_h_rt_gcFreed()\ndeclare i32 @rt_h_rt_heapObjects()\ndeclare i32 @rt_h_rt_heapElements()\n"
          << "declare void @rt_h_random_seed(i32)\ndeclare i32 @rt_h_random_nextInt(ptr, ptr, i32, i32, i32)\ndeclare double @rt_h_random_nextDouble()\n"
          << "declare ptr @rt_h_sys_homeDir()\ndeclare void @rt_set_args(i32, ptr)\ndeclare i32 @rt_h_sys_argCount()\ndeclare ptr @rt_h_sys_arg(ptr, ptr, i32, i32)\ndeclare void @rt_h_env_set(ptr, ptr, i32, ptr, ptr)\n"
          << "declare void @rt_h_file_copy(ptr, ptr, i32, ptr, ptr)\ndeclare void @rt_h_file_move(ptr, ptr, i32, ptr, ptr)\ndeclare i64 @rt_h_file_modified(ptr, ptr, i32, ptr)\ndeclare ptr @rt_h_file_fullPath(ptr, ptr, i32, ptr)\n"
          << "declare void @rt_h_dir_create(ptr, ptr, i32, ptr)\ndeclare zeroext i1 @rt_h_dir_delete(ptr, ptr, i32, ptr, i1 zeroext)\ndeclare ptr @rt_h_dir_list(ptr, ptr, i32, ptr)\n"
          << "declare ptr @rt_box_ref(i32, ptr, ptr, i32)\ndeclare zeroext i1 @rt_istype_ref(ptr, i32, ptr)\ndeclare ptr @rt_unbox_ref(ptr, ptr, i32, ptr, i32, ptr)\n"
          << "declare ptr @rt_dyn_bin(ptr, ptr, i32, i32, ptr, ptr)\ndeclare ptr @rt_dyn_un(ptr, ptr, i32, i32, ptr)\ndeclare ptr @rt_dyn_index(ptr, ptr, i32, ptr, ptr)\n"
          << "declare void @rt_dyn_setindex(ptr, ptr, i32, ptr, ptr, ptr, ptr)\n"
          << "declare ptr @rt_map_new(i32, i32)\ndeclare i64 @rt_map_find(ptr, i64)\ndeclare ptr @rt_map_slot(ptr, i64)\ndeclare i32 @rt_map_len(ptr)\n"
          << "declare i64 @rt_map_put(ptr, i64, ptr)\ndeclare i32 @rt_map_del(ptr, i64)\ndeclare ptr @rt_map_collect(ptr, i32, i32)\n"
          << "declare void @rt_fail_key(ptr, ptr, i32, i32, i64) noreturn\ndeclare void @rt_fail_parse(ptr, ptr, i32, i32, ptr) noreturn\n"
          << "declare i32 @rt_parse_int(ptr, ptr)\ndeclare i64 @rt_parse_lng(ptr, ptr)\ndeclare double @rt_parse_dbl(ptr, ptr)\n"
          << "declare ptr @rt_obj_new(i32, i64, ptr)\ndeclare i32 @rt_isinst(ptr, i32)\ndeclare void @rt_downcast(ptr, ptr, i32, ptr, i32)\n"
          << "@rt_loc_file = external global ptr\n@rt_loc_line = external global i32\n\n";
        // String constants carry the reference-count header (immortal) in front of their characters.
        const char* kImmortal = "4611686018427387904";
        o << "@.true = private unnamed_addr constant { i64, i64, [5 x i8] } { i64 4, i64 " << kImmortal << ", [5 x i8] c\"true\\00\" }\n"
          << "@.false = private unnamed_addr constant { i64, i64, [6 x i8] } { i64 5, i64 " << kImmortal << ", [6 x i8] c\"false\\00\" }\n"
          << "@.empty = private unnamed_addr constant { i64, i64, [1 x i8] } { i64 0, i64 " << kImmortal << ", [1 x i8] zeroinitializer }\n";
        for (size_t i = 0; i < pool_.size(); ++i) {
            o << "@.s" << i << " = private unnamed_addr constant { i64, i64, [" << pool_[i].size() + 1 << " x i8] } { i64 " << pool_[i].size() << ", i64 " << kImmortal << ", [" << pool_[i].size() + 1 << " x i8] c\"";
            for (unsigned char c : pool_[i]) {
                if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') o << c;
                else { char b[8]; std::snprintf(b, sizeof b, "\\%02X", c); o << b; }
            }
            o << "\\00\" }\n";
        }
        // Class tables for the runtime: field kinds (for the collector and the defaults), names and the is-a matrix.
        {
            const size_t n = m_.classes.size();
            // closure objects reuse the class machinery: slot 0 is the code pointer ('p'), then the captured values
            const size_t nc = closureKinds_.size();
            for (size_t c = 0; c < nc; ++c)
                o << "@.ck" << n + c << " = private constant [" << closureKinds_[c].size() + 1 << " x i8] c\"" << closureKinds_[c] << "\\00\"\n";
            for (size_t c = 0; c < n; ++c) {
                std::string kinds, nm = m_.classes[c].name;
                for (const auto& f : m_.classes[c].fields) kinds += kindOf(f.type, "a field of class " + nm);
                o << "@.ck" << c << " = private constant [" << kinds.size() + 1 << " x i8] c\"" << kinds << "\\00\"\n"
                  << "@.cn" << c << " = private constant [" << nm.size() + 1 << " x i8] c\"" << nm << "\\00\"\n";
            }
            o << "@rt_class_count = constant i32 " << n << "\n";
            o << "@rt_class_kinds = constant [" << n + nc << " x ptr] [";
            for (size_t c = 0; c < n + nc; ++c) o << (c ? ", " : "") << "ptr @.ck" << c;
            o << "]\n@rt_class_names = constant [" << n << " x ptr] [";
            for (size_t c = 0; c < n; ++c) o << (c ? ", " : "") << "ptr @.cn" << c;
            o << "]\n@rt_isa = constant [" << n * n << " x i8] [";
            for (size_t a = 0; a < n; ++a)
                for (size_t b = 0; b < n; ++b) o << (a || b ? ", " : "") << "i8 " << (m_.classes[a].isa[b] ? 1 : 0);
            o << "]\n";
        }
        // file-level variables
        {
            std::vector<std::string> roots;
            for (size_t g = 0; g < m_.globals.size(); ++g) {
                const char k = kindOf(m_.globals[g].type, "the file-level variable " + m_.globals[g].name);
                const std::string init = k == 's' ? "getelementptr inbounds (i8, ptr @.empty, i64 16)" : k == 'r' ? "null" : k == 'b' ? "false" : k == 'd' ? "0.0" : "0";
                o << "@g." << g << " = global " << llType(k) << " " << init << "\n";
                if (k == 'r') roots.push_back("ptr @g." + std::to_string(g));
            }
            o << "@rt_groots_n = constant i32 " << roots.size() << "\n@rt_groots = constant [" << roots.size() << " x ptr] [";
            for (size_t i = 0; i < roots.size(); ++i) o << (i ? ", " : "") << roots[i];
            o << "]\n";
        }
        o << "\n" << body_.str() << thunks_;
        o << "define i32 @main(i32 %argc, ptr %argv) {\n  call void @rt_set_args(i32 %argc, ptr %argv)\n  call void @\"f." << m_.entry << "\"()\n  call i32 @main_flush()\n  %ec = call i32 @rt_exit_status()\n  ret i32 %ec\n}\n";
        return o.str();
    }

private:
    const pvm::Module& m_;
    std::ostringstream body_;
    std::vector<std::string> pool_;
    std::map<std::string, size_t> poolIndex_;
    int tmp_ = 0;
    std::vector<std::string> closureKinds_;  // per closure target: "p" + the kinds of the captured values
    std::map<int, int> closureId_;           // function index -> closure number
    std::string thunks_;                     // generated entry points that unpack a closure and call its function

    std::string str(const std::string& s) {
        auto it = poolIndex_.find(s);
        if (it == poolIndex_.end()) {
            it = poolIndex_.emplace(s, pool_.size()).first;
            pool_.push_back(s);
        }
        return cstr("@.s" + std::to_string(it->second));
    }
    // The characters of a constant string start after its 8-byte count.
    static std::string cstr(const std::string& global) { return "getelementptr inbounds (i8, ptr " + global + ", i64 16)"; }
    std::string t() { return "%t" + std::to_string(tmp_++); }

    void emitFunction(const pvm::Function& f) {
        const std::string where = "function '" + f.name + "'";
        std::vector<char> slotKinds;
        std::vector<Type> slotTypes;
        for (const Type& p : f.sig.params) { slotKinds.push_back(kindOf(p, "a parameter of " + where)); slotTypes.push_back(p); }
        for (const Type& l : f.locals) { slotKinds.push_back(kindOf(l, "a local of " + where)); slotTypes.push_back(l); }
        char retKind = 0;
        if (f.sig.ret.kind != Type::Void) retKind = kindOf(f.sig.ret, "the return value of " + where);

        std::ostringstream code;  // instructions after the entry-block allocas
        std::set<std::pair<int, char>> slots;
        std::vector<char> stack;
        std::vector<Type> tys;  // parallel to `stack`: the full type of a reference (array) entry, so element kinds are known
        Type lastTy;            // type of the entry most recently popped
        std::map<size_t, std::pair<std::vector<char>, std::vector<Type>>> stateAt;  // jump target -> stack kinds and types on arrival
        std::set<size_t> targets;
        bool hasTry = false;
        int nTry = 0;
        for (const pvm::Instr& in : f.code) {
            if (in.op == Op::Jump || in.op == Op::JumpFalse || in.op == Op::Try) targets.insert(static_cast<size_t>(in.target));
            if (in.op == Op::Try) hasTry = true;
        }
        // After a longjmp the compiler must not reuse values it believes it knows: locals are volatile in functions that execute `try`.
        const char* vol = hasTry ? "volatile " : "";
        std::string tryAllocas;

        auto push = [&](char k, const std::string& v, const Type& ty = Type()) {
            slots.insert({static_cast<int>(stack.size()), k});
            tys.push_back(ty);
            code << "  store " << llType(k) << " " << v << ", ptr %stk." << stack.size() << "." << k << "\n";
            stack.push_back(k);
        };
        auto pop = [&](char k) {
            stack.pop_back();
            lastTy = tys.back();
            tys.pop_back();
            slots.insert({static_cast<int>(stack.size()), k});
            std::string r = t();
            code << "  " << r << " = load " << llType(k) << ", ptr %stk." << stack.size() << "." << k << "\n";
            if (k == 'r') code << "  store ptr null, ptr %stk." << stack.size() << ".r\n";
            return r;
        };
        auto retain = [&](const std::string& v) { code << "  call void @rt_retain(ptr " << v << ")\n"; };
        auto release = [&](const std::string& v) { code << "  call void @rt_release(ptr " << v << ")\n"; };
        auto releaseLocals = [&]() {
            for (size_t i = 0; i < slotKinds.size(); ++i)
                if (slotKinds[i] == 's') { std::string r = t(); code << "  " << r << " = load " << vol << "ptr, ptr %loc." << i << "\n"; release(r); }
        };
        auto fail = [&](const pvm::Instr& in, const std::string& msg) {
            code << "  call void @rt_fail_at(ptr " << str(f.name == "$main" ? "<main>" : f.name) << ", ptr " << str(m_.fileName(in.srcFile)) << ", i32 " << in.srcLine << ", ptr " << str(msg)
                 << ")\n  unreachable\n";
        };
        auto checked = [&](const pvm::Instr& in, const char* intrinsic, const std::string& a, const std::string& b) {
            std::string r = t(), v = t(), o = t();
            int id = tmp_++;
            code << "  " << r << " = call {i32, i1} @llvm.s" << intrinsic << ".with.overflow.i32(i32 " << a << ", i32 " << b << ")\n"
                 << "  " << v << " = extractvalue {i32, i1} " << r << ", 0\n"
                 << "  " << o << " = extractvalue {i32, i1} " << r << ", 1\n"
                 << "  br i1 " << o << ", label %ovf" << id << ", label %ok" << id << "\novf" << id << ":\n";
            fail(in, "integer overflow");
            code << "ok" << id << ":\n";
            return v;
        };

        auto checked64 = [&](const pvm::Instr& in, const char* intrinsic, const std::string& a, const std::string& b) {
            std::string r = t(), v = t(), o = t();
            int id = tmp_++;
            code << "  " << r << " = call {i64, i1} @llvm.s" << intrinsic << ".with.overflow.i64(i64 " << a << ", i64 " << b << ")\n"
                 << "  " << v << " = extractvalue {i64, i1} " << r << ", 0\n"
                 << "  " << o << " = extractvalue {i64, i1} " << r << ", 1\n"
                 << "  br i1 " << o << ", label %ovf" << id << ", label %ok" << id << "\novf" << id << ":\n";
            fail(in, "long overflow");
            code << "ok" << id << ":\n";
            return v;
        };
        // A runtime error with a value in its message (see rt_fail_cvt); the failing branch is label %bad<id>, the good one %good<id>.
        auto failIf = [&](const pvm::Instr& in, const std::string& cond, int kind, const std::string& a, const std::string& d, const std::string& b) {
            int id = tmp_++;
            code << "  br i1 " << cond << ", label %bad" << id << ", label %good" << id << "\nbad" << id << ":\n"
                 << "  call void @rt_fail_cvt(ptr " << str(f.name == "$main" ? "<main>" : f.name) << ", ptr " << str(m_.fileName(in.srcFile)) << ", i32 " << in.srcLine << ", i32 " << kind
                 << ", i64 " << a << ", double " << d << ", i64 " << b << ")\n  unreachable\ngood" << id << ":\n";
        };
        // Reference slots (arrays) live in the function's frame array so the collector finds them: an operand-stack slot is cleared when it is
        // popped (so garbage is not kept alive) except by operations that allocate, which keep their operands rooted until they are done.
        auto peekk = [&](char k, size_t fromTop) {
            const size_t idx = stack.size() - 1 - fromTop;
            slots.insert({static_cast<int>(idx), k});
            std::string r = t();
            code << "  " << r << " = load " << llType(k) << ", ptr %stk." << idx << "." << k << "\n";
            return r;
        };
        auto clearSlot = [&](size_t idx) { code << "  store ptr null, ptr %stk." << idx << ".r\n"; };
        auto setLoc = [&](const pvm::Instr& in) {
            code << "  store ptr " << str(m_.fileName(in.srcFile)) << ", ptr @rt_loc_file\n  store i32 " << in.srcLine << ", ptr @rt_loc_line\n";
        };
        auto dynLoc = [&](const pvm::Instr& in) {
            return "ptr " + str(f.name == "$main" ? "<main>" : f.name) + ", ptr " + str(m_.fileName(in.srcFile)) + ", i32 " + std::to_string(in.srcLine);
        };
        auto nullCheckMsg = [&](const pvm::Instr& in, const std::string& o, const std::string& msg) {
            int id = tmp_++;
            std::string c = t();
            code << "  " << c << " = icmp eq ptr " << o << ", null\n  br i1 " << c << ", label %nul" << id << ", label %nn" << id << "\nnul" << id << ":\n";
            fail(in, msg);
            code << "nn" << id << ":\n";
        };
        auto nullCheck = [&](const pvm::Instr& in, const std::string& o) {
            int id = tmp_++;
            std::string c = t();
            code << "  " << c << " = icmp eq ptr " << o << ", null\n  br i1 " << c << ", label %nul" << id << ", label %nn" << id << "\nnul" << id << ":\n";
            fail(in, "null array reference");
            code << "nn" << id << ":\n";
        };
        // Address of element idx (an i64 register) of array o; stride 1 for compact byte arrays, 8 otherwise.
        auto elemAddr = [&](const std::string& o, const std::string& idx, bool bytes) {
            std::string dp = t(), d = t(), off = t(), a = t();
            code << "  " << dp << " = getelementptr inbounds i8, ptr " << o << ", i64 40\n  " << d << " = load ptr, ptr " << dp << "\n";
            if (bytes) code << "  " << a << " = getelementptr inbounds i8, ptr " << d << ", i64 " << idx << "\n";
            else code << "  " << off << " = mul i64 " << idx << ", 8\n  " << a << " = getelementptr inbounds i8, ptr " << d << ", i64 " << off << "\n";
            return a;
        };
        auto lenOf = [&](const std::string& o) {
            std::string lp = t(), l = t();
            code << "  " << lp << " = getelementptr inbounds i8, ptr " << o << ", i64 16\n  " << l << " = load i64, ptr " << lp << "\n";
            return l;
        };
        // Loads / stores of array elements. Slots are 8 bytes (an int, char or boolean occupies the low 4), byte arrays are compact.
        auto elemLoadAt = [&](const std::string& a, char ek, char k) {
            std::string v = t();
            if (ek == 'y') { std::string x = t(); code << "  " << x << " = load i8, ptr " << a << "\n  " << v << " = zext i8 " << x << " to i32\n"; }
            else if (k == 'b') { std::string x = t(); code << "  " << x << " = load i32, ptr " << a << "\n  " << v << " = icmp ne i32 " << x << ", 0\n"; }
            else code << "  " << v << " = load " << llType(k) << ", ptr " << a << "\n";
            return v;
        };
        auto elemLoad = [&](const std::string& o, const std::string& idx, char ek, char k) {
            std::string a = elemAddr(o, idx, ek == 'y'), v = t();
            if (ek == 'y') { std::string x = t(); code << "  " << x << " = load i8, ptr " << a << "\n  " << v << " = zext i8 " << x << " to i32\n"; }
            else if (k == 'b') { std::string x = t(); code << "  " << x << " = load i32, ptr " << a << "\n  " << v << " = icmp ne i32 " << x << ", 0\n"; }
            else code << "  " << v << " = load " << llType(k) << ", ptr " << a << "\n";
            return v;
        };
        auto elemStoreAt = [&](const std::string& a, char ek, char k, const std::string& v, bool releaseOld) {
            if (releaseOld && k == 's') { std::string old = t(); code << "  " << old << " = load ptr, ptr " << a << "\n"; code << "  store ptr " << v << ", ptr " << a << "\n"; release(old); return; }
            if (ek == 'y') { std::string x = t(); code << "  " << x << " = trunc i32 " << v << " to i8\n  store i8 " << x << ", ptr " << a << "\n"; }
            else if (k == 'b') { std::string x = t(); code << "  " << x << " = zext i1 " << v << " to i32\n  store i32 " << x << ", ptr " << a << "\n"; }
            else code << "  store " << llType(k) << " " << v << ", ptr " << a << "\n";
        };
        auto elemStore = [&](const std::string& o, const std::string& idx, char ek, char k, const std::string& v, bool releaseOld) {
            elemStoreAt(elemAddr(o, idx, ek == 'y'), ek, k, v, releaseOld);
        };
        auto toI64 = [&](char kc, const std::string& v) {
            std::string r = t();
            if (kc == 's') code << "  " << r << " = ptrtoint ptr " << v << " to i64\n";
            else if (kc == 'b') code << "  " << r << " = zext i1 " << v << " to i64\n";
            else if (kc == 'l') return v;
            else code << "  " << r << " = sext i32 " << v << " to i64\n";
            return r;
        };
        bool terminated = false;  // the previous instruction ended its block
        bool dead = false;        // currently skipping unreachable code
        code << "  br label %B0\n";
        for (size_t idx = 0; idx < f.code.size(); ++idx) {
            const pvm::Instr& in = f.code[idx];
            bool isTarget = targets.count(idx) != 0;
            if (idx == 0 || isTarget || terminated) {
                auto it = stateAt.find(idx);
                if (!terminated && idx != 0) code << "  br label %B" << idx << "\n";
                if (it != stateAt.end()) {
                    stack = it->second.first;
                    tys = it->second.second;
                    dead = false;
                    code << "B" << idx << ":\n";
                } else if (idx == 0) {
                    stack.clear();
                    tys.clear();
                    stateAt.emplace(0, std::make_pair(stack, tys));  // a loop may start at the first instruction
                    code << "B0:\n";
                } else if (terminated && !dead) {  // code after a jump/ret that no earlier jump targets
                    dead = true;
                    code << "B" << idx << ":\n  unreachable\n";
                } else if (!dead) {  // reached by fall-through; later jumps may target it
                    stateAt.emplace(idx, std::make_pair(stack, tys));
                    code << "B" << idx << ":\n";
                }
                terminated = dead;
            }
            if (dead) continue;  // unreachable code: never executed and not stack-typed by the verifier
            switch (in.op) {
                case Op::PushStr: push('s', str(in.str)); break;
                case Op::PushInt: push('i', std::to_string(in.ival)); break;
                case Op::PushBool: push('b', in.ival ? "true" : "false"); break;
                case Op::Load: {
                    char k = slotKinds[static_cast<size_t>(in.ival)];
                    std::string r = t();
                    code << "  " << r << " = load " << vol << llType(k) << ", ptr %loc." << in.ival << "\n";
                    if (k == 's') retain(r);  // the operand stack owns its own reference
                    push(k, r, slotTypes[static_cast<size_t>(in.ival)]);
                    break;
                }
                case Op::Store: {
                    char k = slotKinds[static_cast<size_t>(in.ival)];
                    std::string v = pop(k);
                    if (k == 's') {  // the local takes over the reference from the stack and drops its old string
                        std::string old = t();
                        code << "  " << old << " = load " << vol << "ptr, ptr %loc." << in.ival << "\n";
                        code << "  store " << vol << "ptr " << v << ", ptr %loc." << in.ival << "\n";
                        release(old);
                        break;
                    }
                    code << "  store " << vol << llType(k) << " " << v << ", ptr %loc." << in.ival << "\n";
                    break;
                }
                case Op::AddI: case Op::SubI: case Op::MulI: {
                    std::string b = pop('i'), a = pop('i');
                    push('i', checked(in, in.op == Op::AddI ? "add" : in.op == Op::SubI ? "sub" : "mul", a, b));
                    break;
                }
                case Op::NegI: { std::string a = pop('i'); push('i', checked(in, "sub", "0", a)); break; }
                case Op::DivI: case Op::RemI: {
                    std::string b = pop('i'), a = pop('i');
                    int id = tmp_++;
                    std::string z = t();
                    code << "  " << z << " = icmp eq i32 " << b << ", 0\n  br i1 " << z << ", label %dz" << id << ", label %nz" << id << "\ndz" << id << ":\n";
                    fail(in, in.op == Op::DivI ? "division by zero" : "remainder by zero");
                    code << "nz" << id << ":\n";
                    std::string c1 = t(), c2 = t(), c3 = t();
                    code << "  " << c1 << " = icmp eq i32 " << a << ", -2147483648\n  " << c2 << " = icmp eq i32 " << b << ", -1\n  "
                         << c3 << " = and i1 " << c1 << ", " << c2 << "\n  br i1 " << c3 << ", label %ov" << id << ", label %okd" << id << "\nov" << id << ":\n";
                    fail(in, "integer overflow");
                    code << "okd" << id << ":\n";
                    std::string r = t();
                    code << "  " << r << " = " << (in.op == Op::DivI ? "sdiv" : "srem") << " i32 " << a << ", " << b << "\n";
                    push('i', r);
                    break;
                }
                case Op::PushLng: push('l', std::to_string(in.ival)); break;
                case Op::PushDbl: push('d', dblLit(in.dval)); break;
                case Op::PushChr: push('c', std::to_string(in.ival)); break;
                case Op::AddL: case Op::SubL: case Op::MulL: {
                    std::string b = pop('l'), a = pop('l');
                    push('l', checked64(in, in.op == Op::AddL ? "add" : in.op == Op::SubL ? "sub" : "mul", a, b));
                    break;
                }
                case Op::NegL: { std::string a = pop('l'); push('l', checked64(in, "sub", "0", a)); break; }
                case Op::DivL: case Op::RemL: {
                    std::string b = pop('l'), a = pop('l');
                    int id = tmp_++;
                    std::string z = t();
                    code << "  " << z << " = icmp eq i64 " << b << ", 0\n  br i1 " << z << ", label %dz" << id << ", label %nz" << id << "\ndz" << id << ":\n";
                    fail(in, in.op == Op::DivL ? "division by zero" : "remainder by zero");
                    code << "nz" << id << ":\n";
                    std::string c1 = t(), c2 = t(), c3 = t();
                    code << "  " << c1 << " = icmp eq i64 " << a << ", -9223372036854775808\n  " << c2 << " = icmp eq i64 " << b << ", -1\n  "
                         << c3 << " = and i1 " << c1 << ", " << c2 << "\n  br i1 " << c3 << ", label %ov" << id << ", label %okd" << id << "\nov" << id << ":\n";
                    fail(in, "long overflow");
                    code << "okd" << id << ":\n";
                    std::string r = t();
                    code << "  " << r << " = " << (in.op == Op::DivL ? "sdiv" : "srem") << " i64 " << a << ", " << b << "\n";
                    push('l', r);
                    break;
                }
                case Op::EqL: case Op::NeL: case Op::LtL: case Op::LeL: case Op::GtL: case Op::GeL: {
                    std::string b = pop('l'), a = pop('l'), r = t();
                    const char* cc = in.op == Op::EqL ? "eq" : in.op == Op::NeL ? "ne" : in.op == Op::LtL ? "slt" : in.op == Op::LeL ? "sle" : in.op == Op::GtL ? "sgt" : "sge";
                    code << "  " << r << " = icmp " << cc << " i64 " << a << ", " << b << "\n";
                    push('b', r);
                    break;
                }
                case Op::EqC: case Op::NeC: case Op::LtC: case Op::LeC: case Op::GtC: case Op::GeC: {
                    std::string b = pop('c'), a = pop('c'), r = t();
                    const char* cc = in.op == Op::EqC ? "eq" : in.op == Op::NeC ? "ne" : in.op == Op::LtC ? "slt" : in.op == Op::LeC ? "sle" : in.op == Op::GtC ? "sgt" : "sge";
                    code << "  " << r << " = icmp " << cc << " i32 " << a << ", " << b << "\n";
                    push('b', r);
                    break;
                }
                case Op::AddD: case Op::SubD: case Op::MulD: case Op::DivD: case Op::RemD: {
                    std::string b = pop('d'), a = pop('d'), r = t();
                    const char* o = in.op == Op::AddD ? "fadd" : in.op == Op::SubD ? "fsub" : in.op == Op::MulD ? "fmul" : in.op == Op::DivD ? "fdiv" : "frem";
                    code << "  " << r << " = " << o << " double " << a << ", " << b << "\n";
                    push('d', r);
                    break;
                }
                case Op::NegD: { std::string a = pop('d'), r = t(); code << "  " << r << " = fneg double " << a << "\n"; push('d', r); break; }
                case Op::EqD: case Op::NeD: case Op::LtD: case Op::LeD: case Op::GtD: case Op::GeD: {
                    std::string b = pop('d'), a = pop('d'), r = t();
                    const char* cc = in.op == Op::EqD ? "oeq" : in.op == Op::NeD ? "une" : in.op == Op::LtD ? "olt" : in.op == Op::LeD ? "ole" : in.op == Op::GtD ? "ogt" : "oge";
                    code << "  " << r << " = fcmp " << cc << " double " << a << ", " << b << "\n";
                    push('b', r);
                    break;
                }
                case Op::I2L: { std::string a = pop('i'), r = t(); code << "  " << r << " = sext i32 " << a << " to i64\n"; push('l', r); break; }
                case Op::L2I: {
                    std::string a = pop('l'), lo = t(), hi = t(), bad = t(), r = t();
                    code << "  " << lo << " = icmp slt i64 " << a << ", -2147483648\n  " << hi << " = icmp sgt i64 " << a << ", 2147483647\n  " << bad << " = or i1 " << lo << ", " << hi << "\n";
                    failIf(in, bad, 1, a, "0.0", "0");
                    code << "  " << r << " = trunc i64 " << a << " to i32\n";
                    push('i', r);
                    break;
                }
                case Op::I2D: { std::string a = pop('i'), r = t(); code << "  " << r << " = sitofp i32 " << a << " to double\n"; push('d', r); break; }
                case Op::L2D: { std::string a = pop('l'), r = t(); code << "  " << r << " = sitofp i64 " << a << " to double\n"; push('d', r); break; }
                case Op::D2I: {
                    std::string a = pop('d'), c1 = t(), c2 = t(), ok = t(), bad = t(), r = t();
                    code << "  " << c1 << " = fcmp ogt double " << a << ", -2147483649.0\n  " << c2 << " = fcmp olt double " << a << ", 2147483648.0\n  " << ok << " = and i1 " << c1 << ", " << c2
                         << "\n  " << bad << " = xor i1 " << ok << ", true\n";
                    failIf(in, bad, 3, "0", a, "0");
                    code << "  " << r << " = fptosi double " << a << " to i32\n";
                    push('i', r);
                    break;
                }
                case Op::D2L: {
                    std::string a = pop('d'), c1 = t(), c2 = t(), ok = t(), bad = t(), r = t();
                    code << "  " << c1 << " = fcmp oge double " << a << ", -9223372036854775808.0\n  " << c2 << " = fcmp olt double " << a << ", 9223372036854775808.0\n  " << ok << " = and i1 " << c1 << ", " << c2
                         << "\n  " << bad << " = xor i1 " << ok << ", true\n";
                    failIf(in, bad, 2, "0", a, "0");
                    code << "  " << r << " = fptosi double " << a << " to i64\n";
                    push('l', r);
                    break;
                }
                case Op::C2I: { std::string a = pop('c'); push('i', a); break; }
                case Op::B2I: { std::string a = pop('y'); push('i', a); break; }
                case Op::I2C: {
                    std::string a = pop('i'), c1 = t(), c2 = t(), c3 = t(), c4 = t(), c5 = t(), c6 = t();
                    code << "  " << c1 << " = icmp slt i32 " << a << ", 0\n  " << c2 << " = icmp sgt i32 " << a << ", 1114111\n  " << c3 << " = icmp sge i32 " << a << ", 55296\n  " << c4 << " = icmp sle i32 " << a
                         << ", 57343\n  " << c5 << " = and i1 " << c3 << ", " << c4 << "\n  " << c6 << " = or i1 " << c1 << ", " << c2 << "\n";
                    std::string bad = t();
                    code << "  " << bad << " = or i1 " << c6 << ", " << c5 << "\n";
                    std::string wide = t();
                    code << "  " << wide << " = sext i32 " << a << " to i64\n";
                    failIf(in, bad, 5, wide, "0.0", "0");
                    push('c', a);
                    break;
                }
                case Op::I2B: {
                    std::string a = pop('i'), c1 = t(), c2 = t(), bad = t(), wide = t();
                    code << "  " << c1 << " = icmp slt i32 " << a << ", 0\n  " << c2 << " = icmp sgt i32 " << a << ", 255\n  " << bad << " = or i1 " << c1 << ", " << c2 << "\n  " << wide << " = sext i32 " << a << " to i64\n";
                    failIf(in, bad, 4, wide, "0.0", "0");
                    push('y', a);
                    break;
                }
                case Op::AndI: case Op::OrI: case Op::XorI: {
                    std::string b = pop('i'), a = pop('i'), r = t();
                    code << "  " << r << " = " << (in.op == Op::AndI ? "and" : in.op == Op::OrI ? "or" : "xor") << " i32 " << a << ", " << b << "\n";
                    push('i', r);
                    break;
                }
                case Op::AndL: case Op::OrL: case Op::XorL: {
                    std::string b = pop('l'), a = pop('l'), r = t();
                    code << "  " << r << " = " << (in.op == Op::AndL ? "and" : in.op == Op::OrL ? "or" : "xor") << " i64 " << a << ", " << b << "\n";
                    push('l', r);
                    break;
                }
                case Op::ShlI: case Op::ShrI: {
                    std::string n = pop('i'), a = pop('i'), c = t(), r = t();
                    code << "  " << c << " = and i32 " << n << ", 31\n  " << r << " = " << (in.op == Op::ShlI ? "shl" : "ashr") << " i32 " << a << ", " << c << "\n";
                    push('i', r);
                    break;
                }
                case Op::ShlL: case Op::ShrL: {
                    std::string n = pop('i'), a = pop('l'), c = t(), w = t(), r = t();
                    code << "  " << c << " = and i32 " << n << ", 63\n  " << w << " = zext i32 " << c << " to i64\n  " << r << " = " << (in.op == Op::ShlL ? "shl" : "ashr") << " i64 " << a << ", " << w << "\n";
                    push('l', r);
                    break;
                }
                case Op::BNotI: { std::string a = pop('i'), r = t(); code << "  " << r << " = xor i32 " << a << ", -1\n"; push('i', r); break; }
                case Op::BNotL: { std::string a = pop('l'), r = t(); code << "  " << r << " = xor i64 " << a << ", -1\n"; push('l', r); break; }
                case Op::StrLng: { std::string a = pop('l'), r = t(); code << "  " << r << " = call ptr @rt_str_lng(i64 " << a << ")\n"; push('s', r); break; }
                case Op::StrDbl: { std::string a = pop('d'), r = t(); code << "  " << r << " = call ptr @rt_str_dbl(double " << a << ")\n"; push('s', r); break; }
                case Op::StrChr: { std::string a = pop('c'), r = t(); code << "  " << r << " = call ptr @rt_str_chr(i32 " << a << ")\n"; push('s', r); break; }
                case Op::StrLen: { std::string a = pop('s'), r = t(); code << "  " << r << " = call i32 @rt_str_len(ptr " << a << ")\n"; release(a); push('i', r); break; }
                case Op::StrAt: {
                    std::string idx = pop('i'), a = pop('s'), r = t(), bad = t();
                    code << "  " << r << " = call i32 @rt_str_at(ptr " << a << ", i32 " << idx << ")\n  " << bad << " = icmp slt i32 " << r << ", 0\n";
                    std::string wide = t(), wlen = t();
                    code << "  " << wide << " = sext i32 " << idx << " to i64\n  " << wlen << " = ptrtoint ptr " << a << " to i64\n";
                    failIf(in, bad, 6, wide, "0.0", wlen);
                    release(a);
                    push('c', r);
                    break;
                }
                case Op::LtS: case Op::LeS: case Op::GtS: case Op::GeS: {
                    std::string b = pop('s'), a = pop('s'), c = t(), r = t();
                    const char* cc = in.op == Op::LtS ? "slt" : in.op == Op::LeS ? "sle" : in.op == Op::GtS ? "sgt" : "sge";
                    code << "  " << c << " = call i32 @rt_strcmp(ptr " << a << ", ptr " << b << ")\n  " << r << " = icmp " << cc << " i32 " << c << ", 0\n";
                    release(a);
                    release(b);
                    push('b', r);
                    break;
                }
                case Op::NewArray: {
                    const std::string w = "an array element in " + where;
                    const char ek = arrayElemKind(in.type, w);
                    std::string n = pop('i'), neg = t(), wide = t(), r = t();
                    code << "  " << neg << " = icmp slt i32 " << n << ", 0\n  " << wide << " = sext i32 " << n << " to i64\n";
                    failIf(in, neg, 8, wide, "0.0", "0");
                    setLoc(in);
                    code << "  " << r << " = call ptr @rt_arr_new(i32 " << static_cast<int>(ek) << ", i64 " << wide << ", ptr " << (ek == 's' ? cstr("@.empty") : "null") << ")\n";
                    push('r', r, *in.objType);
                    break;
                }
                case Op::ArrayLit: {
                    const std::string w = "an array element in " + where;
                    const char ek = arrayElemKind(in.type, w), k = kindOf(in.type, w);
                    const size_t n = static_cast<size_t>(in.ival);
                    std::string r = t();
                    setLoc(in);  // allocate first: the elements are still in their operand-stack slots, hence rooted
                    code << "  " << r << " = call ptr @rt_arr_new(i32 " << static_cast<int>(ek) << ", i64 " << n << ", ptr null)\n";
                    for (size_t j = n; j-- > 0;) {
                        std::string v = pop(k);
                        elemStore(r, std::to_string(j), ek, k, v, false);
                    }
                    push('r', r, *in.objType);
                    break;
                }
                case Op::ALoad: {
                    std::string idx = pop('i'), o = pop('r');
                    const Type arr = lastTy;
                    const std::string w = "an array element in " + where;
                    const char ek = arrayElemKind(*arr.elem, w), k = kindOf(*arr.elem, w);
                    nullCheck(in, o);
                    std::string wide = t(), len = lenOf(o), oob = t();
                    code << "  " << wide << " = sext i32 " << idx << " to i64\n  " << oob << " = icmp uge i64 " << wide << ", " << len << "\n";
                    failIf(in, oob, 7, wide, "0.0", len);
                    std::string v = elemLoad(o, wide, ek, k);
                    if (k == 's') retain(v);
                    push(k, v, *arr.elem);
                    break;
                }
                case Op::AStore: {
                    // the array type is only known after the element is popped: peek at the array's type first
                    const Type arr = tys[tys.size() - 3];
                    const std::string w = "an array element in " + where;
                    const char ek = arrayElemKind(*arr.elem, w), k = kindOf(*arr.elem, w);
                    std::string v = pop(k), idx = pop('i'), o = pop('r');
                    nullCheck(in, o);
                    std::string wide = t(), len = lenOf(o), oob = t();
                    code << "  " << wide << " = sext i32 " << idx << " to i64\n  " << oob << " = icmp uge i64 " << wide << ", " << len << "\n";
                    failIf(in, oob, 7, wide, "0.0", len);
                    elemStore(o, wide, ek, k, v, true);
                    break;
                }
                case Op::ALen: {
                    std::string o = pop('r');
                    nullCheck(in, o);
                    std::string len = lenOf(o), r = t();
                    code << "  " << r << " = trunc i64 " << len << " to i32\n";
                    push('i', r);
                    break;
                }
                case Op::APush: {
                    const Type arr = tys[tys.size() - 2];
                    const std::string w = "an array element in " + where;
                    const char ek = arrayElemKind(*arr.elem, w), k = kindOf(*arr.elem, w);
                    std::string v = peekk(k, 0), o = peekk('r', 1);
                    nullCheck(in, o);
                    setLoc(in);
                    std::string slot = t();
                    code << "  " << slot << " = call ptr @rt_arr_push_slot(ptr " << o << ")\n";  // may collect: o and v are still in their slots
                    elemStoreAt(slot, ek, k, v, false);
                    if (k == 'r') clearSlot(stack.size() - 1);
                    stack.pop_back(); tys.pop_back();
                    clearSlot(stack.size() - 1);
                    stack.pop_back(); tys.pop_back();
                    break;
                }
                case Op::ACopy: {
                    const Type arr = tys.back();
                    std::string o = peekk('r', 0);
                    nullCheck(in, o);
                    setLoc(in);
                    std::string r = t();
                    code << "  " << r << " = call ptr @rt_arr_copy(ptr " << o << ")\n";
                    clearSlot(stack.size() - 1);
                    stack.pop_back(); tys.pop_back();
                    push('r', r, arr);
                    break;
                }
                case Op::EqRef: case Op::NeRef: {
                    std::string b = pop('r'), a = pop('r'), r = t();
                    code << "  " << r << " = icmp " << (in.op == Op::EqRef ? "eq" : "ne") << " ptr " << a << ", " << b << "\n";
                    push('b', r);
                    break;
                }
                case Op::Dup: {
                    const char k = stack.back();
                    std::string v = pop(k);
                    if (k == 's') retain(v);  // two owners now
                    push(k, v, lastTy);
                    push(k, v, lastTy);
                    break;
                }
                case Op::NewObj: {
                    const pvm::ClassDecl& cd = m_.classes[static_cast<size_t>(in.cls)];
                    setLoc(in);
                    std::string r = t();
                    code << "  " << r << " = call ptr @rt_obj_new(i32 " << in.cls << ", i64 " << cd.fields.size() << ", ptr " << cstr("@.empty") << ")\n";
                    push('r', r, Type::classOf(cd.name));
                    break;
                }
                case Op::GetField: case Op::SetField: {
                    const pvm::ClassDecl& cd = m_.classes[static_cast<size_t>(in.cls)];
                    const Type ft = cd.fields[static_cast<size_t>(in.ival)].type;
                    const char k = kindOf(ft, "a field of class " + cd.name);
                    const char ek = k == 'y' ? 'i' : k;
                    const std::string field = in.str.substr(in.str.rfind('.') + 1);
                    const bool get = in.op == Op::GetField;
                    std::string v;
                    if (!get) v = pop(k);
                    std::string o = pop('r'), c = t();
                    int id = tmp_++;
                    code << "  " << c << " = icmp eq ptr " << o << ", null\n  br i1 " << c << ", label %nul" << id << ", label %nn" << id << "\nnul" << id << ":\n";
                    fail(in, std::string("null reference: cannot ") + (get ? "read" : "assign") + " field '" + field + "' of a null object");
                    code << "nn" << id << ":\n";
                    if (get) {
                        std::string x = elemLoad(o, std::to_string(in.ival), ek, k);
                        if (k == 's') retain(x);
                        push(k, x, ft);
                    } else {
                        elemStore(o, std::to_string(in.ival), ek, k, v, true);
                    }
                    break;
                }
                case Op::Upcast: {
                    const Type to = Type::classOf(m_.classes[static_cast<size_t>(in.cls)].name);
                    std::string v = pop('r');
                    push('r', v, to);
                    break;
                }
                case Op::Downcast: {
                    const Type to = Type::classOf(m_.classes[static_cast<size_t>(in.cls)].name);
                    std::string v = pop('r');
                    code << "  call void @rt_downcast(ptr " << str(f.name == "$main" ? "<main>" : f.name) << ", ptr " << str(m_.fileName(in.srcFile)) << ", i32 " << in.srcLine << ", ptr " << v << ", i32 " << in.cls << ")\n";
                    push('r', v, to);
                    break;
                }
                case Op::IsInst: {
                    std::string v = pop('r'), r = t(), b = t();
                    code << "  " << r << " = call i32 @rt_isinst(ptr " << v << ", i32 " << in.cls << ")\n  " << b << " = icmp ne i32 " << r << ", 0\n";
                    push('b', b);
                    break;
                }
                case Op::PushNull: {
                    kindOf(in.type, "a null value in " + where);
                    push('r', "null", in.type);
                    break;
                }
                case Op::Wrap: {
                    if (stack.back() == 'r') {  // a class, array or map is null itself
                        if (tys.back().kind == Type::Void) throw Unsupported{"an untyped reference in " + where + " (use the PVM)"};
                        std::string v = pop('r');
                        push('r', v, Type::nullableOf(lastTy));
                        break;
                    }
                    // a scalar or string becomes a one-element box on the heap
                    const char ks = stack.back();
                    Type st;
                    switch (ks) {
                        case 'i': st = Type(Type::Int); break;
                        case 'c': st = Type(Type::Char); break;
                        case 'b': st = Type(Type::Bool); break;
                        case 'l': st = Type(Type::Long); break;
                        case 'y': st = Type(Type::Byte); break;
                        case 'd': st = Type(Type::Double); break;
                        default: st = Type(Type::String); break;
                    }
                    const char ek = arrayElemKind(st, "a nullable value in " + where);
                    std::string v = pop(ks), bx = t();
                    setLoc(in);
                    code << "  " << bx << " = call ptr @rt_arr_new(i32 " << static_cast<int>(ek) << ", i64 1, ptr null)\n";
                    elemStore(bx, "0", ek, ks, v, false);
                    push('r', bx, Type::nullableOf(st));
                    break;
                }
                case Op::Unwrap: {
                    std::string v = pop('r'), c = t();
                    if (lastTy.kind != Type::Nullable) throw Unsupported{"an untyped nullable value in " + where + " (use the PVM)"};
                    const Type inner = *lastTy.elem;
                    int id = tmp_++;
                    code << "  " << c << " = icmp eq ptr " << v << ", null\n  br i1 " << c << ", label %nul" << id << ", label %nn" << id << "\nnul" << id << ":\n";
                    fail(in, "null value: cannot unwrap null (use the null-coalescing operator or check for null first)");
                    code << "nn" << id << ":\n";
                    const char ik = kindOf(inner, "a value in " + where);
                    if (ik == 'r') { push('r', v, inner); break; }
                    const char ek = arrayElemKind(inner, "a value in " + where);
                    std::string x = elemLoad(v, "0", ek, ik);
                    if (ik == 's') retain(x);
                    push(ik, x, inner);
                    break;
                }
                case Op::IsNull: {
                    if (stack.back() != 'r') throw Unsupported{"a null test of a non-reference value in " + where + " is not supported yet (use the PVM)"};
                    std::string v = pop('r'), c = t();
                    code << "  " << c << " = icmp eq ptr " << v << ", null\n";
                    push('b', c);
                    break;
                }
                case Op::NewMap: {
                    const Type& mt = *in.objType;
                    kindOf(mt, "a map in " + where);
                    const char kc = kindOf(*mt.key, "a map key in " + where), vc = kindOf(*mt.elem, "a map value in " + where);
                    setLoc(in);
                    std::string r = t();
                    code << "  " << r << " = call ptr @rt_map_new(i32 " << static_cast<int>(kc == 's' ? 's' : 'i') << ", i32 " << static_cast<int>(vc == 's' || vc == 'r' ? vc : 'i') << ")\n";
                    push('r', r, mt);
                    break;
                }
                case Op::MapGet: case Op::MapTry: case Op::MapHas: case Op::MapDel: {
                    const Type mt = tys[tys.size() - 2];
                    if (mt.kind != Type::Map) throw Unsupported{"an untyped map in " + where + " (use the PVM)"};
                    const char kc = kindOf(*mt.key, "a map key in " + where), vk = kindOf(*mt.elem, "a map value in " + where);
                    const char ek = vk == 'y' ? 'i' : vk;
                    std::string key = pop(kc), m = pop('r');
                    nullCheckMsg(in, m, "null map reference");
                    std::string k64 = toI64(kc, key), idx = t();
                    if (in.op == Op::MapDel) {
                        std::string r = t(), b = t();
                        code << "  " << r << " = call i32 @rt_map_del(ptr " << m << ", i64 " << k64 << ")\n  " << b << " = icmp ne i32 " << r << ", 0\n";
                        if (kc == 's') release(key);
                        push('b', b);
                        break;
                    }
                    code << "  " << idx << " = call i64 @rt_map_find(ptr " << m << ", i64 " << k64 << ")\n";
                    std::string found = t();
                    code << "  " << found << " = icmp sge i64 " << idx << ", 0\n";
                    if (in.op == Op::MapHas) {
                        if (kc == 's') release(key);
                        push('b', found);
                        break;
                    }
                    if (in.op == Op::MapGet) {
                        int id = tmp_++;
                        code << "  br i1 " << found << ", label %mg" << id << ", label %mn" << id << "\nmn" << id << ":\n"
                             << "  call void @rt_fail_key(ptr " << str(f.name == "$main" ? "<main>" : f.name) << ", ptr " << str(m_.fileName(in.srcFile)) << ", i32 " << in.srcLine << ", i32 " << static_cast<int>(kc) << ", i64 " << k64 << ")\n  unreachable\nmg" << id << ":\n";
                        if (kc == 's') release(key);
                        std::string slot = t();
                        code << "  " << slot << " = call ptr @rt_map_slot(ptr " << m << ", i64 " << idx << ")\n";
                        std::string v = elemLoadAt(slot, ek, vk);
                        if (vk == 's') retain(v);
                        push(vk, v, *mt.elem);
                        break;
                    }
                    // MapTry: the value as a nullable (null when the key is missing)
                    int id = tmp_++;
                    const bool refVal = vk == 'r';  // class, array, map and already-nullable values are pointers
                    code << "  br i1 " << found << ", label %mtf" << id << ", label %mtn" << id << "\nmtf" << id << ":\n";
                    std::string slot = t();
                    code << "  " << slot << " = call ptr @rt_map_slot(ptr " << m << ", i64 " << idx << ")\n";
                    std::string v = elemLoadAt(slot, ek, vk), res = v;
                    if (!refVal) {
                        if (vk == 's') retain(v);
                        const char bek = arrayElemKind(*mt.elem, "a map value in " + where);
                        setLoc(in);
                        res = t();
                        code << "  " << res << " = call ptr @rt_arr_new(i32 " << static_cast<int>(bek) << ", i64 1, ptr null)\n";
                        elemStore(res, "0", bek, vk, v, false);
                    }
                    code << "  br label %mtj" << id << "\nmtn" << id << ":\n  br label %mtj" << id << "\nmtj" << id << ":\n";
                    std::string r = t();
                    code << "  " << r << " = phi ptr [ " << res << ", %mtf" << id << " ], [ null, %mtn" << id << " ]\n";
                    if (kc == 's') release(key);
                    push('r', r, mt.elem->kind == Type::Nullable ? *mt.elem : Type::nullableOf(*mt.elem));
                    break;
                }
                case Op::MapSet: {
                    const Type mt = tys[tys.size() - 3];
                    if (mt.kind != Type::Map) throw Unsupported{"an untyped map in " + where + " (use the PVM)"};
                    const char kc = kindOf(*mt.key, "a map key in " + where), vk = kindOf(*mt.elem, "a map value in " + where);
                    const char ek = vk == 'y' ? 'i' : vk;
                    std::string val = peekk(vk, 0), key = peekk(kc, 1), m = peekk('r', 2);
                    nullCheckMsg(in, m, "null map reference");
                    setLoc(in);
                    std::string k64 = toI64(kc, key), idx = t(), slot = t();
                    code << "  " << idx << " = call i64 @rt_map_put(ptr " << m << ", i64 " << k64 << ", ptr " << cstr("@.empty") << ")\n";
                    code << "  " << slot << " = call ptr @rt_map_slot(ptr " << m << ", i64 " << idx << ")\n";
                    elemStoreAt(slot, ek, vk, val, vk == 's');
                    if (vk == 'r') clearSlot(stack.size() - 1);
                    stack.pop_back(); tys.pop_back();
                    stack.pop_back(); tys.pop_back();
                    clearSlot(stack.size() - 1);
                    stack.pop_back(); tys.pop_back();
                    break;
                }
                case Op::MapLen: {
                    std::string m = pop('r');
                    nullCheckMsg(in, m, "null map reference");
                    std::string r = t();
                    code << "  " << r << " = call i32 @rt_map_len(ptr " << m << ")\n";
                    push('i', r);
                    break;
                }
                case Op::MapKeys: case Op::MapValues: {
                    const Type mt = tys.back();
                    if (mt.kind != Type::Map) throw Unsupported{"an untyped map in " + where + " (use the PVM)"};
                    const bool keys = in.op == Op::MapKeys;
                    const Type et = keys ? *mt.key : *mt.elem;
                    const char ek = arrayElemKind(et, "a map entry in " + where);
                    std::string m = peekk('r', 0);
                    nullCheckMsg(in, m, "null map reference");
                    setLoc(in);
                    std::string r = t();
                    code << "  " << r << " = call ptr @rt_map_collect(ptr " << m << ", i32 " << (keys ? 0 : 1) << ", i32 " << static_cast<int>(ek) << ")\n";
                    clearSlot(stack.size() - 1);
                    stack.pop_back(); tys.pop_back();
                    push('r', r, Type::arrayOf(et));
                    break;
                }
                case Op::ParseInt: case Op::ParseLng: case Op::ParseDbl: {
                    const int kind = in.op == Op::ParseInt ? 1 : in.op == Op::ParseLng ? 2 : 3;
                    std::string str_ = pop('s'), v = t(), ok = t(), bad = t();
                    const char* ty = kind == 1 ? "i32" : kind == 2 ? "i64" : "double";
                    code << "  " << v << " = call " << ty << " @rt_parse_" << (kind == 1 ? "int" : kind == 2 ? "lng" : "dbl") << "(ptr " << str_ << ", ptr %okp)\n  " << ok << " = load i32, ptr %okp\n  " << bad << " = icmp eq i32 " << ok << ", 0\n";
                    int id = tmp_++;
                    code << "  br i1 " << bad << ", label %pf" << id << ", label %pg" << id << "\npf" << id << ":\n  call void @rt_fail_parse(ptr " << str(f.name == "$main" ? "<main>" : f.name) << ", ptr " << str(m_.fileName(in.srcFile)) << ", i32 " << in.srcLine << ", i32 " << kind << ", ptr " << str_ << ")\n  unreachable\npg" << id << ":\n";
                    release(str_);
                    push(kind == 1 ? 'i' : kind == 2 ? 'l' : 'd', v);
                    break;
                }
                case Op::Concat: {
                    std::string b = pop('s'), a = pop('s'), r = t();
                    code << "  " << r << " = call ptr @rt_concat(ptr " << a << ", ptr " << b << ")\n";
                    release(a);
                    release(b);
                    push('s', r);
                    break;
                }
                case Op::StrInt: { std::string a = pop('i'), r = t(); code << "  " << r << " = call ptr @rt_str_int(i32 " << a << ")\n"; push('s', r); break; }
                case Op::StrBool: {
                    std::string a = pop('b'), r = t();
                    code << "  " << r << " = select i1 " << a << ", ptr " << cstr("@.true") << ", ptr " << cstr("@.false") << "\n";
                    push('s', r);
                    break;
                }
                case Op::EqI: case Op::NeI: case Op::LtI: case Op::LeI: case Op::GtI: case Op::GeI: {
                    std::string b = pop('i'), a = pop('i'), r = t();
                    const char* cc = in.op == Op::EqI ? "eq" : in.op == Op::NeI ? "ne" : in.op == Op::LtI ? "slt" : in.op == Op::LeI ? "sle" : in.op == Op::GtI ? "sgt" : "sge";
                    code << "  " << r << " = icmp " << cc << " i32 " << a << ", " << b << "\n";
                    push('b', r);
                    break;
                }
                case Op::EqB: case Op::NeB: {
                    std::string b = pop('b'), a = pop('b'), r = t();
                    code << "  " << r << " = icmp " << (in.op == Op::EqB ? "eq" : "ne") << " i1 " << a << ", " << b << "\n";
                    push('b', r);
                    break;
                }
                case Op::EqS: case Op::NeS: {
                    std::string b = pop('s'), a = pop('s'), r = t();
                    code << "  " << r << " = call zeroext i1 @rt_streq(ptr " << a << ", ptr " << b << ")\n";
                    release(a);
                    release(b);
                    if (in.op == Op::NeS) { std::string n = t(); code << "  " << n << " = xor i1 " << r << ", true\n"; r = n; }
                    push('b', r);
                    break;
                }
                case Op::Not: { std::string a = pop('b'), r = t(); code << "  " << r << " = xor i1 " << a << ", true\n"; push('b', r); break; }
                case Op::Jump:
                    if (static_cast<size_t>(in.target) <= idx && !stateAt.count(static_cast<size_t>(in.target)))
                        throw Unsupported{"a backward jump to a label that is not reached by fall-through (ML line " + std::to_string(in.mlLine) + ", " + where + ") is not supported by the LLVM backend"};
                    stateAt.emplace(static_cast<size_t>(in.target), std::make_pair(stack, tys));
                    code << "  br label %B" << in.target << "\n";
                    terminated = true;
                    break;
                case Op::JumpFalse: {
                    std::string c = pop('b');
                    if (static_cast<size_t>(in.target) <= idx && !stateAt.count(static_cast<size_t>(in.target)))
                        throw Unsupported{"a backward jump to a label that is not reached by fall-through (ML line " + std::to_string(in.mlLine) + ", " + where + ") is not supported by the LLVM backend"};
                    stateAt.emplace(static_cast<size_t>(in.target), std::make_pair(stack, tys));
                    code << "  br i1 " << c << ", label %B" << idx + 1 << ", label %B" << in.target << "\n";
                    terminated = true;
                    stateAt.emplace(idx + 1, std::make_pair(stack, tys));
                    break;
                }
                case Op::Call: {
                    const pvm::Function& callee = m_.funcs[static_cast<size_t>(in.target)];
                    std::vector<std::string> args(callee.sig.params.size());
                    for (size_t i = callee.sig.params.size(); i-- > 0;) args[i] = pop(kindOf(callee.sig.params[i], "a parameter"));
                    // Same limit and message as the PVM: more than 5000 nested calls is a runtime error at the call.
                    const int did = tmp_++;
                    const std::string dv = t(), dc = t(), dn = t();
                    code << "  " << dv << " = load i32, ptr @rt_depth\n  " << dc << " = icmp sge i32 " << dv << ", 5000\n  br i1 " << dc
                         << ", label %so" << did << ", label %cd" << did << "\nso" << did << ":\n";
                    fail(in, "call stack overflow (more than 5000 nested calls; infinite recursion?)");
                    code << "cd" << did << ":\n  call void @rt_trace(i32 " << dv << ", ptr " << str(f.name == "$main" ? "<main>" : f.name) << ", ptr " << str(m_.fileName(in.srcFile)) << ", i32 " << in.srcLine
                         << ")\n  " << dn << " = add i32 " << dv << ", 1\n  store i32 " << dn << ", ptr @rt_depth\n";
                    std::string call = "call ";
                    std::string r;
                    if (callee.sig.ret.kind == Type::Void) call += "void";
                    else { r = t(); call = "  " + r + " = call " + llType(kindOf(callee.sig.ret, "a return value")); }
                    if (callee.sig.ret.kind == Type::Void) call = "  call void";
                    code << call << " @\"f." << callee.name << "\"(";
                    for (size_t i = 0; i < args.size(); ++i)
                        code << (i ? ", " : "") << llType(kindOf(callee.sig.params[i], "a parameter")) << " " << args[i];
                    code << ")\n  store i32 " << dv << ", ptr @rt_depth\n";
                    if (callee.sig.ret.kind != Type::Void) push(kindOf(callee.sig.ret, "a return value"), r, callee.sig.ret);
                    break;
                }
                case Op::GetGlobal: {
                    const Type gt = m_.globals[static_cast<size_t>(in.target)].type;
                    const char k = kindOf(gt, "a file-level variable");
                    std::string v = t();
                    code << "  " << v << " = load " << llType(k) << ", ptr @g." << in.target << "\n";
                    if (k == 's') retain(v);
                    push(k, v, gt);
                    break;
                }
                case Op::SetGlobal: {
                    const Type gt = m_.globals[static_cast<size_t>(in.target)].type;
                    const char k = kindOf(gt, "a file-level variable");
                    std::string v = pop(k);
                    if (k == 's') {
                        std::string old = t();
                        code << "  " << old << " = load ptr, ptr @g." << in.target << "\n";
                        release(old);
                    }
                    code << "  store " << llType(k) << " " << v << ", ptr @g." << in.target << "\n";
                    break;
                }
                case Op::MkClosure: {
                    const size_t ncap = static_cast<size_t>(in.ival);
                    const pvm::Function& callee = m_.funcs[static_cast<size_t>(in.target)];
                    std::string kinds = "p";
                    for (size_t i = 0; i < ncap; ++i) kinds += kindOf(callee.sig.params[i], "a captured variable");
                    int cid;
                    auto it = closureId_.find(in.target);
                    if (it == closureId_.end()) {
                        cid = static_cast<int>(closureKinds_.size());
                        closureId_[in.target] = cid;
                        closureKinds_.push_back(kinds);
                        // thunk: (closure, declared arguments) -> result; loads the captured values and calls the function
                        std::ostringstream th;
                        const Type& rt = callee.sig.ret;
                        const std::string rty = rt.kind == Type::Void ? "void" : llType(kindOf(rt, "a return value"));
                        th << "define " << rty << " @\"t." << callee.name << "\"(ptr %clo";
                        for (size_t i = ncap; i < callee.sig.params.size(); ++i) th << ", " << llType(kindOf(callee.sig.params[i], "a parameter")) << " %a" << i;
                        th << ") {\n  %dp = getelementptr inbounds i8, ptr %clo, i64 40\n  %d = load ptr, ptr %dp\n";
                        std::string args;
                        for (size_t i = 0; i < callee.sig.params.size(); ++i) {
                            const char pk = kindOf(callee.sig.params[i], "a parameter");
                            if (i >= ncap) { args += std::string(i ? ", " : "") + llType(pk) + " %a" + std::to_string(i); continue; }
                            const std::string n = std::to_string(i);
                            th << "  %p" << n << " = getelementptr inbounds i8, ptr %d, i64 " << (i + 1) * 8 << "\n";
                            if (pk == 'b') th << "  %w" << n << " = load i32, ptr %p" << n << "\n  %c" << n << " = icmp ne i32 %w" << n << ", 0\n";
                            else th << "  %c" << n << " = load " << llType(pk) << ", ptr %p" << n << "\n";
                            if (pk == 's') th << "  call void @rt_retain(ptr %c" << n << ")\n";
                            args += std::string(i ? ", " : "") + llType(pk) + " %c" + n;
                        }
                        th << "  " << (rt.kind == Type::Void ? "call void" : "%r = call " + rty) << " @\"f." << callee.name << "\"(" << args << ")\n";
                        th << (rt.kind == Type::Void ? "  ret void\n}\n" : "  ret " + rty + " %r\n}\n");
                        thunks_ += th.str();
                    } else cid = it->second;
                    setLoc(in);
                    std::string r = t();
                    code << "  " << r << " = call ptr @rt_obj_new(i32 " << m_.classes.size() + static_cast<size_t>(cid) << ", i64 " << ncap + 1 << ", ptr " << cstr("@.empty") << ")\n";
                    std::string a = elemAddr(r, "0", false);
                    code << "  store ptr @\"t." << callee.name << "\", ptr " << a << "\n";
                    for (size_t i = 0; i < ncap; ++i) {
                        const char k = kinds[i + 1];
                        std::string v = peekk(k, ncap - 1 - i);
                        elemStore(r, std::to_string(i + 1), k == 'y' ? 'i' : k, k, v, false);
                    }
                    std::vector<Type> ptys(callee.sig.params.begin() + static_cast<std::ptrdiff_t>(ncap), callee.sig.params.end());
                    for (size_t i = 0; i < ncap; ++i) {
                        if (stack.back() == 'r') clearSlot(stack.size() - 1);
                        stack.pop_back(); tys.pop_back();
                    }
                    push('r', r, Type::fnOf(ptys, callee.sig.ret));
                    break;
                }
                case Op::CallFn: {
                    const size_t n = static_cast<size_t>(in.ival);
                    const Type ft = tys[tys.size() - n - 1];
                    if (ft.kind != Type::Fn) throw Unsupported{"an untyped function value in " + where + " (use the PVM)"};
                    std::vector<std::string> args(n);
                    for (size_t i = n; i-- > 0;) args[i] = pop(kindOf((*ft.params)[i], "an argument"));
                    std::string clo = pop('r');
                    nullCheckMsg(in, clo, "null reference: cannot call a null function value");
                    const int did = tmp_++;
                    const std::string dv = t(), dc = t(), dn = t(), dp = t(), d = t(), fp = t();
                    code << "  " << dv << " = load i32, ptr @rt_depth\n  " << dc << " = icmp sge i32 " << dv << ", 5000\n  br i1 " << dc
                         << ", label %so" << did << ", label %cd" << did << "\nso" << did << ":\n";
                    fail(in, "call stack overflow (more than 5000 nested calls; infinite recursion?)");
                    code << "cd" << did << ":\n  call void @rt_trace(i32 " << dv << ", ptr " << str(f.name == "$main" ? "<main>" : f.name) << ", ptr " << str(m_.fileName(in.srcFile)) << ", i32 " << in.srcLine
                         << ")\n  " << dn << " = add i32 " << dv << ", 1\n  store i32 " << dn << ", ptr @rt_depth\n";
                    code << "  " << dp << " = getelementptr inbounds i8, ptr " << clo << ", i64 40\n  " << d << " = load ptr, ptr " << dp << "\n  " << fp << " = load ptr, ptr " << d << "\n";
                    const bool isVoid = ft.elem->kind == Type::Void;
                    const std::string rty = isVoid ? "void" : llType(kindOf(*ft.elem, "a return value"));
                    std::string r;
                    if (isVoid) code << "  call void " << fp << "(ptr " << clo;
                    else { r = t(); code << "  " << r << " = call " << rty << " " << fp << "(ptr " << clo; }
                    for (size_t i = 0; i < n; ++i) code << ", " << llType(kindOf((*ft.params)[i], "an argument")) << " " << args[i];
                    code << ")\n  store i32 " << dv << ", ptr @rt_depth\n";
                    if (!isVoid) push(kindOf(*ft.elem, "a return value"), r, *ft.elem);
                    break;
                }
                case Op::OCopy: {
                    if (stack.back() != 'r') throw Unsupported{"a copy of a non-reference in " + where};
                    const Type ty = tys.back();
                    std::string src = peekk('r', 0), r = t();
                    setLoc(in);
                    code << "  " << r << " = call ptr @rt_ocopy(ptr " << str(f.name == "$main" ? "<main>" : f.name) << ", ptr " << str(m_.fileName(in.srcFile)) << ", i32 " << in.srcLine << ", ptr " << src << ", ptr " << cstr("@.empty") << ")\n";
                    pop('r');
                    push('r', r, ty);
                    break;
                }
                case Op::Try: {
                    if (!stack.empty()) throw Unsupported{"a try block inside an expression in " + where + " (use the PVM)"};
                    const int id = nTry++;
                    tryAllocas += "  %h" + std::to_string(id) + " = alloca [64 x i8], align 8\n";
                    const std::string h = "%h" + std::to_string(id);
                    const std::string jb = t(), sj = t(), isz = t(), m = t(), fa = t(), ss = t(), sp = t();
                    code << "  call void @rt_try_push(ptr " << h << ")\n  " << jb << " = getelementptr inbounds i8, ptr " << h << ", i64 8\n  "
                         << fa << " = call ptr @llvm.frameaddress.p0(i32 0)\n  store ptr " << fa << ", ptr " << jb << "\n  " << ss << " = call ptr @llvm.stacksave.p0()\n  "
                         << sp << " = getelementptr inbounds i8, ptr " << jb << ", i64 16\n  store ptr " << ss << ", ptr " << sp << "\n  "
                         << sj << " = call i32 @llvm.eh.sjlj.setjmp(ptr " << jb << ")\n  " << isz << " = icmp eq i32 " << sj << ", 0\n  br i1 " << isz
                         << ", label %TB" << idx << ", label %TH" << idx << "\nTH" << idx << ":\n  " << m << " = call ptr @rt_err_msg()\n";
                    slots.insert({0, 's'});
                    code << "  store ptr " << m << ", ptr %stk.0.s\n  br label %B" << in.target << "\nTB" << idx << ":\n";
                    stateAt.emplace(static_cast<size_t>(in.target), std::make_pair(std::vector<char>{'s'}, std::vector<Type>{Type(Type::String)}));
                    break;
                }
                case Op::EndTry: code << "  call void @rt_try_pop()\n"; break;
                case Op::Throw: case Op::Throwx: {
                    std::string tr = "null", pay = "null";
                    if (in.op == Op::Throwx) tr = pop('s');
                    std::string msg = pop('s');
                    if (in.op == Op::Throwx) pay = pop('r');
                    code << "  call void @rt_raise(ptr " << msg << ", ptr " << tr << ", ptr " << pay << ", ptr " << str(f.name == "$main" ? "<main>" : f.name) << ", ptr " << str(m_.fileName(in.srcFile)) << ", i32 " << in.srcLine << ")\n  unreachable\n";
                    terminated = true;
                    break;
                }
                case Op::Box: {
                    const char bk = stack.back();
                    if (bk == 'r') {
                        const Type bt = tys.back();
                        if (bt.kind == Type::Class || bt.kind == Type::Obj || (bt.kind == Type::Nullable && bt.elem->kind == Type::Class)) {
                            std::string v = pop('r');
                            push('r', v, Type(Type::Obj));
                            break;
                        }
                        if (const int rt = refBoxTag(bt)) {
                            const int tags = rt == 'A' ? boxTag(*bt.elem) : (boxTag(*bt.key) << 8) | boxTag(*bt.elem);
                            std::string p = peekk('r', 0), r = t(), bits = t();
                            setLoc(in);
                            code << "  " << r << " = call ptr @rt_box_ref(i32 " << rt << ", ptr " << p << ", ptr " << str(pvm::typeName(bt)) << ", i32 " << tags << ")\n";
                            clearSlot(stack.size() - 1);
                            stack.pop_back(); tys.pop_back();
                            push('r', r, Type(Type::Obj));
                            (void)bits;
                            break;
                        }
                        throw Unsupported{"boxing a " + pvm::typeName(bt) + " into obj in " + where + " is not supported yet (use the PVM)"};
                    }
                    const int tag = bk == 'i' ? 'i' : bk == 'l' ? 'l' : bk == 'y' ? 'y' : bk == 'b' ? 'b' : bk == 'c' ? 'c' : bk == 'd' ? 'd' : 's';
                    std::string v = pop(bk), bits = v, r = t();
                    if (bk == 'i' || bk == 'y' || bk == 'c') { bits = t(); code << "  " << bits << " = sext i32 " << v << " to i64\n"; }
                    else if (bk == 'b') { bits = t(); code << "  " << bits << " = zext i1 " << v << " to i64\n"; }
                    else if (bk == 'd') { bits = t(); code << "  " << bits << " = bitcast double " << v << " to i64\n"; }
                    else if (bk == 's') { bits = t(); code << "  " << bits << " = ptrtoint ptr " << v << " to i64\n"; }
                    setLoc(in);
                    code << "  " << r << " = call ptr @rt_box(i32 " << tag << ", i64 " << bits << ")\n";
                    push('r', r, Type(Type::Obj));
                    break;
                }
                case Op::IsType: {
                    const Type& want = in.type;
                    if (stack.back() != 'r') throw Unsupported{"a type test of a non-obj in " + where};
                    int tag = 0;
                    size_t ci = 0;
                    bool refType = false;
                    switch (want.kind) {
                        case Type::Int: tag = 'i'; break; case Type::Long: tag = 'l'; break; case Type::Byte: tag = 'y'; break;
                        case Type::Bool: tag = 'b'; break; case Type::Char: tag = 'c'; break; case Type::Double: tag = 'd'; break;
                        case Type::String: tag = 's'; break;
                        case Type::Class:
                            tag = 'o';
                            while (ci < m_.classes.size() && m_.classes[ci].name != want.name) ++ci;
                            break;
                        default:
                            if (const int rt = refBoxTag(want)) { tag = rt; refType = true; break; }
                            throw Unsupported{"a type test for " + pvm::typeName(want) + " in " + where + " is not supported yet (use the PVM)"};
                    }
                    std::string v = pop('r'), r = t();
                    if (refType) code << "  " << r << " = call zeroext i1 @rt_istype_ref(ptr " << v << ", i32 " << tag << ", ptr " << str(pvm::typeName(want)) << ")\n";
                    else code << "  " << r << " = call zeroext i1 @rt_istype(ptr " << v << ", i32 " << tag << ", i32 " << ci << ")\n";
                    push('b', r);
                    break;
                }
                case Op::Unbox: {
                    const Type want = in.type.kind == Type::Nullable ? *in.type.elem : in.type;
                    const bool nullable = in.type.kind == Type::Nullable;
                    const bool scalar = want.kind == Type::Int || want.kind == Type::Long || want.kind == Type::Byte || want.kind == Type::Bool ||
                                        want.kind == Type::Char || want.kind == Type::Double || want.kind == Type::String;
                    if (stack.back() == 'r' && !nullable && refBoxTag(want)) {
                        std::string v = pop('r'), r = t();
                        const std::string loc2 = "ptr " + str(f.name == "$main" ? "<main>" : f.name) + ", ptr " + str(m_.fileName(in.srcFile)) + ", i32 " + std::to_string(in.srcLine);
                        code << "  " << r << " = call ptr @rt_unbox_ref(" << loc2 << ", ptr " << v << ", i32 " << refBoxTag(want) << ", ptr " << str(pvm::typeName(want)) << ")\n";
                        push('r', r, want);
                        break;
                    }
                    if (stack.back() != 'r' || (!scalar && want.kind != Type::Class && want.kind != Type::Obj) || (nullable && want.kind != Type::Class))
                        throw Unsupported{"unboxing to " + pvm::typeName(in.type) + " in " + where + " is not supported yet (use the PVM)"};
                    std::string v = pop('r'), r = t();
                    const std::string loc = "ptr " + str(f.name == "$main" ? "<main>" : f.name) + ", ptr " + str(m_.fileName(in.srcFile)) + ", i32 " + std::to_string(in.srcLine);
                    if (want.kind == Type::Obj) push('r', v, Type(Type::Obj));
                    else if (scalar) {
                        const char k = kindOf(want, "an unboxed value");
                        const int tag = want.kind == Type::Int ? 'i' : want.kind == Type::Long ? 'l' : want.kind == Type::Byte ? 'y' : want.kind == Type::Bool ? 'b' :
                                        want.kind == Type::Char ? 'c' : want.kind == Type::Double ? 'd' : 's';
                        code << "  " << r << " = call i64 @rt_unbox_bits(" << loc << ", ptr " << v << ", i32 " << tag << ", ptr " << str(pvm::typeName(in.type)) << ")\n";
                        std::string x = t();
                        if (k == 'i' || k == 'y' || k == 'c') code << "  " << x << " = trunc i64 " << r << " to i32\n";
                        else if (k == 'b') code << "  " << x << " = trunc i64 " << r << " to i1\n";
                        else if (k == 'd') code << "  " << x << " = bitcast i64 " << r << " to double\n";
                        else if (k == 's') code << "  " << x << " = inttoptr i64 " << r << " to ptr\n";
                        else x = r;
                        push(k, x, want);
                    } else {
                        size_t ci = 0;
                        while (ci < m_.classes.size() && m_.classes[ci].name != want.name) ++ci;
                        code << "  " << r << " = call ptr @rt_unbox_class(" << loc << ", ptr " << v << ", i32 " << ci << ", i32 " << (nullable ? 1 : 0) << ")\n";
                        push('r', r, in.type);
                    }
                    break;
                }
                case Op::DynAdd: case Op::DynSub: case Op::DynMul: case Op::DynDiv: case Op::DynRem:
                case Op::DynEq: case Op::DynNe: case Op::DynLt: case Op::DynLe: case Op::DynGt: case Op::DynGe: {
                    const int opn = in.op == Op::DynAdd ? 0 : in.op == Op::DynSub ? 1 : in.op == Op::DynMul ? 2 : in.op == Op::DynDiv ? 3 : in.op == Op::DynRem ? 4 :
                                    in.op == Op::DynEq ? 5 : in.op == Op::DynNe ? 6 : in.op == Op::DynLt ? 7 : in.op == Op::DynLe ? 8 : in.op == Op::DynGt ? 9 : 10;
                    std::string y = peekk('r', 0), x = peekk('r', 1), r = t();
                    setLoc(in);
                    code << "  " << r << " = call ptr @rt_dyn_bin(" << dynLoc(in) << ", i32 " << opn << ", ptr " << x << ", ptr " << y << ")\n";
                    clearSlot(stack.size() - 1);
                    stack.pop_back(); tys.pop_back();
                    clearSlot(stack.size() - 1);
                    stack.pop_back(); tys.pop_back();
                    push('r', r, Type(Type::Obj));
                    break;
                }
                case Op::DynNeg: case Op::DynNot: {
                    std::string x = peekk('r', 0), r = t();
                    setLoc(in);
                    code << "  " << r << " = call ptr @rt_dyn_un(" << dynLoc(in) << ", i32 " << (in.op == Op::DynNeg ? 0 : 1) << ", ptr " << x << ")\n";
                    clearSlot(stack.size() - 1);
                    stack.pop_back(); tys.pop_back();
                    push('r', r, Type(Type::Obj));
                    break;
                }
                case Op::DynIndex: {
                    std::string i = peekk('r', 0), c = peekk('r', 1), r = t();
                    setLoc(in);
                    code << "  " << r << " = call ptr @rt_dyn_index(" << dynLoc(in) << ", ptr " << c << ", ptr " << i << ")\n";
                    clearSlot(stack.size() - 1);
                    stack.pop_back(); tys.pop_back();
                    clearSlot(stack.size() - 1);
                    stack.pop_back(); tys.pop_back();
                    push('r', r, Type(Type::Obj));
                    break;
                }
                case Op::DynMember: {
                    std::string c = peekk('r', 0), r = t();
                    setLoc(in);
                    code << "  " << r << " = call ptr @rt_dyn_member(" << dynLoc(in) << ", ptr " << str(in.str) << ", ptr " << c << ")\n";
                    clearSlot(stack.size() - 1);
                    stack.pop_back(); tys.pop_back();
                    push('r', r, Type(Type::Obj));
                    break;
                }
                case Op::DynInvoke: {
                    const size_t argc = static_cast<size_t>(in.ival);
                    std::string a0 = argc > 0 ? peekk('r', argc - 1) : std::string("null"), c = peekk('r', argc), r = t();
                    setLoc(in);
                    code << "  " << r << " = call ptr @rt_dyn_invoke(" << dynLoc(in) << ", ptr " << str(in.str) << ", i32 " << argc << ", ptr " << c << ", ptr " << a0 << ")\n";
                    for (size_t n = 0; n <= argc; ++n) { clearSlot(stack.size() - 1); stack.pop_back(); tys.pop_back(); }
                    push('r', r, Type(Type::Obj));
                    break;
                }
                case Op::DynSetIndex: {
                    std::string v = peekk('r', 0), i = peekk('r', 1), c = peekk('r', 2);
                    setLoc(in);
                    code << "  call void @rt_dyn_setindex(" << dynLoc(in) << ", ptr " << c << ", ptr " << i << ", ptr " << v << ", ptr " << cstr("@.empty") << ")\n";
                    for (int n = 0; n < 3; ++n) { clearSlot(stack.size() - 1); stack.pop_back(); tys.pop_back(); }
                    break;
                }
                case Op::EqObj: case Op::NeObj: {
                    std::string y = pop('r'), x = pop('r'), r = t();
                    code << "  " << r << " = call zeroext i1 @rt_obj_eq(ptr " << x << ", ptr " << y << ")\n";
                    if (in.op == Op::NeObj) { std::string n = t(); code << "  " << n << " = xor i1 " << r << ", true\n"; r = n; }
                    push('b', r);
                    break;
                }
                case Op::ToStrObj: {
                    if (stack.back() != 'r') throw Unsupported{"tostr.obj of a non-obj in " + where};
                    std::string v = pop('r'), r = t();
                    code << "  " << r << " = call ptr @rt_obj_str(ptr " << v << ")\n";
                    push('s', r);
                    break;
                }
                case Op::GetMouth: {
                    const std::string& mn = m_.mouths[static_cast<size_t>(in.host)].name;
                    std::string r = t();
                    setLoc(in);
                    if (mn == "sys.exception") code << "  " << r << " = call ptr @rt_mouth_str(i32 0)\n";
                    else if (mn == "sys.stackTrace") code << "  " << r << " = call ptr @rt_mouth_str(i32 1)\n";
                    else if (mn == "sys.exceptionObject") code << "  " << r << " = call ptr @rt_mouth_payload()\n";
                    else if (mn == "sys.os") code << "  " << r << " = call ptr @rt_mouth_sys(i32 0, ptr null)\n";
                    else if (mn == "sys.version") code << "  " << r << " = call ptr @rt_mouth_sys(i32 1, ptr " << str(COYOTE_VERSION) << ")\n";
                    else if (mn == "sys.pid") code << "  " << r << " = call ptr @rt_mouth_sys(i32 2, ptr null)\n";
                    else if (mn == "sys.exitCode") code << "  " << r << " = call ptr @rt_mouth_sys(i32 3, ptr null)\n";
                    else throw Unsupported{"the mouth '" + mn + "' is not available in native code yet (use the PVM)"};
                    push('r', r, Type(Type::Obj));
                    break;
                }
                case Op::SetMouth: {
                    const std::string& mn = m_.mouths[static_cast<size_t>(in.host)].name;
                    if (mn != "sys.exitCode" || stack.back() != 'r') throw Unsupported{"setting the mouth '" + mn + "' is not available in native code yet (use the PVM)"};
                    std::string v = pop('r');
                    code << "  call void @rt_set_exit(ptr " << str(f.name == "$main" ? "<main>" : f.name) << ", ptr " << str(m_.fileName(in.srcFile)) << ", i32 " << in.srcLine << ", ptr " << v << ")\n";
                    break;
                }
                case Op::HostCall: {
                    const pvm::HostDecl& h = m_.hosts[static_cast<size_t>(in.host)];
                    // native host functions: name -> {C function, takes the call site (for error messages), math opcode}
                    struct NH { const char* c; bool loc; int math; };
                    static const std::map<std::string, NH> nat = {
                        {"con.print", {"rt_print", false, -1}}, {"con.println", {"rt_println", false, -1}},
                        {"con.readLine", {"rt_h_con_readLine", false, -1}}, {"con.hasInput", {"rt_h_con_hasInput", false, -1}},
                        {"text.length", {"rt_h_text_length", false, -1}}, {"text.substring", {"rt_h_text_substring", true, -1}},
                        {"text.charAt", {"rt_h_text_charAt", true, -1}}, {"text.indexOf", {"rt_h_text_indexOf", false, -1}},
                        {"text.replace", {"rt_h_text_replace", true, -1}}, {"text.upper", {"rt_h_text_upper", false, -1}},
                        {"text.lower", {"rt_h_text_lower", false, -1}}, {"text.trim", {"rt_h_text_trim", false, -1}},
                        {"time.uptimeMs", {"rt_h_time_uptimeMs", false, -1}}, {"time.unixSeconds", {"rt_h_time_unixSeconds", false, -1}},
                        {"time.sleep", {"rt_h_time_sleep", true, -1}}, {"sys.exit", {"rt_h_sys_exit", false, -1}},
                        {"sys.cwd", {"rt_h_sys_cwd", false, -1}}, {"sys.os", {"rt_h_sys_os", false, -1}},
                        {"env.get", {"rt_h_env_get", false, -1}}, {"env.has", {"rt_h_env_has", false, -1}},
                        {"file.exists", {"rt_h_file_exists", true, -1}}, {"dir.exists", {"rt_h_dir_exists", true, -1}},
                        {"sys.argCount", {"rt_h_sys_argCount", false, -1}}, {"sys.homeDir", {"rt_h_sys_homeDir", false, -1}},
                        {"proc.pid", {"rt_h_proc_pid", false, -1}}, {"proc.which", {"rt_h_proc_which", false, -1}}, {"rt.arch", {"rt_h_rt_arch", false, -1}}, {"rt.cpuCount", {"rt_h_rt_cpuCount", false, -1}}, {"rt.memoryBytes", {"rt_h_rt_memoryBytes", false, -1}}, {"rt.machine", {"rt_h_rt_machine", false, -1}}, {"rt.user", {"rt_h_rt_user", false, -1}},
                        {"rt.gcCollections", {"rt_h_rt_gcCollections", false, -1}}, {"rt.gcFreed", {"rt_h_rt_gcFreed", false, -1}}, {"rt.heapObjects", {"rt_h_rt_heapObjects", false, -1}}, {"rt.heapElements", {"rt_h_rt_heapElements", false, -1}},
                        {"random.seed", {"rt_h_random_seed", false, -1}}, {"random.nextInt", {"rt_h_random_nextInt", true, -1}}, {"random.nextDouble", {"rt_h_random_nextDouble", false, -1}}, {"sys.arg", {"rt_h_sys_arg", true, -1}}, {"env.set", {"rt_h_env_set", true, -1}},
                        {"file.copy", {"rt_h_file_copy", true, -1}}, {"file.move", {"rt_h_file_move", true, -1}}, {"file.modified", {"rt_h_file_modified", true, -1}},
                        {"file.fullPath", {"rt_h_file_fullPath", true, -1}}, {"dir.create", {"rt_h_dir_create", true, -1}}, {"dir.delete", {"rt_h_dir_delete", true, -1}}, {"dir.list", {"rt_h_dir_list", true, -1}},
                        {"file.readAll", {"rt_h_file_readAll", true, -1}}, {"file.readBytes", {"rt_h_file_readBytes", true, -1}},
                        {"file.size", {"rt_h_file_size", true, -1}}, {"time.format", {"rt_h_time_format", true, -1}}, {"encoding.toBytes", {"rt_h_encoding_toBytes", false, -1}}, {"encoding.fromBytes", {"rt_h_encoding_fromBytes", true, -1}}, {"file.delete", {"rt_h_file_delete", true, -1}},
                        {"math.sqrt", {"", true, 0}}, {"math.sin", {"", true, 1}}, {"math.cos", {"", true, 2}}, {"math.tan", {"", true, 3}},
                        {"math.asin", {"", true, 4}}, {"math.acos", {"", true, 5}}, {"math.atan", {"", true, 6}}, {"math.exp", {"", true, 7}},
                        {"math.log", {"", true, 8}}, {"math.log10", {"", true, 9}}, {"math.floor", {"", true, 10}}, {"math.ceil", {"", true, 11}},
                        {"math.round", {"", true, 12}}, {"math.pow", {"", true, 13}}, {"math.atan2", {"", true, 14}},
                    };
                    if (h.name == "rt.version") {  // the version of the toolchain that built the program
                        push('s', str(COYOTE_VERSION), h.sig.ret);
                        break;
                    }
                    const bool isWrite = h.name == "file.writeAll" || h.name == "file.append";
                    const bool isWriteB = h.name == "file.writeBytes" || h.name == "file.appendBytes";
                    auto it = nat.find(h.name);
                    if (it == nat.end() && !isWrite && !isWriteB)
                        throw Unsupported{"host function '" + h.name + "' is not available in native code yet (use the PVM)"};
                    const size_t argc = static_cast<size_t>(in.ival);
                    std::vector<char> ak(argc);
                    std::vector<std::string> av(argc);
                    for (size_t i = 0; i < argc; ++i) ak[i] = kindOf(h.sig.params[i], "an argument of " + h.name);
                    for (size_t i = argc; i-- > 0;) av[i] = pop(ak[i]);
                    const std::string where_ = "ptr " + str(f.name == "$main" ? "<main>" : f.name) + ", ptr " + str(m_.fileName(in.srcFile)) + ", i32 " + std::to_string(in.srcLine);
                    const bool isVoid = h.sig.ret.kind == Type::Void;
                    const char rk = isVoid ? 'v' : kindOf(h.sig.ret, "the result of " + h.name);
                    std::string r;
                    std::ostringstream call;
                    if (rk != 'v') { r = t(); call << "  " << r << " = call " << (rk == 'b' ? "zeroext i1" : llType(rk)) << " "; }
                    else call << "  call void ";
                    std::string args;
                    auto add = [&](const std::string& x) { args += (args.empty() ? "" : ", ") + x; };
                    if (isWriteB) { call << "@rt_h_file_writeBytes("; add(where_); add("ptr " + av[0]); add("ptr " + av[1]); add(std::string("i32 ") + (h.name == "file.appendBytes" ? "1" : "0")); }
                    else if (isWrite) { call << "@rt_h_file_write("; add(where_); add("ptr " + av[0]); add("ptr " + av[1]); add(std::string("i32 ") + (h.name == "file.append" ? "1" : "0")); }
                    else if (it->second.math >= 0) {
                        call << "@rt_h_math("; add(where_); add("i32 " + std::to_string(it->second.math));
                        add("double " + av[0]); add("double " + (argc > 1 ? av[1] : std::string("0.0")));
                    } else {
                        call << "@" << it->second.c << "(";
                        if (it->second.loc) add(where_);
                        for (size_t i = 0; i < argc; ++i) add(std::string(ak[i] == 'b' ? "i1 zeroext" : llType(ak[i])) + " " + av[i]);
                    }
                    call << args << ")\n";
                    setLoc(in);
                    code << call.str();
                    for (size_t i = 0; i < argc; ++i) if (ak[i] == 's') release(av[i]);
                    if (rk != 'v') push(rk, r, h.sig.ret);
                    break;
                }
                case Op::Pop: {
                    const char k = stack.back();
                    std::string v = pop(k);
                    if (k == 's') release(v);
                    break;
                }
                case Op::Ret:
                    {
                        std::string v;
                        if (retKind) v = pop(retKind);  // owned: handed to the caller
                        while (!stack.empty()) { const char k = stack.back(); std::string x = pop(k); if (k == 's') release(x); }
                        releaseLocals();
                        if (hasTry) code << "  call void @rt_handlers_set(ptr %hs0)\n";
                        code << ";;FRAMEPOP\n";
                        if (retKind) code << "  ret " << llType(retKind) << " " << v << "\n";
                        else code << "  ret void\n";
                    }
                    terminated = true;
                    break;
                default:
                    throw Unsupported{"instruction '" + std::string(pvm::opName(in.op)) + "' (ML line " + std::to_string(in.mlLine) + ", " + where +
                                      ") is not supported by the LLVM backend (use the PVM)"};
            }
        }
        if (!terminated && !f.code.empty()) code << "  unreachable\n";

        std::ostringstream o;
        // Reference slots: one frame array per function (locals first, then one slot per operand-stack depth that ever holds a reference).
        std::map<size_t, size_t> refLoc;
        for (size_t i = 0; i < slotKinds.size(); ++i) if (slotKinds[i] == 'r') refLoc[i] = refLoc.size();
        int maxRefDepth = -1;
        for (const auto& sl : slots) if (sl.second == 'r') maxRefDepth = std::max(maxRefDepth, sl.first);
        const size_t frameSize = refLoc.size() + static_cast<size_t>(maxRefDepth + 1);
        o << "define internal " << (retKind ? llType(retKind) : "void") << " @\"f." << f.name << "\"(";
        for (size_t i = 0; i < f.sig.params.size(); ++i) o << (i ? ", " : "") << llType(slotKinds[i]) << " %arg" << i;
        o << ") {\nentry:\n  %okp = alloca i32\n" << tryAllocas;
        if (hasTry) o << "  %hs0 = call ptr @rt_handlers_get()\n";
        if (frameSize > 0) {
            o << "  %fr = alloca [" << frameSize << " x ptr]\n  store [" << frameSize << " x ptr] zeroinitializer, ptr %fr\n  %frm = alloca [24 x i8], align 8\n"
              << "  call void @rt_frame_push(ptr %frm, ptr %fr, i64 " << frameSize << ")\n";
        }
        for (size_t i = 0; i < slotKinds.size(); ++i) {
            if (slotKinds[i] == 'r') o << "  %loc." << i << " = getelementptr inbounds ptr, ptr %fr, i64 " << refLoc[i] << "\n";
            else o << "  %loc." << i << " = alloca " << llType(slotKinds[i]) << "\n";
            if (i < f.sig.params.size()) o << "  store " << llType(slotKinds[i]) << " %arg" << i << ", ptr %loc." << i << "\n";
            else if (slotKinds[i] != 'r') o << "  store " << llType(slotKinds[i]) << " " << (slotKinds[i] == 's' ? cstr("@.empty") : slotKinds[i] == 'b' ? "false" : slotKinds[i] == 'd' ? "0.0" : "0") << ", ptr %loc." << i << "\n";
        }
        for (const auto& sl : slots) {
            if (sl.second == 'r') o << "  %stk." << sl.first << ".r = getelementptr inbounds ptr, ptr %fr, i64 " << refLoc.size() + static_cast<size_t>(sl.first) << "\n";
            else o << "  %stk." << sl.first << "." << sl.second << " = alloca " << llType(sl.second) << "\n";
        }
        std::string text = code.str();
        const std::string marker = ";;FRAMEPOP\n";
        for (size_t p; (p = text.find(marker)) != std::string::npos;)
            text.replace(p, marker.size(), frameSize > 0 ? "  call void @rt_frame_pop(ptr %frm)\n" : "");
        o << text << "}\n\n";
        body_ << o.str();
    }
};

void usage(std::ostream& os) {
    os << "coyote-llvm " COYOTE_VERSION " - native backend (LLVM IR + clang) for a subset of ML\n"
       << "usage: coyote-llvm [options] <program.ml>\n"
       << "  -o <file>        output file (default: a.out, or <name>.ll with --emit-ll; '-' writes IR to stdout)\n"
       << "  --emit-ll        write LLVM IR only; do not run clang\n"
       << "  --clang <path>   clang executable (default: clang)\n"
       << "  --bundle         always build a bundled executable (the PVM with the program embedded; any program works)\n"
       << "  --native-only    fail instead of falling back to a bundled executable when the program is outside the native subset\n"
       << "Default: programs inside the native subset (int, long, byte, double, char, boolean, string; calls; control flow; console print/println)\n"
       << "become real LLVM/clang executables; every other program becomes a bundled executable. See docs/llvm.md.\n"
       << "exit codes: 0 ok, 1 ML error or unsupported program, 2 usage, I/O or toolchain error\n";
}

} // namespace

namespace {
// Directory of this executable ("" when unknown).
std::filesystem::path selfDir() {
    namespace fs = std::filesystem;
    std::error_code ec;
#ifdef _WIN32
    char buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof buf));
    if (n == 0 || n >= sizeof buf) return {};
    return fs::path(std::string(buf, n)).parent_path();
#else
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : p.parent_path();
#endif
}

// The clang to use when --clang is not given: the one installed with Coyote (<install>\clang\bin, next to <install>\bin),
// then plain "clang" from the PATH.
std::string findClang() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path here = selfDir();
    if (!here.empty()) {
        for (const char* rel : {"../clang/bin", "clang/bin", "../llvm/bin"}) {
            for (const char* name : {"x86_64-w64-mingw32-clang.exe", "clang.exe", "clang"}) {
                fs::path c = here / rel / name;
                if (fs::is_regular_file(c, ec)) return fs::weakly_canonical(c, ec).string();
            }
        }
    }
    return "clang";
}
}  // namespace

// A bundled executable is a copy of `pvm` followed by the ML text, an 8-byte little-endian length and the magic
// "CYBNDL01". At start-up pvm looks for that trailer in its own file and runs the embedded program with every
// permission (it is the user's own program), passing the command line to it. Works for every ML program.
int writeBundle(const std::string& mlText, const std::string& output) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path here = selfDir();
    fs::path pvmExe;
    for (const char* rel : {".", "../bin"}) {
        for (const char* name : {"pvm.exe", "pvm"}) {
            fs::path c = here / rel / name;
            if (!here.empty() && fs::is_regular_file(c, ec)) { pvmExe = c; break; }
        }
        if (!pvmExe.empty()) break;
    }
    if (pvmExe.empty()) {
        std::cerr << "coyote-llvm: error: cannot find 'pvm' next to coyote-llvm; a bundled executable is built from it\n";
        return 2;
    }
    std::ifstream in(pvmExe, std::ios::binary);
    std::ostringstream bytes;
    bytes << in.rdbuf();
    std::string image = bytes.str();
    if (image.size() >= 16 && image.compare(image.size() - 8, 8, "CYBNDL01") == 0) {
        std::cerr << "coyote-llvm: error: '" << pvmExe.string() << "' already contains a bundled program\n";
        return 2;
    }
    image += mlText;
    unsigned long long len = mlText.size();
    for (int i = 0; i < 8; ++i) image += static_cast<char>((len >> (8 * i)) & 0xFF);
    image += "CYBNDL01";
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    if (!out || !(out.write(image.data(), static_cast<std::streamsize>(image.size())))) {
        std::cerr << "coyote-llvm: error: cannot write '" << output << "'\n";
        return 2;
    }
    out.close();
    fs::permissions(output, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
    return 0;
}

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string input, output, clang = "clang";
    bool emitLl = false, forceBundle = false, nativeOnly = false;
    if (args.empty()) { usage(std::cerr); return 2; }
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--help" || a == "-h") { usage(std::cout); return 0; }
        if (a == "--version") { std::cout << "coyote-llvm " COYOTE_VERSION "\n"; return 0; }
        if (a == "--emit-ll") { emitLl = true; continue; }
        if (a == "--bundle") { forceBundle = true; continue; }
        if (a == "--native-only") { nativeOnly = true; continue; }
        if (a == "-o" || a == "--clang") {
            if (i + 1 >= args.size()) { std::cerr << "coyote-llvm: error: '" << a << "' needs a value\n"; return 2; }
            (a == "-o" ? output : clang) = args[++i];
            continue;
        }
        if (!a.empty() && a[0] == '-') { std::cerr << "coyote-llvm: error: unknown option '" << a << "'\n"; return 2; }
        if (!input.empty()) { std::cerr << "coyote-llvm: error: only one program file is supported\n"; return 2; }
        input = a;
    }
    if (input.empty()) { std::cerr << "coyote-llvm: error: no program file\n"; return 2; }

    std::ifstream in(input, std::ios::binary);
    if (!in) { std::cerr << "coyote-llvm: error: cannot read '" << input << "'\n"; return 2; }
    std::ostringstream ss;
    ss << in.rdbuf();

    const std::string mlText = ss.str();
    {   // the program must be valid ML for the full PVM (every host function, every permission)
        pvm::HostRegistry full;
        pvm::registerAllHosts(full);
        pvm::grantAllPermissions(full);
        try {
            pvm::Module m = pvm::loadMl(mlText, input);
            pvm::verify(m, full, input);
        } catch (const pvm::MlError& e) {
            std::cerr << pvm::formatMlError(e) << "\n";
            return 1;
        }
    }
    namespace fs = std::filesystem;
    auto bundle = [&]() {
        if (output.empty()) output = fs::path(input).stem().string() + (
#ifdef _WIN32
            ".exe"
#else
            ""
#endif
        );
        if (output == "-") { std::cerr << "coyote-llvm: error: a bundled executable cannot be written to stdout\n"; return 2; }
        return writeBundle(mlText, output);
    };
    if (forceBundle && !emitLl) return bundle();

    pvm::HostRegistry registry;
    pvm::registerAllHosts(registry);  // so that every program verifies; only the subset below is compiled natively
    pvm::Module module;
    std::vector<std::string> problems;
    std::string ir;
    try {
        module = pvm::loadMl(mlText, input);
        pvm::verify(module, registry, input);
        ir = Gen(module).run(problems);
    } catch (const pvm::MlError& e) {  // valid ML, but it uses a host function the native subset does not have
        std::string msg = pvm::formatMlError(e);
        if (msg.find("is not registered with this PVM") != std::string::npos)
            msg = "the program uses a host function that the native subset does not have (" + msg + ")";
        else if (msg.find("is not provided by this PVM") != std::string::npos)
            msg = "the program uses a mouth (host variable), which the native subset does not have yet (" + msg + ")";
        problems.push_back(msg);
    }
    if (!problems.empty()) {
        if (!nativeOnly && !emitLl) {
            std::cerr << "coyote-llvm: note: " << problems.front() << "\n"
                      << "coyote-llvm: note: the program is outside the native subset, so a bundled executable is built instead (the PVM with the program embedded)\n";
            return bundle();
        }
        for (const std::string& p : problems) std::cerr << "coyote-llvm: error: " << p << "\n";
        return 1;
    }

    if (emitLl) {
        if (output.empty()) output = fs::path(input).stem().string() + ".ll";
        if (output == "-") { std::cout << ir; return 0; }
        std::ofstream out(output, std::ios::binary);
        out << ir;
        if (!out) { std::cerr << "coyote-llvm: error: cannot write '" << output << "'\n"; return 2; }
        return 0;
    }
    if (output.empty()) output = "a.out";
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / ("coyote-llvm-" + std::to_string(std::random_device{}()) + std::to_string(std::random_device{}()));
    bool made = fs::create_directory(dir, ec);  // fails if the name is taken, so two runs never share a directory
    if (ec || !made) { std::cerr << "coyote-llvm: error: cannot create a temporary directory\n"; return 2; }
    {
        std::ofstream(dir / "program.ll", std::ios::binary) << ir;
        std::ofstream(dir / "runtime.c", std::ios::binary) << kRuntime;
    }
    auto quote = [](const std::string& s) { return "\"" + s + "\""; };
    if (clang == "clang") clang = findClang();
    std::string cmd = quote(clang) + " -O2 -Wno-override-module -o " + quote(output) + " " + quote((dir / "program.ll").string()) + " " +
                      quote((dir / "runtime.c").string());
#ifndef _WIN32
    cmd += " -lm";
#endif
#ifdef _WIN32
    cmd += " -Wl,--stack,16777216";  // 5000 nested calls need more than the 1 MiB default stack of a Windows program
    // cmd.exe strips the first and the last quote of a command line that starts with a quote; wrapping the whole
    // command keeps every path (including ones with spaces, like "C:\Users\First Last\...") intact.
    cmd = "\"" + cmd + "\"";
#endif
    int rc = std::system(cmd.c_str());
    fs::remove_all(dir, ec);
    if (rc != 0) {
        std::cerr << "coyote-llvm: error: clang failed (command: " << cmd << ")\n"
                  << "  Native builds need clang. The Coyote setup installs it with the \"Native builds\" component (folder 'clang' next to 'bin');\n"
                  << "  otherwise install clang yourself, or use --clang <path>, or --emit-ll to get the LLVM IR only.\n";
        return 2;
    }
    return 0;
}
