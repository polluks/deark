// This file is part of Deark, by Jason Summers.
// This software is in the public domain. See the file COPYING for details.

// This file is for miscellaneous formats that are easy to support.
// Combining them in one file speeds up compilation and development time.

#include <deark-config.h>
#include <deark-modules.h>

// **************************************************************************
// "copy" module
//
// This is a trivial module that makes a copy of the input file.
// **************************************************************************

static int de_identify_copy(deark *c)
{
	return 0;
}

static void de_run_copy(deark *c, const char *params)
{
	de_dbg(c, "In copy module\n");

	dbuf_create_file_from_slice(c->infile, 0, c->infile->len, "bin", NULL);
}

void de_module_copy(deark *c, struct deark_module_info *mi)
{
	mi->id = "copy";
	mi->run_fn = de_run_copy;
	mi->identify_fn = de_identify_copy;
}

// **************************************************************************
// zlib module
//
// This module is for decompressing zlib-compressed files.
// It uses the deark-miniz.c utilities, which in turn use miniz.c.
// **************************************************************************

static void de_run_zlib(deark *c, const char *params)
{
	dbuf *f = NULL;

	de_dbg(c, "In zlib module\n");

	f = dbuf_create_output_file(c, "unc", NULL);
	de_uncompress_zlib(c->infile, 0, c->infile->len, f);
	dbuf_close(f);
}

static int de_identify_zlib(deark *c)
{
	de_byte b[2];
	de_read(b, 0, 2);

	if((b[0]&0x0f) != 8)
		return 0;

	if(b[0]<0x08 || b[0]>0x78)
		return 0;

	if(((((unsigned int)b[0])<<8)|b[1])%31 != 0)
		return 0;

	return 50;
}

void de_module_zlib(deark *c, struct deark_module_info *mi)
{
	mi->id = "zlib";
	mi->run_fn = de_run_zlib;
	mi->identify_fn = de_identify_zlib;
}

// **************************************************************************
// HP 100LX / HP 200LX .ICN icon format
// **************************************************************************

static void de_run_hpicn(deark *c, const char *params)
{
	struct deark_bitmap *img = NULL;
	de_int64 width, height;
	de_int64 src_rowspan;
	de_int64 j;

	de_dbg(c, "In hpicn module\n");

	width = de_getui16le(4);
	height = de_getui16le(6);

	img = de_bitmap_create(c, width, height, 1);
	src_rowspan = (width+7)/8;

	for(j=0; j<height; j++) {
		de_convert_row_bilevel(c->infile, 8+j*src_rowspan, img, j, 1);
	}

	de_bitmap_write_to_file(img, NULL);
	de_bitmap_destroy(img);
}

static int de_identify_hpicn(deark *c)
{
	de_byte b[8];
	de_read(b, 0, 8);
	if(!de_memcmp(b, "\x01\x00\x01\x00\x2c\x00\x20\x00", 8))
		return 100;
	if(!de_memcmp(b, "\x01\x00\x01\x00", 4))
		return 60;
	return 0;
}

void de_module_hpicn(deark *c, struct deark_module_info *mi)
{
	mi->id = "hpicn";
	mi->run_fn = de_run_hpicn;
	mi->identify_fn = de_identify_hpicn;
}


// **************************************************************************
// X11 "puzzle" format
// ftp://ftp.x.org/pub/unsupported/programs/puzzle/
// This is the format generated by Netpbm's ppmtopuzz utility.
// **************************************************************************

struct xpuzzctx {
	de_int64 w, h;
	de_int64 palentries;
};

static int xpuzz_read_header(deark *c, struct xpuzzctx *lctx)
{
	lctx->w = de_getui32be(0);
	lctx->h = de_getui32be(4);
	lctx->palentries = (de_int64)de_getbyte(8);
	if(lctx->w<1 || lctx->w>DE_MAX_IMAGE_DIMENSION) return 0;
	if(lctx->h<1 || lctx->h>DE_MAX_IMAGE_DIMENSION) return 0;
	if(lctx->palentries==0) lctx->palentries = 256;
	return 1;
}

static void de_run_xpuzzle(deark *c, const char *params)
{
	struct xpuzzctx *lctx = NULL;
	struct deark_bitmap *img = NULL;
	de_int64 i, j;
	de_uint32 pal[256];
	de_int64 p;

	de_dbg(c, "In xpuzzle module\n");

	lctx = de_malloc(c, sizeof(struct xpuzzctx));
	if(!xpuzz_read_header(c, lctx)) goto done;

	img = de_bitmap_create(c, lctx->w, lctx->h, 3);

	// Read the palette
	de_memset(pal, 0, sizeof(pal));
	p = 9;
	for(i=0; i<lctx->palentries; i++) {
		pal[i] = DE_MAKE_RGB(de_getbyte(p), de_getbyte(p+1), de_getbyte(p+2));
		p+=3;
	}

	// Read the bitmap
	for(j=0; j<lctx->h; j++) {
		for(i=0; i<lctx->w; i++) {
			de_bitmap_setpixel_rgb(img, i, j, pal[de_getbyte(p+lctx->w*j+i)]);
		}
	}

	de_bitmap_write_to_file(img, NULL);

done:
	de_bitmap_destroy(img);
	de_free(c, lctx);
}

static int de_identify_xpuzzle(deark *c)
{
	struct xpuzzctx *lctx = NULL;
	int retval = 0;

	lctx = de_malloc(c, sizeof(struct xpuzzctx));

	if(!xpuzz_read_header(c, lctx)) goto done;

	if(lctx->w * lctx->h + 3*lctx->palentries + 9 == c->infile->len) {
		retval = 20;
	}

done:
	de_free(c, lctx);
	return retval;
}

void de_module_xpuzzle(deark *c, struct deark_module_info *mi)
{
	mi->id = "xpuzzle";
	mi->run_fn = de_run_xpuzzle;
	mi->identify_fn = de_identify_xpuzzle;
}

// **************************************************************************
// DCX
// **************************************************************************

static void de_run_dcx(deark *c, const char *params)
{
	de_uint32 *page_offset;
	de_int64 num_pages;
	de_int64 page;
	de_int64 page_size;

	de_dbg(c, "In dcx module\n");

	page_offset = de_malloc(c, 1023 * sizeof(de_uint32));
	num_pages = 0;
	while(num_pages < 1023) {
		page_offset[num_pages] = (de_uint32)de_getui32le(4 + 4*num_pages);
		if(page_offset[num_pages]==0)
			break;
		num_pages++;
	}

	de_dbg(c, "number of pages: %d\n", (int)num_pages);

	for(page=0; page<num_pages; page++) {
		if(page == num_pages-1) {
			// Last page. Asssume it goes to the end of file.
			page_size = c->infile->len - page_offset[page];
		}
		else {
			page_size = page_offset[page+1] - page_offset[page];
		}
		if(page_size<0) page_size=0;
		de_dbg(c, "page %d at %d, size=%d\n", (int)page, (int)page_offset[page],
			(int)page_size);

		dbuf_create_file_from_slice(c->infile, page_offset[page], page_size, "pcx", NULL);
	}
}

static int de_identify_dcx(deark *c)
{
	de_byte b[4];
	de_read(b, 0, 4);
	if(!de_memcmp(b, "\xb1\x68\xde\x3a", 4))
		return 100;
	return 0;
}

void de_module_dcx(deark *c, struct deark_module_info *mi)
{
	mi->id = "dcx";
	mi->run_fn = de_run_dcx;
	mi->identify_fn = de_identify_dcx;
}
