; Minimal test harness for Machdyne Scheme.
;
; Usage in a test file (run from this directory, see run_tests.sh):
;   (load "harness.l")
;   (check "1 + 1" (+ 1 1) 2)
;   (check "car of list" (car (list 1 2)) 1)
;   ...
;   (report)          ; prints a summary; exits 1 if anything failed
;
; `check` compares with `equal?`, so lists/strings/numbers/vectors/chars
; all work.

(load "ms_stdlib.l")

(define *checks-run* 0)
(define *checks-failed* 0)

(define (check name actual expected)
  (begin
    (set! *checks-run* (+ *checks-run* 1))
    (if (equal? actual expected)
        (begin
          (display "ok   - ") (display name) (print ""))
        (begin
          (set! *checks-failed* (+ *checks-failed* 1))
          (display "FAIL - ") (display name)
          (display "  expected ") (write expected)
          (display " got ") (write actual)
          (print "")))))

; like `check`, but only requires the value to be truthy, i.e. not #f
; (R4RS: '() and everything else except #f counts as true)
(define (check-true name actual)
  (begin
    (set! *checks-run* (+ *checks-run* 1))
    (if (not actual)
        (begin
          (set! *checks-failed* (+ *checks-failed* 1))
          (display "FAIL - ") (display name) (print "  expected truthy value, got #f"))
        (begin (display "ok   - ") (display name) (print "")))))

(define (report)
  (begin
    (print "")
    (display *checks-run*) (display " checks, ")
    (display *checks-failed*) (print " failed")
    (if (> *checks-failed* 0) (exit 1) (exit 0))))
