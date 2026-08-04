#!/usr/bin/env bash
# Test suite for Machdyne Scheme.
#
# Runs:
#   1. The in-language test files (test_*.l) via the harness in harness.l
#   2. Shell-level tests of properties that can't be checked from inside a
#      running interpreter: garbage collection under memory pressure,
#      tail-call stack safety, and clean (non-crashing) out-of-memory /
#      overflow behavior.
#
# Usage: ./run_tests.sh   (from anywhere; paths are resolved relative to
# this script's location)

set -u
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

MS="./ms"

PASS=0
FAIL=0

note() { printf '\n=== %s ===\n' "$1"; }

build() {
	local name="$1"; shift
	note "build: $name"
	if ! (gcc -Wall -Wextra -DMS_HOST "$@" -o ms ms.c -lm); then
		echo "BUILD FAILED: $name"
		exit 1
	fi
}

run_lisp_test() {
	local file="$1"
	note "lisp test: $file"
	if "$MS" "$file"; then
		echo "PASS: $file"
		PASS=$((PASS + 1))
	else
		echo "FAIL: $file (nonzero exit)"
		FAIL=$((FAIL + 1))
	fi
}

expect_success() {
	local desc="$1"; local timeout_s="$2"; shift 2
	note "$desc"
	if timeout "$timeout_s" "$@" ; then
		echo "PASS: $desc"
		PASS=$((PASS + 1))
	else
		echo "FAIL: $desc (expected success, got failure or timeout)"
		FAIL=$((FAIL + 1))
	fi
}

expect_failure() {
	local desc="$1"; local timeout_s="$2"; shift 2
	note "$desc"
	timeout "$timeout_s" "$@"
	rc=$?
	# rc==1 is our clean ms_log(MS_PANIC,...) exit. Anything indicating a
	# crash (segfault=139, abort=134) or a timeout (124) is a real bug.
	if [ "$rc" -eq 1 ]; then
		echo "PASS: $desc (clean failure, exit=$rc)"
		PASS=$((PASS + 1))
	else
		echo "FAIL: $desc (expected a clean exit=1, got exit=$rc)"
		FAIL=$((FAIL + 1))
	fi
}

# ---------------------------------------------------------------------
# 1. default build + in-language test suites
# ---------------------------------------------------------------------

build "default (desktop) build"

for f in test_core.l test_lists.l test_closures.l test_r4rs.l test_strings.l; do
	run_lisp_test "$f"
done

# ---------------------------------------------------------------------
# 2. GC correctness under memory pressure: a tail-recursive loop doing
#    5,000,000 iterations must succeed even with a tiny heap, proving
#    that garbage (old environments/numbers) is actually being reclaimed
#    and not merely deferred.
# ---------------------------------------------------------------------

build "tiny heap (1000 cells) for GC pressure test" -DMS_HEAP_SIZE=1000

cat > /tmp/ms_gc_pressure.l <<'EOF'
(define (loop n acc) (if (= n 0) acc (loop (- n 1) (+ acc 1))))
(if (= (loop 5000000 0) 5000000) (exit 0) (exit 1))
EOF
expect_success "GC reclaims garbage across 5M tail-recursive iterations (1000-cell heap)" 60 "$MS" /tmp/ms_gc_pressure.l

# ---------------------------------------------------------------------
# 3. Tail calls really are O(1) C-stack: this would blow the C stack
#    (and crash) in a naive recursive-eval implementation.
# ---------------------------------------------------------------------

build "default build for stack-safety test"

cat > /tmp/ms_stack_safety.l <<'EOF'
(define (loop n acc) (if (= n 0) acc (loop (- n 1) (+ acc 1))))
(if (= (loop 10000000 0) 10000000) (exit 0) (exit 1))
EOF
expect_success "10M tail-recursive iterations do not overflow the C stack" 90 "$MS" /tmp/ms_stack_safety.l

# ---------------------------------------------------------------------
# 4. Out-of-memory and protect-stack overflow are clean, reported
#    failures (exit 1), never a segfault/abort -- important on an MCU
#    where a crash may be unrecoverable but a clean panic can be.
# ---------------------------------------------------------------------

build "tiny heap (48 cells, below baseline) for OOM test" -DMS_HEAP_SIZE=48
cat > /tmp/ms_oom.l <<'EOF'
(define (build n) (if (= n 0) '() (cons n (build (- n 1)))))
(build 10000)
EOF
expect_failure "heap exhaustion fails cleanly, not a crash" 15 "$MS" /tmp/ms_oom.l

# Regression test for a real bug found during R4RS development: the
# global environment being built by ms_env_std() wasn't GC-rooted until
# fully constructed and assigned to ms_global_env, so a GC triggered
# mid-boot (e.g. by an undersized heap) silently corrupted it instead of
# failing cleanly. Confirms boot either succeeds outright or fails with
# a clean panic -- never with corrupted state (spurious "unbound symbol"
# errors for things that were never referenced).
build "heap just below boot baseline -- must fail cleanly, not corrupt" -DMS_HEAP_SIZE=600
expect_failure "undersized-for-boot heap fails cleanly during startup" 10 \
	bash -c "echo '(+ 1 2)' | '$MS'"

build "heap comfortably above boot baseline" -DMS_HEAP_SIZE=1000
expect_success "comfortably-sized heap boots and evaluates correctly" 10 \
	bash -c "test \"\$(echo '(+ 1 2)' | '$MS' | tr -d '[:space:]')\" = 'ms>3ms>'"

build "tiny protect stack (64 slots) for overflow test" -DMS_PROTECT_STACK_SIZE=64
cat > /tmp/ms_protect_overflow.l <<'EOF'
(define (sumto n) (if (= n 0) 0 (+ n (sumto (- n 1)))))
(sumto 100000)
EOF
expect_failure "deep non-tail recursion overflows protect stack cleanly, not a crash" 15 "$MS" /tmp/ms_protect_overflow.l

# ---------------------------------------------------------------------
# 5. LIX (embedded, no file I/O) build actually compiles.
#    We can't run the target firmware here, but we can prove the
#    LIX-only code path (no fopen/fs calls) compiles standalone by
#    stubbing the platform headers it expects.
# ---------------------------------------------------------------------

note "LIX (embedded) build compiles without fopen/file I/O"
TMPLIX=$(mktemp -d)
mkdir -p "$TMPLIX/include"
cat > "$TMPLIX/include/fs.h" <<'EOF'
/* stub for standalone LIX compile-check */
EOF
cat > "$TMPLIX/zucker.h" <<'EOF'
/* stub for standalone LIX compile-check */
static volatile unsigned int reg_rtc = 0;
EOF
cat > "$TMPLIX/ms_stdlib.h" <<'EOF'
static const char ms_stdlib_l[] = "(define (identity x) x)";
EOF
mkdir -p "$TMPLIX/common"
mv "$TMPLIX/zucker.h" "$TMPLIX/common/zucker.h"
if gcc -Wall -DLIX -DMS_HEAP_SIZE=256 \
	-I"$TMPLIX" -I"$TMPLIX/include" \
	-c "ms.c" -o /tmp/ms_lix.o 2>/tmp/lix_build.log; then
	echo "PASS: LIX build compiles cleanly (no fopen/file I/O required)"
	PASS=$((PASS + 1))
	if grep -qE 'fopen|fclose|fread|fseek' /tmp/lix_build.log; then
		echo "FAIL: LIX build referenced file I/O"
		FAIL=$((FAIL + 1))
	fi
else
	echo "FAIL: LIX build did not compile"
	cat /tmp/lix_build.log
	FAIL=$((FAIL + 1))
fi
rm -rf "$TMPLIX" /tmp/ms_lix.o /tmp/lix_build.log

# ---------------------------------------------------------------------
# 6. Optional readline build: only checked if readline's headers are
#    actually installed, since -DMS_USE_READLINE/USE_READLINE=1 is
#    opt-in and the default build must not require it.
# ---------------------------------------------------------------------

if echo '#include <readline/readline.h>' | gcc -E - >/dev/null 2>&1; then
	note "optional readline build (make USE_READLINE=1) compiles and links"
	if (gcc -Wall -Wextra -DMS_HOST -DMS_USE_READLINE -o ms ms.c -lm -lreadline); then
		echo "PASS: readline build compiles and links"
		PASS=$((PASS + 1))
	else
		echo "FAIL: readline build did not compile/link"
		FAIL=$((FAIL + 1))
	fi
else
	note "readline headers not installed -- skipping optional readline build check"
fi

# ---------------------------------------------------------------------
# 7. Single-line GC + tail-call regression check (see also README's
#    "single-line test" example). Kept here too so `make test` always
#    covers it even if the standalone one-liner in the README goes stale.
# ---------------------------------------------------------------------

build "1500-cell heap for single-line GC+tail-call check" -DMS_HEAP_SIZE=1500
expect_success "single-line: 3M tail-recursive iterations under a 1500-cell heap" 30 \
	bash -c "echo '(define (loop n acc) (if (= n 0) acc (loop (- n 1) (+ acc 1)))) (if (= (loop 3000000 0) 3000000) (exit 0) (exit 1))' | '$MS'"

# ---------------------------------------------------------------------
# summary
# ---------------------------------------------------------------------

# restore the default desktop build so it's what's left in this directory
build "restore default build" >/dev/null

note "SUMMARY"
echo "passed: $PASS"
echo "failed: $FAIL"

if [ "$FAIL" -ne 0 ]; then
	exit 1
fi
exit 0
