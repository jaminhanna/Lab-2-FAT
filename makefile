all: jdisk_test  FAT FATRW sectordiff

clean:
	rm -f jdisk_test FAT FATRW sectordiff *.o

.c.o: 
	gcc -c -Iinclude $*.c

FATRW: FATRW.o jdisk.o
	gcc -o FATRW FATRW.o jdisk.o
