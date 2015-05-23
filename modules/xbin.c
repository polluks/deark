// This file is part of Deark, by Jason Summers.
// This software is in the public domain. See the file COPYING for details.

// XBIN character graphics

#include <deark-config.h>
#include <deark-modules.h>
#include "fmtutil.h"

typedef struct localctx_struct {
	de_int64 width_in_chars, height_in_chars;
	de_int64 width_in_pixels, height_in_pixels;
	de_int64 font_width;
	de_int64 font_height;
	de_byte has_palette, has_font, compression, nonblink, has_512chars;
	de_byte used_blink;

	de_int64 font_num_chars;
	de_int64 font_data_len;
	de_byte *font_data;

	de_uint32 pal[16];
} lctx;

static void do_render_character(deark *c, lctx *d, struct deark_bitmap *img,
	de_int64 xpos, de_int64 ypos, de_byte ccode, de_byte acode)
{
	de_int64 xpos_in_pix, ypos_in_pix;
	de_int64 k, z, n;
	de_byte font_byte;
	de_uint32 fgcol, bgcol;
	de_uint32 clr;

	if(xpos<0 || ypos<0 || xpos>=d->width_in_chars || ypos>=d->height_in_chars) return;

	xpos_in_pix = xpos * d->font_width;
	ypos_in_pix = ypos * d->font_height;

	fgcol = d->pal[(unsigned int)(acode&0x0f)];
	bgcol = d->pal[(unsigned int)((acode&0xf0)>>4)];

	for(k=0; k<d->font_height; k++) {
		font_byte= d->font_data[ccode*d->font_height + k];
		for(z=0; z<d->font_width; z++) {
			if(z<8) {
				n = font_byte&(1<<(7-z));
			}
			else {
				if(ccode>=0xb0 && ccode<=0xdf) {
					n = font_byte&0x1; // 9th column is same as 8th column
				}
				else {
					n = 0; // 9th column is blank
				}
			}
			clr = n ? fgcol : bgcol;
			de_bitmap_setpixel_rgb(img, xpos_in_pix+z, ypos_in_pix+k, clr);
		}
	}
}

static void do_xbin_main(deark *c, lctx *d, dbuf *unc_data)
{
	struct deark_bitmap *img = NULL;
	de_int64 i, j;
	de_byte ccode, acode;

	img = de_bitmap_create(c, d->width_in_pixels, d->height_in_pixels, 3);

	if(d->font_height==16 && d->font_width==8) {
		// Assume the intended display is 640x400.
		img->density_code = DE_DENSITY_UNK_UNITS;
		img->xdens = 480.0;
		img->ydens = 400.0;
	}
	else if(d->font_height==16 && d->font_width==9) {
		// Assume the intended display is 720x400.
		img->density_code = DE_DENSITY_UNK_UNITS;
		img->xdens = 540.0;
		img->ydens = 400.0;
	}

	for(j=0; j<d->height_in_chars; j++) {
		for(i=0; i<d->width_in_chars; i++) {
			ccode = dbuf_getbyte(unc_data, j*d->width_in_chars*2 + i*2);
			acode = dbuf_getbyte(unc_data, j*d->width_in_chars*2 + i*2 + 1);

			if(acode&0x80) {
				d->used_blink = 1;
			}

			do_render_character(c, d, img, i, j, ccode, acode);
		}
	}

	de_bitmap_write_to_file(img, NULL);

	de_bitmap_destroy(img);
}

static void do_uncompress_data(deark *c, lctx *d, de_int64 pos1, dbuf *unc_data)
{
	de_int64 pos;
	de_byte cmprtype;
	de_int64 count;
	de_int64 xpos, ypos;
	de_byte b;
	de_byte b1, b2;
	de_int64 k;

	pos = pos1;

	xpos = 0; ypos = 0;

	while(pos < c->infile->len) {
		if(xpos >= d->width_in_chars) {
			ypos++;
			xpos = 0;
		}
		if(ypos >= d->height_in_chars) {
			break;
		}

		b = de_getbyte(pos);
		pos++;
		cmprtype = b>>6;
		count = (de_int64)(b&0x3f) +1;

		switch(cmprtype) {
		case 0: // Uncompressed
			dbuf_copy(c->infile, pos, count*2, unc_data);
			pos += count*2;
			break;
		case 1: // Character compression
			b1 = de_getbyte(pos++); // character code
			for(k=0; k<count; k++) {
				b2 = de_getbyte(pos++); // attribute code
				dbuf_writebyte(unc_data, b1);
				dbuf_writebyte(unc_data, b2);
			}
			break;
		case 2: // Attribute compression
			b2 = de_getbyte(pos++); // attribute code
			for(k=0; k<count; k++) {
				b1 = de_getbyte(pos++); // character code
				dbuf_writebyte(unc_data, b1);
				dbuf_writebyte(unc_data, b2);
			}
			break;
		case 3: // Character/Attribute compression
			b1 = de_getbyte(pos++); // character code
			b2 = de_getbyte(pos++); // attribute code
			for(k=0; k<count; k++) {
				dbuf_writebyte(unc_data, b1);
				dbuf_writebyte(unc_data, b2);
			}
			break;
		}

		xpos += count;
	}
}

static void do_read_palette(deark *c, lctx *d, de_int64 pos)
{
	de_int64 k;
	de_byte cr, cg, cb;

	de_dbg(c, "palette at %d\n", (int)pos);

	for(k=0; k<16; k++) {
		cr = de_getbyte(pos+k*3);
		cg = de_getbyte(pos+k*3+1);
		cb = de_getbyte(pos+k*3+2);
		de_dbg2(c, "pal[%2d]: %2d,%2d,%2d\n", (int)k, (int)cr, (int)cg, (int)cb);
		cr = de_palette_sample_6_to_8bit(cr);
		cg = de_palette_sample_6_to_8bit(cg);
		cb = de_palette_sample_6_to_8bit(cb);
		d->pal[k] = DE_MAKE_RGB(cr, cg, cb);
	}
}

static void do_default_palette(deark *c, lctx *d)
{
	int k;

	de_dbg(c, "using default palette\n");
	for(k=0; k<16; k++) {
		d->pal[k] = de_palette_pc16(k);
	}
}

static void do_extract_font(deark *c, lctx *d)
{
	struct de_bitmap_font *font = NULL;
	de_int64 i;
	de_finfo *fi = NULL;

	if(d->font_data_len!=4096 || d->font_num_chars!=256) return;
	fi = de_finfo_create(c);
	de_finfo_set_name_from_sz(c, fi, "font", DE_ENCODING_ASCII);

	font = de_malloc(c, sizeof(struct de_bitmap_font));
	font->num_chars = d->font_num_chars;
	font->nominal_width = (int)d->font_width;
	font->nominal_height = (int)d->font_height;
	font->char_array = de_malloc(c, font->num_chars * sizeof(struct de_bitmap_font_char));

	for(i=0; i<font->num_chars; i++) {
		font->char_array[i].codepoint = (de_int32)i;
		font->char_array[i].width = (int)d->font_width;
		font->char_array[i].height = (int)d->font_height;
		font->char_array[i].rowspan = 1;
		font->char_array[i].bitmap = &d->font_data[i*d->font_height];
	}

	de_fmtutil_bitmap_font_to_image(c, font, fi);

	de_finfo_destroy(c, fi);
	if(font) {
		de_free(c, font->char_array);
		de_free(c, font);
	}
}

static void do_read_font(deark *c, lctx *d, de_int64 pos)
{
	de_dbg(c, "font at %d, %d bytes\n", (int)pos, (int)d->font_data_len);
	d->font_data = de_malloc(c, d->font_data_len);
	de_read(d->font_data, pos, d->font_data_len);

	if(c->extract_level>=2) {
		do_extract_font(c, d);
	}
}

static void do_default_font(deark *c, lctx *d)
{
	de_dbg(c, "using default font\n");
	d->font_data = de_malloc(c, d->font_data_len);
	memcpy(d->font_data, de_get_vga_font_ptr(), 4096);
}

static void de_run_xbin(deark *c, const char *params)
{
	lctx *d = NULL;
	de_int64 pos = 0;
	de_byte flags;
	dbuf *unc_data = NULL;
	const char *s;

	de_dbg(c, "xbin\n");
	d = de_malloc(c, sizeof(lctx));

	d->font_width = 8;
	s = de_get_ext_option(c, "xbin:charwidth");
	if(s) {
		if(de_atoi(s)>=9) {
			d->font_width = 9;
		}
	}

	d->width_in_chars = de_getui16le(5);
	d->height_in_chars = de_getui16le(7);
	d->font_height = (de_int64)de_getbyte(9);
	if(d->font_height<1 || d->font_height>32) {
		de_err(c, "Invalid font height: %d\n", (int)d->font_height);
		goto done;
	}

	flags = de_getbyte(10);
	de_dbg(c, "dimensions: %dx%d characters\n", (int)d->width_in_chars, (int)d->height_in_chars);
	de_dbg(c, "font height: %d\n", (int)d->font_height);
	de_dbg(c, "flags: 0x%02x\n", (unsigned int)flags);
	d->has_palette = (flags&0x01)?1:0;
	d->has_font = (flags&0x02)?1:0;
	d->compression = (flags&0x04)?1:0;
	d->nonblink = (flags&0x08)?1:0;
	d->has_512chars = (flags&0x10)?1:0;
	de_dbg(c, " has palette: %d\n", (int)d->has_palette);
	de_dbg(c, " has font: %d\n", (int)d->has_font);
	de_dbg(c, " compression: %d\n", (int)d->compression);
	de_dbg(c, " non-blink mode: %d\n", (int)d->nonblink);
	de_dbg(c, " 512 character mode: %d\n", (int)d->has_512chars);

	d->width_in_pixels = d->width_in_chars * d->font_width;
	d->height_in_pixels = d->height_in_chars * d->font_height;
	de_dbg(c, "dimensions: %dx%d pixels\n", (int)d->width_in_pixels, (int)d->height_in_pixels);
	if(!de_good_image_dimensions(c, d->width_in_pixels, d->height_in_pixels)) goto done;

	pos = 11;

	if(d->has_palette) {
		do_read_palette(c, d, pos);
		pos += 48;
	}
	else {
		do_default_palette(c, d);
	}

	d->font_num_chars = d->has_512chars ? 512 : 256;
	d->font_data_len = d->font_num_chars * d->font_height;
	if(d->font_num_chars!=256) {
		de_err(c, "%d-character mode is not supported\n", (int)d->font_num_chars);
		goto done;
	}

	if(d->has_font) {
		do_read_font(c, d, pos);
		pos += d->font_data_len;
	}
	else {
		if(d->font_num_chars!=256 || d->font_height!=16) {
			de_err(c, "This type of XBIN file is not supported.\n");
			goto done;
		}

		do_default_font(c, d);
	}

	de_dbg(c, "image data at %d\n", (int)pos);

	if(d->compression) {
		unc_data = dbuf_create_membuf(c, d->width_in_chars * d->height_in_chars * 2);
		dbuf_set_max_length(unc_data, d->width_in_chars * d->height_in_chars * 2);

		do_uncompress_data(c, d, pos, unc_data);
	}
	else {
		unc_data = dbuf_open_input_subfile(c->infile, pos, c->infile->len-pos);
	}
	do_xbin_main(c, d, unc_data);

	if(!d->nonblink && d->used_blink) {
		de_warn(c, "This image uses blinking characters, which are not supported.\n");
	}

done:
	dbuf_close(unc_data);
	de_free(c, d->font_data);
	de_free(c, d);
}

static int de_identify_xbin(deark *c)
{
	if(!dbuf_memcmp(c->infile, 0, "XBIN\x1a", 5))
		return 100;
	return 0;
}

void de_module_xbin(deark *c, struct deark_module_info *mi)
{
	mi->id = "xbin";
	mi->run_fn = de_run_xbin;
	mi->identify_fn = de_identify_xbin;
}
