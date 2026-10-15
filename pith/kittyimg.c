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
     Display MIME images in the message viewer.

     Images are decoded in-process with stb_image, resampled into a
     thumbnail that fits a cell box, and kept in a small cache keyed by
     message part (or data: URI) and cell size.  The formatted message
     text carries a selectable "[Image: ...]" handle line followed by
     the blank rows the thumbnail occupies.  Every screen paint places
     the thumbnails on those rows with the kitty graphics protocol
     (direct chunked transmission, so it works over ssh), iTerm2's
     inline image sequence, or sixels.

  ====*/


#ifndef	_WINDOWS
/*
 * The stb single-header libraries are compiled here, ahead of Alpine's
 * own headers so that c-client's short macro names (T, NIL, ...) cannot
 * collide with their identifiers.  Decoding is from memory only, the
 * float/HDR and rarely used formats are compiled out, dimensions are
 * capped, and the assertion macros are disabled so a malformed image
 * can never abort the mail client.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <stdint.h>
#include <webp/decode.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PIC
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_FAILURE_STRINGS
#define STBI_MAX_DIMENSIONS INT_MAX /* decoder uses signed int offsets */
#define STBI_ASSERT(x)		((void) 0)
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#define STBIW_ASSERT(x)		((void) 0)
#include "stb_image_write.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STBIR_ASSERT(x)		((void) 0)
#include "stb_image_resize2.h"
#endif /* !_WINDOWS */


#include "../pith/headers.h"
#include "../pith/kittyimg.h"
#include "../pith/remoteimg.h"
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
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#endif


#ifdef	_WINDOWS

/* No terminal graphics on Windows; keep the callers linking. */
int	  kitty_img_enabled(void) { return(0); }
int	  kitty_img_count(void) { return(0); }
void	  kitty_img_begin_message(void) { }
void	  kitty_img_end_message(void) { }
void	  kitty_img_shutdown(void) { }
void	  kitty_img_note_resize(void) { }
int	  kitty_img_from_part(ATTACH_S *a, long m, int *r, int *c) { return(0); }
int	  kitty_img_from_cid(char *s, int *r, int *c) { return(0); }
int	  kitty_img_from_data_uri(char *s, int *r, int *c) { return(0); }
int	  kitty_img_from_url(char *s, int f, int *r, int *c, const char **e) { return(0); }
ATTACH_S *kitty_img_attach_for_cid(char *s) { return(NULL); }
int	  kitty_img_emit_block(gf_o_t pc, HANDLE_S **h, int k, int r, int c,
			       ATTACH_S *a, char *alt) { return(0); }
void	  kitty_img_paint_lines(char **l, short *n, long o, int s, int c,
				long t, HANDLE_S *h) { }
char *
kitty_img_alt(char *alt, char *dst, size_t dstlen)
{
    if(dst && dstlen)
      dst[0] = '\0';

    return(dst);
}

#else /* !_WINDOWS */


#define	KITTY_MAX_BYTES		(3L * 1024L * 1024L)	/* encoded bytes we decode */
#define	KITTY_MAX_PIXELS	(40L * 1000L * 1000L)	/* decoded pixel budget */
#define	KITTY_MIN_PIXELS	8			/* skip tracking pixels */
#define	KITTY_ID_BASE		0xA10001u
#define	KITTY_ID_LIMIT		0x7FFFFF00u
#define	KITTY_DEF_COLS		32
#define	KITTY_DEF_ROWS		8
#define	KITTY_DEF_COUNT		12
#define	KITTY_CACHE_MAX_BYTES	(24L * 1024L * 1024L)
#define	KITTY_CACHE_MAX_ENTRIES	96
#define	KITTY_CHUNK		4096
#define	KITTY_ALT_MAX		60

#define	GFX_UNKNOWN		(-1)
#define	GFX_NONE		0
#define	GFX_KITTY		1
#define	GFX_ITERM		2
#define	GFX_SIXEL		3


/*
 * A resampled thumbnail.  The canvas is exactly cols*cw by rows*ch
 * pixels with the image drawn at the top left and transparent
 * padding, so terminals never rescale it.
 */
typedef struct kitty_thumb_s {
    char	  *key;
    unsigned char *rgba;
    int		   w, h;		/* canvas pixels */
    int		   iw, ih;		/* image pixels inside the canvas */
    int		   rows, cols;
    unsigned char *png;			/* lazily encoded canvas */
    size_t	   pnglen;
    size_t	   bytes;
    unsigned long  stamp;
    struct kitty_thumb_s *next;
} KITTY_THUMB_S;


/* An image shown in the message being viewed. */
typedef struct kitty_img_s {
    unsigned	   id;			/* kitty image id, unique for the session */
    KITTY_THUMB_S *thumb;
    int		   rows;
    int		   cols;
    unsigned	   transmitted:1;
} KITTY_IMG_S;


typedef struct kitty_sbuf_s {
    char   *s;
    size_t  n;
    size_t  a;
} KITTY_SBUF;


static KITTY_THUMB_S *k_cache = NULL;
static int	      k_cache_n = 0;
static size_t	      k_cache_bytes = 0;
static unsigned long  k_stamp = 0;

static KITTY_IMG_S   *k_imgs = NULL;
static int	      k_n = 0;
static int	      k_alloc = 0;
static unsigned	      k_next_id = KITTY_ID_BASE;

static int	      k_gfx = GFX_UNKNOWN;
static int	      k_cell_w = 8;
static int	      k_cell_h = 16;
static int	      k_cell_valid = 0;

static int	      k_max_cols = KITTY_DEF_COLS;
static int	      k_max_rows = KITTY_DEF_ROWS;
static int	      k_max_count = KITTY_DEF_COUNT;


/* internal prototypes */
static void	 kitty_read_limits(void);
static int	 kitty_detect_gfx(void);
static int	 kitty_env_kitty(void);
static int	 kitty_env_iterm(void);
static int	 kitty_env_sixel(void);
static int	 kitty_wait_input(int);
static int	 kitty_query_read(const char *, char *, int, int);
static int	 kitty_query_kitty(void);
static int	 kitty_query_sixel(void);
static int	 kitty_query_cell_csi(int *, int *);
static void	 kitty_cell_pixels(int *, int *);
static void	 kitty_fit(int, int, int, int, int *, int *, int *, int *);
static KITTY_THUMB_S *kitty_thumb_make(const char *, const unsigned char *, size_t);
static KITTY_THUMB_S *kitty_cache_find(const char *);
static void	 kitty_cache_add(KITTY_THUMB_S *);
static void	 kitty_cache_evict(KITTY_THUMB_S *);
static int	 kitty_thumb_in_use(KITTY_THUMB_S *);
static void	 kitty_thumb_free(KITTY_THUMB_S *);
static int	 kitty_thumb_png(KITTY_THUMB_S *);
static int	 kitty_register(KITTY_THUMB_S *);
static KITTY_IMG_S *kitty_find(unsigned);
static int	 kitty_extract_part_mem(ATTACH_S *, long, unsigned char **, size_t *);
static int	 kitty_decode_data_uri(char *, unsigned char **, size_t *);
static char	*kitty_key_part(ATTACH_S *, long, int, int);
static char	*kitty_key_data(char *, int, int);
static long	 kitty_current_rawno(void);
static void	 kitty_reset_shown(void);
static int	 kitty_handle_on_line(char *, int, HANDLE_S *, HANDLE_S **);
static int	 kitty_line_indent(char *, int);
static void	 kitty_paint_one(KITTY_IMG_S *, HANDLE_S *, int, int, int, int, int);
static void	 kitty_transmit(KITTY_IMG_S *);
static void	 kitty_delete_image(unsigned);
static void	 kitty_delete_placement(unsigned, int);
static void	 kitty_write_apc(const char *, const char *, size_t);
static void	 kitty_emit(const char *, size_t, int);
static void	 kitty_goto(int, int);
static char	*kitty_sixel_encode(KITTY_THUMB_S *, int, int);
static char	*kitty_b64_bin(const unsigned char *, size_t, size_t *);
static unsigned char *kitty_b64_dec(const char *, size_t, size_t *);
static void	 kitty_sb_put(KITTY_SBUF *, const char *, size_t);
static void	 kitty_sb_puts(KITTY_SBUF *, const char *);
static void	 kitty_sb_putc(KITTY_SBUF *, int);
static void	 kitty_sixel_run(KITTY_SBUF *, int, int);
static int	 kitty_in_tmux(void);


/*----------------------------------------------------------------------
   Exported interface
  ----*/

int
kitty_img_enabled(void)
{
    if(!ps_global || F_OFF(F_KITTY_INLINE_IMAGES, ps_global))
      return(0);

    return(kitty_detect_gfx() != GFX_NONE);
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
    kitty_read_limits();
}


/*
 * Forget the images of the message being viewed: free them in the
 * terminal and drop the per-message list.  Thumbnails stay cached.
 */
void
kitty_img_end_message(void)
{
    int i;

    if(k_gfx == GFX_KITTY){
	for(i = 0; i < k_n; i++)
	  if(k_imgs[i].transmitted)
	    kitty_delete_image(k_imgs[i].id);

	if(k_n > 0)
	  fflush(stdout);
    }

    if(k_imgs)
      fs_give((void **) &k_imgs);

    k_imgs = NULL;
    k_n = 0;
    k_alloc = 0;
}


/*
 * Called once at exit: release the terminal's copies and the cache.
 */
void
kitty_img_shutdown(void)
{
    KITTY_THUMB_S *t, *next;

    kitty_img_end_message();

    for(t = k_cache; t; t = next){
	next = t->next;
	kitty_thumb_free(t);
    }

    k_cache = NULL;
    k_cache_n = 0;
    k_cache_bytes = 0;
    remote_image_clear();
}


void
kitty_img_note_resize(void)
{
    k_cell_valid = 0;
}


int
kitty_img_from_part(ATTACH_S *a, long int msgno, int *rowsp, int *colsp)
{
    KITTY_THUMB_S *t;
    unsigned char *data = NULL;
    size_t	   len = 0;
    char	  *key;
    int		   cw, ch, id;

    if(!(a && a->body && a->number && rowsp && colsp))
      return(0);

    if(a->body->type != TYPEIMAGE || !kitty_img_enabled())
      return(0);

    if(k_max_count > 0 && k_n >= k_max_count)
      return(0);

    if(a->body->size.bytes > (unsigned long) KITTY_MAX_BYTES){
	dprint((5, "kittyimg: skip oversized part %s (%lu bytes)\n",
		   a->number, (unsigned long) a->body->size.bytes));
	return(0);
    }

    if(msgno <= 0)
      msgno = kitty_current_rawno();

    if(msgno <= 0)
      return(0);

    kitty_cell_pixels(&cw, &ch);
    key = kitty_key_part(a, msgno, cw, ch);
    if(!key)
      return(0);

    t = kitty_cache_find(key);
    if(!t){
	if(!kitty_extract_part_mem(a, msgno, &data, &len)){
	    fs_give((void **) &key);
	    return(0);
	}

	t = kitty_thumb_make(key, data, len);
	fs_give((void **) &data);
    }

    fs_give((void **) &key);
    if(!t)
      return(0);

    id = kitty_register(t);
    if(id <= 0)
      return(0);

    *rowsp = t->rows;
    *colsp = t->cols;
    a->kitty_shown = 1;
    return(id);
}


ATTACH_S *
kitty_img_attach_for_cid(char *cid)
{
    ATTACH_S *a;
    char      wrapped[MAILTMPLEN];
    int	      i;

    if(!(cid && *cid && ps_global && ps_global->atmts))
      return(NULL);

    if(cid[0] != '<'){
	snprintf(wrapped, sizeof(wrapped), "<%s>", cid);
	wrapped[sizeof(wrapped)-1] = '\0';
	cid = wrapped;
    }

    for(i = 0; ps_global->atmts[i].description != NULL; i++){
	a = &ps_global->atmts[i];
	if(a->body && a->body->type == TYPEIMAGE
	   && a->body->id && !strcmp(a->body->id, cid))
	  return(a);
    }

    return(NULL);
}


int
kitty_img_from_cid(char *cid, int *rowsp, int *colsp)
{
    ATTACH_S *a;

    if(!(cid && *cid && rowsp && colsp))
      return(0);

    a = kitty_img_attach_for_cid(cid);
    if(!a)
      return(0);

    return(kitty_img_from_part(a, kitty_current_rawno(), rowsp, colsp));
}


int
kitty_img_from_data_uri(char *src, int *rowsp, int *colsp)
{
    KITTY_THUMB_S *t;
    unsigned char *data = NULL;
    size_t	   len = 0;
    char	  *key;
    int		   cw, ch, id;

    if(!(src && rowsp && colsp) || !kitty_img_enabled())
      return(0);

    if(k_max_count > 0 && k_n >= k_max_count)
      return(0);

    if(strlen(src) > (size_t) KITTY_MAX_BYTES * 4 / 3 + 64)
      return(0);

    kitty_cell_pixels(&cw, &ch);
    key = kitty_key_data(src, cw, ch);
    if(!key)
      return(0);

    t = kitty_cache_find(key);
    if(!t){
	if(!kitty_decode_data_uri(src, &data, &len)){
	    fs_give((void **) &key);
	    return(0);
	}

	t = kitty_thumb_make(key, data, len);
	fs_give((void **) &data);
    }

    fs_give((void **) &key);
    if(!t)
      return(0);

    id = kitty_register(t);
    if(id <= 0)
      return(0);

    *rowsp = t->rows;
    *colsp = t->cols;
    return(id);
}


/* A cache-only lookup during formatting; downloads happen on activation. */
int
kitty_img_from_url(char *url, int fetch, int *rowsp, int *colsp, const char **error)
{
    KITTY_THUMB_S *t;
    const unsigned char *data;
    size_t len;
    char *key;
    int cw, ch, id, i;

    if(error)
      *error = NULL;
    if(!rowsp || !colsp || !remote_image_url(url))
      return(0);
    *rowsp = *colsp = 0;
    if(!kitty_img_enabled()){
	if(error)
	  *error = _("This terminal does not support inline images");
	return(0);
    }

    kitty_cell_pixels(&cw, &ch);
    /* Original and preview thumbnails must never mask each other. */
    if(fetch == REMOTE_IMAGE_ORIGINAL){
        char *original_key = malloc(strlen(url) + sizeof("original:"));
        if(!original_key){ if(error) *error = _("Could not allocate image key"); return 0; }
        sprintf(original_key,"original:%s",url);
        key = kitty_key_data(original_key,cw,ch);
        free(original_key);
    }
    else key = kitty_key_data(url, cw, ch);
    t = kitty_cache_find(key);
    if(k_max_count > 0 && k_n >= k_max_count){
	for(i = 0; t && i < k_n && k_imgs[i].thumb != t; i++)
	  ;
	if(!t || i == k_n){
	    fs_give((void **) &key);
	    if(error)
	      *error = _("Inline image limit reached; increase inline-image-max-count");
	    return(0);
	}
    }
    if(!t){
	data = remote_image_data(url, &len, fetch, error);
	if(data){
	    t = kitty_thumb_make(key, data, len);
	    if(!t && error)
	      *error = _("Image cannot be displayed (unsupported format or dimensions)");
	}
    }
    fs_give((void **) &key);
    if(!t)
      return(0);

    id = kitty_register(t);
    if(id > 0){
	*rowsp = t->rows;
	*colsp = t->cols;
    }
    return(id);
}


/*
 * Build the "[Image: ...]" text for a handle line, truncated on a
 * UTF-8 character boundary.
 */
char *
kitty_img_alt(char *alt, char *dst, size_t dstlen)
{
    size_t n, i;

    if(!dst || dstlen < 12)
      return(dst);

    if(!alt || !*alt)
      alt = "image";

    n = strlen(alt);
    if(n > KITTY_ALT_MAX){
	n = KITTY_ALT_MAX;
	while(n > 0 && (((unsigned char) alt[n]) & 0xC0) == 0x80)
	  n--;
    }

    if(n + 10 >= dstlen)
      n = dstlen - 11;

    strncpy(dst, "[Image: ", dstlen);
    i = strlen(dst);
    memcpy(dst + i, alt, n);
    dst[i + n] = ']';
    dst[i + n + 1] = '\0';
    return(dst);
}


/*
 * Write the handle line and the reserved blank rows for an image part
 * into the formatted message text.
 */
int
kitty_img_emit_block(gf_o_t pc, HANDLE_S **handlesp, int kid,
		     int rows, int cols, ATTACH_S *a, char *alt)
{
    HANDLE_S *h;
    char      buf[32], text[128];
    int	      i;

    if(!(pc && handlesp && kid > 0 && rows > 0))
      return(0);

    h = new_handle(handlesp);
    if(!h)
      return(0);

    h->type	      = IMG;
    h->h.img.alt      = cpystr(alt ? alt : "Image");
    h->h.img.attach   = a;
    h->h.img.kitty_id = kid;
    h->h.img.rows     = rows;
    h->h.img.cols     = cols > 0 ? cols : 1;

    snprintf(buf, sizeof(buf), "%d", h->key);
    buf[sizeof(buf)-1] = '\0';
    kitty_img_alt(alt, text, sizeof(text));

    if(!((*pc)(TAG_EMBED) && (*pc)(TAG_HANDLE)
	 && (*pc)((int) strlen(buf)) && gf_puts(buf, pc)
	 && gf_puts(text, pc)
	 && (*pc)(TAG_EMBED) && (*pc)(TAG_HANDLEOFF)
	 && gf_puts(NEWLINE, pc)))
      return(0);

    for(i = 0; i < rows; i++)
      if(!gf_puts(NEWLINE, pc))
	return(0);

    return(1);
}


/*
 * Place every thumbnail whose reserved rows intersect the window.
 *
 *   lines/lens  the formatted text, indexed from 0
 *   offset      index of the line shown on the window's first row
 *   start_row   screen row of the window's first line
 *   nlines      rows in the window
 *   num_lines   valid entries in lines (0 if unknown)
 *   handles     the message's handle list
 */
void
kitty_img_paint_lines(char **lines, short *lens, long int offset,
		      int start_row, int nlines, long int num_lines,
		      HANDLE_S *handles)
{
    HANDLE_S *h;
    int	      i, gfx, lookback = 0, scols;

    if(!kitty_img_enabled() || k_n <= 0 || !lines || nlines <= 0 || !handles)
      return;

    gfx = kitty_detect_gfx();
    for(h = handles; h; h = h->next)
      if((h->type == IMG || h->type == imgData) && h->h.img.kitty_id > 0){
	  h->h.img.seen = 0;
	  if(h->h.img.rows > lookback)
	    lookback = h->h.img.rows;
      }

    scols = (ps_global && ps_global->ttyo) ? ps_global->ttyo->screen_cols : 80;

    for(i = -lookback; i < nlines; i++){
	KITTY_IMG_S *im;
	char	    *line;
	long	     l = offset + i;
	int	     len, top, cut, first, vis, col;

	if(l < 0 || (num_lines > 0 && l >= num_lines))
	  continue;

	line = lines[l];
	len  = lens ? lens[l] : (line ? (int) strlen(line) : 0);
	if(!line || len <= 0)
	  continue;

	if(!kitty_handle_on_line(line, len, handles, &h) || !h)
	  continue;

	im = kitty_find((unsigned) h->h.img.kitty_id);
	if(!im || !im->thumb)
	  continue;

	top   = i + 1;				/* first reserved row */
	cut   = (top < 0) ? -top : 0;		/* rows above the window */
	first = (top < 0) ? 0 : top;
	vis   = h->h.img.rows - cut;
	if(vis > nlines - first)
	  vis = nlines - first;

	if(cut >= h->h.img.rows || vis <= 0)
	  continue;

	col = kitty_line_indent(line, len);
	if(col + im->cols > scols)
	  col = (scols > im->cols) ? scols - im->cols : 0;

	kitty_paint_one(im, h, start_row + first, col, cut, vis, gfx);
	h->h.img.seen = 1;
    }

    if(gfx == GFX_KITTY)
      for(h = handles; h; h = h->next)
	if((h->type == IMG || h->type == imgData)
	   && h->h.img.kitty_id > 0 && h->h.img.placed && !h->h.img.seen){
	    kitty_delete_placement((unsigned) h->h.img.kitty_id, h->key);
	    h->h.img.placed = 0;
	}

    fflush(stdout);
}


/*----------------------------------------------------------------------
   Configuration and terminal detection
  ----*/

static void
kitty_read_limits(void)
{
    char *s;
    int	  c = 0, r = 0, n;

    k_max_cols = KITTY_DEF_COLS;
    k_max_rows = KITTY_DEF_ROWS;
    k_max_count = KITTY_DEF_COUNT;

    if(!ps_global)
      return;

    s = ps_global->VAR_INLINE_IMAGE_SIZE;
    if(s && sscanf(s, "%dx%d", &c, &r) == 2
       && c >= 4 && c <= 400 && r >= 1 && r <= 100){
	k_max_cols = c;
	k_max_rows = r;
    }

    s = ps_global->VAR_INLINE_IMAGE_COUNT;
    if(s && *s){
	n = atoi(s);
	if(n >= 0 && n <= 500)
	  k_max_count = n;
    }
}


static int
kitty_in_tmux(void)
{
    return(getenv("TMUX") != NULL);
}


static int
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
	   || !strucmp(s, "ghostty")
	   || !strucmp(s, "inkline")))
      return(1);

    if(getenv("GHOSTTY_RESOURCES_DIR") || getenv("WEZTERM_EXECUTABLE"))
      return(1);

    return(0);
}


static int
kitty_env_iterm(void)
{
    char *s;

    if((s = getenv("TERM_PROGRAM")) != NULL && !strucmp(s, "iTerm.app"))
      return(1);

    if((s = getenv("LC_TERMINAL")) != NULL && !strucmp(s, "iTerm2"))
      return(1);

    return(0);
}


static int
kitty_env_sixel(void)
{
    char *s;

    if((s = getenv("TERM")) != NULL
       && (strstr(s, "sixel") != NULL
	   || !struncmp(s, "mlterm", 6)
	   || !struncmp(s, "foot", 4)
	   || strstr(s, "mintty") != NULL))
      return(1);

    return(0);
}


/*
 * Wait for input on the tty, retrying when a signal (SIGWINCH during
 * a resize, typically) interrupts the wait.
 */
static int
kitty_wait_input(int ms)
{
    struct timeval tv;
    fd_set	   fds;
    int		   r, tries = 0;

    for(;;){
	tv.tv_sec  = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	FD_ZERO(&fds);
	FD_SET(STDIN_FILENO, &fds);
	r = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
	if(r < 0 && errno == EINTR && ++tries < 8)
	  continue;

	return(r > 0);
    }
}


static int
kitty_query_read(const char *query, char *buf, int buflen, int timeout_ms)
{
    int n, got = 0, oldfl;

    if(!query || !buf || buflen < 8)
      return(0);

    if(!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
      return(0);

    fputs(query, stdout);
    fflush(stdout);

    if(!kitty_wait_input(timeout_ms))
      return(0);

    oldfl = fcntl(STDIN_FILENO, F_GETFL, 0);
    if(oldfl >= 0)
      (void) fcntl(STDIN_FILENO, F_SETFL, oldfl | O_NONBLOCK);

    while(got < buflen - 1){
	n = (int) read(STDIN_FILENO, buf + got, (size_t) (buflen - 1 - got));
	if(n > 0){
	    got += n;
	    if(!kitty_wait_input(40))
	      break;
	}
	else if(n < 0 && errno == EINTR)
	  continue;
	else
	  break;
    }

    if(oldfl >= 0)
      (void) fcntl(STDIN_FILENO, F_SETFL, oldfl);

    buf[got] = '\0';
    return(got);
}


static int
kitty_query_kitty(void)
{
    char buf[256];
    int  n;

    if(kitty_in_tmux())
      n = kitty_query_read("\033Ptmux;\033\033_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\033\033\\\033\\",
			   buf, sizeof(buf), 400);
    else
      n = kitty_query_read("\033_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\033\\",
			   buf, sizeof(buf), 400);

    if(n <= 0)
      return(0);

    return(strstr(buf, "_Gi=31") != NULL);
}


static int
kitty_query_sixel(void)
{
    char *p, buf[256];
    int   n;

    n = kitty_query_read("\033[c", buf, sizeof(buf), 400);
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


static int
kitty_query_cell_csi(int *cwp, int *chp)
{
    char buf[128], *p;
    int  n, w = 0, h = 0;

    n = kitty_query_read("\033[16t", buf, sizeof(buf), 250);
    if(n <= 0)
      return(0);

    p = strstr(buf, "[6;");
    if(!p || sscanf(p, "[6;%d;%dt", &h, &w) != 2 || w <= 0 || h <= 0)
      return(0);

    *cwp = w;
    *chp = h;
    return(1);
}


static int
kitty_detect_gfx(void)
{
    if(k_gfx != GFX_UNKNOWN)
      return(k_gfx);

    k_gfx = GFX_NONE;

    if(kitty_env_kitty())
      k_gfx = GFX_KITTY;
    else if(kitty_env_iterm())
      k_gfx = GFX_ITERM;
    else if(kitty_env_sixel())
      k_gfx = GFX_SIXEL;
    else if(kitty_query_kitty())
      k_gfx = GFX_KITTY;
    else if(kitty_query_sixel())
      k_gfx = GFX_SIXEL;

    dprint((5, "kittyimg: graphics mode %d\n", k_gfx));
    return(k_gfx);
}


static void
kitty_cell_pixels(int *cwp, int *chp)
{
    if(!k_cell_valid){
	int w = 0, h = 0;

#ifdef	TIOCGWINSZ
	{
	    struct winsize ws;

	    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0
	       && ws.ws_col > 0 && ws.ws_row > 0
	       && ws.ws_xpixel > 0 && ws.ws_ypixel > 0){
		w = ws.ws_xpixel / ws.ws_col;
		h = ws.ws_ypixel / ws.ws_row;
	    }
	}
#endif

	if(w <= 0 || h <= 0)
	  (void) kitty_query_cell_csi(&w, &h);

	k_cell_w = (w > 0) ? w : 8;
	k_cell_h = (h > 0) ? h : 16;
	k_cell_valid = 1;
    }

    *cwp = k_cell_w;
    *chp = k_cell_h;
}


/*
 * Fit an image into the configured cell box without ever enlarging it.
 */
static void
kitty_fit(int w, int h, int cw, int ch, int *twp, int *thp, int *colsp, int *rowsp)
{
    double s, sw, sh;
    int	   tw, th, cols, rows;

    sw = (double) (k_max_cols * cw) / (double) w;
    sh = (double) (k_max_rows * ch) / (double) h;
    s  = (sw < sh) ? sw : sh;
    if(s > 1.0)
      s = 1.0;

    tw = (int) ((double) w * s + 0.5);
    th = (int) ((double) h * s + 0.5);
    if(tw < 1)
      tw = 1;

    if(th < 1)
      th = 1;

    cols = (tw + cw - 1) / cw;
    rows = (th + ch - 1) / ch;
    if(cols > k_max_cols)
      cols = k_max_cols;

    if(rows > k_max_rows)
      rows = k_max_rows;

    if(tw > cols * cw)
      tw = cols * cw;

    if(th > rows * ch)
      th = rows * ch;

    *twp = tw;
    *thp = th;
    *colsp = cols;
    *rowsp = rows;
}


/*----------------------------------------------------------------------
   Thumbnails and their cache
  ----*/

static KITTY_THUMB_S *
kitty_thumb_make(const char *key, const unsigned char *data, size_t len)
{
    KITTY_THUMB_S *t;
    unsigned char *pix, *small = NULL, *canvas;
    int		   w = 0, h = 0, comp = 0, cw, ch, tw, th, cols, rows, y;

    if(!key || !data || len == 0)
      return(NULL);

    if(WebPGetInfo(data,len,&w,&h)){
        size_t bytes;
        if(w <= 0 || h <= 0 || w > INT_MAX / 4 || (size_t)h > SIZE_MAX / ((size_t)w * 4)) return NULL;
        bytes = (size_t)w * (size_t)h * 4;
        pix = (unsigned char *)malloc(bytes);
        if(pix && !WebPDecodeRGBAInto(data,len,pix,bytes,w*4)){ free(pix); return NULL; }
    }
    else{
        /* stb's memory decoder takes an int length, not size_t. */
        if(len > INT_MAX) return NULL;
        pix = stbi_load_from_memory(data,(int)len,&w,&h,&comp,4);
    }
    if(!pix)
      return(NULL);

    kitty_cell_pixels(&cw, &ch);
    kitty_fit(w, h, cw, ch, &tw, &th, &cols, &rows);

    if(tw != w || th != h){
	small = (unsigned char *) malloc((size_t) tw * (size_t) th * 4);
	if(!small
	   || !stbir_resize_uint8_srgb(pix, w, h, 0, small, tw, th, 0, STBIR_RGBA)){
	    if(small)
	      free(small);

	    stbi_image_free(pix);
	    return(NULL);
	}

	stbi_image_free(pix);
	pix = small;
    }

    t = (KITTY_THUMB_S *) fs_get(sizeof(KITTY_THUMB_S));
    memset(t, 0, sizeof(KITTY_THUMB_S));
    t->key  = cpystr(key);
    t->w    = cols * cw;
    t->h    = rows * ch;
    t->iw   = tw;
    t->ih   = th;
    t->rows = rows;
    t->cols = cols;
    canvas  = (unsigned char *) fs_get((size_t) t->w * (size_t) t->h * 4);
    memset(canvas, 0, (size_t) t->w * (size_t) t->h * 4);
    for(y = 0; y < th; y++)
      memcpy(canvas + (size_t) y * (size_t) t->w * 4,
	     pix + (size_t) y * (size_t) tw * 4, (size_t) tw * 4);

    if(small)
      free(small);
    else
      stbi_image_free(pix);

    t->rgba  = canvas;
    t->bytes = (size_t) t->w * (size_t) t->h * 4 + strlen(key) + sizeof(*t);
    kitty_cache_add(t);
    return(t);
}


static KITTY_THUMB_S *
kitty_cache_find(const char *key)
{
    KITTY_THUMB_S *t;

    for(t = k_cache; t; t = t->next)
      if(!strcmp(t->key, key)){
	  t->stamp = ++k_stamp;
	  return(t);
      }

    return(NULL);
}


static void
kitty_cache_add(KITTY_THUMB_S *t)
{
    t->stamp = ++k_stamp;
    t->next  = k_cache;
    k_cache  = t;
    k_cache_n++;
    k_cache_bytes += t->bytes;
    kitty_cache_evict(t);
}


static int
kitty_thumb_in_use(KITTY_THUMB_S *t)
{
    int i;

    for(i = 0; i < k_n; i++)
      if(k_imgs[i].thumb == t)
	return(1);

    return(0);
}


static void
kitty_cache_evict(KITTY_THUMB_S *keep)
{
    while(k_cache_n > KITTY_CACHE_MAX_ENTRIES
	  || k_cache_bytes > (size_t) KITTY_CACHE_MAX_BYTES){
	KITTY_THUMB_S *t, **pp, **oldest = NULL;

	for(pp = &k_cache; (t = *pp) != NULL; pp = &t->next)
	  if(t != keep && !kitty_thumb_in_use(t)
	     && (!oldest || t->stamp < (*oldest)->stamp))
	    oldest = pp;

	if(!oldest)
	  break;

	t = *oldest;
	*oldest = t->next;
	kitty_thumb_free(t);
	k_cache_n--;
    }
}


static void
kitty_thumb_free(KITTY_THUMB_S *t)
{
    if(!t)
      return;

    if(k_cache_bytes >= t->bytes)
      k_cache_bytes -= t->bytes;
    else
      k_cache_bytes = 0;

    if(t->key)
      fs_give((void **) &t->key);

    if(t->rgba)
      fs_give((void **) &t->rgba);

    if(t->png)
      free(t->png);

    fs_give((void **) &t);
}


static int
kitty_thumb_png(KITTY_THUMB_S *t)
{
    int len = 0;

    if(!t)
      return(0);

    if(t->png)
      return(1);

    t->png = stbi_write_png_to_mem(t->rgba, t->w * 4, t->w, t->h, 4, &len);
    if(!t->png || len <= 0){
	if(t->png)
	  free(t->png);

	t->png = NULL;
	return(0);
    }

    t->pnglen = (size_t) len;
    t->bytes += t->pnglen;
    k_cache_bytes += t->pnglen;
    return(1);
}


/*
 * Add a thumbnail to the message's image list; the same thumbnail
 * referenced twice gets the same id.
 */
static int
kitty_register(KITTY_THUMB_S *t)
{
    KITTY_IMG_S *im;
    int		 i;

    if(!t)
      return(0);

    for(i = 0; i < k_n; i++)
      if(k_imgs[i].thumb == t)
	return((int) k_imgs[i].id);

    if(k_n >= k_alloc){
	k_alloc = k_alloc ? k_alloc * 2 : 8;
	if(k_imgs)
	  fs_resize((void **) &k_imgs, k_alloc * sizeof(KITTY_IMG_S));
	else
	  k_imgs = (KITTY_IMG_S *) fs_get(k_alloc * sizeof(KITTY_IMG_S));
    }

    if(k_next_id >= KITTY_ID_LIMIT)
      k_next_id = KITTY_ID_BASE;

    im = &k_imgs[k_n++];
    memset(im, 0, sizeof(KITTY_IMG_S));
    im->id    = k_next_id++;
    im->thumb = t;
    im->rows  = t->rows;
    im->cols  = t->cols;
    return((int) im->id);
}


static KITTY_IMG_S *
kitty_find(unsigned int id)
{
    int i;

    for(i = 0; i < k_n; i++)
      if(k_imgs[i].id == id)
	return(&k_imgs[i]);

    return(NULL);
}


/*----------------------------------------------------------------------
   Getting the image bytes
  ----*/

static long
kitty_current_rawno(void)
{
    if(!(ps_global && ps_global->msgmap && ps_global->mail_stream))
      return(0L);

    return(mn_m2raw(ps_global->msgmap, mn_get_cur(ps_global->msgmap)));
}


static void
kitty_reset_shown(void)
{
    ATTACH_S *a;

    if(!(ps_global && ps_global->atmts))
      return;

    for(a = ps_global->atmts; a->description != NULL; a++)
      if(a->kitty_shown){
	  a->kitty_shown = 0;
	  a->shown = 0;
      }
}


static char *
kitty_key_part(ATTACH_S *a, long int msgno, int cw, int ch)
{
    char	  buf[MAILTMPLEN];
    unsigned long uid = 0;
    char	 *mbx = "?";

    if(ps_global && ps_global->mail_stream){
	uid = mail_uid(ps_global->mail_stream, msgno);
	if(ps_global->mail_stream->mailbox)
	  mbx = ps_global->mail_stream->mailbox;
    }

    snprintf(buf, sizeof(buf), "part|%s|%lu|%ld|%s|%dx%d",
	     mbx, uid, msgno, a->number, cw, ch);
    buf[sizeof(buf)-1] = '\0';
    return(cpystr(buf));
}


static char *
kitty_key_data(char *src, int cw, int ch)
{
    char		 buf[128];
    unsigned long long	 hash = 1469598103934665603ULL;
    unsigned char	*p;

    for(p = (unsigned char *) src; *p; p++){
	hash ^= *p;
	hash *= 1099511628211ULL;
    }

    snprintf(buf, sizeof(buf), "data|%016llx|%lu|%dx%d",
	     hash, (unsigned long) strlen(src), cw, ch);
    buf[sizeof(buf)-1] = '\0';
    return(cpystr(buf));
}


static int
kitty_extract_part_mem(ATTACH_S *a, long int msgno,
		       unsigned char **bufp, size_t *lenp)
{
    STORE_S *so;
    gf_o_t   pc;
    char    *err;
    size_t   len;

    *bufp = NULL;
    *lenp = 0;
    if(!(a && a->number && ps_global && ps_global->mail_stream))
      return(0);

    so = so_get(CharStar, NULL, EDIT_ACCESS);
    if(!so)
      return(0);

    gf_set_so_writec(&pc, so);
    err = detach(ps_global->mail_stream, msgno, a->number, 0L, NULL, pc,
		 NULL, DT_BINARY);
    gf_clear_so_writec(so);

    len = so->txt ? (size_t) ((unsigned char *) so->eod - (unsigned char *) so->txt) : 0;
    if(err || len == 0 || len > (size_t) KITTY_MAX_BYTES){
	dprint((5, "kittyimg: extract of %s failed: %s\n",
		   a->number, err ? err : "size"));
	so_give(&so);
	return(0);
    }

    *bufp = (unsigned char *) fs_get(len);
    memcpy(*bufp, so->txt, len);
    *lenp = len;
    so_give(&so);
    return(1);
}


static int
kitty_decode_data_uri(char *src, unsigned char **bufp, size_t *lenp)
{
    char   *comma, *payload, *b64;
    size_t  n;

    *bufp = NULL;
    *lenp = 0;
    if(!src)
      return(0);

    if(!struncmp(src, "data:", 5))
      src += 5;

    comma = strchr(src, ',');
    if(!comma || !comma[1])
      return(0);

    payload = comma + 1;
    n = strlen(payload);

    b64 = srchstr(src, "base64");
    if(b64 && b64 < comma){
	*bufp = kitty_b64_dec(payload, n, lenp);
    }
    else{
	/* percent-encoded raw bytes */
	unsigned char *d;
	size_t	       i, o = 0;

	d = (unsigned char *) fs_get(n + 1);
	for(i = 0; i < n; i++){
	    if(payload[i] == '%' && i + 2 < n
	       && isxdigit((unsigned char) payload[i+1])
	       && isxdigit((unsigned char) payload[i+2])){
		unsigned v = 0;

		sscanf(payload + i + 1, "%2x", &v);
		d[o++] = (unsigned char) v;
		i += 2;
	    }
	    else
	      d[o++] = (unsigned char) payload[i];
	}

	*bufp = d;
	*lenp = o;
    }

    if(!*bufp || *lenp == 0 || *lenp > (size_t) KITTY_MAX_BYTES){
	if(*bufp)
	  fs_give((void **) bufp);

	*lenp = 0;
	return(0);
    }

    return(1);
}


/*----------------------------------------------------------------------
   Finding image handles in formatted lines
  ----*/

static int
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


/*
 * Column of the first visible character on the handle line, so the
 * thumbnail lines up with the view margin or a quote prefix.
 */
static int
kitty_line_indent(char *line, int length)
{
    int col = 0;

    while(length > 0){
	unsigned char c = (unsigned char) *line;

	if(c == (unsigned char) TAG_EMBED && length > 1){
	    int n;

	    switch(line[1]){
	      case TAG_HANDLE :
		if(length < 3)
		  return(col);

		n = (unsigned char) line[2];
		if(n > length - 3)
		  return(col);

		line += 3 + n;
		length -= 3 + n;
		break;

	      case TAG_FGCOLOR :
	      case TAG_BGCOLOR :
		if(length < 2 + RGBLEN)
		  return(col);

		line += 2 + RGBLEN;
		length -= 2 + RGBLEN;
		break;

	      default :
		line += 2;
		length -= 2;
		break;
	    }

	    continue;
	}

	if(c == ' ' || c == '>'){
	    col++;
	    line++;
	    length--;
	    continue;
	}

	break;
    }

    return(col);
}


/*----------------------------------------------------------------------
   Terminal output
  ----*/

/*
 * Write bytes to the terminal, wrapped in a tmux passthrough (with
 * ESC doubled) when asked and we are inside tmux.
 */
static void
kitty_emit(const char *s, size_t n, int passthrough)
{
    if(!s || n == 0)
      return;

    if(passthrough && kitty_in_tmux()){
	fputs("\033Ptmux;", stdout);
	for(; n > 0; n--, s++){
	    if(*s == '\033')
	      fputc('\033', stdout);

	    fputc(*s, stdout);
	}

	fputs("\033\\", stdout);
    }
    else
      fwrite(s, 1, n, stdout);
}


static void
kitty_goto(int row, int col)
{
    fprintf(stdout, "\033[%d;%dH", row + 1, col + 1);
}


static void
kitty_write_apc(const char *ctrl, const char *payload, size_t paylen)
{
    KITTY_SBUF b;

    if(!ctrl)
      return;

    memset(&b, 0, sizeof(b));
    kitty_sb_puts(&b, "\033_G");
    kitty_sb_puts(&b, ctrl);
    if(payload && paylen){
	kitty_sb_putc(&b, ';');
	kitty_sb_put(&b, payload, paylen);
    }

    kitty_sb_puts(&b, "\033\\");
    kitty_emit(b.s, b.n, 1);
    fs_give((void **) &b.s);
}


static void
kitty_transmit(KITTY_IMG_S *im)
{
    char   *b64, ctrl[96];
    size_t  b64len = 0, off, n;

    if(!im || im->transmitted || !im->thumb)
      return;

    if(!kitty_thumb_png(im->thumb))
      return;

    b64 = kitty_b64_bin(im->thumb->png, im->thumb->pnglen, &b64len);
    if(!b64)
      return;

    for(off = 0; off < b64len; off += KITTY_CHUNK){
	int more;

	n = b64len - off;
	if(n > KITTY_CHUNK)
	  n = KITTY_CHUNK;

	more = (off + n < b64len) ? 1 : 0;
	if(off == 0)
	  snprintf(ctrl, sizeof(ctrl), "a=t,f=100,t=d,i=%u,q=2,m=%d",
		   im->id, more);
	else
	  snprintf(ctrl, sizeof(ctrl), "m=%d,q=2", more);

	kitty_write_apc(ctrl, b64 + off, n);
    }

    fs_give((void **) &b64);
    im->transmitted = 1;
}


static void
kitty_delete_image(unsigned int id)
{
    char ctrl[64];

    snprintf(ctrl, sizeof(ctrl), "a=d,d=I,i=%u,q=2", id);	/* frees data */
    kitty_write_apc(ctrl, NULL, 0);
}


static void
kitty_delete_placement(unsigned int id, int pid)
{
    char ctrl[64];

    snprintf(ctrl, sizeof(ctrl), "a=d,d=i,i=%u,p=%d,q=2", id, pid);
    kitty_write_apc(ctrl, NULL, 0);
}


/*
 * Draw rows [cut, cut+vis) of the thumbnail at screen row/col.
 */
static void
kitty_paint_one(KITTY_IMG_S *im, HANDLE_S *h, int row, int col,
		int cut, int vis, int gfx)
{
    KITTY_THUMB_S *t = im->thumb;
    int		   rowpx, y0, hpx;

    if(!t || t->rows <= 0)
      return;

    rowpx = t->h / t->rows;
    y0    = cut * rowpx;
    hpx   = vis * rowpx;
    if(y0 >= t->h || hpx <= 0)
      return;

    if(y0 + hpx > t->h)
      hpx = t->h - y0;

    if(gfx == GFX_KITTY){
	char ctrl[160];

	kitty_transmit(im);
	if(!im->transmitted)
	  return;

	kitty_goto(row, col);
	if(cut == 0 && vis == t->rows)
	  snprintf(ctrl, sizeof(ctrl), "a=p,i=%u,p=%d,c=%d,r=%d,C=1,q=2",
		   im->id, h->key, t->cols, vis);
	else
	  snprintf(ctrl, sizeof(ctrl),
		   "a=p,i=%u,p=%d,c=%d,r=%d,x=0,y=%d,w=%d,h=%d,C=1,q=2",
		   im->id, h->key, t->cols, vis, y0, t->w, hpx);

	kitty_write_apc(ctrl, NULL, 0);
	h->h.img.placed = 1;
    }
    else if(gfx == GFX_ITERM){
	unsigned char *png;
	char	      *b64, hdr[160];
	int	       len = 0;
	size_t	       b64len = 0;

	png = stbi_write_png_to_mem(t->rgba + (size_t) y0 * (size_t) t->w * 4,
				    t->w * 4, t->w, hpx, 4, &len);
	if(!png || len <= 0){
	    if(png)
	      free(png);

	    return;
	}

	b64 = kitty_b64_bin(png, (size_t) len, &b64len);
	free(png);
	if(!b64)
	  return;

	fputs("\0337", stdout);
	kitty_goto(row, col);
	snprintf(hdr, sizeof(hdr),
		 "\033]1337;File=inline=1;size=%d;width=%d;height=%d;preserveAspectRatio=0:",
		 len, t->cols, vis);
	if(kitty_in_tmux()){
	    KITTY_SBUF b;

	    memset(&b, 0, sizeof(b));
	    kitty_sb_puts(&b, hdr);
	    kitty_sb_put(&b, b64, b64len);
	    kitty_sb_putc(&b, '\007');
	    kitty_emit(b.s, b.n, 1);
	    fs_give((void **) &b.s);
	}
	else{
	    fputs(hdr, stdout);
	    fwrite(b64, 1, b64len, stdout);
	    fputc('\007', stdout);
	}

	fputs("\0338", stdout);
	fs_give((void **) &b64);
    }
    else if(gfx == GFX_SIXEL){
	char *six = kitty_sixel_encode(t, y0, hpx);

	if(!six)
	  return;

	fputs("\0337", stdout);
	kitty_goto(row, col);
	kitty_emit(six, strlen(six), 1);
	fputs("\0338", stdout);
	fs_give((void **) &six);
    }
}


/*----------------------------------------------------------------------
   Encoders
  ----*/

static void
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


static void
kitty_sb_puts(KITTY_SBUF *b, const char *p)
{
    if(p)
      kitty_sb_put(b, p, strlen(p));
}


static void
kitty_sb_putc(KITTY_SBUF *b, int c)
{
    char ch = (char) c;

    kitty_sb_put(b, &ch, 1);
}


static void
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


/*
 * Sixel-encode rows [y0, y0+h) of the thumbnail with a 6x6x6 palette,
 * Floyd-Steinberg dithering, and transparent padding left unpainted.
 */
static char *
kitty_sixel_encode(KITTY_THUMB_S *t, int y0, int h)
{
    KITTY_SBUF	   b;
    unsigned char *idx;
    int		  *cur, *nxt, *tmpe;
    int		   w, x, y, c, band, dy, color, used[216], band_used[216];
    char	   tmp[48];

    if(!t || !t->rgba || h <= 0 || y0 < 0 || y0 + h > t->h)
      return(NULL);

    w = t->w;
    memset(&b, 0, sizeof(b));
    idx = (unsigned char *) fs_get((size_t) w * (size_t) h);
    cur = (int *) fs_get((size_t) (w + 2) * 3 * sizeof(int));
    nxt = (int *) fs_get((size_t) (w + 2) * 3 * sizeof(int));
    memset(cur, 0, (size_t) (w + 2) * 3 * sizeof(int));
    memset(used, 0, sizeof(used));

    for(y = 0; y < h; y++){
	unsigned char *row = t->rgba + (size_t) (y0 + y) * (size_t) w * 4;

	memset(nxt, 0, (size_t) (w + 2) * 3 * sizeof(int));
	for(x = 0; x < w; x++){
	    int q[3], e[3];

	    if(row[x * 4 + 3] < 128){
		idx[y * w + x] = 255;		/* transparent */
		continue;
	    }

	    for(c = 0; c < 3; c++){
		int v = row[x * 4 + c] + cur[(x + 1) * 3 + c] / 16;

		if(v < 0)
		  v = 0;

		if(v > 255)
		  v = 255;

		q[c] = (v * 5 + 127) / 255;
		e[c] = v - q[c] * 51;
	    }

	    idx[y * w + x] = (unsigned char) (q[0] * 36 + q[1] * 6 + q[2]);
	    used[idx[y * w + x]] = 1;

	    for(c = 0; c < 3; c++){
		cur[(x + 2) * 3 + c] += e[c] * 7;
		nxt[(x) * 3 + c]     += e[c] * 3;
		nxt[(x + 1) * 3 + c] += e[c] * 5;
		nxt[(x + 2) * 3 + c] += e[c] * 1;
	    }
	}

	tmpe = cur;
	cur = nxt;
	nxt = tmpe;
    }

    kitty_sb_puts(&b, "\033P7;1;0q");
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
	      if(idx[y * w + x] != 255)
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
    fs_give((void **) &cur);
    fs_give((void **) &nxt);
    return(b.s);
}


static char *
kitty_b64_bin(const unsigned char *s, size_t srcl, size_t *outlen)
{
    static const char v[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t	   n;
    unsigned char *d, *ret;

    if(!s)
      return(NULL);

    n = ((srcl + 2) / 3) * 4;
    ret = (unsigned char *) fs_get(n + 1);
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
    if(outlen)
      *outlen = (size_t) (d - ret);

    return((char *) ret);
}


static unsigned char *
kitty_b64_dec(const char *s, size_t n, size_t *outlen)
{
    unsigned char *out, *d;
    unsigned long  acc = 0;
    int		   bits = 0;
    size_t	   i;

    out = (unsigned char *) fs_get(n / 4 * 3 + 4);
    d = out;
    for(i = 0; i < n; i++){
	int c = (unsigned char) s[i], v;

	if(c >= 'A' && c <= 'Z')
	  v = c - 'A';
	else if(c >= 'a' && c <= 'z')
	  v = c - 'a' + 26;
	else if(c >= '0' && c <= '9')
	  v = c - '0' + 52;
	else if(c == '+' || c == '-')
	  v = 62;
	else if(c == '/' || c == '_')
	  v = 63;
	else if(c == '=')
	  break;
	else if(isspace(c))
	  continue;
	else
	  break;

	acc = (acc << 6) | (unsigned long) v;
	bits += 6;
	if(bits >= 8){
	    bits -= 8;
	    *d++ = (unsigned char) ((acc >> bits) & 0xff);
	}
    }

    *outlen = (size_t) (d - out);
    return(out);
}

#endif /* !_WINDOWS */
