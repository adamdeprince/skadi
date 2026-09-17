/*
 * ========================================================================
 * Copyright 2026 Eduardo Chappa
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 * ========================================================================
 *
 * Turn composer text that contains simple HTML markup into a
 * text/html body part.  Known tags are left alone; everything else
 * is escaped.  No colors or other decorative junk.
 */

#include "../pith/headers.h"
#include "../pith/htmlbody.h"
#include "../pith/store.h"
#include "../pith/filter.h"

#include <ctype.h>


static const char *html_ok_tags[] = {
    "b", "i", "u", "s", "ul", "ol", "li", "blockquote",
    "h1", "h2", "h3", "pre", "code", "a", "em", "strong",
    "p", "br", "hr", "div", "html", "head", "body", "meta",
    "!doctype", NULL
};


static int
html_tag_boundary(int c)
{
    return(c == '\0' || c == '>' || c == '/' || c == ' '
	   || c == '\t' || c == '\n' || c == '\r');
}


/*
 * s points at '<'.  Returns 1 if this is a known tag (open or close).
 */
static int
html_match_tag(char *s)
{
    char *p;
    int   i, nlen;

    if(!s || *s != '<')
      return(0);

    p = s + 1;
    if(*p == '/')
      p++;

    for(i = 0; html_ok_tags[i]; i++){
	nlen = strlen(html_ok_tags[i]);
	if(!struncmp(p, html_ok_tags[i], nlen) && html_tag_boundary(p[nlen]))
	  return(1);
    }

    return(0);
}


static int
html_tag_named(char *s, char *name)
{
    char *p;
    int   nlen;

    if(!s || *s != '<' || !name)
      return(0);

    p = s + 1;
    if(*p == '/')
      p++;

    nlen = strlen(name);
    return(!struncmp(p, name, nlen) && html_tag_boundary(p[nlen]));
}


static int
html_tag_closing(char *s)
{
    return(s && s[0] == '<' && s[1] == '/');
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

    return(*q == ';');
}


static int
html_block_line(char *line)
{
    char *p;

    if(!line)
      return(0);

    for(p = line; *p == ' ' || *p == '\t'; p++)
      ;

    if(*p != '<')
      return(0);

    return(html_match_tag(p)
	   && (html_tag_named(p, "ul")
	       || html_tag_named(p, "ol")
	       || html_tag_named(p, "li")
	       || html_tag_named(p, "blockquote")
	       || html_tag_named(p, "h1")
	       || html_tag_named(p, "h2")
	       || html_tag_named(p, "h3")
	       || html_tag_named(p, "pre")
	       || html_tag_named(p, "p")
	       || html_tag_named(p, "hr")
	       || html_tag_named(p, "div")
	       || html_tag_named(p, "!doctype")
	       || html_tag_named(p, "html")
	       || html_tag_named(p, "head")
	       || html_tag_named(p, "body")
	       || html_tag_named(p, "meta")));
}


static int
html_text_has_markup(char *s)
{
    char *p, *gt;

    if(!s)
      return(0);

    for(p = s; *p; p++){
	if(*p != '<' || !html_match_tag(p))
	  continue;

	gt = strchr(p, '>');
	if(gt)
	  return(1);
    }

    return(0);
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
    (void) gf_pipe(gc, pc);
    gf_clear_so_readc(so);
    gf_clear_so_writec(tmp);
    s = cpystr((char *) so_text(tmp));
    so_give(&tmp);
    return(s);
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


static void
html_emit_escaped(STORE_S *nso, char c)
{
    if(c == '&')
      so_puts(nso, "&amp;");
    else if(c == '<')
      so_puts(nso, "&lt;");
    else if(c == '>')
      so_puts(nso, "&gt;");
    else
      so_writec((int) (unsigned char) c, nso);
}


void
htmlify_composed_text(struct mail_bodystruct *b)
{
    STORE_S *so, *nso;
    char    *src, *p, *line, *gt;
    int	     wrapped, in_pre = 0;

    if(!compose_text_is_html(b))
      return;

    so = (STORE_S *) b->contents.text.data;
    src = html_read_all(so);
    if(!src)
      return;

    wrapped = (!struncmp(src, "<!doctype", 9) || !struncmp(src, "<html", 5));

    nso = so_get(CharStar, NULL, EDIT_ACCESS);
    if(!nso){
	fs_give((void **) &src);
	return;
    }

    if(!wrapped)
      so_puts(nso, "<!DOCTYPE html>\n<html><head><meta charset=\"UTF-8\"></head>\n<body>\n");

    p = src;
    line = src;
    while(*p){
	if(*p == '\n'){
	    if(!wrapped && !in_pre && !html_block_line(line))
	      so_puts(nso, "<br>\n");
	    else
	      so_puts(nso, "\n");

	    p++;
	    line = p;
	    continue;
	}

	if(*p == '<' && html_match_tag(p) && (gt = strchr(p, '>'))){
	    if(html_tag_named(p, "pre"))
	      in_pre = !html_tag_closing(p);

	    (void) so_nputs(nso, p, (long) (gt - p + 1));
	    p = gt + 1;
	    continue;
	}

	if(*p == '&' && html_is_entity(p)){
	    gt = strchr(p, ';');
	    (void) so_nputs(nso, p, (long) (gt - p + 1));
	    p = gt + 1;
	    continue;
	}

	html_emit_escaped(nso, *p);
	p++;
    }

    if(!wrapped)
      so_puts(nso, "</body></html>\n");

    fs_give((void **) &src);
    so_give(&so);
    b->contents.text.data = (void *) nso;
    if(b->subtype)
      fs_give((void **) &b->subtype);

    b->subtype = cpystr("HTML");
    b->type = TYPETEXT;
    html_wrap_alternative(b);
}


static void
html_plain_tag(char *tag, int closing, int *list, int *oln, int *quote, int *pre,
	       STORE_S *so)
{
    if(!tag)
      return;

    if(!strucmp(tag, "br") || !strucmp(tag, "p") || !strucmp(tag, "div")
       || !strucmp(tag, "h1") || !strucmp(tag, "h2") || !strucmp(tag, "h3"))
      so_puts(so, "\n");
    else if(!strucmp(tag, "hr"))
      so_puts(so, "\n----\n");
    else if(!strucmp(tag, "li")){
	int i;

	if(!closing){
	    so_puts(so, "\n");
	    for(i = 0; i < *quote; i++)
	      so_puts(so, "> ");

	    if(*list == 2){
		char nbuf[16];

		snprintf(nbuf, sizeof(nbuf), "%d. ", ++(*oln));
		so_puts(so, nbuf);
	    }
	    else
	      so_puts(so, "* ");
	}
    }
    else if(!strucmp(tag, "ul")){
	if(!closing)
	  *list = 1;
	else
	  *list = 0;
    }
    else if(!strucmp(tag, "ol")){
	if(!closing){
	    *list = 2;
	    *oln = 0;
	}
	else
	  *list = 0;
    }
    else if(!strucmp(tag, "blockquote")){
	if(!closing)
	  (*quote)++;
	else if(*quote)
	  (*quote)--;
    }
    else if(!strucmp(tag, "pre"))
      *pre = !closing;
}


static char *
html_tag_name(char *s, char *buf, size_t buflen)
{
    char *p;
    size_t n = 0;

    buf[0] = '\0';
    if(!s || *s != '<')
      return(buf);

    p = s + 1;
    if(*p == '/')
      p++;

    while(*p && n < buflen - 1 && isalnum((unsigned char) *p))
      buf[n++] = *p++;

    buf[n] = '\0';
    return(buf);
}


char *
html_to_plain(char *src)
{
    STORE_S *so;
    char    *p, *gt, tag[32], *out;
    int	     list = 0, oln = 0, quote = 0, pre = 0, closing;

    if(!src)
      return(NULL);

    so = so_get(CharStar, NULL, EDIT_ACCESS);
    if(!so)
      return(NULL);

    p = src;
    while(*p){
	if(*p == '<' && (gt = strchr(p, '>'))){
	    closing = html_tag_closing(p);
	    html_tag_name(p, tag, sizeof(tag));
	    if(html_match_tag(p)){
		if(!strucmp(tag, "a") && !closing){
		    char *hp = p, href[MAILTMPLEN];

		    href[0] = '\0';
		    while(hp < gt){
			if(!struncmp(hp, "href=", 5)){
			    char *q, quote = 0;

			    hp += 5;
			    if(*hp == '"' || *hp == '\'')
			      quote = *hp++;

			    q = href;
			    while(hp < gt && q < href + sizeof(href) - 1
				  && *hp && *hp != quote
				  && (quote || (*hp != ' ' && *hp != '>')))
			      *q++ = *hp++;

			    *q = '\0';
			    break;
			}

			hp++;
		    }

		    if(href[0]){
			so_puts(so, href);
			so_puts(so, " ");
		    }
		}

		html_plain_tag(tag, closing, &list, &oln, &quote, &pre, so);
	    }

	    p = gt + 1;
	    continue;
	}

	if(*p == '&' && html_is_entity(p)){
	    if(!struncmp(p, "&amp;", 5)){
		so_writec('&', so);
		p += 5;
	    }
	    else if(!struncmp(p, "&lt;", 4)){
		so_writec('<', so);
		p += 4;
	    }
	    else if(!struncmp(p, "&gt;", 4)){
		so_writec('>', so);
		p += 4;
	    }
	    else if(!struncmp(p, "&nbsp;", 6)){
		so_writec(' ', so);
		p += 6;
	    }
	    else if(!struncmp(p, "&quot;", 6)){
		so_writec('"', so);
		p += 6;
	    }
	    else{
		gt = strchr(p, ';');
		if(gt)
		  p = gt + 1;
		else
		  p++;
	    }

	    continue;
	}

	so_writec((int) (unsigned char) *p, so);
	p++;
    }

    out = cpystr((char *) so_text(so));
    so_give(&so);
    return(out);
}


int
html_is_simple(char *s)
{
    if(!s || !html_text_has_markup(s))
      return(0);

    if(struncmp(s, "<table", 6) == 0 || strstr(s, "<table") || strstr(s, "<TABLE")
       || strstr(s, "<script") || strstr(s, "<style") || strstr(s, "<STYLE"))
      return(0);

    return(1);
}


char *
html_simple_to_display(char *src)
{
    STORE_S *so;
    char    *p, *gt, tag[32], *out, buf[8];
    int	     list = 0, oln = 0, quote = 0, pre = 0, closing;

    if(!src)
      return(NULL);

    so = so_get(CharStar, NULL, EDIT_ACCESS);
    if(!so)
      return(NULL);

    p = src;
    while(*p){
	if(*p == '<' && (gt = strchr(p, '>'))){
	    closing = html_tag_closing(p);
	    html_tag_name(p, tag, sizeof(tag));
	    if(html_match_tag(p)){
		buf[0] = TAG_EMBED;
		buf[2] = '\0';
		if(!strucmp(tag, "b") || !strucmp(tag, "strong")){
		    buf[1] = closing ? TAG_BOLDOFF : TAG_BOLDON;
		    so_nputs(so, buf, 2);
		}
		else if(!strucmp(tag, "u")){
		    buf[1] = closing ? TAG_ULINEOFF : TAG_ULINEON;
		    so_nputs(so, buf, 2);
		}
		else if(!strucmp(tag, "i") || !strucmp(tag, "em")){
		    buf[1] = closing ? TAG_ITALICOFF : TAG_ITALICON;
		    so_nputs(so, buf, 2);
		}
		else if(!strucmp(tag, "s")){
		    buf[1] = closing ? TAG_STRIKEOFF : TAG_STRIKEON;
		    so_nputs(so, buf, 2);
		}
		else if(!strucmp(tag, "a") && !closing){
		    char *hp = p, href[MAILTMPLEN];

		    href[0] = '\0';
		    while(hp < gt){
			if(!struncmp(hp, "href=", 5)){
			    char *q, quote = 0;

			    hp += 5;
			    if(*hp == '"' || *hp == '\'')
			      quote = *hp++;

			    q = href;
			    while(hp < gt && q < href + sizeof(href) - 1
				  && *hp && *hp != quote
				  && (quote || (*hp != ' ' && *hp != '>')))
			      *q++ = *hp++;

			    *q = '\0';
			    break;
			}

			hp++;
		    }

		    if(href[0]){
			so_puts(so, "[");
			so_puts(so, href);
			so_puts(so, "] ");
		    }
		}
		else
		  html_plain_tag(tag, closing, &list, &oln, &quote, &pre, so);
	    }

	    p = gt + 1;
	    continue;
	}

	if(*p == '&' && html_is_entity(p)){
	    if(!struncmp(p, "&amp;", 5)){
		so_writec('&', so);
		p += 5;
	    }
	    else if(!struncmp(p, "&lt;", 4)){
		so_writec('<', so);
		p += 4;
	    }
	    else if(!struncmp(p, "&gt;", 4)){
		so_writec('>', so);
		p += 4;
	    }
	    else if(!struncmp(p, "&nbsp;", 6)){
		so_writec(' ', so);
		p += 6;
	    }
	    else if(!struncmp(p, "&quot;", 6)){
		so_writec('"', so);
		p += 6;
	    }
	    else{
		gt = strchr(p, ';');
		p = gt ? gt + 1 : p + 1;
	    }

	    continue;
	}

	so_writec((int) (unsigned char) *p, so);
	p++;
    }

    out = cpystr((char *) so_text(so));
    so_give(&so);
    return(out);
}


void
html_wrap_alternative(struct mail_bodystruct *b)
{
    BODY    *alt, htmlb;
    PART    *p1, *p2;
    STORE_S *pso;
    char    *html, *plain;

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
    p2->body = htmlb;
}
