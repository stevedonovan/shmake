#ifndef __SHMAKE_H
#define __SHMAKE_H
#include <llib/str.h>

void shmake_flags(int v_level, bool test, bool silent);

typedef struct File_ {
    str_t name;
} File;

File *File_new(str_t name);
int File_time(File *f);
int File_remove(File *f);

str_t* files_as_strings(File **files);

typedef int (*ShmakeCallback)(const void *);

enum TargetType {TARGET_PHONY, TARGET_FILE, TARGET_OBJ, TARGET_PROG};

typedef struct Target_ {
    // File-like part
    str_t name;
    // we have _prerequistes_. If they change, we must update this target
    File **prereq;
    // don't need to check the same target twice
    bool checked;
    // we have an _action_.
    ShmakeCallback callback; // if not NULL, call with data
    const void *data; // otherwise data is a command string!
    // may have a Special Message printed out when building this target in normal mode
    str_t message;
    enum TargetType type;
} Target;

typedef int (*TargetCallback)(Target *t);

Target *target_from_file(str_t name);
void target_forall(TargetCallback f);
void target_set_command(Target *t, str_t cmd);
Target *target_new(str_t name, str_t *prereq, const void *data, ShmakeCallback callback);
Target *target_first();
void target_push_to_front(Target *t);
Target *target(str_t name, str_t *prereq, str_t cmd);

void target_fire(Target *T);

bool target_check(Target *T);

int target_remove(Target *T);

Target ** targets();

typedef struct Group_ {
    str_t cmd;
    Target **targets;
    str_t name;
} Group;

Group *group_new(str_t cmd, Target **targets);
Group *group_by_name(str_t name);
str_t *group_expand_with_targets(str_t *prereq);

enum {LINK_EXE, LINK_SO, LINK_LIB, LINK_STATIC};

Group *compile_step (str_t compiler, str_t *files, str_t cflags, str_t *incdirs, str_t *defines, str_t odir);
Target *linker (str_t linker, str_t name, File **objs, str_t lflags, str_t *libdirs, str_t *libs, int kind); 

#endif
