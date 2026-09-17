/*
 * ========================================================================
 * Copyright 2026 Adam DePrince
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 * ========================================================================
 *
 * Program:	Basic HTML markup for the pico composer
 *
 * Alt-B/I/U wrap bold/italic/underline.  Lists turn the current
 * line or marked lines into <li> items inside <ul> or <ol>.
 */

#include "headers.h"
#include "htmlfmt.h"
#include "../pith/charconv/utf8.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>


static int	html_ins_utf8(char *);
static int	html_wrap(char *, char *, char *);
static int	html_wrap_current_line(char *, char *, char *);
static int	html_surround_lines(char *, char *, int, char *);
static LINE    *html_region_end_line(REGION *);
static int	html_line_empty(LINE *);
static int	html_line_starts(LINE *, char *);
static void	html_goto_line(LINE *);


static int
html_ins_utf8(char *s)
{
    UCS *u;
    int  i;

    if(!s || !*s)
      return(TRUE);

    u = utf8_to_ucs4_cpystr(s);
    if(!u)
      return(FALSE);

    for(i = 0; u[i]; i++){
	if(u[i] == '\n'){
	    if(lnewline() != TRUE){
		fs_give((void **) &u);
		return(FALSE);
	    }
	}
	else if(linsert(1, u[i]) != TRUE){
	    fs_give((void **) &u);
	    return(FALSE);
	}
    }

    fs_give((void **) &u);
    return(TRUE);
}


static int
html_wrap(char *open, char *close, char *what)
{
    REGION region;
    UCS   *cu;
    int    i, clen = 0;

    if(curbp->b_mode & MDVIEW)
      return(rdonly());

    if(getregion(&region, curwp->w_markp, curwp->w_marko) == TRUE){
	curwp->w_dotp = region.r_linep;
	curwp->w_doto = region.r_offset;
	if(html_ins_utf8(open) != TRUE)
	  return(FALSE);

	for(i = 0; i < region.r_size; i++)
	  if(forwchar(FALSE, 1) != TRUE)
	    break;

	if(html_ins_utf8(close) != TRUE)
	  return(FALSE);

	markregion(0);
	curwp->w_markp = NULL;
    }
    else{
	if(html_ins_utf8(open) != TRUE)
	  return(FALSE);

	if(html_ins_utf8(close) != TRUE)
	  return(FALSE);

	cu = utf8_to_ucs4_cpystr(close);
	if(cu){
	    clen = (int) ucs4_strlen(cu);
	    fs_give((void **) &cu);
	}

	if(clen > 0)
	  (void) backchar_no_header_editor(FALSE, clen);
    }

    if(what)
      emlwrite(what, NULL);

    curwp->w_flag |= WFMOVE;
    return(TRUE);
}


static int
html_wrap_current_line(char *open, char *close, char *what)
{
    REGION region;

    if(curbp->b_mode & MDVIEW)
      return(rdonly());

    if(getregion(&region, curwp->w_markp, curwp->w_marko) == TRUE)
      return(html_wrap(open, close, what));

    if(html_line_empty(curwp->w_dotp) || curwp->w_dotp == curbp->b_linep)
      return(html_wrap(open, close, what));

    curwp->w_doto = 0;
    if(html_ins_utf8(open) != TRUE)
      return(FALSE);

    curwp->w_doto = llength(curwp->w_dotp);
    if(html_ins_utf8(close) != TRUE)
      return(FALSE);

    if(what)
      emlwrite(what, NULL);

    curwp->w_flag |= WFMOVE;
    return(TRUE);
}


static LINE *
html_region_end_line(REGION *rp)
{
    LINE *lp;
    long  left;

    if(!rp || !rp->r_linep)
      return(NULL);

    lp = rp->r_linep;
    left = rp->r_size - (llength(lp) - rp->r_offset);
    while(left > 0 && lp != curbp->b_linep){
	lp = lforw(lp);
	if(lp == curbp->b_linep)
	  break;

	left -= llength(lp) + 1;
    }

    return(lp);
}


static int
html_line_empty(LINE *lp)
{
    int i;

    if(!lp || lp == curbp->b_linep)
      return(1);

    for(i = 0; i < llength(lp); i++)
      if(!ucs4_isspace(lgetc(lp, i).c))
	return(0);

    return(1);
}


static int
html_line_starts(LINE *lp, char *tag)
{
    int i = 0, j;
    UCS c, t;

    if(!lp || lp == curbp->b_linep || !tag)
      return(0);

    while(i < llength(lp) && ucs4_isspace(lgetc(lp, i).c))
      i++;

    for(j = 0; tag[j]; j++, i++){
	if(i >= llength(lp))
	  return(0);

	c = lgetc(lp, i).c;
	t = (UCS) tag[j];
	if(c != t && !(t >= 'a' && t <= 'z' && c == t - 32))
	  return(0);
    }

    return(1);
}


static void
html_goto_line(LINE *lp)
{
    curwp->w_dotp = lp;
    curwp->w_doto = 0;
    curwp->w_flag |= WFMOVE;
}


/*
 * Wrap a span of lines with open/close on the first and last line.
 * If list_items, wrap each non-empty line in <li></li> first.
 *
 * Close is inserted first so a following edit cannot invalidate
 * the start-of-span line we still need to prefix.
 */
static int
html_surround_lines(char *open, char *close, int list_items, char *what)
{
    REGION  region;
    LINE   *start, *end, *lp;
    int	    have_region, nspan, i;
    char    buf[160];
    UCS    *cu;
    int	    back;

    if(curbp->b_mode & MDVIEW)
      return(rdonly());

    have_region = (getregion(&region, curwp->w_markp, curwp->w_marko) == TRUE);
    if(have_region){
	start = region.r_linep;
	end = html_region_end_line(&region);
	markregion(0);
	curwp->w_markp = NULL;
    }
    else{
	start = end = curwp->w_dotp;
    }

    if(start == curbp->b_linep || (start == end && html_line_empty(start))){
	if(list_items)
	  snprintf(buf, sizeof(buf), "%s<li></li>%s", open, close);
	else
	  snprintf(buf, sizeof(buf), "%s%s", open, close);

	if(html_ins_utf8(buf) != TRUE)
	  return(FALSE);

	cu = utf8_to_ucs4_cpystr(close);
	back = cu ? (int) ucs4_strlen(cu) : 0;
	if(cu)
	  fs_give((void **) &cu);

	if(list_items)
	  back += 5;				/* </li> */

	if(back > 0)
	  (void) backchar_no_header_editor(FALSE, back);

	if(what)
	  emlwrite(what, NULL);

	curwp->w_flag |= WFMOVE;
	return(TRUE);
    }

    nspan = 1;
    for(lp = start; lp != end && lp != curbp->b_linep; lp = lforw(lp))
      nspan++;

    html_goto_line(start);
    for(i = 0; i < nspan; i++){
	if(list_items
	   && !html_line_empty(curwp->w_dotp)
	   && !html_line_starts(curwp->w_dotp, "<li>")){
	    curwp->w_doto = 0;
	    if(html_ins_utf8("<li>") != TRUE)
	      return(FALSE);

	    curwp->w_doto = llength(curwp->w_dotp);
	    if(html_ins_utf8("</li>") != TRUE)
	      return(FALSE);
	}

	if(i < nspan - 1){
	    lp = lforw(curwp->w_dotp);
	    if(lp == curbp->b_linep)
	      break;

	    html_goto_line(lp);
	}
    }

    curwp->w_doto = llength(curwp->w_dotp);
    if(html_ins_utf8(close) != TRUE)
      return(FALSE);

    html_goto_line(start);
    if(html_ins_utf8(open) != TRUE)
      return(FALSE);

    if(what)
      emlwrite(what, NULL);

    curwp->w_flag |= WFMOVE;
    return(TRUE);
}


int
htmlfmt_bold(int f, int n)
{
    (void) f;
    (void) n;
    return(html_wrap("<b>", "</b>", _("Bold")));
}


int
htmlfmt_italic(int f, int n)
{
    (void) f;
    (void) n;
    return(html_wrap("<i>", "</i>", _("Italic")));
}


int
htmlfmt_underline(int f, int n)
{
    (void) f;
    (void) n;
    return(html_wrap("<u>", "</u>", _("Underline")));
}


int
htmlfmt_strike(int f, int n)
{
    (void) f;
    (void) n;
    return(html_wrap("<s>", "</s>", _("Strikethrough")));
}


int
htmlfmt_code(int f, int n)
{
    (void) f;
    (void) n;
    return(html_wrap("<code>", "</code>", _("Code")));
}


int
htmlfmt_heading(int f, int n)
{
    (void) f;
    (void) n;
    return(html_wrap_current_line("<h2>", "</h2>", _("Heading")));
}


int
htmlfmt_quote(int f, int n)
{
    (void) f;
    (void) n;
    return(html_surround_lines("<blockquote>", "</blockquote>", 0, _("Quote")));
}


int
htmlfmt_pre(int f, int n)
{
    (void) f;
    (void) n;
    return(html_surround_lines("<pre>", "</pre>", 0, _("Preformatted")));
}


int
htmlfmt_hr(int f, int n)
{
    (void) f;
    (void) n;
    if(curbp->b_mode & MDVIEW)
      return(rdonly());

    if(html_line_empty(curwp->w_dotp) || curwp->w_dotp == curbp->b_linep){
	if(html_ins_utf8("<hr>") != TRUE)
	  return(FALSE);
    }
    else{
	curwp->w_doto = 0;
	if(html_ins_utf8("<hr>\n") != TRUE)
	  return(FALSE);
    }

    emlwrite(_("Horizontal rule"), NULL);
    curwp->w_flag |= WFMOVE;
    return(TRUE);
}


int
htmlfmt_ulist(int f, int n)
{
    (void) f;
    (void) n;
    return(html_surround_lines("<ul>", "</ul>", 1, _("Bullet list")));
}


int
htmlfmt_olist(int f, int n)
{
    (void) f;
    (void) n;
    return(html_surround_lines("<ol>", "</ol>", 1, _("Numbered list")));
}


int
htmlfmt_link(int f, int n)
{
    char url[NLINE], href[NLINE + 64], *p, *d;
    int  s;

    (void) f;
    (void) n;
    if(curbp->b_mode & MDVIEW)
      return(rdonly());

    url[0] = '\0';
    s = mlreply_utf8(_("Link URL: "), url, sizeof(url) - 1, QNORML, NULL);
    if(s != TRUE)
      return(s);

    d = href;
    d += snprintf(href, sizeof(href), "<a href=\"");
    for(p = url; *p && (d - href) < (int) sizeof(href) - 10; p++){
	if(*p == '"'){
	    memcpy(d, "&quot;", 6);
	    d += 6;
	}
	else if(*p == '&'){
	    memcpy(d, "&amp;", 5);
	    d += 5;
	}
	else if(*p == '<'){
	    memcpy(d, "&lt;", 4);
	    d += 4;
	}
	else if(*p == '>'){
	    memcpy(d, "&gt;", 4);
	    d += 4;
	}
	else
	  *d++ = *p;
    }

    *d++ = '"';
    *d++ = '>';
    *d = '\0';
    return(html_wrap(href, "</a>", _("Link")));
}


void
html_view_init(HTML_VIEW *st)
{
    if(st)
      memset(st, 0, sizeof(*st));
}


static int
html_cell_width(UCS ch)
{
    int w;

    if(ch == '\t')
      return(-1);

    if(ISCONTROL(ch))
      return(2);

    w = wcellwidth(ch);
    return(w >= 0 ? w : 1);
}


static void
html_emit_ch(void (*emit)(CELL, void *), void *arg, UCS ch, unsigned a, int *vis)
{
    CELL c;
    int  w;

    c.c = ch;
    c.a = a & 0x7f;
    c.d = 0;
    if(emit)
      emit(c, arg);

    if(ch == '\t'){
	(*vis)++;
	while(*vis & 0x07)
	  (*vis)++;
    }
    else{
	w = html_cell_width(ch);
	*vis += (w > 0 ? w : 1);
    }
}


static int
html_tag_at(LINE *lp, int i, char *name, int nmax, int *closing)
{
    int j, n = 0;
    UCS c;

    if(!lp || i >= llength(lp) || lgetc(lp, i).c != '<')
      return(0);

    j = i + 1;
    if(closing)
      *closing = 0;

    if(j < llength(lp) && lgetc(lp, j).c == '/'){
	if(closing)
	  *closing = 1;

	j++;
    }

    if(j < llength(lp) && lgetc(lp, j).c == '!'){
	if(n < nmax - 1)
	  name[n++] = '!';

	j++;
    }

    while(j < llength(lp)){
	c = lgetc(lp, j).c;
	if(c >= 'A' && c <= 'Z')
	  c = c - 'A' + 'a';

	if(!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
	  break;

	if(n < nmax - 1)
	  name[n++] = (char) c;

	j++;
    }

    if(n == 0)
      return(0);

    name[n] = '\0';

    while(j < llength(lp) && lgetc(lp, j).c != '>')
      j++;

    if(j >= llength(lp) || lgetc(lp, j).c != '>')
      return(0);

    return(j - i + 1);
}


static int
html_known_tag(char *name)
{
    static const char *ok[] = {
	"b", "i", "u", "s", "em", "strong", "code",
	"h1", "h2", "h3", "ul", "ol", "li", "blockquote",
	"pre", "a", "p", "br", "hr", "div", "html", "head",
	"body", "meta", "!doctype", NULL
    };
    int i;

    if(!name)
      return(0);

    for(i = 0; ok[i]; i++)
      if(!strcmp(name, ok[i]))
	return(1);

    return(0);
}


/*
 * 0 = unknown (show as text)
 * 1 = hide
 * 2 = hide and emit a list bullet
 * 3 = hide and emit a horizontal rule
 */
static int
html_apply_tag(HTML_VIEW *st, char *name, int closing)
{
    unsigned bit = 0;

    if(!st || !name)
      return(0);

    if(!strcmp(name, "b") || !strcmp(name, "strong")
       || !strcmp(name, "h1") || !strcmp(name, "h2") || !strcmp(name, "h3"))
      bit = HA_BOLD;
    else if(!strcmp(name, "i") || !strcmp(name, "em"))
      bit = HA_ITAL;
    else if(!strcmp(name, "u"))
      bit = HA_ULINE;
    else if(!strcmp(name, "s"))
      bit = HA_STRIKE;
    else if(!strcmp(name, "a"))
      bit = HA_ULINE;
    else if(!strcmp(name, "ul")){
	st->list = closing ? 0 : 1;
	return(1);
    }
    else if(!strcmp(name, "ol")){
	st->list = closing ? 0 : 2;
	if(!closing)
	  st->ol_num = 0;

	return(1);
    }
    else if(!strcmp(name, "li")){
	if(!closing){
	    if(st->list == 2)
	      st->ol_num++;

	    return(2);
	}

	return(1);
    }
    else if(!strcmp(name, "blockquote")){
	if(closing){
	    if(st->quote > 0)
	      st->quote--;
	}
	else
	  st->quote++;

	return(1);
    }
    else if(!strcmp(name, "pre")){
	st->pre = closing ? 0 : 1;
	return(1);
    }
    else if(!strcmp(name, "hr"))
      return(3);
    else if(!strcmp(name, "code") || !strcmp(name, "p") || !strcmp(name, "br")
	    || !strcmp(name, "div") || !strcmp(name, "html")
	    || !strcmp(name, "head") || !strcmp(name, "body")
	    || !strcmp(name, "meta") || !strcmp(name, "!doctype"))
      return(1);
    else
      return(0);

    if(closing)
      st->attr &= ~bit;
    else
      st->attr |= bit;

    return(1);
}


static int
html_entity_at(LINE *lp, int i, UCS *out)
{
    static const struct {
	char *ent;
	UCS   c;
    } ents[] = {
	{"amp", '&'},
	{"lt", '<'},
	{"gt", '>'},
	{"quot", '"'},
	{"apos", '\''},
	{"nbsp", 0xA0},
	{NULL, 0}
    };
    int j, n = 0, k;
    char buf[16];
    UCS c;

    if(!lp || i >= llength(lp) || lgetc(lp, i).c != '&')
      return(0);

    j = i + 1;
    while(j < llength(lp) && n < (int) sizeof(buf) - 1){
	c = lgetc(lp, j).c;
	if(c == ';')
	  break;

	if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
	     || (c >= '0' && c <= '9') || c == '#'))
	  return(0);

	buf[n++] = (char) c;
	j++;
    }

    if(j >= llength(lp) || lgetc(lp, j).c != ';' || n == 0)
      return(0);

    buf[n] = '\0';
    for(k = 0; ents[k].ent; k++){
	if(!strcmp(buf, ents[k].ent)){
	    if(out)
	      *out = ents[k].c;

	    return(j - i + 1);
	}
    }

    return(0);
}


static void
html_emit_bullet(void (*emit)(CELL, void *), void *arg, HTML_VIEW *st,
		 unsigned a, int *vis)
{
    char num[16];
    int  i;

    if(st->list == 2 && st->ol_num > 0){
	snprintf(num, sizeof(num), "%d. ", st->ol_num);
	for(i = 0; num[i]; i++)
	  html_emit_ch(emit, arg, (UCS) num[i], a, vis);
    }
    else{
	html_emit_ch(emit, arg, 0x2022, a, vis);	/* • */
	html_emit_ch(emit, arg, ' ', a, vis);
    }
}


static void
html_emit_indent(void (*emit)(CELL, void *), void *arg, int depth,
		 unsigned a, int *vis)
{
    int i;

    for(i = 0; i < depth; i++){
	html_emit_ch(emit, arg, '>', a, vis);
	html_emit_ch(emit, arg, ' ', a, vis);
    }
}


static void
html_emit_rule(void (*emit)(CELL, void *), void *arg, unsigned a, int *vis)
{
    int n, i;

    n = term.t_ncol > 8 ? term.t_ncol - 4 : 20;
    if(n > 40)
      n = 40;

    for(i = 0; i < n; i++)
      html_emit_ch(emit, arg, 0x2500, a, vis);	/* ─ */
}


int
html_render_line(LINE *lp, HTML_VIEW *st,
		 void (*emit)(CELL, void *), void *arg,
		 int stop_doto, int *col_at_doto)
{
    int      i, vis = 0, tlen, closing, kind, indented = 0;
    char     name[32];
    UCS      ent;
    CELL     src;
    unsigned a;

    if(!st)
      return(0);

    if(!lp || lp == curbp->b_linep){
	if(col_at_doto)
	  *col_at_doto = 0;

	return(0);
    }

    if(st->quote > 0){
	html_emit_indent(emit, arg, st->quote, st->attr, &vis);
	indented = 1;
    }

    for(i = 0; i < llength(lp); ){
	if(stop_doto >= 0 && i >= stop_doto){
	    if(col_at_doto)
	      *col_at_doto = vis;

	    return(vis);
	}

	src = lgetc(lp, i);
	a = (src.a & HA_REV) | st->attr;

	if(src.c == '<'){
	    tlen = html_tag_at(lp, i, name, sizeof(name), &closing);
	    if(tlen > 0 && html_known_tag(name)){
		if(stop_doto >= 0 && stop_doto < i + tlen){
		    if(col_at_doto)
		      *col_at_doto = vis;

		    return(vis);
		}

		kind = html_apply_tag(st, name, closing);
		if(kind == 2)
		  html_emit_bullet(emit, arg, st, a, &vis);
		else if(kind == 3)
		  html_emit_rule(emit, arg, a, &vis);
		else if(!strcmp(name, "blockquote") && !closing && !indented){
		    html_emit_indent(emit, arg, st->quote, st->attr, &vis);
		    indented = 1;
		}

		i += tlen;
		continue;
	    }
	}

	if(src.c == '&'){
	    tlen = html_entity_at(lp, i, &ent);
	    if(tlen > 0){
		if(stop_doto >= 0 && stop_doto < i + tlen){
		    if(col_at_doto)
		      *col_at_doto = vis;

		    return(vis);
		}

		html_emit_ch(emit, arg, ent, a, &vis);
		i += tlen;
		continue;
	    }
	}

	html_emit_ch(emit, arg, src.c, a, &vis);
	i++;
    }

    if(col_at_doto)
      *col_at_doto = vis;

    return(vis);
}


void
html_view_to_line(LINE *target, HTML_VIEW *st)
{
    LINE *lp;

    html_view_init(st);
    if(!curbp || !target)
      return;

    lp = lforw(curbp->b_linep);
    while(lp != target && lp != curbp->b_linep){
	html_render_line(lp, st, NULL, NULL, -1, NULL);
	lp = lforw(lp);
    }
}


int
html_doto_for_col(LINE *lp, HTML_VIEW *st0, int goal)
{
    int       i, col, best = 0;
    HTML_VIEW st;

    if(!lp || !st0)
      return(0);

    for(i = 0; i <= llength(lp); i++){
	st = *st0;
	html_render_line(lp, &st, NULL, NULL, i, &col);
	if(col > goal)
	  return(best);

	best = i;
    }

    return(llength(lp));
}


void
html_term_cell_attr(int a)
{
    static int last = 0;
    int	       on, off;

    a &= (HA_REV | HA_BOLD | HA_ULINE | HA_ITAL | HA_STRIKE);
    if(a == last)
      return;

    on = a & ~last;
    off = last & ~a;
    if(off & HA_BOLD)
      fputs("\033[22m", stdout);

    if(off & HA_ITAL)
      fputs("\033[23m", stdout);

    if(off & HA_ULINE)
      fputs("\033[24m", stdout);

    if(off & HA_STRIKE)
      fputs("\033[29m", stdout);

    if(off & HA_REV)
      fputs("\033[27m", stdout);

    if(on & HA_BOLD)
      fputs("\033[1m", stdout);

    if(on & HA_ITAL)
      fputs("\033[3m", stdout);

    if(on & HA_ULINE)
      fputs("\033[4m", stdout);

    if(on & HA_STRIKE)
      fputs("\033[9m", stdout);

    if(on & HA_REV)
      fputs("\033[7m", stdout);

    last = a;
}
