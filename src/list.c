#include <ctype.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <zlib.h>

#include "mapper_internal.h"

/* Some useful local list functions. */

/*
 *   Note on the trick used here: Presuming that we can have lists as the result
 * of a search query, we need to be able to return a linked list composed of
 * pointers to arbitrary items.  However a very common operation will be to walk
 * through all the entries.  We prepend a header containing a pointer to the
 * next item and a pointer to the current item, and return the address of the
 * current pointer.
 *   In the normal case, the address of the self-pointer can therefore be used
 * to locate the actual entry, and we can walk along the actual list without
 * needing to allocate any memory for a list header.  However, in the case where
 * we want to walk along the result of a query, we can allocate a dynamic set of
 * list headers, and still have 'self' point to the actual item.
 *   Both types of queries can use the returned double-pointer as context for
 * the search as well as for returning the desired value. This allows us to
 * avoid requiring the user to manage a separate iterator object.
 */

typedef enum {
    OP_UNION,
    OP_INTERSECTION,
    OP_DIFFERENCE
} binary_op_t;

typedef enum {
    QUERY_STATIC,
    QUERY_DYNAMIC
} query_type_t;

typedef struct {
    void *next;
    void *self;
    void *start;
    struct _query_info *query_context;
    query_type_t query_type;
    int data[1]; // stub
}  mapper_list_header_t;

/*! Function for query comparison */
typedef int query_compare_func_t(const void *context_data, const void *item);

/*! Function for freeing query context */
typedef void query_free_func_t(mapper_list_header_t *lh);

/*! Function for handling parallel queries. */
static int cmp_parallel_query(const void *context_data, const void *dev);

/*! Contains some function pointers and data for handling query context. */
typedef struct _query_info {
    unsigned int size;
    query_compare_func_t *query_compare;
    query_free_func_t *query_free;
    int data[0]; // stub
} query_info_t;

#define LIST_HEADER_SIZE (sizeof(mapper_list_header_t)-sizeof(int[1]))

/*! Reserve memory for a list item.  Reserves an extra pointer at the
 *  beginning of the structure to allow for a list pointer. */
static mapper_list_header_t* mapper_list_new_item(size_t size)
{
    mapper_list_header_t *lh=0;

    // make sure the compiler is doing what we think it's doing with
    // the size of mapper_list_header_t and location of data
    die_unless(LIST_HEADER_SIZE == sizeof(void*)*4 + sizeof(query_type_t),
               "unexpected size for mapper_list_header_t");
    die_unless(((char*)&lh->data - (char*)lh) == LIST_HEADER_SIZE,
               "unexpected offset for data in mapper_list_header_t");

    size += LIST_HEADER_SIZE;
    lh = calloc(1, size);
    if (!lh)
        return 0;

    lh->self = lh->start = &lh->data;
    lh->query_type = QUERY_STATIC;

    return (mapper_list_header_t*)&lh->data;
}

/*! Get the list header for memory returned from mapper_list_new_item(). */
static mapper_list_header_t* mapper_list_header_by_data(const void *data)
{
    return (mapper_list_header_t*)(data - LIST_HEADER_SIZE);
}

/*! Get the list header for memory returned from mapper_list_new_item(). */
static mapper_list_header_t* mapper_list_header_by_self(void *self)
{
    mapper_list_header_t *lh=0;
    return (mapper_list_header_t*)(self - ((void*)&lh->self - (void*)lh));
}

void *mapper_list_from_data(const void *data)
{
    if (!data)
        return 0;
    mapper_list_header_t* lh = mapper_list_header_by_data(data);
    die_unless(lh->self == &lh->data, "bad self pointer in list structure");
    return &lh->self;
}

/*! Set the next pointer in memory returned from mapper_list_new_item(). */
static void mapper_list_set_next(void *mem, void *next)
{
    mapper_list_header_by_data(mem)->next = next;
}

/*! Get the next pointer in memory returned from mapper_list_new_item(). */
static void* mapper_list_next_internal(void *mem)
{
    return mapper_list_header_by_data(mem)->next;
}

/*! Prepend an item to the beginning of a list. */
static void *mapper_list_prepend_item(void *item, void **list)
{
    mapper_list_set_next(item, *list);
    *list = item;
    return item;
}

void *mapper_list_add_item(void **list, size_t size)
{
    mapper_list_header_t* lh = mapper_list_new_item(size);
    mapper_list_prepend_item(lh, list);
    return lh;
}

/*! Remove an item from a list but do not free its memory. */
void mapper_list_remove_item(void **head, void *item)
{
    void *prev_node = 0, *node = *head;
    while (node) {
        if (node == item)
            break;
        prev_node = node;
        node = mapper_list_next_internal(node);
    }

    if (!node)
        return;

    if (prev_node)
        mapper_list_set_next(prev_node, mapper_list_next_internal(node));
    else
        *head = mapper_list_next_internal(node);
}

/*! Free the memory used by a list item */
void mapper_list_free_item(void *item)
{
    if (item)
        free(mapper_list_header_by_data(item));
}

/** Structures and functions for performing dynamic queries **/

/* Here are some generalized routines for dealing with typical context
 * format and query continuation. Functions specific to particular
 * queries are defined further down with their compare operation. */

void **mapper_list_query_continuation(mapper_list_header_t *lh)
{
    void *item = mapper_list_header_by_data(lh->self)->next;
    while (item) {
        if (lh->query_context->query_compare(&lh->query_context->data, item))
            break;
        item = mapper_list_next_internal(item);
    }

    if (item) {
        lh->self = item;
        return &lh->self;
    }

    // Clean up
    if (lh->query_context->query_free)
        lh->query_context->query_free(lh);
    return 0;
}

static void free_query_single_context(mapper_list_header_t *lh)
{
    if (lh->query_context->query_compare == cmp_parallel_query) {
        // this is a parallel query – we need to free components also
        void *data = &lh->query_context->data;
        mapper_list_header_t *lh1 = *(mapper_list_header_t**)data;
        mapper_list_header_t *lh2 = *(mapper_list_header_t**)(data+sizeof(void*));
        free_query_single_context(lh1);
        free_query_single_context(lh2);
    }
    free(lh->query_context);
    free(lh);
}

static int get_query_size(const char *types, va_list aq)
{
    if (!types)
        return 0;

    int i = 0, j, size = 0, num_args;
    while (types[i]) {
        switch (types[i]) {
            case MAPPER_INT32:
            case MAPPER_CHAR: // store char as int to avoid alignment problems
                if (types[i+1] && isdigit(types[i+1])) {
                    num_args = atoi(types+i+1);
                    va_arg(aq, int*);
                    ++i;
                }
                else {
                    num_args = 1;
                    va_arg(aq, int);
                }
                size += num_args * sizeof(int);
                break;
            case MAPPER_INT64:
                if (types[i+1] && isdigit(types[i+1])) {
                    num_args = atoi(types+i+1);
                    va_arg(aq, int64_t*);
                    ++i;
                }
                else {
                    num_args = 1;
                    va_arg(aq, int64_t);
                }
                size += num_args * sizeof(int64_t);
                break;
            case MAPPER_STRING:
                if (types[i+1] && isdigit(types[i+1])) {
                    num_args = atoi(types+i+1);
                    const char **val = va_arg(aq, const char**);
                    for (j = 0; j < num_args; j++)
                        size += strlen(val[j]) + 1;
                    ++i;
                }
                else {
                    const char *val = va_arg(aq, const char*);
                    size += (val ? strlen(val) : 0) + 1;
                }
                break;
            case MAPPER_PTR:
                // void ptr
                if (types[i+1] && isdigit(types[i+1])) {
                    num_args = atoi(types+i+1);
                    va_arg(aq, void**);
                    ++i;
                }
                else {
                    num_args = 1;
                    va_arg(aq, void**);
                }
                size += num_args * sizeof(void**);
                break;
            default:
                va_end(aq);
                return 0;
        }
        ++i;
    };
    va_end(aq);
    return size;
}

/* We need to be careful of memory alignment here - for now we will just ensure
 * that string arguments are always passed last. */
static void **new_query_internal(const void *list, int size,
                                 const void *compare_func, const char *types,
                                 va_list aq)
{
    if (!list || !size || !compare_func || !types)
        return 0;

    mapper_list_header_t *lh = (mapper_list_header_t*)malloc(LIST_HEADER_SIZE);
    lh->next = mapper_list_query_continuation;
    lh->query_type = QUERY_DYNAMIC;

    int i = 0, j, num_args;

    lh->query_context = (query_info_t*)malloc(sizeof(query_info_t)+size);

    char *d = (char*)&lh->query_context->data;
    int offset = 0;
    i = 0;
    while (types[i]) {
        switch (types[i]) {
            case MAPPER_INT32:
            case MAPPER_CHAR: // store char as int to avoid alignment problems
                if (types[i+1] && isdigit(types[i+1])) {
                    // is array
                    num_args = atoi(types+i+1);
                    int *val = (int*)va_arg(aq, int*);
                    memcpy(d+offset, val, sizeof(int) * num_args);
                    ++i;
                }
                else {
                    num_args = 1;
                    int val = (int)va_arg(aq, int);
                    memcpy(d+offset, &val, sizeof(int));
                }
                offset += sizeof(int) * num_args;
                break;
            case MAPPER_INT64: {
                if (types[i+1] && isdigit(types[i+1])) {
                    // is array
                    num_args = atoi(types+i+1);
                    int64_t *val = (int64_t*)va_arg(aq, int64_t*);
                    memcpy(d+offset, val, sizeof(int64_t) * num_args);
                    ++i;
                }
                else {
                    num_args = 1;
                    int64_t val = (int64_t)va_arg(aq, int64_t);
                    memcpy(d+offset, &val, sizeof(int64_t));
                }
                offset += sizeof(int64_t) * num_args;
                break;
            }
            case MAPPER_STRING:
                if (types[i+1] && isdigit(types[i+1])) {
                    // is array
                    num_args = atoi(types+i+1);
                    const char **val = (const char**)va_arg(aq, const char**);
                    for (j = 0; j < num_args; j++) {
                        snprintf(d+offset, size-offset, "%s", val[j]);
                    }
                    offset += strlen(val[j]) + 1;
                    ++i;
                }
                else {
                    const char *val = (const char*)va_arg(aq, const char*);
                    snprintf(d+offset, size-offset, "%s", val);
                    offset += (val ? strlen(val) : 0) + 1;
                }
                break;
            case MAPPER_PTR: {
                if (types[i+1] && isdigit(types[i+1])) {
                    // is array
                    num_args = atoi(types+i+1);
                    ++i;
                }
                else {
                    num_args = 1;
                }
                void *val = va_arg(aq, void**);
                memcpy(d+offset, val, sizeof(void*) * num_args);
                offset += sizeof(void**) * num_args;
                break;
            }
            default:
                va_end(aq);
                free(lh->query_context);
                free(lh);
                return 0;
        }
        ++i;
    }

    va_end(aq);

    lh->query_context->size = sizeof(query_info_t)+size;
    lh->query_context->query_compare = (query_compare_func_t*)compare_func;
    lh->query_context->query_free = (query_free_func_t*)free_query_single_context;

    lh->self = lh->start = (void*)list;

    // try evaluating the first item
    if (lh->query_context->query_compare(&lh->query_context->data, list))
        return &lh->self;

    return mapper_list_query_continuation(lh);
}

void **mapper_list_new_query(const void *list, const void *compare_func,
                             const char *types, ...)
{
    va_list aq;
    va_start(aq, types);
    int size = get_query_size(types, aq);
    va_start(aq, types);
    return new_query_internal(list, size, compare_func, types, aq);
}

void **mapper_list_next(void **list)
{
    if (!list) {
        trace("bad pointer in mapper_list_next()\n");
        return 0;
    }

    if (!*list) {
        trace("pointer in mapper_list_next() points nowhere\n");
        return 0;
    }

    mapper_list_header_t *lh = mapper_list_header_by_self(list);

    if (!lh->next)
        return 0;

    if (lh->query_type == QUERY_STATIC) {
        return mapper_list_from_data(lh->next);
    }
    else if (lh->query_type == QUERY_DYNAMIC) {
        /* Here we treat next as a pointer to a continuation function, so we can
         * return items from the graph computed lazily.  The context is
         * simply the string(s) to match.  In the future, it might point to the
         * results of a SQL query for example. */
        void **(*f) (mapper_list_header_t*) = lh->next;
        return f(lh);
    }
    return 0;
}

void mapper_list_free(void **list)
{
    if (!list || !*list)
        return;
    mapper_list_header_t *lh = mapper_list_header_by_self(list);
    if (lh->query_type == QUERY_DYNAMIC && lh->query_context->query_free)
        lh->query_context->query_free(lh);
}

void *mapper_list_get_index(void **list, int index)
{
    if (!list || index < 0)
        return 0;

    mapper_list_header_t *lh = mapper_list_header_by_self(list);

    if (index == 0)
        return lh->start;

    // Reset to beginning of list
    lh->self = lh->start;

    int i = 1;
    while ((list = mapper_list_next(list))) {
        if (i == index)
            return *list;
        ++i;
    }
    return 0;
}

/* Functions for handling parallel queries: unions, intersections, etc. */
static int cmp_parallel_query(const void *context_data, const void *list)
{
    mapper_list_header_t *lh1 = *(mapper_list_header_t**)context_data;
    mapper_list_header_t *lh2 = *(mapper_list_header_t**)(context_data+sizeof(void*));
    mapper_op op = *(mapper_op*)(context_data + sizeof(void*) * 2);

    query_info_t *c1 = lh1->query_context, *c2 = lh2->query_context;

    switch (op) {
        case OP_UNION:
            return (    c1->query_compare(&c1->data, list)
                    ||  c2->query_compare(&c2->data, list));
        case OP_INTERSECTION:
            return (    c1->query_compare(&c1->data, list)
                    &&  c2->query_compare(&c2->data, list));
        case OP_DIFFERENCE:
            return (    c1->query_compare(&c1->data, list)
                    && !c2->query_compare(&c2->data, list));
        default:
            return 0;
    }
}

static mapper_list_header_t *mapper_list_header_copy(mapper_list_header_t *lh)
{
    mapper_list_header_t *copy = (mapper_list_header_t*)malloc(LIST_HEADER_SIZE);
    memcpy(copy, lh, LIST_HEADER_SIZE);

    if (!lh->query_context)
        return copy;

    copy->query_context = (query_info_t*)malloc(lh->query_context->size);
    memcpy(copy->query_context, lh->query_context, lh->query_context->size);

    if (copy->query_context->query_compare == cmp_parallel_query) {
        // this is a parallel query – we need to copy components
        void *data = &copy->query_context->data;
        mapper_list_header_t *lh1 = *(mapper_list_header_t**)data;
        mapper_list_header_t *lh2 = *(mapper_list_header_t**)(data+sizeof(void*));
        lh1 = mapper_list_header_copy(lh1);
        lh2 = mapper_list_header_copy(lh2);
        memcpy(data, &lh1, sizeof(void*));
        memcpy(data+sizeof(void*), &lh2, sizeof(void*));
        lh1 = *(mapper_list_header_t**)data;
        lh2 = *(mapper_list_header_t**)(data+sizeof(void*));
    }
    return copy;
}

void **mapper_list_copy(void **list)
{
    if (!list)
        return 0;

    mapper_list_header_t *lh = mapper_list_header_by_self(list);
    mapper_list_header_t *copy = mapper_list_header_copy(lh);
    return &copy->self;
}

void **mapper_list_union(void **list1, void **list2)
{
    if (!list1)
        return list2;
    if (!list2)
        return list1;

    mapper_list_header_t *lh1 = mapper_list_header_by_self(list1);
    mapper_list_header_t *lh2 = mapper_list_header_by_self(list2);
    return mapper_list_new_query(lh1->start, cmp_parallel_query, "vvi", &lh1,
                                 &lh2, OP_UNION);
}

void **mapper_list_intersection(void **list1, void **list2)
{
    if (!list1 || !list2)
        return 0;

    mapper_list_header_t *lh1 = mapper_list_header_by_self(list1);
    mapper_list_header_t *lh2 = mapper_list_header_by_self(list2);
    return mapper_list_new_query(lh1->start, cmp_parallel_query, "vvi", &lh1,
                                 &lh2, OP_INTERSECTION);
}

void **mapper_list_filter(void **list, const void *compare_func,
                          const char *types, ...)
{
    if (!list)
        return 0;

    va_list aq;

    va_start(aq, types);
    int size = get_query_size(types, aq);

    mapper_list_header_t *lh1 = mapper_list_header_by_self(list);

    va_start(aq, types);
    void **filter = new_query_internal(lh1->start, size, compare_func, types, aq);

    if (lh1->query_type == QUERY_STATIC)
        return filter;

    // return intersection
    mapper_list_header_t *lh2 = mapper_list_header_by_self(filter);
    return mapper_list_new_query(lh1->start, cmp_parallel_query, "vvi", &lh1,
                                 &lh2, OP_INTERSECTION);
}

void **mapper_list_difference(void **list1, void **list2)
{
    if (!list1)
        return 0;
    if (!list2)
        return list1;

    mapper_list_header_t *lh1 = mapper_list_header_by_self(list1);
    mapper_list_header_t *lh2 = mapper_list_header_by_self(list2);
    return mapper_list_new_query(lh1->start, cmp_parallel_query, "vvi", &lh1,
                                 &lh2, OP_DIFFERENCE);
}

int mapper_list_length(void **list)
{
    int length = 0;
    // use a copy
    for (list = mapper_list_copy(list); list; list = mapper_list_next(list))
        ++length;
    return length;
}
