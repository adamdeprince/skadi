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
 * Composer markup <-> text/html.
 *
 * The composer text is plain text with a handful of HTML tags in it.
 * htmlify_composed_text() turns that into a well-formed text/html
 * body: paragraphs become <p>, the known tags are re-emitted without
 * attributes (a safe href on links, a cid: src on inline images), and
 * everything else is escaped.  html_wrap_alternative() adds the
 * text/plain rendering.  html_to_composer() reverses the paragraph
 * markup so a postponed HTML draft comes back the way it was typed,
 * and html_simple_to_markup() reduces an incoming HTML message to the
 * composer's tag set for reply quoting.
 */

#include "../pith/headers.h"
#include "../pith/htmlbody.h"
#include "../pith/store.h"
#include "../pith/filter.h"
#include "../pith/state.h"
#include "../pith/conf.h"
#include "../pith/send.h"
#include "../pith/mimedesc.h"
#include "../pith/charconv/utf8.h"

#include <ctype.h>
#include <time.h>


int html_compose_start_html = 0;


/* tag classes */
#define	HT_NONE		0
#define	HT_INLINE	1		/* b i u s em strong code a		*/
#define	HT_VOID		2		/* br hr img				*/
#define	HT_ITEM		3		/* li h1 h2 h3 p			*/
#define	HT_CONTAIN	4		/* ul ol blockquote div pre		*/
#define	HT_WRAP		5		/* html head body meta title !doctype	*/

#define	HTML_MAXSTACK	64
#define	HTML_MAXNAME	16


static const struct html_tagdef {
    const char *name;
    int		cls;
} html_tags[] = {
    {"b", HT_INLINE},	{"i", HT_INLINE},	{"u", HT_INLINE},
    {"s", HT_INLINE},	{"em", HT_INLINE},	{"strong", HT_INLINE},
    {"code", HT_INLINE},	{"a", HT_INLINE},
    {"br", HT_VOID},	{"hr", HT_VOID},	{"img", HT_VOID},
    {"li", HT_ITEM},	{"h1", HT_ITEM},	{"h2", HT_ITEM},
    {"h3", HT_ITEM},	{"p", HT_ITEM},
    {"ul", HT_CONTAIN},	{"ol", HT_CONTAIN},	{"blockquote", HT_CONTAIN},
    {"div", HT_CONTAIN},	{"pre", HT_CONTAIN},
    {"html", HT_WRAP},	{"head", HT_WRAP},	{"body", HT_WRAP},
    {"meta", HT_WRAP},	{"title", HT_WRAP},	{"!doctype", HT_WRAP},
    {NULL, HT_NONE}
};


/*
 * Context for the composer-text -> HTML conversion.
 */
typedef struct html_ctx {
    STORE_S	*out;
    int		 wrapped;		/* source is already a document	*/
    const char	*inl[HTML_MAXSTACK];	/* open inline tags		*/
    int		 ninl;
    const char	*blk[HTML_MAXSTACK];	/* open block containers	*/
    int		 nblk;
    const char	*item;			/* open li/h1-3/p on this line	*/
    int		 para;			/* a <p> of ours is open	*/
    int		 pre;			/* inside <pre>			*/
    int		 escape_tags;		/* treat every '<' as text	*/
} HTML_CTX;


/*
 * Context for HTML -> plain text.
 */
typedef struct plain_ctx {
    STORE_S	*out;
    int		 nl;			/* newlines at end of output	*/
    int		 bol;			/* at beginning of a line	*/
    int		 started;		/* anything written yet		*/
    int		 quote;			/* blockquote depth		*/
    int		 list[HTML_MAXSTACK];	/* 1 = ul, 2 = ol		*/
    int		 lnum[HTML_MAXSTACK];
    int		 nlist;
    int		 pre;
    char	 href[MAILTMPLEN];
    int		 skip_nl;		/* drop the newline after <br>	*/
} PLAIN_CTX;


/* internal prototypes */
static const struct html_tagdef *html_lookup(const char *);
static int	 html_parse_tag(char *, char *, size_t, int *, char **, char **);
static int	 html_attr_value(char *, char *, const char *, char *, size_t);
static int	 html_is_entity(char *);
static int	 html_safe_href(char *);
static int	 html_safe_cid(char *);
static void	 html_put_attr_escaped(STORE_S *, char *);
static void	 html_put_escaped_char(STORE_S *, int);
static char	*html_read_all(STORE_S *);
static void	 html_ctx_emit_tag(HTML_CTX *, const struct html_tagdef *, int, char *, char *);
static void	 html_ctx_emit_seg(HTML_CTX *, char *, char *);
static void	 html_ctx_close_item(HTML_CTX *);
static void	 html_ctx_close_para(HTML_CTX *);
static void	 html_ctx_open_para(HTML_CTX *);
static int	 html_line_is_block(char *, char *);
static int	 html_line_is_blank(char *, char *);
static void	 pl_prefix(PLAIN_CTX *);
static void	 pl_char(PLAIN_CTX *, int);
static void	 pl_puts(PLAIN_CTX *, const char *);
static void	 pl_newline(PLAIN_CTX *);
static void	 pl_ensure_nl(PLAIN_CTX *, int);
static int	 html_entity_decode(char *, int *, char *, size_t);
static void	 html_utf8_put(STORE_S *, unsigned long);
static char	*html_skip_to_close(char *, const char *);


/*----------------------------------------------------------------------
    Small helpers
  ----*/

static const struct html_tagdef *
html_lookup(const char *name)
{
    int i;

    if(!name)
      return(NULL);

    for(i = 0; html_tags[i].name; i++)
      if(!strcmp(name, html_tags[i].name))
	return(&html_tags[i]);

    return(NULL);
}


/*
 * s points at '<'.  If a complete tag follows on this line, fill in the
 * lower-cased name, the closing flag, and pointers to the attribute text
 * and to the closing '>', and return the tag's total length.  Otherwise
 * return 0.  Any name is accepted; callers decide what they know.
 */
static int
html_parse_tag(char *s, char *name, size_t nlen, int *closing,
	       char **attrs, char **gt)
{
    char  *p = s + 1, q = 0;
    size_t n = 0;

    *closing = 0;
    if(attrs)
      *attrs = NULL;

    if(gt)
      *gt = NULL;

    if(!s || *s != '<')
      return(0);

    if(*p == '/'){
	*closing = 1;
	p++;
    }

    if(*p == '!' && n < nlen - 1){
	name[n++] = '!';
	p++;
    }

    while(*p && n < nlen - 1
	  && (isalnum((unsigned char) *p) || *p == '-')){
	name[n++] = (char) tolower((unsigned char) *p);
	p++;
    }

    name[n] = '\0';
    if(n == 0 || (n == 1 && name[0] == '!'))
      return(0);

    if(!(*p == '>' || *p == '/' || *p == ' ' || *p == '\t'))
      return(0);

    if(attrs)
      *attrs = p;

    for(; *p && *p != '\n' && *p != '\r'; p++){
	if(q){
	    if(*p == q)
	      q = 0;
	}
	else if(*p == '"' || *p == '\'')
	  q = *p;
	else if(*p == '>')
	  break;
    }

    if(*p != '>')
      return(0);

    if(gt)
      *gt = p;

    return((int) (p - s + 1));
}


/*
 * Find attr="value" (or attr=value) between attrs and gt.
 */
static int
html_attr_value(char *attrs, char *gt, const char *attr,
		char *buf, size_t buflen)
{
    char  *p = attrs, *q;
    size_t alen = strlen(attr), n = 0;

    buf[0] = '\0';
    if(!attrs || !gt)
      return(0);

    while(p < gt){
	while(p < gt && (*p == ' ' || *p == '\t' || *p == '/'))
	  p++;

	if(p + alen <= gt && !struncmp(p, (char *) attr, alen)
	   && (p == attrs || !isalnum((unsigned char) p[-1]))
	   && (p[alen] == '=' || p[alen] == ' ' || p[alen] == '\t')){
	    q = p + alen;
	    while(q < gt && (*q == ' ' || *q == '\t'))
	      q++;

	    if(*q != '=')
	      return(1);			/* attribute with no value */

	    q++;
	    while(q < gt && (*q == ' ' || *q == '\t'))
	      q++;

	    if(*q == '"' || *q == '\''){
		char quote = *q++;

		while(q < gt && *q != quote && n < buflen - 1)
		  buf[n++] = *q++;
	    }
	    else{
		while(q < gt && *q != ' ' && *q != '\t' && n < buflen - 1)
		  buf[n++] = *q++;
	    }

	    buf[n] = '\0';
	    return(1);
	}

	/* skip this attribute, honoring quotes */
	while(p < gt && *p != ' ' && *p != '\t'){
	    if(*p == '"' || *p == '\''){
		char quote = *p++;

		while(p < gt && *p != quote)
		  p++;
	    }

	    if(p < gt)
	      p++;
	}
    }

    return(0);
}


static int
html_is_entity(char *p)
{
    char *q;

    if(!p || *p != '&')
      return(0);

    q = p + 1;
    if(*q == '#'){
	q++;
	if(*q == 'x' || *q == 'X'){
	    q++;
	    if(!isxdigit((unsigned char) *q))
	      return(0);

	    while(isxdigit((unsigned char) *q))
	      q++;
	}
	else{
	    if(!isdigit((unsigned char) *q))
	      return(0);

	    while(isdigit((unsigned char) *q))
	      q++;
	}
    }
    else{
	if(!isalpha((unsigned char) *q))
	  return(0);

	while(isalnum((unsigned char) *q))
	  q++;
    }

    return(*q == ';' && (q - p) < 12);
}


/*
 * Only plain web and mail addresses get through as link targets.
 */
static int
html_safe_href(char *href)
{
    char *p;

    if(!href)
      return(0);

    while(*href == ' ' || *href == '\t')
      href++;

    for(p = href; *p; p++)
      if((unsigned char) *p < 0x20 || *p == '"' || *p == '<' || *p == '>')
	return(0);

    return(!struncmp(href, "http://", 7)
	   || !struncmp(href, "https://", 8)
	   || !struncmp(href, "mailto:", 7));
}


static int
html_safe_cid(char *src)
{
    char *p;

    if(!src || struncmp(src, "cid:", 4) || !src[4])
      return(0);

    for(p = src + 4; *p; p++)
      if(!(isalnum((unsigned char) *p) || *p == '.' || *p == '_'
	   || *p == '@' || *p == '+' || *p == '-'))
	return(0);

    return(1);
}


static void
html_put_attr_escaped(STORE_S *so, char *s)
{
    for(; s && *s; s++){
	if(*s == '&')
	  so_puts(so, "&amp;");
	else if(*s == '"')
	  so_puts(so, "&quot;");
	else if(*s == '<')
	  so_puts(so, "&lt;");
	else if(*s == '>')
	  so_puts(so, "&gt;");
	else
	  so_writec((int) (unsigned char) *s, so);
    }
}


static void
html_put_escaped_char(STORE_S *so, int c)
{
    if(c == '&')
      so_puts(so, "&amp;");
    else if(c == '<')
      so_puts(so, "&lt;");
    else if(c == '>')
      so_puts(so, "&gt;");
    else
      so_writec(c, so);
}


static char *
html_read_all(STORE_S *so)
{
    STORE_S *tmp;
    gf_o_t   pc;
    gf_i_t   gc;
    char    *s;

    if(!so)
      return(NULL);

    if(so->src == CharStar && so_text(so))
      return(cpystr((char *) so_text(so)));

    tmp = so_get(CharStar, NULL, EDIT_ACCESS);
    if(!tmp)
      return(NULL);

    so_seek(so, 0L, 0);
    gf_set_so_readc(&gc, so);
    gf_set_so_writec(&pc, tmp);
    gf_filter_init();
    (void) gf_pipe(gc, pc);
    gf_clear_so_readc(so);
    gf_clear_so_writec(tmp);
    s = cpystr((char *) so_text(tmp));
    so_give(&tmp);
    return(s);
}


/*----------------------------------------------------------------------
    Detection
  ----*/

/*
 * Does the text contain a known tag opened and closed on the same
 * line (or a whole document)?  This is the conservative auto-detect
 * used when the composer's HTML mode was never touched.
 */
int
html_text_has_markup(char *s)
{
    char  *p, name[HTML_MAXNAME], cname[HTML_MAXNAME], *q, *gt;
    int	   closing, len, clen, cclosing;
    const struct html_tagdef *td;

    if(!s)
      return(0);

    for(p = s; *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'; p++)
      ;

    if(!struncmp(p, "<!doctype", 9) || !struncmp(p, "<html", 5))
      return(1);

    for(p = s; *p; p++){
	if(*p != '<')
	  continue;

	len = html_parse_tag(p, name, sizeof(name), &closing, NULL, &gt);
	if(!len || closing || !(td = html_lookup(name)))
	  continue;

	if(td->cls == HT_VOID || td->cls == HT_WRAP)
	  continue;

	/* look for the matching close on this line */
	for(q = p + len; *q && *q != '\n'; q++){
	    if(*q != '<')
	      continue;

	    clen = html_parse_tag(q, cname, sizeof(cname), &cclosing, NULL, NULL);
	    if(clen && cclosing && !strcmp(cname, name))
	      return(1);
	}
    }

    return(0);
}


int
compose_text_is_html(struct mail_bodystruct *b)
{
    char *s;
    int   yes;

    if(!(b && b->contents.text.data))
      return(0);

    s = html_read_all((STORE_S *) b->contents.text.data);
    yes = html_text_has_markup(s);
    if(s)
      fs_give((void **) &s);

    return(yes);
}


int
html_is_simple(char *s)
{
    if(!s)
      return(0);

    return(strchr(s, '<') != NULL);
}


/*----------------------------------------------------------------------
    Composer text -> HTML
  ----*/

static void
html_ctx_emit_tag(HTML_CTX *c, const struct html_tagdef *td, int closing,
		  char *attrs, char *gt)
{
    STORE_S *so = c->out;
    char     val[MAILTMPLEN];
    int	     i;

    switch(td->cls){
      case HT_WRAP:
	/* our own wrapper is written by the caller; keep only a
	 * pasted document's skeleton, minus attributes */
	if(!c->wrapped)
	  return;

	if(!strcmp(td->name, "meta")){
	    if(!closing)
	      so_puts(so, "<meta charset=\"UTF-8\">");

	    return;
	}

	so_puts(so, closing ? "</" : "<");
	so_puts(so, (char *) td->name);
	so_puts(so, ">");
	return;

      case HT_VOID:
	if(closing)
	  return;

	if(!strcmp(td->name, "img")){
	    if(html_attr_value(attrs, gt, "src", val, sizeof(val))
	       && html_safe_cid(val)){
		so_puts(so, "<img src=\"");
		html_put_attr_escaped(so, val);
		so_puts(so, "\">");
	    }

	    return;
	}

	so_puts(so, "<");
	so_puts(so, (char *) td->name);
	so_puts(so, ">");
	return;

      case HT_INLINE:
	if(closing){
	    for(i = c->ninl - 1; i >= 0; i--)
	      if(c->inl[i] == td->name)
		break;

	    if(i < 0)
	      return;				/* stray close: drop it */

	    /* close anything opened after it, then it */
	    while(c->ninl - 1 > i){
		so_puts(so, "</");
		so_puts(so, (char *) c->inl[--c->ninl]);
		so_puts(so, ">");
	    }

	    c->ninl--;
	    so_puts(so, "</");
	    so_puts(so, (char *) td->name);
	    so_puts(so, ">");
	    return;
	}

	if(c->ninl >= HTML_MAXSTACK)
	  return;

	if(!strcmp(td->name, "a")){
	    if(html_attr_value(attrs, gt, "href", val, sizeof(val))
	       && html_safe_href(val)){
		so_puts(so, "<a href=\"");
		html_put_attr_escaped(so, val);
		so_puts(so, "\">");
	    }
	    else
	      so_puts(so, "<a>");
	}
	else{
	    so_puts(so, "<");
	    so_puts(so, (char *) td->name);
	    so_puts(so, ">");
	}

	c->inl[c->ninl++] = td->name;
	return;

      case HT_ITEM:
	if(closing){
	    if(c->item == td->name){
		c->item = NULL;
		so_puts(so, "</");
		so_puts(so, (char *) td->name);
		so_puts(so, ">");
	    }

	    return;
	}

	html_ctx_close_item(c);
	c->item = td->name;
	so_puts(so, "<");
	so_puts(so, (char *) td->name);
	so_puts(so, ">");
	return;

      case HT_CONTAIN:
	if(closing){
	    html_ctx_close_item(c);
	    for(i = c->nblk - 1; i >= 0; i--)
	      if(c->blk[i] == td->name)
		break;

	    if(i < 0)
	      return;

	    while(c->nblk - 1 > i){
		so_puts(so, "</");
		so_puts(so, (char *) c->blk[--c->nblk]);
		so_puts(so, ">");
	    }

	    c->nblk--;
	    so_puts(so, "</");
	    so_puts(so, (char *) td->name);
	    so_puts(so, ">");
	    if(!strcmp(td->name, "pre"))
	      c->pre = 0;

	    return;
	}

	if(c->nblk >= HTML_MAXSTACK)
	  return;

	html_ctx_close_item(c);
	so_puts(so, "<");
	so_puts(so, (char *) td->name);
	so_puts(so, ">");
	c->blk[c->nblk++] = td->name;
	if(!strcmp(td->name, "pre"))
	  c->pre = 1;

	return;

      default:
	break;
    }
}


/*
 * Emit the text between s and end with known tags sanitized and
 * everything else escaped.
 */
static void
html_ctx_emit_seg(HTML_CTX *c, char *s, char *end)
{
    char *p = s, *attrs, *gt, name[HTML_MAXNAME];
    int	  len, closing;
    const struct html_tagdef *td;

    while(p < end){
	if(*p == '<' && !c->escape_tags){
	    len = html_parse_tag(p, name, sizeof(name), &closing, &attrs, &gt);
	    if(len && p + len <= end && (td = html_lookup(name))){
		html_ctx_emit_tag(c, td, closing, attrs, gt);
		p += len;
		continue;
	    }
	}
	else if(*p == '&' && html_is_entity(p)){
	    gt = strchr(p, ';');
	    so_nputs(c->out, p, (long) (gt - p + 1));
	    p = gt + 1;
	    continue;
	}

	if(*p != '\r')
	  html_put_escaped_char(c->out, (int) (unsigned char) *p);

	p++;
    }
}


static void
html_ctx_close_item(HTML_CTX *c)
{
    if(c->item){
	so_puts(c->out, "</");
	so_puts(c->out, (char *) c->item);
	so_puts(c->out, ">");
	c->item = NULL;
    }
}


static void
html_ctx_close_para(HTML_CTX *c)
{
    int i;

    if(c->para){
	/* inline spans do not straddle paragraphs; they reopen below */
	for(i = c->ninl - 1; i >= 0; i--){
	    so_puts(c->out, "</");
	    so_puts(c->out, (char *) c->inl[i]);
	    so_puts(c->out, ">");
	}

	so_puts(c->out, "</p>\n");
	c->para = 0;
    }
}


static void
html_ctx_open_para(HTML_CTX *c)
{
    int i;

    so_puts(c->out, "<p>");
    c->para = 1;
    for(i = 0; i < c->ninl; i++){
	so_puts(c->out, "<");
	so_puts(c->out, (char *) c->inl[i]);
	so_puts(c->out, ">");
    }
}


static int
html_line_is_blank(char *s, char *end)
{
    for(; s < end; s++)
      if(*s != ' ' && *s != '\t' && *s != '\r')
	return(0);

    return(1);
}


/*
 * A line carries block structure if any known item, container or
 * wrapper tag, or <hr>, appears on it.
 */
static int
html_line_is_block(char *s, char *end)
{
    char *p, name[HTML_MAXNAME];
    int	  len, closing;
    const struct html_tagdef *td;

    for(p = s; p < end; p++){
	if(*p != '<')
	  continue;

	len = html_parse_tag(p, name, sizeof(name), &closing, NULL, NULL);
	if(!len || p + len > end || !(td = html_lookup(name)))
	  continue;

	if(td->cls == HT_ITEM || td->cls == HT_CONTAIN || td->cls == HT_WRAP
	   || !strcmp(name, "hr"))
	  return(1);
    }

    return(0);
}


/*
 * Convert the composer text in b (text/plain) into text/html in place.
 * The caller decides whether that is wanted; this always converts.
 */
/*
 * The composer text as it was before htmlify_composed_text() rewrote
 * it.  When posting fails, pine_send loops back into the composer and
 * pulls its text out of the body, which by then holds the plain-text
 * rendering; html_restore_composed_text() puts the original back.
 */
static STORE_S *html_saved_text = NULL;

static void
html_save_text(STORE_S *so)
{
    if(html_saved_text)
      so_give(&html_saved_text);

    html_saved_text = so;
}


void
html_forget_composed_text(void)
{
    if(html_saved_text)
      so_give(&html_saved_text);
}


/*
 * Undo create_message_body_text() and html_group_related() on a message
 * whose posting failed: the first text container (multipart/alternative,
 * possibly inside multipart/related for inline images, possibly inside
 * multipart/mixed) becomes the original text/plain again and any inline
 * image parts go back to being ordinary attachments.  Returns 1 when the
 * body was restored.
 */
int
html_restore_composed_text(struct mail_bodystruct **bp)
{
    BODY *top, *node, *mixed = NULL, *html = NULL, *tmp;
    PART *shell, *images = NULL, *p, *tail;

    if(!(html_saved_text && bp && *bp))
      return(0);

    top = *bp;
    node = top;
    /* Newly composed multipart bodies may use the default MIXED subtype. */
    if(top->type == TYPEMULTIPART
       && (!top->subtype || !strucmp(top->subtype, "MIXED"))
       && top->nested.part){
	mixed = top;
	node = &top->nested.part->body;
    }

    /* multipart/related: keep the image parts, drop the shell */
    if(node->type == TYPEMULTIPART && node->subtype
       && !strucmp(node->subtype, "RELATED") && node->nested.part){
	BODY inner;

	shell = node->nested.part;
	images = shell->next;
	shell->next = NULL;
	inner = shell->body;
	memset(&shell->body, 0, sizeof(shell->body));
	fs_give((void **) &shell);
	if(node->subtype)
	  fs_give((void **) &node->subtype);

	mail_free_body_parameter(&node->parameter);
	*node = inner;
    }

    if(node->type == TYPEMULTIPART && node->subtype
       && !strucmp(node->subtype, "ALTERNATIVE") && node->nested.part){
	for(p = node->nested.part; p; p = p->next)
	  if(p->body.type == TYPETEXT && p->body.subtype
	     && !strucmp(p->body.subtype, "HTML"))
	    html = &p->body;
    }
    else if(node->type == TYPETEXT && node->subtype
	    && !strucmp(node->subtype, "HTML"))
      html = node;

    if(!html){
	/* not a shape we made; put the images back where they were */
	if(images){
	    if(mixed){
		for(tail = mixed->nested.part; tail->next; tail = tail->next)
		  ;

		tail->next = images;
	    }
	    else
	      mail_free_body_part(&images);
	}

	return(0);
    }

    /* the original parameters (charset) travelled with the html part */
    if(html != node){
	PARAMETER *keep = html->parameter;

	html->parameter = NULL;
	pine_free_body_data(node);
	mail_free_body_part(&node->nested.part);
	if(node->subtype)
	  fs_give((void **) &node->subtype);

	mail_free_body_parameter(&node->parameter);
	memset(node, 0, sizeof(*node));
	node->parameter = keep;
    }
    else{
	pine_free_body_data(node);
	if(node->subtype)
	  fs_give((void **) &node->subtype);
    }

    node->type = TYPETEXT;
    node->subtype = cpystr("PLAIN");
    node->encoding = ENCOTHER;
    node->contents.text.data = (void *) html_saved_text;
    html_saved_text = NULL;

    for(p = images; p; p = p->next){
	if(p->body.disposition.type)
	  fs_give((void **) &p->body.disposition.type);

	p->body.disposition.type = cpystr("attachment");
    }

    if(images){
	if(!mixed){
	    /* the related part had been promoted; rebuild the mixed */
	    tmp = mail_newbody();
	    tmp->type = TYPEMULTIPART;
	    tmp->nested.part = mail_newbody_part();
	    tmp->nested.part->body = *node;
	    memset(node, 0, sizeof(*node));
	    mail_free_body(&top);
	    *bp = mixed = tmp;
	}

	for(tail = mixed->nested.part; tail->next; tail = tail->next)
	  ;

	tail->next = images;
    }

    return(1);
}


void
htmlify_composed_text(struct mail_bodystruct *b, char *charset)
{
    STORE_S  *so, *nso;
    HTML_CTX  c;
    char     *src, *p, *line, *end, *q;
    int	      quote, sig, prev_hard = 0, i, quoted_tags_ok;

    if(!(b && b->contents.text.data))
      return;

    so = (STORE_S *) b->contents.text.data;
    src = html_read_all(so);
    if(!src)
      return;

    nso = so_get(CharStar, NULL, EDIT_ACCESS);
    if(!nso){
	fs_give((void **) &src);
	return;
    }

    memset(&c, 0, sizeof(c));
    c.out = nso;
    (void) so_attr(nso, "edited", "1");	/* UTF-8 text we wrote */

    for(p = src; *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'; p++)
      ;

    c.wrapped = (!struncmp(p, "<!doctype", 9) || !struncmp(p, "<html", 5));

    /*
     * Quoted lines carry markup only when replies are quoted with
     * markup kept; otherwise a tag mentioned in quoted plain text
     * stays text.
     */
    quoted_tags_ok = (ps_global && F_ON(F_REPLY_KEEP_HTML_MARKUP, ps_global));

    if(!c.wrapped){
	so_puts(nso, "<!DOCTYPE html>\n<html><head><meta charset=\"");
	html_put_attr_escaped(nso, (charset && *charset) ? charset : "UTF-8");
	so_puts(nso, "\"></head>\n<body>\n");
    }

    p = src;
    while(*p){
	line = p;
	end = strchr(p, '\n');
	if(!end)
	  end = p + strlen(p);

	p = (*end == '\n') ? end + 1 : end;
	if(end > line && end[-1] == '\r')
	  end--;

	c.escape_tags = 0;
	if(c.wrapped || c.pre){
	    html_ctx_emit_seg(&c, line, end);
	    so_writec('\n', nso);
	    continue;
	}

	if(html_line_is_blank(line, end)){
	    html_ctx_close_para(&c);
	    html_ctx_close_item(&c);
	    prev_hard = 0;
	    continue;
	}

	for(q = line; q < end && (*q == ' ' || *q == '\t'); q++)
	  ;

	quote = (q < end && *q == '>');
	c.escape_tags = (quote && !quoted_tags_ok);

	if(!c.escape_tags && html_line_is_block(line, end)){
	    html_ctx_close_para(&c);
	    html_ctx_emit_seg(&c, line, end);

	    /* headings and <p> close on their own line; list items
	     * may continue on following lines */
	    if(c.item && strcmp(c.item, "li"))
	      html_ctx_close_item(&c);

	    so_writec('\n', nso);
	    prev_hard = 0;
	    continue;
	}

	if(c.item){				/* continuation of a list item */
	    html_ctx_emit_seg(&c, line, end);
	    so_writec('\n', nso);
	    continue;
	}

	sig = (end - line == 3 && line[0] == '-' && line[1] == '-' && line[2] == ' ');

	if(!c.para)
	  html_ctx_open_para(&c);
	else if(prev_hard || quote || sig)
	  so_puts(nso, "<br>\n");
	else
	  so_writec('\n', nso);

	html_ctx_emit_seg(&c, line, end);
	prev_hard = (quote || sig);
    }

    html_ctx_close_para(&c);
    html_ctx_close_item(&c);

    for(i = c.ninl - 1; i >= 0; i--){
	so_puts(nso, "</");
	so_puts(nso, (char *) c.inl[i]);
	so_puts(nso, ">");
    }

    if(c.ninl)
      so_writec('\n', nso);

    for(i = c.nblk - 1; i >= 0; i--){
	so_puts(nso, "</");
	so_puts(nso, (char *) c.blk[i]);
	so_puts(nso, ">\n");
    }

    if(!c.wrapped)
      so_puts(nso, "</body></html>\n");

    fs_give((void **) &src);
    html_save_text(so);
    b->contents.text.data = (void *) nso;
    if(b->subtype)
      fs_give((void **) &b->subtype);

    b->subtype = cpystr("HTML");
    b->type = TYPETEXT;
    b->encoding = ENCOTHER;			/* re-groped by the caller */
}


/*----------------------------------------------------------------------
    HTML -> plain text
  ----*/

static void
pl_prefix(PLAIN_CTX *c)
{
    int i;

    if(!c->bol)
      return;

    c->bol = 0;
    for(i = 0; i < c->quote; i++)
      so_puts(c->out, "> ");
}


static void
pl_char(PLAIN_CTX *c, int ch)
{
    if(ch == '\n'){
	pl_newline(c);
	return;
    }

    pl_prefix(c);
    so_writec(ch, c->out);
    c->nl = 0;
    c->started = 1;
}


static void
pl_puts(PLAIN_CTX *c, const char *s)
{
    for(; s && *s; s++)
      pl_char(c, (int) (unsigned char) *s);
}


static void
pl_newline(PLAIN_CTX *c)
{
    if(!c->started)
      return;

    if(c->nl >= 2 && !c->pre)
      return;

    so_writec('\n', c->out);
    c->nl++;
    c->bol = 1;
}


static void
pl_ensure_nl(PLAIN_CTX *c, int n)
{
    while(c->started && c->nl < n)
      pl_newline(c);
}


static void
html_utf8_put(STORE_S *so, unsigned long cp)
{
    unsigned char buf[8];
    int	 n = 0;

    if(cp < 0x80)
      buf[n++] = (unsigned char) cp;
    else if(cp < 0x800){
	buf[n++] = 0xC0 | (cp >> 6);
	buf[n++] = 0x80 | (cp & 0x3F);
    }
    else if(cp < 0x10000){
	buf[n++] = 0xE0 | (cp >> 12);
	buf[n++] = 0x80 | ((cp >> 6) & 0x3F);
	buf[n++] = 0x80 | (cp & 0x3F);
    }
    else if(cp < 0x110000){
	buf[n++] = 0xF0 | (cp >> 18);
	buf[n++] = 0x80 | ((cp >> 12) & 0x3F);
	buf[n++] = 0x80 | ((cp >> 6) & 0x3F);
	buf[n++] = 0x80 | (cp & 0x3F);
    }
    else
      buf[n++] = '?';

    so_nputs(so, (char *) buf, (long) n);
}


/*
 * p points at '&'.  If it is an entity we know, write its UTF-8 into
 * buf, set *len to the entity's length and return 1.
 */
static int
html_entity_decode(char *p, int *len, char *buf, size_t buflen)
{
    static const struct {
	const char   *name;
	unsigned long cp;
    } ents[] = {
	{"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'},
	{"apos", '\''}, {"nbsp", 0xA0}, {"copy", 0xA9}, {"reg", 0xAE},
	{"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026},
	{"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C},
	{"rdquo", 0x201D}, {"bull", 0x2022}, {"middot", 0xB7},
	{"trade", 0x2122}, {"euro", 0x20AC}, {"pound", 0xA3},
	{NULL, 0}
    };
    char	 *q, name[16];
    unsigned long cp = 0;
    int		  i, n = 0;
    STORE_S	 *tmp;

    if(!html_is_entity(p))
      return(0);

    q = p + 1;
    if(*q == '#'){
	q++;
	if(*q == 'x' || *q == 'X')
	  cp = strtoul(q + 1, &q, 16);
	else
	  cp = strtoul(q, &q, 10);
    }
    else{
	while(*q != ';' && n < (int) sizeof(name) - 1)
	  name[n++] = *q++;

	name[n] = '\0';
	for(i = 0; ents[i].name; i++)
	  if(!strcmp(name, ents[i].name)){
	      cp = ents[i].cp;
	      break;
	  }

	if(!ents[i].name)
	  return(0);
    }

    *len = (int) (q - p + 1);
    tmp = so_get(CharStar, NULL, EDIT_ACCESS);
    if(!tmp)
      return(0);

    html_utf8_put(tmp, cp);
    strncpy(buf, (char *) so_text(tmp), buflen - 1);
    buf[buflen - 1] = '\0';
    so_give(&tmp);
    return(1);
}


/*
 * Skip over <name ...> ... </name> for head, style and script.
 */
static char *
html_skip_to_close(char *p, const char *name)
{
    char  cname[HTML_MAXNAME];
    int	  len, closing;

    for(; *p; p++){
	if(*p == '<'
	   && (len = html_parse_tag(p, cname, sizeof(cname), &closing, NULL, NULL))
	   && closing && !strcmp(cname, name))
	  return(p + len);
    }

    return(p);
}


/*
 * Render (our, or any simple) HTML as plain text: paragraphs separated
 * by a blank line, list bullets, "> " quoting for blockquotes, link
 * targets after the link text.
 */
char *
html_to_plain(char *src)
{
    PLAIN_CTX c;
    char     *p, *attrs, *gt, name[HTML_MAXNAME], val[MAILTMPLEN], ubuf[8], *out;
    int	      len, closing, elen, i;
    const struct html_tagdef *td;

    if(!src)
      return(NULL);

    memset(&c, 0, sizeof(c));
    c.out = so_get(CharStar, NULL, EDIT_ACCESS);
    if(!c.out)
      return(NULL);

    c.bol = 1;
    c.nl = 2;

    for(p = src; *p; ){
	if(*p == '<'){
	    if(!strncmp(p, "<!--", 4)){
		char *e = strstr(p + 4, "-->");

		p = e ? e + 3 : p + strlen(p);
		continue;
	    }

	    len = html_parse_tag(p, name, sizeof(name), &closing, &attrs, &gt);
	    if(len){
		p += len;
		if(!closing && (!strcmp(name, "head") || !strcmp(name, "style")
				|| !strcmp(name, "script"))){
		    p = html_skip_to_close(p, name);
		    continue;
		}

		td = html_lookup(name);
		if(!td){
		    /* unknown tags: table cells and rows still get
		     * some structure, the rest vanishes */
		    if(!strcmp(name, "tr") || !strcmp(name, "table"))
		      pl_ensure_nl(&c, 1);
		    else if(!closing && (!strcmp(name, "td") || !strcmp(name, "th")))
		      pl_puts(&c, c.nl ? "" : " | ");

		    continue;
		}

		if(!strcmp(name, "p")){
		    pl_ensure_nl(&c, 2);
		}
		else if(!strcmp(name, "br")){
		    pl_newline(&c);
		    c.nl = 1;
		    c.skip_nl = 1;
		}
		else if(!strcmp(name, "hr")){
		    pl_ensure_nl(&c, 2);
		    pl_puts(&c, "----");
		    pl_ensure_nl(&c, 2);
		}
		else if(!strcmp(name, "img")){
		    pl_puts(&c, "[image]");
		}
		else if(!strcmp(name, "li")){
		    pl_ensure_nl(&c, 1);
		    if(!closing){
			for(i = 1; i < c.nlist; i++)
			  pl_puts(&c, "  ");

			if(c.nlist > 0 && c.list[c.nlist - 1] == 2){
			    snprintf(val, sizeof(val), "%d. ", ++c.lnum[c.nlist - 1]);
			    pl_puts(&c, val);
			}
			else
			  pl_puts(&c, "* ");
		    }
		}
		else if(!strcmp(name, "ul") || !strcmp(name, "ol")){
		    if(!closing){
			pl_ensure_nl(&c, c.nlist ? 1 : 2);
			if(c.nlist < HTML_MAXSTACK){
			    c.list[c.nlist] = (name[0] == 'o') ? 2 : 1;
			    c.lnum[c.nlist] = 0;
			    c.nlist++;
			}
		    }
		    else{
			if(c.nlist > 0)
			  c.nlist--;

			pl_ensure_nl(&c, c.nlist ? 1 : 2);
		    }
		}
		else if(!strcmp(name, "blockquote")){
		    pl_ensure_nl(&c, 2);
		    if(!closing)
		      c.quote++;
		    else if(c.quote > 0)
		      c.quote--;
		}
		else if(name[0] == 'h' && isdigit((unsigned char) name[1])){
		    pl_ensure_nl(&c, 2);
		}
		else if(!strcmp(name, "pre")){
		    pl_ensure_nl(&c, closing ? 2 : 1);
		    c.pre = !closing;
		}
		else if(!strcmp(name, "div")){
		    pl_ensure_nl(&c, 1);
		}
		else if(!strcmp(name, "a")){
		    if(!closing){
			if(!html_attr_value(attrs, gt, "href", c.href, sizeof(c.href)))
			  c.href[0] = '\0';
		    }
		    else if(c.href[0]){
			pl_puts(&c, " <");
			pl_puts(&c, c.href);
			pl_puts(&c, ">");
			c.href[0] = '\0';
		    }
		}

		continue;
	    }
	}

	if(*p == '&' && html_entity_decode(p, &elen, ubuf, sizeof(ubuf))){
	    pl_puts(&c, ubuf);
	    p += elen;
	    continue;
	}

	if(*p == '\r'){
	    p++;
	    continue;
	}

	if(*p == '\n'){
	    if(c.skip_nl)
	      c.skip_nl = 0;
	    else if(c.pre || c.nl < 1)
	      pl_newline(&c);

	    p++;
	    continue;
	}

	c.skip_nl = 0;
	pl_char(&c, (int) (unsigned char) *p);
	p++;
    }

    pl_ensure_nl(&c, 1);
    out = cpystr((char *) so_text(c.out));
    so_give(&c.out);
    for(p = out + strlen(out); p - out >= 2 && p[-1] == '\n' && p[-2] == '\n'; p--)
      p[-1] = '\0';

    return(out);
}


/*
 * Undo our paragraph markup so a postponed HTML message is edited in
 * the form it was typed: the document skeleton and <p> tags go away,
 * paragraphs are separated by a blank line, and the composer tags
 * stay as they are.
 */
char *
html_to_composer(char *src)
{
    STORE_S *so;
    char    *p, *out, name[HTML_MAXNAME], *body;
    int	     len, closing, nl = 2;
    const struct html_tagdef *td;

    if(!src)
      return(NULL);

    so = so_get(CharStar, NULL, EDIT_ACCESS);
    if(!so)
      return(NULL);

#define	HC_NL()	do{ if(nl < 2){ so_writec('\n', so); nl++; } }while(0)

    for(p = src; *p; ){
	if(*p == '<'){
	    len = html_parse_tag(p, name, sizeof(name), &closing, NULL, NULL);
	    if(len){
		td = html_lookup(name);
		if(td && td->cls == HT_WRAP){
		    p += len;
		    if(!closing && (!strcmp(name, "head") || !strcmp(name, "title")))
		      p = html_skip_to_close(p, name);

		    /* eat the newline after a skeleton tag */
		    if(*p == '\n')
		      p++;

		    continue;
		}

		if(!strcmp(name, "p")){
		    p += len;
		    if(!closing){
			HC_NL();
			HC_NL();
		    }
		    else{
			HC_NL();
			if(*p == '\n')
			  p++;
		    }

		    continue;
		}

		if(!strcmp(name, "br") && p[len] == '\n'){
		    p += len;			/* keep the newline only */
		    continue;
		}

		so_nputs(so, p, (long) len);
		nl = 0;
		p += len;
		continue;
	    }
	}

	if(*p == '\n'){
	    HC_NL();
	    p++;
	    continue;
	}

	if(*p != '\r'){
	    so_writec((int) (unsigned char) *p, so);
	    nl = 0;
	}

	p++;
    }

#undef HC_NL

    body = (char *) so_text(so);
    while(*body == '\n')
      body++;

    out = cpystr(body);
    so_give(&so);
    return(out);
}


/*----------------------------------------------------------------------
    Incoming HTML -> composer markup (for reply quoting)
  ----*/

typedef struct markup_ctx {
    STORE_S *out;			/* finished lines		*/
    STORE_S *para;			/* current paragraph, unwrapped	*/
    int	     width;
    int	     pre;
    int	     space;			/* pending collapsed space	*/
    int	     nlist;
    int	     list[HTML_MAXSTACK];	/* 1 ul, 2 ol			*/
    int	     in_item;
    int	     quote;
    const char *inl[HTML_MAXSTACK];
    int	     ninl;
    int	     blank;			/* blank line pending		*/
    int	     out_lines;
} MARKUP_CTX;


static int
mk_visible_width(char *s, char *end)
{
    int w = 0, intag = 0;

    for(; s < end; s++){
	if(*s == '<')
	  intag = 1;
	else if(*s == '>' && intag)
	  intag = 0;
	else if(!intag && ((unsigned char) *s & 0xC0) != 0x80)
	  w++;
    }

    return(w);
}


/*
 * Write the buffered paragraph out as wrapped composer lines.
 */
static void
mk_flush(MARKUP_CTX *c, const char *prefix, const char *suffix)
{
    char *s, *p, *wend, *linestart;
    int   col = 0, w, first = 1;

    s = (char *) so_text(c->para);
    if(!s)
      s = "";

    /* trim */
    while(*s == ' ')
      s++;

    p = s + strlen(s);
    while(p > s && p[-1] == ' ')
      p--;

    *p = '\0';

    if(!*s && !prefix && !suffix){
	so_truncate(c->para, 0L);
	return;
    }

    if(c->blank && c->out_lines){
	so_writec('\n', c->out);
	c->out_lines++;
    }

    c->blank = 0;

    if(prefix){
	so_puts(c->out, (char *) prefix);
	col = mk_visible_width((char *) prefix, (char *) prefix + strlen(prefix));
    }

    linestart = s;
    for(p = s; *p; ){
	/* next word: up to the next space, keeping tags with the word */
	wend = p;
	while(*wend && *wend != ' '){
	    if(*wend == '<'){
		char *g = strchr(wend, '>');

		wend = g ? g + 1 : wend + 1;
	    }
	    else
	      wend++;
	}

	w = mk_visible_width(p, wend);
	if(!first && col + 1 + w > c->width){
	    so_writec('\n', c->out);
	    c->out_lines++;
	    col = 0;
	    first = 1;
	}

	if(!first){
	    so_writec(' ', c->out);
	    col++;
	}

	so_nputs(c->out, p, (long) (wend - p));
	col += w;
	first = 0;
	p = wend;
	while(*p == ' ')
	  p++;
    }

    (void) linestart;
    if(suffix)
      so_puts(c->out, (char *) suffix);

    so_writec('\n', c->out);
    c->out_lines++;
    so_truncate(c->para, 0L);
}


static void
mk_text(MARKUP_CTX *c, char *s, int n)
{
    int i;

    for(i = 0; i < n; i++){
	unsigned char ch = (unsigned char) s[i];

	if(c->pre){
	    if(ch == '<')
	      so_puts(c->para, "&lt;");
	    else if(ch == '&')
	      so_puts(c->para, "&amp;");
	    else
	      so_writec((int) ch, c->para);

	    continue;
	}

	if(ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'){
	    c->space = 1;
	    continue;
	}

	if(c->space){
	    if(so_tell(c->para) > 0L)
	      so_writec(' ', c->para);

	    c->space = 0;
	}

	if(ch == '<')
	  so_puts(c->para, "&lt;");
	else if(ch == '&')
	  so_puts(c->para, "&amp;");
	else
	  so_writec((int) ch, c->para);
    }
}


static void
mk_inline(MARKUP_CTX *c, const char *name, int closing)
{
    int i;

    if(closing){
	for(i = c->ninl - 1; i >= 0; i--)
	  if(!strcmp(c->inl[i], name))
	    break;

	if(i < 0)
	  return;

	while(c->ninl - 1 > i){
	    so_puts(c->para, "</");
	    so_puts(c->para, (char *) c->inl[--c->ninl]);
	    so_puts(c->para, ">");
	}

	c->ninl--;
	so_puts(c->para, "</");
	so_puts(c->para, (char *) name);
	so_puts(c->para, ">");
	return;
    }

    if(c->ninl >= HTML_MAXSTACK)
      return;

    if(c->space && so_tell(c->para) > 0L){
	so_writec(' ', c->para);
	c->space = 0;
    }

    so_puts(c->para, "<");
    so_puts(c->para, (char *) name);
    so_puts(c->para, ">");
    c->inl[c->ninl++] = name;
}


static void
mk_close_inline(MARKUP_CTX *c)
{
    while(c->ninl > 0){
	so_puts(c->para, "</");
	so_puts(c->para, (char *) c->inl[--c->ninl]);
	so_puts(c->para, ">");
    }
}


static void
mk_end_item(MARKUP_CTX *c)
{
    if(c->in_item){
	mk_close_inline(c);
	mk_flush(c, "<li>", "</li>");
	c->in_item = 0;
    }
}


static void
mk_end_para(MARKUP_CTX *c)
{
    if(c->in_item){
	mk_end_item(c);
	return;
    }

    mk_close_inline(c);
    mk_flush(c, NULL, NULL);
}


/*
 * Reduce an incoming HTML body to the composer's tag set: paragraphs
 * wrapped at width columns, lists as <ul><li>..</li></ul> lines,
 * bold/italic/underline/strike/code/links kept, everything else
 * dropped to text.
 */
char *
html_simple_to_markup(char *src, int width)
{
    MARKUP_CTX c;
    char      *p, *attrs, *gt, name[HTML_MAXNAME], val[MAILTMPLEN], ubuf[8], *out;
    int	       len, closing, elen;

    if(!src)
      return(NULL);

    memset(&c, 0, sizeof(c));
    c.width = (width > 20) ? width : 72;
    c.out = so_get(CharStar, NULL, EDIT_ACCESS);
    c.para = so_get(CharStar, NULL, EDIT_ACCESS);
    if(!c.out || !c.para){
	if(c.out)
	  so_give(&c.out);

	if(c.para)
	  so_give(&c.para);

	return(NULL);
    }

    for(p = src; *p; ){
	if(*p == '<'){
	    if(!strncmp(p, "<!--", 4)){
		char *e = strstr(p + 4, "-->");

		p = e ? e + 3 : p + strlen(p);
		continue;
	    }

	    len = html_parse_tag(p, name, sizeof(name), &closing, &attrs, &gt);
	    if(len){
		p += len;
		if(!closing && (!strcmp(name, "head") || !strcmp(name, "style")
				|| !strcmp(name, "script") || !strcmp(name, "title"))){
		    p = html_skip_to_close(p, name);
		    continue;
		}

		if(!strcmp(name, "b") || !strcmp(name, "strong"))
		  mk_inline(&c, "b", closing);
		else if(!strcmp(name, "i") || !strcmp(name, "em"))
		  mk_inline(&c, "i", closing);
		else if(!strcmp(name, "u"))
		  mk_inline(&c, "u", closing);
		else if(!strcmp(name, "s") || !strcmp(name, "strike") || !strcmp(name, "del"))
		  mk_inline(&c, "s", closing);
		else if(!strcmp(name, "code") || !strcmp(name, "tt") || !strcmp(name, "kbd"))
		  mk_inline(&c, "code", closing);
		else if(!strcmp(name, "a")){
		    if(!closing){
			if(html_attr_value(attrs, gt, "href", val, sizeof(val))
			   && html_safe_href(val)){
			    if(c.space && so_tell(c.para) > 0L){
				so_writec(' ', c.para);
				c.space = 0;
			    }

			    so_puts(c.para, "<a href=\"");
			    html_put_attr_escaped(c.para, val);
			    so_puts(c.para, "\">");
			    if(c.ninl < HTML_MAXSTACK)
			      c.inl[c.ninl++] = "a";
			}
		    }
		    else
		      mk_inline(&c, "a", 1);
		}
		else if(!strcmp(name, "br")){
		    if(c.pre)
		      so_writec('\n', c.para);
		    else{
			mk_close_inline(&c);
			if(c.in_item){
			    /* keep the item on one line */
			    c.space = 1;
			}
			else
			  mk_flush(&c, NULL, NULL);
		    }
		}
		else if(!strcmp(name, "p") || !strcmp(name, "div")
			|| !strcmp(name, "tr") || !strcmp(name, "table")){
		    mk_end_para(&c);
		    if(!strcmp(name, "p") && !closing)
		      c.blank = 1;
		}
		else if(!strcmp(name, "td") || !strcmp(name, "th")){
		    if(!closing && so_tell(c.para) > 0L)
		      so_puts(c.para, " | ");
		}
		else if(name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]){
		    mk_end_para(&c);
		    if(!closing){
			c.blank = 1;
			mk_inline(&c, (name[1] == '1') ? "h1"
				      : (name[1] == '2') ? "h2" : "h3", 0);
		    }
		}
		else if(!strcmp(name, "ul") || !strcmp(name, "ol")){
		    mk_end_item(&c);
		    if(!closing){
			mk_end_para(&c);
			if(c.nlist < HTML_MAXSTACK)
			  c.list[c.nlist++] = (name[0] == 'o') ? 2 : 1;

			so_puts(c.out, (name[0] == 'o') ? "<ol>" : "<ul>");
			c.out_lines++;
		    }
		    else if(c.nlist > 0){
			long at = so_tell(c.out);

			c.nlist--;
			if(at > 0 && ((char *) so_text(c.out))[at - 1] == '\n')
			  so_truncate(c.out, at - 1);

			so_puts(c.out, (name[0] == 'o') ? "</ol>\n" : "</ul>\n");
			c.out_lines++;
		    }
		}
		else if(!strcmp(name, "li")){
		    mk_end_item(&c);
		    if(!closing)
		      c.in_item = 1;
		}
		else if(!strcmp(name, "blockquote")){
		    mk_end_para(&c);
		    if(!closing){
			so_puts(c.out, "<blockquote>\n");
			c.quote++;
		    }
		    else if(c.quote > 0){
			so_puts(c.out, "</blockquote>\n");
			c.quote--;
		    }

		    c.out_lines++;
		}
		else if(!strcmp(name, "pre")){
		    mk_end_para(&c);
		    if(!closing){
			so_puts(c.out, "<pre>\n");
			c.pre = 1;
		    }
		    else{
			so_puts(c.out, (char *) so_text(c.para));
			so_truncate(c.para, 0L);
			so_puts(c.out, "\n</pre>\n");
			c.pre = 0;
		    }

		    c.out_lines++;
		}
		else if(!strcmp(name, "hr")){
		    mk_end_para(&c);
		    so_puts(c.out, "<hr>\n");
		    c.out_lines++;
		}
		else if(!strcmp(name, "img")){
		    if(html_attr_value(attrs, gt, "alt", val, sizeof(val)) && val[0]){
			mk_text(&c, "[", 1);
			mk_text(&c, val, (int) strlen(val));
			mk_text(&c, "]", 1);
		    }
		    else
		      mk_text(&c, "[image]", 7);
		}

		/* other tags vanish */
		continue;
	    }
	}

	if(*p == '&' && html_entity_decode(p, &elen, ubuf, sizeof(ubuf))){
	    /* nbsp is a space that must survive collapsing */
	    if(!strcmp(ubuf, "\xC2\xA0"))
	      mk_text(&c, " ", 1);
	    else
	      mk_text(&c, ubuf, (int) strlen(ubuf));

	    p += elen;
	    continue;
	}

	mk_text(&c, p, 1);
	p++;
    }

    mk_end_para(&c);
    while(c.nlist > 0){
	c.nlist--;
	so_puts(c.out, "</ul>\n");
    }

    while(c.quote > 0){
	so_puts(c.out, "</blockquote>\n");
	c.quote--;
    }

    out = cpystr((char *) so_text(c.out));
    so_give(&c.out);
    so_give(&c.para);
    return(out);
}


/*----------------------------------------------------------------------
    multipart/alternative and inline image support
  ----*/

/*
 * b is a text/html body produced by htmlify_composed_text().  Turn it
 * into multipart/alternative with a text/plain rendering first.  The
 * caller sets the sizes of the leaf parts.
 */
void
html_wrap_alternative(struct mail_bodystruct *b)
{
    BODY     htmlb;
    PART    *p1, *p2;
    STORE_S *pso;
    char    *html, *plain, *charset;

    if(!(b && b->type == TYPETEXT && b->subtype && !strucmp(b->subtype, "HTML")))
      return;

    html = html_read_all((STORE_S *) b->contents.text.data);
    if(!html)
      return;

    plain = html_to_plain(html);
    fs_give((void **) &html);
    if(!plain)
      return;

    pso = so_get(CharStar, NULL, EDIT_ACCESS);
    if(!pso){
	fs_give((void **) &plain);
	return;
    }

    so_puts(pso, plain);
    fs_give((void **) &plain);
    (void) so_attr(pso, "edited", "1");

    /* label and encode the html part for what it now contains */
    b->encoding = ENCOTHER;
    set_mime_type_by_grope(b);

    htmlb = *b;
    memset(b, 0, sizeof(*b));
    b->type = TYPEMULTIPART;
    b->subtype = cpystr("ALTERNATIVE");
    p1 = mail_newbody_part();
    p2 = mail_newbody_part();
    b->nested.part = p1;
    p1->next = p2;
    p1->body.type = TYPETEXT;
    p1->body.subtype = cpystr("PLAIN");
    p1->body.contents.text.data = (void *) pso;
    p1->body.encoding = ENCOTHER;
    if((charset = parameter_val(htmlb.parameter, "charset")) != NULL){
	set_parameter(&p1->body.parameter, "charset", charset);
	fs_give((void **) &charset);
    }

    set_mime_type_by_grope(&p1->body);
    p2->body = htmlb;
}


/*
 * Collect the cid: references in img tags.  *list gets a NULL
 * terminated array of ids (without the "cid:"), freed by the caller.
 */
int
html_find_cid_refs(char *html, char ***list)
{
    char  *p, *attrs, *gt, name[HTML_MAXNAME], val[MAILTMPLEN];
    int	   len, closing, n = 0, alloc = 0, i;
    char **ids = NULL;

    *list = NULL;
    for(p = html; p && *p; ){
	if(*p != '<'){
	    p++;
	    continue;
	}

	len = html_parse_tag(p, name, sizeof(name), &closing, &attrs, &gt);
	if(!len){
	    p++;
	    continue;
	}

	if(!strcmp(name, "img")
	   && html_attr_value(attrs, gt, "src", val, sizeof(val))
	   && html_safe_cid(val)){
	    for(i = 0; i < n; i++)
	      if(!strcmp(ids[i], val + 4))
		break;

	    if(i == n){
		if(n + 2 > alloc){
		    alloc += 8;
		    fs_resize((void **) &ids, alloc * sizeof(char *));
		}

		ids[n++] = cpystr(val + 4);
		ids[n] = NULL;
	    }
	}

	p += len;
    }

    *list = ids;
    return(n);
}


/*
 * A fresh Content-ID (without angle brackets) for an inline image.
 */
int
html_new_cid(char *buf, size_t buflen)
{
    static unsigned long counter = 0;
    char *host;

    host = (ps_global && ps_global->maildomain && *ps_global->maildomain)
	     ? ps_global->maildomain
	     : ((ps_global && ps_global->hostname && *ps_global->hostname)
		  ? ps_global->hostname : "localhost");

    snprintf(buf, buflen, "img-%lx-%lx-%lu@%s",
	     (unsigned long) time((time_t *) 0),
	     (unsigned long) (random() & 0xffffff),
	     ++counter, host);
    buf[buflen - 1] = '\0';
    return(1);
}
