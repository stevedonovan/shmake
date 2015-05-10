CC=gcc
CFLAGS=-std=c99 -O2 -Wall  -I.
OBJS=shmake.o lib.o utils.o

shmake: llib/libllib.a $(OBJS)
	$(CC) $(OBJS) -L llib -lllib -Wl,-s -o shmake
	
llib/libllib.a:
	make -C llib
