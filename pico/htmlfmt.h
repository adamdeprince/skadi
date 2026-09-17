/*
 * ========================================================================
 * Copyright 2026 Adam DePrince
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 * ========================================================================
 */

#ifndef PICO_HTMLFMT_INCLUDED
#define PICO_HTMLFMT_INCLUDED


/* CELL.a bits used on the virtual screen (bit 0 is existing reverse). */
#define HA_REV		0x01
#define HA_BOLD		0x02
#define HA_ULINE	0x04
#define HA_ITAL		0x08
#define HA_STRIKE	0x10


typedef struct {
    unsigned attr;
    int	     list;	/* 0 none, 1 ul, 2 ol */
    int	     ol_num;
    int	     quote;
    int	     pre;
} HTML_VIEW;


/*
 * HTML mode of the message being composed.  AUTO means the user never
 * touched it: the message is sent as HTML only if it already contains
 * a tag opened and closed on one line.
 */
#define	HTMLFMT_AUTO	(-1)
#define	HTMLFMT_OFF	0
#define	HTMLFMT_ON	1


/* markup commands */
int	htmlfmt_bold(int, int);
int	htmlfmt_italic(int, int);
int	htmlfmt_underline(int, int);
int	htmlfmt_strike(int, int);
int	htmlfmt_code(int, int);
int	htmlfmt_heading(int, int);
int	htmlfmt_quote(int, int);
int	htmlfmt_pre(int, int);
int	htmlfmt_hr(int, int);
int	htmlfmt_ulist(int, int);
int	htmlfmt_olist(int, int);
int	htmlfmt_link(int, int);
int	htmlfmt_image(int, int);
int	htmlfmt_toggle(int, int);
int	htmlfmt_preview(int, int);

/* mode */
int	htmlfmt_mode(void);
void	htmlfmt_set_mode(int);
int	htmlfmt_effective_html(void);
void	htmlfmt_set_preview_hook(char *(*)(char *, int *));

/* rendering */
void	html_view_init(HTML_VIEW *);
void	html_view_to_line(struct LINE *, HTML_VIEW *);
int	html_render_line(struct LINE *, HTML_VIEW *,
			 void (*)(CELL, void *), void *,
			 int, int *);
int	html_doto_for_col(struct LINE *, HTML_VIEW *, int);
void	html_term_cell_attr(int);

/* editing helpers */
int	html_known_tag_name(char *);
int	html_tag_starting_at(struct LINE *, int);
int	html_entity_starting_at(struct LINE *, int);
int	html_hidden_span(struct LINE *, int, int *, int *);
int	html_delete_before_dot(void);
int	html_delete_at_dot(void);
int	html_newline_in_item(void);
char   *html_writetmp_stripped(char *);

#endif
