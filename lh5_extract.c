/*
 * This file is a severely cut down and cleaned up version of lha.
 *
 * All changes compared to lha-svn894 are:
 *
 * Copyright 2009      Luc Verhaegen <libv@skynet.be>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 */
/*
 * LHA has a terrible history... It dates back to 1988, has had many different
 * authors and has been mostly Public Domain Software.
 *
 * Since 1999, Koji Arai <arai@users.sourceforge.jp> has been doing most of
 * the work at http://sourceforge.jp/projects/lha/.
 */

#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <string.h>
#include <stdlib.h>

typedef int boolean;
#define FALSE 0
#define TRUE  1

#define FILENAME_LENGTH 1024

/*
 *
 * LHA header parsing.
 *
 */

#define LZHEADER_STORAGE        4096

static int
calc_sum(unsigned char *p, int len)
{
    int sum = 0;

    while (len--)
	sum += *p++;

    return sum & 0xff;
}

#define COMMON_HEADER_SIZE      21      /* size of common part */
#define I_LEVEL1_HEADER_SIZE  27 /* + name_length */

/*
 * level 1 header
 *
 *
 * offset   size  field name
 * -----------------------------------
 *     0       1  header size   [*1]
 *     1       1  header sum
 *             -------------------------------------
 *     2       5  method ID                        ^
 *     7       4  skip size     [*2]               |
 *    11       4  original size                    |
 *    15       2  time                             |
 *    17       2  date                             |
 *    19       1  attribute (0x20 fixed)           | [*1] header size (X+Y+25)
 *    20       1  level (0x01 fixed)               |
 *    21       1  name length                      |
 *    22       X  filename                         |
 * X+ 22       2  file crc (CRC-16)                |
 * X+ 24       1  OS ID                            |
 * X +25       Y  ???                              |
 * X+Y+25      2  next-header size                 v
 * -------------------------------------------------
 * X+Y+27      Z  ext-header                       ^
 *                 :                               |
 * -----------------------------------             | [*2] skip size
 * X+Y+Z+27       data                             |
 *                 :                               v
 * -------------------------------------------------
 *
 */
static unsigned int
lha_header_level1_parse(FILE *fp, unsigned int *original_size, unsigned int *packed_size,
			char *filename, unsigned int *crc)
{
    unsigned char data[LZHEADER_STORAGE];
    unsigned int offset;
    unsigned char header_size, checksum, name_length;
    unsigned short new_size, extend_size;

    if (fread(data, COMMON_HEADER_SIZE, 1, fp) == 0) {
        fprintf(stderr, "Error: Unable to read lha header: %s\n",
		strerror(errno));
        return FALSE;
    }

    /* check attribute */
    if (data[19] != 0x20) {
	fprintf(stderr, "Error: Invalid lha header attribute byte.\n");
        return FALSE;
    }

    /* check method */
    if (memcmp(data + 2, "-lh5-", 5) != 0) {
	fprintf(stderr, "Error: Compression method is not LZHUFF5.\n");
	return FALSE;
    }

   /* check header level */
    if (data[20] != 1) {
	fprintf(stderr, "Error: Header level %d is not supported\n", data[20]);
	return FALSE;
    }

    /* read in the full header */
    header_size = data[0];
    if (fread(data + COMMON_HEADER_SIZE,
              header_size + 2 - COMMON_HEADER_SIZE, 1, fp) == 0) {
        fprintf(stderr, "Error: Unable to read full lha header: %s\n",
		strerror(errno));
        return FALSE;
    }

    /* verify checksum */
    checksum = data[1];
    if (calc_sum(data + 2, header_size) != checksum) {
        fprintf(stderr, "Error: Invalid lha header checksum.\n");
        return FALSE;
    }

    *packed_size = *(unsigned int *) (data + 7);
    *original_size = *(unsigned int *) (data + 11);

    name_length = data[21];
    memcpy(filename, data + 22, name_length);
    filename[name_length] = '\0';

    *crc = *(unsigned short *) (data + 22 + name_length);

    offset = header_size + 2;
    /* Skip extended headers */
    extend_size = *(unsigned short *) (data + offset);
    while (extend_size) {
	*packed_size -= extend_size;
	offset += extend_size;

	if (fread(&new_size, 2, 1, fp) != 2) {
	    fprintf(stderr, "Error: Invalid extended lha header.\n");
	    return FALSE;
	}

	fseek(fp, extend_size - 2, SEEK_CUR);
	extend_size = new_size;
    }

    return offset;
}

/*
 *
 */
#define CRCPOLY 0xA001 /* CRC-16 (x^16+x^15+x^2+1) */
static unsigned int crctable[0x100];

static void
make_crctable(void)
{
    unsigned int    i, j, r;

    for (i = 0; i <= 0xFF; i++) {
        r = i;
        for (j = 0; j < 8; j++)
            if (r & 1)
                r = (r >> 1) ^ CRCPOLY;
            else
                r >>= 1;
        crctable[i] = r;
    }
}

static unsigned int
calccrc(unsigned int crc, unsigned char *p, unsigned int n)
{
    while (n-- > 0) {
	crc = crctable[(crc ^ (*p)) & 0xFF] ^ (crc >> 8);
	p++;
    }

    return crc;
}

/*
 *
 * LHA extraction.
 *
 */
#define MIN(a,b) ((a) <= (b) ? (a) : (b))

#define LZHUFF5_DICBIT      13      /* 2^13 =  8KB sliding dictionary */
#define MAXMATCH            256 /* formerly F (not more than 255 + 1) */
#define THRESHOLD           3   /* choose optimal value */
#define NP          (LZHUFF5_DICBIT + 1)
#define NT          (16 + 3) /* USHORT + THRESHOLD */
#define NC          (255 + MAXMATCH + 2 - THRESHOLD)

#define PBIT        4       /* smallest integer such that (1 << PBIT) > * NP */
#define TBIT        5       /* smallest integer such that (1 << TBIT) > * NT */
#define CBIT        9       /* smallest integer such that (1 << CBIT) > * NC */

/*      #if NT > NP #define NPT NT #else #define NPT NP #endif  */
#define NPT         0x80

#define BUFFERSIZE  2048

static off_t compsize;
static FILE *infile;

static unsigned short left[2 * NC - 1], right[2 * NC - 1];

static unsigned short c_table[4096];   /* decode */
static unsigned short pt_table[256];   /* decode */

static unsigned char  c_len[NC];
static unsigned char  pt_len[NPT];

static unsigned short bitbuf;

static unsigned char subbitbuf, bitcount;

static void
fillbuf(unsigned char n)          /* Shift bitbuf n bits left, read n bits */
{
    while (n > bitcount) {
        n -= bitcount;
        bitbuf = (bitbuf << bitcount) + (subbitbuf >> (8 - bitcount));
        if (compsize != 0) {
            compsize--;
            subbitbuf = getc(infile);
        } else
            subbitbuf = 0;
        bitcount = 8;
    }
    bitcount -= n;
    bitbuf = (bitbuf << n) + (subbitbuf >> (8 - n));
    subbitbuf <<= n;
}

static unsigned short
getbits(unsigned char n)
{
    unsigned short  x;

    x = bitbuf >> (16 - n);
    fillbuf(n);

    return x;
}

static unsigned short
peekbits(unsigned char n)
{
    unsigned short  x;

    x = bitbuf >> (16 - n);

    return x;
}

static void
make_table(short nchar, unsigned char bitlen[], short tablebits, unsigned short table[])
{
    unsigned short  count[17];  /* count of bitlen */
    unsigned short  weight[17]; /* 0x10000ul >> bitlen */
    unsigned short  start[17];  /* first code of bitlen */
    unsigned short  total;
    unsigned int    i, l;
    int             j, k, m, n, avail;
    unsigned short *p;

    avail = nchar;

    /* initialize */
    for (i = 1; i <= 16; i++) {
        count[i] = 0;
        weight[i] = 1 << (16 - i);
    }

    /* count */
    for (i = 0; i < nchar; i++) {
        if (bitlen[i] > 16) {
            /* CVE-2006-4335 */
            fprintf(stderr, "Error: Bad table (case a)");
            exit(1);
        }
        else
            count[bitlen[i]]++;
    }

    /* calculate first code */
    total = 0;
    for (i = 1; i <= 16; i++) {
        start[i] = total;
        total += weight[i] * count[i];
    }
    if ((total & 0xffff) != 0 || tablebits > 16) { /* 16 for weight below */
        fprintf(stderr, "Error: make_table(): Bad table (case b)");
        exit(1);
    }

    /* shift data for make table. */
    m = 16 - tablebits;
    for (i = 1; i <= tablebits; i++) {
        start[i] >>= m;
        weight[i] >>= m;
    }

    /* initialize */
    j = start[tablebits + 1] >> m;
    k = MIN(1 << tablebits, 4096);
    if (j != 0)
        for (i = j; i < k; i++)
            table[i] = 0;

    /* create table and tree */
    for (j = 0; j < nchar; j++) {
        k = bitlen[j];
        if (k == 0)
            continue;
        l = start[k] + weight[k];
        if (k <= tablebits) {
            /* code in table */
            l = MIN(l, 4096);
            for (i = start[k]; i < l; i++)
                table[i] = j;
        }
        else {
            /* code not in table */
            i = start[k];
            if ((i >> m) > 4096) {
                /* CVE-2006-4337 */
                fprintf(stderr, "Error: Bad table (case c)");
                exit(1);
            }
            p = &table[i >> m];
            i <<= tablebits;
            n = k - tablebits;
            /* make tree (n length) */
            while (--n >= 0) {
                if (*p == 0) {
                    right[avail] = left[avail] = 0;
                    *p = avail++;
                }
                if (i & 0x8000)
                    p = &right[*p];
                else
                    p = &left[*p];
                i <<= 1;
            }
            *p = j;
        }
        start[k] = l;
    }
}

static void
read_pt_len(short nn, short nbit, short i_special)
{
    int           i, c, n;

    n = getbits(nbit);
    if (n == 0) {
        c = getbits(nbit);
        for (i = 0; i < nn; i++)
            pt_len[i] = 0;
        for (i = 0; i < 256; i++)
            pt_table[i] = c;
    }
    else {
        i = 0;
        while (i < MIN(n, NPT)) {
            c = peekbits(3);
            if (c != 7)
                fillbuf(3);
            else {
                unsigned short  mask = 1 << (16 - 4);
                while (mask & bitbuf) {
                    mask >>= 1;
                    c++;
                }
                fillbuf(c - 3);
            }

            pt_len[i++] = c;
            if (i == i_special) {
                c = getbits(2);
                while (--c >= 0 && i < NPT)
                    pt_len[i++] = 0;
            }
        }
        while (i < nn)
            pt_len[i++] = 0;
        make_table(nn, pt_len, 8, pt_table);
    }
}

static void
read_c_len(void)
{
    short           i, c, n;

    n = getbits(CBIT);
    if (n == 0) {
        c = getbits(CBIT);
        for (i = 0; i < NC; i++)
            c_len[i] = 0;
        for (i = 0; i < 4096; i++)
            c_table[i] = c;
    } else {
        i = 0;
        while (i < MIN(n,NC)) {
            c = pt_table[peekbits(8)];
            if (c >= NT) {
                unsigned short  mask = 1 << (16 - 9);
                do {
                    if (bitbuf & mask)
                        c = right[c];
                    else
                        c = left[c];
                    mask >>= 1;
                } while (c >= NT && (mask || c != left[c])); /* CVE-2006-4338 */
            }
            fillbuf(pt_len[c]);
            if (c <= 2) {
                if (c == 0)
                    c = 1;
                else if (c == 1)
                    c = getbits(4) + 3;
                else
                    c = getbits(CBIT) + 20;
                while (--c >= 0)
                    c_len[i++] = 0;
            }
            else
                c_len[i++] = c - 2;
        }
        while (i < NC)
            c_len[i++] = 0;
        make_table(NC, c_len, 12, c_table);
    }
}

static unsigned short
decode_c_st1(void)
{
    unsigned short  j, mask;

    j = c_table[peekbits(12)];
    if (j < NC)
        fillbuf(c_len[j]);
    else {
        fillbuf(12);
        mask = 1 << (16 - 1);
        do {
            if (bitbuf & mask)
                j = right[j];
            else
                j = left[j];
            mask >>= 1;
        } while (j >= NC && (mask || j != left[j])); /* CVE-2006-4338 */
        fillbuf(c_len[j] - 12);
    }
    return j;
}

static unsigned short
decode_p_st1(void)
{
    unsigned short  j, mask;

    j = pt_table[peekbits(8)];
    if (j < NP)
        fillbuf(pt_len[j]);
    else {
        fillbuf(8);
        mask = 1 << (16 - 1);
        do {
            if (bitbuf & mask)
                j = right[j];
            else
                j = left[j];
            mask >>= 1;
        } while (j >= NP && (mask || j != left[j])); /* CVE-2006-4338 */
        fillbuf(pt_len[j] - 8);
    }
    if (j != 0)
        j = (1 << (j - 1)) + getbits(j - 1);
    return j;
}

static unsigned int
decode(off_t origsize, FILE *outfile)
{
    unsigned short blocksize = 0;
    unsigned int i, c;
    unsigned int dicsiz = 1L << LZHUFF5_DICBIT;
    unsigned int dicsize_mask = dicsiz - 1;
    unsigned int crc = 0;
    off_t decode_count = 0;
    unsigned long loc = 0;
    unsigned char dtext[1 << LZHUFF5_DICBIT];

    memset(dtext, ' ', dicsiz);

    bitbuf = 0;
    subbitbuf = 0;
    bitcount = 0;
    fillbuf(2 * 8);

    while (decode_count < origsize) {
	if (blocksize == 0) {
	    blocksize = getbits(16);
	    read_pt_len(NT, TBIT, 3);
	    read_c_len();
	    read_pt_len(NP, PBIT, -1);
	}
	blocksize--;

        c = decode_c_st1();

        if (c < 256) {
            dtext[loc++] = c;
            if (loc == dicsiz) {
		crc = calccrc(crc, dtext, dicsiz);
                fwrite(dtext, 1, dicsiz, outfile);
                loc = 0;
            }
            decode_count++;
        } else {
	    int length;
            unsigned int position;

            length = c - 256 + THRESHOLD;
            position = (loc - 1 - decode_p_st1()) & dicsize_mask;

            decode_count += length;
            for (i = 0; i < length; i++) {
                c = dtext[(position + i) & dicsize_mask];

                dtext[loc++] = c;
                if (loc == dicsiz) {
                    crc = calccrc(crc, dtext, dicsiz);
		    fwrite(dtext, 1, dicsiz, outfile);
                    loc = 0;
                }
            }
        }
    }

    if (loc != 0) {
	crc = calccrc(crc, dtext, loc);
	fwrite(dtext, 1, loc, outfile);
    }

    return crc;
}

int
main(int argc, char *argv[])
{
    FILE *fp; /* output file */
    char filename[FILENAME_LENGTH];
    unsigned int crc, header_crc;
    unsigned int header_size, original_size, packed_size;

    if (argc != 2) {
        fprintf(stderr, "Error: archive file not specified\n");
        return 1;
    }

    /* open archive file */
    infile = fopen(argv[1], "rb");
    if (!infile) {
        fprintf(stderr, "Error: Failed to open \"%s\": %s",
		argv[1], strerror(errno));
	return 1;
    }

    /* extract each files */
    header_size = lha_header_level1_parse(infile, &original_size,
					  &packed_size, filename,
					  &header_crc);
    if (header_size) {
	fp = fopen(filename, "wb");
	if (!fp) {
	    fprintf(stderr, "Error: Failed to open \"%s\": %s\n",
		    filename, strerror(errno));
	    return 1;
	}

	compsize = packed_size;

	make_crctable();

	crc = decode(original_size, fp);

	if (crc != header_crc)
	    fprintf(stderr, "Error: CRC error: \"%s\"", filename);
    }

    return 0;
}