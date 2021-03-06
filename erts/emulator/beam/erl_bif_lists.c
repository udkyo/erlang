/*
 * %CopyrightBegin%
 *
 * Copyright Ericsson AB 1999-2016. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * %CopyrightEnd%
 */

/*
 * BIFs logically belonging to the lists module.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "sys.h"
#include "erl_vm.h"
#include "global.h"
#include "bif.h"
#include "erl_binary.h"


static Eterm keyfind(int Bif, Process* p, Eterm Key, Eterm Pos, Eterm List);


static BIF_RETTYPE append(Process* p, Eterm A, Eterm B)
{
    Eterm list;
    Eterm copy;
    Eterm last;
    Eterm* hp = NULL;
    Sint i;

    list = A;

    if (is_nil(list)) {
        BIF_RET(B);
    }

    if (is_not_list(list)) {
        BIF_ERROR(p, BADARG);
    }

    /* optimistic append on heap first */

    if ((i = HeapWordsLeft(p) / 2) < 4) {
        goto list_tail;
    }

    hp   = HEAP_TOP(p);
    copy = last = CONS(hp, CAR(list_val(list)), make_list(hp+2));
    list = CDR(list_val(list));
    hp  += 2;
    i   -= 2; /* don't use the last 2 words (extra i--;) */

    while(i-- && is_list(list)) {
        Eterm* listp = list_val(list);
        last = CONS(hp, CAR(listp), make_list(hp+2));
        list = CDR(listp);
        hp += 2;
    }

    /* A is proper and B is NIL return A as-is, don't update HTOP */

    if (is_nil(list) && is_nil(B)) {
        BIF_RET(A);
    }

    if (is_nil(list)) {
        HEAP_TOP(p) = hp;
        CDR(list_val(last)) = B;
        BIF_RET(copy);
    }

list_tail:

    if ((i = erts_list_length(list)) < 0) {
        BIF_ERROR(p, BADARG);
    }

    /* remaining list was proper and B is NIL */
    if (is_nil(B)) {
        BIF_RET(A);
    }

    if (hp) {
        /* Note: fall through case, already written
         * on the heap.
         * The last 2 words of the heap is not written yet
         */
        Eterm *hp_save = hp;
        ASSERT(i != 0);
        HEAP_TOP(p) = hp + 2;
        if (i == 1) {
            hp[0] = CAR(list_val(list));
            hp[1] = B;
            BIF_RET(copy);
        }
        hp   = HAlloc(p, 2*(i - 1));
        last = CONS(hp_save, CAR(list_val(list)), make_list(hp));
    } else {
        hp   = HAlloc(p, 2*i);
        copy = last = CONS(hp, CAR(list_val(list)), make_list(hp+2));
        hp  += 2;
    }

    list = CDR(list_val(list));
    i--;

    ASSERT(i > -1);
    while(i--) {
        Eterm* listp = list_val(list);
        last = CONS(hp, CAR(listp), make_list(hp+2));
        list = CDR(listp);
        hp  += 2;
    }

    CDR(list_val(last)) = B;
    BIF_RET(copy);
}

/*
 * erlang:'++'/2
 */

Eterm
ebif_plusplus_2(BIF_ALIST_2)
{
    return append(BIF_P, BIF_ARG_1, BIF_ARG_2);
}

BIF_RETTYPE append_2(BIF_ALIST_2)
{
    return append(BIF_P, BIF_ARG_1, BIF_ARG_2);
}

/* erlang:'--'/2
 *
 * Subtracts a list from another (LHS -- RHS), removing the first occurrence of
 * each element in LHS from RHS. There is no type coercion so the elements must
 * match exactly.
 *
 * The BIF is broken into several stages that can all trap individually, and it
 * chooses its algorithm based on input size. If either input is small it will
 * use a linear scan tuned to which side it's on, and if both inputs are large
 * enough it will convert RHS into a multiset to provide good asymptotic
 * behavior. */

#define SUBTRACT_LHS_THRESHOLD 16
#define SUBTRACT_RHS_THRESHOLD 16

typedef enum {
    SUBTRACT_STAGE_START,
    SUBTRACT_STAGE_LEN_LHS,

    /* Naive linear scan that's efficient when
     * LEN_LHS <= SUBTRACT_LHS_THRESHOLD. */
    SUBTRACT_STAGE_NAIVE_LHS,

    SUBTRACT_STAGE_LEN_RHS,

    /* As SUBTRACT_STAGE_NAIVE_LHS but for RHS. */
    SUBTRACT_STAGE_NAIVE_RHS,

    /* Creates a multiset from RHS for faster lookups before sweeping through
     * LHS. The set is implemented as a red-black tree and duplicate elements
     * are handled by a counter on each node. */
    SUBTRACT_STAGE_SET_BUILD,
    SUBTRACT_STAGE_SET_FINISH
} ErtsSubtractCtxStage;

typedef struct subtract_node__ {
    struct subtract_node__ *parent;
    struct subtract_node__ *left;
    struct subtract_node__ *right;
    int is_red;

    Eterm key;
    Uint count;
} subtract_tree_t;

typedef struct {
    ErtsSubtractCtxStage stage;

    Eterm lhs_original;
    Eterm rhs_original;

    Uint lhs_remaining;
    Uint rhs_remaining;

    Eterm iterator;

    Eterm *result_cdr;
    Eterm result;

    union {
        Eterm lhs_elements[SUBTRACT_LHS_THRESHOLD];
        Eterm rhs_elements[SUBTRACT_RHS_THRESHOLD];

        struct {
            subtract_tree_t *tree;

            /* A memory area for the tree's nodes, saving us the need to have
             * one allocation per node. */
            subtract_tree_t *alloc_start;
            subtract_tree_t *alloc;
        } rhs_set;
    } u;
} ErtsSubtractContext;

#define ERTS_RBT_PREFIX subtract
#define ERTS_RBT_T subtract_tree_t
#define ERTS_RBT_KEY_T Eterm
#define ERTS_RBT_FLAGS_T int
#define ERTS_RBT_INIT_EMPTY_TNODE(T) \
    do { \
        (T)->parent = NULL; \
        (T)->left = NULL; \
        (T)->right = NULL; \
    } while(0)
#define ERTS_RBT_IS_RED(T) ((T)->is_red)
#define ERTS_RBT_SET_RED(T) ((T)->is_red = 1)
#define ERTS_RBT_IS_BLACK(T) (!ERTS_RBT_IS_RED(T))
#define ERTS_RBT_SET_BLACK(T) ((T)->is_red = 0)
#define ERTS_RBT_GET_FLAGS(T) ((T)->is_red)
#define ERTS_RBT_SET_FLAGS(T, F) ((T)->is_red = F)
#define ERTS_RBT_GET_PARENT(T) ((T)->parent)
#define ERTS_RBT_SET_PARENT(T, P) ((T)->parent = P)
#define ERTS_RBT_GET_RIGHT(T) ((T)->right)
#define ERTS_RBT_SET_RIGHT(T, R) ((T)->right = (R))
#define ERTS_RBT_GET_LEFT(T) ((T)->left)
#define ERTS_RBT_SET_LEFT(T, L) ((T)->left = (L))
#define ERTS_RBT_GET_KEY(T) ((T)->key)
#define ERTS_RBT_IS_LT(KX, KY) (CMP_TERM(KX, KY) < 0)
#define ERTS_RBT_IS_EQ(KX, KY) EQ(KX, KY)
#define ERTS_RBT_WANT_LOOKUP_INSERT
#define ERTS_RBT_WANT_LOOKUP
#define ERTS_RBT_WANT_DELETE
#define ERTS_RBT_UNDEF

#include "erl_rbtree.h"

static int subtract_continue(Process *p, ErtsSubtractContext *context);

static void subtract_ctx_dtor(ErtsSubtractContext *context) {
    switch (context->stage) {
        case SUBTRACT_STAGE_SET_BUILD:
        case SUBTRACT_STAGE_SET_FINISH:
            erts_free(ERTS_ALC_T_LIST_TRAP, context->u.rhs_set.alloc_start);
            break;
        default:
            break;
    }
}

static int subtract_ctx_bin_dtor(Binary *context_bin) {
    ErtsSubtractContext *context = ERTS_MAGIC_BIN_DATA(context_bin);
    subtract_ctx_dtor(context);
    return 1;
}

static void subtract_ctx_move(ErtsSubtractContext *from,
                              ErtsSubtractContext *to) {
    int uses_result_cdr = 0;

    to->stage = from->stage;

    to->lhs_original = from->lhs_original;
    to->rhs_original = from->rhs_original;

    to->lhs_remaining = from->lhs_remaining;
    to->rhs_remaining = from->rhs_remaining;

    to->iterator = from->iterator;
    to->result = from->result;

    switch (to->stage) {
        case SUBTRACT_STAGE_NAIVE_LHS:
            sys_memcpy(to->u.lhs_elements,
                       from->u.lhs_elements,
                       sizeof(Eterm) * to->lhs_remaining);
            break;
        case SUBTRACT_STAGE_NAIVE_RHS:
            sys_memcpy(to->u.rhs_elements,
                       from->u.rhs_elements,
                       sizeof(Eterm) * to->rhs_remaining);

            uses_result_cdr = 1;
            break;
        case SUBTRACT_STAGE_SET_FINISH:
            uses_result_cdr = 1;
            /* FALL THROUGH */
        case SUBTRACT_STAGE_SET_BUILD:
            to->u.rhs_set.alloc_start = from->u.rhs_set.alloc_start;
            to->u.rhs_set.alloc = from->u.rhs_set.alloc;
            to->u.rhs_set.tree = from->u.rhs_set.tree;
            break;
        default:
            break;
    }

    if (uses_result_cdr) {
        if (from->result_cdr == &from->result) {
            to->result_cdr = &to->result;
        } else {
            to->result_cdr = from->result_cdr;
        }
    }
}

static Eterm subtract_create_trap_state(Process *p,
                                        ErtsSubtractContext *context) {
    Binary *state_bin;
    Eterm *hp;

    state_bin = erts_create_magic_binary(sizeof(ErtsSubtractContext),
                                         subtract_ctx_bin_dtor);

    subtract_ctx_move(context, ERTS_MAGIC_BIN_DATA(state_bin));

    hp = HAlloc(p, ERTS_MAGIC_REF_THING_SIZE);

    return erts_mk_magic_ref(&hp, &MSO(p), state_bin);
}

static int subtract_enter_len_lhs(Process *p, ErtsSubtractContext *context) {
    context->stage = SUBTRACT_STAGE_LEN_LHS;

    context->iterator = context->lhs_original;
    context->lhs_remaining = 0;

    return subtract_continue(p, context);
}

static int subtract_enter_len_rhs(Process *p, ErtsSubtractContext *context) {
    context->stage = SUBTRACT_STAGE_LEN_RHS;

    context->iterator = context->rhs_original;
    context->rhs_remaining = 0;

    return subtract_continue(p, context);
}

static int subtract_get_length(Process *p, Eterm *iterator_p, Uint *count_p) {
    static const Sint ELEMENTS_PER_RED = 32;

    Sint budget, count;
    Eterm iterator;

    budget = ELEMENTS_PER_RED * ERTS_BIF_REDS_LEFT(p);
    iterator = *iterator_p;

#ifdef DEBUG
    budget = budget / 10 + 1;
#endif

    for (count = 0; count < budget && is_list(iterator); count++) {
        iterator = CDR(list_val(iterator));
    }

    if (!is_list(iterator) && !is_nil(iterator)) {
        return -1;
    }

    BUMP_REDS(p, count / ELEMENTS_PER_RED);

    *iterator_p = iterator;
    *count_p += count;

    if (is_nil(iterator)) {
        return 1;
    }

    return 0;
}

static int subtract_enter_naive_lhs(Process *p, ErtsSubtractContext *context) {
    Eterm iterator;
    int i = 0;

    context->stage = SUBTRACT_STAGE_NAIVE_LHS;

    context->iterator = context->rhs_original;
    context->result = NIL;

    iterator = context->lhs_original;

    while (is_list(iterator)) {
        const Eterm *cell = list_val(iterator);

        ASSERT(i < SUBTRACT_LHS_THRESHOLD);

        context->u.lhs_elements[i++] = CAR(cell);
        iterator = CDR(cell);
    }

    ASSERT(i == context->lhs_remaining);

    return subtract_continue(p, context);
}

static int subtract_naive_lhs(Process *p, ErtsSubtractContext *context) {
    const Sint CHECKS_PER_RED = 16;
    Sint checks, budget;

    budget = CHECKS_PER_RED * ERTS_BIF_REDS_LEFT(p);
    checks = 0;

    while (checks < budget && is_list(context->iterator)) {
        const Eterm *cell;
        Eterm value, next;
        int found_at;

        cell = list_val(context->iterator);

        value = CAR(cell);
        next = CDR(cell);

        for (found_at = 0; found_at < context->lhs_remaining; found_at++) {
            if (EQ(value, context->u.lhs_elements[found_at])) {
                /* We shift the array one step down as we have to preserve
                 * order.
                 *
                 * Note that we can't exit early as that would suppress errors
                 * in the right-hand side (this runs prior to determining the
                 * length of RHS). */

                context->lhs_remaining--;
                sys_memmove(&context->u.lhs_elements[found_at],
                            &context->u.lhs_elements[found_at + 1],
                            (context->lhs_remaining - found_at) * sizeof(Eterm));
                break;
            }
        }

        checks += MAX(1, context->lhs_remaining);
        context->iterator = next;
    }

    BUMP_REDS(p, MIN(checks, budget) / CHECKS_PER_RED);

    if (is_list(context->iterator)) {
        return 0;
    } else if (!is_nil(context->iterator)) {
        return -1;
    }

    if (context->lhs_remaining > 0) {
        Eterm *hp;
        int i;

        hp = HAlloc(p, context->lhs_remaining * 2);

        for (i = context->lhs_remaining - 1; i >= 0; i--) {
            Eterm value = context->u.lhs_elements[i];

            context->result = CONS(hp, value, context->result);
            hp += 2;
        }
    }

    ASSERT(context->lhs_remaining > 0 || context->result == NIL);

    return 1;
}

static int subtract_enter_naive_rhs(Process *p, ErtsSubtractContext *context) {
    Eterm iterator;
    int i = 0;

    context->stage = SUBTRACT_STAGE_NAIVE_RHS;

    context->iterator = context->lhs_original;
    context->result_cdr = &context->result;
    context->result = NIL;

    iterator = context->rhs_original;

    while (is_list(iterator)) {
        const Eterm *cell = list_val(iterator);

        ASSERT(i < SUBTRACT_RHS_THRESHOLD);

        context->u.rhs_elements[i++] = CAR(cell);
        iterator = CDR(cell);
    }

    ASSERT(i == context->rhs_remaining);

    return subtract_continue(p, context);
}

static int subtract_naive_rhs(Process *p, ErtsSubtractContext *context) {
    const Sint CHECKS_PER_RED = 16;
    Sint checks, budget;

    budget = CHECKS_PER_RED * ERTS_BIF_REDS_LEFT(p);
    checks = 0;

#ifdef DEBUG
    budget = budget / 10 + 1;
#endif

    while (checks < budget && is_list(context->iterator)) {
        const Eterm *cell;
        Eterm value, next;
        int found_at;

        cell = list_val(context->iterator);
        value = CAR(cell);
        next = CDR(cell);

        for (found_at = context->rhs_remaining - 1; found_at >= 0; found_at--) {
            if (EQ(value, context->u.rhs_elements[found_at])) {
                break;
            }
        }

        if (found_at < 0) {
            /* Destructively add the value to the result. This is safe
             * since the GC is disabled and the unfinished term is never
             * leaked to the outside world. */
            Eterm *hp = HAllocX(p, 2, context->lhs_remaining * 2);

            *context->result_cdr = make_list(hp);
            context->result_cdr = &CDR(hp);

            CAR(hp) = value;
        } else if (found_at >= 0) {
            Eterm swap;

            if (context->rhs_remaining-- == 1) {
                /* We've run out of items to remove, so the rest of the
                 * result will be equal to the remainder of the input. We know
                 * that LHS is well-formed as any errors would've been reported
                 * during length determination. */
                *context->result_cdr = next;

                BUMP_REDS(p, MIN(budget, checks) / CHECKS_PER_RED);

                return 1;
            }

            swap = context->u.rhs_elements[context->rhs_remaining];
            context->u.rhs_elements[found_at] = swap;
        }

        checks += context->rhs_remaining;
        context->iterator = next;
        context->lhs_remaining--;
    }

    /* The result only has to be terminated when returning it to the user, but
     * we're doing it when trapping as well to prevent headaches when
     * debugging. */
    *context->result_cdr = NIL;

    BUMP_REDS(p, MIN(budget, checks) / CHECKS_PER_RED);

    if (is_list(context->iterator)) {
        ASSERT(context->lhs_remaining > 0 && context->rhs_remaining > 0);
        return 0;
    }

    return 1;
}

static int subtract_enter_set_build(Process *p, ErtsSubtractContext *context) {
    context->stage = SUBTRACT_STAGE_SET_BUILD;

    context->u.rhs_set.alloc_start =
        erts_alloc(ERTS_ALC_T_LIST_TRAP,
                   context->rhs_remaining * sizeof(subtract_tree_t));

    context->u.rhs_set.alloc = context->u.rhs_set.alloc_start;
    context->u.rhs_set.tree = NULL;

    context->iterator = context->rhs_original;

    return subtract_continue(p, context);
}

static int subtract_set_build(Process *p, ErtsSubtractContext *context) {
    const static Sint INSERTIONS_PER_RED = 16;
    Sint budget, insertions;

    budget = INSERTIONS_PER_RED * ERTS_BIF_REDS_LEFT(p);
    insertions = 0;

#ifdef DEBUG
    budget = budget / 10 + 1;
#endif

    while (insertions < budget && is_list(context->iterator)) {
        subtract_tree_t *existing_node, *new_node;
        const Eterm *cell;
        Eterm value, next;

        cell = list_val(context->iterator);
        value = CAR(cell);
        next = CDR(cell);

        new_node = context->u.rhs_set.alloc;
        new_node->key = value;
        new_node->count = 1;

        existing_node = subtract_rbt_lookup_insert(&context->u.rhs_set.tree,
                                                   new_node);

        if (existing_node != NULL) {
            existing_node->count++;
        } else {
            context->u.rhs_set.alloc++;
        }

        context->iterator = next;
        insertions++;
    }

    BUMP_REDS(p, insertions / INSERTIONS_PER_RED);

    ASSERT(is_list(context->iterator) || is_nil(context->iterator));
    ASSERT(context->u.rhs_set.tree != NULL);

    return is_nil(context->iterator);
}

static int subtract_enter_set_finish(Process *p, ErtsSubtractContext *context) {
    context->stage = SUBTRACT_STAGE_SET_FINISH;

    context->result_cdr = &context->result;
    context->result = NIL;

    context->iterator = context->lhs_original;

    return subtract_continue(p, context);
}

static int subtract_set_finish(Process *p, ErtsSubtractContext *context) {
    const Sint CHECKS_PER_RED = 8;
    Sint checks, budget;

    budget = CHECKS_PER_RED * ERTS_BIF_REDS_LEFT(p);
    checks = 0;

#ifdef DEBUG
    budget = budget / 10 + 1;
#endif

    while (checks < budget && is_list(context->iterator)) {
        subtract_tree_t *node;
        const Eterm *cell;
        Eterm value, next;

        cell = list_val(context->iterator);
        value = CAR(cell);
        next = CDR(cell);

        ASSERT(context->rhs_remaining > 0);

        node = subtract_rbt_lookup(context->u.rhs_set.tree, value);

        if (node == NULL) {
            Eterm *hp = HAllocX(p, 2, context->lhs_remaining * 2);

            *context->result_cdr = make_list(hp);
            context->result_cdr = &CDR(hp);

            CAR(hp) = value;
        } else {
            if (context->rhs_remaining-- == 1) {
                *context->result_cdr = next;

                BUMP_REDS(p, checks / CHECKS_PER_RED);

                return 1;
            }

            if (node->count-- == 1) {
                subtract_rbt_delete(&context->u.rhs_set.tree, node);
            }
        }

        context->iterator = next;
        context->lhs_remaining--;
        checks++;
    }

    *context->result_cdr = NIL;

    BUMP_REDS(p, checks / CHECKS_PER_RED);

    if (is_list(context->iterator)) {
        ASSERT(context->lhs_remaining > 0 && context->rhs_remaining > 0);
        return 0;
    }

    return 1;
}

static int subtract_continue(Process *p, ErtsSubtractContext *context) {
    switch (context->stage) {
        case SUBTRACT_STAGE_START: {
            return subtract_enter_len_lhs(p, context);
        }

        case SUBTRACT_STAGE_LEN_LHS: {
            int res = subtract_get_length(p,
                                          &context->iterator,
                                          &context->lhs_remaining);

            if (res != 1) {
                return res;
            }

            if (context->lhs_remaining <= SUBTRACT_LHS_THRESHOLD) {
                return subtract_enter_naive_lhs(p, context);
            }

            return subtract_enter_len_rhs(p, context);
        }

        case SUBTRACT_STAGE_NAIVE_LHS: {
            return subtract_naive_lhs(p, context);
        }

        case SUBTRACT_STAGE_LEN_RHS: {
            int res = subtract_get_length(p,
                                          &context->iterator,
                                          &context->rhs_remaining);

            if (res != 1) {
                return res;
            }

            /* We've walked through both lists fully now so we no longer need
             * to check for errors past this point. */

            if (context->rhs_remaining <= SUBTRACT_RHS_THRESHOLD) {
                return subtract_enter_naive_rhs(p, context);
            }

            return subtract_enter_set_build(p, context);
        }

        case SUBTRACT_STAGE_NAIVE_RHS: {
            return subtract_naive_rhs(p, context);
        }

        case SUBTRACT_STAGE_SET_BUILD: {
            int res = subtract_set_build(p, context);

            if (res != 1) {
                return res;
            }

            return subtract_enter_set_finish(p, context);
        }

        case SUBTRACT_STAGE_SET_FINISH: {
            return subtract_set_finish(p, context);
        }

        default:
            ERTS_ASSERT(!"unreachable");
    }
}

static int subtract_start(Process *p, Eterm lhs, Eterm rhs,
                          ErtsSubtractContext *context) {
    context->stage = SUBTRACT_STAGE_START;

    context->lhs_original = lhs;
    context->rhs_original = rhs;

    return subtract_continue(p, context);
}

/* erlang:'--'/2 */
static Eterm subtract(Export *bif_entry, BIF_ALIST_2) {
    Eterm lhs = BIF_ARG_1, rhs = BIF_ARG_2;

    if ((is_list(lhs) || is_nil(lhs)) && (is_list(rhs) || is_nil(rhs))) {
        /* We start with the context on the stack in the hopes that we won't
         * have to trap. */
        ErtsSubtractContext context;
        int res;

        res = subtract_start(BIF_P, lhs, rhs, &context);

        if (res == 0) {
            Eterm state_mref;

            state_mref = subtract_create_trap_state(BIF_P, &context);
            erts_set_gc_state(BIF_P, 0);

            BIF_TRAP2(bif_entry, BIF_P, state_mref, NIL);
        }

        subtract_ctx_dtor(&context);

        if (res < 0) {
            BIF_ERROR(BIF_P, BADARG);
        }

        BIF_RET(context.result);
    } else if (is_internal_magic_ref(lhs)) {
        ErtsSubtractContext *context;
        int (*dtor)(Binary*);
        Binary *magic_bin;

        int res;

        magic_bin = erts_magic_ref2bin(lhs);
        dtor = ERTS_MAGIC_BIN_DESTRUCTOR(magic_bin);

        if (dtor != subtract_ctx_bin_dtor) {
            BIF_ERROR(BIF_P, BADARG);
        }

        ASSERT(BIF_P->flags & F_DISABLE_GC);
        ASSERT(rhs == NIL);

        context = ERTS_MAGIC_BIN_DATA(magic_bin);
        res = subtract_continue(BIF_P, context);

        if (res == 0) {
            BIF_TRAP2(bif_entry, BIF_P, lhs, NIL);
        }

        erts_set_gc_state(BIF_P, 1);

        if (res < 0) {
            ERTS_BIF_ERROR_TRAPPED2(BIF_P, BADARG, bif_entry,
                                    context->lhs_original,
                                    context->rhs_original);
        }

        BIF_RET(context->result);
    }

    ASSERT(!(BIF_P->flags & F_DISABLE_GC));

    BIF_ERROR(BIF_P, BADARG);
}

BIF_RETTYPE ebif_minusminus_2(BIF_ALIST_2) {
    return subtract(bif_export[BIF_ebif_minusminus_2], BIF_CALL_ARGS);
}

BIF_RETTYPE subtract_2(BIF_ALIST_2) {
    return subtract(bif_export[BIF_subtract_2], BIF_CALL_ARGS);
}


BIF_RETTYPE lists_member_2(BIF_ALIST_2)
{
    Eterm term;
    Eterm list;
    Eterm item;
    int non_immed_key;
    int max_iter = 10 * CONTEXT_REDS;

    if (is_nil(BIF_ARG_2)) {
	BIF_RET(am_false);
    } else if (is_not_list(BIF_ARG_2)) {
	BIF_ERROR(BIF_P, BADARG);
    }
    
    term = BIF_ARG_1;
    non_immed_key = is_not_immed(term);
    list = BIF_ARG_2;
    while (is_list(list)) {
	if (--max_iter < 0) {
	    BUMP_ALL_REDS(BIF_P);
	    BIF_TRAP2(bif_export[BIF_lists_member_2], BIF_P, term, list);
	}
	item = CAR(list_val(list));
	if ((item == term) || (non_immed_key && eq(item, term))) {
	    BIF_RET2(am_true, CONTEXT_REDS - max_iter/10);
	}
	list = CDR(list_val(list));
    }
    if (is_not_nil(list))  {
	BIF_ERROR(BIF_P, BADARG);
    }
    BIF_RET2(am_false, CONTEXT_REDS - max_iter/10);
}

BIF_RETTYPE lists_reverse_2(BIF_ALIST_2)
{
    Eterm list;
    Eterm tmp_list;
    Eterm result;
    Eterm* hp;
    Uint n;
    int max_iter;
    
    /*
     * Handle legal and illegal non-lists quickly.
     */
    if (is_nil(BIF_ARG_1)) {
	BIF_RET(BIF_ARG_2);
    } else if (is_not_list(BIF_ARG_1)) {
    error:
	BIF_ERROR(BIF_P, BADARG);
    }

    /*
     * First use the rest of the remaning heap space.
     */
    list = BIF_ARG_1;
    result = BIF_ARG_2;
    hp = HEAP_TOP(BIF_P);
    n = HeapWordsLeft(BIF_P) / 2;
    while (n != 0 && is_list(list)) {
	Eterm* pair = list_val(list);
	result = CONS(hp, CAR(pair), result);
	list = CDR(pair);
	hp += 2;
	n--;
    }
    HEAP_TOP(BIF_P) = hp;
    if (is_nil(list)) {
	BIF_RET(result);
    }

    /*
     * Calculate length of remaining list (up to a suitable limit).
     */
    max_iter = CONTEXT_REDS * 40;
    n = 0;
    tmp_list = list;
    while (max_iter-- > 0 && is_list(tmp_list)) {
	tmp_list = CDR(list_val(tmp_list));
	n++;
    }
    if (is_not_nil(tmp_list) && is_not_list(tmp_list)) {
	goto error;
    }

    /*
     * Now do one HAlloc() and continue reversing.
     */
    hp = HAlloc(BIF_P, 2*n);
    while (n != 0 && is_list(list)) {
	Eterm* pair = list_val(list);
	result = CONS(hp, CAR(pair), result);
	list = CDR(pair);
	hp += 2;
	n--;
    }
    if (is_nil(list)) {
	BIF_RET(result);
    } else {
	BUMP_ALL_REDS(BIF_P);
	BIF_TRAP2(bif_export[BIF_lists_reverse_2], BIF_P, list, result);
    }
}

BIF_RETTYPE
lists_keymember_3(BIF_ALIST_3)
{
    Eterm res;

    res = keyfind(BIF_lists_keymember_3, BIF_P,
		  BIF_ARG_1, BIF_ARG_2, BIF_ARG_3);
    if (is_value(res) && is_tuple(res)) {
	return am_true;
    } else {
	return res;
    }
}

BIF_RETTYPE
lists_keysearch_3(BIF_ALIST_3)
{
    Eterm res;
    
    res = keyfind(BIF_lists_keysearch_3, BIF_P,
		  BIF_ARG_1, BIF_ARG_2, BIF_ARG_3);
    if (is_non_value(res) || is_not_tuple(res)) {
	return res;
    } else {			/* Tuple */
	Eterm* hp = HAlloc(BIF_P, 3);
	return TUPLE2(hp, am_value, res);
    }
}

BIF_RETTYPE
lists_keyfind_3(BIF_ALIST_3)
{
    return keyfind(BIF_lists_keyfind_3, BIF_P,
		   BIF_ARG_1, BIF_ARG_2, BIF_ARG_3);
}

static Eterm
keyfind(int Bif, Process* p, Eterm Key, Eterm Pos, Eterm List)
{
    int max_iter = 10 * CONTEXT_REDS;
    Sint pos;
    Eterm term;

    if (!is_small(Pos) || (pos = signed_val(Pos)) < 1) {
	BIF_ERROR(p, BADARG);
    }

    if (is_small(Key)) {
	double float_key = (double) signed_val(Key);

	while (is_list(List)) {
	    if (--max_iter < 0) {
		BUMP_ALL_REDS(p);
		BIF_TRAP3(bif_export[Bif], p, Key, Pos, List);
	    }
	    term = CAR(list_val(List));
	    List = CDR(list_val(List));
	    if (is_tuple(term)) {
		Eterm *tuple_ptr = tuple_val(term);
		if (pos <= arityval(*tuple_ptr)) {
		    Eterm element = tuple_ptr[pos];
		    if (Key == element) {
			return term;
		    } else if (is_float(element)) {
			FloatDef f;

			GET_DOUBLE(element, f);
			if (f.fd == float_key) {
			    return term;
			}
		    }
		}
	    }
	}
    } else if (is_immed(Key)) {
	while (is_list(List)) {
	    if (--max_iter < 0) {
		BUMP_ALL_REDS(p);
		BIF_TRAP3(bif_export[Bif], p, Key, Pos, List);
	    }
	    term = CAR(list_val(List));
	    List = CDR(list_val(List));
	    if (is_tuple(term)) {
		Eterm *tuple_ptr = tuple_val(term);
		if (pos <= arityval(*tuple_ptr)) {
		    Eterm element = tuple_ptr[pos];
		    if (Key == element) {
			return term;
		    }
		}
	    }
	}
    } else {
	while (is_list(List)) {
	    if (--max_iter < 0) {
		BUMP_ALL_REDS(p);
		BIF_TRAP3(bif_export[Bif], p, Key, Pos, List);
	    }
	    term = CAR(list_val(List));
	    List = CDR(list_val(List));
	    if (is_tuple(term)) {
		Eterm *tuple_ptr = tuple_val(term);
		if (pos <= arityval(*tuple_ptr)) {
		    Eterm element = tuple_ptr[pos];
		    if (CMP_EQ(Key, element)) {
			return term;
		    }
		}
	    }
	}
    }

    if (is_not_nil(List))  {
	BIF_ERROR(p, BADARG);
    }
    return am_false;
}
