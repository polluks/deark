// This file is part of Deark.
// Copyright (C) 2021 Jason Summers
// See the file COPYING for terms of use.

// This file is for miscellaneous small archive-format modules.

#include <deark-private.h>
#include <deark-fmtutil.h>
#include <deark-fmtutil-arch.h>
DE_DECLARE_MODULE(de_module_cpshrink);
DE_DECLARE_MODULE(de_module_dwc);
DE_DECLARE_MODULE(de_module_tscomp);
DE_DECLARE_MODULE(de_module_edi_pack);
DE_DECLARE_MODULE(de_module_qip);

// **************************************************************************
// CP Shrink (.cpz)
// **************************************************************************

static void cpshrink_decompressor_fn(struct de_arch_member_data *md)
{
	deark *c = md->c;

	switch(md->cmpr_meth) {
	case 0:
	case 1:
		fmtutil_dclimplode_codectype1(c, md->dcmpri, md->dcmpro, md->dres, NULL);
		break;
	case 2:
		fmtutil_decompress_uncompressed(c, md->dcmpri, md->dcmpro, md->dres, 0);
		break;
	default:
		de_dfilter_set_generic_error(c, md->dres, NULL);
	}
}

// Caller creates/destroys md, and sets a few fields.
static void cpshrink_do_member(deark *c, de_arch_lctx *d, struct de_arch_member_data *md)
{
	i64 pos = md->member_hdr_pos;
	UI cdata_crc_reported;
	UI cdata_crc_calc;

	int saved_indent_level;

	de_dbg_indent_save(c, &saved_indent_level);
	md->cmpr_pos = d->cmpr_data_curpos;

	de_dbg(c, "member #%u: hdr at %"I64_FMT", cmpr data at %"I64_FMT,
		(UI)md->member_idx, md->member_hdr_pos, md->cmpr_pos);
	de_dbg_indent(c, 1);

	cdata_crc_reported = (u32)de_getu32le_p(&pos);
	de_dbg(c, "CRC of cmpr. data (reported): 0x%08x", (UI)cdata_crc_reported);

	dbuf_read_to_ucstring(c->infile, pos, 15, md->filename, DE_CONVFLAG_STOP_AT_NUL,
		d->input_encoding);
	pos += 15;
	de_dbg(c, "filename: \"%s\"", ucstring_getpsz_d(md->filename));

	md->cmpr_meth = (UI)de_getbyte_p(&pos);
	de_dbg(c, "cmpr. method: %u", md->cmpr_meth);

	de_arch_read_field_orig_len_p(md, &pos);
	de_arch_read_field_cmpr_len_p(md, &pos);
	d->cmpr_data_curpos += md->cmpr_len;

	de_arch_read_field_dttm_p(d, &md->fi->timestamp[DE_TIMESTAMPIDX_MODIFY], "mod",
		DE_ARCH_TSTYPE_DOS_DT, &pos);

	if(!de_arch_good_cmpr_data_pos(md)) {
		d->fatalerrflag = 1;
		goto done;
	}

	de_crcobj_reset(d->crco);
	de_crcobj_addslice(d->crco, c->infile, md->cmpr_pos, md->cmpr_len);
	cdata_crc_calc = de_crcobj_getval(d->crco);
	de_dbg(c, "CRC of cmpr. data (calculated): 0x%08x", (UI)cdata_crc_calc);
	if(cdata_crc_calc!=cdata_crc_reported) {
		de_err(c, "File data CRC check failed (expected 0x%08x, got 0x%08x). "
			"CPZ file may be corrupted.", (UI)cdata_crc_reported,
			(UI)cdata_crc_calc);
	}

	md->dfn = cpshrink_decompressor_fn;
	de_arch_extract_member_file(md);

done:
	de_dbg_indent_restore(c, saved_indent_level);
}

static void de_run_cpshrink(deark *c, de_module_params *mparams)
{
	de_arch_lctx *d = NULL;
	i64 pos;
	i64 member_hdrs_pos;
	i64 member_hdrs_len;
	u32 member_hdrs_crc_reported;
	u32 member_hdrs_crc_calc;
	i64 i;
	int saved_indent_level;

	de_dbg_indent_save(c, &saved_indent_level);
	d = de_arch_create_lctx(c);
	d->is_le = 1;
	d->input_encoding = de_get_input_encoding(c, NULL, DE_ENCODING_CP437);

	pos = 0;
	de_dbg(c, "archive header at %d", (int)pos);
	de_dbg_indent(c, 1);
	// Not sure if this is a 16-bit, or 32-bit, field, but CP Shrink doesn't
	// work right if the 2 bytes at offset 2 are not 0.
	d->num_members = de_getu32le_p(&pos);
	de_dbg(c, "number of members: %"I64_FMT, d->num_members);
	if(d->num_members<1 || d->num_members>0xffff) {
		de_err(c, "Bad member file count");
		goto done;
	}
	member_hdrs_crc_reported = (u32)de_getu32le_p(&pos);
	de_dbg(c, "member hdrs crc (reported): 0x%08x", (UI)member_hdrs_crc_reported);
	de_dbg_indent(c, -1);

	member_hdrs_pos = pos;
	member_hdrs_len = d->num_members * 32;
	d->cmpr_data_curpos = member_hdrs_pos+member_hdrs_len;

	de_dbg(c, "member headers at %"I64_FMT, member_hdrs_pos);
	de_dbg_indent(c, 1);
	d->crco = de_crcobj_create(c, DE_CRCOBJ_CRC32_IEEE);
	de_crcobj_addslice(d->crco, c->infile, member_hdrs_pos, member_hdrs_len);
	member_hdrs_crc_calc = de_crcobj_getval(d->crco);
	de_dbg(c, "member hdrs crc (calculated): 0x%08x", (UI)member_hdrs_crc_calc);
	if(member_hdrs_crc_calc!=member_hdrs_crc_reported) {
		de_err(c, "Header CRC check failed (expected 0x%08x, got 0x%08x). "
			"This is not a valid CP Shrink file", (UI)member_hdrs_crc_reported,
			(UI)member_hdrs_crc_calc);
	}
	de_dbg_indent(c, -1);

	de_dbg(c, "cmpr data starts at %"I64_FMT, d->cmpr_data_curpos);

	for(i=0; i<d->num_members; i++) {
		struct de_arch_member_data *md;

		md = de_arch_create_md(c, d);
		md->member_idx = i;
		md->member_hdr_pos = pos;
		pos += 32;

		cpshrink_do_member(c, d, md);
		de_arch_destroy_md(c, md);
		if(d->fatalerrflag) goto done;
	}

done:
	de_arch_destroy_lctx(c, d);
	de_dbg_indent_restore(c, saved_indent_level);
}

static int de_identify_cpshrink(deark *c)
{
	i64 n;

	if(!de_input_file_has_ext(c, "cpz")) return 0;
	n = de_getu32le(0);
	if(n<1 || n>0xffff) return 0;
	if(de_getbyte(27)>2) return 0; // cmpr meth of 1st file
	return 25;
}

void de_module_cpshrink(deark *c, struct deark_module_info *mi)
{
	mi->id = "cpshrink";
	mi->desc = "CP Shrink .CPZ";
	mi->run_fn = de_run_cpshrink;
	mi->identify_fn = de_identify_cpshrink;
}

// **************************************************************************
// DWC archive
// **************************************************************************

static void dwc_decompressor_fn(struct de_arch_member_data *md)
{
	deark *c = md->c;

	if(md->cmpr_meth==1) {
		struct de_lzw_params delzwp;

		de_zeromem(&delzwp, sizeof(struct de_lzw_params));
		delzwp.fmt = DE_LZWFMT_DWC;
		fmtutil_decompress_lzw(c, md->dcmpri, md->dcmpro, md->dres, &delzwp);
	}
	else if(md->cmpr_meth==2) {
		fmtutil_decompress_uncompressed(c, md->dcmpri, md->dcmpro, md->dres, 0);
	}
	else {
		de_dfilter_set_generic_error(c, md->dres, NULL);
	}
}

static void squash_slashes(de_ucstring *s)
{
	i64 i;

	for(i=0; i<s->len; i++) {
		if(s->str[i]=='/') {
			s->str[i] = '_';
		}
	}
}

// Set md->filename to the full-path filename, using tmpfn_path + tmpfn_base.
static void dwc_process_filename(deark *c, de_arch_lctx *d, struct de_arch_member_data *md)
{
	ucstring_empty(md->filename);
	squash_slashes(md->tmpfn_base);
	if(ucstring_isempty(md->tmpfn_path)) {
		ucstring_append_ucstring(md->filename, md->tmpfn_base);
		return;
	}

	md->set_name_flags |= DE_SNFLAG_FULLPATH;
	ucstring_append_ucstring(md->filename, md->tmpfn_path);
	de_arch_fixup_path(md->filename, 0);
	if(ucstring_isempty(md->tmpfn_base)) {
		ucstring_append_char(md->filename, '_');
	}
	else {
		ucstring_append_ucstring(md->filename, md->tmpfn_base);
	}
}

static void do_dwc_member(deark *c, de_arch_lctx *d, i64 pos1, i64 fhsize)
{
	i64 pos = pos1;
	struct de_arch_member_data *md = NULL;
	i64 cmt_len = 0;
	i64 path_len = 0;
	UI cdata_crc_reported = 0;
	UI cdata_crc_calc;
	u8 have_cdata_crc = 0;
	u8 b;
	de_ucstring *comment = NULL;

	md = de_arch_create_md(c, d);

	de_dbg(c, "member header at %"I64_FMT, pos1);
	de_dbg_indent(c, 1);
	md->tmpfn_base = ucstring_create(c);
	dbuf_read_to_ucstring(c->infile, pos, 12, md->tmpfn_base, DE_CONVFLAG_STOP_AT_NUL,
		d->input_encoding);
	de_dbg(c, "filename: \"%s\"", ucstring_getpsz_d(md->tmpfn_base));
	// tentative md->filename (could be used by error messages)
	ucstring_append_ucstring(md->filename, md->tmpfn_base);
	pos += 13;

	de_arch_read_field_orig_len_p(md, &pos);
	de_arch_read_field_dttm_p(d, &md->fi->timestamp[DE_TIMESTAMPIDX_MODIFY], "mod",
		DE_ARCH_TSTYPE_UNIX, &pos);
	de_arch_read_field_cmpr_len_p(md, &pos);
	md->cmpr_pos = de_getu32le_p(&pos);
	de_dbg(c, "cmpr. data pos: %"I64_FMT, md->cmpr_pos);

	b = de_getbyte_p(&pos);
	md->cmpr_meth = ((UI)b) & 0x0f;
	de_dbg(c, "cmpr. method: %u", md->cmpr_meth);
	md->file_flags = ((UI)b) >> 4;
	de_dbg(c, "flags: 0x%x", md->file_flags);
	if(md->file_flags & 0x4) {
		md->is_encrypted = 1;
	}

	if(fhsize>=31) {
		cmt_len = (i64)de_getbyte_p(&pos);
		de_dbg(c, "comment len: %d", (int)cmt_len);
	}
	if(fhsize>=32) {
		path_len = (i64)de_getbyte_p(&pos);
		de_dbg(c, "path len: %d", (int)path_len);
	}
	if(fhsize>=34) {
		cdata_crc_reported = (u32)de_getu16le_p(&pos);
		de_dbg(c, "CRC of cmpr. data (reported): 0x%04x", (UI)cdata_crc_reported);
		have_cdata_crc = 1;
	}

	if(!de_arch_good_cmpr_data_pos(md)) {
		goto done;
	}

	if(path_len>1) {
		md->tmpfn_path = ucstring_create(c);
		dbuf_read_to_ucstring(c->infile, md->cmpr_pos+md->cmpr_len,
			path_len-1,
			md->tmpfn_path, DE_CONVFLAG_STOP_AT_NUL, d->input_encoding);
		de_dbg(c, "path: \"%s\"", ucstring_getpsz_d(md->tmpfn_path));
	}
	if(cmt_len>1) {
		comment = ucstring_create(c);
		dbuf_read_to_ucstring(c->infile, md->cmpr_pos+md->cmpr_len+path_len,
			cmt_len-1, comment, DE_CONVFLAG_STOP_AT_NUL, d->input_encoding);
		de_dbg(c, "comment: \"%s\"", ucstring_getpsz_d(comment));
	}

	dwc_process_filename(c, d, md);

	if(have_cdata_crc) {
		if(!d->crco) {
			d->crco = de_crcobj_create(c, DE_CRCOBJ_CRC16_ARC);
		}
		de_crcobj_reset(d->crco);
		de_crcobj_addslice(d->crco, c->infile, md->cmpr_pos, md->cmpr_len);
		cdata_crc_calc = de_crcobj_getval(d->crco);
		de_dbg(c, "CRC of cmpr. data (calculated): 0x%04x", (UI)cdata_crc_calc);
		if(cdata_crc_calc!=cdata_crc_reported) {
			de_err(c, "File data CRC check failed (expected 0x%04x, got 0x%04x). "
				"DWC file may be corrupted.", (UI)cdata_crc_reported,
				(UI)cdata_crc_calc);
		}
	}

	if(d->private1) {
		md->dfn = dwc_decompressor_fn;
		de_arch_extract_member_file(md);
	}

done:
	de_dbg_indent(c, -1);
	de_arch_destroy_md(c, md);
	ucstring_destroy(comment);
}

static int has_dwc_sig(deark *c)
{
	return !dbuf_memcmp(c->infile, c->infile->len-3, (const u8*)"DWC", 3);
}

static void de_run_dwc(deark *c, de_module_params *mparams)
{
	de_arch_lctx *d = NULL;
	i64 trailer_pos;
	i64 trailer_len;
	i64 nmembers;
	i64 fhsize; // size of each file header
	i64 pos;
	i64 i;
	struct de_timestamp tmpts;
	int need_errmsg = 0;
	int saved_indent_level;

	de_dbg_indent_save(c, &saved_indent_level);

	d = de_arch_create_lctx(c);
	d->is_le = 1;
	d->input_encoding = de_get_input_encoding(c, NULL, DE_ENCODING_CP437);
	d->private1 = de_get_ext_option_bool(c, "dwc:extract", 0);

	if(!has_dwc_sig(c)) {
		de_err(c, "Not a DWC file");
		goto done;
	}
	de_declare_fmt(c, "DWC archive");

	if(!d->private1) {
		de_info(c, "Note: Use \"-opt dwc:extract\" to attempt decompression "
			"(works for most small files).");
	}

	de_dbg(c, "trailer");
	de_dbg_indent(c, 1);

	pos = c->infile->len - 27; // Position of the "trailer size" field
	trailer_len = de_getu16le_p(&pos); // Usually 27
	trailer_pos = c->infile->len - trailer_len;
	de_dbg(c, "size: %"I64_FMT" (starts at %"I64_FMT")", trailer_len, trailer_pos);
	if(trailer_len<27 || trailer_pos<0) {
		need_errmsg = 1;
		goto done;
	}

	fhsize = (i64)de_getbyte_p(&pos);
	de_dbg(c, "file header entry size: %d", (int)fhsize);
	if(fhsize<30) {
		need_errmsg = 1;
		goto done;
	}

	pos += 13; // TODO?: name of header file ("h" command)
	de_arch_read_field_dttm_p(d, &tmpts, "archive last-modified", DE_ARCH_TSTYPE_UNIX, &pos);

	nmembers = de_getu16le_p(&pos);
	de_dbg(c, "number of member files: %d", (int)nmembers);
	de_dbg_indent(c, -1);

	pos = trailer_pos - fhsize*nmembers;
	if(pos<0) {
		need_errmsg = 1;
		goto done;
	}
	for(i=0; i<nmembers; i++) {
		do_dwc_member(c, d, pos, fhsize);
		if(d->fatalerrflag) goto done;
		pos += fhsize;
	}

done:
	if(need_errmsg) {
		de_err(c, "Bad DWC file");
	}
	de_arch_destroy_lctx(c, d);
	de_dbg_indent_restore(c, saved_indent_level);
}

static int de_identify_dwc(deark *c)
{
	i64 tsize;
	int has_ext;
	u8 dsize;

	if(!has_dwc_sig(c)) return 0;
	tsize = de_getu16le(c->infile->len-27);
	if(tsize<27 || tsize>c->infile->len) return 0;
	dsize = de_getbyte(c->infile->len-25);
	if(dsize<30) return 0;
	has_ext = de_input_file_has_ext(c, "dwc");
	if(tsize==27 && dsize==34) {
		if(has_ext) return 100;
		return 60;
	}
	if(has_ext) return 10;
	return 0;
}

static void de_help_dwc(deark *c)
{
	de_msg(c, "-opt dwc:extract : Try to decompress");
}

void de_module_dwc(deark *c, struct deark_module_info *mi)
{
	mi->id = "dwc";
	mi->desc = "DWC compressed archive";
	mi->run_fn = de_run_dwc;
	mi->identify_fn = de_identify_dwc;
	mi->help_fn = de_help_dwc;
	mi->flags |= DE_MODFLAG_NONWORKING;
}

// **************************************************************************
// The Stirling Compressor" ("TSComp")
// **************************************************************************

// Probably only TSComp v1.3 is supported.

static void tscomp_decompressor_fn(struct de_arch_member_data *md)
{
	fmtutil_dclimplode_codectype1(md->c, md->dcmpri, md->dcmpro, md->dres, NULL);
}

// Caller creates/destroys md, and sets a few fields.
static void tscomp_do_member(deark *c, de_arch_lctx *d, struct de_arch_member_data *md)
{
	i64 pos = md->member_hdr_pos;
	i64 fnlen;
	int saved_indent_level;

	de_dbg_indent_save(c, &saved_indent_level);
	de_dbg(c, "member #%u at %"I64_FMT, (UI)md->member_idx,
		md->member_hdr_pos);
	de_dbg_indent(c, 1);

	pos += 1;
	de_arch_read_field_cmpr_len_p(md, &pos);
	pos += 4; // ??
	de_arch_read_field_dttm_p(d, &md->fi->timestamp[DE_TIMESTAMPIDX_MODIFY], "mod",
		DE_ARCH_TSTYPE_DOS_DT, &pos);
	pos += 2; // ??

	fnlen = de_getbyte_p(&pos);

	// STOP_AT_NUL is probably not needed.
	dbuf_read_to_ucstring(c->infile, pos, fnlen, md->filename, DE_CONVFLAG_STOP_AT_NUL,
		d->input_encoding);
	de_dbg(c, "filename: \"%s\"", ucstring_getpsz_d(md->filename));
	pos += fnlen;
	pos += 1; // ??

	md->cmpr_pos = pos;
	md->dfn = tscomp_decompressor_fn;
	de_arch_extract_member_file(md);

	pos += md->cmpr_len;
	md->member_total_size = pos - md->member_hdr_pos;

	de_dbg_indent_restore(c, saved_indent_level);
}

static void de_run_tscomp(deark *c, de_module_params *mparams)
{
	de_arch_lctx *d = NULL;
	i64 pos;
	i64 i;
	int saved_indent_level;
	u8 b;
	const char *name;

	de_dbg_indent_save(c, &saved_indent_level);
	d = de_arch_create_lctx(c);
	d->is_le = 1;
	d->input_encoding = de_get_input_encoding(c, NULL, DE_ENCODING_CP437);

	pos = 0;
	de_dbg(c, "archive header at %d", (int)pos);
	de_dbg_indent(c, 1);
	pos += 4;

	b = de_getbyte_p(&pos);
	if(b!=0x08) { d->need_errmsg = 1; goto done; }
	pos += 3; // version?? (01 03 00)
	b = de_getbyte_p(&pos);
	switch(b) {
	case 0: name = "old version"; break;
	case 1: name = "without wildcard"; break;
	case 2: name = "with wildcard"; break;
	default: name = "?";
	}
	de_dbg(c, "filename style: %u (%s)", (UI)b, name);
	if(b!=1 && b!=2) { d->need_errmsg = 1; goto done; }

	pos += 4; // ??
	de_dbg_indent(c, -1);

	i = 0;
	while(1) {
		struct de_arch_member_data *md;

		if(d->fatalerrflag) goto done;
		if(pos+17 > c->infile->len) goto done;
		if(de_getbyte(pos) != 0x12) { d->need_errmsg = 1; goto done; }

		md = de_arch_create_md(c, d);
		md->member_idx = i;
		md->member_hdr_pos = pos;

		tscomp_do_member(c, d, md);
		if(md->member_total_size<=0) d->fatalerrflag = 1;

		pos += md->member_total_size;
		de_arch_destroy_md(c, md);
		i++;
	}

done:
	if(d->need_errmsg) {
		de_err(c, "Bad or unsupported TSComp format");
	}
	de_arch_destroy_lctx(c, d);
	de_dbg_indent_restore(c, saved_indent_level);
}

static int de_identify_tscomp(deark *c)
{
	i64 n;

	n = de_getu32be(0);
	// Note: The "13" might be a version number. The "8c" is a mystery,
	// and seems to be ignored.
	if(n == 0x655d138cU) return 100;
	return 0;
}

void de_module_tscomp(deark *c, struct deark_module_info *mi)
{
	mi->id = "tscomp";
	mi->desc = "The Stirling Compressor";
	mi->run_fn = de_run_tscomp;
	mi->identify_fn = de_identify_tscomp;
}

// **************************************************************************
// EDI Install [Pro] packed file / EDI Pack / EDI LZSS / EDI LZSSLib
// **************************************************************************

static const u8 *g_edilzss_sig = (const u8*)"EDILZSS";

static void edi_pack_decompressor_fn(struct de_arch_member_data *md)
{
	fmtutil_decompress_lzss1(md->c, md->dcmpri, md->dcmpro, md->dres, 0x0);
}

// This basically checks for a valid DOS filename.
// EDI Pack is primarily a Windows 3.x format -- I'm not sure what filenames are
// allowed.
static int edi_is_filename_at(deark *c, de_arch_lctx *d, i64 pos)
{
	u8 buf[13];
	size_t i;
	int found_nul = 0;
	int found_dot = 0;
	int base_len = 0;
	int ext_len = 0;

	if(pos+13 > c->infile->len) return 0;
	de_read(buf, pos, 13);

	for(i=0; i<13; i++) {
		u8 b;

		b = buf[i];
		if(b==0) {
			found_nul = 1;
			break;
		}
		else if(b=='.') {
			if(found_dot) return 0;
			found_dot = 1;
		}
		else if(b<33 || b=='"' || b=='*' || b=='+' || b==',' || b=='/' ||
			b==':' || b==';' || b=='<' || b=='=' || b=='>' || b=='?' ||
			b=='[' || b=='\\' || b==']' || b=='|' || b==127)
		{
			return 0;
		}
		else {
			// TODO: Are capital letters allowed in this format? If not, that
			// would be a good thing to check for.
			if(found_dot) ext_len++;
			else base_len++;
		}
	}

	if(!found_nul || base_len<1 || base_len>8 || ext_len>3) return 0;
	return 1;
}

// Sets d->fmtver to:
//  0 = Not a known format
//  1 = EDI Pack "EDILZSS1"
//  2 = EDI Pack "EDILZSS2"
//  10 = EDI LZSSLib EDILZSSA.DLL
//  Other formats might exist, but are unlikely to ever be supported:
//  * EDI LZSSLib EDILZSSB.DLL
//  * EDI LZSSLib EDILZSSC.DLL
static void edi_detect_fmt(deark *c, de_arch_lctx *d)
{
	u8 ver;
	i64 pos = 0;

	if(dbuf_memcmp(c->infile, pos, g_edilzss_sig, 7)) {
		d->need_errmsg = 1;
		return;
	}
	pos += 7;

	ver = de_getbyte_p(&pos);
	if(ver=='1') {
		// There's no easy way to distinguish some LZSS1 formats. This will not
		// always work.
		if(edi_is_filename_at(c, d, pos)) {
			d->fmtver = 1;
		}
		else {
			d->fmtver = 10;
		}
	}
	else if(ver=='2') {
		d->fmtver = 2;
	}
	else {
		d->need_errmsg = 1;
	}
}

static void de_run_edi_pack(deark *c, de_module_params *mparams)
{
	de_arch_lctx *d = NULL;
	struct de_arch_member_data *md = NULL;
	i64 pos = 0;

	d = de_arch_create_lctx(c);
	d->is_le = 1;
	d->input_encoding = de_get_input_encoding(c, NULL, DE_ENCODING_WINDOWS1252);

	edi_detect_fmt(c, d);
	if(d->fmtver==0) goto done;
	else if(d->fmtver==10) {
		de_declare_fmt(c, "EDI LZSSLib");
	}
	else {
		de_declare_fmtf(c, "EDI Pack LZSS%d", d->fmtver);
	}
	pos = 8;

	md = de_arch_create_md(c, d);
	if(d->fmtver==1 || d->fmtver==2) {
		dbuf_read_to_ucstring(c->infile, pos, 12, md->filename, DE_CONVFLAG_STOP_AT_NUL,
			d->input_encoding);
		de_dbg(c, "filename: \"%s\"", ucstring_getpsz_d(md->filename));
		pos += 13;
	}

	if(d->fmtver==2) {
		de_arch_read_field_orig_len_p(md, &pos);
	}

	if(pos > c->infile->len) {
		d->need_errmsg = 1;
		goto done;
	}

	md->cmpr_pos = pos;
	md->cmpr_len = c->infile->len - md->cmpr_pos;
	md->dfn = edi_pack_decompressor_fn;
	de_arch_extract_member_file(md);

done:
	de_arch_destroy_md(c, md);
	if(d->need_errmsg) {
		de_err(c, "Bad or unsupported EDI Pack format");
	}
	de_arch_destroy_lctx(c, d);
}

static int de_identify_edi_pack(deark *c)
{
	if(!dbuf_memcmp(c->infile, 0, g_edilzss_sig, 7)) {
		u8 v;

		v = de_getbyte(7);
		if(v=='1' || v=='2') return 100;
		return 0;
	}
	return 0;
}

void de_module_edi_pack(deark *c, struct deark_module_info *mi)
{
	mi->id = "edi_pack";
	mi->desc = "EDI Install packed file";
	mi->run_fn = de_run_edi_pack;
	mi->identify_fn = de_identify_edi_pack;
}

// **************************************************************************
// Quarterdeck QIP
// **************************************************************************

static void qip_decompressor_fn(struct de_arch_member_data *md)
{
	fmtutil_dclimplode_codectype1(md->c, md->dcmpri, md->dcmpro, md->dres, NULL);
}

// Returns 0 if no member was found at md->member_hdr_pos.
static int do_qip_member(deark *c, de_arch_lctx *d, struct de_arch_member_data *md)
{
	int saved_indent_level;
	i64 pos;
	UI index;
	int retval = 0;

	de_dbg_indent_save(c, &saved_indent_level);
	de_dbg(c, "member at %"I64_FMT, md->member_hdr_pos);
	de_dbg_indent(c, 1);
	pos = md->member_hdr_pos;
	if(dbuf_memcmp(c->infile, pos, "QD", 2)) goto done;
	pos += 2;
	retval = 1;
	pos += 2; // ?
	de_arch_read_field_cmpr_len_p(md, &pos);
	index = (UI)de_getu16le_p(&pos); // ?
	de_dbg(c, "index: %u", index);

	if(d->fmtver>=2) {
		md->crc_reported = (u32)de_getu32le_p(&pos);
		de_dbg(c, "crc (reported): 0x%08x", (UI)md->crc_reported);
	}

	de_arch_read_field_dos_attr_p(md, &pos); // ?

	de_arch_read_field_dttm_p(d, &md->fi->timestamp[DE_TIMESTAMPIDX_MODIFY], "mod",
		DE_ARCH_TSTYPE_DOS_TD, &pos);
	de_arch_read_field_orig_len_p(md, &pos);
	dbuf_read_to_ucstring(c->infile, pos, 12, md->filename, DE_CONVFLAG_STOP_AT_NUL,
		d->input_encoding);
	de_dbg(c, "filename: \"%s\"", ucstring_getpsz_d(md->filename));
	pos += 12;
	pos += 1; // Maybe to allow the name to always be NUL terminated?

	md->cmpr_pos = pos;
	de_dbg(c, "cmpr data at %"I64_FMT, md->cmpr_pos);
	md->dfn = qip_decompressor_fn;
	if(d->fmtver>=2) {
		md->validate_crc = 1;
	}

	de_arch_extract_member_file(md);

done:
	de_dbg_indent_restore(c, saved_indent_level);
	return retval;
}

static void qip_do_v1(deark *c, de_arch_lctx *d)
{
	i64 pos = 0;
	struct de_arch_member_data *md = NULL;

	// This version doesn't have an index, but we sort of pretend it does,
	// so that v1 and v2 can be handled pretty much the same.

	while(1) {
		i64 cmpr_len;

		if(pos+32 >= c->infile->len) goto done;

		if(md) {
			de_arch_destroy_md(c, md);
			md = NULL;
		}
		md = de_arch_create_md(c, d);

		md->member_hdr_pos = pos;
		cmpr_len = de_getu32le(pos+4);
		if(!do_qip_member(c, d, md)) {
			goto done;
		}
		pos += 32 + cmpr_len;
	}

done:
	if(md) {
		de_arch_destroy_md(c, md);
	}
}

static void qip_do_v2(deark *c, de_arch_lctx *d)
{
	i64 pos;
	i64 index_pos;
	i64 index_len;
	i64 index_endpos;
	i64 i;
	struct de_arch_member_data *md = NULL;

	pos = 2;
	d->num_members = de_getu16le_p(&pos);
	de_dbg(c, "number of members: %"I64_FMT, d->num_members);
	index_len = de_getu32le_p(&pos);
	de_dbg(c, "index size: %"I64_FMT, index_len); // ??
	d->crco = de_crcobj_create(c, DE_CRCOBJ_CRC32_IEEE);
	index_pos = 16;

	de_dbg(c, "index at %"I64_FMT, index_pos);
	index_endpos = index_pos+index_len;
	if(index_endpos > c->infile->len) goto done;
	pos = index_pos;

	for(i=0; i<d->num_members; i++) {
		if(pos+16 > index_endpos) goto done;

		if(md) {
			de_arch_destroy_md(c, md);
			md = NULL;
		}
		md = de_arch_create_md(c, d);

		md->member_hdr_pos = de_getu32le_p(&pos);
		(void)do_qip_member(c, d, md);
		pos += 12;
	}

done:
	if(md) {
		de_arch_destroy_md(c, md);
	}
}

static void de_run_qip(deark *c, de_module_params *mparams)
{
	de_arch_lctx *d = NULL;
	u8 b;
	int unsupp_flag = 0;

	d = de_arch_create_lctx(c);
	d->is_le = 1;
	d->input_encoding = de_get_input_encoding(c, NULL, DE_ENCODING_CP437);

	b = de_getbyte(1);
	if(b=='P') {
		d->fmtver = 2;
	}
	else if(b=='D') {
		d->fmtver = 1;
	}
	else {
		unsupp_flag = 1;
		goto done;
	}

	if(d->fmtver==2) {
		if(de_getbyte(8)!=0x02) {
			unsupp_flag = 1;
			goto done;
		}
	}

	if(d->fmtver==1) {
		qip_do_v1(c, d);
	}
	else {
		qip_do_v2(c, d);
	}

done:
	if(unsupp_flag) {
		de_err(c, "Not a supported QIP format");
	}
	de_arch_destroy_lctx(c, d);
}

static int de_identify_qip(deark *c)
{
	u8 b;
	i64 n;

	if(de_getbyte(0)!='Q') return 0;
	b = de_getbyte(1);
	if(b=='P') {
		if(de_getbyte(8)!=0x02) return 0;
		n = de_getu32le(16);
		if(n>c->infile->len) return 0;
		if(!dbuf_memcmp(c->infile, n, "QD", 2)) return 100;
	}
	else if(b=='D') {
		if(de_getu16le(2)==0 &&
			de_getu16le(8)==1)
		{
			return 70;
		}
	}
	return 0;
}

void de_module_qip(deark *c, struct deark_module_info *mi)
{
	mi->id = "qip";
	mi->desc = "QIP (Quarterdeck)";
	mi->run_fn = de_run_qip;
	mi->identify_fn = de_identify_qip;
}
