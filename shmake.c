/* shmake - a shell-scriptable build tool.
*  a _shmakefile_ is a shell script which sources /tmp/shmake.sh,
*  which provides the functions S (set a default), T (create a target with
*  a command) and C,Cpp (for building C/C++ projects.)
*
* It is inspired by the Lake project, but really only aims for POSIX systems.
*  'shmake -c "C hello hello.c" will create a new shmakefile for you for building a single C file.
*/
#define _BSD_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <llib/str.h>
#include <llib/array.h>
#include <llib/file.h>
#include <llib/config.h>
#include <llib/arg.h>
#include <llib/template.h>
#include <llib/json.h>

#include "shmake.h"
#include "utils.h"

#define MAX_LINE_BUFSZ 4096

static ArgState *arg_state;
static bool testing;
static  int verbose_level;
static str_t private_need_path;


#define quit(msg,...) arg_quit(arg_state,str_fmt(msg,__VA_ARGS__),false)

static void exec(str_t cmd) {
    int res = system(cmd);
    if (res != 0) {
        quit("executing '%s' failed",cmd);
    }
}

//////// Providing NEEDS ///////

// This concept is borrowed from Lake; a need is a shortcut for
// expressing the build requirements of a program/lib.

typedef struct Need_ {
    str_t name;
    str_t cflags;
    str_t lflags;
} Need;

str_t lookup_and_subst(char **cfg, str_t key) {
    char *res = str_lookup(cfg,key);
    if (! res)
        return NULL;
    StrTempl *st = str_templ_new(res,"${}");
    res = str_templ_subst(st, cfg);
    unref(st);
    return res;
}

// first, see if NEED.need exists in current dir or in ~/.shmake
// If so, then it is a property-style file that needs at least one of
// 'cflags' or 'libs' defined.
// If not, then we ask pkg-config
Need *need_from_name(str_t name) {
    Need *N = obj_new(Need,NULL);
    N->name = str_ref(name);
    str_t  nfile = str_fmt("%s.need",name);
    if (! file_exists(nfile,"r") && private_need_path) {
        nfile = str_fmt("%s/%s.need",private_need_path,name);
    }
    if (! file_exists(nfile,"r")) {
        nfile = str_fmt("%s/.shmake/%s.need",getenv("HOME"),name);
    }
    if (file_exists(nfile,"r")) {
        char **cfg = config_read(nfile);
        if (! cfg)
            return NULL;
        
        // add special HERE variable (more verbose than this should be!)
        str_t here = file_dirname(nfile);
        char path[256];
        if (getcwd(path,sizeof(path)) == NULL) {
            quit("can't get current directory; %s",strerror(errno));
        }
        here = str_fmt("%s/%s",path,here);
        str_t** ss = seq_new(str_t);
        seq_adda(ss,cfg,-1);
        seq_add(ss,"HERE");
        seq_add(ss,here);
        cfg = seq_array_ref(ss);
        
        // perform all needed ${} expansions
        for (char** P = cfg; *P; P+= 2) {
            char *value = *(P+1);
            if (str_findstr(value,"${") != -1) {
                StrTempl *st = str_templ_new(value,"${}");
                value = str_templ_subst(st, cfg);
                unref(st);
                *(P+1) = value;
            }
        }
        
        N->cflags = str_lookup(cfg,"cflags");
        N->lflags = str_lookup(cfg,"libs");
        return N;
    }
    N->cflags = file_command_fmt("pkg-config --cflags %s",name);
    N->lflags = file_command_fmt("pkg-config --libs %s",name);
    if (! (*N->cflags || *N->lflags))
        return NULL;
    return N;
}

void need_update(str_t *need_list, str_t *cflags, str_t *lflags) {
    char **cs = strbuf_new();
    char **ls = strbuf_new();        
    strbuf_addsp(cs,*cflags);
    strbuf_addsp(ls,*lflags);
    FOR (i, array_len(need_list)) {
        Need *N = need_from_name(need_list[i]);
        if (! N) {
            quit("unable to resolve need '%s'",need_list[i]);
        }
        strbuf_addsp(cs,N->cflags);
        strbuf_addsp(ls,N->lflags);
    }
    *cflags = strbuf_tostring(cs);
    *lflags = strbuf_tostring(ls);
}

/// parsing arguments passed to C[pp]
typedef struct Args_ {
    str_t include_dirs, defines, cflags, opt;
    str_t lib_dirs, libs, lflags;
    bool debug, exports;
    str_t exclude;
    str_t name;
    str_t needs;
    str_t out_extension;
    str_t output_directory;    
    str_t* files;
} Args;

static Args s_args;

// these will be set by the S command - defaults
static Args s_def;

// globals. main_args specifies the command-line options for shmake.
static str_t shmakefile;
static str_t *shmake_args;
static str_t PLAT, CC, CXX;
static str_t start_directory;
static str_t do_create;
static bool *verbose;
static bool macosx;
static bool debug;
static bool quiet;

void * main_args[] = {
    "// shmake: a simple shell-based make tool",
    "string file='shmakefile'; // -f shmakefile to run if not -c",&shmakefile,
    "string directory=''; // -C directory to switch to first",&start_directory,
    "bool testing; // -t testing mode - show commands but don't execute them",&testing,
    "bool debug; // -g build debug binaries",&debug,
    "bool verbose[]; // -v verbose output",&verbose,
    "bool quiet; // -q no output unless error",&quiet,
    "string create=''; // -c create shmakefile from statement",&do_create,
    "string #1[]; // target and VAR=VALUE assignments",&shmake_args,
    NULL
};

// the command-line options for C[pp]
void * compiler_args[] = {
    "string includes=''; // -I directories to search for include files",&s_args.include_dirs,
    "string defines=''; // -D prepro macro definitions",&s_args.defines,
    "string lib-dirs=''; // -L directories to search for libraries",&s_args.lib_dirs,
    "string libs=''; // -l libraries to link against",&s_args.libs,
    "string needs=''; // -n program needs",&s_args.needs,
    "bool debug; // -g debug build",&s_args.debug,
    "bool exports; // -e export symbols",&s_args.exports,
    "string opt='2'; // -O optimize level",&s_args.opt,
    "string exclude=''; // -x exclude files from list",&s_args.exclude,
    "string rule=''; // -R specify out extension for rule",&s_args.out_extension,
    "string output=''; // -d output directory",&s_args.output_directory,
    "string #1=''; // name of program",&s_args.name,
    "string #2[]; // source files",&s_args.files,
    NULL
};

/// arguments for 'rule'
struct RuleArgs {
    str_t name;
    str_t out_extension;
    str_t output_directory;
    str_t command;
    str_t *files;
};

static struct RuleArgs s_rule_args;

void * rule_args[] = {
   "string output=''; // -d output directory",&s_rule_args.output_directory,
   "string #1; // name of rule",&s_rule_args.name,
   "string #2; // output extension",&s_rule_args.out_extension,
   "string #3; // command",&s_rule_args.command, 
   "string #4[]; // source files",&s_rule_args.files,
   NULL
};

static void process_rule(ArgState *rule_state, str_t *args) {
    arg_reset_used(rule_state); 
    char *err = (char*)arg_process(rule_state,args-1);
    if (value_is_error(err)) {
        quit("R: %s",err);
    }            
    str_t *files = s_rule_args.files;
    str_t odir = s_rule_args.output_directory;
    int n = array_len(files);
    Target **targets = array_new(Target*,n);
    FOR(i,n) {
        str_t  tname = files[i];
        if (! str_eq(s_rule_args.out_extension,"ditto")) {
            tname = file_replace_extension(tname,s_rule_args.out_extension);
        }
        tname = join(odir,tname);
        targets[i] = target(tname,VAS(files[i]),s_rule_args.command);
    }
    Group *G = group_new(s_rule_args.command,targets);
    G->name =s_rule_args.name;
}

//~ // implementing the  S command.  Note that all values except opt , exports and debug may
// be set multiple times, appending new value.
void set_defaults(str_t name, str_t value) {
    if (str_eq(name,"includes")) {
        cat (&s_def.include_dirs,value);
    } else
    if (str_eq(name,"defines")) {
        cat (&s_def.defines,value);
    } else
    if (str_eq(name,"lib-dirs")) {
        cat (&s_def.lib_dirs,value);
    } else
    if (str_eq(name,"libs")) {
        cat (&s_def.libs,value);
    } else
    if (str_eq(name,"needs")) {
        cat (&s_def.needs,value);
    } else
    if (str_eq(name,"cflags")) {
        cat (&s_def.cflags,value);
    } else
    if (str_eq(name,"lflags")) {
        cat (&s_def.lflags,value);
    } else
    if (str_eq(name,"opt")) {
        s_def.opt = value;
    } else
    if (str_eq(name,"out-dir")) {
        s_def.output_directory = value;
    } else
    if (str_eq(name,"debug")) {
        s_def.debug = str2bool(value);
    } else
    if (str_eq(name,"exports")) {
        s_def.exports = str2bool(value);
    } else
    if (str_eq(name,"need-path")) {
        private_need_path = value;
    } else
    if (str_eq(name,"quiet")) {
        quiet = str2bool(value);
    } else {
        quit("unknown default variable name %s",name);
    }
}

// collect all the needs and call need_from_name on them, adding
// extra compile and link flags.
void update_needs() {
    if (s_def.needs) { // any default needs?
        cat (&s_args.needs, s_def.needs);
    }
    str_t *need_list = split(s_args.needs);
    if (need_list) {
        need_update(need_list, &s_args.cflags, &s_args.lflags);
    }
}

Group *compile_from_args(str_t compiler, str_t *files) {
    bool debug = false;
    str_t *cflags = &s_args.cflags;
    if (s_def.cflags) {
        cat (cflags,s_def.cflags);
    }
    
    // strictly speaking, these are not mutually exclusive.
    if (s_args.debug) {
        cat (cflags,"-g");
        debug = true;
    } else {
        cat (cflags,str_fmt("-O%s",s_args.opt));
    }
    
    cat (&s_args.include_dirs,s_def.include_dirs);
    cat (&s_args.defines,s_def.defines);
    
    str_t *includes_list = split(s_args.include_dirs);
    str_t *defines_list = split(s_args.defines);
    
    // output directory for object and deps files
    str_t odir = s_args.output_directory;
    if (*odir == 0) {
        odir = s_def.output_directory;
        if (! odir)
            odir = "";
    }
    if (str_eq(odir,"auto")) {
        odir = str_fmt("%s-%s",compiler,debug ? "debug" : "release");
    }
    return compile_step(compiler,files, s_args.cflags, includes_list, defines_list,odir);
}

Target *link_from_args(str_t compiler, str_t name, File **objs, int kind) {
    if (s_def.lflags) {
        s_args.lflags = s_def.lflags;
    }
    if (s_def.lib_dirs) {
        cat (&s_args.lib_dirs, s_def.lib_dirs);
    }
    if (s_def.libs) {
        cat (&s_args.libs, s_def.libs);
    }
    
    // try to strip executables unless they have debug or needed symbol information.
    if (kind == LINK_EXE) {
        if (s_args.exports || s_def.exports) { // executable exports symbols
            if (! macosx) // Not needed on OS X
                cat (&s_args.lflags,"-Wl,-E"); 
        } else 
        if (! s_args.debug)  { // otherwise strip as much as possible
            cat(&s_args.lflags,"-Wl,-s");
        }
    }
    
    if (! s_args.lflags) {
        s_args.lflags = "";
    }
    
    Target *res = linker(compiler,name,objs,s_args.lflags,split(s_args.lib_dirs),split(s_args.libs),kind);
    // program/lib targets push themselves to the front. Like make, shmake looks for
    // first target as the default.
    target_push_to_front(res);
    return res;
}

// Thunderbirds are Go!
Target  *straight_build(str_t compiler, str_t name, str_t *files) {
    int kind = LINK_EXE;
    update_needs();
    
    // shortcut - if there aren't any names, compile a single file
    // i.e. 'C foo.c' is equivalent to 'C foo foo.c'
    if (array_len(files)==0) {
        files = array_new(str_t,1);
        files[0]  =  name; 
        name = file_replace_extension(name,"");
    }
    
    // often easier to specify files by excluding some from a wildcard list
    if (*s_args.exclude) {
        str_t *excludes = split(s_args.exclude);
        str_t **ss = seq_new(str_t);
        for (str_t *f = files;  *f;  f++) {
            if (str_index(excludes,*f) == -1) {
                seq_add(ss, *f);
            }
        }
        files = seq_array_ref(ss);
    }
    
    int nf  = array_len(files);
    // linking can give us executables, shared libraries or static libraries based on extension of name
    str_t ext = file_extension(name);
    if (str_eq(ext,".so")) {
        cat(&s_args.lflags," -shared ");
        if (! macosx) //* is there any harm in letting this through?
            cat(&s_args.cflags," -fpic ");
        kind = LINK_SO;
    } else
    if (str_eq(ext,".a")) {
        kind = LINK_LIB;
    } else
    if (str_eq(ext,".c")) { // and so forth!!        
        files = array_resize(files,nf+1);
        files[nf] = name;
        name = file_replace_extension(name,"");
    }
    
    // Now our 'files' may not all be source and can also be libraries.
    // Necessary to sift these out and pass any source files to the compile step
    str_t **new_files = seq_new(str_t);
    // these are going to be all the inputs to the linker; keep first entry empty for compile group!
    str_t **ins = seq_new(str_t);
    seq_add(ins,NULL);
    FOR(i,nf) {
        ext = file_extension(files[i]);
        if (str_eq_any(ext,".a",".so")) {
            seq_add(ins,files[i]);
        } else {
            seq_add(new_files,files[i]);
        }
    }
    str_t *inputs = seq_array_ref(ins); // will have at least length 1
    files = seq_array_ref(new_files);
    Group *G = compile_from_args(compiler,files);
    inputs[0] = (str_t)G;
    return link_from_args(compiler,name,(File**)inputs,kind);
}

void setup_compiler(str_t name) {
    if (str_eq(name,"c")) {
        if (CC) return;
        CC = getenv("CC");
        if (! CC)
            CC = file_command("basename $(which gcc || which cc)");    
    } else
    if (str_eq(name,"c++")) {
        if (CXX) return;
        CXX = getenv("CXX");
        if  (! CXX)
            CXX = file_command("basename $(which g++ || which c++)");    
    }
}

// a shmake file needs to source /tmp/shmake.sh, which is generated
// from this string if it doesn't exist. The arguments to the shell functions
// are carefully separated by colons and written to the temporary file
// which we'll consume afterwards.

static str_t shmake_sh =
"out=$1\n"
"pipe() {\n"
"   res=''\n"
"   for f in \"$@\"; do\n" 
"       f=$(echo -n \"$f\" | tr '\\n' '\\001')\n"
"       res=\"$res:$f\"\n"
"   done\n"
"   echo $res >> $out\n"
"}\n"
"C() { pipe C \"$@\"; }\n"
"C99() { pipe C99 \"$@\"; }\n"
"Cpp() { pipe C++ \"$@\"; }\n"
"Cpp11() { pipe C++11 \"$@\"; }\n"
"T() { pipe target \"$@\"; }\n"
"S() { pipe set \"$@\"; }\n"
"R() { pipe rule \"$@\"; }\n"
"Q() { pipe quit \"$@\"; }\n"
"all() { pipe all \"$@\"; }\n";

int run_shmakefile(str_t specific_target) {
    char buff[MAX_LINE_BUFSZ];
    if (! file_exists(shmakefile,"r")) {  // "x"!!!
        quit("'%s' does not exist",shmakefile);
    }
    if (! file_exists("/tmp/shmake.sh","r")) {
        file_write_fmt("/tmp/shmake.sh",shmake_sh);
    }
    ArgState *state = arg_parse_spec(compiler_args);
    ArgState *rule_state = arg_parse_spec(rule_args);
    str_t tmp_file = str_fmt("/tmp/shmake.%d",getpid());
    str_t *args = NULL;
    int n = system(str_fmt("./%s %s",shmakefile,tmp_file));
    if (n != 0) {
        if (errno != 0)
            perror("shmake");
        quit("error executing '%s'",shmakefile);
    }
    FILE *in = fopen(tmp_file,"r");
    if (! in) {
        quit("cannot open %s",tmp_file);
    }
    while (file_gets(in,buff,sizeof(buff))) {
        // get any linefeeds back!
        char *p, *cmd;
        for (p = buff; *p; p++) {
            if (*p == '\001') *p = '\n';
        }
        // args separated by colon; skip first colon
        cmd = buff+1;
        p = strchr(cmd,':');
        if (p) {
            *p = '\0';
            args = (str_t*)str_split(p+1,":");
        }
        if (*cmd=='C') { // general compile target! 
            setup_compiler("c");
            str_t compiler = CC;
            ++cmd;
            if (str_eq2(cmd,"++")) { // C++
                setup_compiler("c++");
                cmd += 2;
                compiler = CXX;
                if (str_eq2(cmd,"11")) {
                    cat (&s_def.cflags," -std=c++0x "); 
                }
            } else {
                if (str_eq2(cmd,"99")) {
                    cat (&s_def.cflags," -std=c99 "); 
                }
            }
            // reset the argument parser
            arg_reset_used(state);            
            // that -1 is important.......
            char* err = (char*)arg_process(state,args-1);
            if (value_is_error(err)) {
                quit("C: %s",err);
            }
            if (! s_args.debug) {
                s_args.debug = debug;
            }
            if (*s_args.out_extension == 0)  {
                straight_build(compiler, s_args.name, s_args.files);
            } else {
                // this generates a group of linker targets
                // e.g. C plugins -R .so *.c  => group 'plugins' contains all resulting .so targets
                str_t o_ext = s_args.out_extension;
                if (str_eq(o_ext,"exe"))
                    o_ext = "";
                int n = array_len(s_args.files);
                Target **targets = array_new(Target*,n);
                FOR (i,n) {
                    str_t src_file = s_args.files[i];
                    str_t output_file = file_replace_extension(src_file,o_ext);
                    targets[i] = straight_build(compiler,output_file,VAS(src_file));
                }
                Group *G = group_new("cmd",targets);
                G->name = s_args.name;
            }
        } else 
        if (str_eq(cmd,"target")){
            str_t command = array_pop(args);
            if (str_eq(command,"none")) {
                command = NULL;
            }
            str_t name = args[0];
            str_t *prereq = array_copy(args,1,-1);
            target(name,group_expand_with_targets(prereq),command);
        } else
        if (str_eq(cmd,"all")) {            
            target("all",group_expand_with_targets(args),NULL);
        } else
        if (str_eq(cmd,"set")) {
            set_defaults(args[0],str_concat(array_copy(args,1,-1)," "));
        } else
        if (str_eq(cmd,"rule")) {
            process_rule(rule_state, args);
        } else 
        if (str_eq(cmd,"quit")) {
            str_t msg = args[0];
            if (str_eq(msg,"exists")) {
                if (! getenv(args[1]))
                    quit("quit '%s' does not exit",args[1]);
            } else
            if (args[1] == NULL) 
                quit("quit %s",msg);
        }
        unref(args);
    }
    unlink(tmp_file);
    if (array_len(targets()) == 0) {
        quit("no targets defined","");
    }
    shmake_flags(verbose_level,testing,quiet);    
    // notice the special case; 'all' matches the first target, if not explicitly
    // present.   target_push_to_front() ensures that program/lib targets end here.
    str_t target_name = specific_target ? specific_target : "all";
    Target *T = target_from_file(target_name);
    if (T == NULL) {
        if (str_eq(target_name,"clean")) {
            // remove all targets.  (Special logic in target_remove also gets rid of .d files)
            target_forall(target_remove);
            return 0;
        } else {
            T = target_first();
        }
    }
    if (T == NULL) {
        quit("no target %s",target_name);
    }
    target_check(T);
    return 0;
}

int main(int argc, const char **argv)
{
    int res = 0;
    arg_state = arg_command_line(main_args, argv);
    if (*do_create) {
        file_write_fmt("shmakefile","#!/bin/sh\n. /tmp/shmake.sh\n\n%s\n",do_create);
        exec("chmod +x shmakefile");
        printf("shmakefile created\n");
        return 0;
    }
    if (*start_directory) {
        if (chdir(start_directory) != 0) {
            fprintf(stderr,"unable to change directory to '%s'\n",start_directory);
            return 1;
        }
    }
    
    PLAT=file_command("uname");
    macosx = str_eq(PLAT,"Darwin");
    setenv("PLAT",PLAT,true);
    
    // verbose is an array of bools.
    // can say -v for level 1 and -vv for level 2
    verbose_level = 0;
    if (verbose[0]) {
        ++verbose_level;
        if (verbose[1]) {
            ++verbose_level;
        }
    }

    str_t shmake_target = NULL;
    for (str_t *a = shmake_args; *a; a++) {
        char *p = strchr(*a,'=');
        if (p) { // any VAR=VALUE pairs passed as enviroment for the script
            *p = '\0';
            setenv(*a,p+1,true);
        } else {
            shmake_target = *a;
        }
    }
    res = run_shmakefile(shmake_target);
    return res;
}
