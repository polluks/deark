// This file is part of Deark, by Jason Summers.
// This software is in the public domain. See the file COPYING for details.

// Inset .PIX

#include <deark-config.h>
#include <deark-modules.h>

typedef struct localctx_struct {
	de_int64 item_count;
	de_byte hmode;
	de_byte htype;
	de_byte graphics_type; // 0=character, 1=bitmap
	de_byte board_type;
	de_int64 width, height;
	de_int64 gfore; // Foreground color bits
	de_int64 max_sample_value;
	de_int64 num_pal_bits[4]; // 0=intens, 1=red, 2=green, 3=blue
	de_int64 haspect, vaspect;

	de_int64 page_rows, page_cols;
	de_int64 stp_rows, stp_cols;

	de_int64 rowspan;
	de_int64 compression_bytes_per_row;
	de_int64 planespan;
	int is_grayscale;
	int pal_bits_hack;

	de_byte max_pal_intensity, max_pal_sample;

	de_int64 pal_entries_used;
	de_uint32 pal[256];
} lctx;

// Warning: Some ugly hacks to try to get images to display correctly, even when they
// appear to violate the spec.
static int do_detect_palette_bits_hack(deark *c, lctx *d)
{
	int retval = 0;

	if(d->is_grayscale) {
		goto done;
	}

	if(d->num_pal_bits[0]>2) {
		de_err(c, "Don't know how to handle images with \"intensity bits\" greater than 2.\n");
		goto done;
	}

	if(d->max_pal_sample==0) goto done;

	if(d->num_pal_bits[0]==0 && d->num_pal_bits[1]==4 && d->num_pal_bits[2]==4 &&
		d->num_pal_bits[3]==4 && d->max_pal_sample<=3)
	{
		d->num_pal_bits[1]=2;
		d->num_pal_bits[2]=2;
		d->num_pal_bits[3]=2;
		d->pal_bits_hack = 1;
		retval = 1;
		goto done;
	}

	if(d->num_pal_bits[0]==2 && d->num_pal_bits[1]==2 && d->num_pal_bits[2]==2 &&
		d->num_pal_bits[3]==2 && d->max_pal_sample<=1)
	{
		d->num_pal_bits[1]=1;
		d->num_pal_bits[2]=1;
		d->num_pal_bits[3]=1;
		d->pal_bits_hack = 1;
		retval = 1;
		goto done;
	}

	retval = 1;
done:
	if(retval && d->pal_bits_hack) {
		de_warn(c, "Assuming the \"number of palette bits\" fields contain the number "
			"of sample values, instead of the number of bits.\n");
	}
	return retval;
}

static void do_prescan_palette(deark *c, lctx *d, de_int64 pos)
{
	de_int64 i;
	de_byte ci, cr, cg, cb;

	for(i=0; i<d->pal_entries_used; i++) {
		ci = de_getbyte(pos+4*i);
		cr = de_getbyte(pos+4*i+1);
		cg = de_getbyte(pos+4*i+2);
		cb = de_getbyte(pos+4*i+3);
		if(ci > d->max_pal_intensity) d->max_pal_intensity = ci;
		if(cr > d->max_pal_sample) d->max_pal_sample = cr;
		if(cg > d->max_pal_sample) d->max_pal_sample = cg;
		if(cb > d->max_pal_sample) d->max_pal_sample = cb;
	}
	de_dbg(c, "prescanning palette: max intensity=%d, max sample=%d\n",
		(int)d->max_pal_intensity, (int)d->max_pal_sample);
}

static int do_palette(deark *c, lctx *d, de_int64 pos, de_int64 len)
{
	de_int64 pal_entries_in_file;
	de_int64 i;
	de_byte ci1, cr1, cg1, cb1;
	de_byte ci2, cr2, cg2, cb2;
	int retval = 0;

	de_dbg(c, "palette at %d\n", (int)pos);
	de_dbg_indent(c, 1);

	pal_entries_in_file = len/4;
	de_dbg(c, "number of palette colors: %d\n", (int)pal_entries_in_file);

	d->pal_entries_used = d->max_sample_value+1;
	if(d->pal_entries_used > pal_entries_in_file) d->pal_entries_used = pal_entries_in_file;

	// Warning: The spec does not say what to do with the "intensity" value, and anyway,
	// every file I've seen seems to violate the spec. This code may be very wrong.

	do_prescan_palette(c, d, pos);
	if(!do_detect_palette_bits_hack(c, d)) goto done;

	for(i=0; i<pal_entries_in_file; i++) {
		if(i>255) break;
		ci1 = de_getbyte(pos+4*i);
		cr1 = de_getbyte(pos+4*i+1);
		cg1 = de_getbyte(pos+4*i+2);
		cb1 = de_getbyte(pos+4*i+3);

		if(d->is_grayscale) {
			ci2 = de_sample_n_to_8bit(ci1, d->num_pal_bits[0]);
			cr2 = ci2;
			cg2 = ci2;
			cb2 = ci2;
			d->pal[i] = DE_MAKE_GRAY(ci2);
		}
		else {
			cr2 = de_sample_n_to_8bit(cr1, d->num_pal_bits[1]);
			cg2 = de_sample_n_to_8bit(cg1, d->num_pal_bits[2]);
			cb2 = de_sample_n_to_8bit(cb1, d->num_pal_bits[3]);
			if(ci1) {
				cr2|=0x80;
				cg2|=0x80;
				cb2|=0x80;
			}
			d->pal[i] = DE_MAKE_RGB(cr2,cg2,cb2);
		}

		de_dbg2(c, "pal[%3d] = (I=%d, %d,%d,%d) -> (%d,%d,%d)%s\n", (int)i,
			(int)ci1, (int)cr1, (int)cg1, (int)cb1,
			(int)cr2, (int)cg2, (int)cb2,
			i<d->pal_entries_used ? "":" [unused]");
	}

	retval = 1;
done:
	de_dbg_indent(c, -1);
	return retval;
}

static int do_image_info(deark *c, lctx *d, de_int64 pos, de_int64 len)
{
	int retval = 0;

	de_dbg(c, "image information at %d\n", (int)pos);
	de_dbg_indent(c, 1);
	if(len<32) {
		de_err(c, "Image Information item too small\n");
		goto done;
	}

	d->hmode = de_getbyte(pos);
	de_dbg(c, "hardware mode: %d\n", (int)d->hmode);

	d->htype = de_getbyte(pos+1);
	d->graphics_type = d->htype & 0x01;
	d->board_type = d->htype & 0xfe;

	de_dbg(c, "graphics type: %d (%s)\n", (int)d->graphics_type,
		d->graphics_type?"bitmap":"character");
	de_dbg(c, "board type: %d\n", (int)d->board_type);

	d->width = de_getui16le(pos+18);
	d->height = de_getui16le(pos+20);
	de_dbg(c, "dimensions: %dx%d\n", (int)d->width, (int)d->height);

	d->gfore = (de_int64)de_getbyte(pos+22);
	de_dbg(c, "foreground color bits: %d\n", (int)d->gfore);
	d->max_sample_value = (de_int64)(1 << (unsigned int)d->gfore) -1;

	d->num_pal_bits[0] = (de_int64)de_getbyte(pos+25);
	d->num_pal_bits[1] = (de_int64)de_getbyte(pos+26);
	d->num_pal_bits[2] = (de_int64)de_getbyte(pos+27);
	d->num_pal_bits[3] = (de_int64)de_getbyte(pos+28);
	de_dbg(c, "\"number of palette bits\" (IRGB): %d,%d,%d,%d\n",
		(int)d->num_pal_bits[0], (int)d->num_pal_bits[1],
		(int)d->num_pal_bits[2], (int)d->num_pal_bits[3] );

	d->haspect = de_getbyte(pos+30);
	d->vaspect = de_getbyte(pos+31);
	de_dbg(c, "aspect ratio: %dx%d\n", (int)d->haspect, (int)d->vaspect);

	retval = 1;
done:
	de_dbg_indent(c, -1);
	return retval;
}

static int do_tileinfo(deark *c, lctx *d, de_int64 pos, de_int64 len)
{
	int retval = 0;

	de_dbg(c, "tile information at %d\n", (int)pos);
	de_dbg_indent(c, 1);
	if(len<8) {
		de_err(c, "Tile Information item too small\n");
		goto done;
	}

	d->page_rows = de_getui16le(pos+0);
	d->page_cols = de_getui16le(pos+2);
	d->stp_rows = de_getui16le(pos+4);
	d->stp_cols = de_getui16le(pos+6);

	de_dbg(c, "page_rows=%d, page_cols=%d\n", (int)d->page_rows, (int)d->page_cols);
	de_dbg(c, "strip_rows=%d, strip_cols=%d\n", (int)d->stp_rows, (int)d->stp_cols);

	if(d->page_cols%8 != 0) {
		de_err(c, "page_cols must be a multiple of 8 (is %d)\n", (int)d->page_cols);
		goto done;
	}

	retval = 1;
done:
	de_dbg_indent(c, -1);
	return retval;
}

static de_byte getbit(const de_byte *m, de_int64 bitnum)
{
	de_byte b;
	b = m[bitnum/8];
	b = (b>>(7-bitnum%8)) & 0x1;
	return b;
}

static void do_uncompress_tile(deark *c, lctx *d, de_int64 tile_num,
	de_int64 tile_loc, de_int64 tile_len,
	dbuf *unc_pixels, de_int64 num_rows)
{
	de_byte *rowbuf1 = NULL;
	de_byte *rowbuf2 = NULL;
	de_byte *compression_bytes = NULL;
	de_int64 pos;
	de_int64 i, j;
	de_int64 plane;

	// There are d->gfore planes (1-bpp images). The first row of each plane is
	// uncompressed. The rest are compressed with a delta compression algorithm.
	// There are d->page_rows rows in each plane.

	rowbuf1 = de_malloc(c, d->rowspan);
	rowbuf2 = de_malloc(c, d->rowspan);
	compression_bytes = de_malloc(c, d->compression_bytes_per_row);

	pos = tile_loc;

	for(plane=0; plane<d->gfore; plane++) {
		if(pos >= tile_loc + tile_len) {
			de_warn(c, "Not enough data in tile %d\n", (int)tile_num);
			goto done;
		}

		for(j=0; j<num_rows; j++) {
			if(j==0) {
				// First row is stored uncompressed
				de_read(rowbuf1, pos, d->rowspan);
				pos += d->rowspan;
				de_memcpy(rowbuf2, rowbuf1, (size_t)d->rowspan);
			}
			else {
				de_read(compression_bytes, pos, d->compression_bytes_per_row);
				pos += d->compression_bytes_per_row;

				// For every 1 bit in the compression_bytes array, read a byte from the file.
				// For every 0 bit, copy the byte from the previous row.
				for(i=0; i<d->rowspan; i++) {
					if(getbit(compression_bytes, i)) {
						rowbuf2[i] = de_getbyte(pos++);
					}
					else {
						rowbuf2[i] = rowbuf1[i];
					}
				}
			}

			// TODO: Maybe instead of having separate rowbufs, we should read back what
			// we wrote to unc_pixels.
			dbuf_write(unc_pixels, rowbuf2, d->rowspan);

			// Remember the previous row
			de_memcpy(rowbuf1, rowbuf2, (size_t)d->rowspan);
		}
	}

done:
	de_free(c, compression_bytes);
	de_free(c, rowbuf1);
	de_free(c, rowbuf2);
}

static void do_render_tile(deark *c, lctx *d, struct deark_bitmap *img,
	de_int64 tile_num, de_int64 tile_loc, de_int64 tile_len)
{
	de_int64 i, j;
	de_int64 plane;
	de_int64 x_pos_in_tiles, y_pos_in_tiles;
	de_int64 x_origin_in_pixels, y_origin_in_pixels;
	de_int64 x_pos_in_pixels, y_pos_in_pixels;
	de_uint32 clr;
	unsigned int palent;
	de_byte b;
	dbuf *unc_pixels = NULL;
	de_int64 nrows_expected;

	x_pos_in_tiles = tile_num % d->stp_cols;
	y_pos_in_tiles = tile_num / d->stp_cols;

	x_origin_in_pixels = x_pos_in_tiles * d->page_cols;
	y_origin_in_pixels = y_pos_in_tiles * d->page_rows;

	// "If the actual row bound of the tile exceeds the image, the extra
	// rows are not present."
	nrows_expected = d->height - y_origin_in_pixels;
	if(nrows_expected > d->page_rows) nrows_expected = d->page_rows;

	de_dbg(c, "tile (%d,%d), pixel position (%d,%d), size %dx%d\n",
		(int)x_pos_in_tiles, (int)y_pos_in_tiles,
		(int)x_origin_in_pixels, (int)y_origin_in_pixels,
		(int)d->page_cols, (int)nrows_expected);

	unc_pixels = dbuf_create_membuf(c, 4096);

	do_uncompress_tile(c, d, tile_num, tile_loc, tile_len, unc_pixels, nrows_expected);

	// Paint the tile into the bitmap.
	for(j=0; j<d->page_rows; j++) {
		y_pos_in_pixels = y_origin_in_pixels+j;
		if(y_pos_in_pixels >= d->height) break;

		for(i=0; i<d->page_cols; i++) {
			x_pos_in_pixels = x_origin_in_pixels+i;
			if(x_pos_in_pixels >= d->width) break;

			palent = 0;
			for(plane=0; plane<d->gfore; plane++) {
				b = de_get_bits_symbol(unc_pixels, 1, plane*d->planespan + j*d->rowspan, i);
				if(b) palent |= (1<<plane);
			}

			if(palent<=255) clr = d->pal[palent];
			else clr=0;

			de_bitmap_setpixel_rgb(img, x_pos_in_pixels, y_pos_in_pixels, clr);
		}
	}

	dbuf_close(unc_pixels);
}

static void do_bitmap(deark *c, lctx *d)
{
	de_int64 pos;
	de_int64 item;
	de_int64 item_id;
	de_int64 tile_loc, tile_len;
	de_int64 tile_num;
	struct deark_bitmap *img = NULL;

	de_dbg(c, "reading image data\n");
	de_dbg_indent(c, 1);

	if(!de_good_image_dimensions(c, d->width, d->height)) goto done;

	d->rowspan = d->page_cols/8;
	d->compression_bytes_per_row = (d->rowspan+7)/8; // Just a guess. Spec doesn't say.
	d->planespan = d->rowspan * d->page_rows;

	img = de_bitmap_create(c, d->width, d->height, d->is_grayscale?1:3);

	// Read through the items again, this time looking only at the image tiles.
	for(item=0; item<d->item_count; item++) {
		pos = 4 + 8*item;
		if(pos+8 > c->infile->len) break;

		item_id = de_getui16le(pos);
		if(item_id<0x8000 || item_id==0xffff) continue;

		tile_len = de_getui16le(pos+2);
		tile_loc = de_getui32le(pos+4);

		tile_num = item_id-0x8000;
		de_dbg(c, "item #%d: tile #%d: loc=%d, len=%d\n", (int)item, (int)tile_num,
			(int)tile_loc, (int)tile_len);

		do_render_tile(c, d, img, tile_num, tile_loc, tile_len);
	}

	de_bitmap_write_to_file(img, NULL);

done:
	de_bitmap_destroy(img);
	de_dbg_indent(c, -1);
}

static void de_run_insetpix(deark *c, de_module_params *mparams)
{
	lctx *d = NULL;
	de_int64 pix_version;
	de_int64 item;
	de_int64 item_id;
	de_int64 item_loc, item_len;
	de_int64 pos;
	de_int64 imginfo_pos=0, imginfo_len=0;
	de_int64 pal_pos=0, pal_len=0;
	de_int64 tileinfo_pos=0, tileinfo_len=0;
	int indent_flag = 0;
	de_int64 k;

	d = de_malloc(c, sizeof(lctx));

	de_warn(c, "The Inset PIX module is experimental, and may not work correctly.\n");

	pix_version = de_getui16le(0);
	d->item_count = de_getui16le(2);
	de_dbg(c, "version: %d\n", (int)pix_version);
	de_dbg(c, "index at 4, %d items\n", (int)d->item_count);

	// Scan the index, and record the location of items we care about.
	// (The index will be read again when converting the image bitmap.)
	de_dbg_indent(c, 1);
	indent_flag = 1;
	for(item=0; item<d->item_count; item++) {
		pos = 4 + 8*item;
		if(pos+8 > c->infile->len) break;

		item_id = de_getui16le(pos);
		if(item_id>=0x8000) continue; // Skip "tile" items for now

		item_len = de_getui16le(pos+2);
		item_loc = de_getui32le(pos+4);
		de_dbg(c, "item #%d: id=%d, loc=%d, len=%d\n", (int)item,
			(int)item_id, (int)item_loc, (int)item_len);

		if(item_loc + item_len > c->infile->len) {
			de_err(c, "Item #%d (ID %d) goes beyond end of file\n",
				(int)item, (int)item_id);
			goto done;
		}

		switch(item_id) {
		case 0:
			imginfo_pos = item_loc;
			imginfo_len = item_len;
			break;
		case 1:
			if(!pal_pos) {
				pal_pos = item_loc;
				pal_len = item_len;
			}
			break;
		case 2:
			tileinfo_pos = item_loc;
			tileinfo_len = item_len;
			break;
		case 17: // Printing Options
		case 0xffff: // Empty item
			break;
		default:
			de_dbg(c, "unknown item type %d\n", (int)item_id);
		}
	}
	de_dbg_indent(c, -1);
	indent_flag = 0;

	if(!imginfo_pos) {
		de_err(c, "Missing Image Information item\n");
		goto done;
	}
	if(!do_image_info(c, d, imginfo_pos, imginfo_len)) goto done;

	if(d->graphics_type==0) {
		de_err(c, "Inset PIX character graphics not supported\n");
		goto done;
	}

	if(!pal_pos) {
		de_err(c, "Missing palette\n");
		goto done;
	}

	if(!do_palette(c, d, pal_pos, pal_len)) goto done;

	if(d->gfore<1 || d->gfore>8) {
		de_err(c, "Inset PIX with %d bits/pixel are not supported\n", (int)d->gfore);
		goto done;
	}

	for(k=0; k<4; k++) {
		if(d->num_pal_bits[k]>8) {
			de_err(c, "Invalid palette bits/sample setting (%d)\n", (int)d->num_pal_bits[k]);
			goto done;
		}
	}

	if(d->num_pal_bits[0]!=0 && d->num_pal_bits[1]==0 &&
		d->num_pal_bits[2]==0 && d->num_pal_bits[3]==0)
	{
		d->is_grayscale = 1;
	}

	if(!tileinfo_pos) {
		de_err(c, "Missing Tile Information item\n");
		goto done;
	}

	if(!do_tileinfo(c, d, tileinfo_pos, tileinfo_len)) goto done;

	do_bitmap(c, d);

done:
	if(indent_flag) de_dbg_indent(c, -1);

	de_free(c, d);
}

// Inset PIX is hard to identify.
static int de_identify_insetpix(deark *c)
{
	de_int64 pix_version;
	de_int64 item_count;
	de_int64 item;
	de_int64 item_loc, item_len;

	if(!de_input_file_has_ext(c, "pix")) return 0;

	pix_version = de_getui16le(0);
	// The only version number I know of is 3, but I don't know what other
	// versions may exist.
	if(pix_version<1 || pix_version>4) return 0;

	item_count = de_getui16le(2);
	// Need at least 4 items (image info, palette info, tile info, and 1 tile).
	if(item_count<4) return 0;

	if(4 + 8*item_count >= c->infile->len) return 0;

	for(item=0; item<item_count && item<16; item++) {
		item_len = de_getui16le(4+8*item+2);
		item_loc = de_getui32le(4+8*item+4);
		if(item_loc < 4 + 8*item_count) return 0;
		if(item_loc+item_len > c->infile->len) return 0;
	}

	return 20;
}

void de_module_insetpix(deark *c, struct deark_module_info *mi)
{
	mi->id = "insetpix";
	mi->run_fn = de_run_insetpix;
	mi->identify_fn = de_identify_insetpix;
}
