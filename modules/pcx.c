// This file is part of Deark.
// Copyright (C) 2016 Jason Summers
// See the file COPYING for terms of use.

// PCX (PC Paintbrush) and related formats

#include <deark-config.h>
#include <deark-private.h>
DE_DECLARE_MODULE(de_module_pcx);
DE_DECLARE_MODULE(de_module_mswordscr);
DE_DECLARE_MODULE(de_module_dcx);
DE_DECLARE_MODULE(de_module_pcx2com);

#define PCX_HDRSIZE 128

enum resmode_type {
	RESMODE_IGNORE = 0,
	RESMODE_AUTO,
	RESMODE_DPI,
	RESMODE_SCREENDIMENSIONS
};

typedef struct localctx_PCX {
	u8 version;
	u8 encoding;
	enum resmode_type resmode;
	i64 bits;
	i64 bits_per_pixel;
	i64 margin_L, margin_T, margin_R, margin_B;
	i64 hscreensize, vscreensize;
	i64 planes;
	i64 rowspan_raw;
	i64 rowspan;
	i64 ncolors;
	UI palette_info;
	u8 reserved1;
	i64 reported_width;
	i64 padded_width;
	i64 width_to_use;
	i64 height;
	u8 is_mswordscr;
	u8 is_pcxsfx;
	int has_vga_pal;
	int has_transparency;

	// Identifier of the palette to use, if there is no palette in the file
	int default_pal_num;
	int default_pal_set;

	dbuf *unc_pixels;
	de_finfo *fi;
	de_color pal[256];
} lctx;

static void simplify_dens(i64 *pxdens, i64 *pydens, i64 factor)
{
	while(*pxdens>factor && *pydens>factor &&
		(*pxdens%factor==0) && (*pydens%factor==0))
	{
		*pxdens /= factor;
		*pydens /= factor;
	}
}

static void set_density_from_screen_res(deark *c, lctx *d, i64 hres, i64 vres)
{
	i64 xdens, ydens;

	d->fi->density.code = DE_DENSITY_UNK_UNITS;
	xdens = hres*3; // Assume 4:3 screen
	ydens = vres*4;

	simplify_dens(&xdens, &ydens, 2);
	simplify_dens(&xdens, &ydens, 3);
	simplify_dens(&xdens, &ydens, 5);

	d->fi->density.xdens = (double)xdens;
	d->fi->density.ydens = (double)ydens;
}

// The resolution field is unreliable. It might contain:
// * Zeroes
// * The DPI
// * The pixel dimensions of the target screen mode
// * The dimensions of the image itself
// * A corrupted attempt at one of the above things (perhaps copied from an
//   older version of the image)
static void do_decode_resolution(deark *c, lctx *d, i64 hres, i64 vres)
{
	enum resmode_type resmode = d->resmode;

	if(hres==0 || vres==0) return;

	// TODO: Account for d->hscreensize, d->vscreensize.

	if(resmode==RESMODE_AUTO) {
		if((hres==320 && vres==200) ||
			(hres==640 && vres==480) ||
			(hres==640 && vres==350) ||
			(hres==640 && vres==200) ||
			(hres==800 && vres==600) ||
			(hres==1024 && vres==768))
		{
			if(d->reported_width<=hres && d->height<=hres) {
				// Looks like screen dimensions, and image fits on the screen
				resmode = RESMODE_SCREENDIMENSIONS;
			}
		}
		else if(hres==d->reported_width && vres==d->height) {
			;
		}
		else {
			if(hres==vres && hres>=50 && hres<=600) {
				resmode = RESMODE_DPI;
			}
		}
	}

	if(resmode==RESMODE_DPI) {
		d->fi->density.code = DE_DENSITY_DPI;
		d->fi->density.xdens = (double)hres;
		d->fi->density.ydens = (double)vres;
	}
	else if(resmode==RESMODE_SCREENDIMENSIONS) {
		set_density_from_screen_res(c, d, hres, vres);
	}
}

static int sane_screensize(i64 h, i64 v)
{
	if(h<320 || v<200) return 0;
	if(h>4096 || v>4096) return 0;
	if((h%8 != 0) || (v%2 != 0)) return 0;
	if(v*5 < h) return 0;
	if(h*3 < v) return 0;
	return 1;
}

static int do_read_header(deark *c, lctx *d)
{
	u8 initialbyte;
	int retval = 0;
	i64 hres, vres;
	i64 pos = 0;
	const char *imgtypename = "";

	de_dbg(c, "header at %"I64_FMT, pos);
	de_dbg_indent(c, 1);

	initialbyte = de_getbyte_p(&pos);
	d->version = de_getbyte_p(&pos);
	if(!d->is_mswordscr) {
		if(initialbyte==0xeb && d->version==0x0e) {
			d->is_pcxsfx = 1;
			d->version = 5;
		}
	}
	de_dbg(c, "format version: %u", (UI)d->version);

	if(d->is_mswordscr) {
		de_declare_fmt(c, "Word for DOS screen capture");
	}
	else if(d->is_pcxsfx) {
		de_declare_fmt(c, "VGAPaint 386 PCX-SFX");
	}
	else {
		de_declare_fmt(c, "PCX");
	}

	d->encoding = de_getbyte_p(&pos);
	de_dbg(c, "encoding: %u", (UI)d->encoding);

	d->bits = (i64)de_getbyte_p(&pos); // Bits per pixel per plane
	de_dbg(c, "bits: %d", (int)d->bits);
	if(d->bits<1) d->bits = 1;

	d->margin_L = de_getu16le_p(&pos);
	d->margin_T = de_getu16le_p(&pos);
	d->margin_R = de_getu16le_p(&pos);
	d->margin_B = de_getu16le_p(&pos);
	de_dbg(c, "margins: %d, %d, %d, %d", (int)d->margin_L, (int)d->margin_T,
		(int)d->margin_R, (int)d->margin_B);
	d->reported_width = d->margin_R - d->margin_L +1;
	d->height = d->margin_B - d->margin_T +1;
	de_dbg_dimensions(c, d->reported_width, d->height);

	hres = de_getu16le_p(&pos);
	vres = de_getu16le_p(&pos);
	de_dbg(c, "resolution: %d"DE_CHAR_TIMES"%d", (int)hres, (int)vres);

	// The palette (offset 16-63) will be read later.

	pos = 64;
	// For older versions of PCX, this field might be useful to help identify
	// the intended video mode. Documentation is lacking, though.
	d->reserved1 = de_getbyte_p(&pos);
	de_dbg(c, "vmode: 0x%02x", (UI)d->reserved1);

	d->planes = (i64)de_getbyte_p(&pos);
	de_dbg(c, "planes: %d", (int)d->planes);
	d->rowspan_raw = de_getu16le_p(&pos);
	de_dbg(c, "bytes/plane/row: %d", (int)d->rowspan_raw);

	// TODO: Is this field (@68) 1 byte or 2?
	d->palette_info = (UI)de_getbyte_p(&pos);
	pos++;
	de_dbg(c, "palette info: %u", (UI)d->palette_info);

	if(d->version>=5) {
		d->hscreensize = de_getu16le_p(&pos);
		d->vscreensize = de_getu16le_p(&pos);
		if(!sane_screensize(d->hscreensize, d->vscreensize)) {
			d->hscreensize = 0;
			d->vscreensize = 0;
		}
	}
	if(d->hscreensize) {
		de_dbg(c, "screen size: %d" DE_CHAR_TIMES "%d", (int)d->hscreensize,
			(int)d->vscreensize);
	}

	//-----

	d->padded_width = (d->rowspan_raw*8) / d->bits;
	d->width_to_use = d->reported_width;
	if(c->padpix) {
		if(d->padded_width>d->reported_width) {
			d->width_to_use = d->padded_width;
		}
	}
	else {
		if(d->width_to_use<1 && d->padded_width>0) {
			de_warn(c, "Invalid width %"I64_FMT"; using %"I64_FMT" instead",
				d->width_to_use, d->padded_width);
			d->width_to_use = d->padded_width;
		}
	}

	if(!de_good_image_dimensions(c, d->width_to_use, d->height)) goto done;

	d->rowspan = d->rowspan_raw * d->planes;
	de_dbg(c, "calculated bytes/row: %d", (int)d->rowspan);

	d->bits_per_pixel = d->bits * d->planes;

	if(d->encoding!=0 && d->encoding!=1) {
		de_err(c, "Unsupported compression type: %d", (int)d->encoding);
		goto done;
	}

	// Enumerate the known PCX image types.
	if(d->planes==1 && d->bits==1) {
		imgtypename = "2-color";
		d->ncolors = 2;
	}
	//else if(d->planes==2 && d->bits==1) {
	//	d->ncolors = 4;
	//}
	else if(d->planes==1 && d->bits==2) {
		imgtypename = "4-color";
		d->ncolors = 4;
	}
	else if(d->planes==1 && d->bits==4) {
		imgtypename = "16-color nonplanar";
		d->ncolors = 16;
	}
	else if(d->planes==3 && d->bits==1) {
		imgtypename = "8-color";
		d->ncolors = 8;
	}
	else if(d->planes==4 && d->bits==1) {
		imgtypename = "16-color";
		d->ncolors = 16;
	}
	//else if(d->planes==1 && d->bits==4) {
	//	d->ncolors = 16;
	//}
	//else if(d->planes==4 && d->bits==2) {
	//	d->ncolors = 16; (?)
	//}
	else if(d->planes==1 && d->bits==8) {
		imgtypename = "256-color";
		d->ncolors = 256;
	}
	//else if(d->planes==4 && d->bits==4) {
	//	d->ncolors = 4096;
	//}
	else if(d->planes==3 && d->bits==8) {
		imgtypename = "truecolor";
		d->ncolors = 16777216;
	}
	else if(d->planes==4 && d->bits==8) {
		// I can't find a PCX spec that mentions 32-bit RGBA images, but
		// ImageMagick and Wikipedia act like they're perfectly normal.
		imgtypename = "truecolor+alpha";
		d->ncolors = 16777216;
		d->has_transparency = 1;
	}
	else {
		de_err(c, "Unsupported image type (bits=%d, planes=%d)",
			(int)d->bits, (int)d->planes);
		goto done;
	}

	de_dbg(c, "image type: %s", imgtypename);

	do_decode_resolution(c, d, hres, vres);

	retval = 1;
done:
	de_dbg_indent(c, -1);
	return retval;
}

static int do_read_vga_palette(deark *c, lctx *d)
{
	i64 pos;

	if(d->version<5) return 0;
	if(d->ncolors!=256) return 0;
	pos = c->infile->len - 769;
	if(pos<PCX_HDRSIZE) return 0;

	if(de_getbyte(pos) != 0x0c) {
		return 0;
	}

	de_dbg(c, "VGA palette at %"I64_FMT, pos);
	d->has_vga_pal = 1;
	pos++;
	de_dbg_indent(c, 1);
	de_read_palette_rgb(c->infile, pos, 256, 3, d->pal, 256, 0);
	de_dbg_indent(c, -1);

	return 1;
}

// Maybe read the palette from a separate file.
// Returns 1 if the palette was read.
static int do_read_alt_palette_file(deark *c, lctx *d)
{
	const char *palfn;
	dbuf *palfile = NULL;
	int retval = 0;
	i64 k,z;
	u8 b1[3];
	u8 b2[3];
	int badflag = 0;
	char tmps[64];

	palfn = de_get_ext_option(c, "file2");
	if(!palfn) goto done;

	palfile = dbuf_open_input_file(c, palfn);
	if(!palfile) goto done;
	de_dbg(c, "using palette from separate file");

	if(palfile->len != d->ncolors*3) {
		badflag = 1;
	}

	de_dbg_indent(c, 1);
	for(k=0; k<d->ncolors && k*3<palfile->len; k++) {
		dbuf_read(palfile, b1, 3*k, 3);
		for(z=0; z<3; z++) {
			if(b1[z]>0x3f) badflag = 1;
			b2[z] = de_scale_63_to_255(b1[z]);
		}
		d->pal[k] = DE_MAKE_RGB(b2[0],b2[1],b2[2]);

		de_snprintf(tmps, sizeof(tmps), "(%2d,%2d,%2d) "DE_CHAR_RIGHTARROW" ",
			(int)b1[0], (int)b1[1], (int)b1[2]);
		de_dbg_pal_entry2(c, k, d->pal[k], tmps, NULL, NULL);
	}
	de_dbg_indent(c, -1);

	if(badflag) {
		de_warn(c, "%s doesn't look like the right kind of palette file", palfn);
	}

	retval = 1;

done:
	dbuf_close(palfile);
	return retval;
}

static const de_color ega16pal_1[16] = {
	0xff000000U,0xffbf0000U,0xff00bf00U,0xffbfbf00U,
	0xff0000bfU,0xffbf00bfU,0xff00bfbfU,0xffc0c0c0U,
	0xff808080U,0xffff0000U,0xff00ff00U,0xffffff00U,
	0xff0000ffU,0xffff00ffU,0xff00ffffU,0xffffffffU
};

static void do_palette_stuff(deark *c, lctx *d)
{
	i64 k;

	if(d->ncolors>256) {
		return;
	}

	if(d->ncolors==256) {
		// For 256-color images, start with a default grayscale palette.
		for(k=0; k<256; k++) {
			d->pal[k] = DE_MAKE_GRAY((UI)k);
		}
	}

	if(do_read_alt_palette_file(c, d)) {
		return;
	}

	if(d->ncolors==2) {
		// TODO: Allegedly, some 2-color PCXs are not simply white-on-black,
		// and at least the foreground color can be something other than white.
		// The color information would be stored in the palette area, but
		// different files use different ways of conveying that information,
		// and it seems hopeless to reliably determine the correct format.
		return;
	}

	if(d->version==3 && d->ncolors>=8 && d->ncolors<=16) {
		// Come up with a 16-color palette, if there is no palette in the file.
		// (8-color version-3 PCXs apparently use only the first 8 colors of the
		// palette.)

		if(!d->default_pal_set) {
			de_info(c, "Note: This paletted PCX file does not contain a palette. "
				"If it is not decoded correctly, try \"-opt pcx:pal=1\".");
		}
		de_dbg(c, "using a default EGA palette");
		if(d->default_pal_num==1) {
			// This is the "default EGA palette" used by several PCX viewers.
			// I don't know its origin.
			de_memcpy(d->pal, ega16pal_1, sizeof(ega16pal_1));
		}
		else {
			// This palette seems to be correct for at least some files.
			de_copy_std_palette(DE_PALID_WIN16, 2, 0, d->pal, 16, 0);
		}
		return;
	}

	if(d->version>=5 && d->ncolors==256) {
		if(do_read_vga_palette(c, d)) {
			return;
		}
		de_warn(c, "Expected VGA palette was not found");
		// (Use the grayscale palette created earlier, as a last resort.)
		return;
	}

	if(d->ncolors==4) {
		u8 p0, p3;
		UI bgcolor;
		UI fgpal;
		int pal_subid;

		de_warn(c, "4-color PCX images might not be supported correctly");

		p0 = de_getbyte(16);
		p3 = de_getbyte(19);
		bgcolor = p0>>4;
		fgpal = p3>>5;
		de_dbg(c, "using a CGA palette: palette #%d, bkgd color %d", (int)fgpal, (int)bgcolor);

		// Set first pal entry to background color
		d->pal[0] = de_get_std_palette_entry(DE_PALID_PC16, 0, (int)bgcolor);

		// TODO: These palettes are quite possibly incorrect. I can't find good
		// information about them.
		switch(fgpal) {
		case 1: case 3:
			pal_subid = 5; break; // C=0 P=? I=1
		case 4:
			pal_subid = 1; break; // C=1 P=0 I=0
		case 5:
			pal_subid = 4; break; // C=1 P=0 I=1
		case 6:
			pal_subid = 0; break; // C=1 P=1 I=0
		case 7:
			pal_subid = 3; break; // C=1 P=1 I=1
		default: // 0, 2
			pal_subid = 2; break; // C=0 P=? I=0
		}
		de_copy_std_palette(DE_PALID_CGA, pal_subid, 1, &d->pal[1], 3, 0);
		return;
	}

	if(d->ncolors>16 && d->ncolors<=256) {
		de_warn(c, "%u-color image format with 16-color palette", (UI)d->ncolors);
	}

	de_dbg(c, "using 16-color palette from header");

	de_dbg_indent(c, 1);
	de_read_palette_rgb(c->infile, 16, 16, 3, d->pal, 256, 0);
	de_dbg_indent(c, -1);
}

static int do_uncompress(deark *c, lctx *d)
{
	i64 pos;
	u8 b, b2;
	i64 count;
	i64 expected_bytes;
	i64 endpos;

	pos = PCX_HDRSIZE;
	de_dbg(c, "compressed bitmap at %"I64_FMT, pos);

	expected_bytes = d->rowspan * d->height;
	d->unc_pixels = dbuf_create_membuf(c, expected_bytes, 0);
	dbuf_enable_wbuffer(d->unc_pixels);

	endpos = c->infile->len;
	if(d->has_vga_pal) {
		// The last 769 bytes of this file are reserved for the palette.
		// Don't try to decode them as pixels.
		endpos -= 769;
	}

	while(1) {
		if(pos>=endpos) {
			break; // Reached the end of source data
		}
		if(d->unc_pixels->len >= expected_bytes) {
			break; // Reached the end of the image
		}
		b = de_getbyte(pos++);

		if(b>=0xc0) {
			count = (i64)(b&0x3f);
			b2 = de_getbyte(pos++);
			dbuf_write_run(d->unc_pixels, b2, count);
		}
		else {
			dbuf_writebyte(d->unc_pixels, b);
		}
	}

	dbuf_flush(d->unc_pixels);
	if(d->unc_pixels->len < expected_bytes) {
		de_warn(c, "Expected %d bytes of image data, only found %d",
			(int)expected_bytes, (int)d->unc_pixels->len);
	}

	return 1;
}

static void do_bitmap_1bpp(deark *c, lctx *d)
{
	// The paletted algorithm would work here (if we construct a palette),
	// but this special case is easy and efficient.
	de_convert_and_write_image_bilevel2(d->unc_pixels, 0,
		d->width_to_use, d->height, d->rowspan_raw, 0, d->fi, 0);
}

static void do_bitmap_paletted(deark *c, lctx *d)
{
	de_bitmap *img = NULL;

	img = de_bitmap_create(c, d->width_to_use, d->height, 3);

	// Impossible to get here unless one of the following conditions is true.
	if(d->planes==1) {
		de_convert_image_paletted(d->unc_pixels, 0, d->bits, d->rowspan,
			d->pal, img, 0);
	}
	else if(d->bits==1) {
		de_convert_image_paletted_planar(d->unc_pixels, 0, d->planes, d->rowspan,
			d->rowspan_raw, d->pal, img, 0x2);
	}

	de_bitmap_write_to_file_finfo(img, d->fi, DE_CREATEFLAG_OPT_IMAGE);
	de_bitmap_destroy(img);
}

static void do_bitmap_24bpp(deark *c, lctx *d)
{
	de_bitmap *img = NULL;
	i64 i, j;
	i64 plane;
	u8 s[4];

	de_memset(s, 0xff, sizeof(s));
	img = de_bitmap_create(c, d->width_to_use, d->height, d->has_transparency?4:3);

	for(j=0; j<d->height; j++) {
		for(i=0; i<d->width_to_use; i++) {
			for(plane=0; plane<d->planes; plane++) {
				s[plane] = dbuf_getbyte(d->unc_pixels, j*d->rowspan + plane*d->rowspan_raw +i);
			}
			de_bitmap_setpixel_rgba(img, i, j, DE_MAKE_RGBA(s[0], s[1], s[2], s[3]));
		}
	}

	de_bitmap_write_to_file_finfo(img, d->fi, 0);
	de_bitmap_destroy(img);
}

static void do_bitmap(deark *c, lctx *d)
{
	if(d->bits_per_pixel==1) {
		do_bitmap_1bpp(c, d);
	}
	else if(d->bits_per_pixel<=8) {
		do_bitmap_paletted(c, d);
	}
	else if(d->bits_per_pixel>=24) {
		do_bitmap_24bpp(c, d);
	}
	else {
		de_err(c, "Unsupported bits/pixel: %d", (int)d->bits_per_pixel);
	}
}

static void de_run_pcx_internal(deark *c, lctx *d, de_module_params *mparams)
{
	const char *s;

	s = de_get_ext_option(c, "pcx:pal");
	if(s) {
		d->default_pal_num = de_atoi(s);
		if(d->default_pal_num<0 || d->default_pal_num>1) {
			d->default_pal_num = 0;
		}
		d->default_pal_set = 1;
	}

	d->resmode = RESMODE_AUTO;
	s = de_get_ext_option(c, "pcx:resmode");
	if(s) {
		if(!de_strcmp(s, "auto")) {
			d->resmode = RESMODE_AUTO;
		}
		else if(!de_strcmp(s, "dpi")) {
			d->resmode = RESMODE_DPI;
		}
		else if(!de_strcmp(s, "screen")) {
			d->resmode = RESMODE_SCREENDIMENSIONS;
		}
	}

	d->fi = de_finfo_create(c);

	if(!do_read_header(c, d)) {
		goto done;
	}

	do_palette_stuff(c, d);

	if(d->encoding==0) {
		// Uncompressed PCXs are probably not standard, but support for them is not
		// uncommon. Imagemagick, for example, will create them if you ask it to.
		de_dbg(c, "uncompressed bitmap at %d", (int)PCX_HDRSIZE);
		d->unc_pixels = dbuf_open_input_subfile(c->infile,
			PCX_HDRSIZE, c->infile->len-PCX_HDRSIZE);
	}
	else {
		if(!do_uncompress(c, d)) {
			goto done;
		}
	}
	dbuf_flush(d->unc_pixels);

	do_bitmap(c, d);

done:
	dbuf_close(d->unc_pixels);
	d->unc_pixels = NULL;
	de_finfo_destroy(c, d->fi);
	d->fi = NULL;
}

static void de_run_pcx(deark *c, de_module_params *mparams)
{
	lctx *d = NULL;

	d = de_malloc(c, sizeof(lctx));
	de_run_pcx_internal(c, d, mparams);
	de_free(c, d);
}

static int de_identify_pcx(deark *c)
{
	u8 buf[8];

	de_read(buf, 0, 8);
	if(buf[0]==0x0a && (buf[1]==0 || buf[1]==2 || buf[1]==3
		|| buf[1]==4 || buf[1]==5) &&
		(buf[2]==0 || buf[2]==1) )
	{
		if(de_input_file_has_ext(c, "pcx"))
			return 100;
		return 16;
	}

	// VGAPaint 386 PCX SFX
	if(buf[0]==0xeb && buf[1]==0x0e && buf[2]==1 && buf[3]==8 &&
		(de_getbyte(16)==0xe8))
	{
		if(de_input_file_has_ext(c, "pcx"))
			return 80;
		return 8;
	}

	return 0;
}

static void de_help_pcx(deark *c)
{
	de_msg(c, "-opt pcx:pal=<0|1> : Code for the predefined palette to use, "
		"if there is no palette in the file");
	de_msg(c, "-opt pcx:resmode=<ignore|dpi|screen|auto> : How to interpret the "
		"\"resolution\" field");
	de_msg(c, "-file2 <file.p13> : Read the palette from a separate file");
}

void de_module_pcx(deark *c, struct deark_module_info *mi)
{
	mi->id = "pcx";
	mi->desc = "PCX image";
	mi->run_fn = de_run_pcx;
	mi->identify_fn = de_identify_pcx;
	mi->help_fn = de_help_pcx;
}

// **************************************************************************
// MS Word for DOS Screen Capture
// **************************************************************************

static void de_run_mswordscr(deark *c, de_module_params *mparams)
{
	lctx *d = NULL;

	d = de_malloc(c, sizeof(lctx));
	d->is_mswordscr = 1;
	de_run_pcx_internal(c, d, mparams);
	de_free(c, d);
}

static int de_identify_mswordscr(deark *c)
{
	u8 buf[8];

	de_read(buf, 0, 8);
	if(buf[0]==0xcd && (buf[1]==0 || buf[1]==2 || buf[1]==3
		|| buf[1]==4 || buf[1]==5) &&
		buf[2]==1 )
	{
		if(de_input_file_has_ext(c, "scr") || de_input_file_has_ext(c, "mwg"))
			return 100;

		return 10;
	}
	return 0;
}

void de_module_mswordscr(deark *c, struct deark_module_info *mi)
{
	mi->id = "mswordscr";
	mi->desc = "MS Word for DOS Screen Capture";
	mi->run_fn = de_run_mswordscr;
	mi->identify_fn = de_identify_mswordscr;
}

// **************************************************************************
// DCX
// **************************************************************************

static void de_run_dcx(deark *c, de_module_params *mparams)
{
	u32 *page_offset;
	i64 num_pages;
	i64 page;
	i64 page_size;

	page_offset = de_mallocarray(c, 1023, sizeof(u32));
	num_pages = 0;
	while(num_pages < 1023) {
		page_offset[num_pages] = (u32)de_getu32le(4 + 4*num_pages);
		if(page_offset[num_pages]==0)
			break;
		num_pages++;
	}

	de_dbg(c, "number of pages: %d", (int)num_pages);

	for(page=0; page<num_pages; page++) {
		if(page == num_pages-1) {
			// Last page. Assume it goes to the end of file.
			page_size = c->infile->len - page_offset[page];
		}
		else {
			page_size = page_offset[page+1] - page_offset[page];
		}
		if(page_size<0) page_size=0;
		de_dbg(c, "page %d at %u, size=%"I64_FMT, (int)page, (UI)page_offset[page],
			page_size);

		dbuf_create_file_from_slice(c->infile, page_offset[page], page_size, "pcx", NULL, 0);
	}
}

static int de_identify_dcx(deark *c)
{
	if(!dbuf_memcmp(c->infile, 0, "\xb1\x68\xde\x3a", 4))
		return 100;
	return 0;
}

void de_module_dcx(deark *c, struct deark_module_info *mi)
{
	mi->id = "dcx";
	mi->desc = "DCX (multi-image PCX)";
	mi->run_fn = de_run_dcx;
	mi->identify_fn = de_identify_dcx;
}

// **************************************************************************
// PCX2COM
// DOS utility by "Dr.Destiny".
// graph/pcx2com.zip in the SAC archive.
// **************************************************************************

static void de_run_pcx2com(deark *c, de_module_params *mparams)
{
	i64 pos;
	dbuf *outf = NULL;

	outf = dbuf_create_output_file(c, "pcx", NULL, 0);

	// header
	dbuf_enable_wbuffer(outf);
	dbuf_write(outf, (const u8*)"\x0a\x05\x01\x08\x00\x00\x00\x00\x3f\x01\xc7", 11);
	dbuf_truncate(outf, 64);
	dbuf_write(outf, (const u8*)"\x00\x01\x40\x01\x01\x00\x40\x01\xc8", 9);
	dbuf_truncate(outf, 128);

	// image data, and 0x0c palette marker
	dbuf_copy(c->infile, 920, c->infile->len-920, outf);

	// VGA palette
	pos = 152;
	while(pos < 152+768) {
		u8 x;

		x = de_getbyte_p(&pos);
		dbuf_writebyte(outf, de_scale_63_to_255(x));
	}

	dbuf_close(outf);
}

static int de_identify_pcx2com(deark *c)
{
	if(c->infile->len<922 || c->infile->len>65280) return 0;

	if((UI)de_getu32be(0)!=0xb81300cdU) return 0;
	if(de_getbyte(c->infile->len-1) != 0x0c) return 0;

	// The is the substring "Self PCX", xor 0x80.
	if(dbuf_memcmp(c->infile, 104,
		(const u8*)"\xd3\xe5\xec\xe6\xa0\xd0\xc3\xd8", 8))
	{
		return 0;
	}

	return 100;
}

void de_module_pcx2com(deark *c, struct deark_module_info *mi)
{
	mi->id = "pcx2com";
	mi->desc = "PCX2COM self-displaying image";
	mi->run_fn = de_run_pcx2com;
	mi->identify_fn = de_identify_pcx2com;
}
