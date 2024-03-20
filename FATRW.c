/* Jamin Hanna
   FATRW.c
   07 March 2024

   This program imports and exports files to and from a jdisk, using a
   file allocation table (composed of the first block(s) of the disk)
   for efficiency. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "jdisk.h"

char **pb; /* pointers to blocks */
unsigned nfatsectors;
unsigned nread;       /* number of FAT sectors read */

void import(char *filename, void *jd, FILE *fp);
void export(void *jd, FILE *fp, unsigned int lba);
void cleanup(void);

int main(int argc, char *argv[])
{
  int i;
  unsigned nsectors;
  unsigned int sblock;           /* starting block */
  char *op;             /* operation to be performed */
  char *mode;           /* file access mode */
  char *usage;        
  char *msg;        
  void *jd;             /* handle to a jdisk */
  FILE *fp;

  usage = "       FATRW diskfile import input-file\n"
          "       FATRW diskfile export starting-block output-file\n";

  if (argc < 3) { fprintf(stderr, "%s", usage); return EXIT_FAILURE; }

  i = 2;

  if (atexit(cleanup) != 0) {
    fprintf(stderr, "Atexit failed\n");
    return EXIT_FAILURE;
  }

  /* validate operation and get file access mode */
  op = argv[i++];
  if      (strcmp(op, "import") == 0) mode = "r";
  else if (strcmp(op, "export") == 0) mode = "w";
  else    { fprintf(stderr, "%s", usage); return EXIT_FAILURE; }

  if (strcmp(op, "import") == 0 && argc < 4) {
    fprintf(stderr, "%s", usage);
    fprintf(stderr, "import needs an input file.\n");
    return EXIT_FAILURE;
  }
  if (strcmp(op, "export") == 0 && argc < 5) {
    fprintf(stderr, "%s", usage);
    fprintf(stderr, "export needs a starting block and output file.n");
    return EXIT_FAILURE;
  }

  /* error if too many arguments */
  if ((strcmp(op, "import") == 0 && argc > 4) ||
      (strcmp(op, "export") == 0 && argc > 5))
    { fprintf(stderr, "%s", usage); return EXIT_FAILURE; }

  /* make sure starting-block argument is a number */
  if (strcmp(op, "export") == 0 &&
      sscanf(argv[i++], "%u", &sblock) == 0) {
    fprintf(stderr, "%s", usage);
    fprintf(stderr, "export: starting block not a number.\n");
    return EXIT_FAILURE;
  }

  /* attach to jdisk */
  if ((jd = jdisk_attach(argv[1])) == NULL) {
    fprintf(stderr, "%s", usage);
    fprintf(stderr, "Could not attach to the diskfile.\n");
    return EXIT_FAILURE;
  }

  /* get information about jdisk and validate its size */
  nsectors = jdisk_size(jd) / JDISK_SECTOR_SIZE;
  if (nsectors == 1) {
    fprintf(stderr, "%s", usage);
    fprintf(stderr, "The jdisk is too small.\n");
    return EXIT_FAILURE;
  }
  /* get the number of FAT sectors that allows the number of data
     sectors to be maximized */
  nfatsectors = (nsectors+1)/513;
  nfatsectors += ((nsectors-nfatsectors+1)%512 == 0) ? 0 : 1;

  /* validate starting-block */
  if (strcmp(op, "export") == 0) {
    if (sblock < 0) {
      fprintf(stderr, "%s", usage);
      fprintf(stderr, "export: starting can't be negative.\n");
      return EXIT_FAILURE;
    }
    if (sblock < nfatsectors) {
      fprintf(stderr, "Error in Export:"
                      " LBA is not for a data sector.\n");
      return EXIT_FAILURE;
    }
    if (sblock >= nsectors) {
      fprintf(stderr, "Error in Export: LBA too big\n");
      return EXIT_FAILURE;
    }
  }

  /* open file with appropriate mode */
  if ((fp = fopen(argv[i], mode)) == NULL)
    { perror(argv[i]); return EXIT_FAILURE; }

  /* allocate memory for pb now that size of disk is known */
  pb = (char **) malloc(nsectors * sizeof(char *));
  if (pb == NULL)
    { fprintf(stderr, "Malloc failed\n"); return EXIT_FAILURE; }
  /* initialize pointers to NULL */
  for (i = 0; i < nsectors; ++i) pb[i] = NULL;

  /* perform the appropriate operation */
  if      (strcmp(op, "import") == 0) import(argv[3], jd, fp);
  else if (strcmp(op, "export") == 0) export(jd, fp, sblock);

  return EXIT_SUCCESS;
}

/* write file to disk if there is sufficient space */
void import(char *filename, void *jd, FILE *fp)
{
  int i;
  int rv;               /* return value of jdisk_read */
  unsigned short lval;  /* index into FAT of next link to read */
  unsigned short first; /* index of first block used by file */
  unsigned block;       /* current block */
  unsigned nblocks;
  long nbytes;
  short *sp;
  struct stat sbuf;
  long sread;
  long lba;
  int nread;
  char buf[JDISK_SECTOR_SIZE];

  /* ensure there are enough free blocks to store the file */
  if (stat(filename, &sbuf) == -1)
    { fprintf(stderr, "Stat failed\n"); exit(EXIT_FAILURE); }
  nbytes  = (long) sbuf.st_size;
  nblocks = (nbytes-1)/JDISK_SECTOR_SIZE+1;
  for (i = lval = 0; i < nblocks; ++i) {
    /* read a block if it is required to get the value of the link */
    block = lval / (JDISK_SECTOR_SIZE/2);
    if (pb[block] == NULL) {
      pb[block] = (char *) malloc(JDISK_SECTOR_SIZE * sizeof(char));
      if (pb[block] == NULL)
        { fprintf(stderr, "Malloc failed\n"); exit(EXIT_FAILURE); }
      rv = jdisk_read(jd, block, pb[block]);
      if (rv < 0)
        { fprintf(stderr, "Jdisk_read failed\n"); exit(EXIT_FAILURE); }
      ++nread;
    }
    /* treat the block as an array of shorts */
    sp = (short *) pb[block];
    /* get the index of the next link */
    if ((lval = sp[lval%(JDISK_SECTOR_SIZE/2)]) == 0) break;
  }

  /* error if not enough free blocks */
  if (i != nblocks) {
    fprintf(stderr, "%s is too big for the disk (%ld vs %ld)\n",
            filename, nbytes, (long) i * JDISK_SECTOR_SIZE);
    exit(EXIT_FAILURE);
  }

  /* save index of first block used by file for printing purposes */
  first = *((short *) *pb) + nfatsectors - 1;

  /* write file to jdisk */
  lval = sread = 0;
  for (i = 0; i < nblocks; ++i) {
    /* get the bytes that will go into the next block */
    nread = fread(buf, sizeof(char), JDISK_SECTOR_SIZE, fp);
    if (nread == 0) { fprintf(stderr, "Error\n"); exit(EXIT_FAILURE); }

    /* get the logical block address of the next block */
    block = lval / (JDISK_SECTOR_SIZE/2);
    sp = (short *) pb[block];
    lval = sp[lval%(JDISK_SECTOR_SIZE/2)];
    lba = (lval + nfatsectors - 1);

    /* handle the last block cases */
    if (i+1 == nblocks) {
      /* set link 0 to the value of the current link */
      block = lval / (JDISK_SECTOR_SIZE/2);
      if (pb[block] == NULL) {
        pb[block] = (char *) malloc(JDISK_SECTOR_SIZE * sizeof(char));
        if (pb[block] == NULL) { fprintf(stderr, "Malloc failed\n");
          exit(EXIT_FAILURE);
        }
        ++nread;
      }
      rv = jdisk_read(jd, block, pb[block]);
      if (rv < 0) {
        fprintf(stderr, "Jdisk_read failed\n"); exit(EXIT_FAILURE);
      }
      sp = (short *) pb[block];
      *((short *) *pb) = sp[lval%(JDISK_SECTOR_SIZE/2)];

      switch (sbuf.st_size % JDISK_SECTOR_SIZE) {
        /* first case: the file's size is a multiple of
           JDISK_SECTOR_SIZE; set the current link to 0 */
        case 0    : sp[lval%(JDISK_SECTOR_SIZE/2)] = 0; break;

        /* second case: the file takes up JDISK_SECTOR_SIZE-1 bytes of
           its last block; set the last byte of the block to 0xff and
           have the current link point to itself */
        case 1023 : buf[JDISK_SECTOR_SIZE-1] = 0xff;
                    sp[lval%(JDISK_SECTOR_SIZE/2)] = lval; break;

        /* third case: the file takes up less than JDISK_SECTOR_SIZE-1
           bytes of its last block; place a short in the last two bytes
           of the block that holds the number of bytes of the block
           taken up by the file, and have the current link point to
           itself */
        default    : *((short *) (buf+JDISK_SECTOR_SIZE-2)) = nread;
                     sp[lval%(JDISK_SECTOR_SIZE/2)] = lval; break;
      }
    }

    /* write the block to disk */
    if (jdisk_write(jd, lba, (void *) buf) < 0) {
      fprintf(stderr, "Jdisk_write failed\n"); exit(EXIT_FAILURE);
    }
  }

  /* write the updated blocks of the fat back to disk */
  jdisk_write(jd, 0L, pb[0]);
  if (block != 0) jdisk_write(jd, block, pb[block]);

  /* Print information pertaining to successful import */
  printf("New file starts at sector %d\n", first);
  printf("Reads: %ld\n", jdisk_reads(jd));
  printf("Writes: %ld\n", jdisk_writes(jd));
}

/* read file from disk */
void export(void *jd, FILE *fp, unsigned int lba)
{
  unsigned int i;
  unsigned short index;
  unsigned int block;
  int rv;
  unsigned short val;
  int to_write;
  unsigned char buf[JDISK_SECTOR_SIZE];

  to_write = JDISK_SECTOR_SIZE;

  while (1) {
    /* get a block from the disk */
    rv = jdisk_read(jd, lba, buf);
    if (rv < 0) {
      fprintf(stderr, "Jdisk_read failed\n");
      exit(EXIT_FAILURE);
    }

    /* read the link corresponding to the block just read, to determine
       how much of the block belongs to the file */
    index = lba - nfatsectors + 1;
    block = index / (JDISK_SECTOR_SIZE/2);
    if (pb[block] == NULL) {
      pb[block] = (char *) malloc(JDISK_SECTOR_SIZE * sizeof(char));
      if (pb[block] == NULL) {
        fprintf(stderr, "Malloc failed\n");
        exit(EXIT_FAILURE);
      }
      /* read the block into the buffer just allocated */
      rv = jdisk_read(jd, block, pb[block]);
      if (rv < 0) {
        fprintf(stderr, "Jdisk_read failed\n");
        exit(EXIT_FAILURE);
      }
      ++nread;
    }
    /* store the value of the current link in val */
    val = *((short *) pb[block] + index%(JDISK_SECTOR_SIZE/2));

    /* use the value of the link to determine how much of the block
       belongs to the file */
    if (val == index) {
      /* if the last byte is 0xff, then JDISK_SECTOR_SIZE-1 bytes of
         the block are used by the file */
      if (buf[JDISK_SECTOR_SIZE-1] == 0xff) {
        to_write = JDISK_SECTOR_SIZE-1;
      } else {
        /* the last two bytes of the block compose a short whose value
           represents the number of bytes of the block used by the
           file */
        to_write = *((short *) (buf+JDISK_SECTOR_SIZE-2));
      }
    }

    /* write to_write bytes to the file */
    if (fwrite(buf, sizeof(char), to_write, fp) != to_write) {
      fprintf(stderr, "Fwrite error\n");
      exit(EXIT_FAILURE);
    }

    /* return if this was the last segment of data to write */
    if (val == index || val == 0) break;

    /* otherwise, calculate the lba of the next block */
    lba = val+nfatsectors-1;
  }

  printf("Reads: %d\n", jdisk_reads(jd));
  printf("Writes: %d\n", jdisk_writes(jd));
}

/* close files and free memory */
void cleanup(void)
{
  unsigned i, j;

  jdisk_unattach(jd);
  fclose(fp);

  /* free memory associated with read blocks of FAT */
  for (i = j = 0; j < nread; ++i) {
    if (pb[i] != NULL) {
      free(pb[i]);
      ++j;
    }
  }
  free(pb);
}
