#include "utils.h"
#include <llib/file.h>
#include <llib/template.h>
#include <sys/stat.h>
#include <sys/types.h>

// make a full path, creating dir if needed
str_t join(str_t odir, str_t tname) {
    if (odir && *odir && ! (str_eq2(tname,"./")  || tname[0] == '/')) {
        if (*odir == '/') { // absolute
            tname = file_basename(tname);
        }
        if (! file_exists(odir,"w")) {
            mkdir(odir,0777);
        }
        return str_fmt("%s/%s",odir,tname); 
    }
    return tname;
}

// append a string to another, if it exists.
 void cat(str_t* s, str_t extra) {
    if (extra && *extra) {
        str_t S = *s;
        if (S && *S) {
            *s = str_fmt("%s %s",S,extra);  // housekeeping
        } else {
            *s = extra;
        }
    } 
}

// given an array ["A","B",...] and a prefix flag like -F,
// return "-FA -FB ..."
 str_t flag_concat(str_t prefix, str_t* strings) {
    if (! strings) return str_new(""); 
    str_t* out = array_new_ref(str_t,array_len(strings));
    FOR(i,array_len(strings)) {
        out[i] = str_fmt("%s%s",prefix,strings[i]);
    }
    str_t sl = str_concat((char**)out," ");    
    str_t res = str_fmt(" %s ",sl);
    dispose(out,sl);
    return res;
}

bool str_eq2(str_t s1, str_t s2) {
    return s1[0]==s2[0] && s1[1]==s2[1];
}

// split a string separated with spaces.
// If the string is _empty_ return NULL
 str_t* split(str_t s) {
    if (s == NULL || *s == '\0')
        return NULL;
    str_t* res = (str_t*)str_split(s," ");
    if (res[0]==NULL)
        return NULL;
    return res;
}

// array as a stack, without re-allocation;
// we shrink the array size after taking the last element
 void *array_pop(void *args) {
    void **A = args;
    int size = array_len(A) - 1;
    if (size < 0) { // empty!
        return NULL;
    }
    void *res = A[size];
    // oh yes, it's an lvalue. Don't abuse the fact ;)
    array_len(A) = size;
    return res;
}

bool str2bool (str_t value) {
    return str_eq_any(value,"true","1") > 0;
}