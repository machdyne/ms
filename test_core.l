(load "harness.l")

; --- arithmetic ---
(check "add" (+ 1 2 3) 6)
(check "add no args" (+) 0)
(check "sub binary" (- 10 3) 7)
(check "sub unary negation" (- 5) -5)
(check "mul" (* 2 3 4) 24)
(check "mul no args" (*) 1)
(check "div" (/ 10 2) 5)
(check "div unary reciprocal" (/ 4) 0.25)
(check "mod" (% 10 3) 1)
(check "float add" (+ 1.5 2.5) 4)
(check "quotient" (quotient 7 2) 3)
(check "remainder" (remainder 7 2) 1)
(check "remainder negative" (remainder -7 2) -1)
(check "modulo negative" (modulo -7 2) 1)
(check "gcd" (gcd 12 18) 6)
(check "lcm" (lcm 4 6) 12)
(check "expt" (expt 2 10) 1024)
(check "sqrt" (sqrt 16) 4)
(check "abs" (abs -5) 5)
(check "floor" (floor 3.7) 3)
(check "ceiling" (ceiling 3.2) 4)
(check "truncate neg" (truncate -3.7) -3)
(check "round down" (round 3.4) 3)
(check "round up" (round 3.6) 4)
(check "min" (min 3 1 4 1 5) 1)
(check "max" (max 3 1 4 1 5) 5)
(check "zero? true" (zero? 0) #t)
(check "zero? false" (zero? 1) #f)
(check "positive?" (positive? 5) #t)
(check "negative?" (negative? -5) #t)
(check "odd?" (odd? 3) #t)
(check "even?" (even? 4) #t)

; --- comparisons ---
(check "lt true" (< 1 2) #t)
(check "lt false" (< 2 1) #f)
(check "lte eq" (<= 2 2) #t)
(check "gt" (> 3 2) #t)
(check "gte" (>= 3 3) #t)
(check "numeq" (= 3 3) #t)
(check "numeq false" (= 3 4) #f)

; --- equality ---
(check "eq? symbols" (eq? 'a 'a) #t)
(check "eq? numbers" (eq? 3 3) #t)
(check "equal? lists" (equal? (list 1 2 (list 3 4)) (list 1 2 (list 3 4))) #t)
(check "equal? mismatched lists" (equal? (list 1 2) (list 1 2 3)) #f)
(check "equal? strings" (equal? "abc" "abc") #t)
(check "not on #f" (not #f) #t)
(check "not on '()" (not '()) #f)     ; R4RS: '() is TRUE, only #f is false
(check "not truthy" (not 5) #f)

; --- R4RS truthiness: only #f is false ---
(check "empty list is true in if" (if '() "yes" "no") "yes")
(check "zero is true in if" (if 0 "yes" "no") "yes")
(check "empty string is true in if" (if "" "yes" "no") "yes")
(check "#f is false in if" (if #f "yes" "no") "no")

; --- quote / quasiquote ---
(check "quote" (quote (1 2 3)) (list 1 2 3))
(check "quote shorthand" '(a b c) (list 'a 'b 'c))
(check "quasiquote plain" `(1 2 3) (list 1 2 3))
(check "quasiquote unquote" `(1 ,(+ 1 1) 3) (list 1 2 3))
(check "quasiquote splice" `(1 ,@(list 2 3) 4) (list 1 2 3 4))

; --- if / cond / case / and / or ---
(check "if true" (if #t 1 2) 1)
(check "if false" (if #f 1 2) 2)
(check "if no else unspecified is not an error" (if #f 1) #f)
(check "cond first match" (cond (#t 1) (#t 2)) 1)
(check "cond fallthrough" (cond (#f 1) (#t 2)) 2)
(check "cond else" (cond (#f 1) (else 99)) 99)
(check "cond arrow" (cond ((assoc 'b (list (list 'a 1) (list 'b 2))) => cadr) (else #f)) 2)
(check "case match" (case 2 ((1) "one") ((2) "two") (else "other")) "two")
(check "case fallthrough to else" (case 99 ((1) "one") (else "other")) "other")
(check "case multiple datums per clause" (case 5 ((1 2 3) "low") ((4 5 6) "mid")) "mid")
(check "and both true" (and 1 2) 2)
(check "and short circuit" (and #f (/ 1 0)) #f)
(check "and empty" (and) #t)
(check "or first true" (or 1 (/ 1 0)) 1)
(check "or fallthrough" (or #f 5) 5)
(check "or empty" (or) #f)

; --- let / let* / letrec / named let / do / set! ---
(check "let" (let ((x 1) (y 2)) (+ x y)) 3)
(check "let scoping" (let ((x 5)) (let ((x 10)) x)) 10)
(check "let* sequential" (let* ((x 1) (y (+ x 1))) y) 2)
(check "letrec mutual recursion"
  (letrec ((ev? (lambda (n) (if (= n 0) #t (od? (- n 1)))))
           (od? (lambda (n) (if (= n 0) #f (ev? (- n 1))))))
    (ev? 10))
  #t)
(check "named let tail loop"
  (let loop ((i 0) (acc 0)) (if (= i 100000) acc (loop (+ i 1) (+ acc 1))))
  100000)
(check "do loop sum" (do ((i 0 (+ i 1)) (acc 0 (+ acc i))) ((= i 5) acc)) 10)
(check "do loop no result body" (let ((n 0)) (do ((i 0 (+ i 1))) ((= i 3)) (set! n (+ n 1))) n) 3)
(check "set!" (let ((x 1)) (begin (set! x 99) x)) 99)

; --- begin / while ---
(check "begin returns last" (begin 1 2 3) 3)
(check "while side effect"
  (let ((i 0) (acc 0))
    (begin (while (< i 5) (begin (set! acc (+ acc i)) (set! i (+ i 1)))) acc))
  10)

; --- lambda / define / defun ---
(check "lambda call" ((lambda (x y) (+ x y)) 3 4) 7)
(check "define value" (begin (define answer 42) answer) 42)
(check "define function shorthand" (begin (define (sq x) (* x x)) (sq 5)) 25)
(check "variadic params" (begin (define (vf . xs) xs) (vf 1 2 3)) (list 1 2 3))
(check "rest params" (begin (define (rf a b . rest) rest) (rf 1 2 3 4)) (list 3 4))

; --- apply / eval ---
(check "apply" (apply + (list 1 2 3 4)) 10)
(check "apply with leading args" (apply + 1 2 (list 3 4)) 10)
(check "eval" (eval (list '+ 1 2)) 3)

; --- delay / force ---
(check "delay/force" (force (delay (+ 1 2))) 3)
(check "force memoizes"
  (let ((n 0))
    (let ((p (delay (begin (set! n (+ n 1)) n))))
      (force p) (force p) (force p)))
  1)
(check "force on non-promise passes through" (force 5) 5)

; --- call/cc (escape-only, see README) ---
(check "call/cc basic escape" (+ 1 (call/cc (lambda (k) (k 41) 999))) 42)
(check "call/cc no escape" (+ 1 (call/cc (lambda (k) 41))) 42)
(check "call/cc for early return in a loop"
  (call/cc (lambda (return)
    (for-each (lambda (x) (if (= x 3) (return x) #f)) (list 1 2 3 4 5))
    'not-found))
  3)

(report)
