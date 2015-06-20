CC=gcc
OPT=-O2
#STRIP=-Wl,-s
#OPT=-g
CFLAGS=-std=c99 $(OPT) -Wall  -I.
OBJS=shmake.o lib.o utils.o

shmake: llib/libllib.a $(OBJS)
	$(CC) $(OBJS) -L llib -lllib $(STRIP) -o shmake
	
llib/libllib.a:
	make -C llib

clean:
	rm *.o

test:
	shmake -C tests/action
	shmake -C tests/c-script P=hello
	shmake -C tests/outdir
	shmake -C tests/rule
	shmake -C tests/self
	shmake -C tests/simple