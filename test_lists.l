(load "harness.l")

; --- core list primitives ---
(check "cons" (cons 1 2) (cons 1 2))
(check "car" (car (list 1 2 3)) 1)
(check "cdr" (cdr (list 1 2 3)) (list 2 3))
(check "pair? true" (pair? (list 1)) #t)
(check "pair? false" (pair? '()) #f)
(check "pair? on number" (pair? 5) #f)
(check "null? true" (null? '()) #t)
(check "null? false" (null? (list 1)) #f)
(check "atom? on symbol" (atom? 'x) #t)
(check "atom? on pair" (atom? (list 1)) #f)
(check "length" (length (list 1 2 3 4)) 4)
(check "length empty" (length '()) 0)

; --- mutation ---
(check "set-car!" (let ((p (cons 1 2))) (set-car! p 99) p) (cons 99 2))
(check "set-cdr!" (let ((p (cons 1 2))) (set-cdr! p 99) p) (cons 1 99))
(check "set-cdr! builds a list" (let ((p (list 1 2))) (set-cdr! (cdr p) (list 3)) p) (list 1 2 3))

; --- caar/cadr family ---
(check "caar" (caar (list (list 1 2) 3)) 1)
(check "cadr" (cadr (list 1 2 3)) 2)
(check "caddr" (caddr (list 1 2 3)) 3)
(check "cdar" (cdar (list (list 1 2) 3)) (list 2))
(check "cadddr" (cadddr (list 1 2 3 4)) 4)

; --- append / reverse ---
(check "append two" (append (list 1 2) (list 3 4)) (list 1 2 3 4))
(check "append many" (append (list 1) (list 2) (list 3)) (list 1 2 3))
(check "append with empty" (append '() (list 1 2)) (list 1 2))
(check "reverse" (reverse (list 1 2 3)) (list 3 2 1))
(check "reverse empty" (reverse '()) '())

; --- higher-order functions ---
(check "map" (map (lambda (x) (* x 2)) (list 1 2 3)) (list 2 4 6))
(check "map two lists" (map + (list 1 2 3) (list 10 20 30)) (list 11 22 33))
(check "map stops at shorter list" (map + (list 1 2 3) (list 10 20)) (list 11 22))
(check "filter" (filter odd? (list 1 2 3 4 5)) (list 1 3 5))
(check "remove" (remove odd? (list 1 2 3 4 5)) (list 2 4))
(check "reduce" (reduce + 0 (list 1 2 3 4)) 10)
(check "reduce mul" (reduce * 1 (list 1 2 3 4)) 24)
(check "for-each accumulate"
  (let ((acc '()))
    (begin (for-each (lambda (x) (set! acc (cons x acc))) (list 1 2 3)) acc))
  (list 3 2 1))
(check "for-each two lists"
  (let ((acc '()))
    (begin (for-each (lambda (x y) (set! acc (cons (+ x y) acc))) (list 1 2) (list 10 20)) acc))
  (list 22 11))

; --- searching (R4RS: #f, not '(), when nothing matches) ---
(check "member? found" (member? 3 (list 1 2 3 4)) #t)
(check "member? not found" (member? 9 (list 1 2 3 4)) #f)
(check "member found returns tail" (member 3 (list 1 2 3 4)) (list 3 4))
(check "member not found" (member 9 (list 1 2 3)) #f)
(check "assoc found" (assoc 'b (list (list 'a 1) (list 'b 2))) (list 'b 2))
(check "assoc not found" (assoc 'z (list (list 'a 1))) #f)
(check "memq found" (memq 'c (list 'a 'b 'c 'd)) (list 'c 'd))
(check "memq not found" (memq 'z (list 'a 'b)) #f)
(check "assq found" (assq 'b (list (list 'a 1) (list 'b 2))) (list 'b 2))
(check "count" (count 2 (list 1 2 2 3 2)) 3)

; --- misc list utilities ---
(check "list-ref" (list-ref (list 10 20 30) 1) 20)
(check "list-tail" (list-tail (list 1 2 3 4) 2) (list 3 4))
(check "last" (last (list 1 2 3)) 3)
(check "flatten" (flatten (list 1 (list 2 3) (list (list 4) 5))) (list 1 2 3 4 5))
(check "range" (range 0 5) (list 0 1 2 3 4))
(check "sort" (sort (list 3 1 4 1 5 9 2 6)) (list 1 1 2 3 4 5 6 9))
(check "list?" (list? (list 1 2)) #t)
(check "list? on empty" (list? '()) #t)
(check "list? on dotted pair" (list? (cons 1 2)) #t) ; our list? is a lenient pair?-or-nil? check

(report)
