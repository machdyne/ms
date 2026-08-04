(load "harness.l")

; --- basic recursion ---
(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))
(check "factorial" (fact 10) 3628800)

(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))
(check "fibonacci" (fib 15) 610)

; --- tail-call optimization: this would stack-overflow a non-TCO
; interpreter. If this check completes at all, tail calls are working. ---
(define (count-to n acc) (if (= n 0) acc (count-to (- n 1) (+ acc 1))))
(check "deep tail recursion (500k)" (count-to 500000 0) 500000)

(define (sum-to n acc) (if (= n 0) acc (sum-to (- n 1) (+ acc n))))
(check "tail-recursive sum 1..1000" (sum-to 1000 0) 500500)

; mutual tail recursion across two closures
(define (is-even? n) (if (= n 0) #t (is-odd? (- n 1))))
(define (is-odd? n) (if (= n 0) #f (is-even? (- n 1))))
(check "mutual tail recursion" (is-even? 100000) #t)

; --- closures capture their defining environment ---
(define (make-adder n) (lambda (x) (+ x n)))
(define add5 (make-adder 5))
(define add10 (make-adder 10))
(check "closure captures n=5" (add5 1) 6)
(check "closure captures n=10" (add10 1) 11)
(check "closures are independent" (list (add5 0) (add10 0)) (list 5 10))

; --- mutable state inside a closure (a counter) ---
(define (make-counter)
  (let ((n 0))
    (lambda () (begin (set! n (+ n 1)) n))))
(define c1 (make-counter))
(define c2 (make-counter))
(check "counter c1 first call" (c1) 1)
(check "counter c1 second call" (c1) 2)
(check "counter c2 independent" (c2) 1)
(check "counter c1 unaffected by c2" (c1) 3)

; --- lambdas as first-class values ---
(check "pass lambda to map" (map (lambda (x) (x 2)) (list add5 add10)) (list 7 12))

; --- let creates a fresh scope per call, not shared across recursive calls ---
(define (build-list n)
  (if (= n 0)
      '()
      (let ((item n)) (cons item (build-list (- n 1))))))
(check "let scope per recursive call" (build-list 5) (list 5 4 3 2 1))

; --- named-let loops are properly tail recursive too ---
(check "named let deep loop (500k)"
  (let loop ((n 500000) (acc 0)) (if (= n 0) acc (loop (- n 1) (+ acc 1))))
  500000)

(report)
