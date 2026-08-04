# Machdyne Scheme

A small Lisp interpreter targeting a documented subset of **R4RS** Scheme,
with mark-and-sweep garbage collection and proper tail calls, designed
to run on very small SOCs/MCUs as well as typical computers.

```
$ make
$ ./ms
ms> (load "ms_stdlib.l")
#t
ms> (define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))
fact
ms> (fact 10)
3628800
```

## Contents

- [Quick start](#quick-start)
- [Optional readline support](#optional-readline-support)
- [A single-line test for GC + tail calls](#a-single-line-test-for-gc--tail-calls)
- [Language overview](#language-overview)
- [R4RS: what's supported, what's not, and why](#r4rs-whats-supported-whats-not-and-why)
- [Memory model & garbage collection](#memory-model--garbage-collection)
- [Tail calls](#tail-calls)
- [Embedding](#embedding)
- [Sizing the heap for small targets](#sizing-the-heap-for-small-targets)
- [Built-in functions and special forms](#built-in-functions-and-special-forms)
- [Standard library](#standard-library)
- [Testing](#testing)
- [Known limitations](#known-limitations)
- [Relationship to the original implementation](#relationship-to-the-original-implementation)

## Quick start

All source, test and library files live in one directory.

```
make            # builds ./ms (desktop REPL)
make run        # builds and starts the REPL
make test       # builds and runs the full test suite
make small      # builds with a 2000-cell heap, as a sanity check for
                # small-memory configurations
make readline   # builds with GNU readline for arrow-key editing/history
                # (opt-in; see "Optional readline support" below)
```

The REPL loads nothing automatically; load the standard library
yourself if you want it:

```
ms> (load "ms_stdlib.l")
```

You can also run a script directly:

```
$ ./ms myscript.l
```

The desktop REPL reads input with `getline()`, so there's no fixed
line-length limit, and it accumulates input across multiple lines
(showing a `...` continuation prompt) until parentheses balance, so you
can paste or type a multi-line expression naturally:

```
ms> (define (fact n)
...    (if (= n 0) 1 (* n (fact (- n 1)))))
fact
```

It also evaluates every top-level form on a line, not just the first, so
`(+ 1 2) (+ 3 4)` prints both `3` and `7`. By default, basic line editing
(backspace, etc.) comes from your terminal's own line discipline and
there's no persistent command history; build with `make readline` (or
`make USE_READLINE=1`) for real arrow-key editing and history via GNU
readline -- see [Optional readline support](#optional-readline-support).

## Optional readline support

By default the desktop REPL uses `getline()` for input -- no fixed
line-length limit, but only whatever line editing your terminal itself
provides, and no persistent command history.

Build with GNU readline instead for real arrow-key editing, `Ctrl-R`
history search, etc.:

```
make readline          # shorthand for: make USE_READLINE=1
```

This requires readline's development headers (`libreadline-dev` on
Debian/Ubuntu, `readline-devel` on Fedora, `readline` via Homebrew on
macOS). It is entirely opt-in: the default `make`/`make small` targets
never link against readline, and the embedded (`-DLIX`) build path
never references it at all, so it costs nothing if you don't ask for it.

If you'd rather not add a build dependency but still want history/arrow
keys occasionally, piping the default build through
[`rlwrap`](https://github.com/hanslub42/rlwrap) (`rlwrap ./ms`)
gets you similar behavior without recompiling.

## A single-line test for GC + tail calls

This one-liner exercises both properties described below at once: it
only succeeds if tail calls really run in constant C-stack space (3
million recursive calls) *and* the garbage collector is actually
reclaiming old environments/numbers as it goes (the heap is capped at
1500 cells -- enough to boot the larger R4RS builtin table, but nowhere
near enough to hold 3 million iterations' worth of garbage if nothing
were being collected):

```sh
make MS_HEAP_SIZE=1500
echo '(define (loop n acc) (if (= n 0) acc (loop (- n 1) (+ acc 1)))) (if (= (loop 3000000 0) 3000000) (exit 0) (exit 1))' | ./ms; echo "exit=$?"
```

`exit=0` means both tail-call optimization and garbage collection are
working; a crash, a hang, or an `out of memory`/panic message means one
of them regressed. `run_tests.sh` runs this same check (and a dedicated
boot-safety variant, see [Testing](#testing)) as part of `make test`.

## Language overview

Machdyne Scheme follows R4RS's evaluation model: closures capture their
defining environment lexically, symbols are looked up by walking the
environment chain outward to the global environment, and **only `#f` is
false** -- `'()` (the empty list), `0`, and `""` are all true, matching
R4RS/R5RS and *unlike* some toy Lisps (including earlier revisions of
this one) that conflate "empty" with "false".

Data types: pairs/lists, symbols, booleans (`#t`/`#f`), numbers (double
precision), characters, strings, vectors, built-in procedures, closures,
promises (`delay`/`force`), and escape-only continuations (`call/cc`).
There is no separate exact/inexact numeric tower and no bignums/rationals
-- see [R4RS: what's supported](#r4rs-whats-supported-whats-not-and-why).

```scheme
(+ 1 2 3)                     ; => 6
(define x 10)                 ; => x
(define (square x) (* x x))   ; => square   (function-definition shorthand)
(square 5)                    ; => 25
(lambda (x y) (+ x y))        ; => #<procedure>
((lambda (x y) (+ x y)) 3 4)  ; => 7

'(1 2 3)                      ; => (1 2 3)      quote
`(1 ,(+ 1 1) ,@(list 3 4))    ; => (1 2 3 4)    quasiquote / unquote / splice
#(1 2 3)                      ; => #(1 2 3)     vector literal
#\a  #\space  #\newline       ; character literals

(if (> x 5) "big" "small")
(cond ((= x 1) "one")
      ((= x 2) "two")
      (else "other"))
(case x ((1 2) "low") ((3 4) "high") (else "other"))

(let ((a 1) (b 2)) (+ a b))          ; parallel bindings
(let* ((a 1) (b (+ a 1))) b)         ; sequential bindings
(letrec ((ev? (lambda (n) (if (= n 0) #t (od? (- n 1)))))
         (od? (lambda (n) (if (= n 0) #f (ev? (- n 1))))))
  (ev? 10))                          ; mutually-recursive bindings

(let loop ((i 0) (acc 0))            ; named let -- properly tail-recursive
  (if (= i 1000000) acc (loop (+ i 1) (+ acc 1))))

(do ((i 0 (+ i 1)) (acc 0 (+ acc i))) ; do loop
    ((= i 5) acc))

(define (sum-list lst)               ; ordinary recursion
  (if (null? lst) 0 (+ (car lst) (sum-list (cdr lst)))))

(define (loop n acc)                 ; tail recursion -- O(1) stack space
  (if (= n 0) acc (loop (- n 1) (+ acc 1))))
(loop 10000000 0)                    ; => 10000000, does not overflow

(define (adder n) (lambda (x) (+ x n)))  ; closures
(define add5 (adder 5))
(add5 10)                            ; => 15

(define (f a b . rest) rest)         ; variadic / rest params
(f 1 2 3 4 5)                        ; => (3 4 5)
(define (g . xs) xs)                 ; fully variadic

(force (delay (+ 1 2)))              ; => 3, memoized

(call/cc (lambda (k) (+ 1 (k 42))))  ; => 42, escape continuation
```

## R4RS: what's supported, what's not, and why

This implementation targets a **documented subset of R4RS** rather than
claiming strict full conformance. That's a deliberate choice, not an
oversight: R4RS's two heaviest features (hygienic macros and fully
re-entrant continuations) are in real tension with running on a
microcontroller with a few KB of RAM, and real embedded Schemes make the
same trade-off -- for example
[PICOBIT](https://www.ccs.neu.edu/~stamourv/papers/picobit.pdf), which
targets <8KB RAM/<64KB ROM, explicitly implements "most of R4RS" and
drops the same features for the same reasons.

**Supported:**
- All primitive and derived expression types except macros: `quote`,
  `if`, `set!`, `lambda`, `define` (value and `(define (f args) body)`
  shorthand), `begin`, `let`, `let*`, `letrec`, named `let`, `do`,
  `cond` (with `else` and `=>`), `case` (with `else`), `and`, `or`,
  `quasiquote`/`unquote`/`unquote-splicing`, `delay`.
- Correct R4RS truthiness (only `#f` is false).
- The full disjoint type set: booleans, pairs, symbols, numbers,
  characters, strings (mutable), vectors, procedures.
- `call/cc` / `call-with-current-continuation` and `dynamic-wind` --
  **escape-only** (see below).
- `apply` (full variadic form), `eval`, `force`, the `eq?`/`eqv?`/
  `equal?` family, `memq`/`memv`/`assq`/`assv`/`member`/`assoc`, and a
  large standard procedure set for numbers, characters, strings, and
  vectors (see [Built-in functions](#built-in-functions-and-special-forms)).

**Not supported, with reasons:**

- **Hygienic macros** (`define-syntax`/`syntax-rules`/`let-syntax`).
  These were an *optional* appendix in R4RS (not required for a
  conforming "core" implementation), and hygiene is enough of an
  engineering project on its own that it isn't included here. Ordinary
  procedures cover most of what you'd reach for a macro for.
- **Full (re-entrant, multi-shot) continuations.** `call/cc` here is
  implemented with `setjmp`/`longjmp`: it can escape outward to an
  enclosing `call/cc` (early return, exception-like unwinding, `for-each`
  with early exit -- see the tests) but a captured continuation **cannot
  be invoked again after its `call/cc` has already returned**. There is
  no support for re-entering a completed extent, so patterns like
  generators, coroutines, or `amb`-style backtracking via continuations
  don't work. This is the single feature most fundamentally in tension
  with this interpreter's design: full continuations require either
  CPS-converting the whole evaluator or copying raw C stack memory,
  neither of which fits a direct AST-walking interpreter that leans on
  the C call stack for control flow. `dynamic-wind` is implemented
  correctly *for this escape-only model*: `after` thunks reliably run
  when a continuation escapes past them (tested), but there's no
  "re-entering a wind" case to handle, since escape-only continuations
  never re-enter.
- **No separate exact/inexact numeric tower, no rationals, no
  bignums, no complex numbers.** All numbers are C `double`s. R4RS
  explicitly permits implementations to support a documented subset of
  the numeric tower, so `exact?` always reports `#f`, `inexact?` always
  reports `#t`, and `exact->inexact`/`inexact->exact` are identity
  functions -- present for API compatibility, not because there's a real
  distinction underneath.
- **No ports / file I/O in the language proper.** `read` takes a string,
  not a port; there's no `open-input-file`, `current-output-port`, etc.
  `load` and `file->str` exist but are plain filesystem helpers, and are
  compiled out entirely under `-DLIX` (see
  [Embedding](#embedding) since a real port
  abstraction is at odds with "no file I/O on this target."
- **`list?` is lenient.** It's implemented as "not a pair and not `()`,
  or a pair" rather than a proper (cycle-safe) well-formed-list check,
  so it returns `#t` for a dotted pair. Fine for everyday code; not
  spec-exact.
- Special forms (`if`, `let`, `define`, ...) cannot be shadowed/redefined
  by user code, since they're recognized by the evaluator before normal
  symbol lookup happens.

If you need any of the unsupported items for a specific program, they're
generally addable without touching the core design (e.g. a non-hygienic
macro system, or ports backed by in-memory string buffers) -- the two
truly architecture-level blockers are hygienic macros and full
continuations.

## Memory model & garbage collection

Cons cells, numbers, characters, closures, promises, continuations, and
even **environments** all live in a single fixed-size static array of
"cells" (`ms_heap[MS_HEAP_SIZE]`) -- there is no per-object `malloc`/
`free` for these, so there's no heap fragmentation and the memory cost
is exactly `MS_HEAP_SIZE * sizeof(ms_val)` (`sizeof(ms_val)` is
typically 24-40 bytes depending on your platform's pointer size).
Strings and vectors still use `malloc`/`free` for their variable-length
backing storage, since a fixed-size cell can't hold them directly.

An environment is represented as an ordinary pair, `(frame . parent)`,
where `frame` is itself a mutable list of `(symbol . value)` pairs. This
is the key design choice that fixes the original implementation's most
serious bug: because environments are built from the same cells as
everything else, a closure's captured environment is automatically kept
alive for as long as the closure itself is reachable, and automatically
collected once neither is reachable. There is no separate, easy-to-forget
"also mark the lambda's environment" step.

Garbage collection is a straightforward two-phase mark-and-sweep:

1. **Mark.** Starting from the roots -- the global environment, the
   interned-symbol table, the active `dynamic-wind` thunks, and an
   explicit "protect stack" (below) -- recursively mark every reachable
   cell (following into vector elements and promise contents too).
2. **Sweep.** Walk the entire static array; any unmarked, in-use cell is
   freed (its string/vector/continuation storage, if any, is `free()`d)
   and returned to the free list.

GC runs automatically and only when the free list is empty and a new
cell is needed (`alloc_cell`). If, after a collection, there is still no
free cell, the interpreter reports a clear
`out of memory: heap exhausted (...)` panic and exits -- it does not
silently corrupt memory or crash unpredictably. (This exact property was
the subject of a real bug found and fixed while adding R4RS support: the
global environment being constructed at boot wasn't rooted until fully
built, so a too-small heap could corrupt it mid-boot instead of failing
cleanly. `run_tests.sh` has a dedicated regression test for this.)

### The protect stack

C's own call stack cannot be scanned conservatively in this
implementation (deliberately, for portability to small/unusual MCU
toolchains), so the interpreter maintains its own small explicit stack
of pointers -- `ms_protect_stack` -- that roots any value that has been
allocated but is not yet reachable from an environment (for example, a
list being built up argument-by-argument during a function call). Every
allocation site that needs this protection pushes onto this stack before
allocating further and pops when the value becomes reachable some other
way (bound into an environment, linked into a larger structure, or
returned to a caller that will protect it in turn).

This stack is bounded (`MS_PROTECT_STACK_SIZE`) and, unlike the cell
heap, its usage scales with **non-tail** call nesting depth, not with
the size of your data. Proper tail calls (see below) reuse their slot in
this stack rather than growing it, so a tail-recursive loop of any
length uses *constant* protect-stack space. A deep chain of *non-tail*
recursive calls will eventually exhaust it, at which point you get a
clean `protect stack overflow` panic rather than memory corruption --
increase `MS_PROTECT_STACK_SIZE` or make the recursion tail-recursive.

Escaping a captured continuation (`call/cc`) resets the protect stack
back to its depth at capture time, so an escape can never leak protect
stack slots even across many `call/cc` invocations.

## Tail calls

`ms_eval` is written as an explicit trampoline: a `goto` back to the top
of the evaluation loop, updating the expression and environment in
place, rather than a recursive C call. This applies to:

- the last expression of `if`/`cond`/`case`/`begin`/`let`/`let*`/
  `letrec`/`and`/`or`
- the result expression of `do`, once its test becomes true
- named `let` -- the common idiom for a tail-recursive loop -- desugars
  to a self-referential closure whose recursive calls go through the
  same tail-call path as any other function call
- the last expression of a `lambda`/`defun`/`define` body
- a direct function call in any of the positions above

so code like this runs in O(1) C-stack space no matter how many
iterations it performs:

```scheme
(define (loop n acc) (if (= n 0) acc (loop (- n 1) (+ acc 1))))
(loop 10000000 0)  ; verified in the test suite; does not overflow the
                    ; C stack, and does not exhaust a tiny cell heap
                    ; either, since old environments are reclaimed by GC
```

Non-tail recursion (e.g. `(+ n (fact (- n 1)))`, where the recursive
call's result still has work done to it afterwards) still uses one C
stack frame *and* a handful of protect-stack slots per level, exactly
like any other Lisp -- see [Known limitations](#known-limitations).

## Embedding

LIX is the OS used by the [Zucker](https://github.com/machdyne/zucker) SOC. LIX mode can be used to embed Machdyne Scheme in small embedded systems.

Machdyne Scheme will be the primary intepreted language used in [Zeitlos](https://github.com/machdyne/zeitlos), the succesor to Zucker.

Building with `-DLIX` removes every use of `fopen`/`fread`/`fseek`/
`fclose` (no filesystem calls at all): `load` and `file->str` are
compiled out, and the standard library is instead expected to be
compiled in as a C string constant.

```sh
gcc -DLIX -DMS_HEAP_SIZE=1500 -DMS_PROTECT_STACK_SIZE=128 \
    -I/path/to/your/platform/headers \
    -c ms.c -o ms.o
```

Your platform must provide the headers the original LIX integration
expected (`include/fs.h`, `../common/zucker.h`) and an `ms_stdlib.h` that
defines:

```c
static const char ms_stdlib_l[] = "...(your stdlib source, or a trimmed"
                                   " subset of ms_stdlib.l)...";
```

At startup call:

```c
ms_init_lix(true);   /* true = also evaluate the embedded stdlib */
```

`ms_global_env` is then ready to use with `ms_eval`/`ms_read`/
`ms_load_string`. `run_tests.sh` includes a build-only check (with stub
headers) that confirms the `-DLIX` code path compiles clean of any file-I/O
calls; it can't run real firmware, so exercise your actual target build too.

`call/cc`/`dynamic-wind` use standard C `setjmp.h`, which is available
on essentially every embedded toolchain, so they work under `-DLIX` too.

## Sizing the heap for small targets

Two numbers matter:

- `MS_HEAP_SIZE` -- number of cells. Cons cells, numbers, characters,
  closures, promises, continuations, and environments all cost one cell
  each (`sizeof(ms_val)`, ~24-40 bytes); strings and vectors additionally
  cost their own `malloc`'d backing storage. The standard library plus
  its ~130 built-in procedures cost a *fixed* baseline of roughly
  700-750 cells just for the global environment and the interned symbol
  table (this grew substantially with the R4RS builtin set -- an earlier,
  smaller-builtin-table version of this interpreter needed only ~300
  cells to boot) -- budget for that before your program's own working
  set. Trim `ms_stdlib.l` (or the builtin table in `ms_env_std`) if you
  need to shrink this baseline further.
- `MS_PROTECT_STACK_SIZE` -- number of pointers (4-8 bytes each). Sized
  by how deep your *non-tail* recursion goes, not by your data size.

For a true ~2KB target: at ~30 bytes/cell, 2KB buys roughly 65 cells --
nowhere near enough to boot the full builtin table as shipped.
Realistically, budget several KB (e.g. 16-24KB) for the full standard
library plus a useful protect stack, or ship a hand-trimmed builtin/
stdlib set for anything tighter (dropping, say, the string/vector/char
procedure sets you don't need saves a meaningful chunk of the boot
baseline). The point of this design is that there is no other hidden
memory cost scaling with your program -- what you configure is what you
get, and an undersized heap now fails cleanly at boot rather than
corrupting silently (see [Memory model](#memory-model--garbage-collection)).

`make small` builds a 2000-cell / 256-slot-protect-stack binary on your
desktop -- comfortably above the ~700-750 cell boot baseline, with
headroom for real programs -- as a way to sanity-check a small
configuration before cross-compiling. To see the clean-failure behavior
itself (rather than a working small build), try something genuinely
below baseline, e.g. `make MS_HEAP_SIZE=400`.

## Built-in functions and special forms

**Special forms** (recognized by the evaluator directly; cannot be
shadowed by user definitions): `quote`, `` ` `` (`quasiquote`, with `,`
unquote and `,@` unquote-splicing), `if`, `cond` (with `else` and `=>`),
`case` (with `else`), `begin`, `and`, `or` (both short-circuiting,
variadic), `while`, `lambda`, `let`, named `let`, `let*`, `letrec`,
`do`, `set!`, `define` (value or `(define (name args...) body)`
shorthand), `defun`, `delay`.

**Built-in procedures:**
- Pairs/lists: `car`, `cdr`, `cons`, `set-car!`, `set-cdr!`, `list`,
  `pair?`, `null?`, `atom?`, `length`, `memq`, `memv`, `assq`, `assv`.
- Equality: `eq?`, `eqv?`, `equal?`, `not`.
- Numbers: `< <= > >= =`, `+ - * / %` (`mod`), `quotient`, `remainder`,
  `modulo`, `gcd`, `lcm`, `expt`, `abs`, `floor`, `ceiling`, `truncate`,
  `round`, `sqrt`, `min`, `max`, `zero?`, `positive?`, `negative?`,
  `odd?`, `even?`, `number?`, `exact?`, `inexact?`, `exact->inexact`,
  `inexact->exact`.
- Characters: `char?`, `char->integer`, `integer->char`, `char=?`
  `char<?` `char>?` `char<=?` `char>=?`, `char-upcase`, `char-downcase`,
  `char-alphabetic?`, `char-numeric?`, `char-whitespace?`,
  `char-upper-case?`, `char-lower-case?`.
- Strings: `string?`, `make-string`, `string`, `string-length`,
  `string-ref`, `string-set!`, `substring`, `string-copy`,
  `string-fill!`, `string=?` `string<?` `string>?` `string<=?`
  `string>=?`, `string->symbol`, `symbol->string`, `string->number`,
  `number->string` (both with an optional radix argument),
  `str->list`, `list->str`, `str-append`/`string-append`.
- Vectors: `vector?`, `make-vector`, `vector`, `vector-length`,
  `vector-ref`, `vector-set!`, `vector-fill!`, `vector->list`,
  `list->vector`.
- Symbols/booleans: `symbol?`, `boolean?`.
- Control: `procedure?`, `apply` (full variadic form), `eval`, `force`,
  `call/cc`/`call-with-current-continuation`, `dynamic-wind`.
- I/O and misc: `read`, `eof-object?`, `print`, `display`, `write`,
  `gc`, `exit`, and (desktop builds only) `file->str`, `load`.

## Standard library

`ms_stdlib.l` builds on the above with things that are naturally Lisp
rather than C: the full `caar`/`cadr`/... family (through 4 levels, per
R4RS), `nil?`, `list?`, `first`/`rest`/`second`/`third`, `append`
(variadic), `reverse`, `list-tail`, `list-ref`, `last`/`last-pair`,
`map`/`for-each` (both accept any number of lists, per R4RS), `reduce`,
`filter`/`remove`, `member`/`member?`/`assoc` (R4RS: return `#f`, not
`'()`, when nothing matches), `count`, `flatten`, `range`, `sort`,
`square`, `identity`, `1+`/`1-`.

It's ordinary Lisp -- read it, and trim what you don't need for very
small deployments.

## Testing

```
make test
```

runs, in order:

1. **In-language tests** (`test_core.l`, `test_lists.l`,
   `test_closures.l`, `test_r4rs.l`, `test_strings.l`) -- a small
   `check`/`report` harness (`harness.l`) exercising arithmetic,
   equality, every special form, closures, tail recursion correctness,
   R4RS truthiness, booleans/characters/vectors, `letrec`/named-`let`/
   `do`/`case`, `delay`/`force`, `call/cc`/`dynamic-wind` (including the
   escape-through-dynamic-wind interaction), list/string/vector library
   functions, and quasiquote. 252 checks in total.
2. **GC-under-pressure**: a tail-recursive loop of 5,000,000 iterations
   against a 1000-cell heap, which only succeeds if garbage (stale
   per-iteration environments and numbers) is actually being reclaimed.
3. **Stack safety**: 10,000,000 tail-recursive iterations against the
   default heap, which would overflow the C stack immediately if tail
   calls were not properly optimized.
4. **Clean-failure checks**: heap exhaustion and protect-stack overflow
   both produce a reported panic and exit code 1 -- never a crash --
   confirmed by deliberately configuring undersized heaps/stacks and
   checking the exit code. Includes a dedicated boot-safety regression
   test (see [Memory model](#memory-model--garbage-collection)).
5. **LIX build check**: compiles `ms.c` with `-DLIX` against stub
   platform headers and greps the build log to confirm no file-I/O
   symbols made it in.
6. **Optional readline build check**: if readline's headers are
   installed, confirms `-DMS_USE_READLINE` compiles and links; skipped
   (not failed) if they aren't, since readline is opt-in.
7. **Single-line GC + tail-call check**: the same one-liner from
   [A single-line test for GC + tail calls](#a-single-line-test-for-gc--tail-calls),
   run at a slightly larger scale.

Run an individual suite directly, e.g.:

```
./ms test_core.l
```

or under sanitizers:

```
make asan && ./ms test_core.l
```

## Known limitations

See [R4RS: what's supported, what's not, and why](#r4rs-whats-supported-whats-not-and-why)
for the language-level deviations (macros, continuations, numeric tower,
ports). Beyond those:

- **Non-tail recursion is bounded** by both the C stack and
  `MS_PROTECT_STACK_SIZE`; deep non-tail-recursive algorithms should be
  rewritten in a tail-recursive (usually accumulator-passing) style, or
  compiled with a larger protect stack.
- **No error/condition system** -- built-in type errors call a panic
  handler that prints a message and exits, matching a small-MCU
  reality where unwinding is often not worth the complexity. This does
  mean a single malformed top-level form can end the process; the REPL
  itself does not attempt to catch and recover from this. (`call/cc`
  gives you a way to build your own escape-based error handling in
  ordinary Scheme code, within its escape-only limits.)
- **`set!`/`define` fall back to defining in the global environment**
  if the target symbol isn't found in any enclosing scope, rather than
  raising an "unbound variable" error. This is a deliberate simplicity
  trade-off, matching the original implementation's permissiveness.
- Vector/string index errors, division by zero, and similar are hard
  panics (process exit), not recoverable conditions -- same reasoning as
  the error/condition point above.

## Relationship to the original implementation

This tree started from an existing, explicitly "unfinished, broken toy
lisp" called Lone Lisp (see the header comment retained at the top of
`ms.c`, which credits the original copyright holder). That version's
garbage collector never traced a closure's captured environment (the
marking code for it was commented out), environments themselves were
`malloc`'d C structs that were never freed (an unconditional per-call
leak), there was no tail-call optimization (its `l_apply` recursed
through the C stack for every call), and a handful of concrete bugs
meant common programs produced wrong answers (e.g. zero-argument
functions evaluated their parameter list as if it were code, and the
standard library referenced functions like `caar`/`cadr`/`eqv` that were
never defined). It also conflated `'()` with false, which this revision
has since corrected to match R4RS.

The rewrite replaced the evaluator and memory manager with the
environment-as-cons-cells design and explicit tail-call trampoline
described above, added a large fraction of R4RS's data types and special
forms (booleans, characters, vectors, promises, escape continuations,
`letrec`, named `let`, `do`, `case`, and much of the standard procedure
library), and ships a test suite that specifically exercises the
properties that were previously broken or newly added (closures over
mutable state, tail-recursive loops of millions of iterations under a
tiny heap, R4RS truthiness, `dynamic-wind` interacting correctly with an
escaping continuation, and clean behavior at the edge of available
memory).

## License

The contents of this repo are released under the [Lone Dynamics Open License](LICENSE.md).
