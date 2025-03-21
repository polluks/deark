// This file is part of Deark.
// Copyright (C) 2016 Jason Summers
// See the file COPYING for terms of use.

// Extract various things from TIFF (and similar) image files

#include <deark-config.h>
#include <deark-private.h>
#include <deark-fmtutil.h>
DE_DECLARE_MODULE(de_module_tiff);

#define ITEMS_IN_ARRAY DE_ARRAYCOUNT
#define MAX_IFDS 1000
#define DE_TIFF_MAX_STRILES 65536
#define DE_TIFF_MAX_SAMPLES 10
#define DE_TIFF_MAX_VALCOUNT (1LL<<44)
#define DE_TIFF_ALLOWED_VIRTUAL_BYTES 1024

#define DE_TIFF_MAX_VALUES_TO_PRINT 100
#define DE_TIFF_MAX_CHARS_TO_PRINT  DE_DBG_MAX_STRLEN

#define DATATYPE_BYTE      1
#define DATATYPE_ASCII     2
#define DATATYPE_UINT16    3
#define DATATYPE_UINT32    4
#define DATATYPE_RATIONAL  5
#define DATATYPE_SBYTE     6
#define DATATYPE_UNDEF     7
#define DATATYPE_SINT16    8
#define DATATYPE_SINT32    9
#define DATATYPE_SRATIONAL 10
#define DATATYPE_FLOAT32   11
#define DATATYPE_FLOAT64   12
#define DATATYPE_IFD32     13
#define DATATYPE_UINT64    16
#define DATATYPE_SINT64    17
#define DATATYPE_IFD64     18
#define DATATYPE_UTF8      129 // From Exif 3.0

#define DE_TIFFFMT_TIFF       1
#define DE_TIFFFMT_BIGTIFF    2
#define DE_TIFFFMT_PANASONIC  3 // Panasonic RAW / RW2
#define DE_TIFFFMT_ORF        4 // Olympus RAW
#define DE_TIFFFMT_DCP        5 // DNG Camera Profile (DCP)
#define DE_TIFFFMT_MDI        6 // Microsoft Office Document Imaging
#define DE_TIFFFMT_JPEGXR     7 // JPEG XR
#define DE_TIFFFMT_MPEXT      8 // "MP Extension" data from MPF format
#define DE_TIFFFMT_NIKONMN    9 // Nikon MakerNote
#define DE_TIFFFMT_APPLEMN    10 // Apple iOS MakerNote
#define DE_TIFFFMT_FUJIFILMMN 11 // FujiFilm MakerNote

#define IFDTYPE_NORMAL       0
#define IFDTYPE_SUBIFD       1
#define IFDTYPE_EXIF         2
#define IFDTYPE_EXIFINTEROP  3
#define IFDTYPE_GPS          4
#define IFDTYPE_GLOBALPARAMS 5 // TIFF-FX
#define IFDTYPE_NIKONMN      6 // First IFD of a Nikon MakerNote
#define IFDTYPE_NIKONPREVIEW 7
#define IFDTYPE_APPLEMN      8
#define IFDTYPE_MASKSUBIFD   9
#define IFDTYPE_FUJIFILMMN   10

#define TAG_NEWSUBFILETYPE      254
#define TAG_OLDSUBFILETYPE      255
#define TAG_IMAGEWIDTH          256
#define TAG_IMAGELENGTH         257
#define TAG_BITSPERSAMPLE       258
#define TAG_COMPRESSION         259
#define TAG_PHOTOMETRIC         262
#define TAG_FILLORDER           266
#define TAG_STRIPOFFSETS        273
#define TAG_ORIENTATION         274
#define TAG_SAMPLESPERPIXEL     277
#define TAG_ROWSPERSTRIP        278
#define TAG_STRIPBYTECOUNTS     279
#define TAG_XRESOLUTION         282
#define TAG_YRESOLUTION         283
#define TAG_PLANARCONFIG        284
#define TAG_T4OPTIONS           292
#define TAG_T6OPTIONS           293
#define TAG_RESOLUTIONUNIT      296
#define TAG_DATETIME            306
#define TAG_PREDICTOR           317
#define TAG_TILEWIDTH           322
#define TAG_TILELENGTH          323
#define TAG_TILEOFFSETS         324
#define TAG_TILEBYTECOUNTS      325
#define TAG_SAMPLEFORMAT        339
#define TAG_JPEGTABLES          347
#define TAG_JPEGINTERCHANGEFORMAT 513
#define TAG_JPEGINTERCHANGEFORMATLENGTH 514
#define TAG_YCBCRPOSITIONING    531

#define CMPR_NONE          1
#define CMPR_CCITTRLE      2
#define CMPR_FAX3          3
#define CMPR_FAX4          4
#define CMPR_LZW           5
#define CMPR_OLDJPEG       6
#define CMPR_NEWJPEG       7
#define CMPR_DEFLATE       8
#define CMPR_PACKBITS      32773
#define CMPR_DEFLATE_ALT   32946

struct page_ctx;
struct decode_page_ctx;
struct localctx_struct;
typedef struct localctx_struct lctx;
struct taginfo;
struct tagnuminfo;

struct ifdstack_item {
	i64 offset;
	u8 ifdtype;
	u8 is_in_main_ifd_list;
};

typedef void (*handler_fn_type)(deark *c, lctx *d, const struct taginfo *tg,
	const struct tagnuminfo *tni);

struct valdec_params {
	lctx *d;
	const struct taginfo *tg;
	i64 idx;
	i64 n;
};
struct valdec_result {
	// Value decoders will be called with a valid, empty ucstring 's'.
	de_ucstring *s;
};

typedef int (*val_decoder_fn_type)(deark *c, const struct valdec_params *vp, struct valdec_result *vr);

struct tagnuminfo {
	int tagnum;

	// 0x0001=NOT valid in normal TIFF files/IFDs
	// 0x0004=multi-string ASCII type expected
	// 0x08=suppress auto display of values
	// 0x10=this is an Exif tag
	// 0x20=an Exif Interoperability-IFD tag
	// 0x40=a GPS attribute tag
	// 0x80=a DNG tag
	// 0x0100=TIFF/EP
	// 0x0200=TIFF/IT
	// 0x0400=tags valid in JPEG XR files (from the spec, and jxrlib)
	// 0x0800=tags for Multi-Picture Format (.MPO) extensions
	// 0x1000=tags for Nikon MakerNote
	// 0x2000=tags for Apple iOS MakerNote
	// 0x4000=Panasonic RAW/RW2
	// 0x8000=FUJIFILM
	unsigned int flags;

	const char *tagname;
	handler_fn_type hfn;
	val_decoder_fn_type vdfn;
};

typedef void (*decompressor_fn)(deark *c, lctx *d, struct page_ctx *pg,
	struct decode_page_ctx *dctx);

struct cmpr_meth_info {
	u32 id;
	UI flags;
	const char *name;
	decompressor_fn decompressor;
};

struct strile_data_struct {
	i64 pos;
	i64 len;
};

struct page_ctx {
	i64 ifdpos;
	int ifd_idx;
	int ifd_idx_in_main_list; // -1 if not in main list
	u8 ifdtype;

	u8 have_imagewidth;
	u8 have_jpeglength;
	u8 is_thumb;
	u8 have_density;
	u8 have_strip_tags, have_tile_tags;
	u8 have_strilebytecounts;
	u8 have_jpegtables;
	u8 have_colormap;
	u8 have_oldsubfiletype;
	u8 have_jxr_imageoffset;

	u32 compression;
	u32 orientation;
	u32 ycbcrpositioning;
	u32 bits_per_sample;
	u32 samples_per_pixel;
	u32 photometric;
	u32 fill_order;
	u32 planarconfig;
	u32 predictor;
	u32 sample_format;
	u32 resolution_unit;
	u32 t4options, t6options;
	double density_ImageWidth, density_ImageLength;
	i64 imagewidth, imagelength; // Raw tag values, before considering Orientation
	i64 jpegoffset;
	i64 jpeglength;
	i64 rows_per_strip;
	i64 tile_width, tile_height;
	u8 is_old_lzw;
	u8 is_dexxa;
	u8 dexxa_ncolors;
	u8 valcount_bitspersample;
	u8 extrasamples_count;
	u8 extrasample_type[DE_TIFF_MAX_SAMPLES]; // [0] = first *extra* sample, not first sample
	const struct cmpr_meth_info *cmi;

	i64 strile_count;
	struct strile_data_struct *strile_data; // array[strile_count]
	struct de_timestamp internal_mod_time;
	de_color pal[256];
	char ifdnum_string[20];
	char errmsgtoken_ifd[40];
};

// Data associated with an actual tag in an IFD in the file
struct taginfo {
	// Might be more logical for us to have a separate struct for page_ctx, but
	// I don't want to add a param to every "handler" function
	struct page_ctx *pg;

	int tagnum;
	int datatype;
	int tag_known;
	i64 valcount;
	i64 val_offset;
	i64 unit_size;
	i64 total_size;
};

struct localctx_struct {
	int is_le;
	int fmt;
	u8 is_bigtiff;
	u8 is_xiff;
	int is_exif_submodule;
	int can_decode_fltpt;
	u8 opt_decode;
	u8 opt_dexxa; // 0xff=auto

	// 0=default
	// 1=if in doubt, ignore
	// 2=reverse pixels after decompressing
	u8 fillorder_strategy;

	u8 is_deark_iptc, is_deark_8bim;

	u32 first_ifd_orientation; // Valid if != 0
	u32 exif_version_as_uint32; // Valid if != 0
	u8 has_exif_gps;
	u8 first_ifd_cosited;

	struct ifdstack_item *ifdstack;
	int ifdstack_capacity;
	int ifdstack_numused;
	de_encoding current_textfield_encoding;

	struct de_inthashtable *ifds_seen;
	int ifd_count; // Number of IFDs that we currently know of
	int num_main_ifds_processed;

	i64 ifdhdrsize;
	i64 ifditemsize;
	i64 offsetoffset;
	i64 offsetsize; // Number of bytes in a file offset

	const struct de_module_in_params *in_params;

	unsigned int mpf_main_image_count;

	char errmsgtoken_module[40];
};

static void detiff_err(deark *c, lctx *d, struct page_ctx *pg, const char *fmt, ...)
	de_gnuc_attribute ((format (printf, 4, 5)));

// pg can be NULL (if not in IFD context)
static void detiff_err(deark *c, lctx *d, struct page_ctx *pg, const char *fmt, ...)
{
	va_list ap;
	char buf[256];

	va_start(ap, fmt);
	if(d->errmsgtoken_module[0] && pg) {
		de_vsnprintf(buf, sizeof(buf), fmt, ap);
		de_err(c, "[%s:%s] %s", d->errmsgtoken_module, pg->errmsgtoken_ifd, buf);
	}
	else if(d->errmsgtoken_module[0]) {
		de_vsnprintf(buf, sizeof(buf), fmt, ap);
		de_err(c, "[%s] %s", d->errmsgtoken_module, buf);
	}
	else if(pg) {
		de_vsnprintf(buf, sizeof(buf), fmt, ap);
		de_err(c, "[%s] %s", pg->errmsgtoken_ifd, buf);
	}
	else {
		de_verr(c, fmt, ap);
	}
	va_end(ap);
}

static void detiff_warn(deark *c, lctx *d, struct page_ctx *pg, const char *fmt, ...)
	de_gnuc_attribute ((format (printf, 4, 5)));

static void detiff_warn(deark *c, lctx *d, struct page_ctx *pg, const char *fmt, ...)
{
	va_list ap;
	char buf[256];

	va_start(ap, fmt);
	if(d->errmsgtoken_module[0] && pg) {
		de_vsnprintf(buf, sizeof(buf), fmt, ap);
		de_warn(c, "[%s:%s] %s", d->errmsgtoken_module, pg->errmsgtoken_ifd, buf);
	}
	else if(d->errmsgtoken_module[0]) {
		de_vsnprintf(buf, sizeof(buf), fmt, ap);
		de_warn(c, "[%s] %s", d->errmsgtoken_module, buf);
	}
	else if(pg) {
		de_vsnprintf(buf, sizeof(buf), fmt, ap);
		de_warn(c, "[%s] %s", pg->errmsgtoken_ifd, buf);
	}
	else {
		de_vwarn(c, fmt, ap);
	}
	va_end(ap);
}

// Returns 0 if stack is empty.
static i64 pop_ifd(deark *c, lctx *d, u8 *ifdtype, u8 *is_in_main_ifd_list)
{
	i64 ifdpos;
	if(!d->ifdstack) return 0;
	if(d->ifdstack_numused<1) return 0;
	ifdpos = d->ifdstack[d->ifdstack_numused-1].offset;
	*ifdtype = d->ifdstack[d->ifdstack_numused-1].ifdtype;
	*is_in_main_ifd_list = d->ifdstack[d->ifdstack_numused-1].is_in_main_ifd_list;
	d->ifdstack_numused--;
	return ifdpos;
}

static void push_ifd(deark *c, lctx *d, i64 ifdpos, u8 ifdtype, u8 is_in_main_ifd_list)
{
	if(ifdpos==0) return;

	if(ifdpos < 0 || ifdpos >= c->infile->len) {
		detiff_warn(c, d, NULL, "Invalid IFD offset: %"I64_FMT, ifdpos);
		return;
	}

	// Append to the IFD list (of all IFDs). This is only used for loop detection.
	if(!d->ifds_seen) {
		d->ifds_seen = de_inthashtable_create(c);
	}
	if(d->ifd_count >= MAX_IFDS) {
		detiff_warn(c, d, NULL, "Too many TIFF IFDs");
		return;
	}
	if(!de_inthashtable_add_item(c, d->ifds_seen, ifdpos, NULL)) {
		detiff_err(c, d, NULL, "IFD loop detected");
		return;
	}
	d->ifd_count++;

	// Add to the IFD stack (of unprocessed IFDs).
	if(!d->ifdstack) {
		d->ifdstack_capacity = 200;
		d->ifdstack = de_mallocarray(c, d->ifdstack_capacity, sizeof(struct ifdstack_item));
		d->ifdstack_numused = 0;
	}
	if(d->ifdstack_numused >= d->ifdstack_capacity) {
		detiff_warn(c, d, NULL, "Too many TIFF IFDs");
		return;
	}
	d->ifdstack[d->ifdstack_numused].offset = ifdpos;
	d->ifdstack[d->ifdstack_numused].ifdtype = ifdtype;
	d->ifdstack[d->ifdstack_numused].is_in_main_ifd_list = is_in_main_ifd_list;
	d->ifdstack_numused++;
}

static int size_of_data_type(int tt)
{
	switch(tt) {
	case DATATYPE_BYTE: case DATATYPE_SBYTE:
	case DATATYPE_ASCII: case DATATYPE_UTF8:
	case DATATYPE_UNDEF:
		return 1;
	case DATATYPE_UINT16: case DATATYPE_SINT16:
		return 2;
	case DATATYPE_UINT32: case DATATYPE_SINT32:
	case DATATYPE_FLOAT32:
	case DATATYPE_IFD32:
		return 4;
	case DATATYPE_RATIONAL: case DATATYPE_SRATIONAL:
	case DATATYPE_FLOAT64:
	case DATATYPE_UINT64: case DATATYPE_SINT64:
	case DATATYPE_IFD64:
		return 8;
	}
	return 1;
}

static int read_rational_as_double(deark *c, lctx *d, i64 pos, double *n)
{
	i64 num, den;

	*n = 0.0;
	num = dbuf_getu32x(c->infile, pos, d->is_le);
	den = dbuf_getu32x(c->infile, pos+4, d->is_le);
	if(den==0) return 0;
	*n = (double)num/(double)den;
	return 1;
}

static int read_srational_as_double(deark *c, lctx *d, i64 pos, double *n)
{
	i64 num, den;

	*n = 0.0;
	num = dbuf_geti32x(c->infile, pos, d->is_le);
	den = dbuf_geti32x(c->infile, pos+4, d->is_le);
	if(den==0) return 0;
	*n = (double)num/(double)den;
	return 1;
}

// FIXME: This function seems superfluous.
// It should somehow be consolidated with read_numeric_value().
static int read_tag_value_as_double(deark *c, lctx *d, const struct taginfo *tg,
	i64 value_index, double *n)
{
	i64 offs;

	*n = 0.0;
	if(value_index<0 || value_index>=tg->valcount) return 0;
	offs = tg->val_offset + value_index*tg->unit_size;

	switch(tg->datatype) {
	case DATATYPE_RATIONAL:
		return read_rational_as_double(c, d, offs, n);
	case DATATYPE_SRATIONAL:
		return read_srational_as_double(c, d, offs, n);
	case DATATYPE_FLOAT32:
		if(!d->can_decode_fltpt) return 0;
		*n = dbuf_getfloat32x(c->infile, offs, d->is_le);
		return 1;
	case DATATYPE_FLOAT64:
		if(!d->can_decode_fltpt) return 0;
		*n = dbuf_getfloat64x(c->infile, offs, d->is_le);
		return 1;

		// There should be no need to support other data types (like UINT32).
	}
	return 0;
}

static int read_tag_value_as_int64(deark *c, lctx *d, const struct taginfo *tg,
	i64 value_index, i64 *n)
{
	double v_dbl;
	i64 offs;

	*n = 0;
	if(value_index<0 || value_index>=tg->valcount) return 0;
	offs = tg->val_offset + value_index*tg->unit_size;

	switch(tg->datatype) {
	case DATATYPE_UINT16:
		*n = dbuf_getu16x(c->infile, offs, d->is_le);
		return 1;
	case DATATYPE_UINT32:
	case DATATYPE_IFD32:
		*n = dbuf_getu32x(c->infile, offs, d->is_le);
		return 1;
	case DATATYPE_BYTE:
	case DATATYPE_UNDEF:
	case DATATYPE_ASCII:
	case DATATYPE_UTF8:
		*n = (i64)de_getbyte(offs);
		return 1;
	case DATATYPE_UINT64:
	case DATATYPE_IFD64:
		// TODO: Somehow support unsigned 64-bit ints that don't fit into
		// a i64?
		*n = dbuf_geti64x(c->infile, offs, d->is_le);
		if(*n < 0) return 0;
		return 1;
	case DATATYPE_SINT16:
		*n = dbuf_geti16x(c->infile, offs, d->is_le);
		return 1;
	case DATATYPE_SINT32:
		*n = dbuf_geti32x(c->infile, offs, d->is_le);
		return 1;
	case DATATYPE_SINT64:
		*n = dbuf_geti64x(c->infile, offs, d->is_le);
		return 1;
	case DATATYPE_SBYTE:
		*n = dbuf_geti8(c->infile, offs);
		return 1;
	case DATATYPE_RATIONAL:
	case DATATYPE_SRATIONAL:
	case DATATYPE_FLOAT32:
	case DATATYPE_FLOAT64:
		if(read_tag_value_as_double(c, d, tg, value_index, &v_dbl)) {
			*n = (i64)v_dbl;
			return 1;
		}
		return 0;
	}
	return 0;
}

static void format_double(de_ucstring *s, double val)
{
	// TODO: Formatting should be more intelligent
	ucstring_printf(s, DE_ENCODING_LATIN1, "%f", val);
}

struct numeric_value {
	int isvalid;
	i64 val_int64;
	double val_double;
};

// Do-it-all function for reading numeric values.
// If dbglinebuf!=NULL, print a string representation of the value to it.
static void read_numeric_value(deark *c, lctx *d, const struct taginfo *tg,
	i64 value_index, struct numeric_value *nv, de_ucstring *dbgline)
{
	int ret;
	i64 offs;

	nv->isvalid = 0;
	nv->val_int64 = 0;
	nv->val_double = 0.0;

	// FIXME: This is recalculated in read_tag_value_as_int64.
	offs = tg->val_offset + value_index*tg->unit_size;

	switch(tg->datatype) {
	case DATATYPE_BYTE:
	case DATATYPE_SBYTE:
	case DATATYPE_UNDEF:
	case DATATYPE_ASCII:
	case DATATYPE_UTF8:
	case DATATYPE_UINT16:
	case DATATYPE_SINT16:
	case DATATYPE_UINT32:
	case DATATYPE_SINT32:
	case DATATYPE_IFD32:
	case DATATYPE_UINT64:
	case DATATYPE_SINT64:
	case DATATYPE_IFD64:
		ret = read_tag_value_as_int64(c, d, tg, value_index, &nv->val_int64);
		nv->val_double = (double)nv->val_int64;
		nv->isvalid = ret;
		if(dbgline) {
			if(nv->isvalid)
				ucstring_printf(dbgline, DE_ENCODING_LATIN1, "%" I64_FMT, nv->val_int64);
			else
				ucstring_append_sz(dbgline, "?", DE_ENCODING_LATIN1);
		}
		break;

	case DATATYPE_RATIONAL:
	case DATATYPE_SRATIONAL:
		{
			i64 num, den;

			if(tg->datatype==DATATYPE_SRATIONAL) {
				num = dbuf_geti32x(c->infile, offs, d->is_le);
				den = dbuf_geti32x(c->infile, offs+4, d->is_le);
			}
			else {
				num = dbuf_getu32x(c->infile, offs, d->is_le);
				den = dbuf_getu32x(c->infile, offs+4, d->is_le);
			}

			if(den==0) {
				nv->isvalid = 0;
				nv->val_double = 0.0;
				nv->val_int64 = 0;
				if(dbgline) {
					ucstring_printf(dbgline, DE_ENCODING_LATIN1, "%" I64_FMT "/%" I64_FMT, num, den);
				}

			}
			else {
				nv->isvalid = 1;
				nv->val_double = (double)num/(double)den;
				nv->val_int64 = (i64)nv->val_double;
				if(dbgline) {
					format_double(dbgline, nv->val_double);
				}
			}
		}
		break;

	case DATATYPE_FLOAT32:
	case DATATYPE_FLOAT64:
		if(tg->datatype==DATATYPE_FLOAT64) {
			nv->val_double = dbuf_getfloat64x(c->infile, offs, d->is_le);
		}
		else {
			nv->val_double = dbuf_getfloat32x(c->infile, offs, d->is_le);
		}
		nv->val_int64 = (i64)nv->val_double;
		nv->isvalid = 1;
		if(dbgline) {
			format_double(dbgline, nv->val_double);
		}
		break;

	default:
		if(dbgline) {
			ucstring_append_sz(dbgline, "?", DE_ENCODING_LATIN1);
		}
	}
}

static i64 get_u32_or_u64_count(deark *c, lctx *d, i64 pos)
{
	if(d->is_bigtiff) {
		i64 n;

		n = dbuf_geti64x(c->infile, pos, d->is_le);
		de_sanitize_count(&n);
		return n;
	}
	return dbuf_getu32x(c->infile, pos, d->is_le);
}

#define getfpos      get_u32_or_u64_count
#define getvalcount  get_u32_or_u64_count

static void do_oldjpeg(deark *c, lctx *d, struct page_ctx *pg)
{
	const char *extension;
	unsigned int createflags;
	i64 jpeglength = pg->jpeglength;

	if(pg->jpegoffset==0) return;
	if(!pg->have_jpeglength) {
		// Missing JPEGInterchangeFormatLength tag. Assume it goes to the end
		// of the file.
		jpeglength = c->infile->len - pg->jpegoffset;
	}
	if(jpeglength<1 || jpeglength>DE_MAX_SANE_OBJECT_SIZE) {
		return;
	}

	// Note that the position & length have already been validated, in
	// postprocess_ifd_tags().

	if(dbuf_memcmp(c->infile, pg->jpegoffset, "\xff\xd8\xff", 3)) {
		detiff_warn(c, d, pg, "Expected JPEG data at %"I64_FMT" not found", pg->jpegoffset);
		return;
	}

	// Found an embedded JPEG image or thumbnail that we can extract.
	if(d->is_exif_submodule) {
		extension = "exifthumb.jpg";
		createflags = DE_CREATEFLAG_IS_AUX;
	}
	else if(d->fmt==DE_TIFFFMT_NIKONMN) {
		extension = "nikonthumb.jpg";
		createflags = DE_CREATEFLAG_IS_AUX;
	}
	else {
		extension = "jpg";
		// TODO: Should createflags be set to DE_CREATEFLAG_IS_AUX in some cases?
		createflags = 0;
	}
	dbuf_create_file_from_slice(c->infile, pg->jpegoffset, jpeglength, extension, NULL, createflags);
}

static void do_leaf_metadata(deark *c, lctx *d, i64 pos1, i64 len)
{
	i64 pos;
	u8 buf[4];
	u8 segtype[40];
	i64 data_len;

	if(len<1) return;
	if(pos1+len > c->infile->len) return;
	de_dbg(c, "leaf metadata at %d size=%d", (int)pos1, (int)len);

	// This format appears to be hierarchical, but for now we only care about
	// the top level.

	pos = pos1;
	while(pos < pos1+len) {
		de_read(buf, pos, 4);
		if(de_memcmp(buf, "PKTS", 4)) {
			break;
		}
		pos+=4;

		pos+=4; // Don't know what these 4 bytes are for.

		de_read(segtype, pos, 40);
		pos+=40;

		// TODO: Is this always big-endian?
		data_len = de_getu32be(pos);
		pos+=4;

		if(!de_memcmp(segtype, "JPEG_preview_data\0", 18)) {
			de_dbg(c, "jpeg preview at %d len=%d", (int)pos, (int)data_len);
			dbuf_create_file_from_slice(c->infile, pos, data_len, "leafthumb.jpg", NULL, DE_CREATEFLAG_IS_AUX);
		}
		pos += data_len;
	}
}

struct int_and_str {
	i64 n;
	const char *s;
};

static const char *lookup_str(const struct int_and_str *items, size_t num_items, i64 n)
{
	i64 i;

	for(i=0; i<(i64)num_items; i++) {
		if(items[i].n==n && items[i].s!=NULL) {
			return items[i].s;
		}
	}
	return "?";
}

static void lookup_str_and_append_to_ucstring(const struct int_and_str *items, size_t num_items,
	i64 n, de_ucstring *s)
{
	const char *sz;

	sz = lookup_str(items, num_items, n);
	ucstring_append_sz(s, sz, DE_ENCODING_UTF8);
}

static int valdec_newsubfiletype(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	i64 n = vp->n;

	if(n<1) return 0;

	if(n&0x1) {
		ucstring_append_flags_item(vr->s, "reduced-res");
		n -= 0x1;
	}
	if(n&0x2) {
		ucstring_append_flags_item(vr->s, "one-page-of-many");
		n -= 0x2;
	}
	if(n&0x4) {
		ucstring_append_flags_item(vr->s, "mask");
		n -= 0x4;
	}
	if(n&0x10) {
		ucstring_append_flags_item(vr->s, "MRC-related");
		n -= 0x10;
	}
	if(n!=0) {
		ucstring_append_flags_itemf(vr->s, "0x%x", (unsigned int)n);
	}

	return 1;
}

static int valdec_oldsubfiletype(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{1, "full-res"}, {2, "reduced-res"}, {3, "one-page-of-many"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static const struct cmpr_meth_info *find_cmpr_meth_info(u32 id);

static int valdec_compression(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	const struct cmpr_meth_info *cmi;

	cmi = find_cmpr_meth_info((u32)vp->n);

	if(cmi && cmi->name) {
		ucstring_append_sz(vr->s, cmi->name, DE_ENCODING_UTF8);
	}
	else {
		ucstring_append_sz(vr->s, "?", DE_ENCODING_LATIN1);
	}
	return 1;
}

static const struct int_and_str photometric_name_map[] = {
	{0, "grayscale/white-is-0"}, {1, "grayscale/black-is-0"},
	{2, "RGB"}, {3, "palette"}, {4, "Holdout Mask"}, {5, "CMYK"}, {6, "YCbCr"},
	{8, "CIELab"}, {9, "ICCLab"}, {10, "ITULab"},
	{32803, "CFA"}, {32844, "CIELog2L"}, {32845, "CIELog2Luv"},
	{34892, "LinearRaw"}
};
static int valdec_photometric(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	lookup_str_and_append_to_ucstring(photometric_name_map, ITEMS_IN_ARRAY(photometric_name_map), vp->n, vr->s);
	return 1;
}

static int valdec_threshholding(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{1, "not dithered"}, {2, "ordered dither"}, {3, "error diffusion"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_fillorder(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{1, "MSB-first"}, {2, "LSB-first"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_orientation(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	ucstring_append_sz(vr->s, fmtutil_tiff_orientation_name(vp->n), DE_ENCODING_UTF8);
	return 1;
}

static int valdec_planarconfiguration(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{1, "contiguous"}, {2, "separated"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_t4options(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	if(vp->n<1) return 0;

	if(vp->n&0x1) {
		ucstring_append_flags_item(vr->s, "2-d encoding");
	}
	if(vp->n&0x2) {
		ucstring_append_flags_item(vr->s, "uncompressed mode allowed");
	}
	if(vp->n&0x4) {
		ucstring_append_flags_item(vr->s, "has fill bits");
	}
	if((vp->n & ~0x7)!=0) {
		ucstring_append_flags_item(vr->s, "?");
	}

	return 1;
}

static int valdec_t6options(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	if(vp->n<1) return 0;

	if(vp->n&0x2) {
		ucstring_append_flags_item(vr->s, "uncompressed mode allowed");
	}
	if((vp->n & ~0x2)!=0) {
		ucstring_append_flags_item(vr->s, "?");
	}

	return 1;
}

static int valdec_resolutionunit(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{1, "unspecified"}, {2, "pixels/inch"}, {3, "pixels/cm"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_pagenumber(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	if(vp->idx==0) {
		ucstring_printf(vr->s, DE_ENCODING_LATIN1, "page %d", (int)(vp->n+1));
		return 1;
	}
	if(vp->idx==1) {
		if(vp->n==0) {
			ucstring_append_sz(vr->s, "of an unknown number", DE_ENCODING_LATIN1);
		}
		else {
			ucstring_printf(vr->s, DE_ENCODING_LATIN1, "of %d", (int)vp->n);
		}
		return 1;
	}
	return 0;
}

static int valdec_predictor(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{1, "none"}, {2, "horizontal differencing"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_inkset(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{1, "CMYK"}, {2, "not CMYK"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_extrasamples(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{0, "unspecified"}, {1, "assoc-alpha"}, {2, "unassoc-alpha"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_sampleformat(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{1, "uint"}, {2, "signed int"}, {3, "float"}, {4, "undefined"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_jpegproc(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{1, "baseline"}, {14, "lossless+huffman"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_ycbcrpositioning(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{1, "centered"}, {2, "cosited"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_exposureprogram(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{0, "not defined"}, {1, "manual"}, {2, "normal program"}, {3, "aperture priority"},
		{4, "shutter priority"}, {5, "creative program"}, {6, "action program"},
		{7, "portrait mode"}, {8, "landscape mode"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_componentsconfiguration(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{0, "n/a"}, {1, "Y"}, {2, "Cb"}, {3, "Cr"}, {4, "R"}, {5, "G"}, {6, "B"},
		{48, "n/a?"}, {49, "Y?"}, {50, "Cb?"}, {51, "Cr?"}, {52, "R?"}, {53, "G?"}, {54, "B?"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_meteringmode(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{0, "unknown"}, {1, "Average"}, {2, "CenterWeightedAverage"},
		{3, "Spot"}, {4, "MultiSpot"}, {5, "Pattern"}, {6, "Partial"},
		{255, "other"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_lightsource(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{0, "unknown"}, {1, "Daylight"}, {2, "Fluorescent"},
		{3, "Tungsten"}, {4, "Flash"}, {9, "Fine weather"}, {10, "Cloudy weather"},
		{11, "Shade"}, {12, "D 5700-7100K"}, {13, "N 4600-5500K"},
		{14, "W 3800-4500K"}, {15, "WW 3250-3800K"}, {16, "L 2600-3260K"},
		{17, "Standard light A"}, {18, "Standard light B"}, {19, "Standard light C"},
		{20, "D55"}, {21, "D65"}, {22, "D75"}, {23, "D50"}, {24, "ISO studio tungsten"},
		{255, "other"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_flash(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	i64 v;

	ucstring_append_flags_item(vr->s, (vp->n&0x01)?"flash fired":"flash did not fire");

	v = (vp->n&0x06)>>1;
	if(v==0) ucstring_append_flags_item(vr->s, "no strobe return detection function");
	else if(v==2) ucstring_append_flags_item(vr->s, "strobe return light not detected");
	else if(v==3) ucstring_append_flags_item(vr->s, "strobe return light detected");

	v = (vp->n&0x18)>>3;
	if(v==1) ucstring_append_flags_item(vr->s, "compulsory flash firing");
	else if(v==2) ucstring_append_flags_item(vr->s, "compulsory flash suppression");
	else if(v==3) ucstring_append_flags_item(vr->s, "auto mode");

	ucstring_append_flags_item(vr->s, (vp->n&0x20)?"no flash function":"flash function present");

	if(vp->n&0x40) ucstring_append_flags_item(vr->s, "red eye reduction supported");

	if((vp->n & ~0x7f)!=0) {
		ucstring_append_flags_item(vr->s, "?");
	}

	return 1;
}

static int valdec_exifcolorspace(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{1, "sRGB"}, {0xffff, "Uncalibrated"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_filesource(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{0, "others"}, {1, "scanner of transparent type"},
		{2, "scanner of reflex type"}, {3, "DSC"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_scenetype(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{1, "directly photographed"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_sensingmethod(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{1, "not defined"}, {2, "1-chip color area"}, {3, "2-chip color area"},
		{4, "3-chip color area"}, {5, "color sequential area"}, {7, "trilinear"},
		{8, "color sequential linear"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_customrendered(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{0, "normal"}, {1, "custom"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_exposuremode(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{0, "auto"}, {1, "manual"}, {2, "auto bracket"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_whitebalance(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{0, "auto"}, {1, "manual"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_scenecapturetype(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{0, "standard"}, {1, "landscape"}, {2, "portrait"}, {3, "night scene"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_gaincontrol(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{0, "none"}, {1, "low gain up"}, {2, "high gain up"},
		{3, "low gain down"}, {4, "high gain down"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_contrast(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{0, "normal"}, {1, "soft"}, {2, "hard"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_saturation(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{0, "normal"}, {1, "low"}, {2, "high"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_sharpness(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{0, "normal"}, {1, "soft"}, {2, "hard"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_subjectdistancerange(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{0, "unknown"}, {1, "macro"}, {2, "close"}, {3, "distant"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_profileembedpolicy(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{0, "allow copying"}, {1, "embed if used"}, {2, "embed never"}, {3, "no restrictions"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static int valdec_dngcolorspace(deark *c, const struct valdec_params *vp, struct valdec_result *vr)
{
	static const struct int_and_str name_map[] = {
		{0, "unknown"}, {1, "gray gamma 2.2"}, {2, "sRGB"}, {3, "Adobe RGB"},
		{4, "ProPhoto RGB"}
	};
	lookup_str_and_append_to_ucstring(name_map, ITEMS_IN_ARRAY(name_map), vp->n, vr->s);
	return 1;
}

static void handler_hexdump(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	de_dbg_hexdump(c, c->infile, tg->val_offset, tg->total_size, 256, NULL, 0x1);
}

// Hex dump with no ASCII
static void handler_hexdumpb(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	de_dbg_hexdump(c, c->infile, tg->val_offset, tg->total_size, 256, NULL, 0);
}

static void handler_bplist(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	if(tg->total_size>=40 &&
		!dbuf_memcmp(c->infile, tg->val_offset, "bplist", 6))
	{
		de_dbg(c, "binary .plist at %"I64_FMT", len=%"I64_FMT, tg->val_offset, tg->total_size);
		de_dbg_indent(c, 1);
		fmtutil_handle_plist(c, c->infile, tg->val_offset, tg->total_size, NULL, 0x0);
		de_dbg_indent(c, -1);
	}
	else {
		handler_hexdump(c, d, tg, tni);
	}
}

static int check_for_8bit_pal(deark *c, lctx *d, const struct taginfo *tg, i64 num_entries)
{
	i64 i;
	i64 num_samples;
	u8 found_dark = 0;

	if(num_entries<2 || num_entries>256) return 0;
	num_samples = num_entries*3;
	if(num_samples > tg->valcount) return 0;

	for(i=0; i<num_samples; i++) {
		i64 s;

		read_tag_value_as_int64(c, d, tg, i, &s);
		if(s>=256) return 0;
		if(s>1) found_dark = 1;
	}
	if(found_dark) return 1;
	return 0; // All colors are black
}

static void handler_colormap(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	i64 num_entries;
	i64 i;
	u8 is_8bit_pal;
	struct page_ctx *pg = tg->pg;

	if(tg->pg->have_colormap || tg->datatype!=3) return;
	tg->pg->have_colormap = 1;
	num_entries = tg->valcount / 3;
	de_dbg(c, "ColorMap with %d entries", (int)num_entries);

	is_8bit_pal = check_for_8bit_pal(c, d, tg, num_entries);
	if(is_8bit_pal) {
		detiff_warn(c, d, pg, "Likely bad colormap detected. Trying to correct.");
	}

	for(i=0; i<num_entries; i++) {
		i64 r1, g1, b1;
		de_colorsample r2, g2, b2;
		de_color clr;
		char tmps[80];

		read_tag_value_as_int64(c, d, tg, num_entries*0 + i, &r1);
		read_tag_value_as_int64(c, d, tg, num_entries*1 + i, &g1);
		read_tag_value_as_int64(c, d, tg, num_entries*2 + i, &b1);
		if(is_8bit_pal) {
			r2 = (de_colorsample)r1;
			g2 = (de_colorsample)g1;
			b2 = (de_colorsample)b1;
		}
		else {
			r2 = (de_colorsample)(r1>>8);
			g2 = (de_colorsample)(g1>>8);
			b2 = (de_colorsample)(b1>>8);
		}
		clr = DE_MAKE_RGB(r2, g2, b2);
		if(c->debug_level>=2) {
			de_snprintf(tmps, sizeof(tmps), "(%5d,%5d,%5d) "DE_CHAR_RIGHTARROW" ",
				(int)r1, (int)g1, (int)b1);
			de_dbg_pal_entry2(c, i, clr, tmps, NULL, NULL);
		}
		if(i<256) {
			pg->pal[i] = clr;
		}
	}
}

static void handler_subifd(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	i64 j;
	i64 tmpoffset;
	u8 ifdtype = IFDTYPE_NORMAL;

	if(d->fmt==DE_TIFFFMT_NIKONMN && tg->tagnum==0x11) ifdtype = IFDTYPE_NIKONPREVIEW;
	else if(tg->tagnum==330) ifdtype = IFDTYPE_SUBIFD;
	else if(tg->tagnum==400) ifdtype = IFDTYPE_GLOBALPARAMS;
	else if(tg->tagnum==34665) ifdtype = IFDTYPE_EXIF;
	else if(tg->tagnum==34731) ifdtype = IFDTYPE_MASKSUBIFD;
	else if(tg->tagnum==34853) ifdtype = IFDTYPE_GPS;
	else if(tg->tagnum==40965) ifdtype = IFDTYPE_EXIFINTEROP;

	for(j=0; j<tg->valcount;j++) {
		read_tag_value_as_int64(c, d, tg, j, &tmpoffset);
		de_dbg(c, "offset of %s: %d", tni->tagname, (int)tmpoffset);
		push_ifd(c, d, tmpoffset, ifdtype, 0);
	}
}

static void handler_xmp(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	dbuf_create_file_from_slice(c->infile, tg->val_offset, tg->total_size, "xmp", NULL, DE_CREATEFLAG_IS_AUX);
}

static void handler_iptc(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	fmtutil_handle_iptc(c, c->infile, tg->val_offset, tg->total_size,
		d->is_deark_iptc?0x2:0x0);
}

static void handler_photoshoprsrc(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	de_dbg(c, "Photoshop resources at %d, len=%d",
		(int)tg->val_offset, (int)tg->total_size);
	de_dbg_indent(c, 1);
	fmtutil_handle_photoshop_rsrc(c, c->infile, tg->val_offset, tg->total_size,
		d->is_deark_8bim?0x2:0x0);
	de_dbg_indent(c, -1);
}

enum makernote_type {
	MAKERNOTE_UNKNOWN = 0,
	MAKERNOTE_NIKON,
	MAKERNOTE_APPLE_IOS,
	MAKERNOTE_FUJIFILM
};

struct makernote_id_info {
	enum makernote_type mntype;
	char name[32];
};

static void identify_makernote(deark *c, lctx *d, const struct taginfo *tg, struct makernote_id_info *mni)
{
	u8 buf[32];
	i64 amt_to_read;

	de_zeromem(buf, sizeof(buf));
	amt_to_read = sizeof(buf);
	if(amt_to_read > tg->total_size) amt_to_read = tg->total_size;
	de_read(buf, tg->val_offset, amt_to_read);

	if(!de_memcmp(buf, "Nikon\x00\x02", 7) &&
		(!de_memcmp(&buf[10], "\x4d\x4d\x00\x2a", 4) ||
		!de_memcmp(&buf[10], "\x49\x49\x2a\x00", 4)))
	{
		// This is one Nikon MakerNote format. There are others.
		mni->mntype = MAKERNOTE_NIKON;
		de_strlcpy(mni->name, "Nikon type 3", sizeof(mni->name));
		goto done;
	}
	else if(!de_memcmp(buf, "Apple iOS\x00\x00\x01\x4d\x4d", 14)) {
		mni->mntype = MAKERNOTE_APPLE_IOS;
		de_strlcpy(mni->name, "Apple iOS", sizeof(mni->name));
		goto done;
	}
	else if(!de_memcmp(buf, "FUJIFILM", 8)) {
		mni->mntype = MAKERNOTE_FUJIFILM;
		de_strlcpy(mni->name, "FujiFilm", sizeof(mni->name));
		goto done;
	}

done:
	;
}

static void do_makernote_nikon(deark *c, lctx *d, i64 pos1, i64 len)
{
	i64 dpos;
	i64 dlen;
	unsigned int ver;

	if(len<10) return;
	ver = (unsigned int)de_getu16be(pos1+6);
	de_dbg(c, "version: 0x%04x", ver); // This is a guess

	dpos = pos1+10;
	dlen = len-10;
	if(dlen<8) return;
	de_dbg(c, "Nikon MakerNote tag data at %d, len=%d", (int)dpos, (int)dlen);
	de_dbg_indent(c, 1);
	de_run_module_by_id_on_slice2(c, "tiff", "N", c->infile, dpos, dlen);
	de_dbg_indent(c, -1);
}

static void do_makernote_apple_ios(deark *c, lctx *d, i64 pos1, i64 len)
{
	unsigned int ver;

	if(len<12) return;
	ver = (unsigned int)de_getu16be(pos1+10);
	de_dbg(c, "version: 0x%04x", ver); // This is a guess
	if(ver!=1) return;
	if(len<20) return;

	// Apple iOS offsets are relative to the beginning of the "Apple iOS"
	// signature, so that's the data we'll pass to the submodule.
	de_dbg(c, "Apple MakerNote tag data at %"I64_FMT", len=%"I64_FMT, pos1, len);
	de_dbg_indent(c, 1);
	de_run_module_by_id_on_slice2(c, "tiff", "A", c->infile, pos1, len);
	de_dbg_indent(c, -1);
}

static void do_makernote_fujifilm(deark *c, lctx *d, i64 pos1, i64 len)
{
	if(len<14) return;

	de_dbg(c, "FujiFilm MakerNote tag data at %"I64_FMT", len=%"I64_FMT, pos1, len);
	de_dbg_indent(c, 1);
	de_run_module_by_id_on_slice2(c, "tiff", "F", c->infile, pos1, len);
	de_dbg_indent(c, -1);
}

static void handler_makernote(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	struct makernote_id_info *mni = NULL;

	mni = de_malloc(c, sizeof(struct makernote_id_info));
	identify_makernote(c, d, tg, mni);

	if(mni->mntype != 0) {
		de_dbg(c, "MakerNote identified as: %s", mni->name);
	}

	if(mni->mntype==MAKERNOTE_NIKON) {
		do_makernote_nikon(c, d, tg->val_offset, tg->total_size);
	}
	else if(mni->mntype==MAKERNOTE_APPLE_IOS) {
		do_makernote_apple_ios(c, d, tg->val_offset, tg->total_size);
	}
	else if(mni->mntype==MAKERNOTE_FUJIFILM) {
		do_makernote_fujifilm(c, d, tg->val_offset, tg->total_size);
	}
	else {
		handler_hexdump(c, d, tg, tni);
	}

	de_free(c, mni);
}

static void handler_usercomment(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	static u8 charcode[8];
	de_ucstring *s = NULL;
	de_encoding enc = DE_ENCODING_UNKNOWN;
	i64 bytes_per_char = 1;

	if(tg->datatype != DATATYPE_UNDEF) goto done;
	if(tg->total_size < 8) goto done;

	de_read(charcode, tg->val_offset, 8);

	if(!de_memcmp(charcode, "ASCII\0\0\0", 8)) {
		enc = DE_ENCODING_ASCII;
	}
	else if(!de_memcmp(charcode, "UNICODE\0", 8)) {
		enc = d->is_le ? DE_ENCODING_UTF16LE : DE_ENCODING_UTF16BE;
		bytes_per_char = 2;
	}

	if(enc == DE_ENCODING_UNKNOWN) goto done;

	s = ucstring_create(c);
	dbuf_read_to_ucstring_n(c->infile, tg->val_offset + 8, tg->total_size - 8,
		DE_TIFF_MAX_CHARS_TO_PRINT*bytes_per_char, s, 0, enc);

	// Should we truncate at NUL, or not? The Exif spec says "NULL termination
	// is not necessary", but it doesn't say whether it is *allowed*.
	// In practice, if we don't do this, we sometimes end up printing a lot of
	// garbage.
	ucstring_truncate_at_NUL(s);

	// FIXME: This is not quite right, though it's not important. We really
	// need to read the entire string, not just the first
	// DE_TIFF_MAX_CHARS_TO_PRINT bytes, in order to determine which characters
	// are trailing spaces.
	ucstring_strip_trailing_spaces(s);

	de_dbg(c, "%s: \"%s\"", tni->tagname, ucstring_getpsz(s));

done:
	ucstring_destroy(s);
}

static void handler_olepropset(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	de_dbg(c, "OLE property set storage dump at %"I64_FMT", len=%"I64_FMT,
		tg->val_offset, tg->total_size);
	de_dbg_indent(c, 1);
	de_run_module_by_id_on_slice2(c, "cfb", "T", c->infile, tg->val_offset, tg->total_size);
	de_dbg_indent(c, -1);
}

// Photoshop "ImageSourceData"
static void handler_37724(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	const char *codes;
	static const i64 siglen = 36;
	i64 dpos, dlen;
	int psdver = 0;

	if(tg->total_size<siglen) {
		;
	}
	else if(!dbuf_memcmp(c->infile, tg->val_offset, "Adobe Photoshop Document Data Block\0", (size_t)siglen)) {
		psdver = 1;
	}
	else if(!dbuf_memcmp(c->infile, tg->val_offset, "Adobe Photoshop Document Data V0002\0", (size_t)siglen)) {
		psdver = 2;
	}

	if(psdver==0) {
		detiff_warn(c, d, tg->pg, "Bad or unsupported ImageSourceData tag at %d", (int)tg->val_offset);
		goto done;
	}

	de_dbg(c, "ImageSourceData signature at %d, PSD version=%d", (int)tg->val_offset, psdver);

	dpos = tg->val_offset + siglen;
	dlen = tg->total_size - siglen;
	de_dbg(c, "ImageSourceData blocks at %d, len=%d", (int)dpos, (int)dlen);

	codes = (psdver==2)? "B" : "T";
	de_dbg_indent(c, 1);
	de_run_module_by_id_on_slice2(c, "psd", codes, c->infile, dpos, dlen);
	de_dbg_indent(c, -1);
done:
	;
}

static void handler_iccprofile(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	dbuf_create_file_from_slice(c->infile, tg->val_offset, tg->total_size, "icc", NULL, DE_CREATEFLAG_IS_AUX);
}

static void handler_exifversion(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	// The only purpose of this handler is to possibly set d->exif_version_as_uint32,
	// for later use.
	if(tg->valcount!=4) return;
	if(tg->datatype!=DATATYPE_UNDEF) return;
	d->exif_version_as_uint32 = (u32)de_getu32be(tg->val_offset);
}

struct mpfctx_struct {
	int warned;
	// per image:
	int is_thumb;
	i64 imgoffs_abs;
	i64 imgsize;
};

static void try_to_extract_mpf_image(deark *c, lctx *d, struct page_ctx *pg,
	struct mpfctx_struct *mpfctx)
{
	dbuf *inf;

	de_dbg2(c, "[trying to extract image at %d, size=%d]", (int)mpfctx->imgoffs_abs,
		(int)mpfctx->imgsize);
	if(!d->in_params) goto done;
	if(!(d->in_params->flags&0x01)) goto done;
	if(!d->in_params->parent_dbuf) goto done;
	if(mpfctx->imgoffs_abs<1) goto done;
	inf = d->in_params->parent_dbuf;

	if(mpfctx->imgoffs_abs + mpfctx->imgsize > inf->len) {
		if(mpfctx->warned) goto done;
		mpfctx->warned = 1;
		detiff_warn(c, d, pg, "Invalid MPF multi-picture data. File size should be at "
			"least %"I64_FMT", is %"I64_FMT".",
			mpfctx->imgoffs_abs+mpfctx->imgsize, inf->len);
		goto done;
	}

	if(dbuf_memcmp(inf, mpfctx->imgoffs_abs, "\xff\xd8\xff", 3)) {
		detiff_warn(c, d, pg, "Invalid or unsupported MPF multi-picture data. Expected image at "
			"%"I64_FMT" not found.", mpfctx->imgoffs_abs);
		goto done;
	}

	dbuf_create_file_from_slice(inf, mpfctx->imgoffs_abs, mpfctx->imgsize,
		mpfctx->is_thumb?"mpfthumb.jpg":"mpf.jpg",
		NULL, DE_CREATEFLAG_IS_AUX);

done:
	;
}

static void handler_mpentry(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	i64 num_entries;
	i64 k;
	i64 pos = tg->val_offset;
	de_ucstring *s = NULL;
	struct mpfctx_struct mpfctx;

	de_zeromem(&mpfctx, sizeof(struct mpfctx_struct));
	d->mpf_main_image_count = 0;
	// Length is supposed to be 16x{NumberOfImages; tag 45057}. We'll just assume
	// it's correct.
	num_entries = tg->total_size/16;

	s = ucstring_create(c);
	for(k=0; k<num_entries; k++) {
		i64 n;
		i64 imgoffs_rel, imgoffs_abs;
		i64 imgsize;
		u32 attrs;
		u32 dataformat;
		u32 typecode;
		char offset_descr[80];

		de_dbg(c, "entry #%d", (int)(k+1));
		de_dbg_indent(c, 1);

		attrs = (u32)dbuf_getu32x(c->infile, pos, d->is_le);
		dataformat = (attrs&0x07000000)>>24;
		typecode = attrs&0x00ffffff;
		ucstring_empty(s);
		if(attrs&0x80000000U) ucstring_append_flags_item(s, "dependent parent");
		if(attrs&0x40000000U) ucstring_append_flags_item(s, "dependent child");
		if(attrs&0x20000000U) ucstring_append_flags_item(s, "representative image");
		if(dataformat==0) ucstring_append_flags_item(s, "JPEG");
		if(typecode==0x030000U) ucstring_append_flags_item(s, "baseline MP primary image");
		if(typecode==0x010001U) ucstring_append_flags_item(s, "large thumbnail class 1");
		if(typecode==0x010002U) ucstring_append_flags_item(s, "large thumbnail class 2");
		if(typecode==0x020001U) ucstring_append_flags_item(s, "multi-frame image panorama");
		if(typecode==0x020002U) ucstring_append_flags_item(s, "multi-frame image disparity");
		if(typecode==0x020003U) ucstring_append_flags_item(s, "multi-frame image multi-angle");

		if(typecode==0x010001U || typecode==0x010002U) {
			mpfctx.is_thumb = 1;
		}
		else {
			// Count that number of non-thumbnail images
			d->mpf_main_image_count++;
			mpfctx.is_thumb = 0;
		}

		de_dbg(c, "image attribs: 0x%08x (%s)", (unsigned int)attrs,
			ucstring_getpsz(s));

		imgsize = dbuf_getu32x(c->infile, pos+4, d->is_le);
		de_dbg(c, "image size: %u", (unsigned int)imgsize);

		imgoffs_rel = dbuf_getu32x(c->infile, pos+8, d->is_le);
		// This is relative to beginning of the payload data (the TIFF header)
		// of the MPF segment, except that 0 is a special case.
		if(imgoffs_rel==0) {
			imgoffs_abs = 0;
			de_strlcpy(offset_descr, "refers to the first image", sizeof(offset_descr));
		}
		else if(d->in_params && (d->in_params->flags&0x01)) {
			imgoffs_abs = d->in_params->offset_in_parent+imgoffs_rel;
			de_snprintf(offset_descr, sizeof(offset_descr), "absolute offset %"I64_FMT,
				imgoffs_abs);
		}
		else {
			imgoffs_abs = imgoffs_rel;
			de_strlcpy(offset_descr, "?", sizeof(offset_descr));
		}
		de_dbg(c, "image offset: %u (%s)", (unsigned int)imgoffs_rel, offset_descr);

		if(imgoffs_rel>0) {
			mpfctx.imgoffs_abs = imgoffs_abs;
			mpfctx.imgsize = imgsize;
			try_to_extract_mpf_image(c, d, tg->pg, &mpfctx);
		}

		n = dbuf_getu16x(c->infile, pos+12, d->is_le);
		de_dbg(c, "dep. image #1 entry: %u", (unsigned int)n);
		n = dbuf_getu16x(c->infile, pos+14, d->is_le);
		de_dbg(c, "dep. image #2 entry: %u", (unsigned int)n);
		de_dbg_indent(c, -1);
		pos += 16;
	}
	ucstring_destroy(s);
}

static void handler_gpslatitude(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	// We look for this tag instead of GPS IFD or GPSVersionID, because we want
	// to know whether the file contains actual GPS coordinates. A lot of files
	// have a GPS IFD that contains nothing.
	d->has_exif_gps = 1;
}

// This is for certain Microsoft tags that are apparently in UTF-16 format.
// They use the BYTE data type (instead of the logical SHORT), and are always
// little-endian, even in big-endian files.
// They end with two 0 bytes. I don't know whether multiple strings can be stored
// in one field (as in TIFF ASCII tags), but Microsoft appears to use semicolons
// to separate multiple items, instead of U+0000 codes.
static void handler_utf16(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	de_ucstring *s = NULL;

	if(tg->datatype!=DATATYPE_BYTE && tg->datatype!=DATATYPE_UNDEF) goto done;
	if(tg->total_size % 2) goto done; // Something's wrong if the byte count is odd.

	s = ucstring_create(c);
	dbuf_read_to_ucstring_n(c->infile, tg->val_offset, tg->total_size,
		DE_TIFF_MAX_CHARS_TO_PRINT*2, s, 0, DE_ENCODING_UTF16LE);
	ucstring_truncate_at_NUL(s);
	de_dbg(c, "UTF-16 string: \"%s\"", ucstring_getpsz(s));

done:
	ucstring_destroy(s);
	return;
}

static void handler_dngprivatedata(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	struct de_stringreaderdata *srd;
	i64 nbytes_to_scan;

	nbytes_to_scan = tg->total_size;
	if(nbytes_to_scan>128) nbytes_to_scan=128;

	srd = dbuf_read_string(c->infile, tg->val_offset, nbytes_to_scan, nbytes_to_scan,
		DE_CONVFLAG_STOP_AT_NUL, DE_ENCODING_ASCII);
	if(srd->found_nul) {
		de_dbg(c, "identifier: \"%s\"", ucstring_getpsz(srd->str));
	}
	de_destroy_stringreaderdata(c, srd);
}

static void handler_panasonicjpg(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	dbuf_create_file_from_slice(c->infile, tg->val_offset, tg->total_size,
		"thumb.jpg", NULL, DE_CREATEFLAG_IS_AUX);
}

static void handler_leafdata(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	do_leaf_metadata(c, d, tg->val_offset, tg->total_size);
}

static int alloc_striledata(deark *c, struct page_ctx *pg, i64 nstriles)
{
	if(nstriles<1) nstriles = 1;
	if(pg->strile_count >= nstriles) return 1;
	if(nstriles>DE_TIFF_MAX_STRILES) return 0;
	pg->strile_data = de_reallocarray(c, pg->strile_data, pg->strile_count, sizeof(struct strile_data_struct), nstriles);
	pg->strile_count = nstriles;
	return 1;
}

static void handler_strileoffsets(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	struct page_ctx *pg = tg->pg;
	i64 i;

	if(tni->tagnum==TAG_TILEOFFSETS) {
		pg->have_tile_tags = 1;
	}
	else {
		pg->have_strip_tags = 1;
	}
	if(!alloc_striledata(c, pg, tg->valcount)) return;

	for(i=0; i<tg->valcount; i++) {
		read_tag_value_as_int64(c, d, tg, i, &pg->strile_data[i].pos);
	}
}

static void handler_strilebytecounts(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	struct page_ctx *pg = tg->pg;
	i64 i;

	pg->have_strilebytecounts = 1;
	if(tni->tagnum==TAG_TILEBYTECOUNTS) {
		pg->have_tile_tags = 1;
	}
	else {
		pg->have_strip_tags = 1;
	}
	if(!alloc_striledata(c, pg, tg->valcount)) return;

	for(i=0; i<tg->valcount; i++) {
		read_tag_value_as_int64(c, d, tg, i, &pg->strile_data[i].len);
	}
}

static void handler_bitspersample(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	i64 val;

	tg->pg->valcount_bitspersample = (u8)tg->valcount;
	if(tg->valcount<1) return;

	// FIXME: This is a multi-valued field.
	read_tag_value_as_int64(c, d, tg, 0, &val);
	tg->pg->bits_per_sample = (u32)val;
}

static void handler_extrasamples(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	struct page_ctx *pg = tg->pg;
	u8 i;

	pg->extrasamples_count = (u8)tg->valcount;
	if(pg->extrasamples_count>DE_TIFF_MAX_SAMPLES) {
		pg->extrasamples_count = DE_TIFF_MAX_SAMPLES;
	}

	for(i=0; i<pg->extrasamples_count; i++) {
		i64 val;

		read_tag_value_as_int64(c, d, tg, 0, &val);
		pg->extrasample_type[(size_t)i] = (u8)val;
	}
}

static void handler_matteing(deark *c, lctx *d, const struct taginfo *tg,
	const struct tagnuminfo *tni)
{
	i64 val;

	if(tg->valcount<1) return;
	read_tag_value_as_int64(c, d, tg, 0, &val);
	if(val==1) {
		tg->pg->extrasamples_count = 1;
		tg->pg->extrasample_type[0] = 1; // = Assoc. alpha
	}
}

static void handler_sampleformat(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	i64 val;

	if(tg->valcount<1) return;

	// FIXME: This is a multi-valued field.
	read_tag_value_as_int64(c, d, tg, 0, &val);
	tg->pg->sample_format = (u32)val;
}

static void handler_resolution(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	struct numeric_value nv;

	if(tg->valcount<1) return;
	read_numeric_value(c, d, tg, 0, &nv, NULL);
	if(!nv.isvalid) return;
	tg->pg->have_density = 1;
	if(tg->tagnum==TAG_XRESOLUTION) {
		tg->pg->density_ImageWidth = nv.val_double;
	}
	else if(tg->tagnum==TAG_YRESOLUTION) {
		tg->pg->density_ImageLength = nv.val_double;
	}
}

static i64 read_fixed_decimal(dbuf *f, i64 pos, size_t ndigits)
{
	i64 n;
	char buf[16];

	if(ndigits >= sizeof(buf)) return 0;
	dbuf_read(f, (u8*)buf, pos, ndigits);
	buf[ndigits] = '\0';
	n = de_atoi64(buf);
	return n;
}

static void parse_tiff_datetime(deark *c, lctx *d, const struct taginfo *tg, struct de_timestamp *ts)
{
	i64 yr, mo, da;
	i64 hr, mi, se;

	if(tg->unit_size!=1) return;
	if(tg->valcount<19) return;
	// Format: "YYYY:MM:DD HH:MM:SS"
	yr = read_fixed_decimal(c->infile, tg->val_offset, 4);
	mo = read_fixed_decimal(c->infile, tg->val_offset+5, 2);
	da = read_fixed_decimal(c->infile, tg->val_offset+8, 2);
	hr = read_fixed_decimal(c->infile, tg->val_offset+11, 2);
	mi = read_fixed_decimal(c->infile, tg->val_offset+14, 2);
	se = read_fixed_decimal(c->infile, tg->val_offset+17, 2);
	de_make_timestamp(ts, yr, mo, da, hr, mi, se);
	if(ts->is_valid) {
		ts->tzcode = DE_TZCODE_UNKNOWN;
		ts->precision = DE_TSPREC_1SEC;
	}
}

static void handler_datetime(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	 parse_tiff_datetime(c, d, tg, &tg->pg->internal_mod_time);
}

static void handler_jxr_pixelformat(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	u64 n1, n2;

	if(tg->total_size != 16) return;
	n1 = dbuf_getu64be(c->infile, tg->val_offset);
	n2 = dbuf_getu64be(c->infile, tg->val_offset+8);
	de_dbg(c, "id: 0x%016"U64_FMTx"%016"U64_FMTx, n1, n2);
}

static void handler_jxr_imageoffset(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	tg->pg->have_jxr_imageoffset = 1;
}

// Handler for some tags expected to have a single integer value
static void handler_various(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni)
{
	struct page_ctx *pg = tg->pg;
	i64 val;

	if(tg->valcount<1) return;
	read_tag_value_as_int64(c, d, tg, 0, &val);

	switch(tg->tagnum) {
	case TAG_NEWSUBFILETYPE:
		if(val & 0x1) pg->is_thumb = 1;
		break;
	case TAG_OLDSUBFILETYPE:
		pg->have_oldsubfiletype = 1;
		if(val==2) pg->is_thumb = 1;
		break;
	case TAG_IMAGEWIDTH:
		pg->have_imagewidth = 1;
		pg->imagewidth = val;
		break;
	case TAG_IMAGELENGTH:
		pg->imagelength = val;
		break;
	case TAG_COMPRESSION:
		pg->compression = (u32)val;
		break;
	case TAG_PHOTOMETRIC:
		pg->photometric = (u32)val;
		break;
	case TAG_FILLORDER:
		pg->fill_order = (u32)val;
		break;
	case TAG_SAMPLESPERPIXEL:
		pg->samples_per_pixel = (u32)val;
		break;
	case TAG_ROWSPERSTRIP:
		pg->have_strip_tags = 1;
		pg->rows_per_strip = val;
		break;
	case TAG_TILEWIDTH:
		pg->have_tile_tags = 1;
		pg->tile_width = val;
		break;
	case TAG_TILELENGTH:
		pg->have_tile_tags = 1;
		pg->tile_height = val;
		break;
	case TAG_ORIENTATION:
		if(val>=1 && val<=8) {
			pg->orientation = (u32)val;
		}
		break;
	case TAG_PLANARCONFIG:
		pg->planarconfig = (u32)val;
		break;
	case TAG_T4OPTIONS:
		pg->t4options = (u32)val;
		break;
	case TAG_T6OPTIONS:
		pg->t6options = (u32)val;
		break;
	case TAG_RESOLUTIONUNIT:
		pg->resolution_unit = (u32)val;
		break;
	case TAG_PREDICTOR:
		pg->predictor = (u32)val;
		break;
	case TAG_JPEGINTERCHANGEFORMAT:
		pg->jpegoffset = val;
		break;
	case TAG_JPEGINTERCHANGEFORMATLENGTH:
		pg->have_jpeglength = 1;
		pg->jpeglength = val;
		break;
	case TAG_YCBCRPOSITIONING:
		pg->ycbcrpositioning = (u32)val;
		break;
	case TAG_JPEGTABLES:
		pg->have_jpegtables = 1;
		break;
	}
}

static const struct tagnuminfo tagnuminfo_arr[] = {
	{ /* 254 */ TAG_NEWSUBFILETYPE, 0x00, "NewSubfileType", handler_various, valdec_newsubfiletype },
	{ /* 255 */ TAG_OLDSUBFILETYPE, 0x00, "OldSubfileType", handler_various, valdec_oldsubfiletype },
	{ /* 256 */ TAG_IMAGEWIDTH, 0x0000, "ImageWidth", handler_various, NULL },
	{ /* 257 */ TAG_IMAGELENGTH, 0x0000, "ImageLength", handler_various, NULL },
	{ /* 258 */ TAG_BITSPERSAMPLE, 0x00, "BitsPerSample", handler_bitspersample, NULL },
	{ /* 259 */ TAG_COMPRESSION, 0x0000, "Compression", handler_various, valdec_compression },
	{ /* 262 */ TAG_PHOTOMETRIC, 0x00, "PhotometricInterpretation", handler_various, valdec_photometric },
	{ 263, 0x00, "Threshholding", NULL, valdec_threshholding },
	{ 264, 0x00, "CellWidth", NULL, NULL },
	{ 265, 0x00, "CellLength", NULL, NULL },
	{ /* 266 */ TAG_FILLORDER, 0x00, "FillOrder", handler_various, valdec_fillorder },
	{ 269, 0x0400, "DocumentName", NULL, NULL },
	{ 270, 0x0400, "ImageDescription", NULL, NULL },
	{ 271, 0x0400, "Make", NULL, NULL },
	{ 272, 0x0400, "Model", NULL, NULL },
	{ /* 273 */ TAG_STRIPOFFSETS, 0x00, "StripOffsets", handler_strileoffsets, NULL },
	{ /* 274 */ TAG_ORIENTATION, 0x0000, "Orientation", handler_various, valdec_orientation },
	{ /* 277 */ TAG_SAMPLESPERPIXEL, 0x00, "SamplesPerPixel", handler_various, NULL },
	{ /* 278 */ TAG_ROWSPERSTRIP, 0x00, "RowsPerStrip", handler_various, NULL },
	{ /* 279 */ TAG_STRIPBYTECOUNTS, 0x00, "StripByteCounts", handler_strilebytecounts, NULL },
	{ 280, 0x00, "MinSampleValue", NULL, NULL },
	{ 281, 0x00, "MaxSampleValue", NULL, NULL },
	{ /* 282 */ TAG_XRESOLUTION, 0x00, "XResolution", handler_resolution, NULL },
	{ /* 283 */ TAG_YRESOLUTION, 0x00, "YResolution", handler_resolution, NULL },
	{ /* 284 */ TAG_PLANARCONFIG, 0x00, "PlanarConfiguration", handler_various, valdec_planarconfiguration },
	{ 285, 0x0400, "PageName", NULL, NULL },
	{ 286, 0x00, "XPosition", NULL, NULL },
	{ 287, 0x00, "YPosition", NULL, NULL },
	{ 288, 0x00, "FreeOffsets", NULL, NULL },
	{ 289, 0x00, "FreeByteCounts", NULL, NULL },
	{ 290, 0x00, "GrayResponseUnit", NULL, NULL },
	{ 291, 0x00, "GrayResponseCurve", NULL, NULL },
	{ /* 292 */ TAG_T4OPTIONS, 0x00, "T4Options", handler_various, valdec_t4options },
	{ /* 293 */ TAG_T6OPTIONS, 0x00, "T6Options", handler_various, valdec_t6options },
	{ /* 296 */ TAG_RESOLUTIONUNIT, 0x00, "ResolutionUnit", handler_various, valdec_resolutionunit },
	{ 297, 0x0400, "PageNumber", NULL, valdec_pagenumber },
	{ 300, 0x0000, "ColorResponseUnit", NULL, NULL },
	{ 301, 0x00, "TransferFunction", NULL, NULL },
	{ 305, 0x0400, "Software", NULL, NULL },
	{ /* 306 */ TAG_DATETIME, 0x0400, "DateTime", handler_datetime, NULL },
	{ 315, 0x0400, "Artist", NULL, NULL },
	{ 316, 0x0400, "HostComputer", NULL, NULL },
	{ /* 317 */ TAG_PREDICTOR, 0x00, "Predictor", handler_various, valdec_predictor },
	{ 318, 0x00, "WhitePoint", NULL, NULL },
	{ 319, 0x00, "PrimaryChromaticities", NULL, NULL },
	{ 320, 0x08, "ColorMap", handler_colormap, NULL },
	{ 321, 0x00, "HalftoneHints", NULL, NULL },
	{ /* 322 */ TAG_TILEWIDTH, 0x00, "TileWidth", handler_various, NULL },
	{ /* 323 */ TAG_TILELENGTH, 0x00, "TileLength", handler_various, NULL },
	{ /* 324 */ TAG_TILEOFFSETS, 0x00, "TileOffsets", handler_strileoffsets, NULL },
	{ /* 325 */ TAG_TILEBYTECOUNTS, 0x00, "TileByteCounts", handler_strilebytecounts, NULL },
	{ 326, 0x00, "BadFaxLines", NULL, NULL },
	{ 327, 0x00, "CleanFaxData", NULL, NULL },
	{ 328, 0x00, "ConsecutiveBadFaxLines", NULL, NULL },
	{ 330, 0x08, "SubIFD", handler_subifd, NULL },
	{ 332, 0x0000, "InkSet", NULL, valdec_inkset },
	{ 333, 0x0004, "InkNames", NULL, NULL },
	{ 334, 0x00, "NumberOfInks", NULL, NULL },
	{ 336, 0x00, "DotRange", NULL, NULL },
	{ 337, 0x00, "TargetPrinter", NULL, NULL },
	{ 338, 0x00, "ExtraSamples", handler_extrasamples, valdec_extrasamples },
	{ /* 339 */ TAG_SAMPLEFORMAT, 0x00, "SampleFormat", handler_sampleformat, valdec_sampleformat },
	{ 340, 0x00, "SMinSampleValue", NULL, NULL },
	{ 341, 0x00, "SMaxSampleValue", NULL, NULL },
	{ 342, 0x00, "TransferRange", NULL, NULL },
	{ 343, 0x0000, "ClipPath", NULL, NULL },
	{ 344, 0x0000, "XClipPathUnits", NULL, NULL },
	{ 345, 0x0000, "YClipPathUnits", NULL, NULL },
	{ 346, 0x0000, "Indexed", NULL, NULL },
	{ /* 347 */ TAG_JPEGTABLES, 0x00, "JPEGTables", handler_various, NULL },
	{ 351, 0x0000, "OPIProxy", NULL, NULL },
	{ 400, 0x0008, "GlobalParametersIFD", handler_subifd, NULL },
	{ 401, 0x0000, "ProfileType", NULL, NULL },
	{ 402, 0x0000, "FaxProfile", NULL, NULL },
	{ 403, 0x0000, "CodingMethods", NULL, NULL },
	{ 404, 0x0000, "VersionYear", NULL, NULL },
	{ 405, 0x0000, "ModeNumber", NULL, NULL },
	{ 433, 0x0000, "Decode", NULL, NULL },
	{ 434, 0x0000, "DefaultImageColor", NULL, NULL },
	{ 435, 0x0000, "T82Options", NULL, NULL },
	{ 512, 0x00, "JPEGProc", NULL, valdec_jpegproc },
	{ /* 513 */ TAG_JPEGINTERCHANGEFORMAT, 0x0000, "JPEGInterchangeFormat", handler_various, NULL },
	{ /* 514 */ TAG_JPEGINTERCHANGEFORMATLENGTH, 0x0000, "JPEGInterchangeFormatLength", handler_various, NULL },
	{ 515, 0x00, "JPEGRestartInterval", NULL, NULL },
	{ 517, 0x00, "JPEGLosslessPredictors", NULL, NULL },
	{ 518, 0x00, "JPEGPointTransforms", NULL, NULL },
	{ 519, 0x00, "JPEGQTables", NULL, NULL },
	{ 520, 0x00, "JPEGDCTables", NULL, NULL },
	{ 521, 0x00, "JPEGACTables", NULL, NULL },
	{ 529, 0x00, "YCbCrCoefficients", NULL, NULL },
	{ 530, 0x00, "YCbCrSubSampling", NULL, NULL },
	{ /* 531 */ TAG_YCBCRPOSITIONING, 0x0000, "YCbCrPositioning", handler_various, valdec_ycbcrpositioning },
	{ 532, 0x00, "ReferenceBlackWhite", NULL, NULL },
	{ 559, 0x0000, "StripRowCounts", NULL, NULL },
	{ 700, 0x0408, "XMP", handler_xmp, NULL },
	{ 769, 0x0010, "PropertyTagGamma", NULL, NULL },
	{ 770, 0x0010, "PropertyTagICCProfileDescriptor", NULL, NULL },
	{ 771, 0x0010, "PropertyTagSRGBRenderingIntent", NULL, NULL },
	//{ 999, 0x0000, "USPTOMiscellaneous", NULL, NULL },
	{ 18246, 0x0400, "RatingStars", NULL, NULL },
	{ 18247, 0x0000, "XP_DIP_XML", NULL, NULL },
	{ 18248, 0x0000, "StitchInfo", NULL, NULL },
	{ 18249, 0x0400, "RatingValue", NULL, NULL },
	{ 20752, 0x0010, "PropertyTagPixelUnit", NULL, NULL },
	{ 20753, 0x0010, "PropertyTagPixelPerUnitX", NULL, NULL },
	{ 20754, 0x0010, "PropertyTagPixelPerUnitY", NULL, NULL },
	//{ 28672, 0x0000, "SonyRawFileType", NULL, NULL },
	//{ 28725, 0x0000, "ChromaticAberrationCorrParams", NULL, NULL },
	//{ 28727, 0x0000, "DistortionCorrParams", NULL, NULL },
	{ 32781, 0x0000, "ImageID", NULL, NULL },
	{ 32932, 0x0000, "Wang Annotation", NULL, NULL },
	{ 32934, 0x0000, "Wang PageControl", NULL, NULL },
	{ 32953, 0x0000, "ImageReferencePoints", NULL, NULL },
	{ 32954, 0x0000, "RegionXformTackPoint", NULL, NULL },
	{ 32955, 0x0000, "RegionWarpCorners", NULL, NULL },
	{ 32956, 0x0000, "RegionAffine", NULL, NULL },
	{ 32995, 0x00, "Matteing(SGI)", handler_matteing, NULL },
	{ 32996, 0x00, "DataType(SGI)", NULL, NULL },
	{ 32997, 0x00, "ImageDepth(SGI)", NULL, NULL },
	{ 32998, 0x00, "TileDepth(SGI)", NULL, NULL },
	{ 33300, 0x0000, "Pixar ImageFullWidth", NULL, NULL },
	{ 33301, 0x0000, "Pixar ImageFullLength", NULL, NULL },
	{ 33302, 0x0000, "Pixar TextureFormat", NULL, NULL },
	{ 33303, 0x0000, "Pixar WrapModes", NULL, NULL },
	{ 33304, 0x0000, "Pixar FOVCOT", NULL, NULL },
	{ 33305, 0x0000, "Pixar MatrixWorldToScreen", NULL, NULL },
	{ 33306, 0x0000, "Pixar MatrixWorldToCamera", NULL, NULL },
	{ 33405, 0x0000, "Model2", NULL, NULL },
	{ 33421, 0x0100, "CFARepeatPatternDim", NULL, NULL },
	{ 33422, 0x0100, "CFAPattern", NULL, NULL },
	{ 33423, 0x0100, "BatteryLevel", NULL, NULL },
	//{ 33424, 0x0000, "KodakIFD", NULL, NULL },
	{ 33432, 0x0404, "Copyright", NULL, NULL },
	{ 33434, 0x10, "ExposureTime", NULL, NULL },
	{ 33437, 0x10, "FNumber", NULL, NULL },
	{ 33445, 0x0000, "MD FileTag", NULL, NULL },
	{ 33446, 0x0000, "MD ScalePixel", NULL, NULL },
	{ 33447, 0x0000, "MD ColorTable", NULL, NULL },
	{ 33448, 0x0000, "MD LabName", NULL, NULL },
	{ 33449, 0x0000, "MD SampleInfo", NULL, NULL },
	{ 33450, 0x0000, "MD PrepDate", NULL, NULL },
	{ 33451, 0x0000, "MD PrepTime", NULL, NULL },
	{ 33452, 0x0000, "MD FileUnits", NULL, NULL },
	{ 33550, 0x0000, "ModelPixelScaleTag", NULL, NULL },
	{ 33589, 0x0000, "AdventScale", NULL, NULL },
	{ 33590, 0x0000, "AdventRevision", NULL, NULL },
	// 33628-33631: UICTags
	{ 33723, 0x0408, "IPTC", handler_iptc, NULL },
	{ 33918, 0x0000, "INGR Packet Data", NULL, NULL },
	{ 33919, 0x0000, "INGR Flag Registers", NULL, NULL },
	{ 33920, 0x0000, "IrasB Transformation Matrix", NULL, NULL },
	{ 33922, 0x0000, "ModelTiepointTag", NULL, NULL },
	{ 34016, 0x0200, "Site", NULL, NULL },
	{ 34017, 0x0200, "ColorSequence", NULL, NULL },
	{ 34018, 0x0200, "IT8Header", NULL, NULL },
	{ 34019, 0x0200, "RasterPadding", NULL, NULL },
	{ 34020, 0x0200, "BitsPerRunLength", NULL, NULL },
	{ 34021, 0x0200, "BitsPerExtendedRunLength", NULL, NULL },
	{ 34022, 0x0200, "ColorTable", NULL, NULL },
	{ 34023, 0x0200, "ImageColorIndicator", NULL, NULL },
	{ 34024, 0x0200, "BackgroundColorIndicator", NULL, NULL },
	{ 34025, 0x0200, "ImageColorValue", NULL, NULL },
	{ 34026, 0x0200, "BackgroundColorValue", NULL, NULL },
	{ 34027, 0x0200, "PixelIntensityRange", NULL, NULL },
	{ 34028, 0x0200, "TransparencyIndicator", NULL, NULL },
	{ 34029, 0x0200, "ColorCharacterization", NULL, NULL },
	{ 34030, 0x0200, "HCUsage", NULL, NULL },
	{ 34031, 0x0200, "TrapIndicator", NULL, NULL },
	{ 34032, 0x0200, "CMYKEquivalent", NULL, NULL },
	{ 34118, 0x0000, "SEMInfo", NULL, NULL },
	{ 34152, 0x0000, "AFCP_IPTC", NULL, NULL },
	// Contradictory info about 34232
	{ 34232, 0x0000, "FrameCount or PixelMagicJBIGOptions", NULL, NULL },
	{ 34263, 0x0000, "JPLCartoIFD", NULL, NULL },
	{ 34264, 0x0000, "ModelTransformationTag", NULL, NULL },
	//{ 34306, 0x0000, "WB_GRGBLevels", NULL, NULL },
	{ 34310, 0x0008, "LeafData", handler_leafdata, NULL },
	{ 34377, 0x0408, "PhotoshopImageResources", handler_photoshoprsrc, NULL },
	{ 34665, 0x0408, "Exif IFD", handler_subifd, NULL },
	{ 34675, 0x0408, "ICC Profile", handler_iccprofile, NULL },
	//{ 34687, 0x0000, "TIFF_FXExtensions", NULL, NULL },
	//{ 34688, 0x0000, "MultiProfiles", NULL, NULL, NULL },
	//{ 34689, 0x0000, "SharedData", NULL, NULL, NULL },
	//{ 34690, 0x0000, "T88Options", NULL, NULL, NULL },
	{ 34730, 0x0000, "Annotation Offsets", NULL, NULL },
	{ 34731, 0x0008, "Mask SubIFDs", handler_subifd, NULL },
	{ 34732, 0x0000, "ImageLayer", NULL, NULL },
	{ 34735, 0x0000, "GeoKeyDirectoryTag", NULL, NULL },
	{ 34736, 0x0000, "GeoDoubleParamsTag", NULL, NULL },
	{ 34737, 0x0000, "GeoAsciiParamsTag", NULL, NULL },
	{ 34750, 0x0000, "JBIGOptions", NULL, NULL },
	{ 34850, 0x10, "ExposureProgram", NULL, valdec_exposureprogram },
	{ 34852, 0x10, "SpectralSensitivity", NULL, NULL },
	{ 34853, 0x0408, "GPS IFD", handler_subifd, NULL },
	{ 34855, 0x10, "PhotographicSensitivity/ISOSpeedRatings", NULL, NULL },
	{ 34856, 0x0018, "OECF", handler_hexdump, NULL },
	{ 34857, 0x0100, "Interlace", NULL, NULL },
	{ 34858, 0x0100, "TimeZoneOffset", NULL, NULL },
	{ 34859, 0x0100, "SelfTimerMode", NULL, NULL },
	{ 34864, 0x10, "SensitivityType", NULL, NULL },
	{ 34865, 0x10, "StandardOutputSensitivity", NULL, NULL },
	{ 34866, 0x10, "RecommendedExposureIndex", NULL, NULL },
	{ 34867, 0x10, "ISOSpeed", NULL, NULL },
	{ 34868, 0x10, "ISOSpeedLatitudeyyy", NULL, NULL },
	{ 34869, 0x10, "ISOSpeedLatitudezzz", NULL, NULL },
	{ 34908, 0x00, "FaxRecvParams", NULL, NULL },
	{ 34909, 0x00, "FaxSubAddress", NULL, NULL },
	{ 34910, 0x0000, "FaxRecvTime", NULL, NULL },
	{ 34911, 0x0000, "FaxDCS", NULL, NULL },
	{ 34929, 0x0000, "FEDEX_EDR", NULL, NULL },
	//{ 34954, 0x0000, "LeafSubIFD", NULL, NULL },
	{ 36864, 0x10, "ExifVersion", handler_exifversion, NULL },
	{ 36867, 0x10, "DateTimeOriginal", NULL, NULL },
	{ 36868, 0x10, "DateTimeDigitized", NULL, NULL },
	{ 36880, 0x0010, "OffsetTime", NULL, NULL },
	{ 36881, 0x0010, "OffsetTimeOriginal", NULL, NULL },
	{ 36882, 0x0010, "OffsetTimeDigitized", NULL, NULL },
	{ 37121, 0x10, "ComponentsConfiguration", NULL, valdec_componentsconfiguration },
	{ 37122, 0x10, "CompressedBitsPerPixel", NULL, NULL },
	{ 37377, 0x10, "ShutterSpeedValue", NULL, NULL },
	{ 37378, 0x10, "ApertureValue", NULL, NULL },
	{ 37379, 0x10, "BrightnessValue", NULL, NULL },
	{ 37380, 0x10, "ExposureBiasValue", NULL, NULL },
	{ 37381, 0x10, "MaxApertureValue", NULL, NULL },
	{ 37382, 0x10, "SubjectDistance", NULL, NULL },
	{ 37383, 0x10, "MeteringMode", NULL, valdec_meteringmode },
	{ 37384, 0x10, "LightSource", NULL, valdec_lightsource },
	{ 37385, 0x10, "Flash", NULL, valdec_flash },
	{ 37386, 0x10, "FocalLength", NULL, NULL },
	{ 37387, 0x0100, "FlashEnergy", NULL, NULL },
	{ 37388, 0x0100, "SpatialFrequencyResponse", NULL, NULL },
	{ 37389, 0x0100, "Noise", NULL, NULL },
	{ 37390, 0x0100, "FocalPlaneXResolution", NULL, NULL },
	{ 37391, 0x0100, "FocalPlaneYResolution", NULL, NULL },
	{ 37392, 0x0100, "FocalPlaneResolutionUnit", NULL, NULL },
	{ 37393, 0x0100, "ImageNumber", NULL, NULL },
	{ 37394, 0x0100, "SecurityClassification", NULL, NULL },
	{ 37395, 0x0100, "ImageHistory", NULL, NULL },
	{ 37396, 0x10, "SubjectArea", NULL, NULL },
	{ 37397, 0x0100, "ExposureIndex", NULL, NULL },
	{ 37398, 0x0100, "TIFF/EPStandardID", NULL, NULL },
	{ 37399, 0x0100, "SensingMethod", NULL, NULL },
	{ 37439, 0x00, "SToNits(SGI)", NULL, NULL },
	{ 37500, 0x0018, "MakerNote", handler_makernote, NULL },
	{ 37510, 0x10, "UserComment", handler_usercomment, NULL },
	{ 37520, 0x10, "SubSec", NULL, NULL },
	{ 37521, 0x10, "SubSecTimeOriginal", NULL, NULL },
	{ 37522, 0x10, "SubsecTimeDigitized", NULL, NULL },
	{ 37679, 0x0000, "OCR Text", NULL, NULL },
	{ 37680, 0x0008, "OLE Property Set Storage", handler_olepropset, NULL },
	{ 37681, 0x0000, "OCR Text Position Info", NULL, NULL },
	{ 37724, 0x0008, "Photoshop ImageSourceData", handler_37724, NULL },
	{ 37888, 0x0010, "Temperature", NULL, NULL },
	{ 37889, 0x0010, "Humidity", NULL, NULL },
	{ 37890, 0x0010, "Pressure", NULL, NULL },
	{ 37891, 0x0010, "WaterDepth", NULL, NULL },
	{ 37892, 0x0010, "Acceleration", NULL, NULL },
	{ 37893, 0x0010, "CameraElevationAngle", NULL, NULL },
	{ 40091, 0x0408, "XPTitle/Caption", handler_utf16, NULL },
	{ 40092, 0x0008, "XPComment", handler_utf16, NULL },
	{ 40093, 0x0008, "XPAuthor", handler_utf16, NULL },
	{ 40094, 0x0008, "XPKeywords", handler_utf16, NULL },
	{ 40095, 0x0008, "XPSubject", handler_utf16, NULL },
	{ 40960, 0x10, "FlashPixVersion", NULL, NULL },
	{ 40961, 0x0410, "ColorSpace", NULL, valdec_exifcolorspace },
	{ 40962, 0x10, "PixelXDimension", NULL, NULL },
	{ 40963, 0x10, "PixelYDimension", NULL, NULL },
	{ 40964, 0x10, "RelatedSoundFile", NULL, NULL },
	{ 40965, 0x0418, "Interoperability IFD", handler_subifd, NULL },
	{ 41483, 0x10, "FlashEnergy", NULL, NULL },
	{ 41484, 0x0018, "SpatialFrequencyResponse", handler_hexdump, NULL },
	{ 41486, 0x10, "FocalPlaneXResolution", NULL, NULL },
	{ 41487, 0x10, "FocalPlaneYResolution", NULL, NULL },
	{ 41488, 0x10, "FocalPlaneResolutionUnit", NULL, valdec_resolutionunit },
	{ 41492, 0x10, "SubjectLocation", NULL, NULL },
	{ 41493, 0x10, "ExposureIndex", NULL, NULL },
	{ 41495, 0x10, "SensingMethod", NULL, valdec_sensingmethod },
	{ 41728, 0x10, "FileSource", NULL, valdec_filesource },
	{ 41729, 0x10, "SceneType", NULL, valdec_scenetype },
	{ 41730, 0x10, "CFAPattern", NULL, NULL },
	{ 41985, 0x10, "CustomRendered", NULL, valdec_customrendered },
	{ 41986, 0x10, "ExposureMode", NULL, valdec_exposuremode },
	{ 41987, 0x10, "WhiteBalance", NULL, valdec_whitebalance },
	{ 41988, 0x10, "DigitalZoomRatio", NULL, NULL },
	{ 41989, 0x10, "FocalLengthIn35mmFilm", NULL, NULL },
	{ 41990, 0x10, "SceneCaptureType", NULL, valdec_scenecapturetype },
	{ 41991, 0x10, "GainControl", NULL, valdec_gaincontrol },
	{ 41992, 0x10, "Contrast", NULL, valdec_contrast },
	{ 41993, 0x10, "Saturation", NULL, valdec_saturation },
	{ 41994, 0x10, "Sharpness", NULL, valdec_sharpness },
	{ 41995, 0x0018, "DeviceSettingDescription", handler_hexdump, NULL },
	{ 41996, 0x10, "SubjectDistanceRange", NULL, valdec_subjectdistancerange },
	{ 42016, 0x10, "ImageUniqueID", NULL, NULL },
	{ 42032, 0x10, "CameraOwnerName", NULL, NULL },
	{ 42033, 0x10, "BodySerialNumber", NULL, NULL },
	{ 42034, 0x10, "LensSpecification", NULL, NULL },
	{ 42035, 0x10, "LensMake", NULL, NULL },
	{ 42036, 0x10, "LensModel", NULL, NULL },
	{ 42037, 0x10, "LensSerialNumber", NULL, NULL },
	{ 42038, 0x10, "ImageTitle", NULL, NULL },
	{ 42039, 0x10, "Photographer", NULL, NULL },
	{ 42040, 0x10, "ImageEditor", NULL, NULL },
	{ 42041, 0x10, "CameraFirmware", NULL, NULL },
	{ 42042, 0x10, "RAWDevelopingSoftware", NULL, NULL },
	{ 42043, 0x10, "ImageEditingSoftware", NULL, NULL },
	{ 42044, 0x10, "MetadataEditingSoftware", NULL, NULL },
	{ 42112, 0x0000, "GDAL_METADATA", NULL, NULL },
	{ 42113, 0x0000, "GDAL_NODATA", NULL, NULL },
	{ 42240, 0x10, "Gamma", NULL, NULL },
	{ 45056, 0x0801, "MPFVersion", NULL, NULL },
	{ 45057, 0x0801, "NumberOfImages", NULL, NULL },
	{ 45058, 0x0809, "MPEntry", handler_mpentry, NULL },
	{ 45059, 0x0801, "ImageUIDList", NULL, NULL },
	{ 45060, 0x0801, "TotalFrames", NULL, NULL },
	{ 45313, 0x0801, "MPIndividualNum", NULL, NULL },
	{ 45569, 0x0801, "PanOrientation", NULL, NULL },
	{ 45570, 0x0801, "PanOverlap_H", NULL, NULL },
	{ 45571, 0x0801, "PanOverlap_V", NULL, NULL },
	{ 45572, 0x0801, "BaseViewpointNum", NULL, NULL },
	{ 45573, 0x0801, "ConvergenceAngle", NULL, NULL },
	{ 45574, 0x0801, "BaselineLength", NULL, NULL },
	{ 45575, 0x0801, "VerticalDivergence", NULL, NULL },
	{ 45576, 0x0801, "AxisDistance_X", NULL, NULL },
	{ 45577, 0x0801, "AxisDistance_Y", NULL, NULL },
	{ 45578, 0x0801, "AxisDistance_Z", NULL, NULL },
	{ 45579, 0x0801, "YawAngle", NULL, NULL },
	{ 45580, 0x0801, "PitchAngle", NULL, NULL },
	{ 45581, 0x0801, "RollAngle", NULL, NULL },
	{ 48129, 0x0401, "PIXEL_FORMAT", handler_jxr_pixelformat, NULL },
	{ 48130, 0x0401, "SPATIAL_XFRM_PRIMARY", NULL, NULL },
	{ 48132, 0x0401, "IMAGE_TYPE", NULL, NULL },
	{ 48133, 0x0401, "PTM_COLOR_INFO", NULL, NULL },
	{ 48134, 0x0401, "PROFILE_LEVEL_CONTAINER", NULL, NULL },
	{ 48256, 0x0401, "IMAGE_WIDTH", NULL, NULL },
	{ 48257, 0x0401, "IMAGE_HEIGHT", NULL, NULL },
	{ 48258, 0x0401, "WIDTH_RESOLUTION", NULL, NULL },
	{ 48259, 0x0401, "HEIGHT_RESOLUTION", NULL, NULL },
	{ 48320, 0x0401, "IMAGE_OFFSET", handler_jxr_imageoffset, NULL },
	{ 48321, 0x0401, "IMAGE_BYTE_COUNT", NULL, NULL },
	{ 48322, 0x0401, "ALPHA_OFFSET", NULL, NULL },
	{ 48323, 0x0401, "ALPHA_BYTE_COUNT", NULL, NULL },
	{ 48324, 0x0401, "IMAGE_BAND_PRESENCE", NULL, NULL },
	{ 48325, 0x0401, "ALPHA_BAND_PRESENCE", NULL, NULL },
	{ 50215, 0x0000, "Oce Scanjob Description", NULL, NULL },
	{ 50216, 0x0000, "Oce Application Selector", NULL, NULL },
	{ 50217, 0x0000, "Oce Identification Number", NULL, NULL },
	{ 50218, 0x0000, "Oce ImageLogic Characteristics", NULL, NULL },
	{ 50341, 0x0008, "PrintImageMatching", handler_hexdump, NULL },
	{ 50706, 0x80, "DNGVersion", NULL, NULL},
	{ 50707, 0x80, "DNGBackwardVersion", NULL, NULL},
	{ 50708, 0x80, "UniqueCameraModel", NULL, NULL},
	{ 50709, 0x80, "LocalizedCameraModel", NULL, NULL},
	{ 50710, 0x80, "CFAPlaneColor", NULL, NULL},
	{ 50711, 0x80, "CFALayout", NULL, NULL},
	{ 50712, 0x80, "LinearizationTable", NULL, NULL},
	{ 50713, 0x80, "BlackLevelRepeatDim", NULL, NULL},
	{ 50714, 0x80, "BlackLevel", NULL, NULL},
	{ 50715, 0x80, "BlackLevelDeltaH", NULL, NULL},
	{ 50716, 0x80, "BlackLevelDeltaV", NULL, NULL},
	{ 50717, 0x80, "WhiteLevel", NULL, NULL},
	{ 50718, 0x80, "DefaultScale", NULL, NULL},
	{ 50719, 0x80, "DefaultCropOrigin", NULL, NULL},
	{ 50720, 0x80, "DefaultCropSize", NULL, NULL},
	{ 50721, 0x80, "ColorMatrix1", NULL, NULL},
	{ 50722, 0x80, "ColorMatrix2", NULL, NULL},
	{ 50723, 0x80, "CameraCalibration1", NULL, NULL},
	{ 50724, 0x80, "CameraCalibration2", NULL, NULL},
	{ 50725, 0x80, "ReductionMatrix1", NULL, NULL},
	{ 50726, 0x80, "ReductionMatrix2", NULL, NULL},
	{ 50727, 0x80, "AnalogBalance", NULL, NULL},
	{ 50728, 0x80, "AsShotNeutral", NULL, NULL},
	{ 50729, 0x80, "AsShotWhiteXY", NULL, NULL},
	{ 50730, 0x80, "BaselineExposure", NULL, NULL},
	{ 50731, 0x80, "BaselineNoise", NULL, NULL},
	{ 50732, 0x80, "BaselineSharpness", NULL, NULL},
	{ 50733, 0x80, "BayerGreenSplit", NULL, NULL},
	{ 50734, 0x80, "LinearResponseLimit", NULL, NULL},
	{ 50735, 0x80, "CameraSerialNumber", NULL, NULL},
	{ 50736, 0x80, "LensInfo", NULL, NULL},
	{ 50737, 0x80, "ChromaBlurRadius", NULL, NULL},
	{ 50738, 0x80, "AntiAliasStrength", NULL, NULL},
	{ 50739, 0x80, "ShadowScale", NULL, NULL},
	{ 50740, 0x0080, "DNGPrivateData", handler_dngprivatedata, NULL},
	{ 50741, 0x80, "MakerNoteSafety", NULL, NULL},
	{ 50778, 0x80, "CalibrationIlluminant1", NULL, NULL},
	{ 50779, 0x80, "CalibrationIlluminant2", NULL, NULL},
	{ 50780, 0x80, "BestQualityScale", NULL, NULL},
	{ 50781, 0x0088, "RawDataUniqueID", handler_hexdumpb, NULL},
	{ 50784, 0x0000, "Alias Layer Metadata", NULL, NULL },
	{ 50827, 0x80, "OriginalRawFileName", NULL, NULL},
	{ 50828, 0x80, "OriginalRawFileData", NULL, NULL},
	{ 50829, 0x80, "ActiveArea", NULL, NULL},
	{ 50830, 0x80, "MaskedAreas", NULL, NULL},
	{ 50831, 0x80, "AsShotICCProfile", NULL, NULL},
	{ 50832, 0x80, "AsShotPreProfileMatrix", NULL, NULL},
	{ 50833, 0x80, "CurrentICCProfile", NULL, NULL},
	{ 50834, 0x80, "CurrentPreProfileMatrix", NULL, NULL},
	{ 50879, 0x80, "ColorimetricReference", NULL, NULL},
	{ 50931, 0x80, "CameraCalibrationSignature", NULL, NULL},
	{ 50932, 0x80, "ProfileCalibrationSignature", NULL, NULL},
	{ 50933, 0x80, "ExtraCameraProfiles", NULL, NULL},
	{ 50934, 0x80, "AsShotProfileName", NULL, NULL},
	{ 50935, 0x80, "NoiseReductionApplied", NULL, NULL},
	{ 50936, 0x80, "ProfileName", NULL, NULL},
	{ 50937, 0x80, "ProfileHueSatMapDims", NULL, NULL},
	{ 50938, 0x80, "ProfileHueSatMapData1", NULL, NULL},
	{ 50939, 0x80, "ProfileHueSatMapData2", NULL, NULL},
	{ 50940, 0x80, "ProfileToneCurve", NULL, NULL},
	{ 50941, 0x80, "ProfileEmbedPolicy", NULL, valdec_profileembedpolicy},
	{ 50942, 0x80, "ProfileCopyright", NULL, NULL},
	{ 50964, 0x80, "ForwardMatrix1", NULL, NULL},
	{ 50965, 0x80, "ForwardMatrix2", NULL, NULL},
	{ 50966, 0x80, "PreviewApplicationName", NULL, NULL},
	{ 50967, 0x80, "PreviewApplicationVersion", NULL, NULL},
	{ 50968, 0x80, "PreviewSettingsName", NULL, NULL},
	{ 50969, 0x0088, "PreviewSettingsDigest", handler_hexdumpb, NULL},
	{ 50970, 0x80, "PreviewColorSpace", NULL, valdec_dngcolorspace},
	{ 50971, 0x80, "PreviewDateTime", NULL, NULL},
	{ 50972, 0x0088, "RawImageDigest", handler_hexdumpb, NULL},
	{ 50973, 0x0088, "OriginalRawFileDigest", handler_hexdumpb, NULL},
	{ 50974, 0x80, "SubTileBlockSize", NULL, NULL},
	{ 50975, 0x80, "RowInterleaveFactor", NULL, NULL},
	{ 50981, 0x80, "ProfileLookTableDims", NULL, NULL},
	{ 50982, 0x80, "ProfileLookTableData", NULL, NULL},
	{ 51008, 0x80, "OpcodeList1", NULL, NULL},
	{ 51009, 0x80, "OpcodeList2", NULL, NULL},
	{ 51022, 0x80, "OpcodeList3", NULL, NULL},
	{ 51041, 0x80, "NoiseProfile", NULL, NULL},
	{ 51089, 0x80, "OriginalDefaultFinalSize", NULL, NULL},
	{ 51090, 0x80, "OriginalBestQualityFinalSize", NULL, NULL},
	{ 51091, 0x80, "OriginalDefaultCropSize", NULL, NULL},
	{ 51107, 0x80, "ProfileHueSatMapEncoding", NULL, NULL},
	{ 51108, 0x80, "ProfileLookTableEncoding", NULL, NULL},
	{ 51109, 0x80, "BaselineExposureOffset", NULL, NULL},
	{ 51110, 0x80, "DefaultBlackRender", NULL, NULL},
	{ 51111, 0x0088, "NewRawImageDigest", handler_hexdumpb, NULL},
	{ 51112, 0x80, "RawToPreviewGain", NULL, NULL},
	{ 51113, 0x80, "CacheBlob", NULL, NULL},
	{ 51114, 0x80, "CacheVersion", NULL, NULL},
	{ 51125, 0x80, "DefaultUserCrop", NULL, NULL},
	{ 59932, 0x0440, "PADDING_DATA", NULL, NULL },
	{ 59933, 0x0010, "OffsetSchema", NULL, NULL },

	{ 1, 0x0021, "InteroperabilityIndex", NULL, NULL },
	{ 2, 0x0021, "InteroperabilityVersion", NULL, NULL },
	{ 4096, 0x0020, "RelatedImageFileFormat", NULL, NULL },
	{ 4097, 0x0020, "RelatedImageWidth", NULL, NULL },
	{ 4098, 0x0020, "RelatedImageLength", NULL, NULL },

	{ 0, 0x0041, "GPSVersionID", NULL, NULL },
	{ 1, 0x0041, "GPSLatitudeRef", NULL, NULL },
	{ 2, 0x0041, "GPSLatitude", handler_gpslatitude, NULL },
	{ 3, 0x0041, "GPSLongitudeRef", NULL, NULL },
	{ 4, 0x0041, "GPSLongitude", NULL, NULL },
	{ 5, 0x0041, "GPSAltitudeRef", NULL, NULL },
	{ 6, 0x0041, "GPSAltitude", NULL, NULL },
	{ 7, 0x0041, "GPSTimeStamp", NULL, NULL },
	{ 8, 0x0041, "GPSSatellites", NULL, NULL },
	{ 9, 0x0041, "GPSStatus", NULL, NULL },
	{ 10, 0x0041, "GPSMeasureMode", NULL, NULL },
	{ 11, 0x0041, "GPSDOP", NULL, NULL },
	{ 12, 0x0041, "GPSSpeedRef", NULL, NULL },
	{ 13, 0x0041, "GPSSpeed", NULL, NULL },
	{ 14, 0x0041, "GPSTrackRef", NULL, NULL },
	{ 15, 0x0041, "GPSTrack", NULL, NULL },
	{ 16, 0x0041, "GPSImgDirectionRef", NULL, NULL },
	{ 17, 0x0041, "GPSImgDirection", NULL, NULL },
	{ 18, 0x0041, "GPSMapDatum", NULL, NULL },
	{ 19, 0x0041, "GPSLatitudeRef", NULL, NULL },
	{ 20, 0x0041, "GPSLatitude", NULL, NULL },
	{ 21, 0x0041, "GPSDestLongitudeRef", NULL, NULL },
	{ 22, 0x0041, "GPSDestLongitude", NULL, NULL },
	{ 23, 0x0041, "GPSDestBearingRef", NULL, NULL },
	{ 24, 0x0041, "GPSDestBearing", NULL, NULL },
	{ 25, 0x0041, "GPSDestDistanceRef", NULL, NULL },
	{ 26, 0x0041, "GPSDestDistance", NULL, NULL },
	{ 27, 0x0041, "GPSProcessingMethod", NULL, NULL },
	{ 28, 0x0041, "GPSAreaInformation", NULL, NULL },
	{ 29, 0x0041, "GPSDateStamp", NULL, NULL },
	{ 30, 0x0041, "GPSDifferential", NULL, NULL },
	{ 31, 0x0041, "GPSHPositioningError", NULL, NULL },

	{ 1, 0x1001, "MakerNoteVersion", NULL, NULL },
	{ 2, 0x1001, "ISOSpeed", NULL, NULL },
	{ 3, 0x1001, "ColorMode", NULL, NULL },
	{ 4, 0x1001, "Quality", NULL, NULL },
	{ 5, 0x1001, "WhiteBalance", NULL, NULL },
	{ 6, 0x1001, "Sharpness", NULL, NULL },
	{ 7, 0x1001, "FocusMode", NULL, NULL },
	{ 8, 0x1001, "FlashSetting", NULL, NULL },
	{ 9, 0x1001, "FlashType", NULL, NULL },
	{ 0xb, 0x1001, "WhiteBalanceFineTune", NULL, NULL },
	{ 0xc, 0x1001, "WB_RBLevels", NULL, NULL },
	{ 0xd, 0x1001, "ProgramShift", NULL, NULL },
	{ 0xe, 0x1001, "ExposureDifference", NULL, NULL },
	{ 0xf, 0x1001, "ISOSelection", NULL, NULL },
	{ 0x10, 0x1001, "DataDump", NULL, NULL },
	{ 0x11, 0x1001, "PreviewIFD", handler_subifd, NULL },
	{ 0x12, 0x1001, "FlashExposureComp", NULL, NULL },
	{ 0x13, 0x1001, "ISOSetting", NULL, NULL },
	{ 0x16, 0x1001, "ImageBoundary", NULL, NULL },
	{ 0x17, 0x1001, "ExternalFlashExposureComp", NULL, NULL },
	{ 0x18, 0x1001, "FlashExposureBracketValue", NULL, NULL },
	{ 0x19, 0x1001, "ExposureBracketValue", NULL, NULL },
	{ 0x1a, 0x1001, "ImageProcessing", NULL, NULL },
	{ 0x1b, 0x1001, "CropHiSpeed", NULL, NULL },
	{ 0x1c, 0x1001, "ExposureTuning", NULL, NULL },
	{ 0x1d, 0x1001, "SerialNumber", NULL, NULL },
	{ 0x1e, 0x1001, "ColorSpace", NULL, NULL },
	{ 0x1f, 0x1001, "VRInfo", NULL, NULL },
	{ 0x20, 0x1001, "ImageAuthentication", NULL, NULL },
	{ 0x21, 0x1001, "FaceDetect", NULL, NULL },
	{ 0x22, 0x1001, "ActiveD-Lighting", NULL, NULL },
	{ 0x23, 0x1001, "PictureControlData", NULL, NULL },
	{ 0x24, 0x1001, "WorldTime", NULL, NULL },
	{ 0x25, 0x1001, "ISOInfo", NULL, NULL },
	{ 0x2a, 0x1001, "VignetteControl", NULL, NULL },
	{ 0x2b, 0x1001, "DistortInfo", NULL, NULL },
	{ 0x35, 0x1001, "HDRInfo", NULL, NULL },
	{ 0x37, 0x1001, "MechanicalShutterCount", NULL, NULL },
	{ 0x39, 0x1001, "LocationInfo", NULL, NULL },
	{ 0x3d, 0x1001, "BlackLevel", NULL, NULL },
	{ 0x4f, 0x1001, "ColorTemperatureAuto", NULL, NULL },
	{ 0x80, 0x1001, "ImageAdjustment", NULL, NULL },
	{ 0x81, 0x1001, "ToneComp", NULL, NULL },
	{ 0x82, 0x1001, "AuxiliaryLens", NULL, NULL },
	{ 0x83, 0x1001, "LensType", NULL, NULL },
	{ 0x84, 0x1001, "Lens", NULL, NULL },
	{ 0x85, 0x1001, "ManualFocusDistance", NULL, NULL },
	{ 0x86, 0x1001, "DigitalZoom", NULL, NULL },
	{ 0x87, 0x1001, "FlashMode", NULL, NULL },
	{ 0x88, 0x1001, "AFFocusPosition", NULL, NULL },
	{ 0x89, 0x1001, "ShootingMode", NULL, NULL },
	{ 0x8b, 0x1001, "LensFStops", NULL, NULL },
	{ 0x8c, 0x1001, "ContrastCurve", NULL, NULL },
	{ 0x8d, 0x1001, "ColorHue", NULL, NULL },
	{ 0x8f, 0x1001, "SceneMode", NULL, NULL },
	{ 0x90, 0x1001, "LightSource", NULL, NULL },
	{ 0x91, 0x1001, "ShotInfo", NULL, NULL },
	{ 0x92, 0x1001, "HueAdjustment", NULL, NULL },
	{ 0x93, 0x1001, "NEFCompression", NULL, NULL },
	{ 0x94, 0x1001, "Saturation", NULL, NULL },
	{ 0x95, 0x1001, "NoiseReduction", NULL, NULL },
	{ 0x96, 0x1001, "NEFLinearizationTable", NULL, NULL },
	{ 0x97, 0x1001, "ColorBalance", NULL, NULL },
	{ 0x98, 0x1001, "LensData", NULL, NULL },
	{ 0x99, 0x1001, "RawImageCenter", NULL, NULL },
	{ 0x9a, 0x1001, "SensorPixelSize", NULL, NULL },
	{ 0x9c, 0x1001, "SceneAssist", NULL, NULL },
	{ 0x9e, 0x1001, "RetouchHistory", NULL, NULL },
	{ 0xa0, 0x1001, "SerialNumber", NULL, NULL },
	{ 0xa2, 0x1001, "ImageDataSize", NULL, NULL },
	{ 0xa5, 0x1001, "ImageCount", NULL, NULL },
	{ 0xa6, 0x1001, "DeletedImageCount", NULL, NULL },
	{ 0xa7, 0x1001, "ShutterCount", NULL, NULL },
	{ 0xa8, 0x1001, "FlashInfo", NULL, NULL },
	{ 0xa9, 0x1001, "ImageOptimization", NULL, NULL },
	{ 0xaa, 0x1001, "Saturation", NULL, NULL },
	{ 0xab, 0x1001, "VariProgram", NULL, NULL },
	{ 0xac, 0x1001, "ImageStabilization", NULL, NULL },
	{ 0xad, 0x1001, "AFResponse", NULL, NULL },
	{ 0xb0, 0x1001, "MultiExposure", NULL, NULL },
	{ 0xb1, 0x1001, "HighISONoiseReduction", NULL, NULL },
	{ 0xb3, 0x1001, "ToningEffect", NULL, NULL },
	{ 0xb6, 0x1001, "PowerUpTime", NULL, NULL },
	{ 0xb7, 0x1001, "AFInfo2", NULL, NULL },
	{ 0xb8, 0x1001, "FileInfo", NULL, NULL },
	{ 0xb9, 0x1001, "AFTune", NULL, NULL },
	{ 0xbb, 0x1001, "RetouchInfo", NULL, NULL },
	{ 0xbd, 0x1001, "PictureControlData", NULL, NULL },
	{ 0xc3, 0x1001, "BarometerInfo", NULL, NULL },
	{ 0xe00, 0x1001, "PrintIM", NULL, NULL },
	{ 0xe01, 0x1001, "NikonCaptureData", NULL, NULL },
	{ 0xe09, 0x1001, "NikonCaptureVersion", NULL, NULL },
	{ 0xe0e, 0x1001, "NikonCaptureOffsets", NULL, NULL },
	{ 0xe10, 0x1001, "NikonScanIFD", NULL, NULL },
	{ 0xe13, 0x1001, "NikonCaptureEditVersions", NULL, NULL },
	{ 0xe1d, 0x1001, "NikonICCProfile", NULL, NULL },
	{ 0xe1e, 0x1001, "NikonCaptureOutput", NULL, NULL },
	{ 0xe22, 0x1001, "NEFBitDepth", NULL, NULL },

	{ 2, 0x2009, "?", handler_bplist, NULL },
	{ 3, 0x2009, "RunTime", handler_bplist, NULL },
	{ 8, 0x2001, "AccelerationVector", NULL, NULL },
	{ 0xa, 0x2001, "HDRImageType", NULL, NULL },
	{ 0xb, 0x2001, "BurstUUID", NULL, NULL },
	{ 0xe, 0x2001, "Orientation?", NULL, NULL },
	{ 0x11, 0x2001, "ContentIdentifier", NULL, NULL },
	{ 0x15, 0x2001, "ImageUniqueID", NULL, NULL },

	{ 0x1, 0x4001, "PanasonicRawVersion", NULL, NULL },
	{ 0x2, 0x4001, "SensorWidth", NULL, NULL },
	{ 0x3, 0x4001, "SensorHeight", NULL, NULL },
	{ 0x4, 0x4001, "SensorTopBorder", NULL, NULL },
	{ 0x5, 0x4001, "SensorLeftBorder", NULL, NULL },
	{ 0x6, 0x4001, "SensorBottomBorder", NULL, NULL },
	{ 0x7, 0x4001, "SensorRightBorder", NULL, NULL },
	{ 0x8, 0x4001, "SamplesPerPixel", NULL, NULL },
	{ 0x9, 0x4001, "CFAPattern", NULL, NULL },
	{ 0xa, 0x4001, "BitsPerSample", NULL, NULL },
	{ 0xb, 0x4001, "Compression", NULL, NULL },
	{ 0xe, 0x4001, "LinearityLimitRed", NULL, NULL },
	{ 0xf, 0x4001, "LinearityLimitGreen", NULL, NULL },
	{ 0x10, 0x4001, "LinearityLimitBlue", NULL, NULL },
	{ 0x11, 0x4001, "RedBalance", NULL, NULL },
	{ 0x12, 0x4001, "BlueBalance", NULL, NULL },
	{ 0x13, 0x4001, "WBInfo", NULL, NULL },
	{ 0x17, 0x4001, "ISO", NULL, NULL },
	{ 0x18, 0x4001, "HighISOMultiplierRed", NULL, NULL },
	{ 0x19, 0x4001, "HighISOMultiplierGreen", NULL, NULL },
	{ 0x1a, 0x4001, "HighISOMultiplierBlue", NULL, NULL },
	{ 0x1b, 0x4001, "NoiseReductionParams", NULL, NULL },
	{ 0x1c, 0x4001, "BlackLevelRed", NULL, NULL },
	{ 0x1d, 0x4001, "BlackLevelGreen", NULL, NULL },
	{ 0x1e, 0x4001, "BlackLevelBlue", NULL, NULL },
	{ 0x24, 0x4001, "WBRedLevel", NULL, NULL },
	{ 0x25, 0x4001, "WBGreenLevel", NULL, NULL },
	{ 0x26, 0x4001, "WBBlueLevel", NULL, NULL },
	{ 0x27, 0x4001, "WBInfo2", NULL, NULL },
	{ 0x2d, 0x4001, "RawFormat", NULL, NULL },
	{ 0x2e, 0x4009, "JpgFromRaw", handler_panasonicjpg, NULL },
	{ 0x2f, 0x4001, "CropTop", NULL, NULL },
	{ 0x30, 0x4001, "CropLeft", NULL, NULL },
	{ 0x31, 0x4001, "CropBottom", NULL, NULL },
	{ 0x32, 0x4001, "CropRight", NULL, NULL },

	{ 0x0000, 0x8001, "Version", NULL, NULL},
	{ 0x0010, 0x8001, "InternalSerialNumber", NULL, NULL},
	{ 0x1000, 0x8001, "Quality", NULL, NULL},
	{ 0x1001, 0x8001, "Sharpness", NULL, NULL},
	{ 0x1002, 0x8001, "WhiteBalance", NULL, NULL},
	{ 0x1003, 0x8001, "Saturation", NULL, NULL},
	{ 0x1010, 0x8001, "FujiFlashMode", NULL, NULL},
	{ 0x1011, 0x8001, "FlashExposureComp", NULL, NULL},
	{ 0x1020, 0x8001, "Macro", NULL, NULL},
	{ 0x1021, 0x8001, "FocusMode", NULL, NULL},
	{ 0x1022, 0x8001, "AFMode", NULL, NULL},
	{ 0x1023, 0x8001, "FocusPixel", NULL, NULL},
	{ 0x1030, 0x8001, "SlowSync", NULL, NULL},
	{ 0x1031, 0x8001, "PictureMode", NULL, NULL},
	{ 0x1032, 0x8001, "ExposureCount", NULL, NULL},
	{ 0x1100, 0x8001, "AutoBracketing", NULL, NULL},
	{ 0x1101, 0x8001, "SequenceNumber", NULL, NULL},
	{ 0x1201, 0x8001, "AdvancedFilter", NULL, NULL},
	{ 0x1210, 0x8001, "ColorMode", NULL, NULL},
	{ 0x1300, 0x8001, "BlurWarning", NULL, NULL},
	{ 0x1301, 0x8001, "FocusWarning", NULL, NULL},
	{ 0x1302, 0x8001, "ExposureWarning", NULL, NULL},
	{ 0x1400, 0x8001, "DynamicRange", NULL, NULL},
	{ 0x1422, 0x8001, "ImageStabilization", NULL, NULL},
	{ 0x4100, 0x8001, "FacesDetected", NULL, NULL},
	{ 0x4200, 0x8001, "NumFaceElements", NULL, NULL}
};

static void do_dbg_print_numeric_values(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni,
	de_ucstring *dbgline)
{
	i64 i;
	struct valdec_params vp;
	struct valdec_result vr;
	struct numeric_value nv;

	de_zeromem(&vr, sizeof(struct valdec_result));

	switch(tg->datatype) {
	case DATATYPE_BYTE: case DATATYPE_SBYTE:
	case DATATYPE_UNDEF: case DATATYPE_ASCII: case DATATYPE_UTF8:
	case DATATYPE_UINT16: case DATATYPE_SINT16:
	case DATATYPE_UINT32: case DATATYPE_SINT32: case DATATYPE_IFD32:
	case DATATYPE_UINT64: case DATATYPE_SINT64: case DATATYPE_IFD64:
	case DATATYPE_RATIONAL: case DATATYPE_SRATIONAL:
	case DATATYPE_FLOAT32: case DATATYPE_FLOAT64:
		break;
	default:
		goto done; // Not a supported numeric datatype
	}

	ucstring_append_sz(dbgline, " {", DE_ENCODING_LATIN1);

	// Populate the fields of vp/vr that don't change.
	vp.d = d;
	vp.tg = tg;
	vr.s = ucstring_create(c);

	for(i=0; i<tg->valcount && i<DE_TIFF_MAX_VALUES_TO_PRINT; i++) {
		read_numeric_value(c, d, tg, i, &nv, dbgline);

		// If possible, decode the value and print its name.
		if(nv.isvalid && tni->vdfn) {
			// Set the remaining fields of vp/vr.
			vp.idx = i;
			vp.n = nv.val_int64;
			ucstring_empty(vr.s);

			if(tni->vdfn(c, &vp, &vr)) {
				ucstring_append_sz(dbgline, "(=", DE_ENCODING_LATIN1);
				ucstring_append_ucstring(dbgline, vr.s);
				ucstring_append_sz(dbgline, ")", DE_ENCODING_LATIN1);
			}
		}

		if(i<tg->valcount-1) {
			ucstring_append_sz(dbgline, ",", DE_ENCODING_LATIN1);
		}
	}
	if(tg->valcount>DE_TIFF_MAX_VALUES_TO_PRINT) {
		ucstring_append_sz(dbgline, "...", DE_ENCODING_LATIN1);
	}
	ucstring_append_sz(dbgline, "}", DE_ENCODING_LATIN1);
done:
	if(vr.s) ucstring_destroy(vr.s);
}

static void do_dbg_print_text_multi_values(deark *c, lctx *d, const struct taginfo *tg,
	const struct tagnuminfo *tni, de_ucstring *dbgline)
{
	int is_truncated = 0;
	int str_count = 0;
	i64 pos, endpos;
	i64 adj_totalsize;
	de_encoding enc;

	enc = (tg->datatype==DATATYPE_UTF8) ? DE_ENCODING_UTF8 : d->current_textfield_encoding;

	// An ASCII field is a sequence of NUL-terminated strings.
	// The spec does not say what to do if an ASCII field does not end in a NUL.
	// Our rule is that if the field does not end in a NUL byte (including the case
	// where it is 0 length), then treat it as if it has a NUL byte appended to it.
	// The other options would be to pretend the last byte is always NUL, or to
	// ignore everything after the last NUL byte.

	adj_totalsize = tg->total_size;
	if(adj_totalsize > DE_TIFF_MAX_CHARS_TO_PRINT) {
		adj_totalsize = DE_TIFF_MAX_CHARS_TO_PRINT;
		// FIXME: Suboptimal things might happen if we truncate exactly one byte
		is_truncated = 1;
	}
	endpos = tg->val_offset + adj_totalsize;

	ucstring_append_sz(dbgline, " {", DE_ENCODING_LATIN1);

	pos = tg->val_offset;
	while(1) {
		struct de_stringreaderdata *srd;

		if(pos>=endpos && str_count>0) break;

		srd = dbuf_read_string(c->infile, pos, endpos-pos, endpos-pos,
			DE_CONVFLAG_STOP_AT_NUL, enc);

		if(str_count>0) ucstring_append_sz(dbgline, ",", DE_ENCODING_LATIN1);
		ucstring_append_sz(dbgline, "\"", DE_ENCODING_LATIN1);
		ucstring_append_ucstring(dbgline, srd->str);
		ucstring_append_sz(dbgline, "\"", DE_ENCODING_LATIN1);
		str_count++;

		pos += srd->bytes_consumed;
		de_destroy_stringreaderdata(c, srd);
	}

	if(is_truncated) {
		ucstring_append_sz(dbgline, "...", DE_ENCODING_LATIN1);
	}
	ucstring_append_sz(dbgline, "}", DE_ENCODING_LATIN1);
}

// Used for ASCII-type tag numbers that we expect to contain only a single
// string (i.e. nearly all of them).
static void do_dbg_print_text_single_value(deark *c, lctx *d, const struct taginfo *tg,
	const struct tagnuminfo *tni, de_ucstring *dbgline)
{
	struct de_stringreaderdata *srd = NULL;
	de_encoding enc;

	enc = (tg->datatype==DATATYPE_UTF8) ? DE_ENCODING_UTF8 : d->current_textfield_encoding;

	srd = dbuf_read_string(c->infile, tg->val_offset, tg->total_size,
		DE_TIFF_MAX_CHARS_TO_PRINT, DE_CONVFLAG_STOP_AT_NUL, enc);

	ucstring_append_sz(dbgline, " {\"", DE_ENCODING_LATIN1);
	ucstring_append_ucstring(dbgline, srd->str);
	if(srd->was_truncated) {
		ucstring_append_sz(dbgline, "...", DE_ENCODING_LATIN1);
	}
	ucstring_append_sz(dbgline, "\"}", DE_ENCODING_LATIN1);

	de_destroy_stringreaderdata(c, srd);
}

static void do_dbg_print_values(deark *c, lctx *d, const struct taginfo *tg, const struct tagnuminfo *tni,
	de_ucstring *dbgline)
{
	if(c->debug_level<1) return;
	if(tni->flags&0x08) return; // Auto-display of values is suppressed for this tag.
	if(tg->valcount<1) return;

	if(tg->datatype==DATATYPE_ASCII || tg->datatype==DATATYPE_UTF8) {
		if(tni->flags & 0x0004) {
			do_dbg_print_text_multi_values(c, d, tg, tni, dbgline);
		}
		else {
			do_dbg_print_text_single_value(c, d, tg, tni, dbgline);
		}
	}
	else {
		do_dbg_print_numeric_values(c, d, tg, tni, dbgline);
	}
}

static const struct tagnuminfo *find_tagnuminfo(int tagnum, int filefmt, u8 ifdtype)
{
	size_t i;

	for(i=0; i<DE_ARRAYCOUNT(tagnuminfo_arr); i++) {
		if(tagnuminfo_arr[i].tagnum!=tagnum) {
			continue;
		}

		if(ifdtype==IFDTYPE_EXIFINTEROP) {
			// For interoperability IFDs, allow only special tags
			if(!(tagnuminfo_arr[i].flags&0x20)) {
				continue;
			}
		}
		else if(ifdtype==IFDTYPE_GPS) {
			// For GPS IFDs, allow only special tags
			if(!(tagnuminfo_arr[i].flags&0x40)) {
				continue;
			}
		}
		else if(ifdtype==IFDTYPE_NIKONMN) {
			// For this IFD, allow only special tags
			if(!(tagnuminfo_arr[i].flags&0x1000)) {
				continue;
			}
		}
		else if(ifdtype==IFDTYPE_APPLEMN) {
			// For this IFD, allow only special tags
			if(!(tagnuminfo_arr[i].flags&0x2000)) {
				continue;
			}
		}
		else if(ifdtype==IFDTYPE_FUJIFILMMN) {
			// For this IFD, allow only special tags
			if(!(tagnuminfo_arr[i].flags&0x8000)) {
				continue;
			}
		}
		else if(tagnuminfo_arr[i].flags&0x01) {
			// A special tag not allowed above
			if(filefmt==DE_TIFFFMT_JPEGXR && (tagnuminfo_arr[i].flags&0x0400)) {
				// Allow all JPEG XR tags in normal JPEG XR IFDs.
				// Maybe we should disallow TIFF tags that are not known to be
				// allowed in JPEG XR files, but I suspect a lot of random TIFF
				// tags are occasionally used in JPEG XR, and I'm not aware of
				// any conflicts.
				;
			}
			else if(filefmt==DE_TIFFFMT_MPEXT && (tagnuminfo_arr[i].flags&0x0800)) {
				;
			}
			else if(filefmt==DE_TIFFFMT_NIKONMN && (tagnuminfo_arr[i].flags&0x1000)) {
				;
			}
			else if(filefmt==DE_TIFFFMT_PANASONIC && (tagnuminfo_arr[i].flags&0x4000) &&
				ifdtype==IFDTYPE_NORMAL)
			{
				;
			}
			else {
				// Ignore this tag -- it's in the wrong "namespace".
				continue;
			}
		}

		return &tagnuminfo_arr[i];
	}
	return NULL;
}

struct decode_page_ctx {
	i64 width; // = ImageWidth + padding
	i64 height; // = ImageLength
	i64 strile_max_w, strile_max_h;
	i64 strile_max_rowspan;
	i64 striles_per_plane;
	u32 samples_per_pixel_per_plane;

	i64 strileset_idx;
	i64 strileset_xpos, strileset_ypos;
	i64 strileset_height;
	i64 strileset_width;
	i64 strileset_rowspan;
	i64 strile_idx;
	dbuf *unc_strile_dbuf[DE_TIFF_MAX_SAMPLES]; // Pointers to other dbufs; do not free

	UI num_planes_to_decode;
	UI samples_per_pixel_to_decode;
	UI base_samples_per_pixel;
	u8 is_tiled;
	u8 is_separated;
	u8 dst_flag_16bit;
	u8 is_grayscale;
	u8 grayscale_reverse_polarity;
	u8 need_to_reverse_bits;
	u8 need_to_reverse_nibbles;
	u8 use_pal;
	u8 has_alpha;
	u8 is_assoc_alpha;
	UI alpha_sample_idx;

	struct de_dfilter_ctx *dfctx; // Decompressor that can be shared between striles

	struct de_dfilter_in_params dcmpri;
	struct de_dfilter_out_params dcmpro;
	struct de_dfilter_results dres;
};

static void reverse_bits_in_membuf(dbuf *f)
{
	i64 len = f->len;
	i64 i;

	dbuf_flush(f);
	len = f->len;

	for(i=0; i<len; i++) {
		u8 b, b2;
		size_t k;

		b = dbuf_getbyte(f, i);
		if(b==0x00 || b==0xff) continue;
		b2 = 0;
		for(k=0; k<8; k++) {
			if(b & (1U<<k)) {
				b2 |= (1U<<(7-k));
			}
		}
		dbuf_writebyte_at(f, i, b2);
	}
}

static void reverse_nibbles_in_membuf(dbuf *f)
{
	i64 len = f->len;
	i64 i;

	dbuf_flush(f);
	len = f->len;

	for(i=0; i<len; i++) {
		u8 b, b2;

		b = dbuf_getbyte(f, i);
		b2 = ((b & 0x0f)<<4) | (b>>4);
		dbuf_writebyte_at(f, i, b2);
	}

	dbuf_flush(f);
}

static void decompress_strile_uncmpr(deark *c, lctx *d, struct page_ctx *pg,
	struct decode_page_ctx *dctx)
{
	fmtutil_decompress_uncompressed(c, &dctx->dcmpri, &dctx->dcmpro, &dctx->dres, 0);
	if(dctx->need_to_reverse_bits) {
		reverse_bits_in_membuf(dctx->dcmpro.f);
	}
}

// If old LZW version, sets pg->is_old_lzw.
static void detect_lzw_version(deark *c, lctx *d, struct page_ctx *pg)
{
	u8 buf[2];

	if(pg->strile_count<1) return;
	// The first LZW code is supposed to be a clear code (=256; 9 bits). If it is
	// LSB-first, presumably this is the old LZW version.
	// So old version should start: 00000000 xxxxxxx1
	// And new version:             10000000 0xxxxxxx
	de_read(buf, pg->strile_data[0].pos, 2);
	if(buf[0]==0x00 && (buf[1]&0x01)) {
		pg->is_old_lzw = 1;
	}
	de_dbg(c, "lzw version: %s", pg->is_old_lzw?"old":"new");
}

// Assumes that detect_lzw_version() has already been called for this pg.
static void decompress_strile_lzw(deark *c, lctx *d, struct page_ctx *pg,
	struct decode_page_ctx *dctx)
{
	struct de_lzw_params delzwp;

	if(dctx->dfctx) {
		de_dfilter_destroy(dctx->dfctx);
		dctx->dfctx = NULL;
		// TODO: Make the LZW decoder support DE_DFILTER_COMMAND_REINITIALIZE
	}

	de_zeromem(&delzwp, sizeof(struct de_lzw_params));
	delzwp.fmt =  (pg->is_old_lzw)?DE_LZWFMT_TIFFOLD:DE_LZWFMT_TIFFNEW;
	delzwp.max_code_size = 12;

	dctx->dfctx = de_dfilter_create(c, dfilter_lzw_codec, (void*)&delzwp, &dctx->dcmpro, &dctx->dres);

	de_dfilter_addslice(dctx->dfctx, dctx->dcmpri.f, dctx->dcmpri.pos, dctx->dcmpri.len);
	de_dfilter_finish(dctx->dfctx);
}

static void decompress_strile_packbits(deark *c, lctx *d, struct page_ctx *pg,
	struct decode_page_ctx *dctx)
{
	if(dctx->dfctx) {
		de_dfilter_command(dctx->dfctx, DE_DFILTER_COMMAND_REINITIALIZE, 0);
	}
	else {
		dctx->dfctx = de_dfilter_create(c, dfilter_packbits_codec, NULL,
			&dctx->dcmpro, &dctx->dres);
	}

	de_dfilter_addslice(dctx->dfctx, dctx->dcmpri.f, dctx->dcmpri.pos, dctx->dcmpri.len);
	de_dfilter_finish(dctx->dfctx);
}

static void decompress_strile_deflate(deark *c, lctx *d, struct page_ctx *pg,
	struct decode_page_ctx *dctx)
{
	struct de_deflate_params inflparams;

	de_zeromem(&inflparams, sizeof(struct de_deflate_params));
	inflparams.flags = DE_DEFLATEFLAG_ISZLIB;
	fmtutil_decompress_deflate_ex(c, &dctx->dcmpri, &dctx->dcmpro, &dctx->dres,
		 &inflparams);
}

static void decompress_strile_fax34(deark *c, lctx *d, struct page_ctx *pg,
	struct decode_page_ctx *dctx)
{
	struct de_fax34_params fax34params;

	de_zeromem(&fax34params, sizeof(struct de_fax34_params));
	fax34params.image_width = dctx->strileset_width;
	fax34params.image_height = dctx->strileset_height;
	fax34params.tiff_cmpr_meth = (UI)pg->compression;
	fax34params.t4options = pg->t4options;
	fax34params.t6options = pg->t6options;
	fax34params.is_lsb = (pg->fill_order==2)?1:0;

	fmtutil_fax34_codectype1(c, &dctx->dcmpri, &dctx->dcmpro, &dctx->dres,
		 (void*)&fax34params);
}

static const struct cmpr_meth_info cmpr_meth_info_arr[] = {
	{ CMPR_NONE, 0x0, "uncompressed", decompress_strile_uncmpr },
	{ CMPR_CCITTRLE, 0x0, "CCITTRLE", decompress_strile_fax34 },
	{ CMPR_FAX3, 0x0, "Fax3", decompress_strile_fax34 },
	{ CMPR_FAX4, 0x0, "Fax4", decompress_strile_fax34 },
	{ CMPR_LZW, 0x0, "LZW", decompress_strile_lzw },
	{ CMPR_OLDJPEG, 0x0, "OldJPEG", NULL},
	{ CMPR_NEWJPEG, 0x0, "NewJPEG", NULL},
	{ CMPR_DEFLATE, 0x0, "DEFLATE", decompress_strile_deflate },
	{ 9, 0x0, "T.85 JBIG", NULL},
	{ 10, 0x0, "T.43 JBIG", NULL},
	{ 103, 0x0, "Pegasus IMPACJ?", NULL},
	{ 32766, 0x0, "NeXT 2-bit RLE", NULL},
	{ 32771, 0x0, "CCITTRLEW", NULL},
	{ CMPR_PACKBITS, 0x0, "PackBits", decompress_strile_packbits },
	{ 32809, 0x0, "ThunderScan", NULL},
	{ 32895, 0x0, "IT8CTPAD", NULL},
	{ 32896, 0x0, "IT8LW", NULL},
	{ 32897, 0x0, "IT8MP/HC", NULL},
	{ 32898, 0x0, "IT8BL", NULL},
	{ 32908, 0x0, "PIXARFILM", NULL},
	{ 32909, 0x0, "PIXARLOG", NULL},
	{ CMPR_DEFLATE_ALT, 0x0, "DEFLATE", decompress_strile_deflate },
	{ 32947, 0x0, "DCS", NULL},
	{ 34661, 0x0, "ISO JBIG", NULL},
	{ 34676, 0x0, "SGILOG", NULL},
	{ 34677, 0x0, "SGILOG24", NULL},
	{ 34712, 0x0, "JPEG2000", NULL},
	{ 34715, 0x0, "JBIG2", NULL},
	{ 34887, 0x0, "LERC", NULL},
	{ 34892, 0x0, "Lossy JPEG(DNG)", NULL},
	{ 34925, 0x0, "LZMA2", NULL},
	{ 50000, 0x0, "Zstd", NULL},
	{ 50001, 0x0, "WebP", NULL}
};

static const struct cmpr_meth_info *find_cmpr_meth_info(u32 id)
{
	size_t k;

	for(k=0; k<DE_ARRAYCOUNT(cmpr_meth_info_arr); k++) {
		if(id == cmpr_meth_info_arr[k].id)
			return &cmpr_meth_info_arr[k];
	}
	return NULL;
}

static int decompress_strile(deark *c, lctx *d, struct page_ctx *pg,
	struct decode_page_ctx *dctx, UI plane_idx)
{
	int retval = 0;

	// TODO: Some of our decompressors have significant overhead. This can
	// be a performance issue with TIFF format, which likes to chop up an
	// image into lots of tiny pieces, each of which is compressed
	// independently.
	// We ought to have a way to efficiently reset a decompressor, without
	// having to recreate it entirely.

	dctx->dcmpri.pos = pg->strile_data[dctx->strile_idx].pos;
	dctx->dcmpri.len = pg->strile_data[dctx->strile_idx].len;

	if(dctx->dcmpri.pos + dctx->dcmpri.len > dctx->dcmpri.f->len) {
		dctx->dcmpri.len = dctx->dcmpri.f->len - dctx->dcmpri.pos;
	}
	if(dctx->dcmpri.len<0) dctx->dcmpri.len = 0;
	dctx->dcmpro.f = dctx->unc_strile_dbuf[plane_idx];
	dctx->dcmpro.len_known = 1;
	dctx->dcmpro.expected_len = dctx->strileset_rowspan * dctx->strileset_height;
	de_dfilter_init_objects(c, NULL, NULL, &dctx->dres);

	if(pg->cmi && pg->cmi->decompressor) {
		pg->cmi->decompressor(c, d, pg, dctx);
	}
	else {
		decompress_strile_uncmpr(c, d, pg, dctx);
	}
	dbuf_flush(dctx->dcmpro.f);

	if(dctx->dres.errcode) {
		detiff_err(c, d, pg, "Decompression failed (strip@%d,%d): %s",
			(int)dctx->strileset_xpos, (int)dctx->strileset_ypos,
			de_dfilter_get_errmsg(c, &dctx->dres));
		// TODO?: Better handling of partial failure
		goto done;
	}

	if(dctx->need_to_reverse_nibbles) {
		reverse_nibbles_in_membuf(dctx->dcmpro.f);
	}

	retval = 1;
done:
	return retval;
}

static int decompress_strileset(deark *c, lctx *d, struct page_ctx *pg,
	struct decode_page_ctx *dctx)
{
	UI sample_idx;
	int retval = 0;

	for(sample_idx=0; sample_idx<dctx->num_planes_to_decode; sample_idx++) {
		dctx->strile_idx = dctx->strileset_idx +(i64)sample_idx * dctx->striles_per_plane;
		retval = decompress_strile(c, d, pg, dctx, sample_idx);
		if(!retval) goto done;
	}
	retval = 1;
done:
	return retval;
}

static void paint_decompressed_strile_to_image(deark *c, lctx *d, struct page_ctx *pg,
	struct decode_page_ctx *dctx, de_bitmap *img, de_bitmap *img_lo)
{
	i64 i, j;
	u32 s_idx;
	u32 sample_mask;
	u32 sample_scalefactor;
	u32 prev_sample[DE_TIFF_MAX_SAMPLES];
	u32 sample[DE_TIFF_MAX_SAMPLES];
	u8 sample_lo[DE_TIFF_MAX_SAMPLES];
	de_color clr_lo = 0;

	de_zeromem(&prev_sample, sizeof(prev_sample));
	de_zeromem(&sample, sizeof(sample));
	de_zeromem(&sample_lo, sizeof(sample_lo));

	sample_mask = (1U<<(u32)pg->bits_per_sample)-1U;
	if(pg->bits_per_sample<=8) {
		sample_scalefactor = 255/sample_mask;
	}
	else {
		sample_scalefactor = 1;
	}

	for(j=0; j<dctx->strileset_height; j++) {

		for(s_idx=0; s_idx<dctx->samples_per_pixel_to_decode; s_idx++) {
			prev_sample[s_idx] = 0;
		}

		for(i=0; i<dctx->strileset_width; i++) {
			de_color clr;

			for(s_idx=0; s_idx<dctx->samples_per_pixel_to_decode; s_idx++) {
				UI dbuf_idx;
				i64 sample_offset;
				i64 bytepos;

				if(dctx->is_separated) {
					dbuf_idx = s_idx;
					sample_offset = 0;
				}
				else {
					dbuf_idx = 0;
					sample_offset = (i64)s_idx;
				}

				switch(pg->bits_per_sample) {
				case 1: case 2: case 4:
					sample[s_idx] = de_get_bits_symbol(dctx->unc_strile_dbuf[dbuf_idx], (i64)pg->bits_per_sample,
						dctx->strileset_rowspan*j, i*(i64)dctx->samples_per_pixel_per_plane + sample_offset);
					break;
				case 8:
					bytepos = dctx->strileset_rowspan*j + i*(i64)dctx->samples_per_pixel_per_plane + sample_offset;
					sample[s_idx] = dbuf_getbyte(dctx->unc_strile_dbuf[dbuf_idx], bytepos);
					break;
				case 16:
					bytepos = dctx->strileset_rowspan*j + (i*(i64)dctx->samples_per_pixel_per_plane + sample_offset)*2;
					// Byte order for 16-bit samples is dictated by the TIFF file header -- at least
					// for uncompressed images.
					sample[s_idx] = (u32)dbuf_getu16x(dctx->unc_strile_dbuf[dbuf_idx], bytepos, d->is_le);
					break;
				default:
					sample[s_idx] = 0;
				}
			}

			if(pg->predictor==2) {
				for(s_idx=0; s_idx<dctx->samples_per_pixel_to_decode; s_idx++) {
					sample[s_idx] = (sample[s_idx] + prev_sample[s_idx]) & sample_mask;
					prev_sample[s_idx] = sample[s_idx];
				}
			}

			if(!dctx->use_pal && sample_scalefactor>1) { // Scale samples to 255
				for(s_idx=0; s_idx<dctx->samples_per_pixel_to_decode; s_idx++) {
					sample[s_idx] *= sample_scalefactor;
				}
			}

			if(dctx->grayscale_reverse_polarity) {
				if(pg->bits_per_sample==16)
					sample[0] = 0xffff - sample[0];
				else
					sample[0] = 0xff - sample[0];
			}

			if(pg->bits_per_sample==16) {
				for(s_idx=0; s_idx<dctx->samples_per_pixel_to_decode; s_idx++) {
					sample_lo[s_idx] = sample[s_idx] & 0xff;
					sample[s_idx] >>= 8;
				}
			}

			if(dctx->use_pal) {
				UI palidx;

				if(pg->is_dexxa && pg->dexxa_ncolors==16) {
					palidx = (sample[3]<<3) | (sample[2]<<2) | (sample[1]<<1) | sample[0];
					if(pg->compression==CMPR_CCITTRLE) {
						palidx = 15 - palidx;
					}
				}
				else {
					palidx = sample[0];
				}
				clr = pg->pal[palidx & 0xff];
			}
			else if(dctx->is_grayscale) {
				clr = DE_MAKE_GRAY(sample[0]);
				if(dctx->dst_flag_16bit) {
					clr_lo = DE_MAKE_GRAY(sample_lo[0]);
				}
			}
			else {
				clr = DE_MAKE_RGB(sample[0], sample[1], sample[2]);
			}

			if(dctx->has_alpha) {
				clr = DE_SET_ALPHA(clr, sample[dctx->alpha_sample_idx]);
				if(dctx->is_assoc_alpha) {
					clr = de_unpremultiply_alpha_clr(clr);
				}
			}

			de_bitmap_setpixel_rgba(img, dctx->strileset_xpos+i, dctx->strileset_ypos+j, clr);
			if(dctx->dst_flag_16bit) {
				de_bitmap_setpixel_rgba(img_lo, dctx->strileset_xpos+i, dctx->strileset_ypos+j, clr_lo);
			}
		}
	}
}

static void set_image_density(deark *c, lctx *d, struct page_ctx *pg, de_finfo *fi)
{
	if(!pg->have_density) return;

	if(pg->orientation>=5 && pg->orientation<=8) {
		// For some orientations, the x dimension is the ImageLength dimension.
		fi->density.xdens = pg->density_ImageLength;
		fi->density.ydens = pg->density_ImageWidth;
	}
	else {
		fi->density.xdens = pg->density_ImageWidth;
		fi->density.ydens = pg->density_ImageLength;
	}
	if(pg->resolution_unit==1) {
		fi->density.code = DE_DENSITY_UNK_UNITS;
	}
	else if(pg->resolution_unit==2) {
		fi->density.code = DE_DENSITY_DPI;
	}
	else if(pg->resolution_unit==3) { // pixels/cm
		fi->density.xdens *= 2.54;
		fi->density.ydens *= 2.54;
		fi->density.code = DE_DENSITY_DPI;
	}
}

// We can extract NewJPEG images if the following conditions are true:
// - There is just one strile.
// - The image does not use separate JPEG tables. Presumably this corresponds to the
//   absence of a JPEGTables tag.
// (TODO: Reconstitute single-strile images that use separate tables.)
static void do_newjpeg(deark *c, lctx *d, struct page_ctx *pg)
{
	if(pg->strile_count!=1) goto done;
	if(pg->strile_data[0].pos + pg->strile_data[0].len > c->infile->len) goto done;
	// TODO: Tiled images might include extra pixels for padding, which we ought to warn
	// about. But we would have to scan the JPEG data to know for sure.

	dbuf_create_file_from_slice(c->infile, pg->strile_data[0].pos, pg->strile_data[0].len,
		"jpg", NULL, 0);

done:
	;
}

// Decide whether to try to decode this IFD as an image.
// A return value of 1 doesn't guarantee that it contains an image, but means
// we can report an error if it turns out not to.
static int is_image_ifd(deark *c, lctx *d, struct page_ctx *pg)
{
	if(d->fmt==DE_TIFFFMT_JPEGXR) {
		return pg->have_jxr_imageoffset;
	}

	if(!pg->have_imagewidth || pg->imagewidth<1 || pg->imagelength<1) {
		return 0;
	}

	// The Exif spec says of ImageWidth: "In JPEG compressed data, this tag
	// shall not be used because a JPEG marker is used instead".
	// However, a fair number of JPEG/Exif files use it anyway (in IFD#0).
	if(d->is_exif_submodule && pg->ifd_idx_in_main_list==0) {
		if(!pg->have_strip_tags && !pg->have_tile_tags && pg->jpegoffset==0) {
			return 0;
		}
	}

	return 1;
}

// A strange broken planar paletted format used by Paint It! (presumably) and
// RainBow Paint by Dexxa, and PaintShow [Plus] by Logitech.
static void detect_and_setup_dexxa(deark *c, lctx *d, struct page_ctx *pg)
{
	if(d->opt_dexxa==0) goto done; // Disabled by user
	if(pg->extrasamples_count!=0 || !pg->have_oldsubfiletype) goto done;
	if(pg->compression!=CMPR_NONE && pg->compression!=CMPR_PACKBITS &&
		pg->compression!=CMPR_CCITTRLE)
	{
		goto done;
	}

	if(pg->planarconfig==2 && pg->samples_per_pixel==4 &&
		(pg->bits_per_sample==1 || pg->bits_per_sample==4) &&
		pg->photometric==2 && pg->have_strip_tags && pg->valcount_bitspersample==1)
	{
		if(d->opt_dexxa==0xff) {
			de_dbg(c, "detected Dexxa variant");
		}
		pg->is_dexxa = 1;
		pg->dexxa_ncolors = 16;
		pg->bits_per_sample = 1; // [E.g. TRAIN1.TIF from PaintShow 1.1]
		goto done;
	}

	if(d->opt_dexxa==0xff) goto done; // No autodetection below this line

	if(pg->planarconfig==1 && pg->samples_per_pixel==1 &&
		pg->photometric==1 && pg->bits_per_sample==2 &&
		pg->compression!=CMPR_CCITTRLE)
	{
		// TODO?: Try to support 4-color CMPR_CCITTRLE. The images are at
		// least partially recoverable, but even the software that created them
		// can't read them back.
		pg->is_dexxa = 1;
		pg->dexxa_ncolors = 4;
		goto done;
	}

done:
	if(pg->is_dexxa) {
		de_dbg(c, "using Dexxa mode");
	}
}

static void preprocess_fillorder_lsb(deark *c, lctx *d, struct page_ctx *pg,
	struct decode_page_ctx *dctx)
{
	if(pg->fill_order!=2) return;

	// FillOrder=LSB is ambiguous in general, but we allow it in some special cases.
	if(pg->compression==CMPR_NONE && pg->samples_per_pixel==1 && pg->bits_per_sample==1) {
		dctx->need_to_reverse_bits = 1;
		return;
	}

	if((pg->compression>=2 && pg->compression<=4) || pg->compression==32771) {
		return; // Fax compression - LSB is ok.
	}
	if(pg->compression==CMPR_LZW && pg->is_old_lzw) {
		// We detected that the LZW data in this image uses LSB bit order, so
		// this is probably ok.
		return;
	}

	if(pg->samples_per_pixel==1 && pg->bits_per_sample==4 &&
		(pg->compression==CMPR_NONE || pg->compression==CMPR_LZW))
	{
		// Many examples in the "FM Towns: Free Software Collection" items at
		// <http://discmaster.textfiles.com/cd-rom/>.

		detiff_warn(c, d, pg, "Ambiguous FillOrder for this image type");
		if(d->fillorder_strategy==0 || d->fillorder_strategy==2) {
			dctx->need_to_reverse_nibbles = 1;
		}
		return;
	}

	detiff_warn(c, d, pg, "Unexpected FillOrder for this image type. Ignoring.");
}

static void do_process_ifd_image(deark *c, lctx *d, struct page_ctx *pg)
{
	de_bitmap *img = NULL;
	de_bitmap *img_lo = NULL;
	struct decode_page_ctx *dctx = NULL;
	de_finfo *fi = NULL;
	int need_errmsg = 0;
	int saved_indent_level;
	int saved_indent_level2;
	i64 i;
	i64 striles_across;
	int output_bypp; // bytes/pixel for the output de_bitmap object(s)
	int ok_bps = 0;
	UI createflags = 0;
	// (Multiple dbufs are needed for PlanarConfig=separated.)
	dbuf *tmp_membuf[DE_TIFF_MAX_SAMPLES];

	de_dbg_indent_save(c, &saved_indent_level);
	dctx = de_malloc(c, sizeof(struct decode_page_ctx));
	de_zeromem(tmp_membuf, sizeof(tmp_membuf));

	// TODO: Should we check pg->compression?
	if(pg->jpegoffset>0) {
		do_oldjpeg(c, d, pg);
		goto done;
	}

	if(pg->compression==CMPR_NEWJPEG && pg->strile_count==1 && !pg->have_jpegtables) {
		// FIXME: This should really happen *after* some of the processing below,
		// but it's complicated.
		do_newjpeg(c, d, pg);
		goto done;
	}

	if(!is_image_ifd(c, d, pg)) {
		de_dbg(c, "[non-image IFD]");
		goto done;
	}

	if(pg->have_jxr_imageoffset) {
		goto done; // JPEG XR images are not supported
	}

	if(!d->opt_decode) goto done;
	de_dbg(c, "decoding ifd image");
	de_dbg_indent(c, 1);

	detect_and_setup_dexxa(c, d, pg);

	if(pg->compression<1) {
		pg->compression = CMPR_NONE;
	}
	pg->cmi = find_cmpr_meth_info(pg->compression);

	if(pg->samples_per_pixel<1) {
		pg->samples_per_pixel = 1;
	}
	if(pg->bits_per_sample<1) {
		pg->bits_per_sample = 1;
	}
	if(pg->predictor<1) {
		pg->predictor = 1;
	}
	if(pg->sample_format<1) {
		pg->sample_format = 1;
	}
	if(pg->resolution_unit<1) {
		pg->resolution_unit = 2;
	}
	if(pg->rows_per_strip<1 || pg->rows_per_strip>pg->imagelength) {
		pg->rows_per_strip = pg->imagelength;
	}

	if(pg->planarconfig<=1 || pg->samples_per_pixel==1) {
		dctx->is_separated = 0;
	}
	else if(pg->planarconfig==2) {
		dctx->is_separated = 1;
	}
	else {
		need_errmsg = 1; goto done;
	}

	dctx->width = pg->imagewidth; // tentative dimensions
	dctx->height = pg->imagelength;
	if(!de_good_image_dimensions(c, dctx->width, dctx->height)) goto done;

	if(!pg->cmi || !pg->cmi->decompressor) {
		detiff_err(c, d, pg, "Unsupported compression method: %d (%s)", (int)pg->compression,
			((pg->cmi && pg->cmi->name)?pg->cmi->name:"?"));
		goto done;
	}

	if(pg->have_tile_tags) {
		if(pg->have_strip_tags) {
			detiff_warn(c, d, pg, "Image seems to have both strips and tiles");
		}
		dctx->is_tiled = 1;
	}

	if(pg->strile_count<1) {
		detiff_err(c, d, pg, "No image data found");
		goto done;
	}
	if((pg->strile_count==1 && pg->strile_data[0].len==0) ||
		(pg->strile_count>1 && !pg->have_strilebytecounts))
	{
		i64 k;

		// Tolerate missing StripByteCounts tag in some cases
		for(k=0; k<pg->strile_count; k++) {
			if(k<pg->strile_count-1) {
				pg->strile_data[k].len = pg->strile_data[k+1].pos - pg->strile_data[k].pos;
			}
			else {
				pg->strile_data[k].len = c->infile->len - pg->strile_data[k].pos;
			}
			if(pg->strile_data[k].len<0) pg->strile_data[k].len = 0;
		}
	}

	if(pg->sample_format!=1) {
		detiff_err(c, d, pg, "Unsupported sample format (%d)", (int)pg->sample_format);
		goto done;
	}

	if(pg->is_dexxa && pg->dexxa_ncolors==16) {
		dctx->base_samples_per_pixel = 4;
		ok_bps = 1;
		if(!pg->have_colormap) {
			de_copy_std_palette(DE_PALID_PC16, 0, 0, pg->pal, 16, 0);
		}
		dctx->use_pal = 1;
	}
	else if(pg->is_dexxa && pg->dexxa_ncolors==4) {
		dctx->base_samples_per_pixel = 1;
		ok_bps = 1;
		if(!pg->have_colormap) {
			de_copy_std_palette(DE_PALID_CGA, 3, 0, pg->pal, 4, 0);
		}
		dctx->use_pal = 1;
	}
	else if(pg->photometric==0 || pg->photometric==1) {
		dctx->is_grayscale = 1;
		dctx->grayscale_reverse_polarity = (pg->photometric==0);
		dctx->base_samples_per_pixel = 1;
		if(pg->bits_per_sample==1 || pg->bits_per_sample==2 || pg->bits_per_sample==4 ||
			pg->bits_per_sample==8 || pg->bits_per_sample==16)
		{
			ok_bps = 1;
		}
	}
	else if(pg->photometric==2) { // RGB
		dctx->base_samples_per_pixel = 3;
		if(pg->bits_per_sample==1 || pg->bits_per_sample==2 || pg->bits_per_sample==4 ||
			pg->bits_per_sample==8 || pg->bits_per_sample==16)
		{
			ok_bps = 1;
		}
	}
	else if(pg->photometric==3) { // paletted
		dctx->base_samples_per_pixel = 1;
		dctx->use_pal = 1;
		if(pg->bits_per_sample==1 || pg->bits_per_sample==2 || pg->bits_per_sample==4 ||
			pg->bits_per_sample==8)
		{
			ok_bps = 1;
		}
	}
	else {
		detiff_err(c, d, pg, "Unsupported color type: %d (%s)", (int)pg->photometric,
			lookup_str(photometric_name_map, DE_ARRAYCOUNT(photometric_name_map), (i64)pg->photometric));
		goto done;
	}

	if(!ok_bps) {
		detiff_err(c, d, pg, "Unsupported bits/sample (%d) for this color type", (int)pg->bits_per_sample);
		goto done;
	}

	if(pg->samples_per_pixel<1) {
		pg->samples_per_pixel = dctx->base_samples_per_pixel;
	}
	if(pg->samples_per_pixel < dctx->base_samples_per_pixel) {
		detiff_err(c, d, pg, "Bad samples/pixel (%d) for this color type", (int)pg->samples_per_pixel);
		goto done;
	}
	dctx->samples_per_pixel_to_decode = pg->samples_per_pixel;
	if(dctx->samples_per_pixel_to_decode>DE_TIFF_MAX_SAMPLES) {
		dctx->samples_per_pixel_to_decode = DE_TIFF_MAX_SAMPLES;
	}

	if(pg->predictor!=1 && pg->predictor!=2) {
		detiff_err(c, d, pg, "Unsupported prediction method (%d)", (int)pg->predictor);
		goto done;
	}

	if(pg->compression==CMPR_LZW) {
		// Looking ahead to detect this may help to decipher the FillOrder tag.
		detect_lzw_version(c, d, pg);
	}

	if(pg->fill_order==2) {
		preprocess_fillorder_lsb(c, d, pg, dctx);
	}

	// TODO: Support -padpix in more situations
	if(c->padpix && !dctx->is_tiled && !dctx->is_separated && pg->compression==CMPR_NONE) {
		if(pg->samples_per_pixel==1 && pg->bits_per_sample<8) {
			dctx->width = de_pad_to_n(dctx->width * pg->samples_per_pixel * pg->bits_per_sample, 8) /
				((i64)pg->samples_per_pixel * pg->bits_per_sample);
		}
	}

	// Look for an alpha sample
	for(i=0; i<(i64)pg->extrasamples_count; i++) {
		if((i64)dctx->base_samples_per_pixel + i >= (i64)dctx->samples_per_pixel_to_decode) break;
		if(pg->extrasample_type[i]==1) { // Assoc. alpha
			dctx->has_alpha = 1;
			dctx->is_assoc_alpha = 1;
			dctx->alpha_sample_idx = (UI)dctx->base_samples_per_pixel + (UI)i;
			break;
		}
		else if(pg->extrasample_type[i]==2) { // Unassoc. alpha
			dctx->has_alpha = 1;
			dctx->is_assoc_alpha = 0;
			dctx->alpha_sample_idx = (UI)dctx->base_samples_per_pixel + (UI)i;
			break;
		}
	}

	if(dctx->has_alpha) {
		// We will read and low-level decode all samples up to and including the
		// first alpha sample, though some may end up being ignored.
		dctx->samples_per_pixel_to_decode = (UI)dctx->alpha_sample_idx+1;
	}
	else {
		dctx->samples_per_pixel_to_decode = dctx->base_samples_per_pixel;
	}

	{
		UI num_samples_respected;

		num_samples_respected = dctx->base_samples_per_pixel +
			(dctx->has_alpha ? 1 : 0);
		if(num_samples_respected < pg->samples_per_pixel) {
			detiff_warn(c, d, pg, "Only using %u of %u samples. This image "
				"may not be fully supported.",
				(UI)num_samples_respected, (UI)pg->samples_per_pixel);
		}
	}

	if(dctx->is_separated) {
		dctx->striles_per_plane = pg->strile_count / pg->samples_per_pixel;
		dctx->num_planes_to_decode = dctx->samples_per_pixel_to_decode;
		dctx->samples_per_pixel_per_plane = 1;
	}
	else {
		dctx->striles_per_plane = pg->strile_count;
		dctx->num_planes_to_decode = 1;
		dctx->samples_per_pixel_per_plane = pg->samples_per_pixel;
	}

	if(dctx->is_tiled) {
		i64 striles_down;

		if(pg->tile_width<1 || pg->tile_height<1 || pg->tile_width>c->max_image_dimension ||
			pg->tile_height>c->max_image_dimension)
		{
			need_errmsg = 1; goto done;
		}
		dctx->strile_max_w = pg->tile_width;
		dctx->strile_max_h = pg->tile_height;
		striles_across = (dctx->width + pg->tile_width - 1) / pg->tile_width;
		striles_down = (dctx->height + pg->tile_height - 1) / pg->tile_height;

		if(c->padpix) {
			dctx->width = striles_across * pg->tile_width;
			dctx->height = striles_down * pg->tile_height;
		}
	}
	else {
		dctx->strile_max_w = dctx->width;
		dctx->strile_max_h = pg->rows_per_strip;
		striles_across = 1;
	}
	dctx->strile_max_rowspan = (dctx->strile_max_w * (i64)dctx->samples_per_pixel_per_plane *
		pg->bits_per_sample + 7)/8;

	de_dfilter_init_objects(c, &dctx->dcmpri, &dctx->dcmpro, NULL);
	dctx->dcmpri.f = c->infile;

	if(dctx->is_grayscale) output_bypp = 1;
	else output_bypp = 3;
	if(dctx->has_alpha) output_bypp++;

	if(dctx->is_grayscale && output_bypp==1 && pg->bits_per_sample==16) {
		// In some limited cases, we fully support depth=16bit.
		dctx->dst_flag_16bit = 1;
	}

	img = de_bitmap_create(c, dctx->width, dctx->height, output_bypp);
	if(dctx->dst_flag_16bit) {
		img_lo = de_bitmap_create(c, dctx->width, dctx->height, output_bypp);
	}

	de_dbg_indent_save(c, &saved_indent_level2);

	for(dctx->strileset_idx=0; dctx->strileset_idx<dctx->striles_per_plane; dctx->strileset_idx++) {
		int ret;
		i64 first_strile_idx;

		first_strile_idx = dctx->strileset_idx;
		dctx->strileset_xpos = (dctx->strileset_idx % striles_across) * dctx->strile_max_w;
		dctx->strileset_ypos = (dctx->strileset_idx / striles_across) * dctx->strile_max_h;
		if(dctx->strileset_ypos >= dctx->height) goto after_paint;

		de_dbg2(c, "strip #%d (%d,%d) at %"I64_FMT"%s, dlen=%"I64_FMT,
			(int)dctx->strileset_idx, (int)dctx->strileset_xpos, (int)dctx->strileset_ypos,
			pg->strile_data[first_strile_idx].pos, (dctx->is_separated?"...":""),
			pg->strile_data[first_strile_idx].len);

		dctx->strileset_width = dctx->strile_max_w;
		if(dctx->is_tiled) { // Tiles are all the same width and height.
			dctx->strileset_height = dctx->strile_max_h;
		}
		else { // The last strip may have a smaller height.
			dctx->strileset_height = de_min_int(dctx->strile_max_h, dctx->height - dctx->strileset_ypos);
		}
		dctx->strileset_rowspan = (dctx->strileset_width * (i64)dctx->samples_per_pixel_per_plane *
			pg->bits_per_sample + 7)/8;
		if(dctx->strileset_height<1) goto after_paint;

		{
			UI k;

			for(k=0; k<dctx->num_planes_to_decode; k++) {
				if(tmp_membuf[k]) {
					dbuf_empty(tmp_membuf[k]);
				}
				else {
					tmp_membuf[k] = dbuf_create_membuf(c, 0, 0);
					dbuf_enable_wbuffer(tmp_membuf[k]);
					dbuf_set_length_limit(tmp_membuf[k], dctx->strile_max_h * dctx->strile_max_rowspan);
				}
				dctx->unc_strile_dbuf[k] = tmp_membuf[k];
			}

			ret = decompress_strileset(c, d, pg, dctx);
			if(!ret) {
				// TODO?: Better handling of partial failure
				if(dctx->strileset_idx>0) goto after_paint;
				goto done;
			}
		}

		paint_decompressed_strile_to_image(c, d, pg, dctx, img, img_lo);
	}

after_paint:
	de_dbg_indent_restore(c, saved_indent_level2);
	fi = de_finfo_create(c);

	if(pg->orientation>=5 && pg->orientation<=8) {
		de_bitmap_transpose(img);
		if(img_lo) de_bitmap_transpose(img_lo);
	}
	if(pg->orientation==2 || pg->orientation==3 || pg->orientation==6 || pg->orientation==7) {
		de_bitmap_mirror(img);
		if(img_lo) de_bitmap_mirror(img_lo);
	}
	if(pg->orientation==3 || pg->orientation==4 || pg->orientation==7 || pg->orientation==8) {
		// More efficient than de_bitmap_flip(). We can do this if flipping is the last step.
		createflags |= DE_CREATEFLAG_FLIP_IMAGE;
	}

	if(pg->is_thumb) {
		createflags |= DE_CREATEFLAG_IS_AUX;
		de_finfo_set_name_from_sz(c, fi, "thumb", 0, DE_ENCODING_LATIN1);
	}

	set_image_density(c, d, pg, fi);
	fi->internal_mod_time = pg->internal_mod_time;

	createflags |= DE_CREATEFLAG_OPT_IMAGE;
	de_bitmap16_write_to_file_finfo(img, img_lo, fi, createflags);

done:
	for(i=0; i<DE_TIFF_MAX_SAMPLES; i++) {
		if(tmp_membuf[i]) dbuf_close(tmp_membuf[i]);
	}
	de_bitmap_destroy(img);
	de_bitmap_destroy(img_lo);
	de_finfo_destroy(c, fi);
	if(need_errmsg) {
		detiff_err(c, d, pg, "Unsupported image type or bad image");
	}
	if(dctx) {
		if(dctx->dfctx) de_dfilter_destroy(dctx->dfctx);
		de_free(c, dctx);
	}
	de_dbg_indent_restore(c, saved_indent_level);
}

// Things to do after reading the tags, and before, e.g., decoding the image.
static void postprocess_ifd_tags(deark *c, lctx *d, struct page_ctx *pg)
{
	i64 i;

	if(pg->ifd_idx==0) {
		d->first_ifd_orientation = pg->orientation;
		d->first_ifd_cosited = (pg->ycbcrpositioning==2);
	}

	if(pg->strile_data) {
		for(i=0; i<pg->strile_count; i++) {
			dbuf_constrain_offset(c->infile, &pg->strile_data[i].pos);
			dbuf_constrain_length(c->infile, pg->strile_data[i].pos,
				&pg->strile_data[i].len);
		}
	}

	dbuf_constrain_offset(c->infile, &pg->jpegoffset);
	dbuf_constrain_length(c->infile, pg->jpegoffset, &pg->jpeglength);
}

static void process_ifd(deark *c, lctx *d, int ifd_idx1, i64 ifdpos1, u8 ifdtype1,
	u8 is_in_main_ifd_list)
{
	struct page_ctx *pg = NULL;
	int num_tags;
	int i;
	i64 tmpoffset;
	de_ucstring *dbgline = NULL;
	struct taginfo tg;
	const char *name;
	static const struct tagnuminfo default_tni = { 0, 0x00, "?", NULL, NULL };

	pg = de_malloc(c, sizeof(struct page_ctx));
	pg->density_ImageWidth = 0.0;
	pg->density_ImageLength = 0.0;
	pg->ifd_idx = ifd_idx1;
	pg->ifdpos = ifdpos1;
	pg->ifdtype = ifdtype1;
	if(is_in_main_ifd_list) {
		pg->ifd_idx_in_main_list = d->num_main_ifds_processed++;
		// Calling the first IFD "#0" is consistent with Exif, though not
		// necessarily with all other TIFF literature.
		de_snprintf(pg->ifdnum_string, sizeof(pg->ifdnum_string), "%d",
			pg->ifd_idx_in_main_list);
	}
	else {
		pg->ifd_idx_in_main_list = -1;
		de_strlcpy(pg->ifdnum_string, "special", sizeof(pg->ifdnum_string));
	}
	de_snprintf(pg->errmsgtoken_ifd, sizeof(pg->errmsgtoken_ifd),
		"IFD#%s@%"I64_FMT, pg->ifdnum_string, pg->ifdpos);

	// NOTE: Some TIFF apps (e.g. Windows Photo Viewer) have been observed to encode
	// ASCII fields (e.g. ImageDescription) in UTF-8, in violation of the TIFF spec.
	// It might be better to give up trying to obey the various specifications, and
	// just try to autodetect the encoding, based on whether it is valid UTF-8, etc.

	if(d->fmt==DE_TIFFFMT_JPEGXR)
		d->current_textfield_encoding = DE_ENCODING_UTF8; // Might be overridden below
	else
		d->current_textfield_encoding = DE_ENCODING_ASCII;

	switch(pg->ifdtype) {
	case IFDTYPE_SUBIFD:
		name=" (SubIFD)";
		break;
	case IFDTYPE_GLOBALPARAMS:
		name=" (Global Parameters IFD)";
		break;
	case IFDTYPE_EXIF:
		name=" (Exif IFD)";
		d->current_textfield_encoding = DE_ENCODING_ASCII;
		break;
	case IFDTYPE_EXIFINTEROP:
		name=" (Exif Interoperability IFD)";
		d->current_textfield_encoding = DE_ENCODING_ASCII;
		break;
	case IFDTYPE_GPS:
		name=" (GPS IFD)";
		d->current_textfield_encoding = DE_ENCODING_ASCII;
		break;
	case IFDTYPE_NIKONPREVIEW:
		name=" (Nikon Preview)";
		break;
	case IFDTYPE_MASKSUBIFD:
		name=" (Mask SubIFD)";
		break;
	default:
		name="";
	}

	de_dbg(c, "IFD#%s at %"I64_FMT"%s", pg->ifdnum_string, pg->ifdpos, name);
	de_dbg_indent(c, 1);

	if(pg->ifdpos >= c->infile->len || pg->ifdpos<8) {
		detiff_warn(c, d, NULL, "Invalid IFD offset: %"I64_FMT, pg->ifdpos);
		goto done;
	}

	if(d->is_bigtiff) {
		num_tags = (int)dbuf_geti64x(c->infile, pg->ifdpos, d->is_le);
	}
	else {
		num_tags = (int)dbuf_getu16x(c->infile, pg->ifdpos, d->is_le);
	}

	de_dbg(c, "number of tags: %d", num_tags);
	if(num_tags>200) {
		detiff_warn(c, d, pg, "Invalid or excessive number of TIFF tags (%d)", num_tags);
		goto done;
	}

	// Record the next IFD in the main list.
	tmpoffset = getfpos(c, d, pg->ifdpos+d->ifdhdrsize+num_tags*d->ifditemsize);
	de_dbg(c, "offset of next IFD: %"I64_FMT"%s", tmpoffset, tmpoffset==0?" (none)":"");
	push_ifd(c, d, tmpoffset, IFDTYPE_NORMAL, 1);

	dbgline = ucstring_create(c);

	for(i=0; i<num_tags; i++) {
		const struct tagnuminfo *tni;

		de_zeromem(&tg, sizeof(struct taginfo));
		tg.pg = pg;

		tg.tagnum = (int)dbuf_getu16x(c->infile, pg->ifdpos+d->ifdhdrsize+i*d->ifditemsize, d->is_le);
		tg.datatype = (int)dbuf_getu16x(c->infile, pg->ifdpos+d->ifdhdrsize+i*d->ifditemsize+2, d->is_le);
		tg.valcount = getvalcount(c, d, pg->ifdpos+d->ifdhdrsize+i*d->ifditemsize+4);
		if(tg.valcount > DE_TIFF_MAX_VALCOUNT) {
			// Must ensure that valcount, even after multiplying by 8 and adding to
			// the file size, does not risk integer overflow.
			tg.valcount = DE_TIFF_MAX_VALCOUNT;
		}

		tg.unit_size = size_of_data_type(tg.datatype);
		tg.total_size = tg.unit_size * tg.valcount;
		if(tg.total_size <= d->offsetsize) {
			tg.val_offset = pg->ifdpos+d->ifdhdrsize+i*d->ifditemsize+d->offsetoffset;
		}
		else {
			tg.val_offset = getfpos(c, d, pg->ifdpos+d->ifdhdrsize+i*d->ifditemsize+d->offsetoffset);
		}

		// Fixup potential bad sizes
		if(tg.val_offset + tg.total_size > c->infile->len + DE_TIFF_ALLOWED_VIRTUAL_BYTES) {
			if(tg.val_offset > c->infile->len) {
				tg.valcount = 0;
				tg.total_size = 0;
			}
			else {
				tg.valcount = (c->infile->len - tg.val_offset)/tg.unit_size;
				tg.total_size = tg.unit_size * tg.valcount;
			}
		}

		tni = find_tagnuminfo(tg.tagnum, d->fmt, pg->ifdtype);
		if(tni) {
			tg.tag_known = 1;
		}
		else {
			tni = &default_tni; // Make sure tni is not NULL.
		}

		ucstring_empty(dbgline);
		ucstring_printf(dbgline, DE_ENCODING_UTF8,
			"tag %d (%s) ty=%d #=%d offs=%" I64_FMT,
			tg.tagnum, tni->tagname,
			tg.datatype, (int)tg.valcount,
			tg.val_offset);

		do_dbg_print_values(c, d, &tg, tni, dbgline);

		// do_dbg_print_values() already tried to limit the line length.
		// The "500+" in the next line is an emergency brake.
		de_dbg(c, "%s", ucstring_getpsz_n(dbgline, 500+DE_DBG_MAX_STRLEN));

		de_dbg_indent(c, 1);

		if(tni->hfn) {
			tni->hfn(c, d, &tg, tni);
		}

		de_dbg_indent(c, -1);
	}

	postprocess_ifd_tags(c, d, pg);

	do_process_ifd_image(c, d, pg);

done:
	de_dbg_indent(c, -1);
	ucstring_destroy(dbgline);
	if(pg) {
		de_free(c, pg->strile_data);
		de_free(c, pg);
	}
}

static void do_tiff(deark *c, lctx *d)
{
	i64 pos;
	i64 ifdoffs;
	int ifd_idx;
	int need_to_read_header = 1;

	pos = 0;

	if(d->fmt==DE_TIFFFMT_APPLEMN) {
		push_ifd(c, d, 14, IFDTYPE_APPLEMN, 1);
		need_to_read_header = 0;
	}
	else if(d->fmt==DE_TIFFFMT_FUJIFILMMN) {
		ifdoffs = getfpos(c, d, 8);
		push_ifd(c, d, ifdoffs, IFDTYPE_FUJIFILMMN, 1);
		need_to_read_header = 0;
	}

	if(need_to_read_header) {
		de_dbg(c, "TIFF%s file header at %"I64_FMT,
			(d->is_exif_submodule?"/Exif":""), pos);
		de_dbg_indent(c, 1);

		de_dbg(c, "byte order: %s-endian", d->is_le?"little":"big");

		// Skip over the signature
		if(d->is_bigtiff) {
			pos += 8;
		}
		else {
			pos += 4;
		}

		// Read the first IFD offset
		ifdoffs = getfpos(c, d, pos);
		de_dbg(c, "offset of first IFD: %d", (int)ifdoffs);
		if(d->fmt==DE_TIFFFMT_NIKONMN) {
			push_ifd(c, d, ifdoffs, IFDTYPE_NIKONMN, 1);
		}
		else {
			push_ifd(c, d, ifdoffs, IFDTYPE_NORMAL, 1);
		}

		de_dbg_indent(c, -1);
	}

	// Process IFDs until we run out of them.
	// ifd_idx tracks how many IFDs we have finished processing, but it's not
	// really meaningful except when it's 0.
	ifd_idx = 0;
	while(1) {
		u8 ifdtype = IFDTYPE_NORMAL;
		u8 is_in_main_ifd_list = 0;

		ifdoffs = pop_ifd(c, d, &ifdtype, &is_in_main_ifd_list);
		if(ifdoffs==0) break;
		process_ifd(c, d, ifd_idx, ifdoffs, ifdtype, is_in_main_ifd_list);
		ifd_idx++;
	}

	de_dbg(c, "number of IFDs: %d (%d in main list)", d->ifd_count, d->num_main_ifds_processed);
}

static int de_identify_tiff_internal(deark *c, int *is_le)
{
	i64 byte_order_sig;
	i64 magic;
	int fmt = 0;

	byte_order_sig = de_getu16be(0);
	*is_le = (byte_order_sig == 0x4d4d) ? 0 : 1;

	if(*is_le)
		magic = de_getu16le(2);
	else
		magic = de_getu16be(2);

	if(byte_order_sig==0x4550 && magic==0x002a) {
		fmt = DE_TIFFFMT_MDI;
	}
	else if(byte_order_sig==0x4d4d || byte_order_sig==0x4949) {

		switch(magic) {
		case 0x002a: // Standard TIFF
			fmt = DE_TIFFFMT_TIFF;
			break;
		case 0x002b:
			fmt = DE_TIFFFMT_BIGTIFF;
			break;
		case 0x0055:
			fmt = DE_TIFFFMT_PANASONIC;
			break;

		case 0x01bc: // JPEG-XR
			fmt = DE_TIFFFMT_JPEGXR;
			break;

		//case 0x314e: // NIFF

		case 0x4352:
			fmt = DE_TIFFFMT_DCP;
			break;
		case 0x4f52:
		case 0x5352:
			fmt = DE_TIFFFMT_ORF;
			break;
		}
	}

	return fmt;
}

static void identify_more_formats(deark *c, lctx *d)
{
	u8 buf[20];

	de_read(buf, 8, sizeof(buf));

	// Deark TIFF container formats. See de_fmtutil_handle_iptc() for example.
	if(!de_memcmp(buf, "Deark extracted ", 16)) {
		if(!de_memcmp(&buf[16], "IPTC", 4)) {
			d->is_deark_iptc = 1;
			return;
		}
		if(!de_memcmp(&buf[16], "8BIM", 4)) {
			d->is_deark_8bim = 1;
			return;
		}
	}

	if(!de_memcmp(buf, "XEROX DIFF", 10)) {
		de_dbg(c, "XIFF/XEROX DIFF format detected");
		d->is_xiff = 1;
		return;
	}
	if(!de_memcmp(buf, " eXtended ", 10)) {
		de_dbg(c, "XIFF/eXtended format detected");
		d->is_xiff = 1;
	}
}

static void de_run_tiff(deark *c, de_module_params *mparams)
{
	lctx *d = NULL;

	d = de_malloc(c, sizeof(lctx));

	if(mparams) {
		d->in_params = &mparams->in_params;
	}

	d->opt_decode = (u8)de_get_ext_option_bool(c, "tiff:decode", 1);
	d->opt_dexxa = (u8)de_get_ext_option_bool(c, "tiff:dexxa", 0xff);

	if(de_havemodcode(c, mparams, 'A')) {
		d->fmt = DE_TIFFFMT_APPLEMN;
		d->is_le = 0;
		de_strlcpy(d->errmsgtoken_module, "Apple MakerNote", sizeof(d->errmsgtoken_module));
	}
	else if(de_havemodcode(c, mparams, 'F')) {
		d->fmt = DE_TIFFFMT_FUJIFILMMN;
		d->is_le = 1;
		de_strlcpy(d->errmsgtoken_module, "FujiFilm MakerNote", sizeof(d->errmsgtoken_module));
	}
	else {
		d->fmt = de_identify_tiff_internal(c, &d->is_le);
	}

	if(de_havemodcode(c, mparams, 'N')) {
		de_strlcpy(d->errmsgtoken_module, "Nikon MakerNote", sizeof(d->errmsgtoken_module));
		d->fmt = DE_TIFFFMT_NIKONMN;
	}

	if(de_havemodcode(c, mparams, 'M') && (d->fmt==DE_TIFFFMT_TIFF)) {
		de_strlcpy(d->errmsgtoken_module, "MPF", sizeof(d->errmsgtoken_module));
		d->fmt = DE_TIFFFMT_MPEXT;
	}

	if(de_havemodcode(c, mparams, 'E')) {
		d->is_exif_submodule = 1;
		de_strlcpy(d->errmsgtoken_module, "Exif", sizeof(d->errmsgtoken_module));
	}

	if(d->fmt==DE_TIFFFMT_TIFF) {
		identify_more_formats(c, d);
	}

	switch(d->fmt) {
	case DE_TIFFFMT_TIFF:
		if(d->is_xiff)
			de_declare_fmt(c, "XIFF");
		else
			de_declare_fmt(c, "TIFF");
		break;
	case DE_TIFFFMT_BIGTIFF:
		de_declare_fmt(c, "BigTIFF");
		d->is_bigtiff = 1;
		break;
	case DE_TIFFFMT_PANASONIC:
		de_declare_fmt(c, "Panasonic RAW/RW2");
		break;
	case DE_TIFFFMT_ORF:
		de_declare_fmt(c, "Olympus RAW");
		break;
	case DE_TIFFFMT_DCP:
		de_declare_fmt(c, "DNG Camera Profile");
		break;
	case DE_TIFFFMT_MDI:
		de_declare_fmt(c, "MDI");
		break;
	case DE_TIFFFMT_JPEGXR:
		de_declare_fmt(c, "JPEG XR");
		break;
	}

	if(d->fmt==0) {
		detiff_warn(c, d, NULL, "This is not a known/supported TIFF or TIFF-like format.");
	}

	if(d->fmt==DE_TIFFFMT_TIFF) {
		d->fillorder_strategy = (u8)de_get_ext_option_bool(c, "tiff:fillorderstrategy", 0);
	}

	if(d->is_bigtiff) {
		d->ifdhdrsize = 8;
		d->ifditemsize = 20;
		d->offsetoffset = 12;
		d->offsetsize = 8;
	}
	else {
		d->ifdhdrsize = 2;
		d->ifditemsize = 12;
		d->offsetoffset = 8;
		d->offsetsize = 4;
	}

	d->current_textfield_encoding = DE_ENCODING_ASCII;

	do_tiff(c, d);

	if(mparams) {
		// .out_params.flags:
		//  0x08: has_exif_gps
		//  0x10: first IFD has subsampling=cosited
		//  0x20: uint1 = first IFD's orientation
		//  0x40: uint2 = Exif version
		//  0x80: uint3 = main image count
		if(d->has_exif_gps) {
			mparams->out_params.flags |= 0x08;
		}
		if(d->first_ifd_cosited) {
			mparams->out_params.flags |= 0x10;
		}
		if(d->first_ifd_orientation>0) {
			mparams->out_params.flags |= 0x20;
			mparams->out_params.uint1 = d->first_ifd_orientation;
		}
		if(d->exif_version_as_uint32>0) {
			mparams->out_params.flags |= 0x40;
			mparams->out_params.uint2 = d->exif_version_as_uint32;
		}
		if(d->fmt==DE_TIFFFMT_MPEXT) {
			mparams->out_params.flags |= 0x80;
			mparams->out_params.uint3 = d->mpf_main_image_count;
		}
	}

	if(d) {
		de_free(c, d->ifdstack);
		de_inthashtable_destroy(c, d->ifds_seen);
		de_free(c, d);
	}
}

static int de_identify_tiff(deark *c)
{
	int fmt;
	int is_le;

	fmt = de_identify_tiff_internal(c, &is_le);
	if(fmt!=0) return 100;
	return 0;
}

static void de_help_tiff(deark *c)
{
	de_msg(c, "-opt tiff:decode=<0|1> : Don't/Do attempt to decode images");
	de_msg(c, "-opt tiff:dexxa[=0] : Refer to documentation");
	de_msg(c, "-opt tiff:fillorderstrategy=<0|1|2> : Refer to documentation");
}

void de_module_tiff(deark *c, struct deark_module_info *mi)
{
	mi->id = "tiff";
	mi->desc = "TIFF image";
	mi->run_fn = de_run_tiff;
	mi->identify_fn = de_identify_tiff;
	mi->help_fn = de_help_tiff;
}
