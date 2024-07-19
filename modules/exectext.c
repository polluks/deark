// This file is part of Deark.
// Copyright (C) 2024 Jason Summers
// See the file COPYING for terms of use.

// TXT2COM, etc.

#include <deark-private.h>
#include <deark-fmtutil.h>
DE_DECLARE_MODULE(de_module_txt2com);
DE_DECLARE_MODULE(de_module_show_gmr);
DE_DECLARE_MODULE(de_module_asc2com);
DE_DECLARE_MODULE(de_module_doc2com);

typedef struct localctx_exectext {
	de_encoding input_encoding;
	UI fmtcode;
	u8 opt_encconv;
	u8 errflag;
	u8 need_errmsg;
	u8 found_text;
	u8 is_encrypted;
	i64 tpos;
	i64 tlen;
} lctx;

// dbuf_copy_slice_convert_to_utf8() in HYBRID mode doesn't quite do what
// we want for TXT2COM (etc.), mainly because it treats 0x00 and 0x09 as controls,
// while TXT2COM treats them as graphics.
// Note:
// - Early versions of TXT2COM stop when they see 0x1a, but later versions don't.
//   We behave like later versions.
// - We might not handle an unpaired LF or CR byte exactly like TXT2COM does.
static void txt2comlike_convert_and_write(deark *c, lctx *d, dbuf *outf)
{
	struct de_encconv_state es;
	i64 endpos = d->tpos + d->tlen;
	i64 pos;

	de_encconv_init(&es, DE_EXTENC_MAKE(d->input_encoding, DE_ENCSUBTYPE_PRINTABLE));
	if(c->write_bom) {
		dbuf_write_uchar_as_utf8(outf, 0xfeff);
	}

	pos = d->tpos;
	while(pos < endpos) {
		u8 x;

		x = de_getbyte_p(&pos);
		if(x==10 || x==13) {
			dbuf_writebyte(outf, x);
		}
		else {
			de_rune u;

			u = de_char_to_unicode_ex((i32)x, &es);
			dbuf_write_uchar_as_utf8(outf, u);
		}
	}
}

static void txt2comlike_extract(deark *c, lctx *d)
{
	dbuf *outf = NULL;

	if(d->errflag) goto done;
	if(d->tpos<=0 || d->tlen<=0 || d->tpos+d->tlen>c->infile->len) {
		d->errflag = 1;
		d->need_errmsg = 1;
		goto done;
	}

	outf = dbuf_create_output_file(c, "txt", NULL, 0);
	dbuf_enable_wbuffer(outf);
	if(d->opt_encconv) {
		txt2comlike_convert_and_write(c, d, outf);
	}
	else {
		dbuf_copy(c->infile, d->tpos, d->tlen, outf);
	}

done:
	dbuf_close(outf);
}

static void txt2com_read_textpos(deark *c, lctx *d, i64 pos1)
{
	i64 pos = pos1;
	i64 pos_of_tlen;

	de_dbg(c, "pos of tlen pointer: %"I64_FMT, pos1);

	pos_of_tlen = de_getu16le_p(&pos) - 256;
	de_dbg(c, "pos of tlen: %"I64_FMT, pos_of_tlen);

	pos += 2;

	d->tpos = de_getu16le_p(&pos) - 256;
	de_dbg(c, "tpos: %"I64_FMT, d->tpos);

	d->tlen = de_getu16le(pos_of_tlen);
	de_dbg(c, "tlen: %"I64_FMT, d->tlen);
}

// For all TXT2COM versions, and TXT2RES v1.0.
static void txt2com_search1(deark *c, lctx *d)
{
#define TXT2COM_BUF_POS1 700
#define TXT2COM_BUF_LEN1 3000
	u8 *mem = NULL;
	i64 foundpos;
	int ret;

	mem = de_malloc(c, TXT2COM_BUF_LEN1);
	de_read(mem, TXT2COM_BUF_POS1, TXT2COM_BUF_LEN1);
	ret = de_memsearch_match(mem, TXT2COM_BUF_LEN1,
		(const u8*)"\x8b\xd8\xb4\x40\x8b\x0e??\x8d\x16??\xcd\x21\xb4\x3e", 16,
		'?', &foundpos);
	if(!ret) goto done;
	d->found_text = 1;
	txt2com_read_textpos(c, d, TXT2COM_BUF_POS1+foundpos+6);

done:
	de_free(c, mem);
}

// For:
// * TXT2RES v2.03 (= code variant 1)
// * TXT2RES v2.06 (= code variant 1)
// * TXT2RES v2.10 (= code variant 1)
// * TXT2PAS v2.03 (= code variant 2)
// * TXT2PAS v2.06 (= code variant 3)
// * TXT2PAS v2.10 (= code variant 3)
// The code variants have enough common bytes that we try to get away with
// only doing a single search.
static void txt2com_search2(deark *c, lctx *d)
{
#define TXT2COM_BUF_POS2 7500
#define TXT2COM_BUF_LEN2 4000
	u8 *mem = NULL;
	i64 foundpos;
	int ret;

	mem = de_malloc(c, TXT2COM_BUF_LEN2);
	de_read(mem, TXT2COM_BUF_POS2, TXT2COM_BUF_LEN2);
	ret = de_memsearch_match(mem, TXT2COM_BUF_LEN2,
		(const u8*)"\xcd?\xa1??\xd1\xe0\x03\x06??\x8d???\x03", 16,
		'?', &foundpos);
	if(!ret) goto done;
	d->found_text = 1;
	txt2com_read_textpos(c, d, TXT2COM_BUF_POS2+foundpos+9);

done:
	de_free(c, mem);
}

static void destroy_lctx(deark *c, lctx *d)
{
	if(!d) return;
	de_free(c, d);
}

static void de_run_txt2com(deark *c, de_module_params *mparams)
{
	lctx *d = NULL;

	d = de_malloc(c, sizeof(lctx));
	d->input_encoding = de_get_input_encoding(c, NULL, DE_ENCODING_CP437);
	d->opt_encconv = (u8)de_get_ext_option_bool(c, "text:encconv", 1);
	if(d->input_encoding==DE_ENCODING_ASCII) {
		d->opt_encconv = 0;
	}
	de_declare_fmt(c, "TXT2COM");

	txt2com_search1(c, d);
	if(!d->found_text) {
		txt2com_search2(c, d);
	}
	if(!d->found_text) {
		d->need_errmsg = 1;
		goto done;
	}
	if(d->errflag) goto done;

	txt2comlike_extract(c, d);

done:
	if(d) {
		if(d->need_errmsg) {
			de_err(c, "Not a TXT2COM file, or unsupported version");
		}
		destroy_lctx(c, d);
	}
}

static int de_identify_txt2com(deark *c)
{
	u8 b1;
	u8 flag = 0;
	u8 buf[28];
	const char *ids[3] = {"TXT2COM C", "TXT2RES C", "TXT2PAS C"};

	if(c->infile->len>65280) return 0;
	b1 = de_getbyte(0);
	if(b1!=0x8d && b1!=0xe8 && b1!=0xe9) return 0;
	de_read(buf, 0, sizeof(buf));
	if(b1==0x8d) {
		if(!de_memcmp(&buf[14], ids[0], 9)) flag = 1;
	}
	else if(b1==0xe8) {
		if(!de_memcmp(&buf[5], ids[0], 9)) flag = 1;
	}
	else if(b1==0xe9) {
		if(!de_memcmp(&buf[3], ids[0], 9)) flag = 1;
		else if(!de_memcmp(&buf[3], ids[1], 9)) flag = 1;
		else if(!de_memcmp(&buf[3], ids[2], 9)) flag = 1;
	}
	return flag ? 92 : 0;
}

static void print_encconv_option(deark *c)
{
	de_msg(c, "-opt text:encconv=0 : Don't convert to UTF-8");
}

static void de_help_txt2com(deark *c)
{
	print_encconv_option(c);
}

void de_module_txt2com(deark *c, struct deark_module_info *mi)
{
	mi->id = "txt2com";
	mi->desc = "TXT2COM (K. P. Graham)";
	mi->run_fn = de_run_txt2com;
	mi->identify_fn = de_identify_txt2com;
	mi->help_fn = de_help_txt2com;
}

///////////////////////////////////////////////////
// SHOW (Gary M. Raymond, Simple Software)

// Finding the text in a precise way seems difficult.
// Instead, we search for the byte pattern that appears right before the start
// of the text.
// The text *length* does not seem to be present in the file at all. The text
// just ends at the 0x1a byte that should be at the end of the file.
static void showgmr_search(deark *c, lctx *d)
{
#define SHOW_BUF_POS1 1800
#define SHOW_BUF_LEN1 1200
	u8 *mem = NULL;
	i64 foundpos;
	int ret;

	mem = de_malloc(c, SHOW_BUF_LEN1);
	de_read(mem, SHOW_BUF_POS1, SHOW_BUF_LEN1);

	// v2.0, 2.0A, 2.1(?)
	ret = de_memsearch_match(mem, SHOW_BUF_LEN1,
		(const u8*)"\x06?\x03\x19\xa1\x6c\x00\x3b\x06?\x03\x72\xf7\x58\x1f\xc3", 16,
		'?', &foundpos);
	if(ret) {
		d->found_text = 1;
		d->tpos = SHOW_BUF_POS1+foundpos+16;
		goto done;
	}

	// v1.0, 1.4
	ret = de_memsearch_match(mem, SHOW_BUF_LEN1,
		(const u8*)"\x4e\x8a\x04\x3c\x0a\x75\xf9\x4d\x75\xf5\x46\x89\x36\xc2\x02\xc3", 16,
		'?', &foundpos);
	if(ret) {
		d->found_text = 1;
		d->tpos = SHOW_BUF_POS1+foundpos+16;
		goto done;
	}

done:
	de_free(c, mem);
}

static void de_run_show_gmr(deark *c, de_module_params *mparams)
{
	lctx *d = NULL;

	d = de_malloc(c, sizeof(lctx));
	d->input_encoding = de_get_input_encoding(c, NULL, DE_ENCODING_CP437);
	d->opt_encconv = (u8)de_get_ext_option_bool(c, "text:encconv", 1);
	if(d->input_encoding==DE_ENCODING_ASCII) {
		d->opt_encconv = 0;
	}
	de_declare_fmt(c, "SHOW (executable text)");

	showgmr_search(c, d);
	if(!d->found_text) {
		d->need_errmsg = 1;
		goto done;
	}
	de_dbg(c, "tpos: %"I64_FMT, d->tpos);

	d->tlen = c->infile->len - d->tpos;
	if(de_getbyte(c->infile->len-1) == 0x1a) {
		d->tlen--;
	}

	txt2comlike_extract(c, d);

done:
	if(d) {
		if(d->need_errmsg) {
			de_err(c, "Not a SHOW file, or unsupported version");
		}
		destroy_lctx(c, d);
	}
}

static int de_identify_show_gmr(deark *c)
{
	if(c->infile->len>65280) return 0;
	if(de_getbyte(0) != 0xe9) return 0;
	// Testing the last byte of the file may screen out corrupt files, but
	// more importantly screens out the SHOW.COM utility itself, which
	// annoyingly has the same the start-of-file signature as the files it
	// generates.
	if(de_getbyte(c->infile->len-1) != 0x1a) return 0;
	if(dbuf_memcmp(c->infile, 3,
		(const u8*)"\x30\x00\x1f\xa0\x00\x00\x53\x48\x4f\x57", 10))
	{
		return 0;
	}
	return 100;
}

static void de_help_show_gmr(deark *c)
{
	print_encconv_option(c);
}

void de_module_show_gmr(deark *c, struct deark_module_info *mi)
{
	mi->id = "show_gmr";
	mi->desc = "SHOW (G. M. Raymond)";
	mi->run_fn = de_run_show_gmr;
	mi->identify_fn = de_identify_show_gmr;
	mi->help_fn = de_help_show_gmr;
}

///////////////////////////////////////////////////
// Asc2Com (MorganSoft)

struct asc2com_detection_data {
	u8 found;
	u8 is_compressed;
	UI fmtcode;
	i64 tpos;
};

struct asc2com_idinfo {
	const u8 sig1[3];
	// flags&0x03: sig2 type  1=\x49\xe3..., 2="ASC2COM"
	// flags&0x80: compressed
	u8 flags;
	u16 sig2pos;
	u16 txtpos;
	UI fmtcode;
};

// lister codes: 0=full/default, 1=page, 2=lite,
//  3=wide, 4=print, 5=compressed
static const struct asc2com_idinfo asc2com_idinfo_arr[] = {
	{ {0xe8,0xd2,0x00}, 0x01,  867,  1350, 0x11020000 }, // 1.10b
	{ {0xe8,0x25,0x01}, 0x01, 1283,  1819, 0x12510100 }, // 1.25 (?)
	{ {0xe8,0x25,0x01}, 0x01, 1288,  1840, 0x12510200 }, // 1.25 (?)
	{ {0xe8,0x1d,0x01}, 0x01, 1360,  1877, 0x13010000 }, // 1.30
	{ {0xe9,0x18,0x05}, 0x01, 2827,  3734, 0x16510100 }, // 1.65 full (?)
	{ {0xe9,0x18,0x05}, 0x01, 2834,  3750, 0x16610000 }, // 1.66 full
	{ {0xe9,0x1d,0x05}, 0x01, 2916,  4050, 0x17510000 }, // 1.75 full
	{ {0xe9,0x18,0x05}, 0x01, 2911,  4051, 0x17610000 }, // 1.76 full
	{ {0xe9,0x12,0x06}, 0x01, 3203,  4517, 0x20010000 }, // 2.00 full
	{ {0xe9,0x21,0x06}, 0x01, 3231,  4533, 0x20060000 }, // 2.00f-2.05 full

	{ {0xe8,0x06,0x01}, 0x01, 1337,  1854, 0x13010001 }, // 1.30 page
	{ {0xe9,0xc4,0x04}, 0x01, 2725,  3638, 0x16510101 }, // 1.65 page (?)
	{ {0xe9,0xc4,0x04}, 0x01, 2732,  3638, 0x16610001 }, // 1.66 page
	{ {0xe9,0xc9,0x04}, 0x01, 2814,  3955, 0x17510001 }, // 1.75-1.76 page
	{ {0xe9,0x12,0x06}, 0x01, 3185,  4485, 0x20010001 }, // 2.00 page
	{ {0xe9,0x21,0x06}, 0x01, 3213,  4517, 0x20060001 }, // 2.00f-2.05 page

	{ {0xe9,0x7e,0x01}, 0x01, 1523,  1555, 0x16510102 }, // 1.65 lite (?)
	{ {0xe9,0x81,0x01}, 0x01, 1526,  1558, 0x16610002 }, // 1.66 lite
	{ {0xe9,0x8f,0x01}, 0x01, 1722,  1799, 0x17510002 }, // 1.75-1.76 lite
	{ {0xe9,0xfc,0x01}, 0x01, 1868,  2005, 0x20010002 }, // 2.00-2.05 lite

	{ {0xe9,0x8c,0x01}, 0x01, 1747,  1816, 0x16610003 }, // 1.66 wide
	{ {0xe9,0xf5,0x01}, 0x01, 2045,  2161, 0x17510003 }, // 1.75-1.76 wide
	{ {0xe9,0x4d,0x02}, 0x01, 2165,  2341, 0x20010003 }, // 2.00-2.05 wide

	{ {0xbb,0x01,0x00}, 0x02,  240,   382, 0x13010004 }, // 1.30 print
	{ {0xeb,0x03,0x00}, 0x02,  245,   387, 0x16610004 }, // 1.66 print
	{ {0xeb,0x2b,0x00}, 0x02,  295,   437, 0x17510004 }, // 1.75-1.76 print
	{ {0xeb,0x40,0x00}, 0x02,  462,   613, 0x20010004 }, // 2.00-2.05 print

	{ {0xe9,0xaa,0x05}, 0x82, 1065, 10263, 0x20010005 }, // 2.00 compr
	{ {0xe9,0xab,0x05}, 0x82, 1065, 10263, 0x20060005 }, // 2.00f compr
	{ {0xe9,0xad,0x05}, 0x82, 1065, 10407, 0x20110005 }, // 2.01 compr
	{ {0xe9,0xa8,0x05}, 0x82, 1065, 10391, 0x20510005 }  // 2.05 compr
};

static void asc2com_identify(deark *c, struct asc2com_detection_data *idd, UI idmode)
{
	u8 buf[3];
	size_t k;
	const struct asc2com_idinfo *found_item = NULL;

	dbuf_read(c->infile, buf, 0, 3);
	if(buf[0]!=0xe8 && buf[0]!=0xe9 && buf[0]!=0xbb && buf[0]!=0xeb) return;

	for(k=0; k<DE_ARRAYCOUNT(asc2com_idinfo_arr); k++) {
		const struct asc2com_idinfo *t;
		u8 sig_type;

		t = &asc2com_idinfo_arr[k];

		if(buf[0]==t->sig1[0] && buf[1]==t->sig1[1] &&
			(t->sig1[0]==0xeb || (buf[2]==t->sig1[2])))
		{

			sig_type = t->flags & 0x03;
			if(sig_type==1) {
				if(!dbuf_memcmp(c->infile, (i64)t->sig2pos,
					(const void*)"\x49\xe3\x0e\x33\xd2\x8a\x14\xfe\xc2\x03\xf2\x49", 12))
				{
					found_item = t;
				}
			}
			else if(sig_type==2) {
				if(!dbuf_memcmp(c->infile, (i64)t->sig2pos,
					(const void*)"ASC2COM", 7))
				{
					found_item = t;
				}
			}
		}

		if(found_item) {
			break;
		}
	}
	if(!found_item) return;
	idd->found = 1;
	if(idmode) return;

	idd->tpos = (i64)found_item->txtpos;
	idd->fmtcode = found_item->fmtcode;
	if(found_item->flags & 0x80) {
		idd->is_compressed = 1;
	}
}

static void asc2com_filter(deark *c, lctx *d, dbuf *tmpf,
	i64 ipos1, i64 endpos, dbuf *outf)
{
	i64 ipos;
	u8 n;

	ipos = ipos1;
	while(ipos < endpos) {
		n = dbuf_getbyte_p(tmpf, &ipos);
		dbuf_copy(tmpf, ipos, (i64)n, outf);
		dbuf_write(outf, (const u8*)"\x0d\x0a", 2);
		ipos += (i64)n;
	}
}

static void asc2com_extract_compressed(deark *c, lctx *d)
{
	struct de_dfilter_in_params dcmpri;
	struct de_dfilter_out_params dcmpro;
	struct de_dfilter_results dres;
	struct de_lzw_params delzwp;
	dbuf *tmpf = NULL;
	dbuf *outf = NULL;

	tmpf = dbuf_create_membuf(c, 0, 0);

	de_dfilter_init_objects(c, &dcmpri, &dcmpro, &dres);
	dcmpri.f = c->infile;
	dcmpri.pos = d->tpos;
	dcmpri.len = d->tlen;
	dcmpro.f = tmpf;
	dcmpro.len_known = 0;

	de_zeromem(&delzwp, sizeof(struct de_lzw_params));
	delzwp.fmt = DE_LZWFMT_ASC2COM;
	fmtutil_decompress_lzw(c, &dcmpri, &dcmpro, &dres, &delzwp);
	dbuf_flush(tmpf);

	if(tmpf->len>0) {
		outf = dbuf_create_output_file(c, "txt", NULL, 0);
		dbuf_enable_wbuffer(outf);
		asc2com_filter(c, d, tmpf, 0, tmpf->len, outf);
	}

	if(dres.errcode) {
		de_err(c, "%s", de_dfilter_get_errmsg(c, &dres));
		goto done;
	}

done:
	dbuf_close(tmpf);
	dbuf_close(outf);
}

static void asc2com_extract_uncompressed(deark *c, lctx *d)
{
	dbuf *outf = NULL;

	outf = dbuf_create_output_file(c, "txt", NULL, 0);
	dbuf_enable_wbuffer(outf);
	asc2com_filter(c, d, c->infile, d->tpos, d->tlen, outf);
	dbuf_close(outf);
}

static void de_run_asc2com(deark *c, de_module_params *mparams)
{
	lctx *d = NULL;
	struct asc2com_detection_data idd;

	de_zeromem(&idd, sizeof(struct asc2com_detection_data));
	asc2com_identify(c, &idd, 0);
	if(!idd.found) {
		de_err(c, "Not a known Asc2Com format");
		goto done;
	}
	de_dbg(c, "format code: 0x%08x", idd.fmtcode);
	de_dbg(c, "compressed: %u", (UI)idd.is_compressed);

	d = de_malloc(c, sizeof(lctx));
	d->tpos = idd.tpos;
	de_dbg(c, "tpos: %"I64_FMT, d->tpos);
	d->tlen = c->infile->len - d->tlen;
	// TODO: Can we read and use the original filename?
	if(idd.is_compressed) {
		asc2com_extract_compressed(c, d);
	}
	else {
		asc2com_extract_uncompressed(c, d);
	}

done:
	destroy_lctx(c, d);
}

static int de_identify_asc2com(deark *c)
{
	struct asc2com_detection_data idd;

	if(c->infile->len>65280) return 0;
	de_zeromem(&idd, sizeof(struct asc2com_detection_data));
	asc2com_identify(c, &idd, 1);
	if(idd.found) return 72;
	return 0;
}

void de_module_asc2com(deark *c, struct deark_module_info *mi)
{
	mi->id = "asc2com";
	mi->desc = "Asc2Com executable text";
	mi->run_fn = de_run_asc2com;
	mi->identify_fn = de_identify_asc2com;
}

///////////////////////////////////////////////////
// DOC2COM (Gerald DePyper)

struct doc2com_detection_data {
	u8 found;
	UI fmtcode;
};

static void doc2com_detect(deark *c, struct doc2com_detection_data *idd, UI idmode)
{
	u8 buf[22];

	dbuf_read(c->infile, buf, 0, sizeof(buf));

	if(buf[0]==0xbe && buf[15]==0x72) {
		if(!de_memcmp(&buf[3], (const void*)"\xb9\x18\x00\xe8\xb2\x01\xe2\xfb\x3b\x36", 10)) {
			idd->fmtcode = 10; // old unversioned releases
			idd->found = 1;
		}
	}
	else if(buf[0]==0xfc && buf[1]==0xbe && buf[16]==0x72) {
		if(!de_memcmp(&buf[4], (const void*)"\xb9\x18\x00\xe8\x2f\x02\xe2\xfb\x3b\x36", 10)) {
			idd->fmtcode = 20; // v1.2
			idd->found = 1;
		}
	}
	else if(buf[0]==0xfc && buf[5]==0x49) {
		// Expecting all v1.3+ files to start with:
		//  fc ?? ?? ?? ?? 49 8b 36 ?? ?? 8b fe ac 32 04 aa e2 fa ac 34 ff aa ...
		// First 3 bytes:
		//  fc 8b 0e if encrypted
		//  fc eb 13 if not encrypted
		if(!de_memcmp(&buf[10],
			(const void*)"\x8b\xfe\xac\x32\x04\xaa\xe2\xfa\xac\x34\xff\xaa", 12))
		{
			idd->fmtcode = 30; // v1.3+
			idd->found = 1;
		}
	}
}

static void doc2com_analyze(deark *c, lctx *d)
{
	i64 pos_a, pos_b, pos_c, pos_d;
	i64 pos_of_tpos;
	i64 pos_of_tlen;
	i64 pos_of_endpos;
	i64 endpos;

	if(d->fmtcode==30) {
		if(de_getbyte(1) != 0xeb) {
			d->is_encrypted = 1;
		}
	}

	if(d->fmtcode==10) {
		pos_of_tpos = 1;
	}
	else if(d->fmtcode==20) {
		pos_of_tpos = 2;
	}
	else if(d->fmtcode==30) {
		pos_d = de_getu16le(8);
		pos_of_tpos = pos_d - 0x100;
	}
	else {
		d->errflag = 1;
		d->need_errmsg = 1;
		goto done;
	}

	de_dbg(c, "pos of tpos: %"I64_FMT, pos_of_tpos);
	pos_a = de_getu16le(pos_of_tpos);
	d->tpos = pos_a - 0x100;
	de_dbg(c, "tpos: %"I64_FMT, d->tpos);

	if(d->fmtcode==10 || d->fmtcode==20) {
		if(d->fmtcode==20) {
			pos_b = de_getu16le(25);
		}
		else { // 10
			pos_b = de_getu16le(24);
		}
		pos_of_endpos = pos_b - 0x100;
		de_dbg(c, "pos of endpos: %"I64_FMT, pos_of_endpos);
		pos_c = de_getu16le(pos_of_endpos);
		endpos = pos_c - 0x100;
		de_dbg(c, "endpos: %"I64_FMT, endpos);
		d->tlen = endpos - d->tpos;
	}
	else { // 30
		pos_b = de_getu16le(3);
		pos_of_tlen = pos_b - 0x100;
		de_dbg(c, "pos of tlen: %"I64_FMT, pos_of_tlen);
		d->tlen = de_getu16le(pos_of_tlen);
	}

	de_dbg(c, "tlen: %"I64_FMT, d->tlen);
	de_dbg(c, "encrypted: %u", (UI)d->is_encrypted);
done:
	;
}

static void doc2com_output(deark *c, lctx *d)
{
	dbuf *outf = NULL;

	if(d->tlen<0 || d->tpos<0 || d->tpos+d->tlen>c->infile->len) {
		d->errflag = 1;
		d->need_errmsg = 1;
		goto done;
	}

	outf = dbuf_create_output_file(c, "txt", NULL, 0);
	if(d->is_encrypted) {
		u8 this_byte = 0;
		u8 next_byte = 0;
		u8 init_flag = 0;
		i64 i;

		dbuf_enable_wbuffer(outf);

		for(i=0; i<d->tlen; i++) {
			u8 b;

			if(init_flag) {
				this_byte = next_byte;
			}
			else {
				this_byte = de_getbyte(d->tpos+i);
				init_flag = 1;
			}

			if(i+1 < d->tlen) {
				next_byte = de_getbyte(d->tpos+i+1);
			}
			else {
				next_byte = 0xff;
			}

			b = this_byte ^ next_byte;
			dbuf_writebyte(outf, b);
		}
	}
	else {
		dbuf_copy(c->infile, d->tpos, d->tlen, outf);
	}

done:
	dbuf_close(outf);
}

static void de_run_doc2com(deark *c, de_module_params *mparams)
{
	lctx *d = NULL;
	struct doc2com_detection_data idd;

	d = de_malloc(c, sizeof(lctx));
	de_zeromem(&idd, sizeof(struct doc2com_detection_data));
	doc2com_detect(c, &idd, 0);
	if(!idd.found) {
		d->need_errmsg = 1;
		goto done;
	}
	d->fmtcode = idd.fmtcode;
	de_dbg(c, "fmt code: %u", d->fmtcode);
	doc2com_analyze(c, d);
	if(d->errflag) goto done;
	doc2com_output(c, d);

done:
	if(d) {
		if(d->need_errmsg) {
			de_err(c, "Not a DOC2COM file, or unsupported version");
		}
		destroy_lctx(c, d);
	}
}

static int de_identify_doc2com(deark *c)
{
	struct doc2com_detection_data idd;
	u8 b;

	if(c->infile->len>65280) return 0;
	b = de_getbyte(0);
	if(b!=0xbe && b!=0xfc) return 0;

	de_zeromem(&idd, sizeof(struct doc2com_detection_data));
	doc2com_detect(c, &idd, 1);
	if(idd.found) return 73;
	return 0;
}

void de_module_doc2com(deark *c, struct deark_module_info *mi)
{
	mi->id = "doc2com";
	mi->desc = "DOC2COM executable text (G. DePyper)";
	mi->run_fn = de_run_doc2com;
	mi->identify_fn = de_identify_doc2com;
}
