/*
* llib little C library
* BSD licence
* Copyright Steve Donovan, 2013
*/

/***
### A Lexical Scanner.

Lexical scanners are a smarter and cleaner alternative to the primitive `strtok` function.
Each time you call `scan_next`, the scanner finds the next _token_,

    ScanState *ts = `scan_new_from_string`("hello = (10,20,30)"));
    `scan_next`(ts);
    char *name = `scan_get_str`(ts);  // will be "hello"
    char ch = `scan_next`(ts);  // will be '='
    `scan_next`(ts);  // skip '('
    `scan_next`(ts);
    double val1 = `scan_get_number`(ts);  // 10
    `scan_next`(ts); // skip ','
    double val2 = `scan_get_number`(ts); // 20
    
At any point, `ts->type` tells you the next available token.
Note that by default this scanner ignores space.

A convenient higher-level function is `scan_scanf`;  the equivalent of above code is
simply:

    `scan_scanf`(ts,"%s %c (%f,%f",&name,&ch,&val1,&val2).

    
See `test-scan.c` for examples of various uses.

@module scan
*/

#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#define LINESIZE 256
#define STRSIZE 256

#define _INSIDE_SCAN_C \
    FILE* inf; \
    char buff[LINESIZE]; \
    char sbuff[STRSIZE]; \
    int flags; \
    int inner_flags; \
    char comment1; \
    char comment2; \
    const char *start; \
    char *P; \
    const char *start_P; \
    const char *end_P;

#include "scan.h"
#include "value.h"

#ifdef _MSC_VER
#define strtoull _strtoui64
#endif

// maximum size of an 'identifier'
#define IDENSZ 128

// a useful function for extracting a substring
static const char *copy_str(char *tok, int len, const char *start, const char *end)
{
    size_t sz = (size_t)((intptr_t)end - (intptr_t)start);
    if (sz > len)
        sz = len;
    strncpy(tok,start,sz);
    tok[sz] = '\0';
    return tok;
}

static long long convert_int(const char *buff, int base)
{
    char endptr[10];
    long long val = strtoull(buff,(char **)&endptr,base);
    return val;
}

static void reset(ScanState* ts)
{
    ts->line = 0;
    scan_set_str(ts,ts->buff);
}

/// Scanner type.
// @int line current line in file, if not just parsing a string.
// @int type One of the following:
//
//   T_END, T_EOF=0,
//   T_TOKEN, T_IDEN=1,
//   T_NUMBER,
//   T_STRING,
//   T_CHAR,
//   T_NADA
//
// @int int_type One of
//
//   T_DOUBLE,
//   T_INT,
//   T_HEX,
//   T_OCT,
//
// @table @ScanState

ScanState* scan_create(ScanState *ts)
{
    ts->inf = NULL;
    reset(ts);
    ts->flags = 0;
    ts->type = T_NADA;
    ts->inner_flags = 0;
    return ts;
}

// intialize the scanner with a text buffer.
void scan_set_str(ScanState* ts, const char *str)
{
    ts->start =ts->start_P = str;
    ts->P = (char *)str; // hack
    ts->type = T_NADA;
}

/// set flags.
//
//  - `C_IDEN` words may contain underscores
//  - `C_NUMBER` instead of `T_NUMBER`, return `T_INT`,`T_HEX` and `T_DOUBLE`
//  - `C_STRING` parse C string escapes
//  - `C_WSPACE` don't skip whitespace
//
// @within Configuration
//
void scan_set_flags(ScanState* ts, int flags)
{
    ts->flags = flags;
}

/// line comment (either one or two characters).
// @within Configuration
void scan_set_line_comment(ScanState *ts, const char *cc) {
    ts->comment1 = cc[0];
    ts->comment2 = cc[1];
}

static bool scan_init(ScanState* ts, FILE* inf)
{
    ts->inf = inf;
    *ts->buff = 0;
    reset(ts);
    return true;
}

// private inner flag values.
enum { OWNS_STREAM = 2, FORCE_LINE_MODE = 8, RETURN_OLD_VALUE = 16};

typedef enum {
    SCAN_STREAM,
    SCAN_FILENAME,
    SCAN_STRING
} ScanArgType;

// Will close the stream if the scanner owned it.
static void scan_free(ScanState *ts) {
    if (ts->inner_flags & OWNS_STREAM)
        fclose(ts->inf);
}

// constructor, from a string/stream `stream` with a `type`.
// Types may be `SCAN_STRING` (argument is that string),
// `SCAN_STREAM` (argument is `FILE *`) or
// `SCAN_FILENAME` (argument is filename)
static ScanState *scan_new(const void *stream, ScanArgType type)
{
    ScanState *st = obj_new(ScanState,scan_free);
    scan_create(st);
    if (type == SCAN_STRING) {
        scan_set_str(st,(const char*)stream);
    } else {
        FILE *inf;
        if (type == SCAN_STREAM) {
            inf = (FILE *)stream;
        } else {
            inf = fopen((const char*)stream,"r");
            if (! inf) {
                obj_unref(st);
                return NULL;
            }
            st->inner_flags |= OWNS_STREAM;
        }
        scan_init(st,inf);
    }
    return st;
}

/// scanner from a string.
// @within Constructing
ScanState *scan_new_from_string(const char *str) {
    return scan_new(str,SCAN_STRING);
}

/// scanner from a file.
// @within Constructing
ScanState *scan_new_from_file(const char *fname) {
    return scan_new(fname,SCAN_FILENAME);
}

/// scanner from an existing file stream.
// @within Constructing
ScanState *scan_new_from_stream(FILE *stream) {
    return scan_new(stream,SCAN_STREAM);
}

/// fetch a new line from the stream, if defined.
// Advances the line count - not used if the scanner has
// been given a string directly.
// @within Grabbing
bool scan_fetch_line(ScanState* ts, int skipws)
{
    do {
        if (! ts->inf) return false;
        if (fgets(ts->buff,LINESIZE,(FILE*)ts->inf) == NULL)
            return false;
        if (feof((FILE*)ts->inf)) return false;
        ++ts->line;
        scan_set_str(ts,ts->buff);
        if (skipws) scan_skip_space(ts);
    } while (*ts->P == 0);
    return true;
}

/// get the next character.
// @within Grabbing
char scan_getch(ScanState* ts)
{
    if (*ts->P == '\0') scan_fetch_line(ts,false);
    return *ts->P++;
}

/// Move the scan reader position directly with an offset.
// @within Grabbing
void scan_advance(ScanState* ts, int offs)
{
    ts->P += offs;
}

/// look at character ahead
// @within Grabbing
char scan_peek(ScanState *ts, int offs) {
    return ts->P[offs];
}

/// grab a string upto (but not including) a final target string.
// Advances the scanner (use `scan_advance` with negative offset to back off)
// @within Grabbing
int scan_get_upto(ScanState* ts, const char *target, char *buff, int bufsz)
{
    int i = 0;
    const char *T = target;
    while (true) {
        char ec = *T++, ch;
        if (ec == '\0')
            break;
        while ((ch=scan_getch(ts)) != ec) {
            buff[i++] = ch;
            if (i == bufsz) return -1;
        }
        buff[i++] = ec;
    }
    i -= strlen(target);
    buff[i] = '\0';
    return i;
}

/// tell the scanner not to grab the next line automatically.
// @within Configuration
void scan_force_line_mode(ScanState* ts)
{
    ts->inner_flags |= FORCE_LINE_MODE;
}

/// skip white space, reading new lines if necessary.
// @within Skipping
bool scan_skip_whitespace(ScanState* ts)
{
    bool skipws = ! (ts->flags & C_WSPACE);
top:
    if (skipws)
        scan_skip_space(ts);
    if (*ts->P == 0) {
        if (ts->inner_flags & FORCE_LINE_MODE) {
            ts->inner_flags ^= FORCE_LINE_MODE;
            return false;
        }
        if (! scan_fetch_line(ts,skipws)) return false; // EOF will pass through as T_END
        goto top;
    }
    return true;
}

/// skip white space and single-line comments.
// @within Skipping
void scan_skip_space(ScanState* ts)
{
    while(*ts->P && isspace(*ts->P)) ts->P++;
    if (ts->comment1 && *ts->P == ts->comment1 && (! ts->comment2 || *(ts->P+1) == ts->comment2)) {
        *ts->P = '\0';
    }
}

/// skip digits.
// @within Skipping
void scan_skip_digits(ScanState* ts)
{
    while(isdigit(*ts->P)) ts->P++;
}

/// tell the scanner not to advance on following @{scan_next}.
// @within Configuration
void scan_push_back(ScanState* ts)
{
    ts->inner_flags |= RETURN_OLD_VALUE;
}

/// advance to the next token.
// Usually this skips whitespace, and single-line comments if defined.
// @within Scanning
ScanTokenType scan_next(ScanState* ts)
{
    if (ts->inner_flags & RETURN_OLD_VALUE) {
        ts->inner_flags ^= RETURN_OLD_VALUE;
        return ts->type;
    }
    int c_parsefloat = ! (ts->flags & C_NOFLOAT);
    if (! scan_skip_whitespace(ts)) return ts->type=T_END;  // means: finis, end of file, bail out.
    char ch = *ts->P;
    int c_iden = ts->flags & C_IDEN;
    if (isalpha(ch) || (ch == '_' && c_iden)) { //--------------------- TOKENS --------------
        ts->start_P = ts->P;
        while (isalnum(*ts->P) || (*ts->P == '_' && c_iden)) ts->P++;
        ts->end_P = ts->P;
        return (ts->type = T_TOKEN);
    } else //------- NUMBERS ------------------
    if (isdigit(ch)  || (c_parsefloat && ch == '-' && isdigit(*(ts->P+1)))) {
        int c_num = ts->flags & C_NUMBER;
        ts->type = T_NUMBER;
        ts->int_type = T_INT;
        ScanTokenType ntype = ts->int_type;
        ts->start_P = ts->P;
        if (*ts->P != '.') {
            if (*ts->P == '0' && c_num) {
                if (*(ts->P+1) == 'x') {       // hex constant
                    while (isxdigit(*ts->P)) ts->P++;
                    ntype = T_HEX;
                } else
                if (isdigit(*(ts->P+1))) {      // octal constant
                    scan_skip_digits(ts);
                    ntype = T_OCT;
                } else {
                    scan_skip_digits(ts);         // plain zero!
                }
            } else {
                ts->P++;                        // skip first - might be '-'
                scan_skip_digits(ts);
            }
        }
        if (c_parsefloat)  {
            if (*ts->P == '.') {               // (opt) fractional part
                ts->P++;
                scan_skip_digits(ts);
                ntype = T_DOUBLE;
            }
            if (*ts->P == 'e' || *ts->P == 'E') { // (opt) exponent part
                ts->P++;
                if (*ts->P == '+' || *ts->P == '-') ts->P++;  // (opt) exp sign
                scan_skip_digits(ts);
                ntype = T_DOUBLE;
            }
        }
        ts->end_P = ts->P;
        ts->int_type = ntype;
        return ts->type = (c_num ? ntype : T_NUMBER);
    } else
    if (ch == '\"' || ch == '\'') { //------------CHAR OR STRING CONSTANT-------
        char *p = ts->sbuff;
        char ch, endch = *ts->P++;
        int c_str = ts->flags & C_STRING;
        ts->start_P = ts->sbuff;
        if (ts->flags & C_STRING_QUOTE)
            *p++ = endch;

        while (*ts->P && *ts->P != endch) {
            if (*ts->P == '\\' && c_str) {
                ts->P++;
                switch(*ts->P) {
                case '\\': ch = '\\'; break;
                case 'n':  ch = '\n'; break;
                case 'r':  ch = '\r'; break;
                case 't':  ch = '\t'; break;
                case 'b':  ch = '\b'; break;
                case '\"': ch = '\"'; break;
                case '\'': ch = '\''; break;
                case '0': case 'x': { //..collecting OCTAL or HEX constant
                    char obuff[10];
                    const char *start = ts->P;
                    bool hex = *start == 'x';
                    if (hex) {
                        ++start; // off 'x'
                        while (isxdigit(*ts->P)) ts->P++;
                    } else {
                        scan_skip_digits(ts);
                    }
                    copy_str(obuff,sizeof(obuff),start,ts->P);
                    ch = (char)convert_int(obuff,hex ? 16 : 8);
                    ts->P--;  // leave us on last digit
                } break;
                default: *p++ = '\\'; ch = *ts->P; break;
                } // switch
                *p++ = ch;
                ts->P++;
            } else {
                *p++ = *ts->P++;
            }
        }
        if (! *ts->P)
            return ts->type=T_END;
        if (ts->flags & C_STRING_QUOTE)
            *p++ = endch;
        ts->P++;  // skip the endch
        *p = '\0';
        ts->end_P = p;
        return ts->type = (endch == '\"' || ! c_str) ? T_STRING : T_CHAR;
    } else { // this is to allow us to use get_str() for ALL token types
        ts->start_P = ts->P;
        ts->P++;
        ts->end_P = ts->P;
        return ts->type = (ScanTokenType)ch;
    }
}

/// copy the current token to a buff.
// @within Getting
char *scan_get_tok(ScanState* ts, char *tok, int len)
{
    copy_str(tok,len,ts->start_P,ts->end_P);
    return tok;
}

/// get current token as string.
// @within Getting
char *scan_get_str(ScanState* ts)
{
    char buff[STRSIZE];
    scan_get_tok(ts, buff, STRSIZE);
    return str_new(buff);
}

#define str_eq(s1,s2) (strcmp((s1),(s2))==0)

/// Formatted reading from the scanner, like `scanf`.
// Flags start with '%', and '%%' encodes a literal '%'.
//
//  * `v` value
//  * `s` identifier
//  * `l` rest of line
//  * `q` quoted string
//  * `i` int
//  * `f` double
//  * `c` char
//  * `.` don't care!
//
// @within Getting
bool scan_scanf(ScanState* ts, const char *fmt,...)
{
    va_list ap;
    va_start(ap,fmt);
    void *P;
    while (true) {
        char f = *fmt++;
        if (f == '\0')
            return true;
        #define CAST(T,P) *((T*)P)
        if (f == '%') {
            P = va_arg(ap,void*);
            char F = *fmt++;
            switch(F) {
            case 'v':  {// value
                ValueType vt;
                scan_next(ts);
                scan_get_tok(ts,ts->sbuff,STRSIZE);
                char *str = ts->sbuff;
                if (ts->type == T_NUMBER) {
                    vt = (ts->int_type == T_INT) ? ValueInt : ValueFloat;
                } else {
                    vt = ValueString;
                    if (ts->type == T_IDEN) {
                        if (str_eq(str,"null"))
                            vt = ValueNull;
                        else if (str_eq(str,"true") || str_eq(str,"false"))
                            vt = ValueBool;
                    }
                }
                CAST(PValue,P) = value_parse(str,vt);
            } break;

            case 's': // identifier
                if (scan_next(ts) != T_IDEN)
                    return false;
                CAST(char*,P) = scan_get_str(ts);
                break;
            case 'l': // rest of line
                scan_get_line(ts,ts->sbuff,STRSIZE);
                CAST(char*,P) = str_new(ts->sbuff);
                break;
            case 'q': // quoted string
                if (scan_next(ts) != T_STRING)
                    return false;
                CAST(char*,P) = scan_get_str(ts);
                break;
            case 'd':  // integer
                if (scan_next(ts) != T_NUMBER)
                    return false;
                CAST(int,P) = (int)scan_get_number(ts);
                break;
            case 'f':  // float
                if (scan_next(ts) != T_NUMBER)
                    return false;
                CAST(double,P) = scan_get_number(ts);
                break;
            case 'c': // 'character'
                if (scan_next(ts) < T_NADA)
                    return false;
                CAST(char,P) = (char)ts->type;
                break;
            case '%': // literal %
                if (scan_getch(ts) != '%')
                    return false;
                break;
            case '!': { // parse function + value
                ScanfFun fn = (ScanfFun)P;
                P = va_arg(ap,void*);
                scan_next(ts);
                CAST(void*,P) = fn(ts);
            } break;
            case '.':  // I don't care!
                scan_next(ts);
                break;
            }
            #undef CAST
        } else
        if (isspace(f)) {
            // do nothing
        } else {
            if (scan_getch(ts) != f)
                return false;
        }
    }
    return true;
}


/// get the rest of the current line.
// This trims any leading whitespace.
// @within Getting
char *scan_get_line(ScanState *ts, char *buff, int len)
{
    scan_skip_space(ts);
    char *P = ts->P;
    ts->start_P = P;
    while (*P && *P != '\n')
        ++P;
    ts->end_P = P;
    if (*P == '\n') {
        ++P;
    }
    ts->P = P;
    return scan_get_tok(ts,buff,len);
}

/// fetch the next line and force line mode.
// After this, the scanner will regard end-of-line as end of input.
// @within Getting
const char *scan_next_line(ScanState *ts)
{
    if (! scan_fetch_line(ts,true)) return NULL;
    scan_force_line_mode(ts);
    return ts->buff;
}

/// get the current token as a number.
// @within Getting
double scan_get_number(ScanState* ts)
{
    char buff[60];
    const char* s = scan_get_tok(ts,buff,sizeof(buff));
    if (ts->int_type == T_DOUBLE) {
        return atof(s);
    } else {
        return convert_int(s,ts->int_type == T_INT ? 10 : 16);
    }
}

/// skip until a token is found with `type`.
// May return `false` if the scanner ran out.
// @within Skipping
bool scan_skip_until(ScanState *ts, ScanTokenType type)
{
    while (ts->type != type && ts->type != T_END) {
        scan_next(ts);
    }
    if (ts->type == T_END) return false;  // ran out of stream
    return true;
}

/// fetch the next number, skipping any other tokens.
// @within Skipping
bool scan_next_number(ScanState *ts, double *val)
{
    if (! scan_skip_until(ts,T_NUMBER)) return false;
    *val = scan_get_number(ts);
    scan_next(ts);
    return true;
}

/// fetch the next word, skipping other tokens.
// @within Skipping
char *scan_next_iden(ScanState *ts, char *buff, int len)
{
    char *res;
    if (! scan_skip_until(ts,T_TOKEN))
        return NULL;
    res = scan_get_tok(ts,buff,len);
    if (res != NULL)
        scan_next(ts);
    return res;
}

/// fetch the next item, skipping other tokens.
// @within Skipping
bool scan_next_item(ScanState *ts, ScanTokenType type, char *buff, int sz)
{
   if (! scan_skip_until(ts,type)) return false;
    scan_get_tok(ts, buff, sz);
    scan_next(ts);
    return true;
}

/// grab up to `sz` numbers from the stream.
// @{scan_next_line} can be used to limit this to the current line only.
// @within Grabbing
int scan_numbers(ScanState *ts, double *values, int sz)
{
    int i = 0;
    while (scan_next_number(ts,&values[i]) && i < sz) {
        ++i;
    }
    return i;
}

void scan_numbers_fun(ScanState *ts, ScanNumberFun fn, void *data)
{
    double x;
    while (scan_next_number(ts,&x)) {
        fn(data,x);
    }
}

void scan_iden_fun(ScanState *ts, ScanStringFun fn, void *data)
{
    char buff[IDENSZ];
    while (scan_next_iden(ts,buff,IDENSZ)) {
        fn(data,buff);
    }
}

