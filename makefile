all: FATRW

clean:
	rm -f FATRW *.o

.c.o: 
	gcc -c -Iinclude $*.c

FATRW: FATRW.o jdisk.o
	gcc -o FATRW FATRW.o jdisk.o
