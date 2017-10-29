;;; Continuation-passing style (CPS) intermediate language (IL)

;; Copyright (C) 2013, 2014, 2015, 2017 Free Software Foundation, Inc.

;;;; This library is free software; you can redistribute it and/or
;;;; modify it under the terms of the GNU Lesser General Public
;;;; License as published by the Free Software Foundation; either
;;;; version 3 of the License, or (at your option) any later version.
;;;;
;;;; This library is distributed in the hope that it will be useful,
;;;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;;;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
;;;; Lesser General Public License for more details.
;;;;
;;;; You should have received a copy of the GNU Lesser General Public
;;;; License along with this library; if not, write to the Free Software
;;;; Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

;;; Commentary:
;;;
;;; Compiling CPS to bytecode.  The result is in the bytecode language,
;;; which happens to be an ELF image as a bytecode.
;;;
;;; Code:

(define-module (language cps compile-bytecode)
  #:use-module (ice-9 match)
  #:use-module (srfi srfi-1)
  #:use-module (language cps)
  #:use-module (language cps primitives)
  #:use-module (language cps slot-allocation)
  #:use-module (language cps utils)
  #:use-module (language cps closure-conversion)
  #:use-module (language cps handle-interrupts)
  #:use-module (language cps optimize)
  #:use-module (language cps reify-primitives)
  #:use-module (language cps renumber)
  #:use-module (language cps split-rec)
  #:use-module (language cps intmap)
  #:use-module (language cps intset)
  #:use-module (system vm assembler)
  #:export (compile-bytecode))

(define (kw-arg-ref args kw default)
  (match (memq kw args)
    ((_ val . _) val)
    (_ default)))

(define (intmap-for-each f map)
  (intmap-fold (lambda (k v seed) (f k v) seed) map *unspecified*))

(define (intmap-select map set)
  (persistent-intmap
   (intset-fold
    (lambda (k out)
      (intmap-add! out k (intmap-ref map k)))
    set
    empty-intmap)))

;; Any $values expression that continues to a $kargs and causes no
;; shuffles is a forwarding label.
(define (compute-forwarding-labels cps allocation)
  (fixpoint
   (lambda (forwarding-map)
     (intmap-fold (lambda (label target forwarding-map)
                    (let ((new-target (intmap-ref forwarding-map target
                                                  (lambda (target) target))))
                      (if (eqv? target new-target)
                          forwarding-map
                          (intmap-replace forwarding-map label new-target))))
                  forwarding-map forwarding-map))
   (intmap-fold (lambda (label cont forwarding-labels)
                  (match cont
                    (($ $kargs _ _ ($ $continue k _ ($ $values)))
                     (match (lookup-parallel-moves label allocation)
                       (()
                        (match (intmap-ref cps k)
                          (($ $ktail) forwarding-labels)
                          (_ (intmap-add forwarding-labels label k))))
                       (_ forwarding-labels)))
                    (_ forwarding-labels)))
                cps empty-intmap)))

(define (compile-function cps asm)
  (let* ((allocation (allocate-slots cps))
         (forwarding-labels (compute-forwarding-labels cps allocation))
         (frame-size (lookup-nlocals allocation)))
    (define (forward-label k)
      (intmap-ref forwarding-labels k (lambda (k) k)))

    (define (elide-cont? label)
      (match (intmap-ref forwarding-labels label (lambda (_) #f))
        (#f #f)
        (target (not (eqv? label target)))))

    (define (maybe-slot sym)
      (lookup-maybe-slot sym allocation))

    (define (slot sym)
      (lookup-slot sym allocation))

    (define (constant sym)
      (lookup-constant-value sym allocation))

    (define (from-sp var)
      (- frame-size 1 var))

    (define (maybe-mov dst src)
      (unless (= dst src)
        (emit-mov asm (from-sp dst) (from-sp src))))

    (define (compile-tail label exp)
      ;; There are only three kinds of expressions in tail position:
      ;; tail calls, multiple-value returns, and single-value returns.
      (match exp
        (($ $call proc args)
         (for-each (match-lambda
                    ((src . dst) (emit-mov asm (from-sp dst) (from-sp src))))
                   (lookup-parallel-moves label allocation))
         (emit-tail-call asm (1+ (length args))))
        (($ $callk k proc args)
         (for-each (match-lambda
                    ((src . dst) (emit-mov asm (from-sp dst) (from-sp src))))
                   (lookup-parallel-moves label allocation))
         (emit-tail-call-label asm (1+ (length args)) k))
        (($ $values args)
         (for-each (match-lambda
                    ((src . dst) (emit-mov asm (from-sp dst) (from-sp src))))
                   (lookup-parallel-moves label allocation))
         (emit-return-values asm (1+ (length args))))))

    (define (compile-value label exp dst)
      (match exp
        (($ $values (arg))
         (maybe-mov dst (slot arg)))
        (($ $const exp)
         (emit-load-constant asm (from-sp dst) exp))
        (($ $closure k 0)
         (emit-load-static-procedure asm (from-sp dst) k))
        (($ $closure k nfree)
         (emit-make-closure asm (from-sp dst) k nfree))
        (($ $primcall 'current-module)
         (emit-current-module asm (from-sp dst)))
        (($ $primcall 'current-thread)
         (emit-current-thread asm (from-sp dst)))
        (($ $primcall 'cached-toplevel-box (scope name bound?))
         (emit-cached-toplevel-box asm (from-sp dst)
                                   (constant scope) (constant name)
                                   (constant bound?)))
        (($ $primcall 'cached-module-box (mod name public? bound?))
         (emit-cached-module-box asm (from-sp dst)
                                 (constant mod) (constant name)
                                 (constant public?) (constant bound?)))
        (($ $primcall 'define! (sym))
         (emit-define! asm (from-sp dst) (from-sp (slot sym))))
        (($ $primcall 'resolve (name bound?))
         (emit-resolve asm (from-sp dst) (constant bound?)
                       (from-sp (slot name))))
        (($ $primcall 'free-ref (closure idx))
         (emit-free-ref asm (from-sp dst) (from-sp (slot closure))
                        (constant idx)))
        (($ $primcall 'vector-ref (vector index))
         (emit-vector-ref asm (from-sp dst) (from-sp (slot vector))
                          (from-sp (slot index))))
        (($ $primcall 'make-vector (length init))
         (emit-make-vector asm (from-sp dst) (from-sp (slot length))
                           (from-sp (slot init))))
        (($ $primcall 'make-vector/immediate (length init))
         (emit-make-vector/immediate asm (from-sp dst) (constant length)
                                     (from-sp (slot init))))
        (($ $primcall 'vector-ref/immediate (vector index))
         (emit-vector-ref/immediate asm (from-sp dst) (from-sp (slot vector))
                                    (constant index)))
        (($ $primcall 'allocate-struct (vtable nfields))
         (emit-allocate-struct asm (from-sp dst) (from-sp (slot vtable))
                               (from-sp (slot nfields))))
        (($ $primcall 'allocate-struct/immediate (vtable nfields))
         (emit-allocate-struct/immediate asm (from-sp dst)
                                         (from-sp (slot vtable))
                                         (constant nfields)))
        (($ $primcall 'struct-ref (struct n))
         (emit-struct-ref asm (from-sp dst) (from-sp (slot struct))
                          (from-sp (slot n))))
        (($ $primcall 'struct-ref/immediate (struct n))
         (emit-struct-ref/immediate asm (from-sp dst) (from-sp (slot struct))
                                    (constant n)))
        (($ $primcall 'char->integer (src))
         (emit-char->integer asm (from-sp dst) (from-sp (slot src))))
        (($ $primcall 'integer->char (src))
         (emit-integer->char asm (from-sp dst) (from-sp (slot src))))
        (($ $primcall 'add/immediate (x y))
         (emit-add/immediate asm (from-sp dst) (from-sp (slot x)) (constant y)))
        (($ $primcall 'sub/immediate (x y))
         (emit-sub/immediate asm (from-sp dst) (from-sp (slot x)) (constant y)))
        (($ $primcall 'uadd/immediate (x y))
         (emit-uadd/immediate asm (from-sp dst) (from-sp (slot x))
                              (constant y)))
        (($ $primcall 'usub/immediate (x y))
         (emit-usub/immediate asm (from-sp dst) (from-sp (slot x))
                              (constant y)))
        (($ $primcall 'umul/immediate (x y))
         (emit-umul/immediate asm (from-sp dst) (from-sp (slot x))
                              (constant y)))
        (($ $primcall 'ursh/immediate (x y))
         (emit-ursh/immediate asm (from-sp dst) (from-sp (slot x))
                              (constant y)))
        (($ $primcall 'ulsh/immediate (x y))
         (emit-ulsh/immediate asm (from-sp dst) (from-sp (slot x))
                              (constant y)))
        (($ $primcall 'builtin-ref (name))
         (emit-builtin-ref asm (from-sp dst) (constant name)))
        (($ $primcall 'scm->f64 (src))
         (emit-scm->f64 asm (from-sp dst) (from-sp (slot src))))
        (($ $primcall 'load-f64 (src))
         (emit-load-f64 asm (from-sp dst) (constant src)))
        (($ $primcall 'f64->scm (src))
         (emit-f64->scm asm (from-sp dst) (from-sp (slot src))))
        (($ $primcall 'scm->u64 (src))
         (emit-scm->u64 asm (from-sp dst) (from-sp (slot src))))
        (($ $primcall 'scm->u64/truncate (src))
         (emit-scm->u64/truncate asm (from-sp dst) (from-sp (slot src))))
        (($ $primcall 'load-u64 (src))
         (emit-load-u64 asm (from-sp dst) (constant src)))
        (($ $primcall 'u64->scm (src))
         (emit-u64->scm asm (from-sp dst) (from-sp (slot src))))
        (($ $primcall 'scm->s64 (src))
         (emit-scm->s64 asm (from-sp dst) (from-sp (slot src))))
        (($ $primcall 'load-s64 (src))
         (emit-load-s64 asm (from-sp dst) (constant src)))
        (($ $primcall 's64->scm (src))
         (emit-s64->scm asm (from-sp dst) (from-sp (slot src))))
        (($ $primcall 'bv-length (bv))
         (emit-bv-length asm (from-sp dst) (from-sp (slot bv))))
        (($ $primcall 'bv-u8-ref (bv idx))
         (emit-bv-u8-ref asm (from-sp dst) (from-sp (slot bv))
                         (from-sp (slot idx))))
        (($ $primcall 'bv-s8-ref (bv idx))
         (emit-bv-s8-ref asm (from-sp dst) (from-sp (slot bv))
                         (from-sp (slot idx))))
        (($ $primcall 'bv-u16-ref (bv idx))
         (emit-bv-u16-ref asm (from-sp dst) (from-sp (slot bv))
                          (from-sp (slot idx))))
        (($ $primcall 'bv-s16-ref (bv idx))
         (emit-bv-s16-ref asm (from-sp dst) (from-sp (slot bv))
                          (from-sp (slot idx))))
        (($ $primcall 'bv-u32-ref (bv idx val))
         (emit-bv-u32-ref asm (from-sp dst) (from-sp (slot bv))
                          (from-sp (slot idx))))
        (($ $primcall 'bv-s32-ref (bv idx val))
         (emit-bv-s32-ref asm (from-sp dst) (from-sp (slot bv))
                          (from-sp (slot idx))))
        (($ $primcall 'bv-u64-ref (bv idx val))
         (emit-bv-u64-ref asm (from-sp dst) (from-sp (slot bv))
                          (from-sp (slot idx))))
        (($ $primcall 'bv-s64-ref (bv idx val))
         (emit-bv-s64-ref asm (from-sp dst) (from-sp (slot bv))
                          (from-sp (slot idx))))
        (($ $primcall 'bv-f32-ref (bv idx val))
         (emit-bv-f32-ref asm (from-sp dst) (from-sp (slot bv))
                          (from-sp (slot idx))))
        (($ $primcall 'bv-f64-ref (bv idx val))
         (emit-bv-f64-ref asm (from-sp dst) (from-sp (slot bv))
                          (from-sp (slot idx))))
        (($ $primcall 'make-atomic-box (init))
         (emit-make-atomic-box asm (from-sp dst) (from-sp (slot init))))
        (($ $primcall 'atomic-box-ref (box))
         (emit-atomic-box-ref asm (from-sp dst) (from-sp (slot box))))
        (($ $primcall 'atomic-box-swap! (box val))
         (emit-atomic-box-swap! asm (from-sp dst) (from-sp (slot box))
                                (from-sp (slot val))))
        (($ $primcall 'atomic-box-compare-and-swap! (box expected desired))
         (emit-atomic-box-compare-and-swap!
          asm (from-sp dst) (from-sp (slot box))
          (from-sp (slot expected)) (from-sp (slot desired))))
        (($ $primcall name args)
         ;; FIXME: Inline all the cases.
         (let ((inst (prim-instruction name)))
           (emit-text asm `((,inst ,(from-sp dst)
                                   ,@(map (compose from-sp slot) args))))))))

    (define (compile-effect label exp k)
      (match exp
        (($ $values ()) #f)
        (($ $prompt escape? tag handler)
         (match (intmap-ref cps handler)
           (($ $kreceive ($ $arity req () rest () #f) khandler-body)
            (let ((receive-args (gensym "handler"))
                  (nreq (length req))
                  (proc-slot (lookup-call-proc-slot label allocation)))
              (emit-prompt asm (from-sp (slot tag)) escape? proc-slot
                           receive-args)
              (emit-j asm k)
              (emit-label asm receive-args)
              (unless (and rest (zero? nreq))
                (emit-receive-values asm proc-slot (->bool rest) nreq))
              (when (and rest
                         (match (intmap-ref cps khandler-body)
                           (($ $kargs names (_ ... rest))
                            (maybe-slot rest))))
                (emit-bind-rest asm (+ proc-slot 1 nreq)))
              (for-each (match-lambda
                          ((src . dst) (emit-fmov asm dst src)))
                        (lookup-parallel-moves handler allocation))
              (emit-reset-frame asm frame-size)
              (emit-j asm (forward-label khandler-body))))))
        (($ $primcall 'cache-current-module! (sym scope))
         (emit-cache-current-module! asm (from-sp (slot sym)) (constant scope)))
        (($ $primcall 'free-set! (closure idx value))
         (emit-free-set! asm (from-sp (slot closure)) (from-sp (slot value))
                         (constant idx)))
        (($ $primcall 'box-set! (box value))
         (emit-box-set! asm (from-sp (slot box)) (from-sp (slot value))))
        (($ $primcall 'struct-set! (struct index value))
         (emit-struct-set! asm (from-sp (slot struct)) (from-sp (slot index))
                           (from-sp (slot value))))
        (($ $primcall 'struct-set!/immediate (struct index value))
         (emit-struct-set!/immediate asm (from-sp (slot struct))
                                     (constant index) (from-sp (slot value))))
        (($ $primcall 'vector-set! (vector index value))
         (emit-vector-set! asm (from-sp (slot vector)) (from-sp (slot index))
                           (from-sp (slot value))))
        (($ $primcall 'vector-set!/immediate (vector index value))
         (emit-vector-set!/immediate asm (from-sp (slot vector))
                                     (constant index) (from-sp (slot value))))
        (($ $primcall 'string-set! (string index char))
         (emit-string-set! asm (from-sp (slot string)) (from-sp (slot index))
                           (from-sp (slot char))))
        (($ $primcall 'set-car! (pair value))
         (emit-set-car! asm (from-sp (slot pair)) (from-sp (slot value))))
        (($ $primcall 'set-cdr! (pair value))
         (emit-set-cdr! asm (from-sp (slot pair)) (from-sp (slot value))))
        (($ $primcall 'push-fluid (fluid val))
         (emit-push-fluid asm (from-sp (slot fluid)) (from-sp (slot val))))
        (($ $primcall 'pop-fluid ())
         (emit-pop-fluid asm))
        (($ $primcall 'push-dynamic-state (state))
         (emit-push-dynamic-state asm (from-sp (slot state))))
        (($ $primcall 'pop-dynamic-state ())
         (emit-pop-dynamic-state asm))
        (($ $primcall 'wind (winder unwinder))
         (emit-wind asm (from-sp (slot winder)) (from-sp (slot unwinder))))
        (($ $primcall 'bv-u8-set! (bv idx val))
         (emit-bv-u8-set! asm (from-sp (slot bv)) (from-sp (slot idx))
                          (from-sp (slot val))))
        (($ $primcall 'bv-s8-set! (bv idx val))
         (emit-bv-s8-set! asm (from-sp (slot bv)) (from-sp (slot idx))
                          (from-sp (slot val))))
        (($ $primcall 'bv-u16-set! (bv idx val))
         (emit-bv-u16-set! asm (from-sp (slot bv)) (from-sp (slot idx))
                           (from-sp (slot val))))
        (($ $primcall 'bv-s16-set! (bv idx val))
         (emit-bv-s16-set! asm (from-sp (slot bv)) (from-sp (slot idx))
                           (from-sp (slot val))))
        (($ $primcall 'bv-u32-set! (bv idx val))
         (emit-bv-u32-set! asm (from-sp (slot bv)) (from-sp (slot idx))
                           (from-sp (slot val))))
        (($ $primcall 'bv-s32-set! (bv idx val))
         (emit-bv-s32-set! asm (from-sp (slot bv)) (from-sp (slot idx))
                           (from-sp (slot val))))
        (($ $primcall 'bv-u64-set! (bv idx val))
         (emit-bv-u64-set! asm (from-sp (slot bv)) (from-sp (slot idx))
                           (from-sp (slot val))))
        (($ $primcall 'bv-s64-set! (bv idx val))
         (emit-bv-s64-set! asm (from-sp (slot bv)) (from-sp (slot idx))
                           (from-sp (slot val))))
        (($ $primcall 'bv-f32-set! (bv idx val))
         (emit-bv-f32-set! asm (from-sp (slot bv)) (from-sp (slot idx))
                           (from-sp (slot val))))
        (($ $primcall 'bv-f64-set! (bv idx val))
         (emit-bv-f64-set! asm (from-sp (slot bv)) (from-sp (slot idx))
                           (from-sp (slot val))))
        (($ $primcall 'unwind ())
         (emit-unwind asm))
        (($ $primcall 'fluid-set! (fluid value))
         (emit-fluid-set! asm (from-sp (slot fluid)) (from-sp (slot value))))
        (($ $primcall 'atomic-box-set! (box val))
         (emit-atomic-box-set! asm (from-sp (slot box)) (from-sp (slot val))))
        (($ $primcall 'handle-interrupts ())
         (emit-handle-interrupts asm))))

    (define (compile-values label exp syms)
      (match exp
        (($ $values args)
         (for-each (match-lambda
                    ((src . dst) (emit-mov asm (from-sp dst) (from-sp src))))
                   (lookup-parallel-moves label allocation)))))

    (define (compile-test label exp kt kf next-label)
      (define (prefer-true?)
        (if (< (max kt kf) label)
            ;; Two backwards branches.  Prefer
            ;; the nearest.
            (> kt kf)
            ;; Otherwise prefer a backwards
            ;; branch or a near jump.
            (< kt kf)))
      (define (emit-branch emit-jt emit-jf)
        (cond
         ((eq? kt next-label)
          (emit-jf asm kf))
         ((eq? kf next-label)
          (emit-jt asm kt))
         ((prefer-true?)
          (emit-jt asm kt)
          (emit-j asm kf))
         (else
          (emit-jf asm kf)
          (emit-j asm kt))))
      (define (unary op a)
        (op asm (from-sp (slot a)))
        (emit-branch emit-je emit-jne))
      (define (binary-test op a b)
        (op asm (from-sp (slot a)) (from-sp (slot b)))
        (emit-branch emit-je emit-jne))
      (define (binary* op emit-jt emit-jf a b)
        (op asm (from-sp (slot a)) (from-sp (slot b)))
        (emit-branch emit-jt emit-jf))
      (define (binary op a b)
        (cond
         ((eq? kt next-label)
          (op asm (from-sp (slot a)) (from-sp (slot b)) #t kf))
         ((eq? kf next-label)
          (op asm (from-sp (slot a)) (from-sp (slot b)) #f kt))
         (else
          (let ((invert? (not (prefer-true?))))
            (op asm (from-sp (slot a)) (from-sp (slot b)) invert?
                (if invert? kf kt))
            (emit-j asm (if invert? kt kf))))))
      (match exp
        (($ $primcall 'heap-object? (a)) (unary emit-heap-object? a))
        (($ $primcall 'null? (a)) (unary emit-null? a))
        (($ $primcall 'nil? (a)) (unary emit-nil? a))
        (($ $primcall 'false? (a)) (unary emit-false? a))
        (($ $primcall 'pair? (a)) (unary emit-pair? a))
        (($ $primcall 'struct? (a)) (unary emit-struct? a))
        (($ $primcall 'char? (a)) (unary emit-char? a))
        (($ $primcall 'symbol? (a)) (unary emit-symbol? a))
        (($ $primcall 'variable? (a)) (unary emit-variable? a))
        (($ $primcall 'vector? (a)) (unary emit-vector? a))
        (($ $primcall 'string? (a)) (unary emit-string? a))
        (($ $primcall 'bytevector? (a)) (unary emit-bytevector? a))
        (($ $primcall 'bitvector? (a)) (unary emit-bitvector? a))
        (($ $primcall 'keyword? (a)) (unary emit-keyword? a))
        (($ $primcall 'heap-number? (a)) (unary emit-heap-number? a))
        ;; Add more TC7 tests here.  Keep in sync with
        ;; *branching-primcall-arities* in (language cps primitives) and
        ;; the set of macro-instructions in assembly.scm.
        (($ $primcall 'eq? (a b)) (binary-test emit-eq? a b))
        (($ $primcall 'heap-numbers-equal? (a b))
         (binary-test emit-heap-numbers-equal? a b))
        (($ $primcall '< (a b)) (binary* emit-<? emit-jl emit-jnl a b))
        (($ $primcall '<= (a b)) (binary* emit-<? emit-jge emit-jnge b a))
        (($ $primcall '= (a b)) (binary-test emit-=? a b))
        (($ $primcall '>= (a b)) (binary* emit-<? emit-jge emit-jnge a b))
        (($ $primcall '> (a b)) (binary* emit-<? emit-jl emit-jnl b a))
        (($ $primcall 'u64-< (a b)) (binary* emit-u64<? emit-jl emit-jnl a b))
        (($ $primcall 'u64-<= (a b)) (binary* emit-u64<? emit-jnl emit-jl b a))
        (($ $primcall 'u64-= (a b)) (binary-test emit-u64=? a b))
        (($ $primcall 'u64->= (a b)) (binary* emit-u64<? emit-jnl emit-jl a b))
        (($ $primcall 'u64-> (a b)) (binary* emit-u64<? emit-jl emit-jnl b a))
        (($ $primcall 'f64-< (a b)) (binary* emit-f64<? emit-jl emit-jnl a b))
        (($ $primcall 'f64-<= (a b)) (binary* emit-f64<? emit-jge emit-jnge b a))
        (($ $primcall 'f64-= (a b)) (binary-test emit-f64=? a b))
        (($ $primcall 'f64->= (a b)) (binary* emit-f64<? emit-jge emit-jnge a b))
        (($ $primcall 'f64-> (a b)) (binary* emit-f64<? emit-jl emit-jnl b a))
        (($ $primcall 'u64-<-scm (a b)) (binary emit-br-if-u64-<-scm a b))
        (($ $primcall 'u64-<=-scm (a b)) (binary emit-br-if-u64-<=-scm a b))
        (($ $primcall 'u64-=-scm (a b)) (binary emit-br-if-u64-=-scm a b))
        (($ $primcall 'u64->=-scm (a b)) (binary emit-br-if-u64->=-scm a b))
        (($ $primcall 'u64->-scm (a b)) (binary emit-br-if-u64->-scm a b))
        (($ $primcall 'logtest (a b)) (binary emit-br-if-logtest a b))))

    (define (compile-trunc label k exp nreq rest-var)
      (define (do-call proc args emit-call)
        (let* ((proc-slot (lookup-call-proc-slot label allocation))
               (nargs (1+ (length args)))
               (arg-slots (map (lambda (x) (+ x proc-slot)) (iota nargs))))
          (for-each (match-lambda
                     ((src . dst) (emit-mov asm (from-sp dst) (from-sp src))))
                    (lookup-parallel-moves label allocation))
          (emit-call asm proc-slot nargs)
          (emit-slot-map asm proc-slot (lookup-slot-map label allocation))
          (cond
           ((and (= 1 nreq) (and rest-var) (not (maybe-slot rest-var))
                 (match (lookup-parallel-moves k allocation)
                   ((((? (lambda (src) (= src (1+ proc-slot))) src)
                      . dst)) dst)
                   (_ #f)))
            ;; The usual case: one required live return value, ignoring
            ;; any additional values.
            => (lambda (dst)
                 (emit-receive asm dst proc-slot frame-size)))
           (else
            (unless (and (zero? nreq) rest-var)
              (emit-receive-values asm proc-slot (->bool rest-var) nreq))
            (when (and rest-var (maybe-slot rest-var))
              (emit-bind-rest asm (+ proc-slot 1 nreq)))
            (for-each (match-lambda
                       ((src . dst) (emit-fmov asm dst src)))
                      (lookup-parallel-moves k allocation))
            (emit-reset-frame asm frame-size)))))
      (match exp
        (($ $call proc args)
         (do-call proc args
                  (lambda (asm proc-slot nargs)
                    (emit-call asm proc-slot nargs))))
        (($ $callk k proc args)
         (do-call proc args
                  (lambda (asm proc-slot nargs)
                    (emit-call-label asm proc-slot nargs k))))))

    (define (skip-elided-conts label)
      (if (elide-cont? label)
          (skip-elided-conts (1+ label))
          label))

    (define (compile-expression label k exp)
      (let* ((forwarded-k (forward-label k))
             (fallthrough? (= forwarded-k (skip-elided-conts (1+ label)))))
        (define (maybe-emit-jump)
          (unless fallthrough?
            (emit-j asm forwarded-k)))
        (match (intmap-ref cps k)
          (($ $ktail)
           (compile-tail label exp))
          (($ $kargs (name) (sym))
           (let ((dst (maybe-slot sym)))
             (when dst
               (compile-value label exp dst)))
           (maybe-emit-jump))
          (($ $kargs () ())
           (match exp
             (($ $branch kt exp)
              (compile-test label exp (forward-label kt) forwarded-k
                            (skip-elided-conts (1+ label))))
             (_
              (compile-effect label exp k)
              (maybe-emit-jump))))
          (($ $kargs names syms)
           (compile-values label exp syms)
           (maybe-emit-jump))
          (($ $kreceive ($ $arity req () rest () #f) kargs)
           (compile-trunc label k exp (length req)
                          (and rest
                               (match (intmap-ref cps kargs)
                                 (($ $kargs names (_ ... rest)) rest))))
           (let* ((kargs (forward-label kargs))
                  (fallthrough? (and fallthrough?
                                     (= kargs (skip-elided-conts (1+ k))))))
             (unless fallthrough?
               (emit-j asm kargs)))))))

    (define (compile-cont label cont)
      (match cont
        (($ $kfun src meta self tail clause)
         (when src
           (emit-source asm src))
         (emit-begin-program asm label meta))
        (($ $kclause ($ $arity req opt rest kw allow-other-keys?) body alt)
         (let ((first? (match (intmap-ref cps (1- label))
                         (($ $kfun) #t)
                         (_ #f)))
               (kw-indices (map (match-lambda
                                 ((key name sym)
                                  (cons key (lookup-slot sym allocation))))
                                kw)))
           (unless first?
             (emit-end-arity asm))
           (emit-label asm label)
           (emit-begin-kw-arity asm req opt rest kw-indices allow-other-keys?
                                frame-size alt)
           ;; All arities define a closure binding in slot 0.
           (emit-definition asm 'closure 0 'scm)
           ;; Usually we just fall through, but it could be the body is
           ;; contified into another clause.
           (let ((body (forward-label body)))
             (unless (= body (skip-elided-conts (1+ label)))
               (emit-j asm body)))))
        (($ $kargs names vars ($ $continue k src exp))
         (emit-label asm label)
         (for-each (lambda (name var)
                     (let ((slot (maybe-slot var)))
                       (when slot
                         (let ((repr (lookup-representation var allocation)))
                           (emit-definition asm name slot repr)))))
                   names vars)
         (when src
           (emit-source asm src))
         (unless (elide-cont? label)
           (compile-expression label k exp)))
        (($ $kreceive arity kargs)
         (emit-label asm label))
        (($ $ktail)
         (emit-end-arity asm)
         (emit-end-program asm))))

    (intmap-for-each compile-cont cps)))

(define (emit-bytecode exp env opts)
  (let ((asm (make-assembler)))
    (intmap-for-each (lambda (kfun body)
                       (compile-function (intmap-select exp body) asm))
                     (compute-reachable-functions exp 0))
    (values (link-assembly asm #:page-aligned? (kw-arg-ref opts #:to-file? #f))
            env
            env)))

(define (lower-cps exp opts)
  ;; FIXME: For now the closure conversion pass relies on $rec instances
  ;; being separated into SCCs.  We should fix this to not be the case,
  ;; and instead move the split-rec pass back to
  ;; optimize-higher-order-cps.
  (set! exp (split-rec exp))
  (set! exp (optimize-higher-order-cps exp opts))
  (set! exp (convert-closures exp))
  (set! exp (optimize-first-order-cps exp opts))
  (set! exp (reify-primitives exp))
  (set! exp (add-handle-interrupts exp))
  (renumber exp))

(define (compile-bytecode exp env opts)
  (set! exp (lower-cps exp opts))
  (emit-bytecode exp env opts))
