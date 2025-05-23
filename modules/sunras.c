// This file is part of Deark.
// Copyright (C) 2016 Jason Summers
// See the file COPYING for terms of use.

// Sun Raster image format

#include <deark-config.h>
#include <deark-private.h>
DE_DECLARE_MODULE(de_module_sunras);

struct color32desc_type {
	u8 channel_shift[4];
	u8 alpha_code; // 0=no, 1=yes, 2=autodetect
	const char *name;
};

typedef struct localctx_sunras {
	i64 npwidth, height;
	i64 pdwidth;
	UI depth;

#define RT_OLD          0
#define RT_STANDARD     1
#define RT_BYTE_ENCODED 2
#define RT_FORMAT_RGB   3
#define RT_FORMAT_TIFF  4
#define RT_FORMAT_IFF   5
	UI imgtype;
	u8 is_compressed;
	u8 is_rgb_order;
	u8 user_set_fmt32;

	struct color32desc_type color32desc;

	i64 imglen;

#define RMT_NONE        0
#define RMT_EQUAL_RGB   1
#define RMT_RAW         2
	UI maptype;

	i64 maplen;

	i64 rowspan;
	i64 unc_pixels_size;
	u8 is_paletted;
	u8 is_grayscale;

	de_color pal[256];

	i64 src_bypp;
	UI getrgbflags;
} lctx;

static void do_read_palette(deark *c, lctx *d, i64 pos)
{
	i64 num_entries;
	i64 num_entries_to_read;
	i64 k;
	u8 r, g, b;

	num_entries = d->maplen/3;
	num_entries_to_read = num_entries;
	if(num_entries_to_read>256) num_entries_to_read = 256;

	for(k=0; k<num_entries_to_read; k++) {
		r = de_getbyte(pos + k);
		g = de_getbyte(pos+num_entries + k);
		b = de_getbyte(pos+num_entries*2 + k);
		d->pal[k] = DE_MAKE_RGB(r, g, b);
		de_dbg_pal_entry(c, k, d->pal[k]);
	}
}

static void decode_image_16(deark *c, lctx *d, dbuf *unc_pixels, de_bitmap *img)
{
	i64 i, j;

	for(j=0; j<d->height; j++) {
		for(i=0; i<d->pdwidth; i++) {
			u32 x;
			de_color clr;

			// The 16-bit files I have all come from a Windows game named
			// Sub Culture, and this works for those files. But it's almost
			// certainly not standard.
			x = (u32)dbuf_getu16le(unc_pixels, d->rowspan*j+i*d->src_bypp);
			clr = de_rgb565_to_888(x);
			de_bitmap_setpixel_rgba(img, i, j, clr);
		}
	}
}

static void decode_image_24_32(deark *c, lctx *d, dbuf *unc_pixels, de_bitmap *img)
{
	de_color clr;
	i64 i, j;

	for(j=0; j<d->height; j++) {
		for(i=0; i<d->pdwidth; i++) {
			if(d->depth==24) {
				clr = dbuf_getRGB(unc_pixels, d->rowspan*j+i*d->src_bypp, d->getrgbflags);
				de_bitmap_setpixel_rgb(img, i, j, clr);
			}
			else if(d->depth==32) {
				u8 pixbuf[4];
				dbuf_read(unc_pixels, pixbuf, d->rowspan*j+i*d->src_bypp, 4);
				clr =
					((UI)pixbuf[0] << d->color32desc.channel_shift[0]) |
					((UI)pixbuf[1] << d->color32desc.channel_shift[1]) |
					((UI)pixbuf[2] << d->color32desc.channel_shift[2]) |
					((UI)pixbuf[3] << d->color32desc.channel_shift[3]);
				de_bitmap_setpixel_rgba(img, i, j, clr);
			}
		}
	}
}

static void do_image(deark *c, lctx *d, dbuf *unc_pixels)
{
	de_bitmap *img = NULL;
	u8 nonport_warn_flag = 0;
	i64 dst_bypp;

	if(d->depth!=1 && d->depth!=4 && d->depth!=8 && d->depth!=16 &&
		d->depth!=24 && d->depth!=32)
	{
		de_err(c, "Bit depth %u not supported", d->depth);
		goto done;
	}
	if(d->depth==16) {
		nonport_warn_flag = 1;
	}
	if(d->depth==32 && !d->user_set_fmt32) {
		// Some apps think the extra channel comes first (e.g. xBGR); others
		// think it comes last (BGRx).
		// Some apps think the extra channel is for alpha; others think it is
		// unused.
		// Some apps think the color channels are always in BGR order; others
		// think the order is RGB for RT_FORMAT_RGB format.
		//
		// By default we use ARGB for RT_FORMAT_RGB, and ABGR otherwise, with
		// alpha autodetected (alpha channel is ignored if all values are 0).

		de_warn(c, "32-bit Sun Raster files are not portable. You may have to use "
			"\"-opt sunras:fmt32=...\".");

		d->color32desc.channel_shift[0] = 24; // A or x
		d->color32desc.channel_shift[2] = 8;  // G
		if(d->is_rgb_order) { // xrgb or argb
			d->color32desc.channel_shift[1] = 16; // R
			d->color32desc.channel_shift[3] = 0;  // B
		}
		else { // xbgr or abgr
			d->color32desc.channel_shift[1] = 0;  // B
			d->color32desc.channel_shift[3] = 16; // R
		}

		d->color32desc.alpha_code = 2;
	}
	if(nonport_warn_flag) {
		de_warn(c, "This image type might not be portable");
	}

	if(!de_good_image_dimensions(c, d->npwidth, d->height)) goto done;

	d->src_bypp = (i64)d->depth/8;

	if(d->is_paletted) {
		dst_bypp = 3;
	}
	else if(d->is_grayscale) {
		dst_bypp = 1;
	}
	else if(d->depth==32) {
		if(d->color32desc.alpha_code==0) {
			dst_bypp = 3;
		}
		else {
			dst_bypp = 4;
		}
	}
	else {
		dst_bypp = 3;
	}

	if(d->is_rgb_order) {
		d->getrgbflags = 0;
	}
	else {
		d->getrgbflags = DE_GETRGBFLAG_BGR;
	}

	img = de_bitmap_create2(c, d->npwidth, d->pdwidth, d->height, (int)dst_bypp);

	if(d->depth==16) {
		decode_image_16(c, d, unc_pixels, img);
	}
	else if(d->is_paletted || d->is_grayscale) {
		de_convert_image_paletted(unc_pixels, 0, d->depth, d->rowspan, d->pal, img, 0);
	}
	else {
		decode_image_24_32(c, d, unc_pixels, img);
	}

	if(d->depth==32 && d->color32desc.alpha_code==2) { // autodetect alpha
		de_bitmap_optimize_alpha(img, 0x1);
	}

	de_bitmap_write_to_file(img, NULL, 0);

done:
	de_bitmap_destroy(img);
}

static const char *get_image_type_name(UI t)
{
	const char *name;

	switch(t) {
	case RT_OLD: name="old"; break;
	case RT_STANDARD: name="standard"; break;
	case RT_BYTE_ENCODED: name="RLE"; break;
	case RT_FORMAT_RGB: name="RGB"; break;
	case RT_FORMAT_TIFF: name="TIFF"; break;
	case RT_FORMAT_IFF: name="IFF"; break;
	case 0xffff: name="experimental"; break;
	default: name="?";
	}
	return name;
}

static const char *get_map_type_name(UI t)
{
	const char *name;

	switch(t) {
	case RMT_NONE: name="NONE"; break;
	case RMT_EQUAL_RGB: name="EQUAL_RGB"; break;
	case RMT_RAW: name="RAW"; break;
	default: name="?";
	}
	return name;
}

static void read_header(deark *c, lctx *d, i64 pos)
{
	de_dbg(c, "header at %"I64_FMT, pos);
	de_dbg_indent(c, 1);

	d->npwidth = de_getu32be(pos+4);
	d->height = de_getu32be(pos+8);
	de_dbg_dimensions(c, d->npwidth, d->height);

	d->depth = (UI)de_getu32be(pos+12);
	de_dbg(c, "depth: %u", d->depth);

	d->imglen = de_getu32be(pos+16);
	d->imgtype = (UI)de_getu32be(pos+20);
	de_dbg(c, "image type=%u (%s), len=%"I64_FMT, d->imgtype,
		get_image_type_name(d->imgtype), d->imglen);
	if(d->imgtype==RT_BYTE_ENCODED) {
		d->is_compressed = 1;
	}
	if(d->imgtype==RT_FORMAT_RGB) {
		d->is_rgb_order = 1;
	}

	d->maptype = (UI)de_getu32be(pos+24);
	d->maplen = de_getu32be(pos+28);
	de_dbg(c, "map type=%u (%s), len=%"I64_FMT, d->maptype,
		get_map_type_name(d->maptype), d->maplen);

	de_dbg_indent(c, -1);
}

static void do_decompress_image(deark *c, lctx *d, i64 pos1, i64 len, dbuf *unc_pixels)
{
	i64 pos = pos1;

	while(1) {
		u8 b0, b1, b2;

		// Stop if we reach the end of the input file.
		if(pos >= c->infile->len) break;

		b0 = de_getbyte(pos++);
		if(b0==0x80) {
			b1 = de_getbyte(pos++);
			if(b1==0x00) { // An escaped 0x80 byte
				dbuf_writebyte(unc_pixels, 0x80);
			}
			else { // A compressed run
				b2 = de_getbyte(pos++);
				dbuf_write_run(unc_pixels, b2, (i64)b1+1);
			}
		}
		else { // An uncompressed byte
			dbuf_writebyte(unc_pixels, b0);
		}
	}
}

static void handle_options(deark *c, lctx *d)
{
	const char *fmt32;
	// Table of user-configurable color "descriptors" for 32-bit images.
	static const struct color32desc_type color32desc_arr[] = {
		{ { 0,  8, 16, 24 }, 0, "bgrx" },
		{ { 0,  8, 16, 24 }, 1, "bgra" },
		{ {16,  8,  0, 24 }, 0, "rgbx" },
		{ {16,  8,  0, 24 }, 1, "rgba" },
		{ {24,  0,  8, 16 }, 0, "xbgr" },
		{ {24,  0,  8, 16 }, 1, "abgr" },
		{ {24, 16,  8,  0 }, 0, "xrgb" },
		{ {24, 16,  8,  0 }, 1, "argb" }
	};

	fmt32 = de_get_ext_option(c, "sunras:fmt32");
	if(fmt32) {
		size_t k;
		for(k=0; k<DE_ARRAYCOUNT(color32desc_arr); k++) {
			if(!de_strcmp(fmt32, color32desc_arr[k].name)) {
				d->color32desc = color32desc_arr[k]; // struct copy
				d->user_set_fmt32 = 1;
				break;
			}
		}
	}
}

static void de_run_sunras(deark *c, de_module_params *mparams)
{
	lctx *d = NULL;
	dbuf *unc_pixels = NULL;
	i64 pos;
	i64 bits_per_row;
	int saved_indent_level;
	de_dbg_indent_save(c, &saved_indent_level);

	d = de_malloc(c, sizeof(lctx));
	handle_options(c, d);

	pos = 0;
	read_header(c, d, pos);
	pos += 32;

	if(pos >= c->infile->len) goto done;

	if(d->maplen > 0)
		de_dbg(c, "colormap at %"I64_FMT, pos);

	de_dbg_indent(c, 1);

	if(d->maptype==RMT_EQUAL_RGB) {
		if(d->depth<=8) {
			d->is_paletted = 1;
			do_read_palette(c, d, pos);
		}
		else {
			de_err(c, "This type of image is not supported");
			goto done;
		}
	}
	else if(d->maptype==RMT_NONE) {
		if(d->depth<=8) {
			d->is_grayscale = 1;
			de_make_grayscale_palette(d->pal, ((i64)1)<<d->depth, d->depth==1 ? 1 : 0);
		}
	}
	else {
		// TODO: Support RMT_RAW
		de_err(c, "Colormap type (%u) is not supported", d->maptype);
		goto done;
	}
	pos += d->maplen;
	de_dbg_indent(c, -1);

	if(pos >= c->infile->len) goto done;
	de_dbg(c, "image data at %"I64_FMT, pos);
	de_dbg_indent(c, 1);

	bits_per_row = de_pad_to_n(d->npwidth * (i64)d->depth, 16);
	d->rowspan = bits_per_row / 8;
	d->pdwidth = bits_per_row / (i64)d->depth;
	d->unc_pixels_size = d->rowspan * d->height;

	if(d->imgtype>5) {
		de_err(c, "This type of image (%u) is not supported", d->imgtype);
		goto done;
	}

	if((d->imgtype==RT_STANDARD || d->imgtype==RT_FORMAT_RGB) && d->imglen!=d->unc_pixels_size) {
		de_warn(c, "Inconsistent image length: reported=%"I64_FMT", calculated=%"I64_FMT,
			d->imglen, d->unc_pixels_size);
	}

	if(d->is_compressed) {
		unc_pixels = dbuf_create_membuf(c, d->unc_pixels_size, 0x1);
		dbuf_enable_wbuffer(unc_pixels);
		do_decompress_image(c, d, pos, c->infile->len - pos, unc_pixels);
		dbuf_flush(unc_pixels);
	}
	else {
		unc_pixels = dbuf_open_input_subfile(c->infile, pos, c->infile->len - pos);
	}

	do_image(c, d, unc_pixels);
	de_dbg_indent(c, -1);

done:
	dbuf_close(unc_pixels);
	de_free(c, d);
	de_dbg_indent_restore(c, saved_indent_level);
}

static int de_identify_sunras(deark *c)
{
	if(!dbuf_memcmp(c->infile, 0, "\x59\xa6\x6a\x95", 4))
		return 100;
	return 0;
}

static void de_help_sunras(deark *c)
{
	de_msg(c, "-opt sunras:fmt32=<"
		"xbgr|abgr|xrgb|argb|"
		"bgrx|bgra|rgbx|rgba> : The interpretation of a 32-bit pixel");
}

void de_module_sunras(deark *c, struct deark_module_info *mi)
{
	mi->id = "sunras";
	mi->desc = "Sun Raster";
	mi->run_fn = de_run_sunras;
	mi->identify_fn = de_identify_sunras;
	mi->help_fn = de_help_sunras;
}
