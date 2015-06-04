#include <stdio.h>
#include "shmake.h"
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <llib/file.h>
#include <llib/template.h>
#include <llib/json.h>
#include <llib/array.h>

#include "shmake.h"
#include "utils.h"

static int verbose_level;
static bool testing;
static bool quiet;
static int s_group_type;
static int s_file_type;

void shmake_flags(int v_level, bool test, bool silent) {
    verbose_level = v_level;
    testing = test;
    quiet = silent;
}

// File is a type representing a file
// We wrap to give files a distinct type and to allow for generalization!
static void File_dispose(File *f) {
    unref(f->name);
}

File *File_new(str_t name) {
    File *res = obj_new(File,File_dispose);
    if (! s_file_type) {
        s_file_type = obj_type_index(res);
    }
    res->name = str_ref(name);
    return res;
}

int File_time(File *f) {
    struct stat buf;
    int res = stat(f->name,&buf);
    if (res == 0) {
        return buf.st_mtime;
    }
    if (errno != ENOENT) {
        fprintf(stderr,"file %s ",f->name);
        perror("stat");
    }
    return 0;
}

int File_remove(File *f) {
    int res = unlink(f->name);
    if (verbose_level > 0) {
        printf("removed %s\n",f->name);
    }
    if (res != 0) {
        perror("unlink");
        return res;
    }    
    return res;
}

// can always get the strings from an array of files (or targets)
str_t* files_as_strings(File **files) {
   str_t *strings = array_new_ref(str_t,array_len(files));
    FOR(i, array_len(files)) {
        strings[i] = ref(files[i]->name);
    }
    return strings;
}

/// The Target type inherits from File, so it may be safely downcast.
// In addition, it has an array of things that this target depends on,
// the _prerequisites_ (in Make terminology).
// There is also an action, which is either a callback+data, or if the
// callback is NULL, the data is interpreted as a shell command.
// (see target_fire)

static void Target_dispose(Target *T) {
    unref(T->name);
    unref(T->prereq);
    // and data?
}

static int s_target_type;
static Target *** s_targets;

Target ** targets() {
    return *s_targets;
}

Target *target_from_file(str_t name) {
    FORA(*s_targets, if (str_eq(_->name,name)) return _);
    return NULL;
}

void target_forall(TargetCallback f) {
    FORA(*s_targets, f(_));
}

void target_set_command(Target *t, str_t cmd) {
    t->data = cmd;
}

// Used to create a named target from a command, depending on the so-called
// prerequisites. These are usually names of files or other targets, but may
// already be File* or Target* objects.

Target *target_new(str_t name, str_t *prereq, const void *data, ShmakeCallback callback) {    
    Target *T = obj_new(Target,Target_dispose);
    if (! s_target_type) {
        s_target_type = obj_type_index(T);
        s_targets = seq_new_ref(Target*);
    }
    if (target_from_file(name) != NULL) { // a warning, perhaps?
        return target_from_file(name);
    }
    T->name = str_ref(name);
    seq_add(s_targets,T);
    File **files = array_new_ref(File*,array_len(prereq));
    FOR(i,array_len(prereq)) {
        char *name = (char*)prereq[i];
        File *f = NULL;
        // prereq may contain strings (the default).
        if (obj_refcount(name) == -1 || value_is_string(name)) {
            // but we do check to see if the name refers to an existing target
            f = (File*)target_from_file(name);
            if (! f) {
                f = File_new(name);
            }
        } else {
            int type = obj_type_index(name);
            // Target is a 'subclass' of File.
            if (type == s_target_type || type == s_file_type) {
                f = (File*)name;
            } else {
                fprintf(stderr,"%d %p?\n",type,name);
            }
        }
        files [i] = f;
    }
    T->prereq = files;
    T->checked = false;
    T->callback = callback;
    T->data = data;
    T->message = NULL;
    T->type = TARGET_PHONY;
    return T;
}

Target *target_first() {
    return (*s_targets)[0];
}

// swop first & last target
void target_push_to_front(Target *t) {
    Target **targets = *s_targets;
    int idx = array_len(targets)-1;
    Target *last = targets[idx];
    targets[idx] = targets[0];
    targets[0] = last;
}

static struct {
    str_t key;
    str_t value;
}   std_map[] = {
    {"TARGET",""},
    {"INPUT",""},
    {"DEPS",""},
    {NULL,NULL}
};

// useful special case of a target, that has a command to be substituted using
// values of the name 'TARGET' and 'INPUT' which is the first input file.
// These variables must be inside @(), chosen to avoid conflicts with $ shell constructs.
Target *target(str_t name, str_t *prereq, str_t cmd) {
    Target *T = target_new(name,prereq,NULL,NULL);
    if (cmd) {
        if (strchr(cmd,'@') != NULL) { // it's a template
            std_map[0].value = T->name;  // TARGET
            if (T->prereq[0]) {
                std_map[1].value = T->prereq[0]->name; // INPUT 
                if (array_len(T->prereq)==0) { // DEPS
                    std_map[2].value = std_map[1].value;
                } else {                
                    std_map[2].value = str_concat((char**)files_as_strings(T->prereq)," ");
                }
            }  else {
                std_map[1].value = NULL;
                std_map[2].value = NULL;
            }
            StrTempl *st = str_templ_new(cmd,"@()");
            if (value_is_error(st))
                return (Target*)st;
            target_set_command(T,str_templ_subst(st, (char**)&std_map));
        } else {
            target_set_command(T,cmd);
        }
        T->type = TARGET_FILE;
    }
    return T;
}

// return all the files that our targets depends on (the 'prerequisites') as a space-separated
// string.
str_t target_depends_as_str(Target *T) {
    return str_concat((char**)files_as_strings(T->prereq)," ");    
}

// Calling the Action of a target. Can be an actual function, but usually is a command
// string. If such a target has its _message_ field set, then we printt that out rather
// than the actual command (unless verbose)
void target_fire(Target *T) {
    if (T->callback) {
        T->callback(T->data);
    } else
    if (T->data) {
        // then the target's data is a shell command line
        str_t xcmd = (str_t)T->data;
        // unlike Make, don't echo the command unless -v
        if (verbose_level > 0) {
            printf("%s\n",xcmd);
        } else // but compile/link targets have a message...
        if (T->message && ! quiet) { 
            printf("%s %s\n",T->message,T->name);
        }
        // -t for testing is useful if you just want to see what will happen with a build.
        if (! testing) {
            int res = system(xcmd);
            if (res != 0) {
                // ALWAYS echo command if it failed!
                fprintf(stderr,"%s\n",xcmd);
                if (errno != 0)
                    perror("system");
                exit(1);
            }
        }
    }
}

// the Special Sauce - checks if a target is out-of-date by checking against the times
// of its prerequisites.
bool target_check(Target *T) {
    if (! T->prereq[0]) { // unconditional action
        target_fire(T);
        return true;
    }
    if (T->checked) { // no point in rechecking targets...
        return true;
    } else {
        T->checked = true;
    }
    time_t target_time = File_time((File*)T);
    bool changed = false;
    // a list of File _or_ Target objects...
    FOR(i,array_len(T->prereq)) {
        File *f = T->prereq[i];
        if (obj_type_index(f) == s_target_type) {
            target_check((Target*)f);
        }
        time_t f_time = File_time(f);
        if (verbose_level > 1) {
            printf("! %s (%d) depends on %s (%d)\n",T->name,(int)target_time,f->name, (int)f_time);
        }
        // if we've been modified after the target, OR we don't yet exist, then fire!
        if (f_time > target_time || f_time == 0) {
            changed = true;
        }
    }
    if (changed) {
        target_fire(T);
        return true;
    }
    return false;
}

int target_remove(Target *T) {
    if (T->type == TARGET_PHONY) 
        return 0;
    File_remove((File*)T);
    if (T->type == TARGET_OBJ) {
        str_t dfile = file_replace_extension(T->name,".d");
        unlink(dfile);
    }
    return 0;
}

// Groups are a useful abstraction; a distinct type which is a list of Target objects

static void Group_dispose(Group *g) {
    unref(g->cmd);
    unref(g->targets);
}

static Group*** s_groups;
static int n_group;

Group *group_new(str_t cmd, Target **targets) {
    Group *G = obj_new(Group,Group_dispose);
    if (! s_group_type) {
        s_group_type = obj_type_index(G);
        s_groups = seq_new_ref(Group*);
    }
    G->cmd = str_ref(cmd);
    G->targets = ref(targets);
    G->name = str_fmt("*G%03d",++n_group);
    seq_add(s_groups,G);
    return G;
}

Group *group_by_name(str_t name) {
    if (! s_groups)
        return NULL;
    Group** groups = *s_groups;
    FOR(i, array_len(groups)) {
        Group *G = groups[i];
        if (G->name && str_eq(G->name,name)) {
            return G;
        }
    }
    return NULL;
}

str_t *group_expand_with_targets(str_t *prereq) {
    // a target's prequisites may contain _rule names_, which contain target
    // references. These must be expanded out.
    str_t **ss = seq_new(str_t);
    FOR(i,array_len(prereq)) {
        str_t name = prereq[i];
        Group *G = group_by_name(name);
        if (G) {
            str_t *tnames = files_as_strings((File**)G->targets);
            seq_adda(ss,tnames,-1);
        } else {
            seq_add(ss,name);
        }
    }
    //unref(prereq); refcount!
    return seq_array_ref(ss);    
}

// read the .d generated by -MMD and extract the actual list of files
// which our target obj is dependent on.
// A .d file starts with TARGET COLON followed by all the files which TARGET
// depends on. Backlashes need to be ignored.
static str_t* prereq_from_dfile (str_t dfile) {
    char *contents = file_read_all (dfile, true);
    if (! contents)
        return NULL;
    char *S = strchr(contents,':');
    if (! S) // not a .d file AT ALL
        return NULL;
    ++S;
    // replace '\\\\n' with '  '
    for (char *P=S; *P; P++) {
        if (*P=='\\' && *(P+1)=='\n') {
            *P = ' ';
            *(P+1) = ' ';
        }
    }
    return (str_t*)str_split(S," ");    
}

Group *compile_step (str_t compiler, str_t *files, str_t cflags, str_t *incdirs, str_t *defines, str_t odir) {
    files = group_expand_with_targets(files);
    
    str_t cmd = str_fmt("%s -c -Wall -MMD %s%s%s",
        compiler,cflags,flag_concat("-D",defines),flag_concat("-I",incdirs));
    
    Target **targets = array_new_ref(Target*,array_len(files));
    FOR(i,array_len(files)) {
        str_t file = files[i];
        str_t obj = file_replace_extension(join(odir,file),".o");
        str_t dfile = file_replace_extension(obj,".d");
        str_t *reqs = prereq_from_dfile(dfile);
        if (! reqs) {
            reqs  = VAS(file);
        }
        targets[i] = target_new(obj,reqs,str_fmt("%s %s -o %s",cmd,file,obj),NULL);
        targets[i]->message = "compiling";
        targets[i]->type = TARGET_OBJ;
    }
    return group_new(cmd, targets);    
}

Target *linker (str_t linker, str_t name, str_t *objs, str_t lflags, str_t *libdirs, str_t *libs, int kind) {
    // build up list of prerequisites for our target.
    // They may be GROUPS which are lists of targets
    // Typically the results are object files but could also be library files referenced directly.
    File ***deps = seq_new_ref(File*);
    FOR(i, array_len(objs)) {
        str_t file = objs[i];
        if (file==NULL) // may happen when linking straight from groups...
           continue;
        Group *G = group_by_name(file);
        if (G) {
            seq_adda(deps, G->targets, -1);
        } else {
            seq_add(deps, (File*)file);
        }        
    }
    void **pr = seq_array_ref(deps);
    Target *T = target_new(name,(str_t*)pr,"",NULL);
    str_t obj_files = target_depends_as_str(T);
    
    str_t cmd;
    if (kind == LINK_LIB) {
        cmd = str_fmt("ar rcu %s %s; ranlib %s",name,obj_files,name);
    } else {
        cmd = str_fmt("%s %s %s%s%s -o %s",
            linker,obj_files,lflags,flag_concat("-L",libdirs),flag_concat("-l",libs),name);
    }
    T->type = TARGET_PROG;
    T->message = "linking";
    target_set_command(T,cmd);
    return T;
}
