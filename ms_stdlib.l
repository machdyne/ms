; Machdyne Scheme Standard Library
;
; Notes on this revision:
;   - `and`, `or`, `if`, `cond`, `case`, `let`, `let*`, `letrec`, `do`,
;     `define`, `defun`, `lambda`, `begin`, `set!`, `while`, `quote`,
;     `quasiquote`, `delay` are core special forms built into the
;     interpreter (see ms.c) and are NOT defined here.
;   - Only #f is false; '() (the empty list) is a TRUE value, matching
;     R4RS/R5RS and unlike some earlier revisions of this Lisp.
;   - A large set of R4RS procedures (numeric predicates/ops, char and
;     string procedures, vector procedures, eq?/eqv?/equal?, memq/assq
;     family, apply, force, call/cc, dynamic-wind, ...) are built into
;     the interpreter directly -- see README.md for the full list. Only
;     what's naturally expressed in Lisp itself lives here.
;   - Everything below is ordinary user-level Lisp, so it is easy to trim
;     for very small deployments: just drop the definitions you don't need.

; --- caar/cadr family (through 4 levels, per R4RS) ----------------------

(define (caar x) (car (car x)))
(define (cadr x) (car (cdr x)))
(define (cdar x) (cdr (car x)))
(define (cddr x) (cdr (cdr x)))
(define (caaar x) (car (caar x)))
(define (caadr x) (car (cadr x)))
(define (cadar x) (car (cdar x)))
(define (caddr x) (car (cddr x)))
(define (cdaar x) (cdr (caar x)))
(define (cdadr x) (cdr (cadr x)))
(define (cddar x) (cdr (cdar x)))
(define (cdddr x) (cdr (cddr x)))
(define (caaaar x) (car (caaar x)))
(define (caaadr x) (car (caadr x)))
(define (caadar x) (car (cadar x)))
(define (caaddr x) (car (caddr x)))
(define (cadaar x) (car (cdaar x)))
(define (cadadr x) (car (cdadr x)))
(define (caddar x) (car (cddar x)))
(define (cadddr x) (car (cdddr x)))
(define (cdaaar x) (cdr (caaar x)))
(define (cdaadr x) (cdr (caadr x)))
(define (cdadar x) (cdr (cadar x)))
(define (cdaddr x) (cdr (caddr x)))
(define (cddaar x) (cdr (cdaar x)))
(define (cddadr x) (cdr (cdadr x)))
(define (cdddar x) (cdr (cddar x)))
(define (cddddr x) (cdr (cdddr x)))

; --- basic predicates ----------------------------------------------------

(define (nil? x) (eq? x '()))
(define null nil?)
(define (list? x) (or (nil? x) (pair? x)))

(define first car)
(define rest cdr)
(define second cadr)
(define third caddr)

; --- list construction / traversal --------------------------------------

(define (append . lists)
  (if (nil? lists)
      '()
      (if (nil? (cdr lists))
          (car lists)
          (append2 (car lists) (apply append (cdr lists))))))

(define (append2 x y)
  (if (nil? x) y (cons (car x) (append2 (cdr x) y))))

(define (reverse l)
  (define (rev l acc)
    (if (nil? l) acc (rev (cdr l) (cons (car l) acc))))
  (rev l '()))

(define (list-tail l k)
  (if (= k 0) l (list-tail (cdr l) (- k 1))))

(define (list-ref l i) (car (list-tail l i)))

(define (last-pair l) (if (nil? (cdr l)) l (last-pair (cdr l))))
(define (last l) (car (last-pair l)))

; single-list map/for-each (used to build the variadic versions below,
; and useful directly when you only have one list anyway)
(define (map1 f l)
  (if (nil? l) '() (cons (f (car l)) (map1 f (cdr l)))))
(define (for-each1 f l)
  (if (nil? l) '() (begin (f (car l)) (for-each1 f (cdr l)))))

(define (any-nil? lists)
  (cond ((nil? lists) #f)
        ((nil? (car lists)) #t)
        (else (any-nil? (cdr lists)))))

; (map f list1 list2 ...) -- R4RS map accepts any number of lists,
; stepping through them in parallel and stopping at the shortest.
(define (map f . lists)
  (if (any-nil? lists)
      '()
      (cons (apply f (map1 car lists))
            (apply map f (map1 cdr lists)))))

(define (for-each f . lists)
  (if (any-nil? lists)
      #f
      (begin (apply f (map1 car lists))
             (apply for-each f (map1 cdr lists)))))

(define (reduce f a l)
  (if (nil? l) a (reduce f (f a (car l)) (cdr l))))

(define (filter pred l)
  (cond ((nil? l) '())
        ((pred (car l)) (cons (car l) (filter pred (cdr l))))
        (else (filter pred (cdr l)))))

(define (remove pred l)
  (filter (lambda (x) (not (pred x))) l))

; member?/count are extensions (not R4RS); member/assoc below are R4RS
; and, per spec, return #f (not '()) when nothing matches.
(define (member? x l)
  (cond ((nil? l) #f)
        ((equal? (car l) x) #t)
        (else (member? x (cdr l)))))

(define (member x l)
  (cond ((nil? l) #f)
        ((equal? (car l) x) l)
        (else (member x (cdr l)))))

(define (assoc x l)
  (cond ((nil? l) #f)
        ((equal? (caar l) x) (car l))
        (else (assoc x (cdr l)))))

(define (count x l)
  (cond ((nil? l) 0)
        ((equal? (car l) x) (+ 1 (count x (cdr l))))
        (else (count x (cdr l)))))

(define (flatten x)
  (cond ((nil? x) '())
        ((not (pair? x)) (list x))
        (else (append (flatten (car x)) (flatten (cdr x))))))

(define (range a b)
  (if (>= a b) '() (cons a (range (+ a 1) b))))

; --- sorting --------------------------------------------------------------

(define (sort l)
  (if (or (nil? l) (nil? (cdr l)))
      l
      (let ((pivot (car l)) (tail (cdr l)))
        (append
          (sort (filter (lambda (x) (<= x pivot)) tail))
          (list pivot)
          (sort (filter (lambda (x) (> x pivot)) tail))))))

; --- misc -------------------------------------------------------------

(define (square x) (* x x))
(define (identity x) x)
(define (1+ x) (+ x 1))
(define (1- x) (- x 1))
