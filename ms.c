/*
 * Machdyne Scheme
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Redistribution and use in source, binary or physical forms, with or
 * without modification, is permitted provided that the following condition
 * is met:
 *
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * THIS HARDWARE, SOFTWARE, DATA AND/OR DOCUMENTATION ("THE ASSETS") IS
 * PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
 * OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE ASSETS OR THE USE OR OTHER
 * DEALINGS IN THE ASSETS. USE AT YOUR OWN RISK.
 *
 * Originally forked from the LIX version of "Lone Lisp". Renamed due to
 * a more popular project with the same name that came along later.
 *
 * This revision: fixes broken GC, adds proper tail calls, and targets very
 * small memory footprints (a static cell pool, no fopen()/file I/O when
 * built with -DLIX).
 *
 * Build (desktop):
 *   $ gcc -DMS_HOST -o ms ms.c -lm
 *   $ ./ms
 *   ms> (load "ms_stdlib.l")
 *
 * Build (embedded / LIX, no file I/O, tiny heap):
 *   $ gcc -DLIX -DMS_HEAP_SIZE=512 -c ms.c
 *   (link against your platform; call ms_init_lix(true) to boot with the
 *   stdlib compiled in via ms_stdlib.h -- see tools/embed_stdlib.sh)
 *
 * See README.md for full language + embedding documentation.
 */

#ifndef LIX
/* needed for getline() on some libcs/std= settings; the desktop REPL uses
 * it for dynamically-sized, multi-line-aware input (see repl() below). */
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <setjmp.h>

#ifdef LIX
#include "fs.h"
#include "ms_stdlib.h"
#endif

/* ----------------------------------------------------------------------
 * CONFIGURATION
 * ------------------------------------------------------------------- */

/* Number of cons/value cells in the static heap. Each cell is small
 * (type + mark + a union, typically 24-32 bytes depending on platform).
 * On a system with ~2KB total RAM you would set this very low (e.g. 32-64)
 * and expect to run only small scripts -- the point is that the engine
 * itself makes no other heap-scaling assumptions. Tune to taste. */
#ifndef MS_HEAP_SIZE
#define MS_HEAP_SIZE 16384
#endif

/* Maximum depth of the GC protection ("shadow") stack. This bounds how
 * many not-yet-reachable intermediate values can be alive at once during
 * evaluation. It scales with *non-tail* call nesting (proper tail calls
 * use O(1) protect-stack space via the trampoline in ms_eval), so deeply
 * recursive but non-tail-recursive Lisp code needs a deeper stack here.
 * Each slot is one pointer (8 bytes on a 64-bit host, 4 on a 32-bit MCU).
 * Tune down for tiny MCU targets -- just expect correspondingly shallower
 * non-tail recursion before a clean "protect stack overflow" error. */
#ifndef MS_PROTECT_STACK_SIZE
#define MS_PROTECT_STACK_SIZE 16384
#endif

/* Maximum length of a single token (symbol, string, number literal). */
#ifndef MS_MAX_TOKEN 
#define MS_MAX_TOKEN 256
#endif

/* ----------------------------------------------------------------------
 * TYPES
 * ------------------------------------------------------------------- */

typedef struct ms_val ms_val;

typedef ms_val *(*ms_builtin)(ms_val *args);

typedef enum {
	T_FREE = 0,  /* cell is on the free list, not a live value */
	T_NIL,       /* the empty list. NOTE: unlike #f, this is a TRUE value --
	              * see truthy() below; R4RS requires only #f to be false. */
	T_BOOL,      /* #t / #f -- the only two instances are g_true/g_false */
	T_SYM,       /* interned symbol */
	T_NUM,       /* double */
	T_CHAR,      /* a character (code point) */
	T_STR,       /* heap-allocated (malloc'd) string */
	T_PAIR,      /* cons cell: car/cdr. also used for environments/frames */
	T_VECTOR,    /* heap-allocated (malloc'd) array of ms_val* */
	T_FUN,       /* builtin C function */
	T_LAMBDA,    /* user-defined closure */
	T_PROMISE,   /* delay/force */
	T_CONT,      /* an escape-only continuation captured by call/cc */
	T_MACRO      /* define-macro: a closure called with its arguments
	              * unevaluated, whose result is then evaluated in the
	              * caller's place (u.lambda, like T_LAMBDA) */
} ms_type;

typedef struct ms_cont_data {
	jmp_buf buf;
	bool used;        /* true once invoked or once call/cc has returned
	                    * normally -- either way it can't be used again */
	int wind_depth;   /* dynamic-wind stack depth at the moment of capture,
	                    * so invoking this continuation later knows how many
	                    * enclosing dynamic-wind "after" thunks to run */
	ms_val *result;   /* value being delivered to the captured point */
} ms_cont_data;

struct ms_val {
	unsigned char type;
	unsigned char mark;
	union {
		double num;
		long ch;                   /* T_CHAR */
		char *str;                 /* T_STR, T_SYM (owns the memory) */
		struct { ms_val *car, *cdr; } pair;   /* T_PAIR */
		struct { ms_val **items; long len; } vec; /* T_VECTOR (owns items) */
		ms_builtin fn;               /* T_FUN */
		struct { ms_val *params, *body, *env; } lambda; /* T_LAMBDA, T_MACRO */
		struct { bool forced; ms_val *value; ms_val *thunk; } promise; /* T_PROMISE */
		ms_cont_data *cont;          /* T_CONT (owns the ms_cont_data) */
	} u;
};

typedef enum { MS_INFO, MS_ERROR, MS_PANIC } ms_log_level;

/* ----------------------------------------------------------------------
 * GLOBALS
 * ------------------------------------------------------------------- */

static ms_val *ms_heap = NULL;
static ms_val *ms_freelist = NULL;
static long ms_heap_live = 0;

static ms_val *ms_symbols = NULL; /* intern list, linked through u.pair, car=sym cdr=rest */

static ms_val **ms_protect_stack = NULL;
static int ms_protect_top = 0;

static ms_val *g_nil;
static ms_val *g_true;
static ms_val *g_false;
static ms_val *g_unassigned; /* letrec placeholder, before a binding's init runs */
static ms_val *g_eof;        /* end-of-file object */
static ms_val *sym_quote, *sym_if, *sym_cond, *sym_begin, *sym_lambda,
	*sym_define, *sym_defun, *sym_let, *sym_letstar, *sym_letrec, *sym_set,
	*sym_and, *sym_or, *sym_while, *sym_quasiquote, *sym_unquote,
	*sym_unquote_splicing, *sym_do, *sym_case, *sym_delay, *sym_else, *sym_arrow,
	*sym_define_macro;

ms_val *ms_global_env;

int ms_out_of_memory_count = 0;

/* dynamic-wind bookkeeping for escape-only continuations: a stack of
 * (before . after) thunk pairs for every dynamic-wind currently in
 * effect. When a continuation captured at an outer point is invoked,
 * we pop and run "after" thunks down to the depth recorded at capture
 * time. Because these continuations are escape-only (they can only
 * jump to an *enclosing* call/cc, never back into a completed one),
 * there is never a need to re-run "before" thunks -- only "after". */
#ifndef MS_MAX_WINDS
#define MS_MAX_WINDS 256
#endif
static struct { ms_val *before, *after; } ms_wind_stack[MS_MAX_WINDS];
static int ms_wind_top = 0;

/* ----------------------------------------------------------------------
 * PROTOTYPES
 * ------------------------------------------------------------------- */

/* NOT static upstream -- exposed (alongside ms_log_level's own values,
 * MS_INFO/MS_ERROR/MS_PANIC, already a plain non-static typedef) so
 * an embedder-defined builtin (Zeitlos's sw/apps/repl/zapi.c) can
 * raise a real Scheme error the same way every bi_* builtin above
 * does, rather than inventing a second, inconsistent error-reporting
 * convention just for embedder-added procedures. */
void ms_log(ms_log_level level, const char *fmt, ...);
extern void (*ms_log_hook)(int level, const char *msg);	/* level: an ms_log_level */
extern bool (*ms_step_hook)(void);
static void ms_gc(void);
static ms_val *alloc_cell(void);
static ms_val *mk_num(double n);
static ms_val *mk_str(char *owned);
static ms_val *mk_pair(ms_val *car, ms_val *cdr);
static ms_val *mk_sym(const char *name);
static ms_val *mk_fun(ms_builtin fn);
static ms_val *mk_lambda(ms_val *params, ms_val *body, ms_val *env);
static ms_val *mk_char(long code);
static ms_val *mk_vector(long len, ms_val *fill);
static ms_val *mk_promise(ms_val *thunk);
static ms_val *call_procedure(ms_val *f, ms_val *args);
static ms_val *invoke_continuation(ms_val *k, ms_val *args);
static ms_val *pcar(ms_val *v);
static ms_val *pcdr(ms_val *v);
static bool nilp(ms_val *v);
static bool truthy(ms_val *v);
ms_val *ms_eval(ms_val *x, ms_val *env);
static ms_val *env_lookup(ms_val *env, ms_val *sym);
static void env_define(ms_val *env, ms_val *sym, ms_val *val);
static bool env_set(ms_val *env, ms_val *sym, ms_val *val);
static ms_val *ms_new_env(ms_val *parent);
static bool ms_equal(ms_val *a, ms_val *b);
ms_val *ms_read(const char **s);
void ms_print(ms_val *v, bool readable);
char *ms_to_string(ms_val *v, bool readable);	/* NOT static upstream --
	 * exposed so an embedder can get the printed representation of a
	 * value as a malloc'd string, without ms_print()'s hardcoded
	 * printf()-to-stdout (irrelevant/wrong on a target where "stdout"
	 * isn't necessarily where the result should go -- e.g. Zeitlos's
	 * `lisp` app, sw/apps/lisp/lisp.c, needs it as a string to send
	 * down a port or a LISP_EVAL reply message, not printed to its
	 * own UART debug console). Caller owns the returned string, must
	 * free() it. */
void ms_load_string(const char *src, ms_val *env);
#ifndef LIX
static void ms_load_file(const char *filename, ms_val *env);
#endif
static ms_val *qq_expand(ms_val *t, ms_val *env);
ms_val *ms_env_std(void);
void ms_init(void);
void ms_deinit(void);	/* frees the heap/protect stack malloc'd by
						 * ms_init(); not part of upstream ms.c -- added
						 * so an embedding caller can reclaim this RAM
						 * when the interpreter isn't in use */

/* NOT part of upstream ms.c -- Zeitlos embedder-registration API, see
 * the full explanation right above these functions' definitions
 * (near ms_heap_used()/ms_cell_size() below). */
void ms_def_builtin(const char *name, ms_builtin fn);
bool ms_is_callable(const char *name);
ms_val *ms_mk_str(char *owned);
ms_val *ms_mk_num(double n);
ms_val *ms_mk_bool(bool b);
ms_val *ms_nil_val(void);
ms_val *ms_mk_str_list(char **items, int count);
bool ms_is_str(ms_val *v);
bool ms_is_num(ms_val *v);
bool ms_is_pair(ms_val *v);
bool ms_is_nil(ms_val *v);
const char *ms_str_val(ms_val *v);
double ms_num_val(ms_val *v);
ms_val *ms_car(ms_val *v);
ms_val *ms_cdr(ms_val *v);

/* opt-in panic recovery -- not part of upstream ms.c; see the full
 * explanation and correct usage pattern near ms_log() below. setjmp()
 * on ms_panic_recovery must be called directly by the embedding
 * caller, not through a wrapper function. */
extern jmp_buf ms_panic_recovery;
extern bool ms_panic_recovery_armed;
extern bool ms_exit_requested;
extern int ms_exit_code;
void ms_panic_before_try(void);
void ms_panic_after_recover(void);
void ms_panic_disarm(void);

#define PUSH(v) \
	do { \
		if (ms_protect_top >= MS_PROTECT_STACK_SIZE) \
			ms_log(MS_PANIC, "protect stack overflow (recursion too deep for " \
				"MS_PROTECT_STACK_SIZE=%d; note this limits *non-tail* " \
				"recursion depth -- proper tail calls don't consume this " \
				"stack -- increase MS_PROTECT_STACK_SIZE or rewrite the " \
				"recursion to be tail-recursive)", (int)MS_PROTECT_STACK_SIZE); \
		ms_protect_stack[ms_protect_top++] = (v); \
	} while (0)
#define POP() (ms_protect_top--)
#define POPN(n) (ms_protect_top -= (n))

/* ----------------------------------------------------------------------
 * LOGGING
 * ------------------------------------------------------------------- */

/* Opt-in panic recovery -- NOT part of upstream ms.c. A panic
 * (type errors, heap exhaustion, protect-stack overflow, calling a
 * non-procedure, etc.) is the COMMON case for an interactive REPL,
 * not a rare failure -- a single typo like (car 5) would otherwise
 * exit() the whole process.
 *
 * IMPORTANT: setjmp(ms_panic_recovery) must be called DIRECTLY by the
 * embedding caller, in the same stack frame that will go on to call
 * ms_eval() -- NOT wrapped in a helper function that returns before
 * the protected work runs. longjmp() can only target a setjmp() whose
 * enclosing stack frame is still live; a helper that calls setjmp()
 * and then returns has already destroyed that frame by the time a
 * later panic tries to jump back into it (this exact mistake was
 * caught by testing against a real (car 5) panic: ms_log's longjmp
 * call executed, but landed in an exited frame, silently continuing
 * into undefined behavior -- a non-pair being read as a pair's car --
 * rather than either recovering correctly or crashing loudly).
 *
 * Correct usage, directly in the caller's own stack frame:
 *
 *     ms_panic_before_try();
 *     if (setjmp(ms_panic_recovery) == 0) {
 *         // normal path: ms_read()/ms_eval()/ms_print() here
 *     } else {
 *         ms_panic_after_recover();
 *         // a panic occurred; report it and continue
 *     }
 *
 * Both helpers reset ms_protect_top/ms_wind_top back to their depth
 * at ms_panic_before_try() time, the same way call/cc already resets
 * its own bookkeeping on escape, so a panic can never leak
 * protect-stack slots or dangling dynamic-wind frames across repeated
 * recoveries. When ms_panic_recovery_armed is false (e.g. the desktop
 * REPL, or upstream's own test suite, which never call
 * ms_panic_before_try() at all), ms_log's behavior on a panic is
 * byte-for-byte identical to the original: exit(EXIT_FAILURE). */
jmp_buf ms_panic_recovery;
bool ms_panic_recovery_armed = false;
static int ms_panic_saved_protect_top = 0;
static int ms_panic_saved_wind_top = 0;
bool ms_exit_requested = false;	/* set by (exit) when longjmp'ing back
									 * via code 2 instead of really
									 * exiting -- see bi_exit() */
int ms_exit_code = 0;

/* Embedder hooks (not part of upstream ms.c):
 *
 * ms_log_hook, when set, receives every message ms_log() would print
 * (formatted, without the "[error] " prefix) instead of stderr -- an
 * app with no console shows them where its user looks.
 *
 * ms_step_hook, when set, is called every MS_STEP_EVERY evaluations; if
 * it returns true the evaluation stops with a panic ("stopped"), which
 * the embedder's ms_panic_recovery catches. That is how a program
 * stuck in a loop is stopped from the keyboard: the hook looks at the
 * app's input without blocking. */
#ifndef MS_STEP_EVERY
#define MS_STEP_EVERY 2048
#endif
void (*ms_log_hook)(int level, const char *msg) = NULL;
bool (*ms_step_hook)(void) = NULL;
static unsigned ms_step_count;

void ms_log(ms_log_level level, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	if (ms_log_hook) {
		char m[200];
		vsnprintf(m, sizeof(m), fmt, args);
		va_end(args);
		ms_log_hook((int)level, m);
		if (level == MS_PANIC) {
			if (ms_panic_recovery_armed) longjmp(ms_panic_recovery, 1);
			exit(EXIT_FAILURE);
		}
		return;
	}
	if (level == MS_INFO) fprintf(stderr, "[info] ");
	else if (level == MS_ERROR) fprintf(stderr, "[error] ");
	else if (level == MS_PANIC) fprintf(stderr, "[panic] ");
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
	va_end(args);
	if (level == MS_PANIC) {
		if (ms_panic_recovery_armed) {
			longjmp(ms_panic_recovery, 1);
		} else {
			exit(EXIT_FAILURE);
		}
	}
}

void ms_panic_before_try(void) {
	ms_panic_saved_protect_top = ms_protect_top;
	ms_panic_saved_wind_top = ms_wind_top;
	ms_panic_recovery_armed = true;
}

void ms_panic_after_recover(void) {
	ms_protect_top = ms_panic_saved_protect_top;
	ms_wind_top = ms_panic_saved_wind_top;
}

void ms_panic_disarm(void) {
	ms_panic_recovery_armed = false;
}

/* ----------------------------------------------------------------------
 * GARBAGE COLLECTION (mark & sweep over the static cell pool)
 *
 * Roots:
 *   - the global environment
 *   - the explicit "protect" shadow stack (values not yet reachable from
 *     an environment or another live structure, e.g. mid-construction)
 *   - the interned symbol table
 *
 * Environments and closures are ordinary cons cells (see ms_new_env), so
 * they are automatically kept alive by the same marking pass that keeps
 * any other reachable data alive -- there is no separate, easy-to-forget
 * "mark the environment of a lambda" step as there was in the original
 * implementation.
 * ------------------------------------------------------------------- */

static void gc_mark(ms_val *v) {
	if (v == NULL || v->mark) return;
	v->mark = 1;
	switch (v->type) {
		case T_PAIR:
			gc_mark(v->u.pair.car);
			gc_mark(v->u.pair.cdr);
			break;
		case T_LAMBDA:
		case T_MACRO:
			gc_mark(v->u.lambda.params);
			gc_mark(v->u.lambda.body);
			gc_mark(v->u.lambda.env);
			break;
		case T_VECTOR:
			for (long i = 0; i < v->u.vec.len; i++) gc_mark(v->u.vec.items[i]);
			break;
		case T_PROMISE:
			gc_mark(v->u.promise.thunk);
			gc_mark(v->u.promise.value);
			break;
		default:
			break;
	}
}

static void ms_gc(void) {

	gc_mark(ms_global_env);
	gc_mark(ms_symbols);
	for (int i = 0; i < ms_protect_top; i++) gc_mark(ms_protect_stack[i]);
	for (int i = 0; i < ms_wind_top; i++) {
		gc_mark(ms_wind_stack[i].before);
		gc_mark(ms_wind_stack[i].after);
	}

	ms_freelist = NULL;
	ms_heap_live = 0;

	for (long i = 0; i < MS_HEAP_SIZE; i++) {
		ms_val *c = &ms_heap[i];
		if (c->type == T_FREE) {
			c->u.pair.cdr = ms_freelist;
			ms_freelist = c;
			continue;
		}
		if (c->mark) {
			c->mark = 0;
			ms_heap_live++;
		} else {
			if (c->type == T_STR || c->type == T_SYM) free(c->u.str);
			if (c->type == T_VECTOR) free(c->u.vec.items);
			if (c->type == T_CONT) free(c->u.cont);
			c->type = T_FREE;
			c->u.pair.cdr = ms_freelist;
			ms_freelist = c;
		}
	}
}

long ms_gc_live_count(void) { return ms_heap_live; }
long ms_gc_heap_size(void) { return MS_HEAP_SIZE; }

static ms_val *alloc_cell(void) {
	if (ms_freelist == NULL) {
		ms_gc();
		if (ms_freelist == NULL) {
			ms_out_of_memory_count++;
			ms_log(MS_PANIC,
				"out of memory: heap exhausted (%d live cells / %d total, "
				"increase MS_HEAP_SIZE)",
				(int)ms_heap_live, (int)MS_HEAP_SIZE);
		}
	}
	ms_val *c = ms_freelist;
	ms_freelist = c->u.pair.cdr;
	c->mark = 0;
	ms_heap_live++;
	return c;
}

/* ----------------------------------------------------------------------
 * CONSTRUCTORS
 * ------------------------------------------------------------------- */

static ms_val *mk_num(double n) {
	ms_val *v = alloc_cell();
	v->type = T_NUM;
	v->u.num = n;
	return v;
}

/* takes ownership of `owned` (must be malloc'd, or NULL) */
static ms_val *mk_str(char *owned) {
	ms_val *v = alloc_cell();
	v->type = T_STR;
	v->u.str = owned;
	return v;
}

static ms_val *mk_pair(ms_val *car, ms_val *cdr) {
	PUSH(car); PUSH(cdr);
	ms_val *v = alloc_cell();
	POPN(2);
	v->type = T_PAIR;
	v->u.pair.car = car;
	v->u.pair.cdr = cdr;
	return v;
}

static ms_val *mk_fun(ms_builtin fn) {
	ms_val *v = alloc_cell();
	v->type = T_FUN;
	v->u.fn = fn;
	return v;
}

static ms_val *mk_lambda(ms_val *params, ms_val *body, ms_val *env) {
	PUSH(params); PUSH(body); PUSH(env);
	ms_val *v = alloc_cell();
	POPN(3);
	v->type = T_LAMBDA;
	v->u.lambda.params = params;
	v->u.lambda.body = body;
	v->u.lambda.env = env;
	return v;
}

static ms_val *mk_char(long code) {
	ms_val *v = alloc_cell();
	v->type = T_CHAR;
	v->u.ch = code;
	return v;
}

static ms_val *mk_vector(long len, ms_val *fill) {
	PUSH(fill);
	ms_val *v = alloc_cell();
	POP();
	v->type = T_VECTOR;
	v->u.vec.len = len;
	v->u.vec.items = len > 0 ? malloc(sizeof(ms_val *) * (size_t)len) : NULL;
	for (long i = 0; i < len; i++) v->u.vec.items[i] = fill;
	return v;
}

static ms_val *mk_promise(ms_val *thunk) {
	PUSH(thunk);
	ms_val *v = alloc_cell();
	POP();
	v->type = T_PROMISE;
	v->u.promise.forced = false;
	v->u.promise.value = NULL;
	v->u.promise.thunk = thunk;
	return v;
}

/* Lookup tables: finding a symbol by name, and a global binding by
 * symbol, would otherwise be walks along lists of every symbol and every
 * global -- hundreds long once a library is loaded, and the top of an
 * embedder's profile when a program makes thousands of global lookups.
 * Both tables only point at cells that are never freed: every symbol is
 * kept by ms_symbols, a GC root, and every global binding by the global
 * frame. A table that fills up is dropped and the lists are walked
 * instead; ms_init() clears both. */
#ifndef MS_HASH_SIZE
#define MS_HASH_SIZE 2048		/* a power of two; 16 KB of pointers here */
#endif
static ms_val *sym_hash[MS_HASH_SIZE];
static int sym_hash_n;
static bool sym_hash_off;
static ms_val *ghash_key[MS_HASH_SIZE], *ghash_bind[MS_HASH_SIZE];
static int ghash_n;
static ms_val *ghash_env;		/* the env whose top frame is in the table, NULL none */
static bool ghash_full;			/* it did not fit: walk the list, and stop trying */

static unsigned name_hash(const char *s) {
	unsigned h = 2166136261u;
	while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
	return h & (MS_HASH_SIZE - 1);
}

static unsigned ptr_hash(const ms_val *p) {
	unsigned long v = (unsigned long)p;
	v ^= v >> 9;
	return (unsigned)(v * 2654435761u) & (MS_HASH_SIZE - 1);
}

/* interned symbols: identical names share one cell, so `eq?` on symbols
 * is a pointer comparison, and special forms can be recognized by
 * comparing against the fixed sym_* pointers. */
static ms_val *mk_sym(const char *name) {
	unsigned i = name_hash(name);
	if (!sym_hash_off) {
		while (sym_hash[i]) {
			if (strcmp(sym_hash[i]->u.str, name) == 0) return sym_hash[i];
			i = (i + 1) & (MS_HASH_SIZE - 1);
		}
	} else {
		for (ms_val *p = ms_symbols; p != NULL && p->type == T_PAIR; p = p->u.pair.cdr) {
			ms_val *s = p->u.pair.car;
			if (strcmp(s->u.str, name) == 0) return s;
		}
	}
	ms_val *v = alloc_cell();
	v->type = T_SYM;
	v->u.str = (char *)malloc(strlen(name) + 1);
	strcpy(v->u.str, name);
	PUSH(v);
	ms_symbols = mk_pair(v, ms_symbols);
	POP();
	if (!sym_hash_off) {
		if (sym_hash_n * 4 >= MS_HASH_SIZE * 3) sym_hash_off = true;	/* full: the list from now on */
		else { sym_hash[i] = v; sym_hash_n++; }
	}
	return v;
}

/* the binding of sym in the hashed frame; *found false if it has none */
static ms_val *ghash_find(ms_val *sym) {
	unsigned i = ptr_hash(sym);
	while (ghash_key[i]) {
		if (ghash_key[i] == sym) return ghash_bind[i];
		i = (i + 1) & (MS_HASH_SIZE - 1);
	}
	return NULL;
}

static void ghash_put(ms_val *sym, ms_val *binding) {
	unsigned i = ptr_hash(sym);
	while (ghash_key[i] && ghash_key[i] != sym) i = (i + 1) & (MS_HASH_SIZE - 1);
	if (!ghash_key[i]) {
		if (ghash_n * 4 >= MS_HASH_SIZE * 3) { ghash_env = NULL; ghash_full = true; return; }	/* full: walk instead */
		ghash_n++;
	}
	ghash_key[i] = sym;
	ghash_bind[i] = binding;
}

/* the global frame into the table, the newest binding of each name */
static bool ghash_ready(ms_val *env) {
	if (ghash_env == env) return true;
	if (ghash_full) return false;
	memset(ghash_key, 0, sizeof ghash_key);
	ghash_n = 0;
	ghash_env = env;
	for (ms_val *f = env->u.pair.car; f && f->type == T_PAIR && ghash_env; f = f->u.pair.cdr) {
		ms_val *b = f->u.pair.car;
		if (!ghash_find(b->u.pair.car)) ghash_put(b->u.pair.car, b);
	}
	return ghash_env == env;
}

/* ----------------------------------------------------------------------
 * ACCESSORS / PREDICATES
 * ------------------------------------------------------------------- */

static ms_val *pcar(ms_val *v) { return (v && v->type == T_PAIR) ? v->u.pair.car : g_nil; }
static ms_val *pcdr(ms_val *v) { return (v && v->type == T_PAIR) ? v->u.pair.cdr : g_nil; }
#define CAAR(v) pcar(pcar(v))
#define CADR(v) pcar(pcdr(v))
#define CDAR(v) pcdr(pcar(v))
#define CDDR(v) pcdr(pcdr(v))
#define CADDR(v) pcar(pcdr(pcdr(v)))

static bool nilp(ms_val *v) { return v == NULL || v->type == T_NIL; }
/* R4RS: #f is the only false value. In particular '() (the empty list)
 * is TRUE, unlike many toy Lisps (including earlier revisions of this
 * one) that conflate "empty" with "false". */
static bool truthy(ms_val *v) { return !(v && v->type == T_BOOL && v == g_false); }

static long list_length(ms_val *v) {
	long n = 0;
	while (v && v->type == T_PAIR) { n++; v = v->u.pair.cdr; }
	return n;
}

/* ----------------------------------------------------------------------
 * EQUALITY
 *
 * eq?    -- identity; numbers/strings compare by value as a practical
 *           convenience (as in most small Schemes), everything else by
 *           pointer.
 * equal? -- deep structural equality.
 * ------------------------------------------------------------------- */

static bool ms_eq(ms_val *a, ms_val *b) {
	if (a == b) return true;
	if (a == NULL || b == NULL) return false;
	if (a->type != b->type) return false;
	if (a->type == T_NUM) return a->u.num == b->u.num;
	if (a->type == T_CHAR) return a->u.ch == b->u.ch;
	if (a->type == T_NIL) return true;
	return false;
}

static bool ms_equal(ms_val *a, ms_val *b) {
	if (a == b) return true;
	if (a == NULL || b == NULL) return false;
	if (a->type != b->type) return false;
	switch (a->type) {
		case T_NIL: return true;
		case T_BOOL: return a == b; /* only the two singletons exist */
		case T_NUM: return a->u.num == b->u.num;
		case T_CHAR: return a->u.ch == b->u.ch;
		case T_STR: return strcmp(a->u.str, b->u.str) == 0;
		case T_SYM: return a == b; /* interned */
		case T_PAIR:
			return ms_equal(a->u.pair.car, b->u.pair.car) &&
				ms_equal(a->u.pair.cdr, b->u.pair.cdr);
		case T_VECTOR:
			if (a->u.vec.len != b->u.vec.len) return false;
			for (long i = 0; i < a->u.vec.len; i++)
				if (!ms_equal(a->u.vec.items[i], b->u.vec.items[i])) return false;
			return true;
		default: return a == b;
	}
}

/* ----------------------------------------------------------------------
 * ENVIRONMENTS
 *
 * An environment is a plain cons cell: (frame . parent)
 *   frame  = a mutable list of (symbol . value) pairs
 *   parent = the enclosing environment, or NIL for the global environment
 *
 * Because environments are built entirely from ordinary pairs, they are
 * automatically kept alive (or collected) by the same mark/sweep pass
 * used for all other data -- closures simply hold a pointer to the
 * environment they were created in (see mk_lambda / gc_mark T_LAMBDA),
 * which is what makes closures over local variables actually work.
 * ------------------------------------------------------------------- */

static ms_val *ms_new_env(ms_val *parent) {
	return mk_pair(g_nil, parent);
}

static ms_val *env_lookup(ms_val *env, ms_val *sym) {
	while (env && env->type == T_PAIR) {
		if (env == ms_global_env && ms_global_env && ghash_ready(env)) {
			ms_val *b = ghash_find(sym);
			return b ? b->u.pair.cdr : NULL;
		}
		ms_val *frame = env->u.pair.car;
		while (frame && frame->type == T_PAIR) {
			ms_val *binding = frame->u.pair.car;
			if (binding->u.pair.car == sym) return binding->u.pair.cdr;
			frame = frame->u.pair.cdr;
		}
		env = env->u.pair.cdr;
	}
	return NULL;
}

static void env_define(ms_val *env, ms_val *sym, ms_val *val) {
	PUSH(env); PUSH(sym); PUSH(val);
	ms_val *binding = mk_pair(sym, val);
	PUSH(binding);
	env->u.pair.car = mk_pair(binding, env->u.pair.car);
	if (env == ghash_env) ghash_put(sym, binding);
	POPN(4);
}

/* mutate an existing binding if found anywhere in scope; otherwise
 * (as a practical convenience) define it in the global environment. */
static bool env_set(ms_val *env, ms_val *sym, ms_val *val) {
	ms_val *e = env;
	while (e && e->type == T_PAIR) {
		if (e == ms_global_env && ghash_ready(e)) {
			ms_val *b = ghash_find(sym);
			if (b) { b->u.pair.cdr = val; return true; }
			break;
		}
		ms_val *frame = e->u.pair.car;
		while (frame && frame->type == T_PAIR) {
			ms_val *binding = frame->u.pair.car;
			if (binding->u.pair.car == sym) {
				binding->u.pair.cdr = val;
				return true;
			}
			frame = frame->u.pair.cdr;
		}
		e = e->u.pair.cdr;
	}
	env_define(ms_global_env, sym, val);
	return false;
}

/* ----------------------------------------------------------------------
 * PRINTING
 * ------------------------------------------------------------------- */

/* NOT part of upstream ms.c. Formats a whole-number double as decimal
 * text WITHOUT ever casting it to (or passing it as) a `long long`.
 *
 * Why: on this project's actual RV32I target (Zeitlos,
 * -mabi=ilp32 -- no hardware float, so under a pure soft-float ABI a
 * `long long` and a `double` are passed to a variadic function
 * exactly the same way, both as an 8-byte value split across the
 * integer-register/stack argument path), `printf("%lld", (long long)
 * 3.0)` was observed to print 12884901888 -- exactly 3 << 32, the
 * textbook signature of a 64-bit variadic argument's two 32-bit
 * halves ending up swapped or offset by one register slot, not a
 * logic bug in ms's own arithmetic (the value 3 is present and
 * correctly positioned in ONE of the two halves, just the wrong one).
 * Reproduced via lisp.c's `scheme` command (sw/apps/lisp/lisp.c,
 * Zeitlos), which calls ms_to_string() -> tostr_inner() -> exactly
 * this code path.
 *
 * The fix sidesteps the whole question of exactly which layer is at
 * fault (this toolchain's variadic long-long marshaling specifically,
 * vs. something in the double-to-int64 conversion itself feeding it)
 * by never materializing or passing a 64-bit integer at all: digits
 * are extracted one at a time with fmod()/floor(), staying entirely
 * in `double` and single-digit `int` (32-bit, one register, no
 * marshaling ambiguity possible) the whole way. fmod()/floor() on an
 * operand that's already an exact integer within a double's 53-bit
 * exact-integer range (guaranteed here by the `fabs(d) < 1e15` guard
 * at both call sites, matching upstream's own existing bound) are
 * exact per IEEE 754, so this produces the same output as a correct
 * %lld would have, just without ever forming the value that
 * particular formatting path couldn't reliably deliver on this
 * target. */
static void ms_num_to_str(double d, char *out, size_t out_cap) {

	size_t pos = 0;
	bool neg = (d < 0);
	if (neg) d = -d;

	char digits[24];
	int ndigits = 0;

	if (d < 1.0) {
		digits[ndigits++] = '0';
	} else {
		while (d >= 0.5 && ndigits < (int)sizeof(digits)) {
			int digit = (int)(fmod(d, 10.0) + 0.5);
			if (digit > 9) digit = 9; // clamp -- shouldn't be
				// reachable for a genuine exact-integer double, kept
				// as a safety net rather than trusted to never fire
			digits[ndigits++] = (char)('0' + digit);
			d = floor(d / 10.0);
		}
	}

	if (neg && pos + 1 < out_cap) out[pos++] = '-';
	while (ndigits > 0 && pos + 1 < out_cap) out[pos++] = digits[--ndigits];
	if (pos < out_cap) out[pos] = 0;

}

static void print_double(double d) {
	if (d == (long long)d && fabs(d) < 1e15) {
		char buf[24];
		ms_num_to_str(d, buf, sizeof(buf));
		printf("%s", buf);
	} else
		printf("%g", d);
}

/* R4RS named characters. char_name_for returns the canonical name for
 * printing (or NULL if the char should just print literally); the
 * reader (below) recognizes all names on the left when parsing #\name. */
static const struct { const char *name; long code; } ms_char_names[] = {
	{"space", ' '}, {"newline", '\n'}, {"tab", '\t'}, {"return", '\r'},
	{"nul", 0}, {"null", 0}, {"backspace", 8}, {"delete", 127},
	{"rubout", 127}, {"escape", 27}, {"altmode", 27}, {"linefeed", '\n'},
};
#define MS_NUM_CHAR_NAMES (int)(sizeof(ms_char_names) / sizeof(ms_char_names[0]))

static const char *char_name_for(long code) {
	for (int i = 0; i < MS_NUM_CHAR_NAMES; i++)
		if (ms_char_names[i].code == code) return ms_char_names[i].name;
	return NULL;
}

static bool str_ieq(const char *a, const char *b) {
	while (*a && *b) {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
		a++; b++;
	}
	return *a == *b;
}

static bool char_code_for_name(const char *name, long *out) {
	for (int i = 0; i < MS_NUM_CHAR_NAMES; i++)
		if (str_ieq(name, ms_char_names[i].name)) { *out = ms_char_names[i].code; return true; }
	return false;
}

static void ms_print_inner(ms_val *v, bool readable) {
	if (v == NULL) { printf("()"); return; }
	switch (v->type) {
		case T_NIL: printf("()"); break;
		case T_BOOL: printf(v == g_true ? "#t" : "#f"); break;
		case T_SYM: printf("%s", v->u.str); break;
		case T_NUM: print_double(v->u.num); break;
		case T_CHAR:
			if (!readable) { putchar((int)v->u.ch); break; }
			{
				const char *nm = char_name_for(v->u.ch);
				if (nm) printf("#\\%s", nm);
				else if (v->u.ch >= 33 && v->u.ch < 127) printf("#\\%c", (int)v->u.ch);
				else printf("#\\x%lx", v->u.ch);
			}
			break;
		case T_STR:
			if (readable) {
				printf("\"");
				for (char *p = v->u.str; *p; p++) {
					if (*p == '"' || *p == '\\') printf("\\%c", *p);
					else if (*p == '\n') printf("\\n");
					else putchar(*p);
				}
				printf("\"");
			} else {
				printf("%s", v->u.str);
			}
			break;
		case T_FUN: printf("#<builtin>"); break;
		case T_LAMBDA: printf("#<procedure>"); break;
		case T_MACRO: printf("#<macro>"); break;
		case T_PROMISE: printf("#<promise>"); break;
		case T_CONT: printf("#<continuation>"); break;
		case T_VECTOR: {
			printf("#(");
			for (long i = 0; i < v->u.vec.len; i++) {
				if (i) printf(" ");
				ms_print_inner(v->u.vec.items[i], readable);
			}
			printf(")");
			break;
		}
		case T_PAIR:
			printf("(");
			ms_val *p = v;
			bool first = true;
			while (p && p->type == T_PAIR) {
				if (!first) printf(" ");
				ms_print_inner(p->u.pair.car, readable);
				first = false;
				p = p->u.pair.cdr;
			}
			if (!nilp(p)) { printf(" . "); ms_print_inner(p, readable); }
			printf(")");
			break;
	}
}

void ms_print(ms_val *v, bool readable) { ms_print_inner(v, readable); }

/* render into a malloc'd string (used by (write-to-string x) style helpers) */
static void tostr_inner(ms_val *v, bool readable, char **buf, size_t *len, size_t *cap);

static void tostr_append(char **buf, size_t *len, size_t *cap, const char *s) {
	size_t sl = strlen(s);
	if (*len + sl + 1 > *cap) {
		while (*len + sl + 1 > *cap) *cap *= 2;
		*buf = realloc(*buf, *cap);
	}
	memcpy(*buf + *len, s, sl + 1);
	*len += sl;
}

static void tostr_inner(ms_val *v, bool readable, char **buf, size_t *len, size_t *cap) {
	char num[64];
	if (v == NULL || v->type == T_NIL) { tostr_append(buf, len, cap, "()"); return; }
	switch (v->type) {
		case T_BOOL: tostr_append(buf, len, cap, v == g_true ? "#t" : "#f"); break;
		case T_SYM: tostr_append(buf, len, cap, v->u.str); break;
		case T_CHAR: {
			if (!readable) { char c1[2] = { (char)v->u.ch, 0 }; tostr_append(buf, len, cap, c1); break; }
			const char *nm = char_name_for(v->u.ch);
			if (nm) { snprintf(num, sizeof(num), "#\\%s", nm); tostr_append(buf, len, cap, num); }
			else if (v->u.ch >= 33 && v->u.ch < 127) { snprintf(num, sizeof(num), "#\\%c", (int)v->u.ch); tostr_append(buf, len, cap, num); }
			else { snprintf(num, sizeof(num), "#\\x%lx", v->u.ch); tostr_append(buf, len, cap, num); }
			break;
		}
		case T_NUM:
			if (v->u.num == (long long)v->u.num && fabs(v->u.num) < 1e15)
				ms_num_to_str(v->u.num, num, sizeof(num));	// NOT
					// snprintf(..., "%lld", (long long)v->u.num) --
					// see ms_num_to_str()'s own comment, this is the
					// exact call site that produced 12884901888 for
					// (+ 1 2) via lisp.c's `scheme` command
			else
				snprintf(num, sizeof(num), "%g", v->u.num);
			tostr_append(buf, len, cap, num);
			break;
		case T_STR:
			if (readable) tostr_append(buf, len, cap, "\"");
			tostr_append(buf, len, cap, v->u.str);
			if (readable) tostr_append(buf, len, cap, "\"");
			break;
		case T_FUN: tostr_append(buf, len, cap, "#<builtin>"); break;
		case T_LAMBDA: tostr_append(buf, len, cap, "#<procedure>"); break;
		case T_MACRO: tostr_append(buf, len, cap, "#<macro>"); break;
		case T_PROMISE: tostr_append(buf, len, cap, "#<promise>"); break;
		case T_CONT: tostr_append(buf, len, cap, "#<continuation>"); break;
		case T_VECTOR: {
			tostr_append(buf, len, cap, "#(");
			for (long i = 0; i < v->u.vec.len; i++) {
				if (i) tostr_append(buf, len, cap, " ");
				tostr_inner(v->u.vec.items[i], readable, buf, len, cap);
			}
			tostr_append(buf, len, cap, ")");
			break;
		}
		case T_PAIR: {
			tostr_append(buf, len, cap, "(");
			ms_val *p = v;
			bool first = true;
			while (p && p->type == T_PAIR) {
				if (!first) tostr_append(buf, len, cap, " ");
				tostr_inner(p->u.pair.car, readable, buf, len, cap);
				first = false;
				p = p->u.pair.cdr;
			}
			if (!nilp(p)) {
				tostr_append(buf, len, cap, " . ");
				tostr_inner(p, readable, buf, len, cap);
			}
			tostr_append(buf, len, cap, ")");
			break;
		}
	}
}

char *ms_to_string(ms_val *v, bool readable) {	/* see the public API
	 * declaration above for why this isn't static upstream, in this
	 * build */
	size_t cap = 64, len = 0;
	char *buf = malloc(cap);
	buf[0] = 0;
	tostr_inner(v, readable, &buf, &len, &cap);
	return buf;
}

/* ----------------------------------------------------------------------
 * READER
 * ------------------------------------------------------------------- */

static void skip_ws(const char **s) {
	for (;;) {
		while (**s && isspace((unsigned char)**s)) (*s)++;
		if (**s == ';') {
			while (**s && **s != '\n') (*s)++;
			continue;
		}
		break;
	}
}

static bool token_is_number(const char *tok) {
	if (!*tok) return false;
	const char *p = tok;
	if (*p == '+' || *p == '-') p++;
	if (!*p) return false;
	bool digits = false, dot = false;
	for (; *p; p++) {
		if (isdigit((unsigned char)*p)) { digits = true; continue; }
		if (*p == '.' && !dot) { dot = true; continue; }
		return false;
	}
	return digits;
}

ms_val *ms_read(const char **s) {

	skip_ws(s);
	if (**s == 0) return NULL; /* end of input */

	if (**s == '(') {
		(*s)++;
		ms_val *head = g_nil, *tail = NULL;
		PUSH(head);
		for (;;) {
			skip_ws(s);
			if (**s == 0) { ms_log(MS_ERROR, "unexpected end of input in list"); POP(); return NULL; }
			if (**s == ')') { (*s)++; break; }
			/* dotted pair: "( a b . c )" -- a lone '.' followed by
			 * whitespace/paren (not e.g. ".5" or a symbol like "->") */
			if (**s == '.' && (isspace((unsigned char)(*s)[1]) || (*s)[1] == '(' || (*s)[1] == 0)) {
				(*s)++;
				ms_val *tailval = ms_read(s);
				if (tailval == NULL) { POP(); return NULL; }
				skip_ws(s);
				if (**s == ')') (*s)++;
				else ms_log(MS_ERROR, "malformed dotted list (expected ')')");
				if (tail == NULL) { POP(); return tailval; }
				tail->u.pair.cdr = tailval;
				POP();
				return head;
			}
			ms_val *item = ms_read(s);
			if (item == NULL) { POP(); return NULL; }
			PUSH(item);
			ms_val *cell = mk_pair(item, g_nil);
			POP(); /* item */
			if (tail == NULL) { head = cell; ms_protect_stack[ms_protect_top - 1] = head; }
			else tail->u.pair.cdr = cell;
			tail = cell;
		}
		POP();
		return head;
	}

	if (**s == ')') { ms_log(MS_ERROR, "unexpected ')'"); (*s)++; return ms_read(s); }

	if (**s == '\'') {
		(*s)++;
		ms_val *inner = ms_read(s);
		if (inner == NULL) return NULL;
		PUSH(inner);
		ms_val *r = mk_pair(sym_quote, mk_pair(inner, g_nil));
		POP();
		return r;
	}

	if (**s == '`') {
		(*s)++;
		ms_val *inner = ms_read(s);
		if (inner == NULL) return NULL;
		PUSH(inner);
		ms_val *r = mk_pair(sym_quasiquote, mk_pair(inner, g_nil));
		POP();
		return r;
	}

	if (**s == ',') {
		(*s)++;
		ms_val *tag = sym_unquote;
		if (**s == '@') { (*s)++; tag = sym_unquote_splicing; }
		ms_val *inner = ms_read(s);
		if (inner == NULL) return NULL;
		PUSH(inner);
		ms_val *r = mk_pair(tag, mk_pair(inner, g_nil));
		POP();
		return r;
	}

	if (**s == '#') {
		(*s)++;
		if (**s == 't') { (*s)++; while (isalpha((unsigned char)**s)) (*s)++; return g_true; }
		if (**s == 'f') { (*s)++; while (isalpha((unsigned char)**s)) (*s)++; return g_false; }
		if (**s == '(') {
			/* vector literal: read the following "(...)" as an ordinary
			 * list, then convert it to a vector. */
			ms_val *lst = ms_read(s);
			long n = list_length(lst);
			PUSH(lst);
			ms_val *vec = mk_vector(n, g_false);
			POP();
			long i = 0;
			for (ms_val *p = lst; !nilp(p); p = pcdr(p)) vec->u.vec.items[i++] = pcar(p);
			return vec;
		}
		if (**s == '\\') {
			(*s)++;
			char buf[MS_MAX_TOKEN];
			int n = 0;
			if (**s) { buf[n++] = **s; (*s)++; } /* first char always literal */
			while (**s && isalnum((unsigned char)**s) && n < MS_MAX_TOKEN - 1) { buf[n++] = **s; (*s)++; }
			buf[n] = 0;
			if (n == 1) return mk_char((unsigned char)buf[0]);
			long code;
			if (char_code_for_name(buf, &code)) return mk_char(code);
			if ((buf[0] == 'x' || buf[0] == 'X') && n > 1) return mk_char(strtol(buf + 1, NULL, 16));
			ms_log(MS_ERROR, "unknown character name #\\%s", buf);
			return mk_char((unsigned char)buf[0]);
		}
		if (**s == 'x' || **s == 'X' || **s == 'o' || **s == 'O' ||
			**s == 'b' || **s == 'B' || **s == 'd' || **s == 'D') {
			char radixc = **s;
			(*s)++;
			char buf[MS_MAX_TOKEN];
			int n = 0;
			while (**s && !isspace((unsigned char)**s) && **s != '(' && **s != ')' &&
				**s != '"' && **s != ';' && n < MS_MAX_TOKEN - 1) { buf[n++] = **s; (*s)++; }
			buf[n] = 0;
			int base = (radixc == 'x' || radixc == 'X') ? 16
				: (radixc == 'o' || radixc == 'O') ? 8
				: (radixc == 'b' || radixc == 'B') ? 2 : 10;
			return mk_num((double)strtol(buf, NULL, base));
		}
		if (**s == 'e' || **s == 'i') {
			/* exactness prefix: a no-op here (see README -- numbers are
			 * always doubles, there's no separate exact/inexact tower). */
			(*s)++;
			return ms_read(s);
		}
		ms_log(MS_ERROR, "unsupported # syntax");
		return ms_read(s);
	}

	if (**s == '"') {
		(*s)++;
		char buf[MS_MAX_TOKEN];
		int n = 0;
		while (**s && **s != '"') {
			char c = **s;
			if (c == '\\' && (*s)[1]) {
				(*s)++;
				switch (**s) {
					case 'n': c = '\n'; break;
					case 't': c = '\t'; break;
					case '"': c = '"'; break;
					case '\\': c = '\\'; break;
					default: c = **s; break;
				}
			}
			if (n < MS_MAX_TOKEN - 1) buf[n++] = c;
			(*s)++;
		}
		if (**s == '"') (*s)++;
		buf[n] = 0;
		char *owned = malloc(n + 1);
		memcpy(owned, buf, n + 1);
		return mk_str(owned);
	}

	/* bareword: symbol or number */
	{
		char buf[MS_MAX_TOKEN];
		int n = 0;
		while (**s && !isspace((unsigned char)**s) && **s != '(' && **s != ')' &&
			**s != '\'' && **s != '"' && **s != ';')
		{
			if (n < MS_MAX_TOKEN - 1) buf[n++] = **s;
			(*s)++;
		}
		buf[n] = 0;
		if (n == 0) { ms_log(MS_ERROR, "parse error"); (*s)++; return ms_read(s); }
		if (token_is_number(buf)) return mk_num(strtod(buf, NULL));
		return mk_sym(buf);
	}
}

/* ----------------------------------------------------------------------
 * EVAL
 * ------------------------------------------------------------------- */

static ms_val *eval_body(ms_val *body, ms_val *env) {
	/* evaluate all but the last expr for effect; caller tail-calls the last */
	if (nilp(body)) return g_nil;
	while (!nilp(pcdr(body))) {
		ms_eval(pcar(body), env);
		body = pcdr(body);
	}
	return pcar(body);
}

static ms_val *eval_list(ms_val *args, ms_val *env) {
	ms_val *head = g_nil, *tail = NULL;
	PUSH(head); PUSH(env);
	while (!nilp(args)) {
		ms_val *v = ms_eval(pcar(args), env);
		PUSH(v);
		ms_val *cell = mk_pair(v, g_nil);
		POP();
		if (tail == NULL) { head = cell; ms_protect_stack[ms_protect_top - 2] = head; }
		else tail->u.pair.cdr = cell;
		tail = cell;
		args = pcdr(args);
	}
	POPN(2);
	return head;
}

static ms_val *bind_params(ms_val *params, ms_val *args, ms_val *parent_env) {
	PUSH(parent_env); PUSH(args);
	ms_val *env = ms_new_env(parent_env);
	POPN(2);
	PUSH(env); PUSH(args);
	while (!nilp(params) && !nilp(args)) {
		if (params->type == T_SYM) {
			/* dotted rest-param: (lambda (a b . rest) ...) */
			env_define(env, params, args);
			args = g_nil;
			params = g_nil;
			break;
		}
		env_define(env, pcar(params), pcar(args));
		params = pcdr(params);
		args = pcdr(args);
	}
	if (params->type == T_SYM) env_define(env, params, g_nil);
	POPN(2);
	return env;
}

ms_val *ms_eval(ms_val *x, ms_val *env) {

	PUSH(x); PUSH(env);

tail_call:
	ms_protect_stack[ms_protect_top - 2] = x;
	ms_protect_stack[ms_protect_top - 1] = env;
	if (ms_step_hook && ++ms_step_count >= MS_STEP_EVERY) {
		ms_step_count = 0;
		if (ms_step_hook()) ms_log(MS_PANIC, "stopped");
	}

	if (x == NULL || x->type == T_NIL || x->type == T_NUM || x->type == T_STR ||
		x->type == T_BOOL || x->type == T_CHAR || x->type == T_VECTOR ||
		x->type == T_FUN || x->type == T_LAMBDA || x->type == T_PROMISE ||
		x->type == T_CONT || x->type == T_MACRO) {
		POPN(2);
		return x;
	}

	if (x->type == T_SYM) {
		ms_val *v = env_lookup(env, x);
		POPN(2);
		if (v == NULL) {
			ms_log(MS_ERROR, "unbound symbol: %s", x->u.str);
			return g_false;
		}
		return v;
	}

	if (x->type != T_PAIR) { POPN(2); return g_false; }

	ms_val *op = x->u.pair.car;
	ms_val *args = x->u.pair.cdr;

	if (op->type == T_SYM) {

		if (op == sym_quote) { POPN(2); return pcar(args); }

		if (op == sym_if) {
			ms_val *test = ms_eval(pcar(args), env);
			if (truthy(test)) { x = CADR(args); goto tail_call; }
			ms_val *rest = CDDR(args);
			if (nilp(rest)) { POPN(2); return g_false; }
			x = pcar(rest);
			goto tail_call;
		}

		if (op == sym_cond) {
			ms_val *clauses = args;
			while (!nilp(clauses)) {
				ms_val *clause = pcar(clauses);
				ms_val *test = pcar(clause);
				bool is_else = (test == sym_else);
				ms_val *tv = is_else ? g_true : ms_eval(test, env);
				if (truthy(tv)) {
					ms_val *body = pcdr(clause);
					if (nilp(body)) { POPN(2); return tv; }
					/* (test => proc) form: call proc with the test value */
					if (pcar(body) == sym_arrow) {
						ms_val *proc = ms_eval(CADR(body), env);
						PUSH(proc); PUSH(tv);
						ms_val *r = call_procedure(proc, mk_pair(tv, g_nil));
						POPN(2);
						POPN(2);
						return r;
					}
					x = eval_body(body, env);
					goto tail_call;
				}
				clauses = pcdr(clauses);
			}
			POPN(2);
			return g_false;
		}

		if (op == sym_begin) {
			if (nilp(args)) { POPN(2); return g_false; }
			x = eval_body(args, env);
			goto tail_call;
		}

		if (op == sym_and) {
			if (nilp(args)) { POPN(2); return g_true; }
			while (!nilp(pcdr(args))) {
				ms_val *v = ms_eval(pcar(args), env);
				if (!truthy(v)) { POPN(2); return v; }
				args = pcdr(args);
			}
			x = pcar(args);
			goto tail_call;
		}

		if (op == sym_or) {
			if (nilp(args)) { POPN(2); return g_false; }
			while (!nilp(pcdr(args))) {
				ms_val *v = ms_eval(pcar(args), env);
				if (truthy(v)) { POPN(2); return v; }
				args = pcdr(args);
			}
			x = pcar(args);
			goto tail_call;
		}

		if (op == sym_while) {
			ms_val *test = pcar(args);
			ms_val *body = pcdr(args);
			while (truthy(ms_eval(test, env)))
				for (ms_val *b = body; !nilp(b); b = pcdr(b)) ms_eval(pcar(b), env);
			POPN(2);
			return g_false;
		}

		if (op == sym_lambda) {
			ms_val *r = mk_lambda(pcar(args), pcdr(args), env);
			POPN(2);
			return r;
		}

		if (op == sym_let) {
			ms_val *first = pcar(args);
			if (first->type == T_SYM) {
				/* named let: (let loop ((x init) ...) body...) desugars to
				 * a self-referential procedure, called once with the
				 * initial values. Recursive calls to `loop` inside body
				 * go through the normal application path, so this loop
				 * is properly tail-recursive just like a top-level
				 * tail-recursive function. */
				ms_val *name = first;
				ms_val *bindings = CADR(args);
				ms_val *body = CDDR(args);
				PUSH(name); PUSH(bindings); PUSH(body);
				ms_val *newenv = ms_new_env(env);
				PUSH(newenv);
				ms_val *params = g_nil, *ptail = NULL;
				ms_val *b = bindings;
				while (!nilp(b)) {
					ms_val *bind = pcar(b);
					ms_val *val = ms_eval(CADR(bind), env);
					env_define(newenv, pcar(bind), val);
					ms_val *pc = mk_pair(pcar(bind), g_nil);
					if (ptail == NULL) { params = pc; ms_protect_stack[ms_protect_top - 1] = newenv; }
					else ptail->u.pair.cdr = pc;
					ptail = pc;
					b = pcdr(b);
				}
				ms_val *fn = mk_lambda(params, body, newenv);
				env_define(newenv, name, fn);
				POPN(4);
				env = newenv;
				ms_protect_stack[ms_protect_top - 1] = env;	/* this frame's env slot: newenv is kept while its body runs */
				x = eval_body(body, env);
				goto tail_call;
			}
			ms_val *bindings = first;
			ms_val *body = pcdr(args);
			PUSH(bindings); PUSH(body);
			ms_val *newenv = ms_new_env(env);
			PUSH(newenv);
			ms_val *b = bindings;
			while (!nilp(b)) {
				ms_val *bind = pcar(b);
				ms_val *val = ms_eval(CADR(bind), env); /* let: evaluated in outer env */
				env_define(newenv, pcar(bind), val);
				b = pcdr(b);
			}
			POPN(3);
			env = newenv;
			ms_protect_stack[ms_protect_top - 1] = env;	/* this frame's env slot: newenv is kept while its body runs */
			x = eval_body(body, env);
			goto tail_call;
		}

		if (op == sym_letstar) {
			ms_val *bindings = pcar(args);
			ms_val *body = pcdr(args);
			PUSH(bindings); PUSH(body);
			ms_val *newenv = ms_new_env(env);
			PUSH(newenv);
			ms_val *b = bindings;
			while (!nilp(b)) {
				ms_val *bind = pcar(b);
				ms_val *val = ms_eval(CADR(bind), newenv); /* let*: sequential */
				env_define(newenv, pcar(bind), val);
				b = pcdr(b);
			}
			POPN(3);
			env = newenv;
			ms_protect_stack[ms_protect_top - 1] = env;	/* this frame's env slot: newenv is kept while its body runs */
			x = eval_body(body, env);
			goto tail_call;
		}

		if (op == sym_letrec) {
			/* bind every variable to an "unassigned" placeholder first, so
			 * that mutually-recursive lambdas defined here can already see
			 * each other's names, then evaluate each init expression (in
			 * the new environment, so they too can refer to one another)
			 * and fill in the real value. */
			ms_val *bindings = pcar(args);
			ms_val *body = pcdr(args);
			PUSH(bindings); PUSH(body);
			ms_val *newenv = ms_new_env(env);
			PUSH(newenv);
			for (ms_val *b = bindings; !nilp(b); b = pcdr(b))
				env_define(newenv, pcar(pcar(b)), g_unassigned);
			for (ms_val *b = bindings; !nilp(b); b = pcdr(b)) {
				ms_val *bind = pcar(b);
				ms_val *val = ms_eval(CADR(bind), newenv);
				env_set(newenv, pcar(bind), val);
			}
			POPN(3);
			env = newenv;
			ms_protect_stack[ms_protect_top - 1] = env;	/* this frame's env slot: newenv is kept while its body runs */
			x = eval_body(body, env);
			goto tail_call;
		}

		if (op == sym_do) {
			/* (do ((var init step) ...) (test result...) command...) */
			ms_val *bindings = pcar(args);
			ms_val *testclause = CADR(args);
			ms_val *commands = CDDR(args);
			PUSH(bindings); PUSH(testclause); PUSH(commands);
			ms_val *loopenv = ms_new_env(env);
			PUSH(loopenv);
			for (ms_val *b = bindings; !nilp(b); b = pcdr(b)) {
				ms_val *bind = pcar(b);
				env_define(loopenv, pcar(bind), ms_eval(CADR(bind), env));
			}
			for (;;) {
				ms_val *tv = ms_eval(pcar(testclause), loopenv);
				if (truthy(tv)) {
					ms_val *results = pcdr(testclause);
					POPN(4);
					if (nilp(results)) return g_false;
					env = loopenv;
					x = eval_body(results, env);
					goto tail_call;
				}
				for (ms_val *c = commands; !nilp(c); c = pcdr(c)) ms_eval(pcar(c), loopenv);
				/* compute all step values using the CURRENT bindings
				 * before rebinding any of them (simultaneous update) */
				ms_val *newvals = g_nil, *nvtail = NULL;
				PUSH(newvals);
				for (ms_val *b = bindings; !nilp(b); b = pcdr(b)) {
					ms_val *bind = pcar(b);
					ms_val *step = nilp(CDDR(bind)) ? pcar(bind) : CADDR(bind);
					ms_val *v = ms_eval(step, loopenv);
					PUSH(v);
					ms_val *cell = mk_pair(v, g_nil);
					POP();
					if (nvtail == NULL) { newvals = cell; ms_protect_stack[ms_protect_top - 1] = newvals; }
					else nvtail->u.pair.cdr = cell;
					nvtail = cell;
				}
				ms_val *nv = newvals;
				for (ms_val *b = bindings; !nilp(b); b = pcdr(b), nv = pcdr(nv))
					env_set(loopenv, pcar(pcar(b)), pcar(nv));
				POP(); /* newvals */
			}
		}

		if (op == sym_case) {
			ms_val *keyval = ms_eval(pcar(args), env);
			PUSH(keyval);
			ms_val *clauses = pcdr(args);
			while (!nilp(clauses)) {
				ms_val *clause = pcar(clauses);
				ms_val *datums = pcar(clause);
				bool match = (datums == sym_else);
				if (!match) for (ms_val *p = datums; !nilp(p); p = pcdr(p))
					if (ms_eq(keyval, pcar(p))) { match = true; break; }
				if (match) {
					ms_val *body = pcdr(clause);
					POP();
					if (nilp(body)) { POPN(2); return g_false; }
					x = eval_body(body, env);
					goto tail_call;
				}
				clauses = pcdr(clauses);
			}
			POPN(3);
			return g_false;
		}

		if (op == sym_delay) {
			ms_val *thunk = mk_lambda(g_nil, args, env);
			ms_val *p = mk_promise(thunk);
			POPN(2);
			return p;
		}

		if (op == sym_set) {
			ms_val *sym = pcar(args);
			ms_val *val = ms_eval(CADR(args), env);
			env_set(env, sym, val);
			POPN(2);
			return val;
		}

		if (op == sym_define) {
			ms_val *target = pcar(args);
			if (target->type == T_PAIR) {
				/* (define (name . params) body...) */
				ms_val *name = pcar(target);
				ms_val *params = pcdr(target);
				ms_val *body = pcdr(args);
				PUSH(name);
				ms_val *fn = mk_lambda(params, body, env);
				PUSH(fn);
				env_define(env, name, fn);
				POPN(4);
				return name;
			} else {
				ms_val *val = nilp(pcdr(args)) ? g_nil : ms_eval(CADR(args), env);
				PUSH(val);
				env_define(env, target, val);
				POPN(3);
				return target;
			}
		}

		if (op == sym_defun) {
			ms_val *name = pcar(args);
			ms_val *params = CADR(args);
			ms_val *body = CDDR(args);
			PUSH(name);
			ms_val *fn = mk_lambda(params, body, env);
			PUSH(fn);
			env_define(env, name, fn);
			POPN(4);
			return name;
		}

		if (op == sym_define_macro) {
			/* (define-macro (name . params) body...): a closure, marked
			 * as a macro -- see the T_MACRO case of application below */
			ms_val *target = pcar(args);
			if (target->type != T_PAIR || pcar(target)->type != T_SYM) {
				ms_log(MS_ERROR, "define-macro: (define-macro (name args...) body...)");
				POPN(2);
				return g_false;
			}
			{
				ms_val *name = pcar(target);
				ms_val *m;
				PUSH(name);
				m = mk_lambda(pcdr(target), pcdr(args), env);
				m->type = T_MACRO;
				PUSH(m);
				env_define(env, name, m);
				POPN(4);
				return name;
			}
		}

		if (op == sym_quasiquote) {
			/* minimal quasiquote: supports ,x and ,@x at any depth-1 position */
			ms_val *tmpl = pcar(args);
			ms_val *r = qq_expand(tmpl, env);
			POPN(2);
			return r;
		}
	}

	/* function application. `f` and `argv` are kept protected together
	 * through the entire call -- important because `f` may be an
	 * ephemeral, not-yet-bound-anywhere value, e.g. the result of
	 * evaluating an inline `(lambda ...)` in operator position, as in
	 * `((lambda (x) x) 5)`. */
	ms_val *f = ms_eval(op, env);
	PUSH(f);
	if (f->type == T_MACRO) {
		/* a macro: its parameters bound to the forms as written, its
		 * body run to make a new form, and that evaluated here, in
		 * the caller's environment (a tail position, like any call).
		 * Not hygienic: a macro's own names can meet the caller's --
		 * give helpers unusual names. */
		ms_val *menv = bind_params(f->u.lambda.params, args, f->u.lambda.env);
		ms_val *form;
		PUSH(menv);
		form = ms_eval(eval_body(f->u.lambda.body, menv), menv);
		POPN(2);
		x = form;
		goto tail_call;
	}
	ms_val *argv = eval_list(args, env);
	PUSH(argv);

	if (f->type != T_FUN && f->type != T_LAMBDA && f->type != T_CONT) {
		if (truthy(f)) ms_log(MS_ERROR, "attempt to call a non-function");
		POPN(4);
		return g_false;
	}

	if (f->type == T_FUN) {
		ms_val *r = f->u.fn(argv);
		POPN(4);
		return r;
	}

	if (f->type == T_CONT) {
		ms_val *r = invoke_continuation(f, argv);
		POPN(4);
		return r; /* only reached if the continuation turned out to be
		           * invalid/expired; invoke_continuation longjmps away
		           * on the normal path and never returns here */
	}

	{
		ms_val *newenv = bind_params(f->u.lambda.params, argv, f->u.lambda.env);
		ms_val *body = f->u.lambda.body;
		POPN(2); /* drop f, argv only -- keep this frame's x/env slots for goto */
		env = newenv;
		ms_protect_stack[ms_protect_top - 1] = env;	/* this frame's env slot: newenv is kept while its body runs */
		x = eval_body(body, env);
		goto tail_call;
	}
}

/* Calls any callable value (builtin, closure, or continuation) with an
 * already-evaluated argument list. Used by builtins that need to call
 * back into Lisp code (apply, force, dynamic-wind, call/cc, map, etc.)
 * without going through the special-form machinery in ms_eval. */
static ms_val *call_procedure(ms_val *f, ms_val *args) {
	PUSH(f); PUSH(args);
	ms_val *r;
	if (f->type == T_FUN) {
		r = f->u.fn(args);
	} else if (f->type == T_CONT) {
		r = invoke_continuation(f, args);
	} else if (f->type == T_LAMBDA) {
		ms_val *newenv = bind_params(f->u.lambda.params, args, f->u.lambda.env);
		PUSH(newenv);
		ms_val *last = eval_body(f->u.lambda.body, newenv);
		r = ms_eval(last, newenv);
		POP();
	} else {
		ms_log(MS_PANIC, "attempt to call a non-procedure");
		r = g_false;
	}
	POPN(2);
	return r;
}

/* Invokes a captured continuation: runs any dynamic-wind "after" thunks
 * for winds entered since the continuation was captured, then longjmps
 * back to the call/cc call site. This is an ESCAPE-ONLY continuation --
 * it can only transfer control to an *enclosing*, still-active call/cc;
 * it cannot be invoked again after that call/cc has already returned
 * (there is no re-entry / multi-shot support -- see README). */
static ms_val *invoke_continuation(ms_val *k, ms_val *args) {
	ms_cont_data *cd = k->u.cont;
	if (cd->used) {
		ms_log(MS_ERROR, "continuation invoked outside its dynamic extent "
			"(this is an escape-only continuation -- it can't be re-entered "
			"after its call/cc has already returned)");
		return g_false;
	}
	ms_val *value = nilp(args) ? g_false : pcar(args);
	PUSH(value); /* protect across the after-thunk calls below */
	while (ms_wind_top > cd->wind_depth) {
		ms_wind_top--;
		call_procedure(ms_wind_stack[ms_wind_top].after, g_nil);
	}
	cd->result = value;
	cd->used = true;
	longjmp(cd->buf, 1);
	return g_false; /* unreachable */
}

/* quasiquote expansion (kept separate for readability) */
static ms_val *qq_expand(ms_val *t, ms_val *env) {
	if (t == NULL || t->type != T_PAIR) return t;
	ms_val *h = pcar(t);
	if (h == sym_unquote) return ms_eval(CADR(t), env);
	if (h->type == T_PAIR && pcar(h) == sym_unquote_splicing) {
		ms_val *spliced = ms_eval(CADR(h), env);
		PUSH(spliced);
		ms_val *rest = qq_expand(pcdr(t), env);
		POP();
		if (nilp(spliced)) return rest;
		/* copy spliced list and append rest */
		ms_val *head = g_nil, *tail = NULL;
		PUSH(rest); PUSH(head);
		for (ms_val *p = spliced; !nilp(p); p = pcdr(p)) {
			ms_val *cell = mk_pair(pcar(p), g_nil);
			if (tail == NULL) { head = cell; ms_protect_stack[ms_protect_top - 1] = head; }
			else tail->u.pair.cdr = cell;
			tail = cell;
		}
		if (tail) tail->u.pair.cdr = rest; else head = rest;
		POPN(2);
		return head;
	}
	PUSH(env);
	ms_val *car2 = qq_expand(h, env);
	PUSH(car2);
	ms_val *cdr2 = qq_expand(pcdr(t), env);
	POP(); POP();
	return mk_pair(car2, cdr2);
}

/* ----------------------------------------------------------------------
 * LOAD (source is entirely in-memory; no fopen on LIX)
 * ------------------------------------------------------------------- */

void ms_load_string(const char *src, ms_val *env) {
	const char *p = src;
	for (;;) {
		ms_val *form = ms_read(&p);
		if (form == NULL) break;
		PUSH(form);
		ms_eval(form, env);
		POP();
	}
}

#ifndef LIX
static void ms_load_file(const char *filename, ms_val *env) {
	FILE *fp = fopen(filename, "r");
	if (!fp) { ms_log(MS_ERROR, "file not found: %s", filename); return; }
	fseek(fp, 0L, SEEK_END);
	long size = ftell(fp);
	rewind(fp);
	char *buf = malloc(size + 1);
	size_t rd = fread(buf, 1, size, fp);
	buf[rd] = 0;
	fclose(fp);
	ms_load_string(buf, env);
	free(buf);
}
#endif

/* ----------------------------------------------------------------------
 * BUILTINS
 *
 * All builtins receive an already-evaluated argument list (eval_list
 * handles evaluation for ordinary calls; special forms above are handled
 * directly in ms_eval and never reach here).
 * ------------------------------------------------------------------- */

#define ARG1 (pcar(args))
#define ARG2 (CADR(args))
#define ARG3 (CADDR(args))

static void need_num(ms_val *v, const char *who) {
	if (v->type != T_NUM) ms_log(MS_PANIC, "%s: expected a number", who);
}

static ms_val *bi_car(ms_val *args) {
	ms_val *x = ARG1;
	if (x->type != T_PAIR) ms_log(MS_PANIC, "car: expected a pair");
	return x->u.pair.car;
}
static ms_val *bi_cdr(ms_val *args) {
	ms_val *x = ARG1;
	if (x->type != T_PAIR) ms_log(MS_PANIC, "cdr: expected a pair");
	return x->u.pair.cdr;
}
static ms_val *bi_cons(ms_val *args) { return mk_pair(ARG1, ARG2); }
static ms_val *bi_set_car(ms_val *args) {
	ms_val *x = ARG1;
	if (x->type != T_PAIR) ms_log(MS_PANIC, "set-car!: expected a pair");
	x->u.pair.car = ARG2;
	return g_false;
}
static ms_val *bi_set_cdr(ms_val *args) {
	ms_val *x = ARG1;
	if (x->type != T_PAIR) ms_log(MS_PANIC, "set-cdr!: expected a pair");
	x->u.pair.cdr = ARG2;
	return g_false;
}
static ms_val *bi_pair_p(ms_val *args) { return ARG1->type == T_PAIR ? g_true : g_false; }
static ms_val *bi_null_p(ms_val *args) { return nilp(ARG1) ? g_true : g_false; }
static ms_val *bi_atom_p(ms_val *args) {
	ms_val *x = ARG1;
	return (x->type != T_PAIR) ? g_true : g_false;
}
static ms_val *bi_num_p(ms_val *args) { return ARG1->type == T_NUM ? g_true : g_false; }
static ms_val *bi_str_p(ms_val *args) { return ARG1->type == T_STR ? g_true : g_false; }
static ms_val *bi_sym_p(ms_val *args) { return ARG1->type == T_SYM ? g_true : g_false; }
static ms_val *bi_bool_p(ms_val *args) { return ARG1->type == T_BOOL ? g_true : g_false; }
static ms_val *bi_char_p(ms_val *args) { return ARG1->type == T_CHAR ? g_true : g_false; }
static ms_val *bi_vector_p(ms_val *args) { return ARG1->type == T_VECTOR ? g_true : g_false; }
/* (macroexpand-1 form): what a macro call turns into, once -- for
 * looking at a macro; anything else comes back as it is */
static ms_val *bi_macroexpand_1(ms_val *args) {
	ms_val *form = ARG1, *m, *menv, *r;
	if (form->type != T_PAIR || pcar(form)->type != T_SYM) return form;
	m = env_lookup(ms_global_env, pcar(form));
	if (!m || m->type != T_MACRO) return form;
	PUSH(form); PUSH(m);
	menv = bind_params(m->u.lambda.params, pcdr(form), m->u.lambda.env);
	PUSH(menv);
	r = ms_eval(eval_body(m->u.lambda.body, menv), menv);
	POPN(3);
	return r;
}

static ms_val *bi_proc_p(ms_val *args) {
	ms_val *x = ARG1;
	return (x->type == T_FUN || x->type == T_LAMBDA || x->type == T_CONT) ? g_true : g_false;
}

static ms_val *bi_eq(ms_val *args) { return ms_eq(ARG1, ARG2) ? g_true : g_false; }
static ms_val *bi_equal(ms_val *args) { return ms_equal(ARG1, ARG2) ? g_true : g_false; }
static ms_val *bi_not(ms_val *args) { return truthy(ARG1) ? g_false : g_true; }

static ms_val *bi_lt(ms_val *args) { need_num(ARG1,"<"); need_num(ARG2,"<"); return ARG1->u.num < ARG2->u.num ? g_true : g_false; }
static ms_val *bi_lte(ms_val *args) { need_num(ARG1,"<="); need_num(ARG2,"<="); return ARG1->u.num <= ARG2->u.num ? g_true : g_false; }
static ms_val *bi_gt(ms_val *args) { need_num(ARG1,">"); need_num(ARG2,">"); return ARG1->u.num > ARG2->u.num ? g_true : g_false; }
static ms_val *bi_gte(ms_val *args) { need_num(ARG1,">="); need_num(ARG2,">="); return ARG1->u.num >= ARG2->u.num ? g_true : g_false; }
static ms_val *bi_numeq(ms_val *args) {
	ms_val *p = args;
	if (nilp(p)) return g_true;
	need_num(pcar(p), "=");
	double first = pcar(p)->u.num;
	p = pcdr(p);
	while (!nilp(p)) {
		need_num(pcar(p), "=");
		if (pcar(p)->u.num != first) return g_false;
		p = pcdr(p);
	}
	return g_true;
}

static ms_val *bi_add(ms_val *args) {
	double r = 0;
	for (ms_val *p = args; !nilp(p); p = pcdr(p)) { need_num(pcar(p), "+"); r += pcar(p)->u.num; }
	return mk_num(r);
}
static ms_val *bi_sub(ms_val *args) {
	if (nilp(args)) return mk_num(0);
	need_num(pcar(args), "-");
	if (nilp(pcdr(args))) return mk_num(-pcar(args)->u.num);
	double r = pcar(args)->u.num;
	for (ms_val *p = pcdr(args); !nilp(p); p = pcdr(p)) { need_num(pcar(p), "-"); r -= pcar(p)->u.num; }
	return mk_num(r);
}
static ms_val *bi_mul(ms_val *args) {
	double r = 1;
	for (ms_val *p = args; !nilp(p); p = pcdr(p)) { need_num(pcar(p), "*"); r *= pcar(p)->u.num; }
	return mk_num(r);
}
static ms_val *bi_div(ms_val *args) {
	if (nilp(args)) return mk_num(1);
	need_num(pcar(args), "/");
	if (nilp(pcdr(args))) return mk_num(1.0 / pcar(args)->u.num);
	double r = pcar(args)->u.num;
	for (ms_val *p = pcdr(args); !nilp(p); p = pcdr(p)) {
		need_num(pcar(p), "/");
		if (pcar(p)->u.num == 0) ms_log(MS_PANIC, "/: division by zero");
		r /= pcar(p)->u.num;
	}
	return mk_num(r);
}
static ms_val *bi_mod(ms_val *args) {
	need_num(ARG1, "%"); need_num(ARG2, "%");
	if (ARG2->u.num == 0) ms_log(MS_PANIC, "%%: division by zero");
	return mk_num(fmod(ARG1->u.num, ARG2->u.num));
}
static ms_val *bi_quotient(ms_val *args) {
	need_num(ARG1, "quotient"); need_num(ARG2, "quotient");
	if (ARG2->u.num == 0) ms_log(MS_PANIC, "quotient: division by zero");
	return mk_num(trunc(ARG1->u.num / ARG2->u.num));
}
static ms_val *bi_remainder(ms_val *args) {
	need_num(ARG1, "remainder"); need_num(ARG2, "remainder");
	if (ARG2->u.num == 0) ms_log(MS_PANIC, "remainder: division by zero");
	return mk_num(fmod(ARG1->u.num, ARG2->u.num));
}
static ms_val *bi_modulo(ms_val *args) {
	need_num(ARG1, "modulo"); need_num(ARG2, "modulo");
	double a = ARG1->u.num, b = ARG2->u.num;
	if (b == 0) ms_log(MS_PANIC, "modulo: division by zero");
	double r = fmod(a, b);
	if (r != 0 && ((r < 0) != (b < 0))) r += b;
	return mk_num(r);
}
static double gcd2(double a, double b) { a = fabs(a); b = fabs(b); while (b != 0) { double t = fmod(a, b); a = b; b = t; } return a; }
static ms_val *bi_gcd(ms_val *args) {
	double r = 0;
	for (ms_val *p = args; !nilp(p); p = pcdr(p)) { need_num(pcar(p), "gcd"); r = gcd2(r, pcar(p)->u.num); }
	return mk_num(r);
}
static ms_val *bi_lcm(ms_val *args) {
	double r = 1;
	for (ms_val *p = args; !nilp(p); p = pcdr(p)) {
		need_num(pcar(p), "lcm");
		double v = fabs(pcar(p)->u.num);
		if (v == 0) return mk_num(0);
		double g = gcd2(r, v);
		r = (r / g) * v;
	}
	return mk_num(r);
}
static ms_val *bi_expt(ms_val *args) {
	need_num(ARG1, "expt"); need_num(ARG2, "expt");
	return mk_num(pow(ARG1->u.num, ARG2->u.num));
}
static ms_val *bi_abs(ms_val *args) { need_num(ARG1, "abs"); return mk_num(fabs(ARG1->u.num)); }
static ms_val *bi_floor(ms_val *args) { need_num(ARG1, "floor"); return mk_num(floor(ARG1->u.num)); }
static ms_val *bi_ceiling(ms_val *args) { need_num(ARG1, "ceiling"); return mk_num(ceil(ARG1->u.num)); }
static ms_val *bi_truncate(ms_val *args) { need_num(ARG1, "truncate"); return mk_num(trunc(ARG1->u.num)); }
static ms_val *bi_round(ms_val *args) {
	need_num(ARG1, "round");
	double v = ARG1->u.num, f = floor(v), diff = v - f;
	double r = (diff < 0.5) ? f : (diff > 0.5) ? f + 1 : (fmod(f, 2) == 0 ? f : f + 1); /* round-to-even */
	return mk_num(r);
}
static ms_val *bi_sqrt(ms_val *args) { need_num(ARG1, "sqrt"); return mk_num(sqrt(ARG1->u.num)); }
static ms_val *bi_min(ms_val *args) {
	need_num(ARG1, "min");
	double r = ARG1->u.num;
	for (ms_val *p = pcdr(args); !nilp(p); p = pcdr(p)) { need_num(pcar(p), "min"); if (pcar(p)->u.num < r) r = pcar(p)->u.num; }
	return mk_num(r);
}
static ms_val *bi_max(ms_val *args) {
	need_num(ARG1, "max");
	double r = ARG1->u.num;
	for (ms_val *p = pcdr(args); !nilp(p); p = pcdr(p)) { need_num(pcar(p), "max"); if (pcar(p)->u.num > r) r = pcar(p)->u.num; }
	return mk_num(r);
}
static ms_val *bi_zero_p(ms_val *args) { need_num(ARG1, "zero?"); return ARG1->u.num == 0 ? g_true : g_false; }
static ms_val *bi_positive_p(ms_val *args) { need_num(ARG1, "positive?"); return ARG1->u.num > 0 ? g_true : g_false; }
static ms_val *bi_negative_p(ms_val *args) { need_num(ARG1, "negative?"); return ARG1->u.num < 0 ? g_true : g_false; }
static ms_val *bi_odd_p(ms_val *args) { need_num(ARG1, "odd?"); return fmod(ARG1->u.num, 2) != 0 ? g_true : g_false; }
static ms_val *bi_even_p(ms_val *args) { need_num(ARG1, "even?"); return fmod(ARG1->u.num, 2) == 0 ? g_true : g_false; }
/* documented numeric-tower simplification: numbers are always doubles,
 * there's no separate exact/inexact representation (see README). */
static ms_val *bi_exact_p(ms_val *args) { (void)args; return g_false; }
static ms_val *bi_inexact_p(ms_val *args) { (void)args; return g_true; }
static ms_val *bi_exact_to_inexact(ms_val *args) { return ARG1; }
static ms_val *bi_inexact_to_exact(ms_val *args) { return ARG1; }

static ms_val *bi_list(ms_val *args) { return args; }

static ms_val *bi_length(ms_val *args) {
	ms_val *x = ARG1;
	if (x->type == T_STR) return mk_num((double)strlen(x->u.str));
	return mk_num((double)list_length(x));
}

static ms_val *bi_apply(ms_val *args) {
	ms_val *f = ARG1;
	/* (apply proc arg1 ... args) -- all args except the last are passed
	 * through directly, the last must be a list whose elements are
	 * appended to the call. */
	ms_val *rest = pcdr(args);
	if (nilp(rest)) return call_procedure(f, g_nil);
	PUSH(f);
	ms_val *head = g_nil, *tail = NULL;
	PUSH(head);
	while (!nilp(pcdr(rest))) {
		ms_val *cell = mk_pair(pcar(rest), g_nil);
		if (tail == NULL) { head = cell; ms_protect_stack[ms_protect_top - 1] = head; }
		else tail->u.pair.cdr = cell;
		tail = cell;
		rest = pcdr(rest);
	}
	ms_val *final_list = pcar(rest);
	if (tail == NULL) head = final_list; else tail->u.pair.cdr = final_list;
	POPN(2);
	return call_procedure(f, head);
}

static ms_val *bi_eval(ms_val *args) { return ms_eval(ARG1, ms_global_env); }

/* (dump) -- prints every symbol currently bound in the global
 * environment, wrapped at 80 columns, and returns #t (matching
 * bi_gc()'s own convention just above: a side-effecting diagnostic,
 * not a value meant for further computation). Environments are built
 * entirely from ordinary pairs (see ms_new_env()/env_lookup() above):
 * env is (frame . parent), frame is a list of (symbol . value)
 * bindings. The global env has no parent frames to walk through
 * (nothing above it), but the outer while loop is kept anyway to stay
 * correct if that ever changes.
 *
 * Deliberately done entirely in C rather than walking the list from
 * Scheme (e.g. via a Scheme-level for-each over a returned list, as
 * an earlier revision of this did): with the stdlib loaded, the
 * environment holds close to 200 bound symbols, and iterating that
 * many elements from Scheme through non-tail Scheme calls consumed
 * enough of the protect stack (see ms_protect_stack/
 * MS_PROTECT_STACK_SIZE above) to overflow it on a memory-constrained
 * embedded build. A plain C loop touches none of that -- symbol names
 * are read directly off T_SYM's own u.str, with no Scheme evaluation
 * involved in the walk or the printing at all. */
static ms_val *bi_dump(ms_val *args) {

	(void)args;

	long col = 0;

	ms_val *env = ms_global_env;
	while (env && env->type == T_PAIR) {
		ms_val *frame = env->u.pair.car;
		while (frame && frame->type == T_PAIR) {

			ms_val *binding = frame->u.pair.car;
			const char *name = binding->u.pair.car->u.str;
			long len = (long)strlen(name);
			long need = (col == 0) ? len : (col + 1 + len);

			if (need > 80) {
				if (col > 0) fputs("\r\n", stdout);
				fputs(name, stdout);
				col = len;
			} else {
				if (col > 0) fputs(" ", stdout);
				fputs(name, stdout);
				col = need;
			}

			frame = frame->u.pair.cdr;

		}
		env = env->u.pair.cdr;
	}

	fputs("\r\n", stdout);
	fflush(stdout);

	return g_true;

}

static ms_val *bi_read(ms_val *args) {
	ms_val *x = ARG1;
	if (x->type != T_STR) ms_log(MS_PANIC, "read: expected a string");
	const char *p = x->u.str;
	ms_val *r = ms_read(&p);
	return r == NULL ? g_eof : r;
}
static ms_val *bi_eof_object_p(ms_val *args) { return ARG1 == g_eof ? g_true : g_false; }

static ms_val *bi_print(ms_val *args) {
	if (nilp(args)) { printf("\n"); return g_false; }
	ms_val *x = ARG1;
	if (x->type == T_STR) printf("%s\n", x->u.str); else { ms_print(x, true); printf("\n"); }
	return g_false;
}
static ms_val *bi_display(ms_val *args) {
	ms_val *x = ARG1;
	if (x->type == T_STR) printf("%s", x->u.str); else ms_print(x, false);
	fflush(stdout);
	return g_false;
}
static ms_val *bi_write(ms_val *args) { ms_print(ARG1, true); fflush(stdout); return g_false; }

static ms_val *bi_gc(ms_val *args) {
	(void)args;
	long before = ms_heap_live;
	ms_gc();
	printf("gc: %ld -> %ld live cells (of %ld)\n", before, ms_heap_live, (long)MS_HEAP_SIZE);
	return g_true;
}

static ms_val *bi_exit(ms_val *args) {
	int code = nilp(args) ? 0 : (int)ARG1->u.num;
	if (ms_panic_recovery_armed) {
		ms_exit_requested = true;
		ms_exit_code = code;
		longjmp(ms_panic_recovery, 2);
	}
	exit(code);
}

static ms_val *bi_str_to_list(ms_val *args) {
	ms_val *x = ARG1;
	if (x->type != T_STR) ms_log(MS_PANIC, "str->list: expected a string");
	ms_val *head = g_nil, *tail = NULL;
	PUSH(head);
	for (char *p = x->u.str; *p; p++) {
		char buf[2] = { *p, 0 };
		ms_val *sym = mk_sym(buf);
		PUSH(sym);
		ms_val *cell = mk_pair(sym, g_nil);
		POP();
		if (tail == NULL) { head = cell; ms_protect_stack[ms_protect_top - 1] = head; }
		else tail->u.pair.cdr = cell;
		tail = cell;
	}
	POP();
	return head;
}

static ms_val *bi_list_to_str(ms_val *args) {
	ms_val *x = ARG1;
	if (x->type != T_PAIR && x->type != T_NIL) ms_log(MS_PANIC, "list->str: expected a list");
	size_t cap = 64, len = 0;
	char *buf = malloc(cap);
	for (ms_val *p = x; !nilp(p); p = pcdr(p)) {
		ms_val *item = pcar(p);
		const char *s;
		char onec[2];
		if (item->type == T_NUM) { onec[0] = (char)(int)item->u.num; onec[1] = 0; s = onec; }
		else if (item->type == T_SYM || item->type == T_STR) s = item->u.str;
		else { ms_log(MS_ERROR, "list->str: expected atoms/strings/nums"); continue; }
		size_t sl = strlen(s);
		if (len + sl + 1 > cap) { while (len + sl + 1 > cap) cap *= 2; buf = realloc(buf, cap); }
		memcpy(buf + len, s, sl);
		len += sl;
		buf[len] = 0;
	}
	return mk_str(buf);
}

static ms_val *bi_num_to_str(ms_val *args) {
	ms_val *x = ARG1;
	need_num(x, "num->str");
	return mk_str((char *)strdup(ms_to_string(x, false)));
}

static ms_val *bi_str_to_num(ms_val *args) {
	ms_val *x = ARG1;
	if (x->type != T_STR) ms_log(MS_PANIC, "str->num: expected a string");
	return mk_num(strtod(x->u.str, NULL));
}

static ms_val *bi_str_append(ms_val *args) {
	size_t cap = 64, len = 0;
	char *buf = malloc(cap);
	buf[0] = 0;
	for (ms_val *p = args; !nilp(p); p = pcdr(p)) {
		ms_val *x = pcar(p);
		if (x->type != T_STR) ms_log(MS_PANIC, "str-append: expected strings");
		size_t sl = strlen(x->u.str);
		if (len + sl + 1 > cap) { while (len + sl + 1 > cap) cap *= 2; buf = realloc(buf, cap); }
		memcpy(buf + len, x->u.str, sl);
		len += sl;
		buf[len] = 0;
	}
	return mk_str(buf);
}

static ms_val *bi_str_len(ms_val *args) {
	ms_val *x = ARG1;
	if (x->type != T_STR) ms_log(MS_PANIC, "str-length: expected a string");
	return mk_num((double)strlen(x->u.str));
}

#ifndef LIX
static ms_val *bi_file_to_str(ms_val *args) {
	ms_val *x = ARG1;
	FILE *fp = fopen(x->u.str, "r");
	if (!fp) ms_log(MS_PANIC, "file not found: %s", x->u.str);
	fseek(fp, 0L, SEEK_END);
	long size = ftell(fp);
	rewind(fp);
	char *buf = malloc(size + 1);
	size_t rd = fread(buf, 1, size, fp);
	buf[rd] = 0;
	fclose(fp);
	return mk_str(buf);
}

static ms_val *bi_load(ms_val *args) {
	ms_val *x = ARG1;
	if (x->type != T_STR) ms_log(MS_PANIC, "load: expected a string (filename)");
	ms_load_file(x->u.str, ms_global_env);
	return g_true;
}
#endif

/* R4RS string procedures beyond what's above */
static ms_val *bi_make_string(ms_val *args) {
	need_num(ARG1, "make-string");
	long n = (long)ARG1->u.num;
	char fill = nilp(pcdr(args)) ? ' ' : (char)ARG2->u.ch;
	char *buf = malloc(n + 1);
	memset(buf, fill, n);
	buf[n] = 0;
	return mk_str(buf);
}
static ms_val *bi_string(ms_val *args) {
	long n = list_length(args);
	char *buf = malloc(n + 1);
	long i = 0;
	for (ms_val *p = args; !nilp(p); p = pcdr(p)) buf[i++] = (char)pcar(p)->u.ch;
	buf[n] = 0;
	return mk_str(buf);
}
static ms_val *bi_string_ref(ms_val *args) {
	ms_val *s = ARG1;
	if (s->type != T_STR) ms_log(MS_PANIC, "string-ref: expected a string");
	need_num(ARG2, "string-ref");
	long i = (long)ARG2->u.num;
	long len = (long)strlen(s->u.str);
	if (i < 0 || i >= len) ms_log(MS_PANIC, "string-ref: index out of range");
	return mk_char((unsigned char)s->u.str[i]);
}
static ms_val *bi_string_set(ms_val *args) {
	ms_val *s = ARG1;
	if (s->type != T_STR) ms_log(MS_PANIC, "string-set!: expected a string");
	need_num(ARG2, "string-set!");
	long i = (long)ARG2->u.num;
	long len = (long)strlen(s->u.str);
	if (i < 0 || i >= len) ms_log(MS_PANIC, "string-set!: index out of range");
	s->u.str[i] = (char)ARG3->u.ch;
	return g_false;
}
static ms_val *bi_substring(ms_val *args) {
	ms_val *s = ARG1;
	if (s->type != T_STR) ms_log(MS_PANIC, "substring: expected a string");
	need_num(ARG2, "substring"); need_num(ARG3, "substring");
	long start = (long)ARG2->u.num, end = (long)ARG3->u.num;
	long len = (long)strlen(s->u.str);
	if (start < 0 || end > len || start > end) ms_log(MS_PANIC, "substring: index out of range");
	char *buf = malloc(end - start + 1);
	memcpy(buf, s->u.str + start, end - start);
	buf[end - start] = 0;
	return mk_str(buf);
}
static ms_val *bi_string_copy(ms_val *args) {
	if (ARG1->type != T_STR) ms_log(MS_PANIC, "string-copy: expected a string");
	return mk_str(strdup(ARG1->u.str));
}
static ms_val *bi_string_fill(ms_val *args) {
	ms_val *s = ARG1;
	if (s->type != T_STR) ms_log(MS_PANIC, "string-fill!: expected a string");
	memset(s->u.str, (char)ARG2->u.ch, strlen(s->u.str));
	return g_false;
}
static int str_cmp(ms_val *args, const char *who) {
	ms_val *a = ARG1, *b = ARG2;
	if (a->type != T_STR || b->type != T_STR) ms_log(MS_PANIC, "%s: expected strings", who);
	return strcmp(a->u.str, b->u.str);
}
static ms_val *bi_string_eq(ms_val *args) { return str_cmp(args, "string=?") == 0 ? g_true : g_false; }
static ms_val *bi_string_lt(ms_val *args) { return str_cmp(args, "string<?") < 0 ? g_true : g_false; }
static ms_val *bi_string_gt(ms_val *args) { return str_cmp(args, "string>?") > 0 ? g_true : g_false; }
static ms_val *bi_string_le(ms_val *args) { return str_cmp(args, "string<=?") <= 0 ? g_true : g_false; }
static ms_val *bi_string_ge(ms_val *args) { return str_cmp(args, "string>=?") >= 0 ? g_true : g_false; }
static ms_val *bi_string_to_symbol(ms_val *args) {
	if (ARG1->type != T_STR) ms_log(MS_PANIC, "string->symbol: expected a string");
	return mk_sym(ARG1->u.str);
}
static ms_val *bi_symbol_to_string(ms_val *args) {
	if (ARG1->type != T_SYM) ms_log(MS_PANIC, "symbol->string: expected a symbol");
	return mk_str(strdup(ARG1->u.str));
}

/* number<->string with the R4RS names (aliases of num->str/str->num, plus
 * an optional radix argument) */
static ms_val *bi_number_to_string(ms_val *args) {
	need_num(ARG1, "number->string");
	if (nilp(pcdr(args)) || (long)ARG2->u.num == 10) return mk_str(strdup(ms_to_string(ARG1, false)));
	long radix = (long)ARG2->u.num;
	long n = (long)ARG1->u.num;
	bool neg = n < 0; if (neg) n = -n;
	char buf[80]; int i = 80; buf[--i] = 0;
	if (n == 0) buf[--i] = '0';
	while (n > 0) { int d = n % radix; buf[--i] = d < 10 ? '0' + d : 'a' + d - 10; n /= radix; }
	if (neg) buf[--i] = '-';
	return mk_str(strdup(buf + i));
}
static ms_val *bi_string_to_number(ms_val *args) {
	if (ARG1->type != T_STR) ms_log(MS_PANIC, "string->number: expected a string");
	long radix = nilp(pcdr(args)) ? 10 : (long)ARG2->u.num;
	char *end;
	double v = (radix == 10) ? strtod(ARG1->u.str, &end) : (double)strtol(ARG1->u.str, &end, (int)radix);
	if (end == ARG1->u.str) return g_false;
	return mk_num(v);
}

/* characters */
static ms_val *bi_char_to_int(ms_val *args) {
	if (ARG1->type != T_CHAR) ms_log(MS_PANIC, "char->integer: expected a character");
	return mk_num((double)ARG1->u.ch);
}
static ms_val *bi_int_to_char(ms_val *args) { need_num(ARG1, "integer->char"); return mk_char((long)ARG1->u.num); }
static int char_cmp(ms_val *args, const char *who) {
	if (ARG1->type != T_CHAR || ARG2->type != T_CHAR) ms_log(MS_PANIC, "%s: expected characters", who);
	return (int)(ARG1->u.ch - ARG2->u.ch);
}
static ms_val *bi_char_eq(ms_val *args) { return char_cmp(args, "char=?") == 0 ? g_true : g_false; }
static ms_val *bi_char_lt(ms_val *args) { return char_cmp(args, "char<?") < 0 ? g_true : g_false; }
static ms_val *bi_char_gt(ms_val *args) { return char_cmp(args, "char>?") > 0 ? g_true : g_false; }
static ms_val *bi_char_le(ms_val *args) { return char_cmp(args, "char<=?") <= 0 ? g_true : g_false; }
static ms_val *bi_char_ge(ms_val *args) { return char_cmp(args, "char>=?") >= 0 ? g_true : g_false; }
static ms_val *bi_char_upcase(ms_val *args) { return mk_char(toupper((int)ARG1->u.ch)); }
static ms_val *bi_char_downcase(ms_val *args) { return mk_char(tolower((int)ARG1->u.ch)); }
static ms_val *bi_char_alphabetic_p(ms_val *args) { return isalpha((int)ARG1->u.ch) ? g_true : g_false; }
static ms_val *bi_char_numeric_p(ms_val *args) { return isdigit((int)ARG1->u.ch) ? g_true : g_false; }
static ms_val *bi_char_whitespace_p(ms_val *args) { return isspace((int)ARG1->u.ch) ? g_true : g_false; }
static ms_val *bi_char_upper_p(ms_val *args) { return isupper((int)ARG1->u.ch) ? g_true : g_false; }
static ms_val *bi_char_lower_p(ms_val *args) { return islower((int)ARG1->u.ch) ? g_true : g_false; }

/* vectors */
static void need_vec(ms_val *v, const char *who) { if (v->type != T_VECTOR) ms_log(MS_PANIC, "%s: expected a vector", who); }
static ms_val *bi_make_vector(ms_val *args) {
	need_num(ARG1, "make-vector");
	return mk_vector((long)ARG1->u.num, nilp(pcdr(args)) ? g_false : ARG2);
}
static ms_val *bi_vector(ms_val *args) {
	long n = list_length(args);
	PUSH(args);
	ms_val *v = mk_vector(n, g_false);
	POP();
	long i = 0;
	for (ms_val *p = args; !nilp(p); p = pcdr(p)) v->u.vec.items[i++] = pcar(p);
	return v;
}
static ms_val *bi_vector_length(ms_val *args) { need_vec(ARG1, "vector-length"); return mk_num((double)ARG1->u.vec.len); }
static ms_val *bi_vector_ref(ms_val *args) {
	need_vec(ARG1, "vector-ref"); need_num(ARG2, "vector-ref");
	long i = (long)ARG2->u.num;
	if (i < 0 || i >= ARG1->u.vec.len) ms_log(MS_PANIC, "vector-ref: index out of range");
	return ARG1->u.vec.items[i];
}
static ms_val *bi_vector_set(ms_val *args) {
	need_vec(ARG1, "vector-set!"); need_num(ARG2, "vector-set!");
	long i = (long)ARG2->u.num;
	if (i < 0 || i >= ARG1->u.vec.len) ms_log(MS_PANIC, "vector-set!: index out of range");
	ARG1->u.vec.items[i] = ARG3;
	return g_false;
}
static ms_val *bi_vector_fill(ms_val *args) {
	need_vec(ARG1, "vector-fill!");
	for (long i = 0; i < ARG1->u.vec.len; i++) ARG1->u.vec.items[i] = ARG2;
	return g_false;
}
static ms_val *bi_vector_to_list(ms_val *args) {
	need_vec(ARG1, "vector->list");
	ms_val *head = g_nil, *tail = NULL;
	PUSH(args); PUSH(head);
	for (long i = 0; i < ARG1->u.vec.len; i++) {
		ms_val *cell = mk_pair(ARG1->u.vec.items[i], g_nil);
		if (tail == NULL) { head = cell; ms_protect_stack[ms_protect_top - 1] = head; }
		else tail->u.pair.cdr = cell;
		tail = cell;
	}
	POPN(2);
	return head;
}
static ms_val *bi_list_to_vector(ms_val *args) {
	long n = list_length(ARG1);
	PUSH(args);
	ms_val *v = mk_vector(n, g_false);
	POP();
	long i = 0;
	for (ms_val *p = ARG1; !nilp(p); p = pcdr(p)) v->u.vec.items[i++] = pcar(p);
	return v;
}

/* memq/memv/assq/assv -- like member/assoc but using eq?/eqv? (here the
 * same function) instead of equal? */
static ms_val *bi_memq(ms_val *args) {
	for (ms_val *p = ARG2; !nilp(p); p = pcdr(p)) if (ms_eq(pcar(p), ARG1)) return p;
	return g_false;
}
static ms_val *bi_assq(ms_val *args) {
	for (ms_val *p = ARG2; !nilp(p); p = pcdr(p)) if (ms_eq(pcar(pcar(p)), ARG1)) return pcar(p);
	return g_false;
}

/* delay / force */
static ms_val *bi_force(ms_val *args) {
	ms_val *p = ARG1;
	if (p->type != T_PROMISE) return p; /* forcing a non-promise: pass through */
	if (!p->u.promise.forced) {
		ms_val *v = call_procedure(p->u.promise.thunk, g_nil);
		if (!p->u.promise.forced) { p->u.promise.value = v; p->u.promise.forced = true; }
	}
	return p->u.promise.value;
}

/* call/cc + dynamic-wind (escape-only continuations -- see README) */
static ms_val *bi_callcc(ms_val *args) {
	ms_val *f = ARG1;
	PUSH(f);
	ms_val *k = alloc_cell();
	k->type = T_CONT;
	k->u.cont = malloc(sizeof(ms_cont_data));
	k->u.cont->used = false;
	k->u.cont->wind_depth = ms_wind_top;
	k->u.cont->result = g_false;
	PUSH(k);
	int saved_protect_top = ms_protect_top;
	ms_val *result;
	if (setjmp(k->u.cont->buf) == 0) {
		result = call_procedure(f, mk_pair(k, g_nil));
		k->u.cont->used = true; /* normal return: can no longer be invoked */
	} else {
		ms_protect_top = saved_protect_top;
		result = k->u.cont->result;
	}
	POPN(2);
	return result;
}

static ms_val *bi_dynamic_wind(ms_val *args) {
	ms_val *before = ARG1, *thunk = ARG2, *after = ARG3;
	call_procedure(before, g_nil);
	if (ms_wind_top >= MS_MAX_WINDS) ms_log(MS_PANIC, "dynamic-wind: too many nested winds (increase MS_MAX_WINDS)");
	ms_wind_stack[ms_wind_top].before = before;
	ms_wind_stack[ms_wind_top].after = after;
	ms_wind_top++;
	ms_val *result = call_procedure(thunk, g_nil);
	ms_wind_top--;
	call_procedure(after, g_nil);
	return result;
}

/* ----------------------------------------------------------------------
 * ENV SETUP
 * ------------------------------------------------------------------- */

static void def_builtin(ms_val *env, const char *name, ms_builtin fn) {
	env_define(env, mk_sym(name), mk_fun(fn));
}

ms_val *ms_env_std(void) {
	ms_val *env = ms_new_env(g_nil);
	PUSH(env); /* not yet reachable via ms_global_env -- see comment above call site */

	def_builtin(env, "car", bi_car);
	def_builtin(env, "cdr", bi_cdr);
	def_builtin(env, "cons", bi_cons);
	def_builtin(env, "set-car!", bi_set_car);
	def_builtin(env, "set-cdr!", bi_set_cdr);
	def_builtin(env, "list", bi_list);
	def_builtin(env, "pair?", bi_pair_p);
	def_builtin(env, "null?", bi_null_p);
	def_builtin(env, "atom?", bi_atom_p);
	def_builtin(env, "number?", bi_num_p);
	def_builtin(env, "string?", bi_str_p);
	def_builtin(env, "symbol?", bi_sym_p);
	def_builtin(env, "boolean?", bi_bool_p);
	def_builtin(env, "char?", bi_char_p);
	def_builtin(env, "vector?", bi_vector_p);
	def_builtin(env, "procedure?", bi_proc_p);
	def_builtin(env, "macroexpand-1", bi_macroexpand_1);
	def_builtin(env, "eof-object?", bi_eof_object_p);
	def_builtin(env, "length", bi_length);

	def_builtin(env, "eq?", bi_eq);
	def_builtin(env, "eqv?", bi_eq);
	def_builtin(env, "equal?", bi_equal);
	def_builtin(env, "not", bi_not);

	def_builtin(env, "<", bi_lt);
	def_builtin(env, "<=", bi_lte);
	def_builtin(env, ">", bi_gt);
	def_builtin(env, ">=", bi_gte);
	def_builtin(env, "=", bi_numeq);

	def_builtin(env, "+", bi_add);
	def_builtin(env, "-", bi_sub);
	def_builtin(env, "*", bi_mul);
	def_builtin(env, "/", bi_div);
	def_builtin(env, "%", bi_mod);
	def_builtin(env, "mod", bi_mod);
	def_builtin(env, "quotient", bi_quotient);
	def_builtin(env, "remainder", bi_remainder);
	def_builtin(env, "modulo", bi_modulo);
	def_builtin(env, "gcd", bi_gcd);
	def_builtin(env, "lcm", bi_lcm);
	def_builtin(env, "expt", bi_expt);
	def_builtin(env, "abs", bi_abs);
	def_builtin(env, "floor", bi_floor);
	def_builtin(env, "ceiling", bi_ceiling);
	def_builtin(env, "truncate", bi_truncate);
	def_builtin(env, "round", bi_round);
	def_builtin(env, "sqrt", bi_sqrt);
	def_builtin(env, "min", bi_min);
	def_builtin(env, "max", bi_max);
	def_builtin(env, "zero?", bi_zero_p);
	def_builtin(env, "positive?", bi_positive_p);
	def_builtin(env, "negative?", bi_negative_p);
	def_builtin(env, "odd?", bi_odd_p);
	def_builtin(env, "even?", bi_even_p);
	def_builtin(env, "exact?", bi_exact_p);
	def_builtin(env, "inexact?", bi_inexact_p);
	def_builtin(env, "exact->inexact", bi_exact_to_inexact);
	def_builtin(env, "inexact->exact", bi_inexact_to_exact);

	def_builtin(env, "apply", bi_apply);
	def_builtin(env, "eval", bi_eval);
	def_builtin(env, "dump", bi_dump);
	def_builtin(env, "read", bi_read);
	def_builtin(env, "force", bi_force);
	def_builtin(env, "call/cc", bi_callcc);
	def_builtin(env, "call-with-current-continuation", bi_callcc);
	def_builtin(env, "dynamic-wind", bi_dynamic_wind);

	def_builtin(env, "print", bi_print);
	def_builtin(env, "display", bi_display);
	def_builtin(env, "write", bi_write);
	def_builtin(env, "gc", bi_gc);
	def_builtin(env, "exit", bi_exit);

	def_builtin(env, "str->list", bi_str_to_list);
	def_builtin(env, "list->str", bi_list_to_str);
	def_builtin(env, "num->str", bi_num_to_str);
	def_builtin(env, "str->num", bi_str_to_num);
	def_builtin(env, "str-append", bi_str_append);
	def_builtin(env, "string-append", bi_str_append);
	def_builtin(env, "str-length", bi_str_len);
	def_builtin(env, "number->string", bi_number_to_string);
	def_builtin(env, "string->number", bi_string_to_number);
	def_builtin(env, "string->symbol", bi_string_to_symbol);
	def_builtin(env, "symbol->string", bi_symbol_to_string);
	def_builtin(env, "make-string", bi_make_string);
	def_builtin(env, "string", bi_string);
	def_builtin(env, "string-length", bi_str_len);
	def_builtin(env, "string-ref", bi_string_ref);
	def_builtin(env, "string-set!", bi_string_set);
	def_builtin(env, "substring", bi_substring);
	def_builtin(env, "string-copy", bi_string_copy);
	def_builtin(env, "string-fill!", bi_string_fill);
	def_builtin(env, "string=?", bi_string_eq);
	def_builtin(env, "string<?", bi_string_lt);
	def_builtin(env, "string>?", bi_string_gt);
	def_builtin(env, "string<=?", bi_string_le);
	def_builtin(env, "string>=?", bi_string_ge);

	def_builtin(env, "char->integer", bi_char_to_int);
	def_builtin(env, "integer->char", bi_int_to_char);
	def_builtin(env, "char=?", bi_char_eq);
	def_builtin(env, "char<?", bi_char_lt);
	def_builtin(env, "char>?", bi_char_gt);
	def_builtin(env, "char<=?", bi_char_le);
	def_builtin(env, "char>=?", bi_char_ge);
	def_builtin(env, "char-upcase", bi_char_upcase);
	def_builtin(env, "char-downcase", bi_char_downcase);
	def_builtin(env, "char-alphabetic?", bi_char_alphabetic_p);
	def_builtin(env, "char-numeric?", bi_char_numeric_p);
	def_builtin(env, "char-whitespace?", bi_char_whitespace_p);
	def_builtin(env, "char-upper-case?", bi_char_upper_p);
	def_builtin(env, "char-lower-case?", bi_char_lower_p);

	def_builtin(env, "make-vector", bi_make_vector);
	def_builtin(env, "vector", bi_vector);
	def_builtin(env, "vector-length", bi_vector_length);
	def_builtin(env, "vector-ref", bi_vector_ref);
	def_builtin(env, "vector-set!", bi_vector_set);
	def_builtin(env, "vector-fill!", bi_vector_fill);
	def_builtin(env, "vector->list", bi_vector_to_list);
	def_builtin(env, "list->vector", bi_list_to_vector);

	def_builtin(env, "memq", bi_memq);
	def_builtin(env, "memv", bi_memq);
	def_builtin(env, "assq", bi_assq);
	def_builtin(env, "assv", bi_assq);

#ifndef LIX
	def_builtin(env, "file->str", bi_file_to_str);
	def_builtin(env, "load", bi_load);
#endif

	POP();
	return env;
}

#ifdef MS_STATIC_HEAP
/* NOT part of upstream ms.c. Compile-time-sized static arrays instead
 * of ms_init()'s own malloc(MS_HEAP_SIZE * sizeof(ms_val)) /
 * malloc(MS_PROTECT_STACK_SIZE * sizeof(ms_val *)) -- opt-in via this
 * macro, upstream's own malloc-based behavior is completely unchanged
 * when it's left undefined.
 *
 * Why this exists: on a target where a process's total memory is
 * fixed at CREATION time from its own binary's static footprint (see
 * Zeitlos's docs/app_runtime.md -- k_proc_create() is handed exactly
 * fs_size(binary), no headroom reserved for later runtime malloc()
 * growth beyond that), a large single runtime malloc() for the cell
 * heap has no guarantee of the room it needs -- _sbrk() can only grow
 * into memory the process was actually allocated up front. Declaring
 * the heap as an ordinary static array instead makes its size part of
 * the binary's own .bss, and therefore part of exactly what
 * k_proc_create() ends up requesting -- deterministic, known at
 * compile time, no dependency on how generously (or not) a given
 * target's allocator rounds up an oversized runtime malloc().
 * `lisp` (sw/apps/lisp/lisp.c) builds with this defined; nothing about
 * this changes ms's own cons/GC code at all -- ms_heap/ms_freelist
 * are still plain pointers as far as the rest of this file is
 * concerned, just pointed at static storage instead of malloc'd
 * storage in ms_init(), and never free()'d in ms_deinit() (freeing
 * static storage isn't meaningful -- see below). */
static ms_val ms_heap_storage[MS_HEAP_SIZE];
static ms_val *ms_protect_stack_storage[MS_PROTECT_STACK_SIZE];
#endif

void ms_init(void) {
#ifdef MS_STATIC_HEAP
	if (!ms_heap) ms_heap = ms_heap_storage;
	if (!ms_protect_stack) ms_protect_stack = ms_protect_stack_storage;
#else
	if (!ms_heap) ms_heap = malloc(MS_HEAP_SIZE * sizeof(ms_val));
	if (!ms_protect_stack) ms_protect_stack = malloc(MS_PROTECT_STACK_SIZE * sizeof(ms_val *));
#endif
	ms_heap_live = 0;
	ms_freelist = NULL;
	for (long i = MS_HEAP_SIZE - 1; i >= 0; i--) {
		ms_heap[i].type = T_FREE;
		ms_heap[i].u.pair.cdr = ms_freelist;
		ms_freelist = &ms_heap[i];
	}
	ms_symbols = NULL;
	ms_protect_top = 0;
	ms_wind_top = 0;
	memset(sym_hash, 0, sizeof sym_hash);
	sym_hash_n = 0;
	sym_hash_off = false;
	ghash_env = NULL;
	ghash_full = false;

	/* g_nil/g_true/g_false/g_unassigned/g_eof are permanent singletons;
	 * protect them forever by pushing them at the bottom of the protect
	 * stack (this also makes gc_mark reachable-from-roots work even
	 * before ms_global_env exists). */
	g_nil = alloc_cell();
	g_nil->type = T_NIL;
	PUSH(g_nil);

	g_true = alloc_cell();
	g_true->type = T_BOOL;
	PUSH(g_true);

	g_false = alloc_cell();
	g_false->type = T_BOOL;
	PUSH(g_false);

	g_unassigned = alloc_cell();
	g_unassigned->type = T_SYM;
	g_unassigned->u.str = (char *)malloc(1);
	g_unassigned->u.str[0] = 0;
	PUSH(g_unassigned);

	g_eof = alloc_cell();
	g_eof->type = T_SYM;
	g_eof->u.str = (char *)malloc(1);
	g_eof->u.str[0] = 0;
	PUSH(g_eof);

	sym_quote = mk_sym("quote");
	sym_if = mk_sym("if");
	sym_cond = mk_sym("cond");
	sym_begin = mk_sym("begin");
	sym_lambda = mk_sym("lambda");
	sym_define = mk_sym("define");
	sym_defun = mk_sym("defun");
	sym_define_macro = mk_sym("define-macro");
	sym_let = mk_sym("let");
	sym_letstar = mk_sym("let*");
	sym_letrec = mk_sym("letrec");
	sym_set = mk_sym("set!");
	sym_and = mk_sym("and");
	sym_or = mk_sym("or");
	sym_while = mk_sym("while");
	sym_quasiquote = mk_sym("quasiquote");
	sym_unquote = mk_sym("unquote");
	sym_unquote_splicing = mk_sym("unquote-splicing");
	sym_do = mk_sym("do");
	sym_case = mk_sym("case");
	sym_delay = mk_sym("delay");
	sym_else = mk_sym("else");
	sym_arrow = mk_sym("=>");

	ms_global_env = ms_env_std();
	env_define(ms_global_env, mk_sym("nil"), g_nil);
	env_define(ms_global_env, mk_sym("t"), g_true);
	env_define(ms_global_env, mk_sym("true"), g_true);
	env_define(ms_global_env, mk_sym("false"), g_false);
}

/* NOT part of upstream ms.c. Small read-only accessors for an
 * embedder that wants to report memory usage (e.g. Zeitlos's `lisp`
 * app's "free" command, sw/apps/lisp/lisp.c) without reaching into
 * ms's own internals -- ms_heap_live is otherwise `static` (invisible
 * outside this file), and sizeof(ms_val) is otherwise unknowable from
 * outside since ms_val is deliberately kept opaque in
 * sw/apps/lisp/ms_api.h. Both trivial, both just read a value that
 * already exists -- no new state, no behavior change to anything
 * else in this file. */
long ms_heap_used(void) { return ms_heap_live; }
size_t ms_cell_size(void) { return sizeof(ms_val); }

/* NOT part of upstream ms.c. Registration + return-value-construction
 * API for an embedder (Zeitlos's sw/apps/repl/zapi.c -- see
 * docs/scheme_api.md) that wants to add its OWN C-backed Scheme
 * procedures without needing direct access to this file's own
 * (otherwise `static`) mk_fun()/mk_sym()/env_define()/mk_str()/
 * mk_num()/mk_pair(). One-time patch: everything an embedder adds
 * AFTER this point is a plain C function of the same
 * ms_val *(*)(ms_val *args) shape every bi_* builtin above already
 * has, registered via ms_def_builtin() -- ms.c itself never needs
 * touching again for a new procedure.
 *
 * The scalar constructors (ms_mk_str/_num/_bool, ms_nil_val) are safe
 * to call directly and return immediately -- each is exactly one
 * alloc_cell() call, no intermediate unprotected state between two
 * allocations the way a hand-rolled multi-step construction could
 * leave one exposed to a GC triggered by the next (see mk_pair()'s
 * own PUSH/POP pattern for why THAT needs protecting and a single
 * mk_str()/mk_num() call doesn't). Building anything bigger than one
 * cell from outside this file goes through a ready-made composite
 * constructor instead (ms_mk_str_list() below) rather than exposing
 * PUSH/POP themselves -- keeps every embedder-side builtin from
 * having to get GC-safety right by hand. */

void ms_def_builtin(const char *name, ms_builtin fn) {
	env_define(ms_global_env, mk_sym(name), mk_fun(fn));
}

/* true if `name` is currently bound to something callable (a builtin
 * or a closure) in the global environment -- used by repl.c's
 * bare-word command-syntax translation (docs/scheme_api.md \S1) to
 * decide whether e.g. typing `ls` should become `(ls)` before
 * evaluating. Interning a symbol just to check it costs the same
 * small, one-time-per-unique-name amount typing that name normally
 * would already pay (mk_sym() below) -- nothing new. */
bool ms_is_callable(const char *name) {
	ms_val *v = env_lookup(ms_global_env, mk_sym(name));
	return v && (v->type == T_FUN || v->type == T_LAMBDA);
}

ms_val *ms_mk_str(char *owned) { return mk_str(owned); } /* takes ownership */
ms_val *ms_mk_num(double n) { return mk_num(n); }
ms_val *ms_mk_bool(bool b) { return b ? g_true : g_false; }
ms_val *ms_nil_val(void) { return g_nil; }

/* builds a proper Scheme list of strings from a plain C array --
 * takes ownership of every string in `items` (same convention
 * ms_mk_str() uses), same as if the caller had called ms_mk_str() on
 * each one and consed them together itself, just without needing
 * this file's own (otherwise static) PUSH/POP to stay GC-safe across
 * the several allocations that takes. Safe for count == 0 (returns
 * the empty list). */
/* type/value accessors for embedder-defined builtins -- ms_val stays
 * fully opaque (see ms_api.h's own comment on why), these are the
 * ONLY way a zapi_* function can inspect its own arguments. Pure
 * reads, no allocation -- always safe to call, nothing GC-related to
 * think about. */
bool ms_is_str(ms_val *v) { return v && v->type == T_STR; }
bool ms_is_num(ms_val *v) { return v && v->type == T_NUM; }
bool ms_is_pair(ms_val *v) { return v && v->type == T_PAIR; }
bool ms_is_nil(ms_val *v) { return nilp(v); }
const char *ms_str_val(ms_val *v) { return v->u.str; }
double ms_num_val(ms_val *v) { return v->u.num; }
ms_val *ms_car(ms_val *v) { return pcar(v); }
ms_val *ms_cdr(ms_val *v) { return pcdr(v); }

ms_val *ms_mk_str_list(char **items, int count) {
	ms_val *head = g_nil;
	PUSH(head);
	for (int i = count - 1; i >= 0; i--) {
		ms_val *s = mk_str(items[i]);
		PUSH(s);
		ms_val *cell = mk_pair(s, head);
		POP();
		head = cell;
		ms_protect_stack[ms_protect_top - 1] = head;
	}
	POP();
	return head;
}

void ms_deinit(void) {
#ifdef MS_STATIC_HEAP
	/* static storage -- nothing to free(); just drop the pointers so
	 * a later ms_init() re-points them and re-runs its setup, same
	 * as the malloc'd case re-mallocing on a NULL check. The RAM
	 * itself was never reclaimable (it's part of `lisp`'s own fixed
	 * process footprint regardless of whether ms is "in use" -- see
	 * the MS_STATIC_HEAP comment above), unlike upstream's
	 * malloc'd-heap case, where ms_deinit() genuinely does free real
	 * memory back to the process's own heap. */
#else
	free(ms_heap);
	free(ms_protect_stack);
#endif
	ms_heap = NULL;
	ms_protect_stack = NULL;
	ms_global_env = NULL;
	ms_symbols = NULL;
	ms_freelist = NULL;
	ms_heap_live = 0;
	ms_protect_top = 0;
	ms_wind_top = 0;
}

#ifdef LIX
void ms_init_lix(bool stdlib) {
	ms_init();
	if (stdlib) ms_load_string((const char *)ms_stdlib_l, ms_global_env);
}
#endif

/* ----------------------------------------------------------------------
 * REPL / MAIN (desktop build only)
 * ------------------------------------------------------------------- */

#ifndef LIX

#ifdef MS_USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

/* Tracks whether `buf` contains a "complete" read: parens balanced and
 * not mid-string-literal, so the REPL knows whether to keep prompting
 * for more lines or go ahead and parse/evaluate what it has. Comments
 * and string escapes are honored so a `;` or an escaped quote inside a
 * string doesn't confuse the depth count. */
static bool input_is_complete(const char *s) {
	int depth = 0;
	bool in_str = false;
	for (const char *p = s; *p; p++) {
		if (in_str) {
			if (*p == '\\' && p[1]) { p++; continue; }
			if (*p == '"') in_str = false;
			continue;
		}
		if (*p == ';') { while (*p && *p != '\n') p++; if (!*p) break; continue; }
		if (*p == '"') { in_str = true; continue; }
		if (*p == '(') depth++;
		else if (*p == ')') { if (depth > 0) depth--; }
	}
	return depth <= 0 && !in_str;
}

static bool input_has_content(const char *s) {
	for (const char *p = s; *p; p++)
		if (!isspace((unsigned char)*p)) return true;
	return false;
}

/* Reads one line of input, always returning a malloc'd, NUL-terminated
 * buffer with a trailing '\n' (or NULL at EOF) so the rest of the REPL
 * doesn't need to care whether readline or plain getline() supplied it.
 *
 * With -DMS_USE_READLINE: uses GNU readline for arrow-key editing and
 * command history (added per non-empty line). This is opt-in specifically
 * so the default build -- including the embedded/LIX target -- never
 * requires linking against readline.
 *
 * Without it: getline(), which still has no fixed line-length limit; you
 * just don't get history/arrow-key recall (piping through `rlwrap` is a
 * lightweight way to get that without a build-time dependency). */
static char *read_one_line(const char *prompt) {
#ifdef MS_USE_READLINE
	char *rl = readline(prompt);
	if (rl == NULL) return NULL; /* EOF (Ctrl-D) */
	if (rl[0] != '\0') add_history(rl);
	size_t len = strlen(rl);
	char *buf = malloc(len + 2);
	memcpy(buf, rl, len);
	buf[len] = '\n';
	buf[len + 1] = '\0';
	free(rl);
	return buf;
#else
	printf("%s", prompt);
	fflush(stdout);
	char *line = NULL;
	size_t cap = 0;
	ssize_t n = getline(&line, &cap, stdin);
	if (n < 0) { free(line); return NULL; }
	return line; /* getline NUL-terminates and keeps the trailing '\n' */
#endif
}

static void repl(ms_val *env) {
	char *acc = NULL;    /* accumulated (possibly multi-line) input */
	size_t acclen = 0, acccap = 0;

	for (;;) {
		char *line = read_one_line(acclen ? "...  " : "ms> ");
		if (line == NULL) { /* EOF */
			if (acclen) printf("\n");
			break;
		}

		size_t n = strlen(line);
		size_t need = acclen + n + 1;
		if (need > acccap) {
			acccap = need < 256 ? 256 : need * 2;
			acc = realloc(acc, acccap);
		}
		memcpy(acc + acclen, line, n + 1);
		acclen += n;
		free(line);

		if (!input_has_content(acc)) { acclen = 0; continue; }
		if (!input_is_complete(acc)) continue; /* prompt for more lines */

		const char *p = acc;
		for (;;) {
			ms_val *form = ms_read(&p);
			if (form == NULL) break;
			PUSH(form);
			ms_val *r = ms_eval(form, env);
			POP();
			ms_print(r, true);
			printf("\n");
		}
		acclen = 0;
	}

	free(acc);
}

int main(int argc, char **argv) {
	ms_init();
	if (argc > 1) {
		for (int i = 1; i < argc; i++) ms_load_file(argv[i], ms_global_env);
		return 0;
	}
	repl(ms_global_env);
	return 0;
}
#endif /* !LIX */
