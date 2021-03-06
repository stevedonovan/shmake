## A shell-based build tool

shmake is a build tool which is driven by POSIX shell scripts, specialized around
gcc and compilers which behave like it.  It deliverately lacks ambition to be an universal
build tool like [lake](https://github.com/stevedonovan/Lake), but aims to make the simple stuff easy
and the complicated stuff easier.

For instance, say we have a little project with two source files, 'hello.c' and 'common.c',
with 'hello.c' including 'common.h'.   This would be the _shmakefile_.

```Shell
#!/bin/sh
. /tmp/shmake.sh

C hello *.c

```

Shmakefiles are executable shell scripts - the second line sources any special magic
which shmake needs to provide.  But they are not executed directly.  When shmake
is run, it will look for a shmakefile, in time-honoured fashion.

```Shell
simple$ shmake
compiling common.o
compiling hello.o
linking hello
simple$ shmake
simple$ # nothing happened (shmake is quiet)
simple$ touch common.c
simple$ shmake
compiling common.o
linking hello
# rebuild after change to common.c
simple$ touch common.h
simple$ shmake
compiling hello.o
linking hello
# rebuild hello.c after change to included common.h
```

So the single  'C hello *.c' gives us a build which already tracks all the dependencies.
It does this by using GCC's -MMD flag and reading the resulting .d files, saving you the most tedious
part of writing makefiles.

By default it gives you an optimized stripped executable, and does not show you the actual compilation.
The '-g' flag will give you a debug build, and '-v' will make shmake more verbose and chattery:

```Shell
simple$ shmake clean
simple$ shmake -g -v
gcc -c -Wall -MMD -g common.c -o common.o
gcc -c -Wall -MMD -g hello.c -o hello.o
gcc common.o hello.o  -o hello
simple$ shmake clean
simple$ shmake -v
gcc -c -Wall -MMD -O2 common.c -o common.o
gcc -c -Wall -MMD -O2 hello.c -o hello.o
gcc common.o hello.o -Wl,-s -o hello

```

Just to make things a little easier; to create an initial shmakefile, use the '-c' option

```Shell
simple$ shmake -c 'C hello *.c'
shmakefile created
simple$ cat shmakefile
#!/bin/sh
. /tmp/shmake.sh

C hello *.c
```

## Full Control

This is all very cute, but can it do real projects?  Here is a shmakefile for shmake, in the 'tests/self'
directory. First the simplified version, assuming that the source has been copied up:

```Shell
#!/bin/sh
. /tmp/shmake.sh

SRC=../..

C99 shmake -I$SRC -L$SRC/llib -lllib shmake.c lib.c utils.c
```

It is not a complicated project (only about 1200 lines in total) but leans heavily on llib.
Here we specify the include and library directories, and the libs to link against.  C99 means we specifically
want '-std=std99'.  This is pretty much how we would write a build as a shell script, except that this
takes care of the dependency tracking.

This is entirely equivalent, except using the S ('set') command for specifying _default flags_.
It's clearer for more complicated projects, plus it applies to _any_ program target unless they
explicitly override the defaults.

```Shell
#!/bin/sh
. /tmp/shmake.sh

SRC=../..

S includes $SRC
S lib-dirs $SRC/llib
S libs llib

C99 shmake shmake.c lib.c utils.c

```
The set variables are, with their compile command flag equivalents:

  - includes  (-I)  include directorie
  - defines  (-D) preprocessor defines
  - lib-dirs  (-L) directories to search for libraries
  - libs   (-l) libraries
  - needs (-n) any 'needs'  (see next section)
  - need-path extra dir to find needs (ditto)
  - out-dir (-d) directory for object files and dependency (.d) files
  - cflags any extra compilation flags
  - lflags any extra link flags
  - opt  (-O) optimization level (default '2' - hence '-O2')
  - debug (-g) debug build
  - exports (-e) executable exports its symbols
  - slack (-S) don't use "-Wall"
  - quiet (-q) only generate output on error

Most of these are additive, so that setting a variable multiple times will add new values.

The -d flag is useful for keeping your source directory nice and clean, since the .o and .d files will
have their own directory.  The output directory will be created if it does not already exist;  if it is the
special word 'auto', then its name is a combination of the compiler and the build type, e.g.
'gcc-release' or 'clang-debug'.  This makes switching between a release and debug build more efficient.

## Defining Needs

If you had a number of projects depending on llib or some other external dependency, then _needs_
are a useful concept.  A borrowing from Lake, needs are a general way of looking up the compile
and link flags.  A need can be defined by a file with the extension '.need', either in current dir
or in ~/.shmake. Additionally, a _private_ need path can be provided with 'S need-path <path>'
and this will be checked as well.  It is defined as a script which echoes at least _one_ of 'cflags ...' or 'libs ... ':

```Shell
self$ cat ~/.shmake/llib.need
#!/bin/sh
# simple file providing a Need
echo cflags -I/home/user/dev/c/llib
echo libs -L/home/user/dev/c/llib/llib -lllib

self$ cat need.shmak
#!/bin/sh
. /tmp/shmake.sh
C99 shmake -n llib shmake.c lib.c utils.c

self$ shmake -v -f need.shmak
gcc -c -Wall -MMD  -std=c99 -I/home/user/dev/c/llib  -O2 shmake.c -o shmake.o
gcc -c -Wall -MMD  -std=c99 -I/home/user/dev/c/llib  -O2 lib.c -o lib.o
gcc -c -Wall -MMD  -std=c99 -I/home/user/dev/c/llib  -O2 utils.c -o utils.o
gcc shmake.o lib.o utils.o  -L/home/user/dev/c/llib/llib -lllib  -Wl,-s -o shmake
```
If there's no such file, we ask `pkg-config`.

The `-n/---needs` flag generally takes a space-separated list of needs - it cannot be used multiply like '-I'.

(Note: in the initial release of shmake, the format of .need files resembled .pc files as understood by 'pkg-config`.
However, it was _different_ from the actual .pc file format, and did not allow the flexibility of 'shell everywhere'.
Defining need files as scripts allows for checking for the existence and location of packages; 
they may also be written in Python or any other language if you feel frustrated.)

## A Step Back: Targets

Underneath, shmake is very much like make, and allows the same style of dependency-based
programming.

For the simple example, we could perform the build explicitly:

```Shell
#!/bin/sh
. /tmp/shmake.sh

COMPILE='gcc -c @(INPUT) -o @(TARGET)'
LINK='gcc @(DEPS) -o @(TARGET)'

T hello.o hello.c common.h "$COMPILE"
T common.o common.c "$COMPILE"
T hello hello.o common.o "$LINK"
all hello

```

The T ('target') command is of the form 'T target (prequisites) shell-command'. 'Prerequisites'
is the make term for 'inputs that our target depends on'.
So 'hello.o' depends on hello.o _and_ on common.h.  (This is a clunky word so I will just call them 'inputs'.)
The shell-command can contain @() expansions of the three variables TARGET (first name) INPUT (first input)
and DEPS (all inputs). The targets are hello.o, common.o and hello itself.
We need the 'dummy' target 'all'  ('all a b c' is short for 'T all a b c none') so that shmake knows what
default target to build.  Otherwise, just as in make, it would just compile hello.c and stop.

This style is tedious for actually building projects because you must manually track all the inputs
your targets depend on. For gcc and compatible compilers, the marvelous
-MMD flag (create .d dependency file) helps tremendously and that's the big convenience of the
C compile command.  But it does mean you can use shmake with any set of commands,
using all the power of shell scripting.

If there are a lot of targets using the same command, then _rules_ are useful.
For instance, in tests/self/shmakefile, there is:

```Shell
SRC='../..'
R copy-files -d $PWD ditto 'cp @(INPUT) @(TARGET)' $SRC/*.c $SRC/*.h
```

A rule takes the following arguments:
  - it has a _name_
  - an _output extension_. 'ditto' means that the out extension is the in extension.
  - a _command_, just as with a target
  - a set of input files
  - can use -d to set the output directory, which you _must_ do with 'ditto'!

The name is not a target, although it can be used in the same contexts; it is a _group_
of related targets, generated by the rule.  Rules can be seen as target factories.

## Rules, Groups and the 'C' Command

It is common enough to want to make a set of simple executables using the same rule. The `R` command
can do this, but then you lose the special compiling support.   We have a number of source files, and each
needs to be compiled to an executable of the same name. The `-R` flag does this:

```Shell
C progs -R '' *.c
all progs
```
The name 'progs' is again a group name, and represents a list of targets. When a group name is
specified as an input or dependency, then it expands out that list of targets - it is _not_ itself a target.
Like with the`R` command, you need to specify the output directory, here just the empty string - you may
use the special name 'exe' instead. So `-R`, like the `R` command, is a target factory.

The `C` command creates both the compile targets, plus the final link target. The `-G` flag
makes `C` only produce the compile targets.  It is still necessary to give a name but then the
name becomes the _group name_.  This name can be passed to another `C` command and gets
added to the list of object files that will be linked.

```Shell
C objs -G *.c
C prog objs
````
Why would you want to separate out compiling and linking like this?  When some files need to be
compiled differently, say C files and C++ files.

## Invoking shmake

I have already mentioned -v for verbose; if you double this up as -vv then you will get a
detailed dump of dependency checks.  -t for testing is useful if you just want to see what
shmake will do, without actually executing the commands.

'-f' has the same meaning as in make - use a named file as the shmakefile. '-C' also means
'switch to directory first'.

shmake provides three predefined variables to shmakefiles: CC (the C compiler),
CXX (the C++ compiler) and PLAT, which is the value of `uname`.
The compilers are initialized to the values found, e.g CC is either 'gcc' or 'cc' depending on what
exists on the path.

The non-flag arguments are either VAR=VALUE assignments or targets.
So 'shmake CC=clang' will override the default value of CC.
Currently only one explicit target is supported; the default is 'all'.
There is a predefined target 'clean' which removes all targets representing files.

Please note an important 'gotcha' with shmake - the shmakefile is executed, and its output is then
processed by shmake.  Any variable assignments are not passed to shmake itself, because they are
created in a subshell.  Any _actions_ are executed from shmake directly, and so do ensure that any
variables within them have already been expanded.

## Example: Building Lua

For a complete programming language, Lua is relatively easy to build.
But the makefile is a bit convolved and obscures what's going on.  This shmakefile makes things
a lot clearer.

```Shell
#!/bin/sh
. /tmp/shmake.sh

LUA_LIB=liblua52.a
S defines LUA_COMPAT_ALL
S exports true
S libs readline
S libs m

echo "Building Lua for $PLAT platform"

case $PLAT in
freebsd)
    S defines LUA_USE_LINUX
    ;;
Linux)
    S defines LUA_USE_LINUX
    S libs dl
    ;;
ansi)
    S defines LUA_ANSI
    ;;
posix)
    S defines LUA_USE_POSIX
    ;;
solaris)
    S defines LUA_USE_POSIX LUA_USE_DLOPEN
    S libs dl
    ;;
darwin)
    S defines LUA_USE_MACOSX
    ;;
*)
    echo "unsupported platform $PLAT. One of freebsd,Linux,Darwin,posix,ansi,solaris"
    exit 1
    ;;
esac

C $LUA_LIB *.c -x 'lua.c luac.c'
C lua lua.c $LUA_LIB
C luac luac.c $LUA_LIB

all lua luac

```
Note the very useful '-x' (for 'exclude') flag that avoids the need to write out all those C files!
Apart from being clearer, this shmakefile immediately can give youa debug build with the global '-g' flag.

If nothing else, shmake is a demonstration of a useful tactic: using shell script as your
program's domain specific language (DSL).  In this way, a deeper knowledge of shell
translates directly into a more powerful way to build projects.
