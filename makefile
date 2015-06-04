CC=gcc
OPT=-02
STRIP=-Wl,-s
#OPT=-g
CFLAGS=-std=c99 $(OPT) -Wall  -I.
OBJS=shmake.o lib.o utils.o

shmake: llib/libllib.a $(OBJS)
	$(CC) $(OBJS) -L llib -lllib $(STRIP) -o shmake
	
llib/libllib.a:
	make -C llib

clean:
	rm *.o
