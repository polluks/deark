// This file is part of Deark, by Jason Summers.
// This software is in the public domain. See the file COPYING for details.

// ANSI art
// (very incomplete)

#include <deark-config.h>
#include <deark-modules.h>

struct cell_struct {
	de_int32 codepoint;
	de_byte fgcol;
	de_byte bgcol;
};

typedef struct localctx_struct {
	dbuf *ofile;

	int width, height;
	struct cell_struct *cells; // Array of height*width cells

	de_int64 xpos, ypos; // 0-based
	de_int64 saved_xpos, saved_ypos;

	de_byte curr_fgcol;
	de_byte curr_bgcol;

	de_byte param_string_buf[100];

#define MAX_ESC_PARAMS 16
	int num_params;
	de_int64 params[MAX_ESC_PARAMS];
} lctx;

static const de_uint32 ansi_palette[16] = {
	0x000000,0xaa0000,0x00aa00,0xaa5500,0x0000aa,0xaa00aa,0x00aaaa,0xaaaaaa,
	0x555555,0xff5555,0x55ff55,0xffff55,0x5555ff,0xff55ff,0x55ffff,0xffffff
};

static struct cell_struct *get_cell_at(deark *c, lctx *d, de_int64 xpos, de_int64 ypos)
{
	if(xpos<0 || ypos<0) return NULL;
	if(xpos>=d->width || ypos>=d->height) return NULL;
	return &d->cells[ypos*d->width + xpos];
}

static void do_normal_char(deark *c, lctx *d, de_byte ch)
{
	struct cell_struct *cell;
	de_int32 u;

	if(ch==13) { // CR
		d->xpos = 0;
	}
	else if(ch==10) { // LF
		d->ypos++;
	}
	else if(ch>=32) {
		u = de_cp437g_to_unicode(c, (int)ch);

		cell = get_cell_at(c, d, d->xpos, d->ypos);
		if(cell) {
			cell->codepoint = u;
			cell->fgcol = d->curr_fgcol;
			cell->bgcol = d->curr_bgcol;
		}

		d->xpos++;
	}
}

// Convert d->param_string_buf to d->params and d->num_params.
static void parse_params(deark *c, lctx *d, de_int64 default_val)
{
	de_int64 buf_len;
	de_int64 ppos;
	de_int64 param_len;
	char *p_ptr;
	int last_param = 0;

	d->num_params = 0;

	buf_len = de_strlen((const char*)d->param_string_buf);

	ppos = 0;
	while(1) {
		if(d->num_params >= MAX_ESC_PARAMS) {
			break;
		}

		p_ptr = de_strchr((const char*)&d->param_string_buf[ppos], ';');
		if(p_ptr) {
			param_len = p_ptr - (char*)&d->param_string_buf[ppos];
		}
		else {
			param_len = buf_len - ppos;
			last_param = 1;
		}

		if(param_len>=1) {
			d->params[d->num_params] = de_atoi64((const char*)&d->param_string_buf[ppos]);
		}
		else {
			d->params[d->num_params] = default_val;
		}
		d->num_params++;

		if(last_param) {
			break;
		}

		// Advance past the parameter data and the terminating semicolon.
		ppos += param_len + 1;
	}

}

static void read_one_int(deark *c, lctx *d, const de_byte *buf,
  de_int64 *a, de_int64 a_default)
{
	parse_params(c, d, a_default);

	if(d->num_params>=1) {
		*a = d->params[0];
	}
	else {
		*a = a_default;
	}
}

// m - Select Graphic Rendition
static void do_code_m(deark *c, lctx *d)
{
	de_int64 i;
	de_int64 sgi_code;

	parse_params(c, d, 0);

	for(i=0; i<d->num_params; i++) {
		sgi_code = d->params[i];

		if(sgi_code>=30 && sgi_code<=37) {
			// Set foreground color
			d->curr_fgcol = (de_byte)(sgi_code-30);
		}
		else if(sgi_code>=40 && sgi_code<=47) {
			// Set background color
			d->curr_bgcol = (de_byte)(sgi_code-40);
		}
	}
}

// H: Set cursor position
static void do_code_H(deark *c, lctx *d)
{
	de_int64 row, col;

	parse_params(c, d, 1);

	if(d->num_params>=1) row = d->params[0];
	else row = 1;

	if(d->num_params>=2) col = d->params[1];
	else col = 1;

	d->xpos = col-1;
	d->ypos = row-1;
}

// A: Up
static void do_code_A(deark *c, lctx *d)
{
	de_int64 n;
	read_one_int(c, d, d->param_string_buf, &n, 1);
	d->ypos -= n;
}

// B: Down
static void do_code_B(deark *c, lctx *d)
{
	de_int64 n;
	read_one_int(c, d, d->param_string_buf, &n, 1);
	d->ypos += n;
}

// C: Forward
static void do_code_C(deark *c, lctx *d)
{
	de_int64 n;
	read_one_int(c, d, d->param_string_buf, &n, 1);
	d->xpos += n;
}

// D: Back
static void do_code_D(deark *c, lctx *d)
{
	de_int64 n;
	read_one_int(c, d, d->param_string_buf, &n, 1);
	d->xpos -= n;
}

static void do_control_sequence(deark *c, lctx *d, de_byte code,
	de_int64 param_start, de_int64 param_len)
{
	de_dbg2(c, "[%c at %d %d]\n", (char)code, (int)param_start, (int)param_len);

	if(param_len > sizeof(d->param_string_buf)-1) {
		de_warn(c, "Ignoring long escape sequence (len %d at %d)\n",
			(int)param_len, (int)param_start);
		return;
	}

	de_read(d->param_string_buf, param_start, param_len);
	d->param_string_buf[param_len] = '\0';

	switch(code) {
	case 'A': do_code_A(c, d); break;
	case 'B': do_code_B(c, d); break;
	case 'C': do_code_C(c, d); break;
	case 'D': do_code_D(c, d); break;
	case 'H': do_code_H(c, d); break;
	case 'm': do_code_m(c, d); break;
	case 's':
		d->saved_xpos = d->xpos;
		d->saved_ypos = d->ypos;
		break;
	case 'u':
		d->xpos = d->saved_xpos;
		d->ypos = d->saved_ypos;
		break;
	default:
		de_dbg(c, "[unsupported escape sequence %c]\n", (char)code);
	}
}

static void do_main(deark *c, lctx *d)
{
	de_int64 pos;
	de_int64 params_start_pos = 0;
#define STATE_NORMAL 0
#define STATE_GOT_ESC 1
#define STATE_READING_PARAM 2
	int state;
	de_byte ch;

	d->xpos = 0; d->ypos = 0;
	state = STATE_NORMAL;

	for(pos=0; pos<c->infile->len; pos++) {
		ch = de_getbyte(pos);

		if(state==STATE_NORMAL) {
			if(ch==0x1b) { // ESC
				state=STATE_GOT_ESC;
				continue;
			}
			else if(ch==0x9b) {
				state=STATE_READING_PARAM;
				params_start_pos = pos+1;
				continue;
			}
			else { // a non-escape character
				do_normal_char(c, d, ch);
			}
		}
		else if(state==STATE_GOT_ESC) {
			if(ch=='[') {
				state=STATE_READING_PARAM;
				params_start_pos = pos+1;
				continue;
			}
			else if(ch>=64 && ch<=95) {
				// A 2-character escape sequence
				state=STATE_NORMAL;
				continue;
			}
		}
		else if(state==STATE_READING_PARAM) {
			// Control sequences end with a byte from 64-126
			if(ch>=64 && ch<=126) {
				do_control_sequence(c, d, ch, params_start_pos, pos-params_start_pos);
				state=STATE_NORMAL;
				continue;
			}
		}
	}
}

static void ansi_16_color_to_css(int index, char *buf, int buflen)
{
	de_uint32 clr;

	if(index>=0 && index<16) clr = ansi_palette[index];
	else clr = 0;

	de_color_to_css(clr, buf, buflen);
}

static void do_output_main(deark *c, lctx *d)
{
	const struct cell_struct *cell;
	int i, j;
	de_int32 n;
	int span_count = 0;
	de_byte active_fgcol = 0;
	de_byte active_bgcol = 0;
	char fgcol_css[16];
	char bgcol_css[16];

	dbuf_fputs(d->ofile, "<pre>\n");
	for(j=0; j<d->height; j++) {
		for(i=0; i<d->width; i++) {

			cell = get_cell_at(c, d, i, j);
			if(!cell) continue;

			if(cell->fgcol!=active_fgcol || cell->bgcol!=active_bgcol) {
				while(span_count>0) {
					dbuf_fprintf(d->ofile, "</span>");
					span_count--;
				}

				ansi_16_color_to_css(cell->fgcol, fgcol_css, sizeof(fgcol_css));
				ansi_16_color_to_css(cell->bgcol, bgcol_css, sizeof(bgcol_css));
				dbuf_fprintf(d->ofile, "<span style='color:%s;background-color:%s'>", fgcol_css, bgcol_css);
				span_count++;
				active_fgcol = cell->fgcol;
				active_bgcol = cell->bgcol;

			}

			n = cell->codepoint;
			if(n==0x00) n=0x20;
			if(n<0x20) n='?';

			if(n=='<' || n=='>' || n>126) {
				dbuf_fprintf(d->ofile, "&#%d;", (int)n);
			}
			else {
				dbuf_writebyte(d->ofile, (de_byte)n);
			}
		}
		dbuf_fputs(d->ofile, "\n");
	}

	while(span_count>0) {
		dbuf_fprintf(d->ofile, "</span>");
		span_count--;
	}

	dbuf_fputs(d->ofile, "</pre>\n");
}

static void do_header(deark *c, lctx *d)
{
	d->ofile = dbuf_create_output_file(c, "html", NULL);

	dbuf_fputs(d->ofile, "<!DOCTYPE html>\n");
	dbuf_fputs(d->ofile, "<html>\n");
	dbuf_fputs(d->ofile, "<head>\n");
	dbuf_fputs(d->ofile, "<title></title>\n");
	dbuf_fputs(d->ofile, "</head>\n");
	dbuf_fputs(d->ofile, "<body>\n");
}

static void do_footer(deark *c, lctx *d)
{
	dbuf_fputs(d->ofile, "</body>\n</html>\n");
	dbuf_close(d->ofile);
	d->ofile = NULL;
}

static void de_run_ansiart(deark *c, const char *params)
{
	lctx *d = NULL;

	d = de_malloc(c, sizeof(lctx));
	do_header(c, d);

	d->width = 80;
	d->height = 25;
	d->cells = de_malloc(c, sizeof(struct cell_struct)*d->height*d->width);

	do_main(c, d);
	do_output_main(c, d);
	do_footer(c, d);

	de_free(c, d->cells);
	de_free(c, d);
}

static int de_identify_ansiart(deark *c)
{
	if(de_input_file_has_ext(c, "ans"))
		return 10;
	return 0;
}

void de_module_ansiart(deark *c, struct deark_module_info *mi)
{
	mi->id = "ansiart";
	mi->run_fn = de_run_ansiart;
	mi->identify_fn = de_identify_ansiart;
	mi->flags |= DE_MODFLAG_NONWORKING;
}
