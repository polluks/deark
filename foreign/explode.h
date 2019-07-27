// ZIP type 6 "implode" decompression.
// Based on Mark Adler's public domain code from Info-ZIP UnZip v5.4.
// See the file readme-explode.txt for more information.
// The code has been heavily modified for Deark (2019-03).
// This file (explode.h) is hereby left in the public domain; or it may, at
// your option, be distributed under the same terms as the main Deark software.
// -JS

#ifndef UI6A_CALLOC
#define UI6A_CALLOC(u, nmemb, size) calloc(nmemb, size)
#endif
#ifndef UI6A_FREE
#define UI6A_FREE(u, ptr) free(ptr)
#endif

//========================= unzip.h begin =========================

typedef unsigned char   uch;    /* code assumes unsigned bytes; these type-  */
typedef u16   ush;    /*  defs replace byte/UWORD/ULONG (which are */
typedef u32   ulg;    /*  predefined on some systems) & match zip  */

#define UI6A_OK   0
#define UI6A_ERR1 1
#define UI6A_ERR2 2
#define UI6A_ERR3 3
#define UI6A_ERR4 4

//========================= unzpriv.h begin =========================

#define UI6A_WSIZE 0x8000  /* window size--must be a power of two, and */
                      /* at least 8K for zip's implode method */
                      /* (at least 32K for zip's deflate method) */

#define HUFT_ARRAY_OFFSET 0

struct ui6a_huftarray;

struct ui6a_huft {
	uch e;                /* number of extra bits or operation */
	uch b;                /* number of bits in this code or subcode */
	ush n;            /* literal, length base, or distance base */
	struct ui6a_huftarray *t_arr;   /* pointer to next level of table */
};

struct ui6a_huftarray {
	unsigned int num_alloc_h;
	struct ui6a_huft *h;
	struct ui6a_huftarray *next_array;
};

struct ui6a_htable {
	struct ui6a_huftarray *first_array;
	int b; /* bits for this table */
	const char *tblname;
};

struct ui6a_htables {
	struct ui6a_htable b; /* literal code table */
	struct ui6a_htable l; /* length code table */
	struct ui6a_htable d; /* distance code table */
};

//========================= globals.h begin =========================

struct ui6a_ctx_struct;
typedef struct ui6a_ctx_struct ui6a_ctx;

typedef int (*ui6a_cb_readbyte_type)(ui6a_ctx *ui6a);
typedef void (*ui6a_cb_flush_type)(ui6a_ctx *ui6a, uch *rawbuf, ulg size);
typedef void (*ui6a_cb_post_read_trees_type)(ui6a_ctx *ui6a, struct ui6a_htables *tbls);

struct ui6a_ctx_struct {
	void *userdata;
	i64 csize;           /* used by decompr. (UI6A_NEXTBYTE): must be signed */
	i64 ucsize;          /* used by unReduce(), explode() */
	ush lrec_general_purpose_bit_flag;

	// Originally:
	/* refill inbuf and return a byte if available, else EOF */
	// Currently, we don't bother with ui6a->inbuf, though that would be more
	// efficient. The UI6A_NEXTBYTE macro has been modified to not use inbuf.
	ui6a_cb_readbyte_type cb_readbyte;

	ui6a_cb_flush_type cb_flush;
	ui6a_cb_post_read_trees_type cb_post_read_trees;
	uch Slide[UI6A_WSIZE];
};

//========================= globals.h end =========================

typedef ush (*ui6a_len_or_dist_getter)(unsigned int i);

static void ui6a_huft_free(ui6a_ctx *ui6a, struct ui6a_htable *tbl);
static int ui6a_huft_build(ui6a_ctx *ui6a, const unsigned *b, unsigned n,
	unsigned s, ui6a_len_or_dist_getter d_fn, ui6a_len_or_dist_getter e_fn,
	struct ui6a_htable *tbl);

#define UI6A_NEXTBYTE  (ui6a->cb_readbyte(ui6a))

//========================= unzpriv.h end =========================

//========================= unzip.h end =========================

//========================= consts.h begin =========================

static ush ui6a_get_mask_bits(unsigned int n)
{
	if(n>=17) return 0;
	return (ush)(0xffffU >> (16-n));
}

//========================= consts.h end =========================

//========================= explode.c begin =========================

/* explode.c -- put in the public domain by Mark Adler
   version c15, 6 July 1996 */


/* You can do whatever you like with this source file, though I would
   prefer that if you modify it and redistribute it that you include
   comments to that effect with your name and the date.  Thank you.

   [...]
 */


/*
   Explode imploded (PKZIP method 6 compressed) data.  This compression
   method searches for as much of the current string of bytes (up to a length
   of ~320) in the previous 4K or 8K bytes.  If it doesn't find any matches
   (of at least length 2 or 3), it codes the next byte.  Otherwise, it codes
   the length of the matched string and its distance backwards from the
   current position.  Single bytes ("literals") are preceded by a one (a
   single bit) and are either uncoded (the eight bits go directly into the
   compressed stream for a total of nine bits) or Huffman coded with a
   supplied literal code tree.  If literals are coded, then the minimum match
   length is three, otherwise it is two.

   There are therefore four kinds of imploded streams: 8K search with coded
   literals (min match = 3), 4K search with coded literals (min match = 3),
   8K with uncoded literals (min match = 2), and 4K with uncoded literals
   (min match = 2).  The kind of stream is identified in two bits of a
   general purpose bit flag that is outside of the compressed stream.

   Distance-length pairs for matched strings are preceded by a zero bit (to
   distinguish them from literals) and are always coded.  The distance comes
   first and is either the low six (4K) or low seven (8K) bits of the
   distance (uncoded), followed by the high six bits of the distance coded.
   Then the length is six bits coded (0..63 + min match length), and if the
   maximum such length is coded, then it's followed by another eight bits
   (uncoded) to be added to the coded length.  This gives a match length
   range of 2..320 or 3..321 bytes.

   The literal, length, and distance codes are all represented in a slightly
   compressed form themselves.  What is sent are the lengths of the codes for
   each value, which is sufficient to construct the codes.  Each byte of the
   code representation is the code length (the low four bits representing
   1..16), and the number of values sequentially with that length (the high
   four bits also representing 1..16).  There are 256 literal code values (if
   literals are coded), 64 length code values, and 64 distance code values,
   in that order at the beginning of the compressed stream.  Each set of code
   values is preceded (redundantly) with a byte indicating how many bytes are
   in the code description that follows, in the range 1..256.

   The codes themselves are decoded using tables made by ui6a_huft_build() from
   the bit lengths.
 */


/* The implode algorithm uses a sliding 4K or 8K byte window on the
   uncompressed stream to find repeated byte strings.  This is implemented
   here as a circular buffer.  The index is updated simply by incrementing
   and then and'ing with 0x0fff (4K-1) or 0x1fff (8K-1).  Here, the 32K
   buffer of inflate is used, and it works just as well to always have
   a 32K circular buffer, so the index is anded with 0x7fff.  This is
   done to allow the window to also be used as the output buffer. */
/* This must be supplied in an external module useable like "uch slide[8192];"
   or "uch *slide;", where the latter would be malloc'ed.  In unzip, slide[]
   is actually a 32K area for use by inflate, which uses a 32K sliding window.
 */


/* (virtual) Tables for length and distance */

static ush ui6a_get_cplen2(unsigned int i)
{
	if(i>=64) return 0;
	return i+2;
}

static ush ui6a_get_cplen3(unsigned int i)
{
	if(i>=64) return 0;
	return i+3;
}

static ush ui6a_get_extra(unsigned int i)
{
	return (i==63) ? 8 : 0;
}

static ush ui6a_get_cpdist4(unsigned int i)
{
	if(i>=64) return 0;
	return 1 + i*64;
}

static ush ui6a_get_cpdist8(unsigned int i)
{
	if(i>=64) return 0;
	return 1 + i*128;
}

/* Macros for inflate() bit peeking and grabbing.
   The usage is:

        UI6A_NEEDBITS(j);
        x = b & mask_bits[j];
        UI6A_DUMPBITS(j);

   where UI6A_NEEDBITS makes sure that b has at least j bits in it, and
   UI6A_DUMPBITS removes the bits from b.  The macros use the variable k
   for the number of bits in b.  Normally, b and k are register
   variables for speed.
 */

#define UI6A_NEEDBITS(n) do {while(k<(n)){b|=((ulg)UI6A_NEXTBYTE)<<k;k+=8;}} while(0)
#define UI6A_DUMPBITS(n) do {b>>=(n);k-=(n);} while(0)

struct ui6a_iarray {
	size_t count;
	int *data;
	ui6a_ctx *ui6a;
};

struct ui6a_uarray {
	size_t count;
	unsigned int *data;
	ui6a_ctx *ui6a;
};

static void ui6a_iarray_init(ui6a_ctx *ui6a, struct ui6a_iarray *a, int *data, size_t count)
{
	de_zeromem(data, count * sizeof(int));
	a->data = data;
	a->count = count;
	a->ui6a = ui6a;
}

static void ui6a_uarray_init(ui6a_ctx *ui6a, struct ui6a_uarray *a, unsigned int *data, size_t count)
{
	de_zeromem(data, count * sizeof(unsigned int));
	a->data = data;
	a->count = count;
	a->ui6a = ui6a;
}

static void ui6a_iarray_setval(struct ui6a_iarray *a, size_t idx, int val)
{
	if(idx<a->count) {
		a->data[idx] = val;
	}
}

static void ui6a_uarray_setval(struct ui6a_uarray *a, size_t idx, unsigned int val)
{
	if(idx<a->count) {
		a->data[idx] = val;
	}
}

static int ui6a_iarray_getval(struct ui6a_iarray *a, size_t idx)
{
	if(idx<a->count) {
		return a->data[idx];
	}
	return 0;
}

static unsigned int ui6a_uarray_getval(struct ui6a_uarray *a, size_t idx)
{
	if(idx<a->count) {
		return a->data[idx];
	}
	return 0;
}

/* Get the bit lengths for a code representation from the compressed
   stream.  If ui6a_get_tree() returns 4, then there is an error in the data.
   Otherwise zero is returned. */
// l: bit lengths
// n: number expected
static int ui6a_get_tree(ui6a_ctx *ui6a, unsigned *l, unsigned n)
{
	unsigned i;           /* bytes remaining in list */
	unsigned k;           /* lengths entered */
	unsigned j;           /* number of codes */
	unsigned b;           /* bit length for those codes */

	/* get bit lengths */
	i = UI6A_NEXTBYTE + 1;                     /* length/count pairs to read */
	k = 0;                                /* next code */
	do {
		b = ((j = UI6A_NEXTBYTE) & 0xf) + 1;     /* bits in code (1..16) */
		j = ((j & 0xf0) >> 4) + 1;          /* codes with those bits (1..16) */
		if (k + j > n)
			return UI6A_ERR4;                         /* don't overflow l[] */
		do {
			l[k++] = b;
		} while (--j);
	} while (--i);
	return k != n ? UI6A_ERR4 : UI6A_OK;                /* should have read n of them */
}

static struct ui6a_huft *ui6a_huftarr_plus_offset(struct ui6a_huftarray *ha, ulg offset)
{
	ulg real_offset;

	real_offset = HUFT_ARRAY_OFFSET+offset;
	if(real_offset >= ha->num_alloc_h) {
		return NULL;
	}
	return &(ha->h[real_offset]);
}

static struct ui6a_huft *ui6a_follow_huft_ptr(struct ui6a_huft *h1, ulg offset)
{
	return ui6a_huftarr_plus_offset(h1->t_arr, offset);
}

// tb, tl, td: literal, length, and distance tables
//  Uses literals if tbls->b.t!=NULL.
// bb, bl, bd: number of bits decoded by those
static int ui6a_explode_internal(ui6a_ctx *ui6a, unsigned window_k,
	struct ui6a_htables *tbls)
{
	i64 s;               /* bytes to decompress */
	unsigned e;  /* table entry flag/number of extra bits */
	unsigned n, d;        /* length and index for copy */
	unsigned w;           /* current window position */
	struct ui6a_huft *t;       /* pointer to table entry */
	unsigned mb, ml, md;  /* masks for bb, bl, and bd bits */
	ulg b;       /* bit buffer */
	unsigned k;  /* number of bits in bit buffer */
	unsigned u;           /* true if unflushed */

	/* explode the coded data */
	b = k = w = 0;                /* initialize bit buffer, window */
	u = 1;                        /* buffer unflushed */
	mb = ui6a_get_mask_bits(tbls->b.b);           /* precompute masks for speed */
	ml = ui6a_get_mask_bits(tbls->l.b);
	md = ui6a_get_mask_bits(tbls->d.b);
	s = ui6a->ucsize;
	while (s > 0) {               /* do until ucsize bytes uncompressed */
		UI6A_NEEDBITS(1);
		if (b & 1) {                /* then literal--decode it */
			UI6A_DUMPBITS(1);
			s--;
			if(tbls->b.first_array) {
				UI6A_NEEDBITS((unsigned)tbls->b.b);    /* get coded literal */
				t = ui6a_huftarr_plus_offset(tbls->b.first_array, ((~(unsigned)b) & mb));
				if(!t) goto done;
				e = t->e;
				if (e > 16) {
					do {
						if (e == 99)
							return 1;
						UI6A_DUMPBITS(t->b);
						e -= 16;
						UI6A_NEEDBITS(e);
						t = ui6a_follow_huft_ptr(t, ((~(unsigned)b) & ui6a_get_mask_bits(e)));
						if(!t) goto done;
						e = t->e;
					} while (e > 16);
				}
				UI6A_DUMPBITS(t->b);
				ui6a->Slide[w++] = (uch)t->n;
			}
			else {
				UI6A_NEEDBITS(8);
				ui6a->Slide[w++] = (uch)b;
			}
			if (w == UI6A_WSIZE) {
				ui6a->cb_flush(ui6a, ui6a->Slide, (ulg)w);
				w = u = 0;
			}
			if(!tbls->b.first_array) {
				UI6A_DUMPBITS(8);
			}
		}
		else {                      /* else distance/length */
			UI6A_DUMPBITS(1);

			if(window_k==8) {
				UI6A_NEEDBITS(7);               /* get distance low bits */
				d = (unsigned)b & 0x7f;
				UI6A_DUMPBITS(7);
			}
			else {
				UI6A_NEEDBITS(6);               /* get distance low bits */
				d = (unsigned)b & 0x3f;
				UI6A_DUMPBITS(6);
			}

			UI6A_NEEDBITS((unsigned)tbls->d.b);    /* get coded distance high bits */
			t = ui6a_huftarr_plus_offset(tbls->d.first_array, ((~(unsigned)b) & md));
			if(!t) goto done;
			e = t->e;
			if (e > 16) {
				do {
					if (e == 99)
						return 1;
					UI6A_DUMPBITS(t->b);
					e -= 16;
					UI6A_NEEDBITS(e);
					t = ui6a_follow_huft_ptr(t, ((~(unsigned)b) & ui6a_get_mask_bits(e)));
					if(!t) goto done;
					e = t->e;
				} while (e > 16);
			}
			UI6A_DUMPBITS(t->b);
			d = w - d - t->n;       /* construct offset */
			UI6A_NEEDBITS((unsigned)tbls->l.b);    /* get coded length */
			t = ui6a_huftarr_plus_offset(tbls->l.first_array, ((~(unsigned)b) & ml));
			if(!t) goto done;
			e = t->e;
			if (e > 16) {
				do {
					if (e == 99)
						return 1;
					UI6A_DUMPBITS(t->b);
					e -= 16;
					UI6A_NEEDBITS(e);
					t = ui6a_follow_huft_ptr(t, ((~(unsigned)b) & ui6a_get_mask_bits(e)));
					if(!t) goto done;
					e = t->e;
				} while (e > 16);
			}
			UI6A_DUMPBITS(t->b);
			n = t->n;
			if (e) {                  /* get length extra bits */
				UI6A_NEEDBITS(8);
				n += (unsigned)b & 0xff;
				UI6A_DUMPBITS(8);
			}

			/* do the copy */
			s -= n;
			do {
				d &= (UI6A_WSIZE-1);
				e = UI6A_WSIZE - (d > w ? d : w);
				if(e>n) { e = n; }
				n -= e;
				if (u && w <= d) {
					if(w+e > UI6A_WSIZE) goto done;
					de_zeromem(&ui6a->Slide[w], e);
					w += e;
					d += e;
				}
				else {
					if (w - d >= e) {     /* (this test assumes unsigned comparison) */
						if(w+e > UI6A_WSIZE) goto done;
						if(d+e > UI6A_WSIZE) goto done;
						de_memcpy(&ui6a->Slide[w], &ui6a->Slide[d], e);
						w += e;
						d += e;
					}
					else {                 /* do it slow to avoid memcpy() overlap */
						do {
							if(w >= UI6A_WSIZE) goto done;
							if(d >= UI6A_WSIZE) goto done;
							ui6a->Slide[w++] = ui6a->Slide[d++];
						} while (--e);
					}
				}
				if (w == UI6A_WSIZE) {
					ui6a->cb_flush(ui6a, ui6a->Slide, (ulg)w);
					w = u = 0;
				}
			} while (n);
		}
	}

	/* flush out ui6a->Slide */
	ui6a->cb_flush(ui6a, ui6a->Slide, (ulg)w);
done:
	return 0;
}

/* Explode an imploded compressed stream.  Based on the general purpose
   bit flag, decide on coded or uncoded literals, and an 8K or 4K sliding
   window.  Construct the literal (if any), length, and distance codes and
   the tables needed to decode them (using ui6a_huft_build(),
   and call the appropriate routine for the type of data in the remainder
   of the stream.  The four routines are nearly identical, differing only
   in whether the literal is decoded or simply read in, and in how many
   bits are read in, uncoded, for the low distance bits. */
static int ui6a_explode(ui6a_ctx *ui6a)
{
	unsigned r = 1;           /* return codes */
	struct ui6a_htables tbls;
	unsigned l[256];      /* bit lengths for codes */
	int has_literal_tree;
	int has_8k_window;

	de_zeromem(&tbls, sizeof(struct ui6a_htables));
	tbls.b.tblname = "B";
	tbls.l.tblname = "L";
	tbls.d.tblname = "D";

	has_8k_window = (ui6a->lrec_general_purpose_bit_flag & 2) ? 1 : 0;
	has_literal_tree = (ui6a->lrec_general_purpose_bit_flag & 4) ? 1 : 0;

	/* Tune base table sizes.  Note: I thought that to truly optimize speed,
	   I would have to select different bl, bd, and bb values for different
	   compressed file sizes.  I was surprised to find out that the values of
	   7, 7, and 9 worked best over a very wide range of sizes, except that
	   bd = 8 worked marginally better for large compressed sizes. */
	tbls.l.b = 7;
	tbls.d.b = ui6a->csize > 200000L ? 8 : 7;

	if (has_literal_tree) { /* With literal tree--minimum match length is 3 */
		tbls.b.b = 9;                     /* base table size for literals */
		if ((r = ui6a_get_tree(ui6a, l, 256)) != UI6A_OK)
			goto done;
		if ((r = ui6a_huft_build(ui6a, l, 256, 256, NULL, NULL, &tbls.b)) != UI6A_OK)
			goto done;
	}
	else {  /* No literal tree--minimum match length is 2 */
		tbls.b.first_array = NULL;
	}

	if ((r = ui6a_get_tree(ui6a, l, 64)) != UI6A_OK)
		goto done;
	if ((r = ui6a_huft_build(ui6a, l, 64, 0, (has_literal_tree ? ui6a_get_cplen3 : ui6a_get_cplen2),
		ui6a_get_extra, &tbls.l)) != UI6A_OK)
	{
		goto done;
	}

	if ((r = ui6a_get_tree(ui6a, l, 64)) != UI6A_OK)
		goto done;
	if ((r = ui6a_huft_build(ui6a, l, 64, 0, (has_8k_window ? ui6a_get_cpdist8 : ui6a_get_cpdist4),
		ui6a_get_extra, &tbls.d)) != UI6A_OK)
	{
		goto done;
	}

	if(ui6a->cb_post_read_trees) {
		ui6a->cb_post_read_trees(ui6a, &tbls);
	}

	r = ui6a_explode_internal(ui6a, (has_8k_window ? 8 : 4), &tbls);

done:
	ui6a_huft_free(ui6a, &tbls.d);
	ui6a_huft_free(ui6a, &tbls.l);
	ui6a_huft_free(ui6a, &tbls.b);
	return (int)r;
}

#undef UI6A_NEXTBYTE
#undef UI6A_NEEDBITS
#undef UI6A_DUMPBITS

//========================= explode.c end =========================

//========================= inflate.c begin =========================

/* inflate.c -- put in the public domain by Mark Adler
   version c16b, 29 March 1998 */


/* If UI6A_BMAX needs to be larger than 16, then h and x[] should be ulg. */
#define UI6A_BMAX 16         /* maximum bit length of any code (16 for explode) */
#define UI6A_N_MAX 288       /* maximum number of codes in any set */

/* Given a list of code lengths and a maximum table size, make a set of
   tables to decode that set of codes.  Return zero on success, one if
   the given code set is incomplete (the tables are still built in this
   case), two if the input is invalid (all zero length codes or an
   oversubscribed set of lengths), and three if not enough memory.
   The code with value 256 is special, and the tables are constructed
   so that no bits beyond that code are fetched when that code is
   decoded. */
// b: code lengths in bits (all assumed <= UI6A_BMAX)
// n: number of codes (assumed <= UI6A_N_MAX)
// s: number of simple-valued codes (0..s-1)
// d: list of base values for non-simple codes
// e: list of extra bits for non-simple codes
// tbl->t: result: starting table
// tbl->b: maximum lookup bits, returns actual
static int ui6a_huft_build(ui6a_ctx *ui6a, const unsigned *b, unsigned n, unsigned s,
	ui6a_len_or_dist_getter d_fn, ui6a_len_or_dist_getter e_fn,
	struct ui6a_htable *tbl)
{
	unsigned a;                   /* counter for codes of length k */
	struct ui6a_uarray c_arr;           /* bit length count table */
	unsigned c_data[UI6A_BMAX+1];
	unsigned el;                  /* length of EOB code (value 256) */
	unsigned f;                   /* i repeats in table every f entries */
	int g;                        /* maximum code length */
	int h;                        /* table level */
	unsigned i;          /* counter, current code */
	unsigned j;          /* counter */
	int k;               /* number of bits in current code */
	struct ui6a_iarray lx_arr;         /* memory for l[-1..UI6A_BMAX-1] */
	int lx_data[UI6A_BMAX+1];          /* &lx[1] = stack of bits per table */
	struct ui6a_huft *q;      /* points to current table */
	struct ui6a_huft r;        /* table entry for structure assignment */
	struct ui6a_huftarray *u[UI6A_BMAX];  /* table stack */
	struct ui6a_uarray v_arr;            /* values in order of bit length */
	unsigned v_data[UI6A_N_MAX];
	int w;               /* bits before this table == (l * h) */
	struct ui6a_uarray x_arr;           /* bit offsets, then code stack */
	unsigned x_data[UI6A_BMAX+1];
	int y;                        /* number of dummy codes added */
	unsigned z;                   /* number of entries in current table */
	unsigned int c_idx;
	unsigned int v_idx;
	unsigned int x_idx;
	int retval = UI6A_ERR2;
	struct ui6a_huftarray **loc_of_prev_next_ha_ptr = &tbl->first_array;

	*loc_of_prev_next_ha_ptr = NULL;
	if(n>256) goto done;

	/* Generate counts for each bit length */
	el = UI6A_BMAX; /* set length of EOB code, if any */
	ui6a_uarray_init(ui6a, &c_arr, c_data, DE_ITEMS_IN_ARRAY(c_data));

	for(i=0; i<n; i++) {
		if(b[i] >= UI6A_BMAX+1) goto done;
		/* assume all entries <= UI6A_BMAX */
		ui6a_uarray_setval(&c_arr, b[i], ui6a_uarray_getval(&c_arr, b[i])+1);
	}

	if (ui6a_uarray_getval(&c_arr, 0) == n) {              /* null input--all zero length codes */
		tbl->b = 0;
		return UI6A_OK;
	}

	/* Find minimum and maximum length, bound *m by those */
	for (j = 1; j <= UI6A_BMAX; j++) {
		if (ui6a_uarray_getval(&c_arr, j))
			break;
	}
	k = j;                        /* minimum code length */
	if ((unsigned)tbl->b < j)
		tbl->b = j;
	for (i = UI6A_BMAX; i; i--) {
		if (ui6a_uarray_getval(&c_arr, i))
			break;
	}
	g = i;                        /* maximum code length */
	if ((unsigned)tbl->b > i)
		tbl->b = i;

	/* Adjust last length count to fill out codes, if needed */
	for (y = 1 << j; j < i; j++, y <<= 1) {
		y -= ui6a_uarray_getval(&c_arr, j);
		if (y < 0)
			return UI6A_ERR2;                 /* bad input: more codes than bits */
	}
	y -= ui6a_uarray_getval(&c_arr, i);
	if (y < 0)
		return UI6A_ERR2;
	ui6a_uarray_setval(&c_arr, i, ui6a_uarray_getval(&c_arr, i) + y);

	/* Generate starting offsets into the value table for each length */
	j = 0;
	ui6a_uarray_init(ui6a, &x_arr, x_data, DE_ITEMS_IN_ARRAY(x_data));
	ui6a_uarray_setval(&x_arr, 1, 0);
	c_idx = 1;
	x_idx = 2;
	while (--i) {                 /* note that i == g from above */
		j += ui6a_uarray_getval(&c_arr, c_idx);
		c_idx++;
		ui6a_uarray_setval(&x_arr, x_idx, j);
		x_idx++;
	}

	/* Make a table of values in order of bit lengths */
	ui6a_uarray_init(ui6a, &v_arr, v_data, DE_ITEMS_IN_ARRAY(v_data));
	for(i=0; i<n; i++) {
		j = b[i];
		if (j != 0) {
			ui6a_uarray_setval(&v_arr, ui6a_uarray_getval(&x_arr, j), i);
			ui6a_uarray_setval(&x_arr, j, ui6a_uarray_getval(&x_arr, j) + 1);
		}
	}
	n = ui6a_uarray_getval(&x_arr, g);                     /* set n to length of v */

	/* Generate the Huffman codes and for each, make the table entries */
	i = 0;                        /* first Huffman code is zero */
	ui6a_uarray_setval(&x_arr, 0, 0);
	v_idx = 0;                    /* grab values in bit order */
	h = -1;                       /* no tables yet--level -1 */
	ui6a_iarray_init(ui6a, &lx_arr, lx_data, DE_ITEMS_IN_ARRAY(lx_data));
	ui6a_iarray_setval(&lx_arr, 0, 0);                    /* no bits decoded yet */
	w = 0;
	u[0] = NULL;                  /* just to keep compilers happy */
	q = NULL;                     /* ditto */
	z = 0;                        /* ditto */

	/* go through the bit lengths (k already is bits in shortest code) */
	for (; k <= g; k++) {
		a = ui6a_uarray_getval(&c_arr, k);
		while (a--) {
			/* here i is the Huffman code of length k bits for value *p */
			/* make tables up to required level */
			while (k > w + ui6a_iarray_getval(&lx_arr, 1+ (size_t)h)) {
				struct ui6a_huftarray *ha;

				w += ui6a_iarray_getval(&lx_arr, 1+ (size_t)h);            /* add bits already decoded */
				h++;

				/* compute minimum size table less than or equal to *m bits */
				z = g - w;
				z = (z > (unsigned)tbl->b) ? ((unsigned)tbl->b) : z;        /* upper limit */
				j = k - w;
				f = 1 << j;
				if (f > a + 1) {   /* try a k-w bit table */
				                   /* too few codes for k-w bit table */
					f -= a + 1;           /* deduct codes from patterns left */

					c_idx = k;
					while (++j < z) {     /* try smaller tables up to z bits */
						c_idx++;
						f <<= 1;
						if (f <= ui6a_uarray_getval(&c_arr, c_idx))
							break;            /* enough codes to use up j bits */
						f -= ui6a_uarray_getval(&c_arr, c_idx);        /* else deduct codes from patterns */
					}
				}
				if ((unsigned)w + j > el && (unsigned)w < el)
					j = el - w;           /* make EOB code end at table */
				z = 1 << j;             /* table entries for j-bit table */
				ui6a_iarray_setval(&lx_arr, 1+ (size_t)h, j);               /* set table size in stack */

				/* allocate and link in new table */
				ha = UI6A_CALLOC(ui6a->userdata, 1, sizeof(struct ui6a_huftarray));
				if(!ha) {
					retval = UI6A_ERR3;
					goto done;
				}
				ha->h = UI6A_CALLOC(ui6a->userdata, (size_t)((i64)z + HUFT_ARRAY_OFFSET), sizeof(struct ui6a_huft));
				if(!ha->h) {
					UI6A_FREE(ui6a->userdata, ha);
					retval = UI6A_ERR3;
					goto done;
				}
				ha->num_alloc_h = z + HUFT_ARRAY_OFFSET;
				q = ha->h;
				*loc_of_prev_next_ha_ptr = ha;             /* link to list for ui6a_huft_free() */
				loc_of_prev_next_ha_ptr = &ha->next_array;
				*loc_of_prev_next_ha_ptr = NULL;
				q += HUFT_ARRAY_OFFSET;
				if(h<0 || h>=UI6A_BMAX) goto done;
				u[h] = ha;

				/* connect to last table, if there is one */
				if (h) {
					de_zeromem(&r, sizeof(struct ui6a_huft));
					ui6a_uarray_setval(&x_arr, h, i);             /* save pattern for backing up */
					r.b = (uch)ui6a_iarray_getval(&lx_arr, 1+ (size_t)h-1);    /* bits to dump before this table */
					r.e = (uch)(16 + j);  /* bits in this table */
					r.t_arr = ha;            /* pointer to this table */
					j = (i & ((1 << w) - 1)) >> (w - ui6a_iarray_getval(&lx_arr, 1+ (size_t)h-1));
					if((h-1 < 0) || (h-1 >= UI6A_BMAX)) goto done;
					u[h-1]->h[HUFT_ARRAY_OFFSET+j] = r;        /* connect to last table */
				}
			}

			/* set up table entry in r */
			de_zeromem(&r, sizeof(struct ui6a_huft));
			r.b = (uch)(k - w);
			if (v_idx >= n) {
				r.e = 99;               /* out of values--invalid code */
			}
			else if (ui6a_uarray_getval(&v_arr, v_idx) < s) {
				r.e = (uch)(ui6a_uarray_getval(&v_arr, v_idx) < 256 ? 16 : 15);  /* 256 is end-of-block code */
				r.n = (ush)ui6a_uarray_getval(&v_arr, v_idx);                /* simple code is just the value */
				v_idx++;
			}
			else {
				r.e = (uch)e_fn(ui6a_uarray_getval(&v_arr, v_idx) - s);   /* non-simple--look up in lists */
				r.n = d_fn(ui6a_uarray_getval(&v_arr, v_idx) - s);
				v_idx++;
			}

			/* fill code-like entries with r */
			f = 1 << (k - w);
			for (j = i >> w; j < z; j += f) {
				q[j] = r;
			}

			/* backwards increment the k-bit code i */
			for (j = 1 << (k - 1); i & j; j >>= 1) {
				i ^= j;
			}
			i ^= j;

			/* backup over finished tables */
			while ((i & ((1 << w) - 1)) != ui6a_uarray_getval(&x_arr, h)) {
				--h;
				w -= ui6a_iarray_getval(&lx_arr, 1+ (size_t)h);            /* don't need to update q */
			}
		}
	}

	/* return actual size of base table */
	tbl->b = ui6a_iarray_getval(&lx_arr, 1+ 0);

	/* Return true (1) if we were given an incomplete table */
	if(y != 0 && g != 1)
		retval = UI6A_ERR1;
	else
		retval = UI6A_OK;

done:
	return retval;
}

/* Free the malloc'ed tables built by ui6a_huft_build(), which makes a linked
   list of the tables it made, with the links in a dummy first entry of
   each table. */
// t: table to free
static void ui6a_huft_free(ui6a_ctx *ui6a, struct ui6a_htable *tbl)
{
	struct ui6a_huftarray *p, *q;

	p = tbl->first_array;
	while(p) {
		q = p->next_array;

		UI6A_FREE(ui6a->userdata, p->h);
		UI6A_FREE(ui6a->userdata, p);
		p = q;
	}
}

//========================= inflate.c end =========================

//========================= globals.c begin =========================

static ui6a_ctx *ui6a_create(void *userdata)
{
	ui6a_ctx *ui6a = UI6A_CALLOC(userdata, 1, sizeof(ui6a_ctx));
	if(!ui6a) return NULL;
	ui6a->userdata = userdata;
	return ui6a;
}

static void ui6a_destroy(ui6a_ctx *ui6a)
{
	UI6A_FREE(ui6a->userdata, ui6a);
}

//========================= globals.c end =========================
