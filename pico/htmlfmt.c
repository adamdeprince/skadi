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
 * Alt-B/I/U and friends wrap the region (or insert an empty pair of
 * tags) and pressed again inside such a span remove it.  Lists turn
 * the current line or marked lines into <li> items inside <ul> or
 * <ol>.  The tags are hidden by the renderer, the cursor skips over
 * them, and deleting into a tag removes it with its partner.  Alt-M
 * toggles HTML mode, Alt-V previews the text/plain rendering, Alt-G
 * attaches an image and inserts a cid: reference to it.
 */

#include "headers.h"
#include "htmlfmt.h"
#include "../pith/charconv/utf8.h"
#include "../pith/charconv/filesys.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#ifndef	_WINDOWS
#include <unistd.h>
#endif


#define	HTML_TAGNAME	32
#define	HTML_PARTNER_LINES 2000


static int	html_ins_utf8(char *);
static int	html_wrap(char *, char *, char *);
static int	html_wrap_current_line(char *, char *, char *);
static int	html_surround_lines(char *, char *, int, char *);
static LINE    *html_region_end_line(REGION *);
static int	html_line_empty(LINE *);
static int	html_line_starts(LINE *, char *);
static void	html_goto_line(LINE *);
static void	html_mode_on(void);
static int	html_buffer_has_markup(void);
static void	html_tag_name_of(char *, char *, size_t);
static int	html_paired_tag(char *);
static int	html_tag_at(LINE *, int, char *, int, int *);
static int	html_known_tag(char *);
static int	html_apply_tag(HTML_VIEW *, char *, int);
static int	html_entity_at(LINE *, int, UCS *);
static int	html_entity_ending_at(LINE *, int, int *);
static int	html_tag_ending_at(LINE *, int, int *);
static void	html_delete_range(LINE *, int, int);
static int	html_delete_partner(LINE *, int, char *, int);
static int	html_find_enclosing(char *, LINE **, int *, int *, LINE **, int *, int *);
static int	html_unwrap_enclosing(char *, char *);
static char    *html_buffer_utf8(void);


/*----------------------------------------------------------------------
    HTML mode
  ----*/

static int	html_mode = HTMLFMT_AUTO;
static char  *(*html_preview_hook)(char *, int *) = NULL;


int
htmlfmt_mode(void)
{
    return(html_mode);
}


void
htmlfmt_set_mode(int mode)
{
    html_mode = mode;
    if(curwp)
      curwp->w_flag |= WFMODE;
}


void
htmlfmt_set_preview_hook(char *(*hook)(char *, int *))
{
    html_preview_hook = hook;
}


static void
html_mode_on(void)
{
    if(html_mode != HTMLFMT_ON){
	html_mode = HTMLFMT_ON;
	if(curwp)
	  curwp->w_flag |= WFMODE;

	sgarbk = TRUE;
    }
}


/*
 * Will the message go out as HTML with the mode as it stands?
 */
int
htmlfmt_effective_html(void)
{
    if(html_mode == HTMLFMT_ON)
      return(1);

    if(html_mode == HTMLFMT_OFF)
      return(0);

    return(html_buffer_has_markup());
}


/*
 * A known paired tag opened and closed on one line, anywhere in the
 * buffer?  Mirrors the auto-detect on the sending side.
 */
static int
html_buffer_has_markup(void)
{
    LINE *lp;
    char  name[HTML_TAGNAME], cname[HTML_TAGNAME];
    int	  i, j, tl, cl, closing, cclosing;

    if(!curbp)
      return(0);

    for(lp = lforw(curbp->b_linep); lp != curbp->b_linep; lp = lforw(lp)){
	for(i = 0; i < llength(lp); ){
	    tl = html_tag_at(lp, i, name, sizeof(name), &closing);
	    if(tl <= 0 || closing || !html_known_tag(name) || !html_paired_tag(name)){
		i += (tl > 0) ? tl : 1;
		continue;
	    }

	    for(j = i + tl; j < llength(lp); ){
		cl = html_tag_at(lp, j, cname, sizeof(cname), &cclosing);
		if(cl > 0 && cclosing && !strcmp(cname, name))
		  return(1);

		j += (cl > 0) ? cl : 1;
	    }

	    i += tl;
	}
    }

    return(0);
}


int
htmlfmt_toggle(int f, int n)
{
    (void) f;
    (void) n;
    if(html_mode == HTMLFMT_ON){
	html_mode = HTMLFMT_OFF;
	emlwrite(_("HTML mode off: message will be sent as plain text"), NULL);
    }
    else{
	html_mode = HTMLFMT_ON;
	emlwrite(_("HTML mode on: message will be sent as text/html"), NULL);
    }

    if(curwp)
      curwp->w_flag |= WFMODE;

    sgarbk = TRUE;
    return(TRUE);
}


/*----------------------------------------------------------------------
    Inserting markup
  ----*/

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


/*
 * "<b>" -> "b", "<a href=...>" -> "a".
 */
static void
html_tag_name_of(char *tag, char *name, size_t nlen)
{
    size_t n = 0;

    if(tag && *tag == '<')
      tag++;

    while(tag && *tag && n < nlen - 1
	  && (isalnum((unsigned char) *tag) || *tag == '!'))
      name[n++] = (char) tolower((unsigned char) *tag++);

    name[n] = '\0';
}


static int
html_paired_tag(char *name)
{
    return(name && *name && strcmp(name, "br") && strcmp(name, "hr")
	   && strcmp(name, "img") && strcmp(name, "meta")
	   && strcmp(name, "!doctype"));
}


static int
html_wrap(char *open, char *close, char *what)
{
    REGION region;
    UCS   *cu;
    char   name[HTML_TAGNAME];
    int    i, clen = 0;

    if(curbp->b_mode & MDVIEW)
      return(rdonly());

    html_tag_name_of(open, name, sizeof(name));

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
	/* pressed again inside a span of this kind: take it off */
	if(html_unwrap_enclosing(name, what))
	  return(TRUE);

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

    html_mode_on();
    if(what)
      emlwrite(what, NULL);

    curwp->w_flag |= WFMOVE | WFHARD;
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

    html_mode_on();
    if(what)
      emlwrite(what, NULL);

    curwp->w_flag |= WFMOVE | WFHARD;
    return(TRUE);
}


/*
 * The last line touched by a region.  A region that ends at column 0
 * of a line does not include that line.
 */
static LINE *
html_region_end_line(REGION *rp)
{
    LINE *lp;
    long  left;

    if(!rp || !rp->r_linep)
      return(NULL);

    lp = rp->r_linep;
    left = rp->r_size - (llength(lp) - rp->r_offset) - 1;	/* -1: newline */
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

    html_mode_on();

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

	curwp->w_flag |= WFMOVE | WFHARD;
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

    curwp->w_flag |= WFMOVE | WFHARD;
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

    html_mode_on();
    emlwrite(_("Horizontal rule"), NULL);
    curwp->w_flag |= WFMOVE | WFHARD;
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


/*
 * Attach an image file and insert a reference to it.  The attachment
 * comment carries the Content-ID, which the sending side turns into
 * the part's id and a multipart/related wrapper.
 */
int
htmlfmt_image(int f, int n)
{
    char  fn[NLINE], cid[160], cmt[200], tag[240], host[128];
    off_t sz = 0;
    int   s;

    (void) f;
    (void) n;
    if(curbp->b_mode & MDVIEW)
      return(rdonly());

    if(!(Pmaster && Pmaster->headents)){
	emlwrite(_("Inline images need the Alpine composer"), NULL);
	return(FALSE);
    }

    fn[0] = '\0';
    s = mlreplyd_utf8(_("Image file to attach: "), fn, sizeof(fn) - 1,
		      QDEFLT | QFFILE, NULL);
    if(s != TRUE)
      return(s);

    if(!fn[0])
      return(FALSE);

    fixpath(fn, sizeof(fn));
    if(fexist(fn, "r", &sz) != FIOSUC){
	EML eml;

	eml.s = fn;
	emlwwrite(_("Can't read image file \"%s\""), &eml);
	return(FALSE);
    }

    host[0] = '\0';
#ifndef	_WINDOWS
    if(gethostname(host, sizeof(host) - 1) != 0)
      host[0] = '\0';
#endif
    host[sizeof(host) - 1] = '\0';
    if(!host[0] || !isalnum((unsigned char) host[0]))
      strncpy(host, "localhost", sizeof(host));

    snprintf(cid, sizeof(cid), "img-%lx-%x@%s",
	     (unsigned long) time((time_t *) 0), (unsigned) (rand() & 0xffff), host);
    snprintf(cmt, sizeof(cmt), "cid:%s", cid);
    if(AppendAttachment(fn, NULL, cmt, 0) != 0){
	emlwrite(_("Image not attached"), NULL);
	return(FALSE);
    }

    snprintf(tag, sizeof(tag), "<img src=\"cid:%s\">", cid);
    if(html_ins_utf8(tag) != TRUE)
      return(FALSE);

    html_mode_on();
    emlwrite(_("Image attached and inserted"), NULL);
    curwp->w_flag |= WFMOVE | WFHARD;
    return(TRUE);
}


/*----------------------------------------------------------------------
    Preview
  ----*/

/*
 * The whole buffer as one UTF-8 string, freed by the caller.
 */
static char *
html_buffer_utf8(void)
{
    LINE  *lp;
    UCS   *ucs;
    char  *line, *out = NULL;
    size_t used = 0, alloc = 0, len;
    int	   i;

    if(!curbp)
      return(NULL);

    for(lp = lforw(curbp->b_linep); lp != curbp->b_linep; lp = lforw(lp)){
	ucs = (UCS *) fs_get((llength(lp) + 1) * sizeof(UCS));
	for(i = 0; i < llength(lp); i++)
	  ucs[i] = lgetc(lp, i).c;

	ucs[i] = 0;
	line = ucs4_to_utf8_cpystr(ucs);
	fs_give((void **) &ucs);
	len = line ? strlen(line) : 0;
	if(used + len + 2 > alloc){
	    alloc = (used + len + 2) * 2;
	    fs_resize((void **) &out, alloc);
	}

	if(line){
	    memcpy(out + used, line, len);
	    used += len;
	    fs_give((void **) &line);
	}

	out[used++] = '\n';
	out[used] = '\0';
    }

    if(!out){
	out = (char *) fs_get(1);
	out[0] = '\0';
    }

    return(out);
}


int
htmlfmt_preview(int f, int n)
{
    char  *text, *plain = NULL, **lines, *p, *q;
    int	   is_html = 0, nlines = 0, i;

    (void) f;
    (void) n;

    text = html_buffer_utf8();
    if(html_preview_hook && text)
      plain = (*html_preview_hook)(text, &is_html);

    if(text)
      fs_give((void **) &text);

    if(!plain || !is_html){
	if(plain)
	  fs_give((void **) &plain);

	emlwrite(html_preview_hook
		   ? _("HTML mode is off; the message would be sent as plain text")
		   : _("Preview needs the Alpine composer"), NULL);
	return(TRUE);
    }

    for(p = plain; *p; p++)
      if(*p == '\n')
	nlines++;

    if(nlines == 0 || plain[strlen(plain) - 1] != '\n')
      nlines++;

    lines = (char **) fs_get((nlines + 1) * sizeof(char *));
    for(i = 0, p = plain; i < nlines && *p; i++){
	q = strchr(p, '\n');
	if(!q)
	  q = p + strlen(p);

	/* the scroller treats a leading ~ as a stripe marker */
	lines[i] = (char *) fs_get((size_t) (q - p) + 2);
	if(*p == '~'){
	    lines[i][0] = ' ';
	    memcpy(lines[i] + 1, p, (size_t) (q - p));
	    lines[i][q - p + 1] = '\0';
	}
	else{
	    memcpy(lines[i], p, (size_t) (q - p));
	    lines[i][q - p] = '\0';
	}

	p = *q ? q + 1 : q;
    }

    nlines = i;
    lines[nlines] = NULL;
    fs_give((void **) &plain);

    (void) pico_help(lines, _("Preview of the plain text rendering"), 0);

    for(i = 0; i < nlines; i++)
      fs_give((void **) &lines[i]);

    fs_give((void **) &lines);

    ttresize();
    picosigs();
    sgarbf = TRUE;
    curwp->w_flag |= WFHARD | WFMODE;
    return(TRUE);
}


/*----------------------------------------------------------------------
    Rendering
  ----*/

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


/*
 * A tag at position i of lp?  Fills in the lower-cased name and the
 * closing flag and returns the tag's length, or 0.  Any name is
 * accepted; see html_known_tag().
 */
static int
html_tag_at(LINE *lp, int i, char *name, int nmax, int *closing)
{
    int j, n = 0;
    UCS c, q = 0;

    if(!lp || lp == curbp->b_linep || i >= llength(lp) || lgetc(lp, i).c != '<')
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

    if(j < llength(lp)){
	c = lgetc(lp, j).c;
	if(!(c == '>' || c == '/' || c == ' ' || c == '\t'))
	  return(0);
    }

    while(j < llength(lp)){
	c = lgetc(lp, j).c;
	if(q){
	    if(c == q)
	      q = 0;
	}
	else if(c == '"' || c == '\'')
	  q = c;
	else if(c == '>')
	  break;

	j++;
    }

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
	"pre", "a", "img", "p", "br", "hr", "div", "html", "head",
	"body", "meta", "title", "!doctype", NULL
    };
    int i;

    if(!name)
      return(0);

    for(i = 0; ok[i]; i++)
      if(!strcmp(name, ok[i]))
	return(1);

    return(0);
}


int
html_known_tag_name(char *name)
{
    return(html_known_tag(name));
}


/*
 * 0 = unknown (show as text)
 * 1 = hide
 * 2 = hide and emit a list bullet
 * 3 = hide and emit a horizontal rule
 * 4 = hide and emit an image placeholder
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
    else if(!strcmp(name, "img"))
      return(closing ? 1 : 4);
    else if(!strcmp(name, "code") || !strcmp(name, "p") || !strcmp(name, "br")
	    || !strcmp(name, "div") || !strcmp(name, "html")
	    || !strcmp(name, "head") || !strcmp(name, "body")
	    || !strcmp(name, "meta") || !strcmp(name, "title")
	    || !strcmp(name, "!doctype"))
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

    if(!lp || lp == curbp->b_linep || i >= llength(lp) || lgetc(lp, i).c != '&')
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
    if(buf[0] == '#'){
	unsigned long cp;

	if(buf[1] == 'x' || buf[1] == 'X')
	  cp = strtoul(buf + 2, NULL, 16);
	else
	  cp = strtoul(buf + 1, NULL, 10);

	if(cp == 0 || cp > 0x10FFFF)
	  return(0);

	if(out)
	  *out = (UCS) cp;

	return(j - i + 1);
    }

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
	html_emit_ch(emit, arg, 0x2022, a, vis);	/* bullet */
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
      html_emit_ch(emit, arg, 0x2500, a, vis);	/* box drawing bar */
}


static void
html_emit_image(void (*emit)(CELL, void *), void *arg, unsigned a, int *vis)
{
    static const char *txt = "[image]";
    int i;

    for(i = 0; txt[i]; i++)
      html_emit_ch(emit, arg, (UCS) txt[i], a | HA_REV, vis);
}


int
html_render_line(LINE *lp, HTML_VIEW *st,
		 void (*emit)(CELL, void *), void *arg,
		 int stop_doto, int *col_at_doto)
{
    int      i, vis = 0, tlen, closing, kind, indented = 0;
    char     name[HTML_TAGNAME];
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
		else if(kind == 4)
		  html_emit_image(emit, arg, a, &vis);
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


/*
 * The largest buffer offset on lp whose display column is at most
 * goal, never inside a hidden tag or entity.  One pass over the line.
 */
int
html_doto_for_col(LINE *lp, HTML_VIEW *st0, int goal)
{
    HTML_VIEW st;
    int	      i = 0, vis = 0, best = 0, tlen, closing, kind, indented = 0;
    char      name[HTML_TAGNAME];
    UCS	      ent;
    CELL      src;

    if(!lp || !st0 || lp == curbp->b_linep)
      return(0);

    st = *st0;
    if(st.quote > 0){
	html_emit_indent(NULL, NULL, st.quote, 0, &vis);
	indented = 1;
    }

    while(i < llength(lp)){
	if(vis > goal)
	  return(best);

	best = i;
	src = lgetc(lp, i);
	if(src.c == '<'){
	    tlen = html_tag_at(lp, i, name, sizeof(name), &closing);
	    if(tlen > 0 && html_known_tag(name)){
		kind = html_apply_tag(&st, name, closing);
		if(kind == 2)
		  html_emit_bullet(NULL, NULL, &st, 0, &vis);
		else if(kind == 3)
		  html_emit_rule(NULL, NULL, 0, &vis);
		else if(kind == 4)
		  html_emit_image(NULL, NULL, 0, &vis);
		else if(!strcmp(name, "blockquote") && !closing && !indented){
		    html_emit_indent(NULL, NULL, st.quote, 0, &vis);
		    indented = 1;
		}

		i += tlen;
		continue;
	    }
	}

	if(src.c == '&' && (tlen = html_entity_at(lp, i, &ent)) > 0){
	    html_emit_ch(NULL, NULL, ent, 0, &vis);
	    i += tlen;
	    continue;
	}

	html_emit_ch(NULL, NULL, src.c, 0, &vis);
	i++;
    }

    return((vis > goal) ? best : llength(lp));
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


/*----------------------------------------------------------------------
    Tag-aware editing
  ----*/

/*
 * Length of the known tag starting at offset i, else 0.
 */
int
html_tag_starting_at(LINE *lp, int i)
{
    char name[HTML_TAGNAME];
    int  closing, tl;

    tl = html_tag_at(lp, i, name, sizeof(name), &closing);
    return((tl > 0 && html_known_tag(name)) ? tl : 0);
}


int
html_entity_starting_at(LINE *lp, int i)
{
    UCS e;

    return(html_entity_at(lp, i, &e));
}


/*
 * Is doto strictly inside a hidden tag or entity?  Then *s and *e are
 * the offsets just outside it.
 */
int
html_hidden_span(LINE *lp, int doto, int *s, int *e)
{
    int i = 0, tl;

    if(!lp || lp == curbp->b_linep)
      return(0);

    while(i < doto && i < llength(lp)){
	tl = html_tag_starting_at(lp, i);
	if(tl <= 0)
	  tl = html_entity_starting_at(lp, i);

	if(tl > 0){
	    if(doto > i && doto < i + tl){
		*s = i;
		*e = i + tl;
		return(1);
	    }

	    i += tl;
	}
	else
	  i++;
    }

    return(0);
}


/*
 * A known tag that ends exactly at doto: returns its length, *s its start.
 */
static int
html_tag_ending_at(LINE *lp, int doto, int *s)
{
    int i = 0, tl;

    if(!lp || lp == curbp->b_linep)
      return(0);

    while(i < doto){
	tl = html_tag_starting_at(lp, i);
	if(tl > 0){
	    if(i + tl == doto){
		*s = i;
		return(tl);
	    }

	    i += tl;
	    continue;
	}

	tl = html_entity_starting_at(lp, i);
	i += (tl > 0) ? tl : 1;
    }

    return(0);
}


static int
html_entity_ending_at(LINE *lp, int doto, int *s)
{
    int i = 0, tl;

    if(!lp || lp == curbp->b_linep)
      return(0);

    while(i < doto){
	tl = html_tag_starting_at(lp, i);
	if(tl > 0){
	    i += tl;
	    continue;
	}

	tl = html_entity_starting_at(lp, i);
	if(tl > 0){
	    if(i + tl == doto){
		*s = i;
		return(tl);
	    }

	    i += tl;
	    continue;
	}

	i++;
    }

    return(0);
}


/*
 * Delete len characters at (lp, off), keeping dot where it was.
 */
static void
html_delete_range(LINE *lp, int off, int len)
{
    LINE *dlp = curwp->w_dotp;
    int	  doto = curwp->w_doto;

    curwp->w_dotp = lp;
    curwp->w_doto = off;
    (void) ldelete((long) len, NULL);
    curwp->w_dotp = dlp;
    if(dlp == lp && doto > off)
      curwp->w_doto = (doto - len > off) ? doto - len : off;
    else
      curwp->w_doto = doto;

    curwp->w_flag |= WFHARD;
}


/*
 * Delete the partner of the tag that was at (lp, off): the matching
 * close after it when closing is 0, the matching open before it when
 * closing is 1.  Nesting of the same tag name is honored.
 */
static int
html_delete_partner(LINE *lp, int off, char *name, int closing)
{
    LINE *l, *best_lp = NULL, *start;
    char  tname[HTML_TAGNAME];
    int	  i, tl, tclosing, depth = 0, best_off = -1, best_len = 0, lines = 0;
    LINE *stack_lp[256];
    int	  stack_off[256], stack_len[256], sp = 0;

    if(!html_paired_tag(name))
      return(0);

    if(!closing){
	/* forward for the close */
	for(l = lp; l != curbp->b_linep && lines < HTML_PARTNER_LINES; l = lforw(l), lines++){
	    for(i = (l == lp) ? off : 0; i < llength(l); ){
		tl = html_tag_at(l, i, tname, sizeof(tname), &tclosing);
		if(tl > 0 && !strcmp(tname, name)){
		    if(!tclosing)
		      depth++;
		    else if(depth == 0){
			html_delete_range(l, i, tl);
			return(1);
		    }
		    else
		      depth--;
		}

		i += (tl > 0) ? tl : 1;
	    }
	}

	return(0);
    }

    /* backward for the open: scan forward from a few lines up, keeping
     * a stack of unmatched opens */
    start = lp;
    for(lines = 0; lines < HTML_PARTNER_LINES && lback(start) != curbp->b_linep; lines++)
      start = lback(start);

    for(l = start; ; l = lforw(l)){
	for(i = 0; i < llength(l); ){
	    if(l == lp && i >= off)
	      break;

	    tl = html_tag_at(l, i, tname, sizeof(tname), &tclosing);
	    if(tl > 0 && !strcmp(tname, name)){
		if(!tclosing){
		    if(sp < 256){
			stack_lp[sp] = l;
			stack_off[sp] = i;
			stack_len[sp] = tl;
			sp++;
		    }
		}
		else if(sp > 0)
		  sp--;
	    }

	    i += (tl > 0) ? tl : 1;
	}

	if(l == lp)
	  break;
    }

    if(sp > 0){
	best_lp = stack_lp[sp - 1];
	best_off = stack_off[sp - 1];
	best_len = stack_len[sp - 1];
	html_delete_range(best_lp, best_off, best_len);
	return(1);
    }

    return(0);
}


/*
 * Backspace: if the character before dot ends a tag or entity, take
 * the whole thing (and a tag's partner) out.  Returns TRUE if handled.
 */
int
html_delete_before_dot(void)
{
    LINE *lp = curwp->w_dotp;
    char  name[HTML_TAGNAME];
    int	  doto = curwp->w_doto, s, tl, closing;

    if(!lp || lp == curbp->b_linep || doto <= 0)
      return(FALSE);

    tl = html_tag_ending_at(lp, doto, &s);
    if(tl > 0){
	(void) html_tag_at(lp, s, name, sizeof(name), &closing);
	html_delete_range(lp, s, tl);
	(void) html_delete_partner(curwp->w_dotp, curwp->w_doto, name, closing);
	curwp->w_flag |= WFHARD | WFMOVE;
	return(TRUE);
    }

    tl = html_entity_ending_at(lp, doto, &s);
    if(tl > 0){
	html_delete_range(lp, s, tl);
	curwp->w_flag |= WFHARD | WFMOVE;
	return(TRUE);
    }

    return(FALSE);
}


/*
 * Delete forward: same for a tag or entity that starts at dot.
 */
int
html_delete_at_dot(void)
{
    LINE *lp = curwp->w_dotp;
    char  name[HTML_TAGNAME];
    int	  doto = curwp->w_doto, tl, closing;

    if(!lp || lp == curbp->b_linep || doto >= llength(lp))
      return(FALSE);

    tl = html_tag_starting_at(lp, doto);
    if(tl > 0){
	(void) html_tag_at(lp, doto, name, sizeof(name), &closing);
	html_delete_range(lp, doto, tl);
	(void) html_delete_partner(curwp->w_dotp, curwp->w_doto, name, closing);
	curwp->w_flag |= WFHARD | WFMOVE;
	return(TRUE);
    }

    tl = html_entity_starting_at(lp, doto);
    if(tl > 0){
	html_delete_range(lp, doto, tl);
	curwp->w_flag |= WFHARD | WFMOVE;
	return(TRUE);
    }

    return(FALSE);
}


/*
 * Find the <name> ... </name> span enclosing dot, if any.
 */
static int
html_find_enclosing(char *name, LINE **olp, int *ooff, int *olen,
		    LINE **clp, int *coff, int *clen)
{
    LINE *l, *start;
    char  tname[HTML_TAGNAME];
    int	  i, tl, tclosing, lines, depth = 0;
    LINE *stack_lp[256];
    int	  stack_off[256], stack_len[256], sp = 0;

    /* opens before dot */
    start = curwp->w_dotp;
    for(lines = 0; lines < HTML_PARTNER_LINES && lback(start) != curbp->b_linep; lines++)
      start = lback(start);

    for(l = start; ; l = lforw(l)){
	for(i = 0; i < llength(l); ){
	    if(l == curwp->w_dotp && i >= curwp->w_doto)
	      break;

	    tl = html_tag_at(l, i, tname, sizeof(tname), &tclosing);
	    if(tl > 0 && !strcmp(tname, name)){
		if(!tclosing){
		    if(sp < 256){
			stack_lp[sp] = l;
			stack_off[sp] = i;
			stack_len[sp] = tl;
			sp++;
		    }
		}
		else if(sp > 0)
		  sp--;
	    }

	    i += (tl > 0) ? tl : 1;
	}

	if(l == curwp->w_dotp)
	  break;
    }

    if(sp == 0)
      return(0);

    *olp = stack_lp[sp - 1];
    *ooff = stack_off[sp - 1];
    *olen = stack_len[sp - 1];

    /* the close after dot */
    for(l = curwp->w_dotp, lines = 0;
	l != curbp->b_linep && lines < HTML_PARTNER_LINES;
	l = lforw(l), lines++){
	for(i = (l == curwp->w_dotp) ? curwp->w_doto : 0; i < llength(l); ){
	    tl = html_tag_at(l, i, tname, sizeof(tname), &tclosing);
	    if(tl > 0 && !strcmp(tname, name)){
		if(!tclosing)
		  depth++;
		else if(depth == 0){
		    *clp = l;
		    *coff = i;
		    *clen = tl;
		    return(1);
		}
		else
		  depth--;
	    }

	    i += (tl > 0) ? tl : 1;
	}
    }

    return(0);
}


/*
 * If dot is inside <name>...</name>, remove both tags and say so.
 */
static int
html_unwrap_enclosing(char *name, char *what)
{
    LINE *olp, *clp;
    int	  ooff, olen, coff, clen;
    char  msg[80];

    if(!html_paired_tag(name)
       || !html_find_enclosing(name, &olp, &ooff, &olen, &clp, &coff, &clen))
      return(0);

    html_delete_range(clp, coff, clen);		/* the later one first */
    html_delete_range(olp, ooff, olen);
    if(what){
	snprintf(msg, sizeof(msg), "%s %s", what, _("removed"));
	emlwrite(msg, NULL);
    }

    curwp->w_flag |= WFHARD | WFMOVE;
    return(1);
}


/*
 * Return inside a list item.  Handled here when the current line is
 * a <li>...</li> item and dot is within it: an empty item ends the
 * list, otherwise a new item is started below.
 */
int
html_newline_in_item(void)
{
    LINE *lp = curwp->w_dotp;
    char  name[HTML_TAGNAME];
    int	  doto = curwp->w_doto, i, tl, closing;
    int	  li_s = -1, li_e = -1, cl_s = -1, cl_e = -1, empty = 1;

    if(!lp || lp == curbp->b_linep)
      return(FALSE);

    for(i = 0; i < llength(lp); ){
	tl = html_tag_at(lp, i, name, sizeof(name), &closing);
	if(tl > 0 && !strcmp(name, "li")){
	    if(!closing && li_s < 0){
		li_s = i;
		li_e = i + tl;
	    }
	    else if(closing && li_s >= 0 && cl_s < 0){
		cl_s = i;
		cl_e = i + tl;
	    }
	}

	i += (tl > 0) ? tl : 1;
    }

    if(li_s < 0 || cl_s < 0 || doto > cl_e)
      return(FALSE);

    for(i = li_e; i < cl_s; i++)
      if(!ucs4_isspace(lgetc(lp, i).c)){
	  empty = 0;
	  break;
      }

    if(empty){
	int  close_s = -1, close_len = 0, k;
	char cname[HTML_TAGNAME];
	UCS  tagbuf[HTML_TAGNAME];

	/* a closing </ul> or </ol> after the item moves up a line */
	for(i = cl_e; i < llength(lp); ){
	    tl = html_tag_at(lp, i, cname, sizeof(cname), &closing);
	    if(tl > 0 && closing && (!strcmp(cname, "ul") || !strcmp(cname, "ol"))){
		close_s = i;
		close_len = tl;
		break;
	    }

	    i += (tl > 0) ? tl : 1;
	}

	if(close_s >= 0 && close_len < HTML_TAGNAME && lback(lp) != curbp->b_linep){
	    for(k = 0; k < close_len; k++)
	      tagbuf[k] = lgetc(lp, close_s + k).c;

	    tagbuf[k] = 0;
	    html_delete_range(lp, close_s, close_len);
	    curwp->w_dotp = lback(lp);
	    curwp->w_doto = llength(curwp->w_dotp);
	    for(k = 0; tagbuf[k]; k++)
	      (void) linsert(1, tagbuf[k]);
	}

	curwp->w_dotp = lp;
	curwp->w_doto = 0;
	html_delete_range(lp, li_s, cl_e - li_s);
	curwp->w_dotp = lp;
	curwp->w_doto = li_s;
	curwp->w_flag |= WFHARD | WFMOVE;
	emlwrite(_("List ended"), NULL);
	return(TRUE);
    }

    curwp->w_doto = cl_e;
    if(lnewline() != TRUE)
      return(FALSE);

    if(html_ins_utf8("<li></li>") != TRUE)
      return(FALSE);

    (void) backchar_no_header_editor(FALSE, 5);
    curwp->w_flag |= WFHARD | WFMOVE;
    return(TRUE);
}


/*
 * Like writetmp(0, dir) for the spelling checker, but with tags
 * removed and entities decoded so they are not flagged as words.
 * Quoted lines (starting with '>') are left out as writetmp does.
 */
char *
html_writetmp_stripped(char *dir)
{
    static char fn[NFILEN];
    LINE  *lp;
    CELL  *buf;
    int	   s = FIOSUC, t, i, n, tl;
    UCS	   ent;

    tmpname(dir, fn);
    if(!fn[0] || (s = ffwopen(fn, TRUE)) != FIOSUC){
	if(fn[0])
	  our_unlink(fn);

	return(NULL);
    }

    for(lp = lforw(curbp->b_linep); lp != curbp->b_linep; lp = lforw(lp)){
	if(llength(lp) > 0 && lgetc(lp, 0).c == '>')
	  continue;

	buf = (CELL *) fs_get((llength(lp) + 1) * sizeof(CELL));
	for(i = n = 0; i < llength(lp); ){
	    tl = html_tag_starting_at(lp, i);
	    if(tl > 0){
		/* a tag boundary separates words */
		buf[n].c = ' ';
		buf[n].a = 0;
		buf[n].d = 0;
		n++;
		i += tl;
		continue;
	    }

	    tl = html_entity_at(lp, i, &ent);
	    if(tl > 0){
		buf[n] = lgetc(lp, i);
		buf[n].c = ent;
		n++;
		i += tl;
		continue;
	    }

	    buf[n++] = lgetc(lp, i);
	    i++;
	}

	s = ffputline(buf, n);
	fs_give((void **) &buf);
	if(s != FIOSUC)
	  break;
    }

    t = ffclose();
    if(s == FIOSUC)
      s = t;

    if(s != FIOSUC){
	our_unlink(fn);
	return(NULL);
    }

    return(fn);
}
