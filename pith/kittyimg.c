/*
 * ========================================================================
 * Copyright 2026 Adam DePrince
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * ========================================================================
 */

/*======================================================================

     kittyimg.c
     Display MIME images in the message viewer using the kitty
     graphics protocol when available, otherwise sixels.  Preview
     size is a cell box (at most KITTY_MAX_COLS by KITTY_MAX_ROWS);
     cell pixel size is used only to rasterize into that box.

  ====*/


#include "../pith/headers.h"
#include "../pith/kittyimg.h"
#include "../pith/state.h"
#include "../pith/conf.h"
#include "../pith/filter.h"
#include "../pith/store.h"
#include "../pith/detach.h"
#include "../pith/msgno.h"
#include "../pith/string.h"
#include "../pith/util.h"
#include "../pith/status.h"

#ifndef	_WINDOWS
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#endif


#define	KITTY_MAX_BYTES		(3L * 1024L * 1024L)
#define	KITTY_ID_BASE		0xA10001u
#define	KITTY_MAX_COLS		32
#define	KITTY_MAX_ROWS		8
#define	KITTY_MIN_COLS		1
#define	KITTY_MIN_ROWS		1

#define	GFX_UNKNOWN		(-1)
#define	GFX_NONE		0
#define	GFX_KITTY		1
#define	GFX_SIXEL		2


typedef struct kitty_img_s {
    unsigned	 id;
    char	*png_path;
    char	*src_path;
    char	*sixel;
    int		 rows;
    int		 cols;
    int		 px_w;
    int		 px_h;
    unsigned	 transmitted:1;
    unsigned	 own_png:1;
    unsigned	 own_src:1;
} KITTY_IMG_S;


static KITTY_IMG_S *k_imgs = NULL;
static int	    k_n = 0;
static int	    k_alloc = 0;
static unsigned	    k_next_id = KITTY_ID_BASE;
static int	    k_gfx = GFX_UNKNOWN;
static int	    k_cell_w = 8;
static int	    k_cell_h = 16;
static int	    k_cell_valid = 0;


/* internal prototypes */
int	 kitty_detect_gfx(void);
int	 kitty_env_kitty(void);
int	 kitty_env_sixel(void);
#ifndef	_WINDOWS
int	 kitty_query_read(const char *, char *, int, int);
int	 kitty_query_kitty(void);
int	 kitty_query_sixel(void);
int	 kitty_query_cell_csi(int *, int *);
#endif
int	 kitty_cell_pixels(int *, int *);
void	 kitty_fit_preview(int, int, int *, int *, int *, int *);
int	 kitty_read_u16be(unsigned char *);
unsigned long kitty_read_u32be(unsigned char *);
unsigned long kitty_read_u32le(unsigned char *);
int	 kitty_read_u16le(unsigned char *);
int	 kitty_dims_from_mem(unsigned char *, long, int *, int *);
int	 kitty_dims_from_file(char *, int *, int *);
int	 kitty_file_is_png(char *);
char	*kitty_convert_to_png(char *, int, int);
unsigned char *kitty_load_ppm(char *, int *, int *);
unsigned char *kitty_load_bmp(char *, int *, int *);
unsigned char *kitty_convert_to_rgb(char *, int, int, int *, int *);
char	*kitty_sixel_encode(unsigned char *, int, int);
char	*kitty_b64(const char *);
#ifndef	_WINDOWS
int	 kitty_run_cmd(char **);
#endif
int	 kitty_register(char *, char *, char *, int, int, int, int, int, int);
KITTY_IMG_S *kitty_find(unsigned);
void	 kitty_write_raw(const char *);
void	 kitty_write_apc(const char *, const char *);
void	 kitty_transmit(KITTY_IMG_S *);
void	 kitty_delete_id(unsigned, int);
void	 kitty_hide_placements(void);
void	 kitty_place(int, int, KITTY_IMG_S *);
void	 kitty_place_sixel(int, int, KITTY_IMG_S *);
int	 kitty_handle_on_line(char *, int, HANDLE_S *, HANDLE_S **);
long	 kitty_current_rawno(void);
int	 kitty_extract_part(ATTACH_S *, long, char **);
int	 kitty_decode_data_uri(char *, char **);
void	 kitty_reset_shown(void);
int	 kitty_prep_from_file(char *, int, int *, int *);


int
kitty_img_enabled(void)
{
#ifdef	_WINDOWS
    return(0);
#else
    if(F_OFF(F_KITTY_INLINE_IMAGES, ps_global))
      return(0);

    return(kitty_detect_gfx() != GFX_NONE);
#endif
}


int
kitty_img_count(void)
{
    return(k_n);
}


void
kitty_img_begin_message(void)
{
    kitty_img_end_message();
    kitty_reset_shown();
}


void
kitty_img_note_resize(void)
{
    k_cell_valid = 0;
}


void
kitty_reset_shown(void)
{
    ATTACH_S *a;

    if(!(ps_global && ps_global->atmts))
      return;

    for(a = ps_global->atmts; a->description != NULL; a++){
	if(a->kitty_shown){
	    a->kitty_shown = 0;
	    a->shown = 0;
	}
    }
}


void
kitty_img_end_message(void)
{
    int i;

    for(i = 0; i < k_n; i++){
	if(k_imgs[i].transmitted)
	  kitty_delete_id(k_imgs[i].id, 1);

	if(k_imgs[i].png_path){
	    if(k_imgs[i].own_png)
	      our_unlink(k_imgs[i].png_path);

	    fs_give((void **) &k_imgs[i].png_path);
	}

	if(k_imgs[i].src_path){
	    if(k_imgs[i].own_src)
	      our_unlink(k_imgs[i].src_path);

	    fs_give((void **) &k_imgs[i].src_path);
	}

	if(k_imgs[i].sixel)
	  fs_give((void **) &k_imgs[i].sixel);
    }

    if(k_imgs)
      fs_give((void **) &k_imgs);

    k_imgs = NULL;
    k_n = 0;
    k_alloc = 0;
    k_next_id = KITTY_ID_BASE;
}


int
kitty_img_from_part(ATTACH_S *a, long int msgno, int *rowsp, int *colsp)
{
    char *src = NULL;
    int   rows = 0, cols = 0, id;

    if(!(a && a->body && a->number && rowsp && colsp))
      return(0);

    if(a->body->type != TYPEIMAGE)
      return(0);

    if(a->body->size.bytes > KITTY_MAX_BYTES){
	dprint((5, "kittyimg: skip oversized part %s (%lu)\n",
		   a->number ? a->number : "?",
		   (unsigned long) a->body->size.bytes));
	return(0);
    }

    if(msgno <= 0)
      msgno = kitty_current_rawno();

    if(msgno <= 0 || !kitty_extract_part(a, msgno, &src) || !src)
      return(0);

    id = kitty_prep_from_file(src, 1, &rows, &cols);
    if(id <= 0)
      return(0);

    *rowsp = rows;
    *colsp = cols;
    a->kitty_shown = 1;
    return(id);
}


int
kitty_img_from_cid(char *cid, int *rowsp, int *colsp)
{
    ATTACH_S *a;
    char      wrapped[MAILTMPLEN];
    int	      i;
    long      msgno;

    if(!(cid && *cid && rowsp && colsp && ps_global && ps_global->atmts))
      return(0);

    if(cid[0] != '<'){
	snprintf(wrapped, sizeof(wrapped), "<%s>", cid);
	wrapped[sizeof(wrapped)-1] = '\0';
	cid = wrapped;
    }

    for(i = 0, a = NULL; ps_global->atmts[i].description != NULL; i++)
      if(ps_global->atmts[i].body
	 && ps_global->atmts[i].body->type == TYPEIMAGE
	 && ps_global->atmts[i].body->id
	 && !strcmp(ps_global->atmts[i].body->id, cid)){
	  a = &ps_global->atmts[i];
	  break;
      }

    if(!a)
      return(0);

    msgno = kitty_current_rawno();
    return(kitty_img_from_part(a, msgno, rowsp, colsp));
}


int
kitty_img_from_data_uri(char *src, int *rowsp, int *colsp)
{
    char *path = NULL;
    int   rows = 0, cols = 0, id;

    if(!(src && rowsp && colsp))
      return(0);

    if(!kitty_decode_data_uri(src, &path) || !path)
      return(0);

    id = kitty_prep_from_file(path, 1, &rows, &cols);
    if(id <= 0)
      return(0);

    *rowsp = rows;
    *colsp = cols;
    return(id);
}


int
kitty_img_emit_block(gf_o_t pc, HANDLE_S **handlesp, int kid,
		     int rows, int cols, char *alt)
{
    HANDLE_S *h;
    char      buf[32];
    int	      i;

    if(!(pc && handlesp && kid > 0 && rows > 0))
      return(0);

    h = new_handle(handlesp);
    if(!h)
      return(0);

    h->type	      = IMG;
    h->h.img.alt      = cpystr(alt ? alt : "Image");
    h->h.img.kitty_id = kid;
    h->h.img.rows     = rows;
    h->h.img.cols     = cols > 0 ? cols : 1;

    snprintf(buf, sizeof(buf), "%d", h->key);
    buf[sizeof(buf)-1] = '\0';

    if(!((*pc)(TAG_EMBED) && (*pc)(TAG_HANDLE)
	 && (*pc)((int) strlen(buf)) && gf_puts(buf, pc)))
      return(0);

    for(i = 0; i < rows; i++)
      if(!gf_puts(NEWLINE, pc))
	return(0);

    if(!((*pc)(TAG_EMBED) && (*pc)(TAG_HANDLEOFF)))
      return(0);

    return(1);
}


void
kitty_img_paint_lines(char **lines, short *lens, long int offset,
		      int start_row, int nlines, HANDLE_S *handles)
{
    int i, skip = 0;

    if(!kitty_img_enabled() || k_n <= 0 || !lines || nlines <= 0)
      return;

    kitty_hide_placements();

    for(i = 0; i < nlines; i++){
	HANDLE_S *h = NULL;
	char	 *line;
	int	  len;

	if(skip > 0){
	    skip--;
	    continue;
	}

	if((offset + i) < 0)
	  continue;

	line = lines[offset + i];
	len  = lens ? lens[offset + i] : (line ? (int) strlen(line) : 0);
	if(!line || len <= 0)
	  continue;

	if(kitty_handle_on_line(line, len, handles, &h)
	   && h && h->h.img.kitty_id > 0){
	    KITTY_IMG_S *im = kitty_find((unsigned) h->h.img.kitty_id);

	    if(im){
		if(!im->transmitted)
		  kitty_transmit(im);

		if(im->transmitted)
		  kitty_place(start_row + i, 0, im);

		if(h->h.img.rows > 1)
		  skip = h->h.img.rows - 1;
	    }
	}
    }

    fflush(stdout);
}


void
kitty_img_paint_text(char *text, int start_row, int nlines, HANDLE_S *handles)
{
    char *p, *line;
    int   i, skip = 0, len;

    if(!kitty_img_enabled() || k_n <= 0 || !text || nlines <= 0)
      return;

    kitty_hide_placements();

    p = text;
    for(i = 0; i < nlines && *p; i++){
	HANDLE_S *h = NULL;

	line = p;
	for(; *p && *p != '\n' && *p != '\r'; p++)
	  ;

	len = p - line;

	if(*p == '\r' && *(p+1) == '\n')
	  p += 2;
	else if(*p == '\n' || *p == '\r')
	  p++;

	if(skip > 0){
	    skip--;
	    continue;
	}

	if(kitty_handle_on_line(line, len, handles, &h)
	   && h && h->h.img.kitty_id > 0){
	    KITTY_IMG_S *im = kitty_find((unsigned) h->h.img.kitty_id);

	    if(im){
		if(!im->transmitted)
		  kitty_transmit(im);

		if(im->transmitted)
		  kitty_place(start_row + i, 0, im);

		if(h->h.img.rows > 1)
		  skip = h->h.img.rows - 1;
	    }
	}
    }

    fflush(stdout);
}


/*
 * Internal helpers
 */


int
kitty_env_kitty(void)
{
    char *s;

    if(getenv("KITTY_WINDOW_ID") || getenv("KITTY_PID"))
      return(1);

    if((s = getenv("TERM")) != NULL
       && (strstr(s, "kitty") != NULL
	   || strstr(s, "ghostty") != NULL
	   || strstr(s, "wezterm") != NULL))
      return(1);

    if((s = getenv("TERM_PROGRAM")) != NULL
       && (!strucmp(s, "kitty")
	   || !strucmp(s, "WezTerm")
	   || !strucmp(s, "ghostty")))
      return(1);

    if(getenv("GHOSTTY_RESOURCES_DIR") || getenv("WEZTERM_EXECUTABLE"))
      return(1);

    return(0);
}


int
kitty_env_sixel(void)
{
    char *s;

    if((s = getenv("TERM")) != NULL
       && (strstr(s, "sixel") != NULL
	   || !struncmp(s, "mlterm", 6)
	   || !struncmp(s, "foot", 4)
	   || strstr(s, "mintty") != NULL))
      return(1);

    if((s = getenv("TERM_PROGRAM")) != NULL
       && !strucmp(s, "iTerm.app"))
      return(1);

    if((s = getenv("LC_TERMINAL")) != NULL
       && !strucmp(s, "iTerm2"))
      return(1);

    return(0);
}


#ifndef	_WINDOWS
int
kitty_query_read(const char *query, char *buf, int buflen, int timeout_ms)
{
    struct timeval tv;
    fd_set	   fds;
    int		   n, got = 0, oldfl;

    if(!query || !buf || buflen < 8)
      return(0);

    if(!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
      return(0);

    fputs(query, stdout);
    fflush(stdout);

    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    if(select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0)
      return(0);

    oldfl = fcntl(STDIN_FILENO, F_GETFL, 0);
    if(oldfl >= 0)
      (void) fcntl(STDIN_FILENO, F_SETFL, oldfl | O_NONBLOCK);

    while(got < buflen - 1){
	n = (int) read(STDIN_FILENO, buf + got, (size_t) (buflen - 1 - got));
	if(n > 0){
	    got += n;
	    tv.tv_sec = 0;
	    tv.tv_usec = 40000;
	    FD_ZERO(&fds);
	    FD_SET(STDIN_FILENO, &fds);
	    if(select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0)
	      break;
	}
	else
	  break;
    }

    if(oldfl >= 0)
      (void) fcntl(STDIN_FILENO, F_SETFL, oldfl);

    buf[got] = '\0';
    return(got);
}


int
kitty_query_kitty(void)
{
    char buf[256];
    int  n, tmux;

    tmux = (getenv("TMUX") != NULL);
    if(tmux)
      n = kitty_query_read("\033Ptmux;\033\033_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\033\033\\\033\\",
			   buf, sizeof(buf), 150);
    else
      n = kitty_query_read("\033_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\033\\",
			   buf, sizeof(buf), 150);

    if(n <= 0)
      return(0);

    return(strstr(buf, "OK") != NULL || strstr(buf, "_Gi=") != NULL);
}


int
kitty_query_sixel(void)
{
    char *p, buf[256];
    int   n;

    n = kitty_query_read("\033[c", buf, sizeof(buf), 150);
    if(n <= 0)
      return(0);

    p = buf;
    while((p = strchr(p, '4')) != NULL){
	if((p == buf || p[-1] == '?' || p[-1] == ';')
	   && (p[1] == ';' || p[1] == 'c' || p[1] == '\0'))
	  return(1);

	p++;
    }

    return(0);
}


int
kitty_query_cell_csi(int *cwp, int *chp)
{
    char buf[128], *p;
    int  n, w = 0, h = 0;

    n = kitty_query_read("\033[16t", buf, sizeof(buf), 80);
    if(n <= 0)
      return(0);

    p = strstr(buf, "\033[6;");
    if(!p)
      p = strstr(buf, "[6;");

    if(!p)
      return(0);

    if(p[0] == '\033')
      p++;

    if(sscanf(p, "[6;%d;%dt", &h, &w) != 2)
      return(0);

    if(w <= 0 || h <= 0)
      return(0);

    *cwp = w;
    *chp = h;
    return(1);
}
#endif /* !_WINDOWS */


int
kitty_detect_gfx(void)
{
    if(k_gfx != GFX_UNKNOWN)
      return(k_gfx);

    k_gfx = GFX_NONE;

    if(kitty_env_kitty())
      k_gfx = GFX_KITTY;
    else if(kitty_env_sixel())
      k_gfx = GFX_SIXEL;
#ifndef	_WINDOWS
    else if(kitty_query_kitty())
      k_gfx = GFX_KITTY;
    else if(kitty_query_sixel())
      k_gfx = GFX_SIXEL;
#endif

    return(k_gfx);
}


int
kitty_cell_pixels(int *cwp, int *chp)
{
    if(k_cell_valid){
	*cwp = k_cell_w;
	*chp = k_cell_h;
	return(k_cell_w > 0 && k_cell_h > 0);
    }

    k_cell_w = 8;
    k_cell_h = 16;

#ifdef	TIOCGWINSZ
    {
	struct winsize ws;

	if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0
	   && ws.ws_col > 0 && ws.ws_row > 0
	   && ws.ws_xpixel > 0 && ws.ws_ypixel > 0){
	    k_cell_w = ws.ws_xpixel / ws.ws_col;
	    k_cell_h = ws.ws_ypixel / ws.ws_row;
	}
    }
#endif

#ifndef	_WINDOWS
    if(k_cell_w <= 0 || k_cell_h <= 0)
      (void) kitty_query_cell_csi(&k_cell_w, &k_cell_h);
#endif

    if(k_cell_w <= 0)
      k_cell_w = 8;

    if(k_cell_h <= 0)
      k_cell_h = 16;

    k_cell_valid = 1;
    *cwp = k_cell_w;
    *chp = k_cell_h;
    return(1);
}


void
kitty_fit_preview(int img_w, int img_h, int *colsp, int *rowsp,
		  int *px_wp, int *px_hp)
{
    int    cw, ch, cols, rows;
    double nat_c, nat_r, sc, sr, s;

    kitty_cell_pixels(&cw, &ch);
    if(cw < 1)
      cw = 8;

    if(ch < 1)
      ch = 16;

    /*
     * Preview size is a cell box, not a pixel budget.  Fit the
     * image into KITTY_MAX_COLS x KITTY_MAX_ROWS cells, keeping
     * aspect.  Cell pixels are used only to turn that box into a
     * raster for sixel/PNG.
     */
    if(img_w <= 0 || img_h <= 0){
	cols = KITTY_MAX_COLS;
	rows = MIN(KITTY_MAX_ROWS, 6);
    }
    else{
	nat_c = (double) img_w / (double) cw;
	nat_r = (double) img_h / (double) ch;
	if(nat_c < 1.0)
	  nat_c = 1.0;

	if(nat_r < 1.0)
	  nat_r = 1.0;

	sc = (double) KITTY_MAX_COLS / nat_c;
	sr = (double) KITTY_MAX_ROWS / nat_r;
	s  = (sc < sr) ? sc : sr;

	cols = MAX(KITTY_MIN_COLS, (int) (nat_c * s + 0.5));
	rows = MAX(KITTY_MIN_ROWS, (int) (nat_r * s + 0.5));
	if(cols > KITTY_MAX_COLS)
	  cols = KITTY_MAX_COLS;

	if(rows > KITTY_MAX_ROWS)
	  rows = KITTY_MAX_ROWS;
    }

    *colsp = cols;
    *rowsp = rows;
    *px_wp = cols * cw;
    *px_hp = rows * ch;
}


int
kitty_prep_from_file(char *src, int own_src, int *rowsp, int *colsp)
{
    char	  *png = NULL, *sixel = NULL;
    unsigned char *rgb = NULL;
    int		   iw = 0, ih = 0, rows = 0, cols = 0, pw = 0, ph = 0;
    int		   rw = 0, rh = 0, own_png = 0, gfx, id;

    if(!src)
      return(0);

    gfx = kitty_detect_gfx();
    (void) kitty_dims_from_file(src, &iw, &ih);
    kitty_fit_preview(iw, ih, &cols, &rows, &pw, &ph);

    if(gfx == GFX_KITTY){
	png = kitty_convert_to_png(src, pw, ph);
	if(png)
	  own_png = 1;
	else if(kitty_file_is_png(src)){
	    png = src;
	    own_png = own_src;
	    own_src = 0;
	}
    }
    else if(gfx == GFX_SIXEL){
	rgb = kitty_convert_to_rgb(src, pw, ph, &rw, &rh);
	if(rgb && rw > 0 && rh > 0){
	    sixel = kitty_sixel_encode(rgb, rw, rh);
	    fs_give((void **) &rgb);
	}
    }

    if(!png && !sixel){
	if(own_src && src){
	    our_unlink(src);
	    fs_give((void **) &src);
	}

	return(0);
    }

    id = kitty_register(png, src, sixel, own_png, own_src, rows, cols, pw, ph);
    if(id <= 0){
	if(own_png && png && png != src){
	    our_unlink(png);
	    fs_give((void **) &png);
	}

	if(sixel)
	  fs_give((void **) &sixel);

	if(own_src && src){
	    our_unlink(src);
	    fs_give((void **) &src);
	}

	return(0);
    }

    *rowsp = rows;
    *colsp = cols;
    return(id);
}


int
kitty_read_u16be(unsigned char *p)
{
    return((p[0] << 8) | p[1]);
}


unsigned long
kitty_read_u32be(unsigned char *p)
{
    return(((unsigned long) p[0] << 24)
	   | ((unsigned long) p[1] << 16)
	   | ((unsigned long) p[2] << 8)
	   | (unsigned long) p[3]);
}


unsigned long
kitty_read_u32le(unsigned char *p)
{
    return((unsigned long) p[0]
	   | ((unsigned long) p[1] << 8)
	   | ((unsigned long) p[2] << 16)
	   | ((unsigned long) p[3] << 24));
}


int
kitty_read_u16le(unsigned char *p)
{
    return(p[0] | (p[1] << 8));
}


int
kitty_dims_from_mem(unsigned char *b, long int n, int *wp, int *hp)
{
    *wp = *hp = 0;
    if(!b || n < 10)
      return(0);

    /* PNG */
    if(n >= 24
       && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G'
       && b[4] == 0x0d && b[5] == 0x0a && b[6] == 0x1a && b[7] == 0x0a
       && b[12] == 'I' && b[13] == 'H' && b[14] == 'D' && b[15] == 'R'){
	*wp = (int) kitty_read_u32be(b + 16);
	*hp = (int) kitty_read_u32be(b + 20);
	return(*wp > 0 && *hp > 0);
    }

    /* GIF */
    if(n >= 10
       && b[0] == 'G' && b[1] == 'I' && b[2] == 'F'
       && b[3] == '8' && (b[4] == '7' || b[4] == '9') && b[5] == 'a'){
	*wp = kitty_read_u16le(b + 6);
	*hp = kitty_read_u16le(b + 8);
	return(*wp > 0 && *hp > 0);
    }

    /* BMP */
    if(n >= 26 && b[0] == 'B' && b[1] == 'M'){
	long w, h;

	w = (long) kitty_read_u32le(b + 18);
	h = (long) kitty_read_u32le(b + 22);
	if(h < 0)
	  h = -h;

	*wp = (int) w;
	*hp = (int) h;
	return(*wp > 0 && *hp > 0);
    }

    /* JPEG: walk markers for SOF0/SOF1/SOF2 */
    if(n > 4 && b[0] == 0xff && b[1] == 0xd8){
	long i = 2;

	while(i + 9 < n){
	    int marker, seglen;

	    if(b[i] != 0xff)
	      break;

	    while(i < n && b[i] == 0xff)
	      i++;

	    if(i >= n)
	      break;

	    marker = b[i++];
	    if(marker == 0xd8 || marker == 0xd9 || marker == 0x01)
	      continue;

	    if(i + 1 >= n)
	      break;

	    seglen = kitty_read_u16be(b + i);
	    if(seglen < 2 || i + seglen > n)
	      break;

	    if((marker >= 0xc0 && marker <= 0xc3)
	       || (marker >= 0xc5 && marker <= 0xc7)
	       || (marker >= 0xc9 && marker <= 0xcb)
	       || (marker >= 0xcd && marker <= 0xcf)){
		*hp = kitty_read_u16be(b + i + 3);
		*wp = kitty_read_u16be(b + i + 5);
		return(*wp > 0 && *hp > 0);
	    }

	    i += seglen;
	}
    }

    /* WebP */
    if(n >= 30
       && b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F'
       && b[8] == 'W' && b[9] == 'E' && b[10] == 'B' && b[11] == 'P'){
	if(b[12] == 'V' && b[13] == 'P' && b[14] == '8' && b[15] == ' '){
	    *wp = kitty_read_u16le(b + 26) & 0x3fff;
	    *hp = kitty_read_u16le(b + 28) & 0x3fff;
	    return(*wp > 0 && *hp > 0);
	}

	if(b[12] == 'V' && b[13] == 'P' && b[14] == '8' && b[15] == 'L'
	   && n >= 25){
	    unsigned long bits = kitty_read_u32le(b + 21);

	    *wp = (int) ((bits & 0x3fff) + 1);
	    *hp = (int) (((bits >> 14) & 0x3fff) + 1);
	    return(*wp > 0 && *hp > 0);
	}

	if(b[12] == 'V' && b[13] == 'P' && b[14] == '8' && b[15] == 'X'
	   && n >= 30){
	    *wp = 1 + (b[24] | (b[25] << 8) | (b[26] << 16));
	    *hp = 1 + (b[27] | (b[28] << 8) | (b[29] << 16));
	    return(*wp > 0 && *hp > 0);
	}
    }

    return(0);
}


int
kitty_dims_from_file(char *path, int *wp, int *hp)
{
    FILE	  *fp;
    unsigned char  buf[4096];
    long	   n;

    *wp = *hp = 0;
    if(!path)
      return(0);

    if((fp = our_fopen(path, "rb")) == NULL)
      return(0);

    n = (long) fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    if(n <= 0)
      return(0);

    return(kitty_dims_from_mem(buf, n, wp, hp));
}


int
kitty_file_is_png(char *path)
{
    FILE	  *fp;
    unsigned char  sig[8];

    if(!path || (fp = our_fopen(path, "rb")) == NULL)
      return(0);

    if(fread(sig, 1, 8, fp) != 8){
	fclose(fp);
	return(0);
    }

    fclose(fp);
    return(sig[0] == 0x89 && sig[1] == 'P' && sig[2] == 'N' && sig[3] == 'G'
	   && sig[4] == 0x0d && sig[5] == 0x0a && sig[6] == 0x1a && sig[7] == 0x0a);
}


#ifndef	_WINDOWS
int
kitty_run_cmd(char **argv)
{
    pid_t pid;
    int   status, fd;

    if(!argv || !argv[0])
      return(-1);

    pid = fork();
    if(pid < 0)
      return(-1);

    if(pid == 0){
	fd = open("/dev/null", O_RDWR);
	if(fd >= 0){
	    dup2(fd, STDIN_FILENO);
	    dup2(fd, STDOUT_FILENO);
	    dup2(fd, STDERR_FILENO);
	    if(fd > 2)
	      close(fd);
	}

	execvp(argv[0], argv);
	_exit(127);
    }

    if(waitpid(pid, &status, 0) < 0)
      return(-1);

    return((WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1);
}
#endif


char *
kitty_convert_to_png(char *src, int pw, int ph)
{
#ifdef	_WINDOWS
    (void) src;
    (void) pw;
    (void) ph;
    return(NULL);
#else
    char *dst, *argv[12], geom[32], hbuf[16], wbuf[16];

    if(!src)
      return(NULL);

    if(pw < 1)
      pw = 256;

    if(ph < 1)
      ph = 128;

    dst = temp_nam_ext(NULL, "kp", "png");
    if(!dst)
      return(NULL);

    snprintf(wbuf, sizeof(wbuf), "%d", pw);
    snprintf(hbuf, sizeof(hbuf), "%d", ph);
    snprintf(geom, sizeof(geom), "%dx%d>", pw, ph);

    /*
     * Write a preview whose pixel size matches the current cell
     * geometry. sips -z is height then width; magick/convert ">"
     * only shrinks.
     */
    argv[0] = "sips";
    argv[1] = "-s";
    argv[2] = "format";
    argv[3] = "png";
    argv[4] = "-z";
    argv[5] = hbuf;
    argv[6] = wbuf;
    argv[7] = src;
    argv[8] = "--out";
    argv[9] = dst;
    argv[10] = NULL;
    if(kitty_run_cmd(argv) == 0 && kitty_file_is_png(dst))
      return(dst);

    argv[0] = "magick";
    argv[1] = src;
    argv[2] = "-resize";
    argv[3] = geom;
    argv[4] = dst;
    argv[5] = NULL;
    if(kitty_run_cmd(argv) == 0 && kitty_file_is_png(dst))
      return(dst);

    argv[0] = "convert";
    argv[1] = src;
    argv[2] = "-resize";
    argv[3] = geom;
    argv[4] = dst;
    argv[5] = NULL;
    if(kitty_run_cmd(argv) == 0 && kitty_file_is_png(dst))
      return(dst);

    our_unlink(dst);
    fs_give((void **) &dst);
    return(NULL);
#endif
}


static int
kitty_ppm_skip(FILE *fp)
{
    int c;

    for(;;){
	c = fgetc(fp);
	if(c == EOF)
	  return(0);

	if(c == '#'){
	    while((c = fgetc(fp)) != EOF && c != '\n')
	      ;

	    if(c == EOF)
	      return(0);
	}
	else if(!isspace((unsigned char) c)){
	    ungetc(c, fp);
	    return(1);
	}
    }
}


unsigned char *
kitty_load_ppm(char *path, int *wp, int *hp)
{
    FILE	  *fp;
    unsigned char *rgb;
    int		   w = 0, h = 0, maxv = 0, c;
    size_t	   need, got;

    *wp = *hp = 0;
    if(!path || (fp = our_fopen(path, "rb")) == NULL)
      return(NULL);

    if(fgetc(fp) != 'P' || fgetc(fp) != '6'){
	fclose(fp);
	return(NULL);
    }

    if(!kitty_ppm_skip(fp) || fscanf(fp, "%d", &w) != 1
       || !kitty_ppm_skip(fp) || fscanf(fp, "%d", &h) != 1
       || !kitty_ppm_skip(fp) || fscanf(fp, "%d", &maxv) != 1
       || w <= 0 || h <= 0 || maxv != 255){
	fclose(fp);
	return(NULL);
    }

    c = fgetc(fp);
    if(c == EOF || !isspace((unsigned char) c)){
	fclose(fp);
	return(NULL);
    }

    need = (size_t) w * (size_t) h * 3;
    rgb = (unsigned char *) fs_get(need);
    got = fread(rgb, 1, need, fp);
    fclose(fp);
    if(got != need){
	fs_give((void **) &rgb);
	return(NULL);
    }

    *wp = w;
    *hp = h;
    return(rgb);
}


unsigned char *
kitty_load_bmp(char *path, int *wp, int *hp)
{
    FILE	  *fp;
    unsigned char  hdr[54], *rgb, *row;
    unsigned long  off, w_ul;
    long	   h_l, y;
    int		   w, h, bits, x, topdown;
    size_t	   rowb;

    *wp = *hp = 0;
    if(!path || (fp = our_fopen(path, "rb")) == NULL)
      return(NULL);

    if(fread(hdr, 1, 54, fp) != 54
       || hdr[0] != 'B' || hdr[1] != 'M'){
	fclose(fp);
	return(NULL);
    }

    off  = kitty_read_u32le(hdr + 10);
    w_ul = kitty_read_u32le(hdr + 18);
    h_l  = (long) kitty_read_u32le(hdr + 22);
    bits = kitty_read_u16le(hdr + 28);
    if(kitty_read_u32le(hdr + 30) != 0
       || (bits != 24 && bits != 32)
       || w_ul == 0 || w_ul > 8192){
	fclose(fp);
	return(NULL);
    }

    topdown = 0;
    if(h_l < 0){
	h_l = -h_l;
	topdown = 1;
    }

    if(h_l <= 0 || h_l > 8192){
	fclose(fp);
	return(NULL);
    }

    w = (int) w_ul;
    h = (int) h_l;
    rowb = ((size_t) bits * (size_t) w + 31) / 32 * 4;

    if(fseek(fp, (long) off, SEEK_SET) != 0){
	fclose(fp);
	return(NULL);
    }

    rgb = (unsigned char *) fs_get((size_t) w * (size_t) h * 3);
    row = (unsigned char *) fs_get(rowb);
    for(y = 0; y < h; y++){
	unsigned char *dst;
	int	       srcy;

	if(fread(row, 1, rowb, fp) != rowb){
	    fs_give((void **) &rgb);
	    fs_give((void **) &row);
	    fclose(fp);
	    return(NULL);
	}

	srcy = topdown ? (int) y : (h - 1 - (int) y);
	dst = rgb + (size_t) srcy * (size_t) w * 3;
	for(x = 0; x < w; x++){
	    unsigned char *p = row + x * (bits / 8);

	    dst[x * 3 + 0] = p[2];
	    dst[x * 3 + 1] = p[1];
	    dst[x * 3 + 2] = p[0];
	}
    }

    fs_give((void **) &row);
    fclose(fp);
    *wp = w;
    *hp = h;
    return(rgb);
}


unsigned char *
kitty_convert_to_rgb(char *src, int pw, int ph, int *wp, int *hp)
{
#ifdef	_WINDOWS
    (void) src;
    (void) pw;
    (void) ph;
    *wp = *hp = 0;
    return(NULL);
#else
    char	  *dst, *argv[12], geom[32], hbuf[16], wbuf[16];
    unsigned char *rgb = NULL;

    *wp = *hp = 0;
    if(!src)
      return(NULL);

    if(pw < 1)
      pw = 256;

    if(ph < 1)
      ph = 128;

    snprintf(wbuf, sizeof(wbuf), "%d", pw);
    snprintf(hbuf, sizeof(hbuf), "%d", ph);
    snprintf(geom, sizeof(geom), "%dx%d>", pw, ph);

    dst = temp_nam_ext(NULL, "ks", "ppm");
    if(dst){
	argv[0] = "magick";
	argv[1] = src;
	argv[2] = "-resize";
	argv[3] = geom;
	argv[4] = dst;
	argv[5] = NULL;
	if(kitty_run_cmd(argv) == 0)
	  rgb = kitty_load_ppm(dst, wp, hp);

	if(!rgb){
	    argv[0] = "convert";
	    if(kitty_run_cmd(argv) == 0)
	      rgb = kitty_load_ppm(dst, wp, hp);
	}

	our_unlink(dst);
	fs_give((void **) &dst);
    }

    if(rgb)
      return(rgb);

    dst = temp_nam_ext(NULL, "ks", "bmp");
    if(dst){
	argv[0] = "sips";
	argv[1] = "-s";
	argv[2] = "format";
	argv[3] = "bmp";
	argv[4] = "-z";
	argv[5] = hbuf;
	argv[6] = wbuf;
	argv[7] = src;
	argv[8] = "--out";
	argv[9] = dst;
	argv[10] = NULL;
	if(kitty_run_cmd(argv) == 0)
	  rgb = kitty_load_bmp(dst, wp, hp);

	our_unlink(dst);
	fs_give((void **) &dst);
    }

    return(rgb);
#endif
}


typedef struct kitty_sbuf_s {
    char   *s;
    size_t  n;
    size_t  a;
} KITTY_SBUF;


void
kitty_sb_put(KITTY_SBUF *b, const char *p, size_t n)
{
    if(!b || !p || n == 0)
      return;

    if(b->n + n + 1 > b->a){
	b->a = b->a ? b->a : 1024;
	while(b->n + n + 1 > b->a)
	  b->a *= 2;

	if(b->s)
	  fs_resize((void **) &b->s, b->a);
	else
	  b->s = (char *) fs_get(b->a);
    }

    memcpy(b->s + b->n, p, n);
    b->n += n;
    b->s[b->n] = '\0';
}


void
kitty_sb_puts(KITTY_SBUF *b, const char *p)
{
    if(p)
      kitty_sb_put(b, p, strlen(p));
}


void
kitty_sb_putc(KITTY_SBUF *b, int c)
{
    char ch = (char) c;

    kitty_sb_put(b, &ch, 1);
}


void
kitty_sixel_run(KITTY_SBUF *b, int ch, int n)
{
    char tmp[24];

    if(n <= 0 || ch < 0)
      return;

    if(n >= 3){
	snprintf(tmp, sizeof(tmp), "!%d%c", n, ch);
	kitty_sb_puts(b, tmp);
    }
    else{
	while(n-- > 0)
	  kitty_sb_putc(b, ch);
    }
}


char *
kitty_sixel_encode(unsigned char *rgb, int w, int h)
{
    KITTY_SBUF	   b;
    unsigned char *idx;
    int		   i, x, y, band, dy, color, used[216], band_used[216];
    char	   tmp[48];

    memset(&b, 0, sizeof(b));
    if(!rgb || w <= 0 || h <= 0)
      return(NULL);

    idx = (unsigned char *) fs_get((size_t) w * (size_t) h);
    memset(used, 0, sizeof(used));
    for(i = 0; i < w * h; i++){
	int r = rgb[i * 3 + 0] * 5 / 255;
	int g = rgb[i * 3 + 1] * 5 / 255;
	int bl = rgb[i * 3 + 2] * 5 / 255;

	idx[i] = (unsigned char) (r * 36 + g * 6 + bl);
	used[idx[i]] = 1;
    }

    kitty_sb_puts(&b, "\033P0;1;0q");
    snprintf(tmp, sizeof(tmp), "\"1;1;%d;%d", w, h);
    kitty_sb_puts(&b, tmp);

    for(color = 0; color < 216; color++){
	if(!used[color])
	  continue;

	snprintf(tmp, sizeof(tmp), "#%d;2;%d;%d;%d",
		 color,
		 (color / 36) * 20,
		 ((color / 6) % 6) * 20,
		 (color % 6) * 20);
	kitty_sb_puts(&b, tmp);
    }

    for(band = 0; band < (h + 5) / 6; band++){
	int first = 1;

	memset(band_used, 0, sizeof(band_used));
	for(dy = 0; dy < 6; dy++){
	    y = band * 6 + dy;
	    if(y >= h)
	      break;

	    for(x = 0; x < w; x++)
	      band_used[idx[y * w + x]] = 1;
	}

	for(color = 0; color < 216; color++){
	    int run = 0, last = -1;

	    if(!band_used[color])
	      continue;

	    if(!first)
	      kitty_sb_putc(&b, '$');

	    first = 0;
	    snprintf(tmp, sizeof(tmp), "#%d", color);
	    kitty_sb_puts(&b, tmp);

	    for(x = 0; x < w; x++){
		int six = 0, ch;

		for(dy = 0; dy < 6; dy++){
		    y = band * 6 + dy;
		    if(y < h && idx[y * w + x] == color)
		      six |= 1 << dy;
		}

		ch = '?' + six;
		if(ch == last)
		  run++;
		else{
		    kitty_sixel_run(&b, last, run);
		    last = ch;
		    run = 1;
		}
	    }

	    kitty_sixel_run(&b, last, run);
	}

	kitty_sb_putc(&b, '-');
    }

    kitty_sb_puts(&b, "\033\\");
    fs_give((void **) &idx);
    return(b.s);
}


int
kitty_register(char *png, char *src, char *sixel, int own_png, int own_src,
	       int rows, int cols, int px_w, int px_h)
{
    KITTY_IMG_S *im;

    if((!png && !sixel) || rows <= 0 || cols <= 0)
      return(0);

    if(k_n >= k_alloc){
	k_alloc = k_alloc ? k_alloc * 2 : 8;
	if(k_imgs)
	  fs_resize((void **) &k_imgs, k_alloc * sizeof(KITTY_IMG_S));
	else{
	    k_imgs = (KITTY_IMG_S *) fs_get(k_alloc * sizeof(KITTY_IMG_S));
	    memset(k_imgs, 0, k_alloc * sizeof(KITTY_IMG_S));
	}
    }

    im = &k_imgs[k_n++];
    memset(im, 0, sizeof(KITTY_IMG_S));
    im->id	 = k_next_id++;
    im->png_path = png;
    im->src_path = (src && src != png) ? src : NULL;
    im->sixel	 = sixel;
    im->rows	 = rows;
    im->cols	 = cols;
    im->px_w	 = px_w;
    im->px_h	 = px_h;
    im->own_png	 = own_png ? 1 : 0;
    im->own_src	 = (own_src && im->src_path) ? 1 : 0;
    return((int) im->id);
}


KITTY_IMG_S *
kitty_find(unsigned int id)
{
    int i;

    for(i = 0; i < k_n; i++)
      if(k_imgs[i].id == id)
	return(&k_imgs[i]);

    return(NULL);
}


void
kitty_write_raw(const char *s)
{
    if(s)
      fputs(s, stdout);
}


void
kitty_write_apc(const char *ctrl, const char *payload)
{
#ifdef	_WINDOWS
    (void) ctrl;
    (void) payload;
#else
    int tmux = (getenv("TMUX") != NULL);

    if(!ctrl)
      return;

    if(tmux)
      kitty_write_raw("\033Ptmux;");

    if(tmux)
      fputc('\033', stdout);

    kitty_write_raw("\033_G");
    kitty_write_raw(ctrl);
    if(payload){
	fputc(';', stdout);
	kitty_write_raw(payload);
    }

    if(tmux)
      fputc('\033', stdout);

    kitty_write_raw("\033\\");

    if(tmux)
      kitty_write_raw("\033\\");
#endif
}


char *
kitty_b64(const char *src)
{
    static const char v[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned long srcl, n;
    unsigned char *s, *d, *ret;

    if(!src)
      return(NULL);

    srcl = strlen(src);
    n = ((srcl + 2) / 3) * 4;
    ret = (unsigned char *) fs_get((size_t) n + 1);
    s = (unsigned char *) src;
    d = ret;
    for(; srcl >= 3; s += 3, srcl -= 3){
	*d++ = v[s[0] >> 2];
	*d++ = v[((s[0] << 4) + (s[1] >> 4)) & 0x3f];
	*d++ = v[((s[1] << 2) + (s[2] >> 6)) & 0x3f];
	*d++ = v[s[2] & 0x3f];
    }

    if(srcl){
	*d++ = v[s[0] >> 2];
	*d++ = v[((s[0] << 4) + (--srcl ? (s[1] >> 4) : 0)) & 0x3f];
	*d++ = srcl ? v[(s[1] << 2) & 0x3f] : '=';
	*d++ = '=';
    }

    *d = '\0';
    return((char *) ret);
}


void
kitty_transmit(KITTY_IMG_S *im)
{
    char *b64;
    char  ctrl[80];

    if(!im || im->transmitted)
      return;

    if(im->sixel){
	im->transmitted = 1;
	return;
    }

    if(!im->png_path)
      return;

    b64 = kitty_b64(im->png_path);
    if(!b64)
      return;

    snprintf(ctrl, sizeof(ctrl), "a=t,f=100,t=f,i=%u,q=2", im->id);
    kitty_write_apc(ctrl, b64);
    fs_give((void **) &b64);
    im->transmitted = 1;
}


void
kitty_delete_id(unsigned int id, int free_data)
{
    char ctrl[80];

    snprintf(ctrl, sizeof(ctrl), "a=d,d=%c,i=%u,q=2",
	     free_data ? 'I' : 'i', id);
    kitty_write_apc(ctrl, NULL);
}


void
kitty_hide_placements(void)
{
    int i;

    for(i = 0; i < k_n; i++)
      if(k_imgs[i].transmitted && k_imgs[i].png_path && !k_imgs[i].sixel)
	kitty_delete_id(k_imgs[i].id, 0);
}


void
kitty_place_sixel(int row, int col, KITTY_IMG_S *im)
{
#ifdef	_WINDOWS
    (void) row;
    (void) col;
    (void) im;
#else
    if(!im || !im->sixel)
      return;

    fprintf(stdout, "\0337\033[%d;%dH", row + 1, col + 1);
    kitty_write_raw(im->sixel);
    kitty_write_raw("\0338");
#endif
}


void
kitty_place(int row, int col, KITTY_IMG_S *im)
{
#ifdef	_WINDOWS
    (void) row;
    (void) col;
    (void) im;
#else
    char ctrl[96];
    int  tmux;

    if(!im)
      return;

    if(im->sixel){
	kitty_place_sixel(row, col, im);
	return;
    }

    tmux = (getenv("TMUX") != NULL);
    if(tmux)
      fprintf(stdout, "\033Ptmux;\033\033[%d;%dH\033\\", row + 1, col + 1);
    else
      fprintf(stdout, "\033[%d;%dH", row + 1, col + 1);

    snprintf(ctrl, sizeof(ctrl), "a=p,i=%u,c=%d,r=%d,C=1,q=2",
	     im->id, im->cols, im->rows);
    kitty_write_apc(ctrl, NULL);
#endif
}


int
kitty_handle_on_line(char *line, int length, HANDLE_S *handles, HANDLE_S **hp)
{
    unsigned char c;

    if(!(line && length > 0 && handles && hp))
      return(0);

    *hp = NULL;

    while(length-- > 0){
	c = (unsigned char) *line++;
	if(c == (unsigned char) TAG_EMBED && length > 0){
	    length--;
	    switch(*line++){
	      case TAG_HANDLE : {
		  int key = 0, n;

		  if(length < 1)
		    return(0);

		  n = (unsigned char) *line++;
		  length--;
		  if(n < 0 || n > length)
		    return(0);

		  while(n--){
		      key = (key * 10) + (*line++ - '0');
		      length--;
		  }

		  *hp = get_handle(handles, key);
		  if(*hp
		     && ((*hp)->type == IMG || (*hp)->type == imgData)
		     && (*hp)->h.img.kitty_id > 0)
		    return(1);

		  *hp = NULL;
		  break;
	      }

	      case TAG_FGCOLOR :
	      case TAG_BGCOLOR :
		if(length < RGBLEN)
		  return(0);

		line += RGBLEN;
		length -= RGBLEN;
		break;

	      default :
		break;
	    }
	}
    }

    return(0);
}


long
kitty_current_rawno(void)
{
    if(!(ps_global && ps_global->msgmap && ps_global->mail_stream))
      return(0L);

    return(mn_m2raw(ps_global->msgmap, mn_get_cur(ps_global->msgmap)));
}


int
kitty_extract_part(ATTACH_S *a, long int msgno, char **pathp)
{
    STORE_S *so;
    gf_o_t   pc;
    char    *err, *path;

    *pathp = NULL;
    if(!(a && a->number && pathp && ps_global && ps_global->mail_stream))
      return(0);

    path = temp_nam(NULL, "ki");
    if(!path)
      return(0);

    so = so_get(FileStar, path, WRITE_ACCESS | OWNER_ONLY);
    if(!so){
	fs_give((void **) &path);
	return(0);
    }

    gf_set_so_writec(&pc, so);
    err = detach(ps_global->mail_stream, msgno, a->number, 0L, NULL, pc,
		 NULL, DT_BINARY);
    gf_clear_so_writec(so);
    if(so_give(&so))
      err = "write error";

    if(err || name_file_size(path) <= 0L){
	dprint((5, "kittyimg: extract failed for %s: %s\n",
		   a->number, err ? err : "empty"));
	our_unlink(path);
	fs_give((void **) &path);
	return(0);
    }

    *pathp = path;
    return(1);
}


int
kitty_decode_data_uri(char *src, char **pathp)
{
    STORE_S *raw, *out;
    gf_o_t   writec, pc;
    gf_i_t   readc;
    char    *comma, *payload, *path, *err = NULL;
    int	     is_b64 = 0;

    *pathp = NULL;
    if(!src)
      return(0);

    if(!struncmp(src, "data:", 5))
      src += 5;

    comma = strchr(src, ',');
    if(!comma || !comma[1])
      return(0);

    payload = comma + 1;
    if(srchstr(src, "base64") != NULL)
      is_b64 = 1;

    if((size_t) (comma - src) > 6 && !struncmp(src, "image/", 6))
      /* keep going */ ;

    path = temp_nam(NULL, "kd");
    if(!path)
      return(0);

    raw = so_get(CharStar, NULL, EDIT_ACCESS);
    out = so_get(FileStar, path, WRITE_ACCESS | OWNER_ONLY);
    if(!(raw && out)){
	if(raw)
	  so_give(&raw);

	if(out)
	  so_give(&out);

	our_unlink(path);
	fs_give((void **) &path);
	return(0);
    }

    gf_set_so_writec(&writec, raw);
    (void) gf_puts(payload, writec);
    gf_clear_so_writec(raw);

    so_seek(raw, 0L, 0);
    gf_set_so_readc(&readc, raw);
    gf_set_so_writec(&pc, out);
    gf_filter_init();
    if(is_b64)
      gf_link_filter(gf_b64_binary, NULL);

    err = gf_pipe(readc, pc);
    gf_clear_so_writec(out);
    gf_clear_so_readc(raw);
    if(so_give(&out))
      err = "write error";

    so_give(&raw);

    if(err || name_file_size(path) <= 0L){
	our_unlink(path);
	fs_give((void **) &path);
	return(0);
    }

    *pathp = path;
    return(1);
}
