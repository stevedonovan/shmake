/*
* llib little C library
* BSD licence
* Copyright Steve Donovan, 2013
*/

/***
### Defining and Using Interfaces.

llib has a mechanism for types to implement _interfaces_. For instance,
`list` objects implement the Iterable` interface, which has a function `init`
that returns an 'Iterator` object.

```C
    #include <llib/interface.h"
    ...
    List *ls = list_new_str();
    list_add(ls, "ein");
    list_add(ls, "zwei");
    list_add(ls, "drei");
    Iterator *it = interface_get_iterator(ls);
    char *s;
    while (it->next(it,&s)) {
        printf("got '%s'\n",s);
    }
    unref(it);
```

There is an optional function `nextpair` in the `Iterator` struct,  which grabs key/value
pairs:

```C
    Map *m = map_new_str_str();
    map_put(m,"one","1");
    map_put(m,"two","2");
    map_put(m,"three","3");
    Iterator *it = interface_get_iterator(m);
    char *key, *val;
    while (it->nextpair(it,&key,&val)) {
        printf("'%s': '%s'\n",key,val);
    }
```

`Accessible` is simpler; the type provides a single `lookup` function for finding
the value associated with a key;  `map` implements this.

This module also defines default iterators and accessors for arrays.
In this way, the JSON generator and the `template` module may consume arbitrary llib
data.


@module interface
*/

#include <stdlib.h>
#define _LLIB_EXPOSE_OBJTYPE
#include "interface.h"
#include "str.h"

/// an interface for iterating over the elements of an object.
// @typename Iterable

/// an interface for returning a lookup function for a map-like object.
// @typename Accessor

/// make a type `type` support a particular interface `itype`.
void interface_add(int itype, int type, void *funs) {
    ObjType* t_inter = obj_type_from_index(itype);
    ObjType* t_obj = obj_type_from_index(type);
    int n = 1;
    InterfaceS **iface = &t_obj->interfaces;
    if (*iface == NULL) {
        *iface = (InterfaceS*)malloc(2*sizeof(InterfaceS));
    } else {
        for (InterfaceS *ii = *iface; ii->type; ++ii)
            ++n;
        *iface = (InterfaceS*)realloc(*iface,(n+1)*sizeof(InterfaceS));
    }
    (*iface)[n-1].type = t_inter->idx;
    (*iface)[n-1].interface = funs;
    (*iface)[n].type = 0;
}

/// find the interface `type` for this object.
void* interface_get(int itype, const void *obj) {
    ObjType* t_iter = obj_type_from_index(itype);
    int idx = t_iter->idx;
    ObjType* t_obj = obj_type_from_index(obj_type_index(obj));
    if (! t_obj || ! t_obj->interfaces)
        return NULL;
    for (InterfaceS *ii = t_obj->interfaces; ii->type; ++ii) {
        if (ii->type == idx)
            return ii->interface;
    }
    return NULL;
}

// iterating over arrays of pointers

typedef struct ArrayIter_ ArrayIter;

struct ArrayIter_ {
    bool (*next)(ArrayIter *ai, void *pval);
    bool (*nextpair)(Iterator *iter, void *pkey, void *pval); // optional
    int len;
    int n;
    void **P;
};

static bool array_next(ArrayIter *ai, void *pval) {
    if (ai->n == 0)
        return false;
    --ai->n;
    *((void**)pval) = *(ai->P)++;
    return true;
}

// over keys of simple maps
static bool smap_next(ArrayIter *ai, void *pval) {
    if (ai->n == 0)
        return false;
    ai->n -= 2;
    *((void**)pval) = *(ai->P)++;
    ++ai->P;
    return true;
}

// over key/value values of simple maps
static bool smap_nextpair(Iterator *iter, void *pkey, void *pval) {
    ArrayIter* ai = (ArrayIter*)iter;
    if (ai->n == 0)
        return false;
    ai->n -= 2;
    *((void**)pkey) = *(ai->P)++;
    *((void**)pval) = *(ai->P)++;
    return true;
}

static ArrayIter *array_iter(void **A, int n) {
    ArrayIter *ai = obj_new(ArrayIter,NULL);
    ai->nextpair = NULL;
    ai->len = n;
    ai->n = n;
    ai->P = A;
    return ai;
}

static Iterator* smap_init (const void *o) {
    ArrayIter *ai = array_iter((void**)o,array_len(o));
    ai->next = smap_next;
    ai->nextpair = smap_nextpair;
    ai->len = array_len(o)/2;
    return (Iterator*)ai;
}

static Iterable smap_i = {
    smap_init
};

static Accessor smap_a = {
    (ObjLookup)str_lookup,
};

static bool initialized;
static int t_accessor, t_iterable;
int obj_typeof_(const char *name);

static void initialize() {
    if (initialized) return;
    initialized = true;
    t_accessor = obj_new_type(Accessor,NULL);
    t_iterable = obj_new_type(Iterable,NULL);
    interface_add(t_accessor,OBJ_KEYVALUE_T,&smap_a);
    interface_add(t_iterable,OBJ_KEYVALUE_T,&smap_i);
}

/// type of an interface
// @param name
// @treturn int type index
// @name interface_typeof
int interface_typeof_(const char *tname) {
    initialize();
    return obj_typeof_(tname);
}

static bool is_pointer_array(const void *obj) {
    return obj_is_array(obj) && obj_elem_size(obj) == sizeof(void*);
}

/// get a lookup function for this object.
// Will look for 'Accessor', otherwise returns 'str_lookup'
ObjLookup interface_get_lookup(const void *P) {
    initialize();
    Accessor* a = (Accessor*)interface_get(t_accessor,P);
    if (a) {
        return a->lookup;
    } else {
        return (ObjLookup)str_lookup;
    }
}

/// get an iterator for this object.
// Will look for 'Iterable', otherwise returns an array iterator
Iterator* interface_get_iterator(const void *obj) {
    initialize();
    Iterable* ii = NULL;
    bool ours = obj_refcount(obj) != -1;
    if (ours) {
        ii = (Iterable*)interface_get(t_iterable,obj);
    }
    if (! ii && is_pointer_array(obj)) {
        void **A = (void**)obj;
        int n = 0;
        if (! ours) { // assume it's a NULL-terminated array
            for (void** a = A; *a; ++a)
                ++n;
        } else {
            n = array_len(obj);
        }
        ArrayIter *ai = array_iter(A,n);
        ai->next = array_next;
        ai->len = n;
        return (Iterator*)ai;
    }
    if (! ii)
        return NULL;
    return ii->init(obj);
}

