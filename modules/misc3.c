// This file is part of Deark.
// Copyright (C) 2021 Jason Summers
// See the file COPYING for terms of use.

// This file is for miscellaneous small archive-format modules.

#include <deark-private.h>
#include <deark-fmtutil.h>
#include <deark-fmtutil-arch.h>
DE_DECLARE_MODULE(de_module_cpshrink);
DE_DECLARE_MODULE(de_module_dwc);
DE_DECLARE_MODULE(de_module_edi_pack);
DE_DECLARE_MODULE(de_module_qip);
DE_DECLARE_MODULE(de_module_pcxlib);
DE_DECLARE_MODULE(de_module_gxlib);
DE_DECLARE_MODULE(de_module_mdcd);
DE_DECLARE_MODULE(de_module_cazip);
DE_DECLARE_MODULE(de_module_cmz);
DE_DECLARE_MODULE(de_module_pcshrink);
DE_DECLARE_MODULE(de_module_arcv);

static int dclimplode_header_at(deark *c, i64 pos)
{
	u8 b;

	b = de_getbyte(pos);
	if(b>1) return 0;
	b = de_getbyte(pos+1);
	if(b<4 || b>6) return 0;
	return 1;
}

static void dclimplode_decompressor_fn(struct de_arch_member_data *md)
{
	fmtutil_dclimplode_codectype1(md->c, md->dcmpri, md->dcmpro, md->dres, NULL);
}

static void backslashes_to_slashes(de_ucstring *s)
{
	i64 i;

	for(i=0; i<s->len; i++) {
		if(s->str[i]=='\\') {
			s->str[i] = '/';
		}
	}
}

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
	md->dfn = dclimplode_decompressor_fn;
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

// **************************************************************************
// PCX Library (by Genus Microprogramming)
// **************************************************************************

#define FMT_PCXLIB 0
#define FMT_GXLIB  1

static void noncompressed_decompressor_fn(struct de_arch_member_data *md)
{
	fmtutil_decompress_uncompressed(md->c, md->dcmpri, md->dcmpro, md->dres, 0);
}

static void read_pcxgxlib_filename(deark *c, de_arch_lctx *d, struct de_arch_member_data *md,
	i64 pos)
{
	de_ucstring *tmps = NULL;

	tmps = ucstring_create(c);
	dbuf_read_to_ucstring(c->infile, pos, 8, tmps, DE_CONVFLAG_STOP_AT_NUL,
		d->input_encoding);
	ucstring_strip_trailing_spaces(tmps);
	ucstring_append_ucstring(md->filename, tmps);
	ucstring_empty(tmps);
	dbuf_read_to_ucstring(c->infile, pos+8, 4, tmps, DE_CONVFLAG_STOP_AT_NUL,
		d->input_encoding);
	ucstring_strip_trailing_spaces(tmps);
	if(tmps->len>1) {
		// The extension part includes the dot. If len==1, there is no extension.
		ucstring_append_ucstring(md->filename, tmps);
	}
	ucstring_destroy(tmps);
}

static void do_pcxgxlib_member(deark *c,  de_arch_lctx *d, struct de_arch_member_data *md)
{
	int saved_indent_level;
	i64 pos = md->member_hdr_pos;

	de_dbg_indent_save(c, &saved_indent_level);

	de_dbg(c, "member file at %"I64_FMT, md->member_hdr_pos);
	de_dbg_indent(c, 1);

	if(d->fmtcode==FMT_PCXLIB) {
		pos++; // already read
	}
	if(d->fmtcode==FMT_GXLIB) {
		md->cmpr_meth = de_getbyte_p(&pos);
	}

	read_pcxgxlib_filename(c, d, md, pos);
	de_dbg(c, "filename: \"%s\"", ucstring_getpsz_d(md->filename));
	pos += 13;

	if(d->fmtcode==FMT_GXLIB) {
		md->cmpr_pos = de_getu32le_p(&pos);
		de_dbg(c, "cmpr. data pos: %"I64_FMT, md->cmpr_pos);
	}

	de_arch_read_field_cmpr_len_p(md, &pos);

	de_arch_read_field_dttm_p(d, &md->fi->timestamp[DE_TIMESTAMPIDX_MODIFY], "mod",
		DE_ARCH_TSTYPE_DOS_DT, &pos);

	if(d->fmtcode==FMT_PCXLIB) {
		md->cmpr_meth = (UI)de_getu16le_p(&pos);
	}

	de_dbg(c, "packing type: %u", (UI)md->cmpr_meth);
	if(md->cmpr_meth==0) {
		md->orig_len = md->cmpr_len;
		md->orig_len_known = 1;
	}
	else {
		de_err(c, "Unsupported compression: %u", (UI)md->cmpr_meth);
		goto done;
	}

	if(d->fmtcode==FMT_PCXLIB) {
		pos += 40; // note
		pos += 20; // unused
	}

	if(d->fmtcode==FMT_PCXLIB) {
		md->cmpr_pos = pos;
	}

	if(!de_arch_good_cmpr_data_pos(md)) {
		d->fatalerrflag = 1;
		goto done;
	}

	if(d->fmtcode==FMT_GXLIB) {
		md->member_total_size = pos - md->member_hdr_pos;
	}
	else {
		md->member_total_size = md->cmpr_pos + md->cmpr_len - md->member_hdr_pos;
	}

	md->dfn = noncompressed_decompressor_fn;
	de_arch_extract_member_file(md);

done:
	de_dbg_indent_restore(c, saved_indent_level);
}

static void do_pcxgxlib_main(deark *c, de_module_params *mparams, UI fmtcode)
{
	i64 pos = 0;
	i64 member_count = 0;
	struct de_arch_member_data *md = NULL;
	de_arch_lctx *d = NULL;

	d = de_arch_create_lctx(c);
	d->fmtcode = fmtcode;
	d->is_le = 1;
	d->input_encoding = de_get_input_encoding(c, NULL, DE_ENCODING_CP437);

	if(d->fmtcode==FMT_GXLIB) {
		pos += 2;
		pos += 50; // copyright message
		d->fmtver = (int)de_getu16le_p(&pos);
		de_dbg(c, "gxLib ver: %d", d->fmtver);
		pos += 40; // label
		d->num_members = de_getu16le_p(&pos);
		de_dbg(c, "number of members: %"I64_FMT, d->num_members);
		pos += 32; // unused
	}
	else {
		pos += 10;
		pos += 50;
		d->fmtver = (int)de_getu16le_p(&pos);
		de_dbg(c, "pcxLib ver: %d", d->fmtver);
		pos += 40; // TODO: volume label
		pos += 20; // unused
	}

	while(1) {
		if(pos >= c->infile->len) goto done;

		if(d->fmtcode==FMT_GXLIB) {
			if(member_count >= d->num_members) goto done;
		}

		if(d->fmtcode==FMT_PCXLIB) {
			u8 b;

			b = de_getbyte(pos);
			if(b != 0x01) goto done;
		}

		if(md) {
			de_arch_destroy_md(c, md);
			md = NULL;
		}
		md = de_arch_create_md(c, d);
		md->member_hdr_pos = pos;
		do_pcxgxlib_member(c, d, md);
		if(d->fatalerrflag) goto done;
		if(md->member_total_size<1) goto done;
		pos += md->member_total_size;
		member_count++;
	}

done:
	if(md) {
		de_arch_destroy_md(c, md);
	}
	de_arch_destroy_lctx(c, d);
}

static void de_run_pcxlib(deark *c, de_module_params *mparams)
{
	do_pcxgxlib_main(c, mparams, FMT_PCXLIB);
}

static int de_identify_pcxlib(deark *c)
{
	if(!dbuf_memcmp(c->infile, 0, "pcxLib\0", 7)) return 100;
	return 0;
}

void de_module_pcxlib(deark *c, struct deark_module_info *mi)
{
	mi->id = "pcxlib";
	mi->desc = "PCX Library";
	mi->run_fn = de_run_pcxlib;
	mi->identify_fn = de_identify_pcxlib;
}

// **************************************************************************
// GX Library / Genus Graphics Library
// **************************************************************************

static void de_run_gxlib(deark *c, de_module_params *mparams)
{
	do_pcxgxlib_main(c, mparams, FMT_GXLIB);
}

static int de_identify_gxlib(deark *c)
{
	UI n;
	u8 has_copyr, has_ver;

	n = (UI)de_getu16be(0);
	if(n!=0x01ca) return 0;
	has_copyr = !dbuf_memcmp(c->infile, 2, "Copyri", 6);
	n = (UI)de_getu16le(52);
	has_ver = (n==100);
	if(has_copyr && has_ver) return 100;
	if(has_copyr || has_ver) return 25;
	return 0;
}

void de_module_gxlib(deark *c, struct deark_module_info *mi)
{
	mi->id = "gxlib";
	mi->desc = "GX Library";
	mi->run_fn = de_run_gxlib;
	mi->identify_fn = de_identify_gxlib;
}

// **************************************************************************
// MDCD
// **************************************************************************

#define MDCD_MINHEADERLEN 54

static int mdcd_sig_at(deark *c, i64 pos)
{
	return !dbuf_memcmp(c->infile, pos, (const void*)"MDmd", 4);
}

static void mdcd_decompressor_fn(struct de_arch_member_data *md)
{
	deark *c = md->c;

	if(md->cmpr_meth==1) {
		struct de_lzw_params delzwp;

		de_zeromem(&delzwp, sizeof(struct de_lzw_params));
		delzwp.fmt = DE_LZWFMT_ZOOLZD;
		fmtutil_decompress_lzw(c, md->dcmpri, md->dcmpro, md->dres, &delzwp);
	}
	else if(md->cmpr_meth==0) {
		fmtutil_decompress_uncompressed(c, md->dcmpri, md->dcmpro, md->dres, 0);
	}
	else {
		de_dfilter_set_generic_error(c, md->dres, NULL);
	}
}

// Returns 0 if no member was found at md->member_hdr_pos.
static int do_mdcd_member(deark *c, de_arch_lctx *d, struct de_arch_member_data *md)
{
	i64 pos;
	i64 s_len;
	i64 hdrlen;
	UI attr;
	int saved_indent_level;
	int retval = 0;
	int have_path = 0;
	u8 hdrtype;

	de_dbg_indent_save(c, &saved_indent_level);

	// Note: For info about the MDCD header format, see MDCD.PAS, near the
	// "FileHeader = Record" line.

	pos = md->member_hdr_pos;
	if(!mdcd_sig_at(c, pos)) {
		if(md->member_hdr_pos==0) {
			de_err(c, "Not an MDCD file");
		}
		else {
			de_dbg(c, "[member not found at %"I64_FMT"]", pos);
		}
		goto done;
	}
	pos += 4;

	de_dbg(c, "member at %"I64_FMT, md->member_hdr_pos);
	de_dbg_indent(c, 1);
	pos++; // software version?
	hdrtype = de_getbyte_p(&pos);
	de_dbg(c, "header type: %u", (UI)hdrtype);
	if(hdrtype!=1) {
		d->need_errmsg = 1;
		goto done;
	}
	hdrlen = de_getu16le_p(&pos);
	de_dbg(c, "header len: %"I64_FMT, hdrlen);
	if(hdrlen<MDCD_MINHEADERLEN) {
		d->need_errmsg = 1;
		goto done;
	}

	pos += 16; // various
	md->cmpr_meth = (UI)de_getbyte_p(&pos);
	de_dbg(c, "cmpr. method: %u", md->cmpr_meth);
	de_arch_read_field_orig_len_p(md, &pos);
	de_arch_read_field_cmpr_len_p(md, &pos);

	md->cmpr_pos = md->member_hdr_pos + hdrlen;
	// TODO: If we can set the filename first, we should.
	if(!de_arch_good_cmpr_data_pos(md)) {
		goto done;
	}
	md->member_total_size = hdrlen + md->cmpr_len;
	retval = 1;

	attr = (UI)de_getu16le_p(&pos);
	de_arch_handle_field_dos_attr(md, attr);

	de_arch_read_field_dttm_p(d, &md->fi->timestamp[DE_TIMESTAMPIDX_MODIFY], "mod",
		DE_ARCH_TSTYPE_DOS_TD, &pos);

	md->crc_reported = (u32)de_getu16le_p(&pos);
	de_dbg(c, "crc (reported): 0x%04x", (UI)md->crc_reported);

	s_len = de_getbyte_p(&pos);
	if(s_len>12) {
		d->need_errmsg = 1;
		goto done;
	}
	md->tmpfn_base = ucstring_create(c);
	dbuf_read_to_ucstring(c->infile, pos, s_len, md->tmpfn_base, 0, d->input_encoding);
	de_dbg(c, "filename: \"%s\"", ucstring_getpsz_d(md->tmpfn_base));
	pos += 12;

	if(hdrlen>=55) {
		s_len = de_getbyte_p(&pos);
	}
	else {
		s_len = 0;
	}
	md->tmpfn_path = ucstring_create(c);
	dbuf_read_to_ucstring(c->infile, pos, s_len, md->tmpfn_path, 0, d->input_encoding);
	de_dbg(c, "path: \"%s\"", ucstring_getpsz_d(md->tmpfn_path));

	// Ignore paths that look like absolute paths. Not sure what to do with them.
	have_path = ucstring_isnonempty(md->tmpfn_path);

	if(have_path && md->tmpfn_path->len >= 1 && (md->tmpfn_path->str[0]=='\\' ||
		md->tmpfn_path->str[0]=='/'))
	{
		have_path = 0;
	}
	if(have_path && md->tmpfn_path->len >= 2 && md->tmpfn_path->str[1]==':') {
		have_path = 0;
	}

	if(have_path) {
		de_arch_fixup_path(md->tmpfn_path, 0);
		ucstring_append_ucstring(md->filename, md->tmpfn_path);
		md->set_name_flags |= DE_SNFLAG_FULLPATH;
	}
	ucstring_append_ucstring(md->filename, md->tmpfn_base);

	de_dbg(c, "compressed data at %"I64_FMT", len=%"I64_FMT, md->cmpr_pos, md->cmpr_len);

	if(md->cmpr_meth>1) {
		de_err(c, "Unsupported compression: %u", (UI)md->cmpr_meth);
		goto done;
	}

	md->dfn = mdcd_decompressor_fn;

	md->validate_crc = 1;
	// When extracting, MDCD (1.0) does not validate the CRC of files that were
	// stored uncompressed. Some files seem to exploit this, and set the CRC to
	// 0, so we tolerate it with a warning.
	if(md->crc_reported==0 && md->cmpr_meth==0) {
		md->behavior_on_wrong_crc = 1;
	}

	de_arch_extract_member_file(md);

done:
	de_dbg_indent_restore(c, saved_indent_level);
	return retval;
}

static void de_run_mdcd(deark *c, de_module_params *mparams)
{
	de_arch_lctx *d = NULL;
	i64 pos = 0;
	struct de_arch_member_data *md = NULL;

	d = de_arch_create_lctx(c);
	d->is_le = 1;
	d->input_encoding = de_get_input_encoding(c, NULL, DE_ENCODING_CP437);
	d->crco = de_crcobj_create(c, DE_CRCOBJ_CRC16_XMODEM);

	while(1) {
		if(pos+MDCD_MINHEADERLEN >= c->infile->len) goto done;

		if(md) {
			de_arch_destroy_md(c, md);
			md = NULL;
		}
		md = de_arch_create_md(c, d);
		md->member_hdr_pos = pos;
		if(!do_mdcd_member(c, d, md)) goto done;
		if(md->member_total_size<=0) goto done;
		pos += md->member_total_size;
	}

done:
	if(md) {
		de_arch_destroy_md(c, md);
	}
	if(d) {
		if(d->need_errmsg) {
			de_err(c, "Bad or unsupported MDCD file");
		}
		de_arch_destroy_lctx(c, d);
	}
}

static int de_identify_mdcd(deark *c)
{
	if(mdcd_sig_at(c, 0)) {
		return 91;
	}
	return 0;
}

void de_module_mdcd(deark *c, struct deark_module_info *mi)
{
	mi->id = "mdcd";
	mi->desc = "MDCD archive";
	mi->run_fn = de_run_mdcd;
	mi->identify_fn = de_identify_mdcd;
}

// **************************************************************************
// CAZIP
// **************************************************************************

static void de_run_cazip(deark *c, de_module_params *mparams)
{
	de_arch_lctx *d = NULL;
	struct de_arch_member_data *md = NULL;
	i64 pos;
	UI verfield, field10, field12, field18;

	d = de_arch_create_lctx(c);
	d->is_le = 1;
	d->crco = de_crcobj_create(c, DE_CRCOBJ_CRC32_IEEE);

	md = de_arch_create_md(c, d);
	//d->input_encoding = de_get_input_encoding(c, NULL, DE_ENCODING_CP437);

	pos = 8;
	verfield = (UI)de_getu16be_p(&pos);
	field10 = (UI)de_getu16le_p(&pos);
	field12 = (UI)de_getu16le_p(&pos);

	md->crc_reported = (u32)de_getu32le_p(&pos);
	de_dbg(c, "crc (reported): 0x%08x", (UI)md->crc_reported);

	field18 = (UI)de_getu16le_p(&pos);

	if(verfield!=0x3333 || field10!=1 || field12!=1 || field18!=0) {
		de_warn(c, "This version of CAZIP file might not be handled correctly");
	}

	md->cmpr_pos = pos;
	md->cmpr_len = c->infile->len - md->cmpr_pos;
	if(!de_arch_good_cmpr_data_pos(md)) {
		goto done;
	}

	md->dfn = dclimplode_decompressor_fn;
	md->validate_crc = 1;
	de_arch_extract_member_file(md);

done:
	if(md) {
		de_arch_destroy_md(c, md);
		md = NULL;
	}
	if(d) {
		de_arch_destroy_lctx(c, d);
	}
}


static int de_identify_cazip(deark *c)
{
	if(dbuf_memcmp(c->infile, 0, (const void*)"\x0d\x0a\x1a" "CAZIP", 8)) {
		return 0;
	}

	return 100;
}

void de_module_cazip(deark *c, struct deark_module_info *mi)
{
	mi->id = "cazip";
	mi->desc = "CAZIP compressed file";
	mi->run_fn = de_run_cazip;
	mi->identify_fn = de_identify_cazip;
}

// **************************************************************************
// CMZ (Ami Pro installer archive)
// **************************************************************************

#define CMZ_MINHEADERLEN 21

static int cmz_sig_at(deark *c, i64 pos)
{
	return !dbuf_memcmp(c->infile, pos, (const void*)"Clay", 4);
}

// Returns 0 if no member was found at md->member_hdr_pos.
static int do_cmz_member(deark *c, de_arch_lctx *d, struct de_arch_member_data *md)
{
	i64 pos;
	i64 namelen;
	int saved_indent_level;
	int retval = 0;

	de_dbg_indent_save(c, &saved_indent_level);

	pos = md->member_hdr_pos;
	if(!cmz_sig_at(c, pos)) {
		if(md->member_hdr_pos==0) {
			de_err(c, "Not a CMZ file");
		}
		else {
			de_err(c, "Bad data found at %"I64_FMT, pos);
		}
		goto done;
	}
	pos += 4;

	de_dbg(c, "member at %"I64_FMT, md->member_hdr_pos);
	de_dbg_indent(c, 1);

	de_arch_read_field_cmpr_len_p(md, &pos);
	de_arch_read_field_orig_len_p(md, &pos);
	de_arch_read_field_dttm_p(d, &md->fi->timestamp[DE_TIMESTAMPIDX_MODIFY], "mod",
		DE_ARCH_TSTYPE_DOS_DT, &pos);

	namelen = de_getu16le_p(&pos);
	if(namelen>255) {
		d->need_errmsg = 1;
		goto done;
	}
	pos += 2; // Unknown field (flags?)

	dbuf_read_to_ucstring(c->infile, pos, namelen, md->filename, 0, d->input_encoding);
	de_dbg(c, "filename: \"%s\"", ucstring_getpsz_d(md->filename));
	pos += namelen;

	md->cmpr_pos = pos;
	de_dbg(c, "compressed data at %"I64_FMT", len=%"I64_FMT, md->cmpr_pos, md->cmpr_len);
	if(!de_arch_good_cmpr_data_pos(md)) {
		goto done;
	}

	md->member_total_size =  md->cmpr_pos + md->cmpr_len - md->member_hdr_pos;
	retval = 1;

	md->dfn = dclimplode_decompressor_fn;
	de_arch_extract_member_file(md);

done:
	de_dbg_indent_restore(c, saved_indent_level);
	return retval;
}

static void de_run_cmz(deark *c, de_module_params *mparams)
{
	de_arch_lctx *d = NULL;
	i64 pos = 0;
	struct de_arch_member_data *md = NULL;

	d = de_arch_create_lctx(c);
	d->is_le = 1;
	d->input_encoding = de_get_input_encoding(c, NULL, DE_ENCODING_CP437);

	while(1) {
		if(pos+CMZ_MINHEADERLEN >= c->infile->len) goto done;

		if(md) {
			de_arch_destroy_md(c, md);
			md = NULL;
		}
		md = de_arch_create_md(c, d);
		md->member_hdr_pos = pos;
		if(!do_cmz_member(c, d, md)) goto done;
		if(md->member_total_size<=0) goto done;
		pos += md->member_total_size;
	}

done:
	if(md) {
		de_arch_destroy_md(c, md);
		md = NULL;
	}
	if(d) {
		if(d->need_errmsg) {
			de_err(c, "Bad or unsupported CMZ file");
		}
		de_arch_destroy_lctx(c, d);
	}
}

static int de_identify_cmz(deark *c)
{
	i64 name_len;
	i64 cmpr_len;

	if(!cmz_sig_at(c, 0)) {
		return 0;
	}

	cmpr_len = de_getu32le(4);
	name_len = de_getu16le(16);
	if(cmpr_len==0 && name_len>0) {
		return 60;
	}
	if(dclimplode_header_at(c, 20+name_len)) {
		return 100;
	}

	return 0;
}

void de_module_cmz(deark *c, struct deark_module_info *mi)
{
	mi->id = "cmz";
	mi->desc = "CMZ installer archive";
	mi->run_fn = de_run_cmz;
	mi->identify_fn = de_identify_cmz;
}

// **************************************************************************
// SHR - PC-Install "PC-Shrink" format
// **************************************************************************

#define PCSHRINK_MINHEADERLEN 48

// Returns:
//  0 - not SHR
//  1 - old format
//  2 - new format
static int detect_pcshrink_internal(deark *c, u8 *pmultipart_flag)
{
	u8 b;
	UI n;
	i64 cmpr_len;

	*pmultipart_flag = 0;
	b = de_getbyte(13);
	if(b==0x74) { // maybe old format
		if(de_getbyte(0)!=0) return 0;
		if(de_getbyte(16)!=0x74) return 0;
		if(de_getbyte(56)==0) return 0; // 1st byte of filename
		cmpr_len = de_getu32le(76);
		if(cmpr_len!=0) {
			if(!dclimplode_header_at(c, 104)) return 0;
		}
		return 1;
	}
	else if(b==0) { // maybe new format
		if(de_getu16le(14)!=0x74) return 0;
		n = (UI)de_getu16le(18);
		if(n==0x75) {
			*pmultipart_flag = 1;
		}
		else if(n!=0x74) {
			return 0;
		}
		if(n==0x74) {
			if(de_getbyte(58)==0) return 0; // 1st byte of filename
			cmpr_len = de_getu32le(194);
			if(cmpr_len!=0) {
				if(!dclimplode_header_at(c, 226)) return 0;
			}
		}
		if(de_getbyte(0)!=0) {
			*pmultipart_flag = 1;
		}
		return 2;
	}
	return 0;
}

static int do_pcshrink_member(deark *c, de_arch_lctx *d, struct de_arch_member_data *md)
{
	i64 pos;
	int saved_indent_level;
	int retval = 0;
	i64 fnfieldlen;

	de_dbg_indent_save(c, &saved_indent_level);
	pos = md->member_hdr_pos;
	de_dbg(c, "member at %"I64_FMT, md->member_hdr_pos);
	de_dbg_indent(c, 1);

	if(d->fmtver==2) {
		fnfieldlen = 128;
	}
	else {
		fnfieldlen = 14;
	}
	dbuf_read_to_ucstring(c->infile, pos, fnfieldlen, md->filename,
		DE_CONVFLAG_STOP_AT_NUL, d->input_encoding);
	de_dbg(c, "filename: \"%s\"", ucstring_getpsz_d(md->filename));

	if(d->fmtver==2) {
		backslashes_to_slashes(md->filename);
		md->set_name_flags |= DE_SNFLAG_FULLPATH;
	}
	pos += fnfieldlen;

	pos++; // attributes?

	if(d->fmtver==2) {
		pos += 7;
	}
	else {
		pos += 5;
	}

	de_arch_read_field_cmpr_len_p(md, &pos);

	if(d->fmtver==2) {
		de_arch_read_field_dttm_p(d, &md->fi->timestamp[DE_TIMESTAMPIDX_MODIFY], "mod",
			DE_ARCH_TSTYPE_DOS_DXT, &pos);
	}
	else {
		de_arch_read_field_dttm_p(d, &md->fi->timestamp[DE_TIMESTAMPIDX_MODIFY], "mod",
			DE_ARCH_TSTYPE_DOS_DT, &pos);
	}

	if(d->fmtver==2) {
		md->cmpr_pos = md->member_hdr_pos + 168;
	}
	else {
		md->cmpr_pos = md->member_hdr_pos + 48;
	}

	de_dbg(c, "compressed data at %"I64_FMT", len=%"I64_FMT, md->cmpr_pos, md->cmpr_len);
	if(!de_arch_good_cmpr_data_pos(md)) {
		goto done;
	}

	md->member_total_size =  md->cmpr_pos + md->cmpr_len - md->member_hdr_pos;
	retval = 1;

	md->dfn = dclimplode_decompressor_fn;
	de_arch_extract_member_file(md);

done:
	de_dbg_indent_restore(c, saved_indent_level);
	return retval;
}

static void de_run_pcshrink(deark *c, de_module_params *mparams)
{
	de_arch_lctx *d = NULL;
	i64 pos = 0;
	i64 idx;
	u8 multipart_flag;
	struct de_arch_member_data *md = NULL;

	d = de_arch_create_lctx(c);
	d->is_le = 1;

	// Detect version
	d->fmtver = detect_pcshrink_internal(c, &multipart_flag);
	if(d->fmtver!=1 && d->fmtver!=2) {
		de_err(c, "Not a PC-Shrink file");
		goto done;
	}
	de_dbg(c, "format version: %d", d->fmtver);

	// Read archive header
	if(d->fmtver==2) {
		d->num_members = de_getu16le(16);
	}
	else {
		d->num_members = de_getu16le(14);
	}
	de_dbg(c, "number of members: %d", (int)d->num_members);

	// TODO: Can we tell Windows files from DOS files?
	d->input_encoding = de_get_input_encoding(c, NULL, DE_ENCODING_CP437);

	if(d->fmtver==2) {
		pos = 58;
	}
	else {
		pos = 56;
	}

	if(multipart_flag) {
		de_err(c, "Multi-part PC-Shrink files are not supported");
		goto done;
	}

	for(idx=0; idx<d->num_members; idx++) {
		if(pos+PCSHRINK_MINHEADERLEN >= c->infile->len) {
			de_err(c, "Unexpected end of file");
			goto done;
		}

		if(md) {
			de_arch_destroy_md(c, md);
			md = NULL;
		}
		md = de_arch_create_md(c, d);
		md->member_hdr_pos = pos;
		if(!do_pcshrink_member(c, d, md)) goto done;
		if(md->member_total_size<=0) goto done;
		pos += md->member_total_size;
	}

done:
	if(md) {
		de_arch_destroy_md(c, md);
		md = NULL;
	}
	if(d) {
		de_arch_destroy_lctx(c, d);
	}
}

static int de_identify_pcshrink(deark *c)
{
	u8 multipart_flag;
	int ver;

	ver = detect_pcshrink_internal(c, &multipart_flag);
	if(ver==1) {
		return 60;
	}
	else if(ver==2) {
		if(multipart_flag) return 40;
		return 80;
	}

	return 0;
}

void de_module_pcshrink(deark *c, struct deark_module_info *mi)
{
	mi->id = "pcshrink";
	mi->desc = "PC-Install compressed archive";
	mi->run_fn = de_run_pcshrink;
	mi->identify_fn = de_identify_pcshrink;
}

// **************************************************************************
// ARCV - Eschalon Setup / EDI Install
// **************************************************************************

// Warning: This ARCV code is not based on any specification. It may be wrong
// or misleading.

// This format was popular enough that I wanted to at least parse it, but I'm
// not optimistic about figuring out how to decompress it.
// Initial testing suggests LZ77 with a 4k window, possibly with adaptive
// Huffman coding or arithmetic coding. But it's not LZHUF or LZARI.

#define CODE_ARCV 0x41524356U
#define CODE_BLCK 0x424c434bU
#define CODE_CHNK 0x43484e4bU

static void do_arcv_common_fields(deark *c, de_arch_lctx *d,
	struct de_arch_member_data *md, i64 pos1, i64 *nbytes_consumed)
{
	i64 fnlen;
	UI fver_maj1, fver_min1;
	UI fver_maj2, fver_min2;
	i64 pos = pos1;
	UI attr;

	fnlen = (i64)de_getbyte_p(&pos);
	dbuf_read_to_ucstring(c->infile, pos, fnlen, md->filename, 0,
		d->input_encoding);
	de_dbg(c, "filename: \"%s\"", ucstring_getpsz_d(md->filename));
	pos += fnlen;

	de_arch_read_field_orig_len_p(md, &pos);
	de_arch_read_field_cmpr_len_p(md, &pos);

	attr = (UI)de_getu32le_p(&pos); // TODO: How big is this field
	de_arch_handle_field_dos_attr(md, attr);

	de_arch_read_field_dttm_p(d, &md->fi->timestamp[DE_TIMESTAMPIDX_MODIFY], "mod",
		DE_ARCH_TSTYPE_DOS_TD, &pos);

	fver_min1 = (UI)de_getu16le_p(&pos);
	fver_maj1 = (UI)de_getu16le_p(&pos);
	de_dbg(c, "file ver (1): %u.%u", fver_maj1, fver_min1);
	fver_min2 = (UI)de_getu16le_p(&pos);
	fver_maj2 = (UI)de_getu16le_p(&pos);
	de_dbg(c, "file ver (2): %u.%u", fver_maj2, fver_min2);

	// Algorithm seems to be "CRC-32/JAMCRC".
	// Same as the usual CRC-32, except it doesn't invert the bits as a final step.
	md->crc_reported = (u32)de_getu32le_p(&pos);
	de_dbg(c, "crc (reported): 0x%08x", (UI)md->crc_reported);

	*nbytes_consumed = pos - pos1;
}

static void do_arcv_v1(deark *c, de_arch_lctx *d)
{
	struct de_arch_member_data *md = NULL;
	i64 arcv_hdr_len;
	i64 chnk_pos;
	i64 chnk_hdr_len;
	i64 chnk_dlen;
	i64 pos;
	i64 nbytes_consumed = 0;
	u32 id;
	int saved_indent_level;

	de_dbg_indent_save(c, &saved_indent_level);
	md = de_arch_create_md(c, d);

	pos = 6;
	arcv_hdr_len = de_getu16le_p(&pos);
	de_dbg(c, "arcv hdr len: %"I64_FMT, arcv_hdr_len);
	d->archive_flags = (UI)de_getu32le_p(&pos);
	de_dbg(c, "flags: 0x%08x", (UI)d->archive_flags);

	do_arcv_common_fields(c, d, md, pos, &nbytes_consumed);

	pos = arcv_hdr_len; // Seek to the CHNK segment
	chnk_pos = pos;
	id = (u32)de_getu32be_p(&pos);
	if(id != CODE_CHNK) {
		d->need_errmsg = 1;
		goto done;
	}
	pos += 2; // segment version number?
	chnk_hdr_len = de_getu16le_p(&pos);
	de_dbg(c, "chnk header len: %"I64_FMT, chnk_hdr_len);
	pos += 4; // flags?
	chnk_dlen = de_getu32le_p(&pos);
	md->cmpr_pos = chnk_pos + chnk_hdr_len;
	de_dbg(c, "file data at %"I64_FMT", len=%"I64_FMT, md->cmpr_pos, chnk_dlen);

done:
	de_dbg_indent_restore(c, saved_indent_level);
	de_arch_destroy_md(c, md);
}

static void do_arcv_v2(deark *c, de_arch_lctx *d)
{
	i64 pos = 0;
	i64 arcv_hdr_len;
	i64 blck_hdr_len;
	i64 blck_dlen;
	u32 id;
	struct de_arch_member_data *md = NULL;
	int saved_indent_level;

	de_dbg_indent_save(c, &saved_indent_level);

	arcv_hdr_len = de_getu16le(6);
	de_dbg(c, "arcv hdr len: %"I64_FMT, arcv_hdr_len);

	pos = arcv_hdr_len;
	while(1) {
		i64 nbytes_consumed = 0;

		if(pos >= c->infile->len) {
			goto done;
		}
		de_dbg(c, "member at %"I64_FMT, pos);

		if(md) {
			de_arch_destroy_md(c, md);
			md = NULL;
		}
		md = de_arch_create_md(c, d);
		md->member_hdr_pos = pos;

		de_dbg_indent(c, 1);
		id = (u32)de_getu32be_p(&pos);
		if(id!=CODE_BLCK) {
			de_dbg(c, "can't find item at %"I64_FMT, md->member_hdr_pos);
			goto done;
		}
		pos += 2; // format version?
		blck_hdr_len = de_getu16le_p(&pos);
		de_dbg(c, "block hdr len: %"I64_FMT, blck_hdr_len);
		pos += 4; // ?

		blck_dlen = de_getu32le_p(&pos);
		de_dbg(c, "block dlen: %"I64_FMT, blck_dlen);

		pos = md->member_hdr_pos+16;

		do_arcv_common_fields(c, d, md, pos, &nbytes_consumed);
		pos += nbytes_consumed;

		md->cmpr_pos = pos;
		de_dbg(c, "file data at %"I64_FMT", len=%"I64_FMT, md->cmpr_pos, md->cmpr_len);

		pos = md->member_hdr_pos + blck_hdr_len + blck_dlen;
		de_dbg_indent(c, -1);
	}

done:
	de_dbg_indent_restore(c, saved_indent_level);
	if(md) {
		de_arch_destroy_md(c, md);
	}
}

static void de_run_arcv(deark *c, de_module_params *mparams)
{
	de_arch_lctx *d = NULL;
	UI ver_maj;

	d = de_arch_create_lctx(c);
	d->is_le = 1;
	d->input_encoding = de_get_input_encoding(c, NULL, DE_ENCODING_WINDOWS1252);

	d->fmtver = (int)de_getu16le(4);
	ver_maj = (UI)d->fmtver >> 8;
	de_dbg(c, "format ver: 0x%04x", (UI)d->fmtver);
	if(ver_maj==1) {
		do_arcv_v1(c, d);
	}
	else if(ver_maj==2) {
		do_arcv_v2(c, d);
	}
	else {
		d->need_errmsg = 1;
		goto done;
	}

done:
	if(d) {
		if(d->need_errmsg) {
			de_err(c, "Bad or unsupported ARCV file");
		}
		de_arch_destroy_lctx(c, d);
	}
}

static int de_identify_arcv(deark *c)
{
	u8 ver;

	if((UI)de_getu32be(0) != CODE_ARCV) return 0;
	ver = de_getbyte(5);
	if(ver==1 || ver==2) {
		return 100;
	}
	return 0;
}

void de_module_arcv(deark *c, struct deark_module_info *mi)
{
	mi->id = "arcv";
	mi->desc = "ARCV installer archive";
	mi->run_fn = de_run_arcv;
	mi->identify_fn = de_identify_arcv;
	mi->flags |= DE_MODFLAG_NONWORKING;
}
