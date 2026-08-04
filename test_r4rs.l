(load "harness.l")

; --- booleans as a real disjoint type ---
(check "boolean? on #t" (boolean? #t) #t)
(check "boolean? on #f" (boolean? #f) #t)
(check "boolean? on '()" (boolean? '()) #f)
(check "boolean? on 0" (boolean? 0) #f)
(check "#t is not eq? to '()" (eq? #t '()) #f)
(check "#f is not eq? to '()" (eq? #f '()) #f)
(check "'() is not eq? to the symbol nil-marker" (eq? '() (quote ())) #t)

; --- characters ---
(check "char? true" (char? #\a) #t)
(check "char? false" (char? "a") #f)
(check "char literal" #\a #\a)
(check "char space literal" (char->integer #\space) 32)
(check "char newline literal" (char->integer #\newline) 10)
(check "char->integer" (char->integer #\A) 65)
(check "integer->char" (integer->char 65) #\A)
(check "char=?" (char=? #\a #\a) #t)
(check "char<?" (char<? #\a #\b) #t)
(check "char-upcase" (char-upcase #\a) #\A)
(check "char-downcase" (char-downcase #\A) #\a)
(check "char-alphabetic?" (char-alphabetic? #\a) #t)
(check "char-alphabetic? false" (char-alphabetic? #\1) #f)
(check "char-numeric?" (char-numeric? #\5) #t)
(check "char-whitespace?" (char-whitespace? #\space) #t)
(check "char-upper-case?" (char-upper-case? #\A) #t)
(check "char-lower-case?" (char-lower-case? #\a) #t)
(check "write shows char literal" (equal? (num->str 1) "1") #t) ; sanity

; --- vectors ---
(check "vector? true" (vector? (vector 1 2 3)) #t)
(check "vector? false" (vector? (list 1 2 3)) #f)
(check "vector literal reads" #(1 2 3) (vector 1 2 3))
(check "make-vector" (make-vector 3 0) (vector 0 0 0))
(check "make-vector default fill" (vector-length (make-vector 5)) 5)
(check "vector-length" (vector-length (vector 1 2 3 4)) 4)
(check "vector-ref" (vector-ref (vector 10 20 30) 1) 20)
(check "vector-set!" (let ((v (vector 1 2 3))) (vector-set! v 1 99) v) (vector 1 99 3))
(check "vector-fill!" (let ((v (make-vector 3 0))) (vector-fill! v 7) v) (vector 7 7 7))
(check "vector->list" (vector->list (vector 1 2 3)) (list 1 2 3))
(check "list->vector" (list->vector (list 1 2 3)) (vector 1 2 3))
(check "vector equal? deep" (equal? (vector 1 (vector 2 3)) (vector 1 (vector 2 3))) #t)
(check "vector not equal? to a list" (equal? (vector 1 2 3) (list 1 2 3)) #f)

; --- eqv?/eq? on chars, numbers, symbols ---
(check "eq? on same char" (eq? #\a #\a) #t)
(check "eqv? on chars" (eqv? #\a #\a) #t)
(check "eq? on symbols" (eq? 'foo 'foo) #t)

; --- letrec placeholder / do loop edge cases ---
(check "do with no step (var stays the same)"
  (do ((x 5) (i 0 (+ i 1))) ((= i 3) x))
  5)

; --- dynamic-wind ---
(check "dynamic-wind normal (before/thunk/after all run in order)"
  (let ((log '()))
    (dynamic-wind
      (lambda () (set! log (cons 'before log)))
      (lambda () (set! log (cons 'thunk log)) 'result)
      (lambda () (set! log (cons 'after log))))
    (reverse log))
  (list 'before 'thunk 'after))

(check "dynamic-wind after runs even when thunk escapes via call/cc"
  (let ((log '()))
    (call/cc (lambda (k)
      (dynamic-wind
        (lambda () (set! log (cons 'before log)))
        (lambda () (set! log (cons 'thunk log)) (k 'escaped) (set! log (cons 'unreachable log)))
        (lambda () (set! log (cons 'after log))))))
    (reverse log))
  (list 'before 'thunk 'after))

(check "dynamic-wind returns thunk's value normally"
  (dynamic-wind (lambda () #f) (lambda () 42) (lambda () #f))
  42)

; --- quasiquote ---
(check "quasiquote with vector-producing expr"
  `(1 ,(vector 2 3) 4)
  (list 1 (vector 2 3) 4))
(check "nested list splice" `(a ,@(list 'b 'c) d) (list 'a 'b 'c 'd))

; --- reader edge cases ---
(check "radix hex literal" #x1A 26)
(check "radix octal literal" #o17 15)
(check "radix binary literal" #b101 5)
(check "dotted pair read" (cons 1 2) '(1 . 2))
(check "vector of symbols" #(a b c) (vector 'a 'b 'c))

(report)
