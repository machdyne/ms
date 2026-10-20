; define-macro: a closure called with its arguments as written, whose
; result is evaluated in the caller's place.

(load "harness.l")

(define-macro (my-when test . body)
  `(if ,test (begin ,@body) #f))

(check "a macro's body runs when the test holds" (my-when #t 1 2 3) 3)
(check "and not otherwise" (my-when #f (car '())) #f)

; its arguments are not evaluated before the call
(define hits 0)
(define-macro (ignore x) ''ignored)
(check "an argument is not evaluated" (begin (ignore (set! hits (+ hits 1))) hits) 0)

; a macro that wraps its body in a lambda: what slide's (after MS ...) does
(define saved #f)
(define (keep thunk) (set! saved thunk) 'kept)
(define-macro (later . body) `(keep (lambda () ,@body)))
(check "a body kept for later" (later (+ 40 2)) 'kept)
(check "and run later" (saved) 42)

; recursion through the expansion
(define-macro (my-or . xs)
  (if (null? xs) #f
      `(let ((t ,(car xs))) (if t t (my-or ,@(cdr xs))))))
(check "a recursive macro" (my-or #f #f 7) 7)
(check "with nothing" (my-or) #f)

; in tail position: a loop through a macro does not grow the stack
(define-macro (my-unless test . body) `(if ,test #f (begin ,@body)))
(define (count-down n) (my-unless (= n 0) (count-down (- n 1))))
(check "tail calls through a macro" (begin (count-down 100000) 'done) 'done)

(check "macroexpand-1" (macroexpand-1 '(my-when a b)) '(if a (begin b) #f))
(check "macroexpand-1 of a plain call" (macroexpand-1 '(car x)) '(car x))
(check "a macro is a value" (procedure? my-when) #f)

(report)
