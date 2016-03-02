// This file is part of Deark, by Jason Summers.
// This software is in the public domain. See the file COPYING for details.

// Extract comments from ZIP files.

#include <deark-config.h>
#include <deark-modules.h>

typedef struct localctx_struct {
	de_int64 end_of_central_dir_pos;
	de_int64 central_dir_num_entries;
	de_int64 central_dir_byte_size;
	de_int64 central_dir_offset;
} lctx;

typedef void (*extrafield_decoder_fn)(deark *c, lctx *d, de_int64 fieldtype, de_int64 pos,
	de_int64 len, int is_central);

// Write a buffer to a file, converting the encoding.
static void copy_cp437c_to_utf8(deark *c, const de_byte *buf, de_int64 len, dbuf *outf)
{
	de_int32 u;
	de_int64 i;

	for(i=0; i<len; i++) {
		u = de_char_to_unicode(c, (de_int32)buf[i], DE_ENCODING_CP437_C);
		dbuf_write_uchar_as_utf8(outf, u);
	}
}

static int detect_bom(dbuf *f, de_int64 pos)
{
	de_byte buf[3];

	dbuf_read(f, buf, pos, 3);
	if(buf[0]==0xef && buf[1]==0xbb && buf[2]==0xbf) {
		return 1;
	}
	return 0;
}

static void do_comment(deark *c, lctx *d, de_int64 pos, de_int64 len, int utf8_flag,
	const char *ext)
{
	de_byte *comment = NULL;
	dbuf *f = NULL;

	if(len<1) return;

	comment = de_malloc(c, len);
	de_read(comment, pos, len);

	f = dbuf_create_output_file(c, ext, NULL);

	if(de_is_ascii(comment, len)) {
		// No non-ASCII characters, so write the comment as-is.
		dbuf_write(f, comment, len);
	}
	else if(utf8_flag) {

		// Comment is already UTF-8. Copy as-is, but maybe add a BOM.

		if(c->write_bom) {
			int already_has_bom = 0;

			// A UTF-8 comment is not expected to have a BOM, but just in case it does,
			// make sure we don't add a second one.
			if(len>=3) {
				already_has_bom = detect_bom(c->infile, pos);
			}

			if(!already_has_bom) {
				dbuf_write_uchar_as_utf8(f, 0xfeff);
			}
		}

		dbuf_write(f, comment, len);
	}
	else {
		// Convert the comment to UTF-8.

		if(c->write_bom) {
			// Write a BOM.
			dbuf_write_uchar_as_utf8(f, 0xfeff);
		}

		copy_cp437c_to_utf8(c, comment, len, f);
	}

	dbuf_close(f);
	de_free(c, comment);
}

static void ef_extended_timestamp(deark *c, lctx *d, de_int64 fieldtype,
	de_int64 pos, de_int64 len, int is_central)
{
	de_byte flags;
	de_int64 endpos;
	int has_mtime, has_atime, has_ctime;
	de_int64 mtime, atime, ctime;

	endpos = pos+len;
	if(pos+1>endpos) return;
	flags = de_getbyte(pos);
	pos++;
	has_mtime = (flags & 0x01)?1:0;
	has_atime = (!is_central) && (flags & 0x02);
	has_ctime = (!is_central) && (flags & 0x04);
	if(has_mtime) {
		if(pos+4>endpos) return;
		mtime = dbuf_geti32le(c->infile, pos);
		pos+=4;
		de_dbg(c, "mtime: %d\n", (int)mtime);
	}
	if(has_atime) {
		if(pos+4>endpos) return;
		atime = dbuf_geti32le(c->infile, pos);
		pos+=4;
		de_dbg(c, "atime: %d\n", (int)atime);
	}
	if(has_ctime) {
		if(pos+4>endpos) return;
		ctime = dbuf_geti32le(c->infile, pos);
		pos+=4;
		de_dbg(c, "ctime: %d\n", (int)ctime);
	}
}

struct extra_item_type_info_struct {
	de_uint16 id;
	const char *name;
	extrafield_decoder_fn fn;
};
static const struct extra_item_type_info_struct extra_item_type_info_arr[] = {
	{ 0x0001 /*    */, "Zip64 extended information", NULL },
	{ 0x0007 /*    */, "AV Info", NULL },
	{ 0x0008 /*    */, "extended language encoding data", NULL },
	{ 0x0009 /*    */, "OS/2", NULL },
	{ 0x000a /*    */, "NTFS", NULL },
	{ 0x000c /*    */, "OpenVMS", NULL },
	{ 0x000d /*    */, "Unix", NULL },
	{ 0x000e /*    */, "file stream and fork descriptors", NULL },
	{ 0x000f /*    */, "Patch Descriptor", NULL },
	{ 0x0014 /*    */, "PKCS#7 Store for X.509 Certificates", NULL },
	{ 0x0015 /*    */, "X.509 Certificate ID and Signature for individual file", NULL },
	{ 0x0016 /*    */, "X.509 Certificate ID for Central Directory", NULL },
	{ 0x0017 /*    */, "Strong Encryption Header", NULL },
	{ 0x0018 /*    */, "Record Management Controls", NULL },
	{ 0x0019 /*    */, "PKCS#7 Encryption Recipient Certificate List", NULL },
	{ 0x0065 /*    */, "IBM S/390 (Z390), AS/400 (I400) attributes", NULL },
	{ 0x0066 /*    */, "IBM S/390 (Z390), AS/400 (I400) attributes - compressed", NULL },
	{ 0x07c8 /*    */, "Macintosh", NULL },
	{ 0x2605 /*    */, "ZipIt Macintosh", NULL },
	{ 0x2705 /*    */, "ZipIt Macintosh 1.3.5+", NULL },
	{ 0x2805 /*    */, "ZipIt Macintosh 1.3.5+", NULL },
	{ 0x334d /* M3 */, "Info-ZIP Macintosh", NULL },
	{ 0x4154 /* TA */, "Tandem NSK", NULL },
	{ 0x4341 /* AC */, "Acorn/SparkFS", NULL },
	{ 0x4453 /* SE */, "Windows NT security descriptor (binary ACL)", NULL },
	{ 0x4690 /*    */, "POSZIP 4690", NULL },
	{ 0x4704 /*    */, "VM/CMS", NULL },
	{ 0x470f /*    */, "MVS", NULL },
	{ 0x4854 /* TH */, "Theos, old inofficial port", NULL },
	{ 0x4b46 /* FK */, "FWKCS MD5", NULL },
	{ 0x4c41 /* AL */, "OS/2 access control list (text ACL)", NULL },
	{ 0x4d49 /* IM */, "Info-ZIP OpenVMS", NULL },
	{ 0x4d63 /* cM */, "Macintosh SmartZIP", NULL },
	{ 0x4f4c /* LO */, "Xceed original location extra field", NULL },
	{ 0x5350 /* PS */, "Psion?", NULL }, // observed in some Psion files
	{ 0x5356 /* VS */, "AOS/VS (ACL)", NULL },
	{ 0x5455 /* UT */, "extended timestamp", ef_extended_timestamp },
	{ 0x554e /* NU */, "Xceed unicode extra field", NULL },
	{ 0x5855 /* UX */, "Info-ZIP Unix, first version", NULL },
	{ 0x6375 /* uc */, "Info-ZIP Unicode Comment", NULL },
	{ 0x6542 /* Be */, "BeOS/BeBox", NULL },
	{ 0x6854 /* Th */, "Theos", NULL },
	{ 0x7075 /* up */, "Info-ZIP Unicode Path", NULL },
	{ 0x7441 /* At */, "AtheOS", NULL },
	{ 0x756e /* nu */, "ASi Unix", NULL },
	{ 0x7855 /* Ux */, "Info-ZIP Unix, second version", NULL },
	{ 0x7875 /* ux */, "Info-ZIP Unix, third version", NULL },
	{ 0xa220 /*    */, "Microsoft Open Packaging Growth Hint", NULL },
	{ 0xfb4a /*    */, "SMS/QDOS", NULL }, // according to Info-ZIP zip 3.0
	{ 0xfd4a /*    */, "SMS/QDOS", NULL }, // according to ZIP v6.3.4 APPNOTE
	{ 0x0000, NULL, NULL }
};

static const struct extra_item_type_info_struct *get_extra_item_type_info(de_int64 id)
{
	de_int64 i;
	i=0;
	for(i=0; extra_item_type_info_arr[i].name!=NULL; i++) {
		if(id == (de_int64)extra_item_type_info_arr[i].id) {
			return &extra_item_type_info_arr[i];
		}
	}
	return NULL;
}

static void do_extra_data(deark *c, lctx *d, de_int64 pos1, de_int64 len,
	int is_central)
{
	de_int64 pos;
	de_int64 item_id;
	de_int64 item_len;
	const char *item_name;
	const struct extra_item_type_info_struct *ei;

	de_dbg(c, "extra data at %d, len=%d\n", (int)pos1, (int)len);
	de_dbg_indent(c, 1);

	pos = pos1;
	while(1) {
		if(pos+4 >= pos1+len) break;
		item_id = de_getui16le(pos);
		item_len = de_getui16le(pos+2);

		ei = get_extra_item_type_info(item_id);
		item_name = "?";
		if(ei && ei->name) item_name = ei->name;

		de_dbg(c, "item id=0x%04x (%s), dlen=%d\n", (unsigned int)item_id, item_name,
			(int)item_len);
		if(pos+4+item_len > pos1+len) break;

		if(ei->fn) {
			de_dbg_indent(c, 1);
			ei->fn(c, d, item_id, pos+4, item_len, is_central);
			de_dbg_indent(c, -1);
		}

		pos += 4+item_len;
	}

	de_dbg_indent(c, -1);
}

static const char *get_platform_name(unsigned int ver_hi)
{
	static const char *pltf_names[20] = {
		"MS-DOS, etc.", "Amiga", "OpenVMS", "Unix",
		"VM/CMS", "Atari ST", "HPFS", "Macintosh",
		"Z-System", "CP/M", "NTFS or TOPS-20", "MVS or NTFS",
		"VSE or SMS/QDOS", "Acorn RISC OS", "VFAT", "MVS",
		"BeOS", "Tandem", "OS/400", "OS X" };

	if(ver_hi<20)
		return pltf_names[ver_hi];
	if(ver_hi==30) return "AtheOS/Syllable";
	return "?";
}

// Read either a central directory entry (a.k.a. central directory file header),
// or a local file header.
static int do_file_header(deark *c, lctx *d, int is_central, de_int64 central_index,
	de_int64 pos1, de_int64 *p_entry_size)
{
	de_int64 pos;
	int ret;
	de_int64 sig;
	de_int64 cmpr_method;
	unsigned int bit_flags;
	de_int64 size1, size2;
	de_int64 ver_made_by;
	de_int64 ver_needed;
	unsigned int ver_hi, ver_lo;
	de_int64 fn_len, extra_len, comment_len;
	int utf8_flag;
	int retval = 0;
	de_int64 fixed_header_size;
	de_int64 offset_of_local_header  = 0;
	de_int64 disk_number_start = 0;

	pos = pos1;
	if(is_central) {
		fixed_header_size = 46;
		de_dbg(c, "central dir entry #%d at %d\n", (int)central_index, (int)pos);
	}
	else {
		fixed_header_size = 30;
		de_dbg(c, "local file header at %d\n", (int)pos);
	}
	de_dbg_indent(c, 1);

	sig = de_getui32le(pos);
	pos += 4;
	if(is_central && sig!=0x02014b50) {
		de_err(c, "Invalid central file header at %d\n", (int)pos1);
		goto done;
	}
	else if(!is_central && sig!=0x04034b50) {
		de_err(c, "Invalid local file header at %d\n", (int)pos1);
		goto done;
	}

	if(is_central) {
		const char *pltf_name;
		ver_made_by = de_getui16le(pos);
		pos += 2;
		ver_hi = (unsigned int)((ver_made_by%0xff00)>>8);
		ver_lo = (unsigned int)(ver_made_by%0x00ff);
		pltf_name = get_platform_name(ver_hi);
		de_dbg(c, "version made by: platform=%u (%s), ZIP spec=%u.%u\n",
			ver_hi, pltf_name,
			(unsigned int)(ver_lo/10), (unsigned int)(ver_lo%10));
	}

	ver_needed = de_getui16le(pos);
	pos += 2;
	ver_lo = (unsigned int)(ver_needed%0x00ff);
	de_dbg(c, "version needed to extract: %u.%u\n",
		(unsigned int)(ver_lo/10), (unsigned int)(ver_lo%10));

	bit_flags = (unsigned int)de_getui16le(pos);
	pos += 2;
	de_dbg(c, "flags: 0x%04x\n", bit_flags);

	utf8_flag = (bit_flags & 0x800)?1:0;

	cmpr_method = de_getui16le(pos);
	pos += 2;
	de_dbg(c, "cmpr method: %d\n", (int)cmpr_method);

	pos += 4; // last mod time & date
	pos += 4; // crc-32

	size1 = de_getui32le(pos); // compressed size
	pos += 4;
	size2 = de_getui32le(pos); // uncompressed size
	pos += 4;
	de_dbg(c, "cmpr size: %" INT64_FMT ", uncmpr size: %" INT64_FMT "\n", size1, size2);

	fn_len = de_getui16le(pos);
	pos += 2;

	extra_len = de_getui16le(pos);
	pos += 2;

	if(is_central) {
		comment_len = de_getui16le(pos);
		pos += 2;
	}
	else {
		comment_len = 0;
	}

	if(is_central) {
		disk_number_start = de_getui16le(pos);
		pos += 2;
		pos += 2; // internal file attributes
		pos += 4; // external file attributes
		offset_of_local_header = de_getui32le(pos);
		pos += 4;
		de_dbg(c, "offset of local header: %d, disk: %d\n", (int)offset_of_local_header,
			(int)disk_number_start);
	}

	if(is_central) {
		de_dbg(c, "filename_len=%d, extra_len=%d, comment_len=%d\n", (int)fn_len,
			(int)extra_len, (int)comment_len);
	}
	else {
		de_dbg(c, "filename_len=%d, extra_len=%d\n", (int)fn_len,
			(int)extra_len);
	}

	*p_entry_size = fixed_header_size + fn_len + extra_len + comment_len;

	if(extra_len>0) {
		do_extra_data(c, d, pos1+fixed_header_size+fn_len, extra_len, is_central);
	}

	if(comment_len>0) {
		do_comment(c, d, pos1+fixed_header_size+fn_len+extra_len, comment_len, utf8_flag, "fcomment.txt");
	}

	if(is_central && disk_number_start==0) {
		// Read the corresponding local file header
		de_int64 tmp_entry_size = 0;
		ret = do_file_header(c, d, 0, central_index, offset_of_local_header, &tmp_entry_size);
		if(!ret) goto done;
	}

	retval = 1;

done:
	de_dbg_indent(c, -1);
	return retval;
}

static int do_central_dir(deark *c, lctx *d)
{
	de_int64 i;
	de_int64 pos;
	de_int64 entry_size;
	int retval = 0;

	pos = d->central_dir_offset;
	de_dbg(c, "central dir at %d\n", (int)pos);
	de_dbg_indent(c, 1);

	for(i=0; i<d->central_dir_num_entries; i++) {
		if(pos >= d->central_dir_offset+d->central_dir_byte_size) {
			goto done;
		}

		if(!do_file_header(c, d, 1, i, pos, &entry_size)) {
			goto done;
		}

		pos += entry_size;
	}
	retval = 1;

done:
	de_dbg_indent(c, -1);
	return retval;
}

static int do_end_of_central_dir(deark *c, lctx *d)
{
	de_int64 pos;
	de_int64 this_disk_num;
	de_int64 num_entries_this_disk;
	de_int64 disk_num_with_central_dir_start;
	de_int64 comment_length;
	int retval = 0;

	pos = d->end_of_central_dir_pos;
	de_dbg(c, "end-of-central-dir record at %d\n", (int)pos);
	de_dbg_indent(c, 1);

	this_disk_num = de_getui16le(pos+4);
	de_dbg(c, "this disk num: %d\n", (int)this_disk_num);
	disk_num_with_central_dir_start = de_getui16le(pos+6);
	de_dbg(c, "disk with central dir start: %d\n", (int)disk_num_with_central_dir_start);
	num_entries_this_disk = de_getui16le(pos+8);

	d->central_dir_num_entries = de_getui16le(pos+10);
	d->central_dir_byte_size  = de_getui32le(pos+12);
	d->central_dir_offset = de_getui32le(pos+16);

	de_dbg(c, "central dir: num_entries=%d, offset=%d, size=%d\n",
		(int)d->central_dir_num_entries,
		(int)d->central_dir_offset,
		(int)d->central_dir_byte_size);

	comment_length = de_getui16le(pos+20);
	de_dbg(c, "comment length: %d\n", (int)comment_length);
	if(comment_length>0) {
		// The comment for the whole .ZIP file presumably has to use
		// cp437 encoding. There's no flag that could indicate otherwise.
		do_comment(c, d, pos+22, comment_length, 0, "comment.txt");
	}

	// TODO: Figure out exactly how to detect disk spanning.
	if(this_disk_num!=0 || disk_num_with_central_dir_start!=0 ||
		num_entries_this_disk!=d->central_dir_num_entries)
	{
		de_err(c, "Disk spanning not supported\n");
		goto done;
	}

	retval = 1;

done:
	de_dbg_indent(c, -1);
	return retval;
}

static int find_end_of_central_dir(deark *c, lctx *d)
{
	de_int64 x;
	de_byte *buf = NULL;
	int retval = 0;
	de_int64 buf_offset;
	de_int64 buf_size;
	de_int64 i;

	if(c->infile->len < 22) goto done;

	// End-of-central-dir record usually starts 22 bytes from EOF. Try that first.
	x = de_getui32le(c->infile->len - 22);
	if(x == 0x06054b50) {
		d->end_of_central_dir_pos = c->infile->len - 22;
		retval = 1;
		goto done;
	}

	// Search for the signature.
	// The end-of-central-directory record could theoretically appear anywhere
	// in the file. We'll follow Info-Zip/UnZip's lead and search the last 66000
	// bytes.
#define MAX_EOCD_SEARCH 66000
	buf_size = c->infile->len;
	if(buf_size > MAX_EOCD_SEARCH) buf_size = MAX_EOCD_SEARCH;

	buf = de_malloc(c, buf_size);
	buf_offset = c->infile->len - buf_size;
	de_read(buf, buf_offset, buf_size);

	for(i=buf_size-22; i>=0; i--) {
		if(buf[i]=='P' && buf[i+1]=='K' && buf[i+2]==5 && buf[i+3]==6) {
			d->end_of_central_dir_pos = buf_offset + i;
			retval = 1;
			goto done;
		}
	}

done:
	de_free(c, buf);
	return retval;
}

static void de_run_zip(deark *c, de_module_params *mparams)
{
	lctx *d = NULL;

	d = de_malloc(c, sizeof(lctx));

	de_declare_fmt(c, "ZIP (extract comments only)");

	if(!find_end_of_central_dir(c, d)) {
		de_err(c, "Not a ZIP file\n");
		goto done;
	}

	de_dbg(c, "end-of-central-dir record signature found at %d\n", (int)d->end_of_central_dir_pos);

	if(!do_end_of_central_dir(c, d)) {
		goto done;
	}

	if(!do_central_dir(c, d)) {
		goto done;
	}

done:
	de_free(c, d);
}

static int de_identify_zip(deark *c)
{
	de_byte b[4];

	// This will not detect every ZIP file, but there is no cheap way to do that.

	de_read(b, 0, 4);
	if(!de_memcmp(b, "PK\x03\x04", 4)) {
		return 90;
	}

	if(c->infile->len >= 22) {
		de_read(b, c->infile->len - 22, 4);
		if(!de_memcmp(b, "PK\x05\x06", 4)) {
			return 90;
		}
	}

	return 0;
}

void de_module_zip(deark *c, struct deark_module_info *mi)
{
	mi->id = "zip";
	mi->desc = "ZIP archive (extract comments only)";
	mi->run_fn = de_run_zip;
	mi->identify_fn = de_identify_zip;
}
