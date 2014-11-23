// This file is part of Deark, by Jason Summers.
// This software is in the public domain. See the file COPYING for details.

// Old Print Shop and PrintMaster formats

#include <deark-config.h>
#include <deark-modules.h>

// **************************************************************************
// The Print Shop .DAT/.NAM format
// **************************************************************************

#define PRINTSHOP_FMT_DAT 0
#define PRINTSHOP_FMT_POG 1
#define PRINTSHOP_FMT_SHP 2

static void do_printshop_etc(deark *c, int fmt)
{
	de_int64 width, height;
	de_int64 rowspan;
	de_int64 imgspan;
	de_int64 num_images;
	de_int64 i;
	de_int64 imgoffset = 0;
	de_int64 headersize = 0;

	width = 88;
	height = 52;
	rowspan = (width+7)/8;

	switch(fmt) {
	case PRINTSHOP_FMT_POG: // The New Print Shop .POG
		headersize = 10;
		imgspan = rowspan * height;
		break;
	case PRINTSHOP_FMT_SHP: // PrintMaster .SHP
		imgoffset = 4;
		imgspan = 4 + rowspan * height + 1;
	default:
		// Print Shop .DAT
		imgspan = rowspan * height;
	}

	num_images = c->infile->len / imgspan;
	if(num_images>DE_MAX_IMAGES_PER_FILE) {
		return;
	}

	// TODO: Read the .NAM file
	for(i=0; i<num_images; i++) {
		de_convert_and_write_image_bilevel(c->infile, headersize + i*imgspan + imgoffset,
			width, height, rowspan, DE_CVTF_WHITEISZERO);
	}
}

static void de_run_printshop(deark *c, const char *params)
{
	de_declare_fmt(c, "The Print Shop (DAT)");
	do_printshop_etc(c, PRINTSHOP_FMT_DAT);
}

static int de_identify_printshop(deark *c)
{
	if(de_input_file_has_ext(c, "dat")) {
		if((c->infile->len % 572)==0) {
			return 10;
		}
	}
	return 0;
}

void de_module_printshop(deark *c, struct deark_module_info *mi)
{
	mi->id = "printshop";
	mi->run_fn = de_run_printshop;
	mi->identify_fn = de_identify_printshop;
}

// **************************************************************************
// The New Print Shop .POG/.PNM format
// **************************************************************************

static void de_run_newprintshop(deark *c, const char *params)
{
	de_declare_fmt(c, "The New Print Shop (POG)");
	do_printshop_etc(c, PRINTSHOP_FMT_POG);
}

static int de_identify_newprintshop(deark *c)
{
	if(de_input_file_has_ext(c, "pog")) {
		if((c->infile->len % 572)==10) {
			return 90;
		}
		else {
			return 10;
		}
	}
	return 0;
}

void de_module_newprintshop(deark *c, struct deark_module_info *mi)
{
	// There's no surefire way to distinguish between Print Shop and
	// New Print Shop files, so it's more convenient to put them in separate
	// modules (so the user can simply use -m to select the format).
	mi->id = "newprintshop";
	mi->run_fn = de_run_newprintshop;
	mi->identify_fn = de_identify_newprintshop;
}

// **************************************************************************
// PrintMaster .SHP/.SDR format
// **************************************************************************

static void de_run_printmaster(deark *c, const char *params)
{
	de_declare_fmt(c, "PrintMaster (SHP)");
	do_printshop_etc(c, PRINTSHOP_FMT_SHP);
}

static int de_identify_printmaster(deark *c)
{
	de_byte b[4];
	// TODO: Verify that this signature is correct.
	de_read(b, 0, 4);
	if(!de_memcmp(b, "\xb0\x34\x58", 3))
		return 90;
	return 0;
}

void de_module_printmaster(deark *c, struct deark_module_info *mi)
{
	mi->id = "printmaster";
	mi->run_fn = de_run_printmaster;
	mi->identify_fn = de_identify_printmaster;
}
