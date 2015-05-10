/*
* llib little C library
* BSD licence
* Copyright Steve Donovan, 2013
*/

#ifndef __LLIB_TEMPLATE_H
#define __LLIB_TEMPLATE_H
#include "obj.h"
#include "value.h"

struct StrTempl_;
typedef struct StrTempl_ StrTempl;
#ifndef STRLOOKUP_DEFINED
typedef char *(*StrLookup) (void *obj, const char *key);
#define STRLOOKUP_DEFINED
#endif

typedef char *(*TemplateFun)(void *arg, StrTempl *stl);
void str_templ_add_builtin(const char *name, TemplateFun fun);
void str_templ_add_macro(const char *name, StrTempl *stl, void *data);

StrTempl *str_templ_new(const char *templ, const char *markers);
char *str_templ_subst_using(StrTempl *stl, StrLookup lookup, void *data);
char *str_templ_subst(StrTempl *stl, char **substs);
char *str_templ_subst_values(StrTempl *st, PValue v);

#define str_templ_subst_map(stl,m) str_templ_subst_using(stl,(StrLookup)map_get,m)

#endif

