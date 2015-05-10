#ifndef __UTILS_H
#define __UTILS_H
#include <llib/str.h>
str_t join(str_t tname, str_t odir);
 str_t flag_concat(str_t prefix, str_t* strings) ;
 str_t* split(str_t s);
 void cat(str_t *s, str_t extra);
 bool str_eq2(str_t s1, str_t s2);
 void *array_pop(void *args);
 #endif
