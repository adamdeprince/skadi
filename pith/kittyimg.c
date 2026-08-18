/*
 * ========================================================================
 * Copyright 2026 Eduardo Chappa
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
     graphics protocol (file transfer + placement at the cursor).

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
#include <unistd.h>
#include <fcntl.h>
#endif


#define	KITTY_MAX_BYTES		(20L * 1024L * 1024L)
#define	KITTY_ID_BASE		0xA10001u
#define	KITTY_MAX_ROWS		40
#define	KITTY_MIN_COLS		4
#define	KITTY_MIN_ROWS		2


typedef struct kitty_img_s {
    unsigned	 id;
    char	*png_path;
    char	*src_path;
    int		 rows;
    int		 cols;
    unsigned	 transmitted:1;
    unsigned	 own_png:1;
    unsigned	 own_src:1;
} KITTY_IMG_S;


static KITTY_IMG_S *k_imgs = NULL;
static int	    k_n = 0;
static int	    k_alloc = 0;
static unsigned	    k_next_id = KITTY_ID_BASE;
static int	    k_detected = -1;


/* internal prototypes */
int	 kitty_terminal_detected(void);
int	 kitty_cell_pixels(int *, int *);
void	 kitty_fit_cells(int, int, int, int *, int *);
int	 kitty_read_u16be(unsigned char *);
unsigned long kitty_read_u32be(unsigned char *);
unsigned long kitty_read_u32le(unsigned char *);
int	 kitty_read_u16le(unsigned char *);
int	 kitty_dims_from_mem(unsigned char *, long, int *, int *);
int	 kitty_dims_from_file(char *, int *, int *);
int	 kitty_file_is_png(char *);
char	*kitty_convert_to_png(char *);
char	*kitty_b64(const char *);
#ifndef	_WINDOWS
int	 kitty_run_cmd(char **);
#endif
int	 kitty_register(char *, char *, int, int, int, int);
KITTY_IMG_S *kitty_find(unsigned);
void	 kitty_write_raw(const char *);
void	 kitty_write_apc(const char *, const char *);
void	 kitty_transmit(KITTY_IMG_S *);
void	 kitty_delete_id(unsigned, int);
void	 kitty_hide_placements(void);
void	 kitty_place(int, int, KITTY_IMG_S *);
int	 kitty_handle_on_line(char *, int, HANDLE_S *, HANDLE_S **);
long	 kitty_current_rawno(void);
int	 kitty_extract_part(ATTACH_S *, long, char **);
int	 kitty_decode_data_uri(char *, char **);


int
kitty_img_enabled(void)
{
#ifdef	_WINDOWS
    return(0);
#else
    if(F_OFF(F_KITTY_INLINE_IMAGES, ps_global))
      return(0);

    return(kitty_terminal_detected());
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
    char *src = NULL, *png = NULL;
    int   iw = 0, ih = 0, rows = 0, cols = 0, own_png, id;

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

    if(kitty_file_is_png(src)){
	png = src;
	own_png = 1;
    }
    else if((png = kitty_convert_to_png(src)) != NULL)
      own_png = 1;
    else{
	our_unlink(src);
	fs_give((void **) &src);
	return(0);
    }

    (void) kitty_dims_from_file(png, &iw, &ih);
    kitty_fit_cells(iw, ih,
		    ps_global && ps_global->ttyo
		      ? ps_global->ttyo->screen_cols : 80,
		    &cols, &rows);

    id = kitty_register(png, src, own_png, (src != png), rows, cols);
    if(id <= 0){
	if(own_png && png){
	    our_unlink(png);
	    fs_give((void **) &png);
	}

	if(src){
	    our_unlink(src);
	    fs_give((void **) &src);
	}

	return(0);
    }

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
    char *path = NULL, *png = NULL;
    int   iw = 0, ih = 0, rows = 0, cols = 0, own_png, id;

    if(!(src && rowsp && colsp))
      return(0);

    if(!kitty_decode_data_uri(src, &path) || !path)
      return(0);

    if(kitty_file_is_png(path)){
	png = path;
	own_png = 1;
    }
    else if((png = kitty_convert_to_png(path)) != NULL)
      own_png = 1;
    else{
	our_unlink(path);
	fs_give((void **) &path);
	return(0);
    }

    (void) kitty_dims_from_file(png, &iw, &ih);
    kitty_fit_cells(iw, ih,
		    ps_global && ps_global->ttyo
		      ? ps_global->ttyo->screen_cols : 80,
		    &cols, &rows);

    id = kitty_register(png, path, own_png, (path != png), rows, cols);
    if(id <= 0){
	if(own_png && png){
	    our_unlink(png);
	    fs_give((void **) &png);
	}

	if(path){
	    our_unlink(path);
	    fs_give((void **) &path);
	}

	return(0);
    }

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
kitty_terminal_detected(void)
{
    char *s;

    if(k_detected >= 0)
      return(k_detected);

    k_detected = 0;

    if(getenv("KITTY_WINDOW_ID") || getenv("KITTY_PID"))
      k_detected = 1;
    else if((s = getenv("TERM")) != NULL
	    && strstr(s, "kitty") != NULL)
      k_detected = 1;
    else if((s = getenv("TERM_PROGRAM")) != NULL
	    && !strucmp(s, "kitty"))
      k_detected = 1;

    return(k_detected);
}


int
kitty_cell_pixels(int *cwp, int *chp)
{
#ifdef	TIOCGWINSZ
    struct winsize ws;

    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0
       && ws.ws_col > 0 && ws.ws_row > 0
       && ws.ws_xpixel > 0 && ws.ws_ypixel > 0){
	*cwp = ws.ws_xpixel / ws.ws_col;
	*chp = ws.ws_ypixel / ws.ws_row;
	if(*cwp > 0 && *chp > 0)
	  return(1);
    }
#endif

    *cwp = 8;
    *chp = 16;
    return(0);
}


void
kitty_fit_cells(int img_w, int img_h, int avail_cols, int *colsp, int *rowsp)
{
    int cw, ch, max_cols, max_rows, nat_c, nat_r;
    int screen_rows;

    kitty_cell_pixels(&cw, &ch);

    screen_rows = (ps_global && ps_global->ttyo)
		    ? ps_global->ttyo->screen_rows : 24;

    max_cols = MAX(KITTY_MIN_COLS, avail_cols > 2 ? avail_cols - 2 : avail_cols);
    max_rows = screen_rows > 8 ? screen_rows - 6 : MAX(KITTY_MIN_ROWS, screen_rows / 2);
    if(max_rows > KITTY_MAX_ROWS)
      max_rows = KITTY_MAX_ROWS;
    if(max_rows < KITTY_MIN_ROWS)
      max_rows = KITTY_MIN_ROWS;

    if(img_w <= 0 || img_h <= 0 || cw <= 0 || ch <= 0){
	*colsp = MIN(max_cols, 48);
	*rowsp = MIN(max_rows, 12);
	return;
    }

    nat_c = (img_w + cw - 1) / cw;
    nat_r = (img_h + ch - 1) / ch;
    if(nat_c < 1)
      nat_c = 1;
    if(nat_r < 1)
      nat_r = 1;

    if(nat_c <= max_cols && nat_r <= max_rows){
	*colsp = nat_c;
	*rowsp = nat_r;
    }
    else{
	double sc = (double) max_cols / (double) nat_c;
	double sr = (double) max_rows / (double) nat_r;
	double s  = (sc < sr) ? sc : sr;

	*colsp = MAX(1, (int) (nat_c * s));
	*rowsp = MAX(1, (int) (nat_r * s));
    }
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
kitty_convert_to_png(char *src)
{
#ifdef	_WINDOWS
    (void) src;
    return(NULL);
#else
    char *dst, *argv[10];

    if(!src)
      return(NULL);

    dst = temp_nam_ext(NULL, "kp", "png");
    if(!dst)
      return(NULL);

    argv[0] = "sips";
    argv[1] = "-s";
    argv[2] = "format";
    argv[3] = "png";
    argv[4] = src;
    argv[5] = "--out";
    argv[6] = dst;
    argv[7] = NULL;
    if(kitty_run_cmd(argv) == 0 && kitty_file_is_png(dst))
      return(dst);

    argv[0] = "magick";
    argv[1] = src;
    argv[2] = dst;
    argv[3] = NULL;
    if(kitty_run_cmd(argv) == 0 && kitty_file_is_png(dst))
      return(dst);

    argv[0] = "convert";
    argv[1] = src;
    argv[2] = dst;
    argv[3] = NULL;
    if(kitty_run_cmd(argv) == 0 && kitty_file_is_png(dst))
      return(dst);

    our_unlink(dst);
    fs_give((void **) &dst);
    return(NULL);
#endif
}


int
kitty_register(char *png, char *src, int own_png, int own_src,
	       int rows, int cols)
{
    KITTY_IMG_S *im;

    if(!png || rows <= 0 || cols <= 0)
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
    im->id	= k_next_id++;
    im->png_path = png;
    im->src_path = (src && src != png) ? src : NULL;
    im->rows	= rows;
    im->cols	= cols;
    im->own_png	= own_png ? 1 : 0;
    im->own_src	= (own_src && im->src_path) ? 1 : 0;
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

    if(!im || !im->png_path || im->transmitted)
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
	     free_data ? 'i' : 'I', id);
    kitty_write_apc(ctrl, NULL);
}


void
kitty_hide_placements(void)
{
    int i;

    for(i = 0; i < k_n; i++)
      if(k_imgs[i].transmitted)
	kitty_delete_id(k_imgs[i].id, 0);
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
