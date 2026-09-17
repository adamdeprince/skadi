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

     goblindl.c
     Send an attachment through the terminal to the local client.
     Kitty file transfer (OSC 5113) is preferred; OSC 777
     goblin-download is the fallback.

  ====*/


#include "../pith/headers.h"
#include "../pith/goblindl.h"
#include "../pith/state.h"
#include "../pith/store.h"
#include "../pith/filter.h"
#include "../pith/detach.h"
#include "../pith/status.h"
#include "../pith/busy.h"
#include "../pith/util.h"
#include "../pith/mimedesc.h"
#include "../pith/osdep/temp_nam.h"
#include "../pith/osdep/filesize.h"

#ifndef	_WINDOWS
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#endif


#define	GOBLIN_MAX_CHUNK	4096
#define	GOBLIN_QUERY_MS		1500
#define	GOBLIN_SAVE_WAIT_MS	30000
#define	GOBLIN_SAVE_WAIT_MAX	300000

#define	KITTY_XFER_PERM_MS	60000
#define	KITTY_XFER_STARTED_MS	8000
#define	KITTY_XFER_CHUNK_MS	2000
#define	KITTY_XFER_DONE_MS	30000


static unsigned	g_next_id = 1;
static int	g_supported = -1;	/* -1 unknown, 0 no, 1 yes */
static int	g_max_chunk = GOBLIN_MAX_CHUNK;
static long	g_sent;
static long	g_total;


/* internal prototypes */
unsigned goblin_new_id(void);
char	*goblin_b64(const unsigned char *, unsigned long);
void	 goblin_write_osc(const char *);
#ifndef	_WINDOWS
int	 goblin_read_osc(char *, int, int);
int	 goblin_parse_status(char *, unsigned, char *, size_t, int *);
int	 goblin_wait_status(unsigned, int, char *, size_t, int *);
int	 goblin_query(int *);
int	 goblin_percent(void);
char	*goblin_basename(char *);
int	 goblin_extract_part(ATTACH_S *, long, char **);
int	 goblin_send_file(unsigned, char *, char *, long, int);
int	 kitty_xfer_available(void);
int	 kitty_b64dec(char *, unsigned char *, int);
typedef struct kitty_msg {
    char	  action[32];
    char	  fid[80];
    char	  status[256];
    char	  name[1024];
    char	  ftype[16];
    unsigned long size;
    unsigned char *data;
    unsigned long datalen;
} KITTY_MSG;
void	 kitty_msg_free(KITTY_MSG *);
int	 kitty_parse_msg(char *, char *, KITTY_MSG *);
int	 kitty_parse_osc(char *, char *, char *, size_t, char *, size_t,
			 char *, size_t);
int	 kitty_wait(char *, char *, char *, int, char *, size_t);
void	 kitty_safe_name(char *);
void	 kitty_remote_path(char *, char *, size_t);
void	 kitty_cancel(char *);
int	 kitty_send_file(char *, char *, char *, long);
int	 kitty_xfer_attachment(long, ATTACH_S *);
int	 kitty_xfer_receive(char *, char **, long *);
#endif


unsigned
goblin_new_id(void)
{
    if(g_next_id == 0)
      g_next_id = 1;

    return(g_next_id++);
}


char *
goblin_b64(const unsigned char *src, unsigned long srcl)
{
    static const char v[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned long n;
    unsigned char *d, *ret;
    const unsigned char *s;

    if(!src)
      return(NULL);

    n = ((srcl + 2) / 3) * 4;
    ret = (unsigned char *) fs_get((size_t) n + 1);
    s = src;
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
goblin_write_osc(const char *body)
{
#ifdef	_WINDOWS
    (void) body;
#else
    int tmux = (getenv("TMUX") != NULL);

    if(!body)
      return;

    if(tmux)
      fputs("\033Ptmux;", stdout);

    if(tmux)
      fputc('\033', stdout);

    fputs("\033]", stdout);
    fputs(body, stdout);

    if(tmux)
      fputc('\033', stdout);

    fputs("\033\\", stdout);

    if(tmux)
      fputs("\033\\", stdout);

    fflush(stdout);
#endif
}


#ifndef	_WINDOWS
int
goblin_read_osc(char *buf, int buflen, int timeout_ms)
{
    struct timeval tv;
    fd_set	   fds;
    int		   n, got = 0, oldfl, esc = 0;
    char	   c;

    if(!buf || buflen < 8)
      return(0);

    if(!isatty(STDIN_FILENO))
      return(0);

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
	n = (int) read(STDIN_FILENO, &c, 1);
	if(n <= 0){
	    tv.tv_sec = 0;
	    tv.tv_usec = 40000;
	    FD_ZERO(&fds);
	    FD_SET(STDIN_FILENO, &fds);
	    if(select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0)
	      break;

	    continue;
	}

	if(!got){
	    /* skip until OSC: ESC ] */
	    if(esc && c == ']'){
		buf[got++] = '\033';
		buf[got++] = ']';
		esc = 0;
		continue;
	    }

	    esc = (c == '\033');
	    continue;
	}

	if(c == '\007')			/* BEL terminator */
	  break;

	if(esc && c == '\\'){		/* ST terminator */
	    if(got > 0)
	      got--;			/* drop the ESC */

	    break;
	}

	esc = (c == '\033');
	buf[got++] = c;
    }

    if(oldfl >= 0)
      (void) fcntl(STDIN_FILENO, F_SETFL, oldfl);

    buf[got] = '\0';
    return(got);
}


int
goblin_parse_status(char *s, unsigned int want_id, char *status, size_t slen,
		    int *max_chunk)
{
    char *p, *semi, *colon, *eq, key[32], val[64];
    unsigned id = 0;
    int	     is_status = 0;

    if(status && slen)
      status[0] = '\0';

    if(!s)
      return(0);

    p = s;
    if(p[0] == '\033' && p[1] == ']')
      p += 2;

    if(strncmp(p, "777;goblin-download;", 20) != 0)
      return(0);

    p += 20;
    semi = strchr(p, ';');
    if(semi)
      *semi = '\0';

    while(p && *p){
	colon = strchr(p, ':');
	if(colon)
	  *colon = '\0';

	eq = strchr(p, '=');
	if(eq){
	    *eq = '\0';
	    strncpy(key, p, sizeof(key) - 1);
	    key[sizeof(key)-1] = '\0';
	    strncpy(val, eq + 1, sizeof(val) - 1);
	    val[sizeof(val)-1] = '\0';

	    if(!strcmp(key, "op") && !strcmp(val, "status"))
	      is_status = 1;
	    else if(!strcmp(key, "id"))
	      id = (unsigned) strtoul(val, NULL, 10);
	    else if(!strcmp(key, "status") && status && slen){
		strncpy(status, val, slen - 1);
		status[slen-1] = '\0';
	    }
	    else if(!strcmp(key, "max_chunk") && max_chunk)
	      *max_chunk = atoi(val);
	}

	p = colon ? colon + 1 : NULL;
    }

    return(is_status && id == want_id && status && status[0]);
}


int
goblin_wait_status(unsigned int id, int timeout_ms, char *status, size_t slen,
		   int *max_chunk)
{
    char buf[1024];
    int  n, elapsed = 0, slice;

    if(status && slen)
      status[0] = '\0';

    while(elapsed < timeout_ms){
	slice = timeout_ms - elapsed;
	if(slice > 200)
	  slice = 200;

	n = goblin_read_osc(buf, sizeof(buf), slice);
	elapsed += slice;
	ALARM_BLIP();
	if(n <= 0)
	  continue;

	if(goblin_parse_status(buf, id, status, slen, max_chunk))
	  return(1);
    }

    return(0);
}


int
goblin_query(int *max_chunk)
{
    unsigned id;
    char     body[128], status[32];
    int	     mc = GOBLIN_MAX_CHUNK;

    if(g_supported >= 0){
	if(max_chunk)
	  *max_chunk = g_max_chunk;

	return(g_supported);
    }

    id = goblin_new_id();
    snprintf(body, sizeof(body),
	     "777;goblin-download;v=1:op=query:id=%u", id);
    goblin_write_osc(body);

    status[0] = '\0';
    if(goblin_wait_status(id, GOBLIN_QUERY_MS, status, sizeof(status), &mc)
       && !strcmp(status, "supported")){
	if(mc <= 0 || mc > GOBLIN_MAX_CHUNK)
	  mc = GOBLIN_MAX_CHUNK;

	g_supported = 1;
	g_max_chunk = mc;
    }
    else
      g_supported = 0;

    if(max_chunk)
      *max_chunk = g_max_chunk;

    return(g_supported);
}


int
goblin_percent(void)
{
    if(g_total <= 0)
      return(0);

    return((int) ((100L * g_sent) / g_total));
}


char *
goblin_basename(char *name)
{
    char *p;

    if(!name || !*name)
      return(cpystr("attachment"));

    p = last_cmpnt(name);
    if(!p)
      p = name;

    while(*p == '/' || *p == '\\')
      p++;

    if(!*p)
      return(cpystr("attachment"));

    return(cpystr(p));
}


int
goblin_extract_part(ATTACH_S *a, long int msgno, char **pathp)
{
    STORE_S *so;
    gf_o_t   pc;
    char    *err, *path;

    *pathp = NULL;
    if(!(a && a->number && pathp && ps_global && ps_global->mail_stream))
      return(0);

    path = temp_nam(NULL, "gd");
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
	our_unlink(path);
	fs_give((void **) &path);
	return(0);
    }

    *pathp = path;
    return(1);
}


int
goblin_send_file(unsigned int id, char *name, char *path, long int nbytes,
		 int chunk)
{
    FILE	  *fp;
    unsigned char  raw[GOBLIN_MAX_CHUNK];
    char	  *b64, *nb64, body[128 + GOBLIN_MAX_CHUNK * 2];
    size_t	   n;
    char	   status[32];
    int		   waited = 0, busy = 0;

    if(chunk <= 0 || chunk > GOBLIN_MAX_CHUNK)
      chunk = GOBLIN_MAX_CHUNK;

    nb64 = goblin_b64((unsigned char *) name, (unsigned long) strlen(name));
    if(!nb64)
      return(GOBLIN_DL_ERROR);

    snprintf(body, sizeof(body),
	     "777;goblin-download;v=1:op=begin:id=%u:name=%s:size=%ld:reply=1",
	     id, nb64, nbytes);
    goblin_write_osc(body);
    fs_give((void **) &nb64);

    fp = our_fopen(path, "rb");
    if(!fp){
	snprintf(body, sizeof(body),
		 "777;goblin-download;v=1:op=cancel:id=%u", id);
	goblin_write_osc(body);
	return(GOBLIN_DL_ERROR);
    }

    g_sent = 0;
    g_total = nbytes;
    busy = busy_cue(_("Downloading to local client"), goblin_percent, 0);

    while((n = fread(raw, 1, (size_t) chunk, fp)) > 0){
	b64 = goblin_b64(raw, (unsigned long) n);
	if(!b64){
	    fclose(fp);
	    snprintf(body, sizeof(body),
		     "777;goblin-download;v=1:op=cancel:id=%u", id);
	    goblin_write_osc(body);
	    if(busy)
	      cancel_busy_cue(-1);

	    return(GOBLIN_DL_ERROR);
	}

	snprintf(body, sizeof(body),
		 "777;goblin-download;v=1:op=data:id=%u;%s", id, b64);
	goblin_write_osc(body);
	fs_give((void **) &b64);
	g_sent += (long) n;
	ALARM_BLIP();
    }

    fclose(fp);

    snprintf(body, sizeof(body),
	     "777;goblin-download;v=1:op=end:id=%u", id);
    goblin_write_osc(body);

    status[0] = '\0';
    while(waited < GOBLIN_SAVE_WAIT_MAX){
	int slice = GOBLIN_SAVE_WAIT_MS;

	if(goblin_wait_status(id, slice, status, sizeof(status), NULL)){
	    if(!strcmp(status, "saved")){
		if(busy)
		  cancel_busy_cue(-1);

		return(GOBLIN_DL_OK);
	    }

	    if(!strcmp(status, "error")){
		if(busy)
		  cancel_busy_cue(-1);

		return(GOBLIN_DL_ERROR);
	    }

	    /* queued: keep waiting */
	    waited += slice;
	    continue;
	}

	waited += slice;
	break;
    }

    if(busy)
      cancel_busy_cue(-1);

    snprintf(body, sizeof(body),
	     "777;goblin-download;v=1:op=cancel:id=%u", id);
    goblin_write_osc(body);
    return(GOBLIN_DL_ERROR);
}


int
kitty_xfer_available(void)
{
    char *s;

    if(getenv("KITTY_WINDOW_ID") || getenv("KITTY_PID"))
      return(1);

    if((s = getenv("TERM")) != NULL && strstr(s, "kitty") != NULL)
      return(1);

    if((s = getenv("TERM_PROGRAM")) != NULL && !strucmp(s, "kitty"))
      return(1);

    return(0);
}


int
kitty_b64dec(char *in, unsigned char *out, int outlen)
{
    static const char tbl[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned char d[4];
    int		  n, v, o = 0, pads;
    char	 *p;

    if(!in || !out || outlen < 1)
      return(0);

    while(*in && o < outlen){
	n = 0;
	pads = 0;
	memset(d, 0, sizeof(d));
	while(n < 4 && *in){
	    if(*in == '='){
		in++;
		d[n++] = 0;
		pads++;
		continue;
	    }

	    if(isspace((unsigned char) *in)){
		in++;
		continue;
	    }

	    p = strchr(tbl, *in++);
	    if(!p)
	      return(0);

	    d[n++] = (unsigned char) (p - tbl);
	}

	if(n < 2)
	  break;

	v = (d[0] << 18) | (d[1] << 12) | (d[2] << 6) | d[3];
	if(o < outlen)
	  out[o++] = (unsigned char) ((v >> 16) & 0xff);

	if(n > 2 && pads < 2 && o < outlen)
	  out[o++] = (unsigned char) ((v >> 8) & 0xff);

	if(n > 3 && pads < 1 && o < outlen)
	  out[o++] = (unsigned char) (v & 0xff);

	if(n < 4)
	  break;
    }

    return(o);
}


void
kitty_msg_free(KITTY_MSG *m)
{
    if(!m)
      return;

    if(m->data)
      fs_give((void **) &m->data);

    memset(m, 0, sizeof(*m));
}


int
kitty_parse_msg(char *s, char *want_id, KITTY_MSG *m)
{
    char *p, *semi, *eq, *rawst = NULL, *rawn = NULL, *rawd = NULL;
    int   id_ok = 0, n;

    if(!m)
      return(0);

    memset(m, 0, sizeof(*m));
    if(!s || !want_id || !want_id[0])
      return(0);

    p = s;
    if(p[0] == '\033' && p[1] == ']')
      p += 2;

    if(strncmp(p, "5113;", 5) != 0)
      return(0);

    p += 5;
    while(p && *p){
	semi = strchr(p, ';');
	if(semi)
	  *semi = '\0';

	eq = strchr(p, '=');
	if(eq){
	    *eq = '\0';
	    if(!strcmp(p, "ac") || !strcmp(p, "action")){
		strncpy(m->action, eq + 1, sizeof(m->action) - 1);
	    }
	    else if(!strcmp(p, "id")){
		if(strcmp(eq + 1, want_id) != 0)
		  return(0);

		id_ok = 1;
	    }
	    else if(!strcmp(p, "fid") || !strcmp(p, "file_id")){
		strncpy(m->fid, eq + 1, sizeof(m->fid) - 1);
	    }
	    else if(!strcmp(p, "st") || !strcmp(p, "status")){
		rawst = eq + 1;
	    }
	    else if(!strcmp(p, "n") || !strcmp(p, "name")){
		rawn = eq + 1;
	    }
	    else if(!strcmp(p, "d") || !strcmp(p, "data")){
		rawd = eq + 1;
	    }
	    else if(!strcmp(p, "ft") || !strcmp(p, "file_type")){
		strncpy(m->ftype, eq + 1, sizeof(m->ftype) - 1);
	    }
	    else if(!strcmp(p, "sz") || !strcmp(p, "size")){
		m->size = strtoul(eq + 1, NULL, 10);
	    }
	}

	p = semi ? semi + 1 : NULL;
    }

    if(rawst && *rawst){
	n = kitty_b64dec(rawst, (unsigned char *) m->status,
			 (int) sizeof(m->status) - 1);
	if(n > 0)
	  m->status[n] = '\0';
	else{
	    strncpy(m->status, rawst, sizeof(m->status) - 1);
	    m->status[sizeof(m->status) - 1] = '\0';
	}
    }

    if(rawn && *rawn){
	n = kitty_b64dec(rawn, (unsigned char *) m->name,
			 (int) sizeof(m->name) - 1);
	if(n > 0)
	  m->name[n] = '\0';
    }

    if(rawd && *rawd){
	int maxb = ((int) strlen(rawd) * 3) / 4 + 4;

	m->data = (unsigned char *) fs_get((size_t) maxb);
	n = kitty_b64dec(rawd, m->data, maxb);
	if(n > 0)
	  m->datalen = (unsigned long) n;
	else
	  fs_give((void **) &m->data);
    }

    return(id_ok && m->action[0]);
}


int
kitty_parse_osc(char *s, char *want_id, char *action, size_t alen,
		char *fid, size_t flen, char *status, size_t slen)
{
    KITTY_MSG m;
    int	      ok;

    if(action && alen)
      action[0] = '\0';

    if(fid && flen)
      fid[0] = '\0';

    if(status && slen)
      status[0] = '\0';

    ok = kitty_parse_msg(s, want_id, &m);
    if(ok){
	if(action && alen){
	    strncpy(action, m.action, alen - 1);
	    action[alen - 1] = '\0';
	}

	if(fid && flen){
	    strncpy(fid, m.fid, flen - 1);
	    fid[flen - 1] = '\0';
	}

	if(status && slen){
	    strncpy(status, m.status, slen - 1);
	    status[slen - 1] = '\0';
	}
    }

    kitty_msg_free(&m);
    return(ok);
}


int
kitty_wait(char *id, char *want, char *want_fid, int timeout_ms,
	   char *status, size_t slen)
{
    char buf[2048], ac[32], fid[32], st[256];
    int  n, elapsed = 0, slice;

    if(status && slen)
      status[0] = '\0';

    while(elapsed < timeout_ms){
	slice = timeout_ms - elapsed;
	if(slice > 200)
	  slice = 200;

	n = goblin_read_osc(buf, sizeof(buf), slice);
	elapsed += slice;
	ALARM_BLIP();
	if(n <= 0)
	  continue;

	ac[0] = fid[0] = st[0] = '\0';
	if(!kitty_parse_osc(buf, id, ac, sizeof(ac), fid, sizeof(fid),
			    st, sizeof(st)))
	  continue;

	if(strcmp(ac, "status") != 0)
	  continue;

	if(want_fid && want_fid[0] && strcmp(fid, want_fid) != 0)
	  continue;

	if(status && slen){
	    strncpy(status, st, slen - 1);
	    status[slen - 1] = '\0';
	}

	if(!want || !want[0])
	  return(1);

	if(!strncmp(st, want, strlen(want)))
	  return(1);

	if(!strncmp(st, "EPERM", 5) || !strncmp(st, "CANCELED", 8)
	   || (st[0] == 'E' && isupper((unsigned char) st[1])))
	  return(-1);
    }

    return(0);
}


void
kitty_remote_path(char *in, char *out, size_t outlen)
{
    char *p;

    if(!out || outlen < 3)
      return;

    out[0] = '\0';
    if(!in)
      return;

    while(*in == ' ' || *in == '\t')
      in++;

    if((in[0] == '"' && in[strlen(in) - 1] == '"')
       || (in[0] == '\'' && in[strlen(in) - 1] == '\'')){
	strncpy(out, in + 1, outlen - 1);
	out[outlen - 1] = '\0';
	if(out[0])
	  out[strlen(out) - 1] = '\0';
    }
    else{
	strncpy(out, in, outlen - 1);
	out[outlen - 1] = '\0';
    }

    for(p = out; *p; p++){
	if(*p == '\\')
	  *p = '/';
    }

    if(out[0] == '/' || (out[0] == '~' && (out[1] == '/' || out[1] == '\0')))
      return;

    if(outlen > 3){
	char tmp[1024];

	strncpy(tmp, out, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = '\0';
	snprintf(out, outlen, "~/%s", tmp);
    }
}


void
kitty_safe_name(char *s)
{
    if(!s)
      return;

    for(; *s; s++){
	if(*s == '/' || *s == '\\' || *s == ':' || *s == '*' || *s == '?'
	   || *s == '"' || *s == '<' || *s == '>' || *s == '|')
	  *s = '_';
    }
}


void
kitty_cancel(char *id)
{
    char body[128];

    if(!id)
      return;

    snprintf(body, sizeof(body), "5113;ac=cancel;id=%s", id);
    goblin_write_osc(body);
    (void) kitty_wait(id, "CANCELED", NULL, 1500, NULL, 0);
}


int
kitty_send_file(char *id, char *name, char *path, long int nbytes)
{
    FILE	  *fp;
    unsigned char  raw[GOBLIN_MAX_CHUNK];
    char	  *b64, body[128 + GOBLIN_MAX_CHUNK * 2], dest[MAILTMPLEN];
    char	   status[256];
    size_t	   n, lastn;
    int		   busy = 0, rv, got;
    unsigned char  last[GOBLIN_MAX_CHUNK];

    if(!id || !name || !path)
      return(GOBLIN_DL_ERROR);

    snprintf(dest, sizeof(dest), "~/Downloads/%s", name);
    snprintf(body, sizeof(body), "5113;ac=send;id=%s", id);
    goblin_write_osc(body);

    busy = busy_cue(_("Allow the transfer in kitty"), NULL, 0);
    got = kitty_wait(id, "OK", NULL, KITTY_XFER_PERM_MS, status, sizeof(status));
    if(busy)
      cancel_busy_cue(-1);

    if(got < 0){
	if(status[0] && !strncmp(status, "EPERM", 5))
	  return(GOBLIN_DL_CANCEL);

	return(GOBLIN_DL_ERROR);
    }

    if(got == 0){
	kitty_cancel(id);
	return(GOBLIN_DL_UNSUPPORTED);
    }

    b64 = goblin_b64((unsigned char *) dest, (unsigned long) strlen(dest));
    if(!b64)
      return(GOBLIN_DL_ERROR);

    snprintf(body, sizeof(body),
	     "5113;ac=file;id=%s;fid=f1;n=%s;sz=%ld;ft=regular",
	     id, b64, nbytes);
    goblin_write_osc(body);
    fs_give((void **) &b64);

    got = kitty_wait(id, "STARTED", "f1", KITTY_XFER_STARTED_MS,
		     status, sizeof(status));
    if(got < 0){
	kitty_cancel(id);
	return(GOBLIN_DL_ERROR);
    }

    fp = our_fopen(path, "rb");
    if(!fp){
	kitty_cancel(id);
	return(GOBLIN_DL_ERROR);
    }

    g_sent = 0;
    g_total = nbytes;
    busy = busy_cue(_("Downloading via kitty"), goblin_percent, 0);

    lastn = 0;
    rv = GOBLIN_DL_OK;
    while((n = fread(raw, 1, (size_t) GOBLIN_MAX_CHUNK, fp)) > 0){
	if(lastn){
	    b64 = goblin_b64(last, (unsigned long) lastn);
	    if(!b64){
		rv = GOBLIN_DL_ERROR;
		break;
	    }

	    snprintf(body, sizeof(body),
		     "5113;ac=data;id=%s;fid=f1;d=%s", id, b64);
	    goblin_write_osc(body);
	    fs_give((void **) &b64);
	    g_sent += (long) lastn;
	    (void) kitty_wait(id, "PROGRESS", "f1", KITTY_XFER_CHUNK_MS,
			      NULL, 0);
	}

	memcpy(last, raw, n);
	lastn = n;
	ALARM_BLIP();
    }

    fclose(fp);
    if(rv != GOBLIN_DL_OK){
	if(busy)
	  cancel_busy_cue(-1);

	kitty_cancel(id);
	return(rv);
    }

    if(lastn){
	b64 = goblin_b64(last, (unsigned long) lastn);
	if(!b64){
	    if(busy)
	      cancel_busy_cue(-1);

	    kitty_cancel(id);
	    return(GOBLIN_DL_ERROR);
	}

	snprintf(body, sizeof(body),
		 "5113;ac=end_data;id=%s;fid=f1;d=%s", id, b64);
	goblin_write_osc(body);
	fs_give((void **) &b64);
	g_sent += (long) lastn;
    }
    else{
	snprintf(body, sizeof(body), "5113;ac=end_data;id=%s;fid=f1", id);
	goblin_write_osc(body);
    }

    got = kitty_wait(id, "OK", "f1", KITTY_XFER_DONE_MS, status, sizeof(status));
    if(got <= 0){
	if(busy)
	  cancel_busy_cue(-1);

	kitty_cancel(id);
	return(got < 0 ? GOBLIN_DL_ERROR : GOBLIN_DL_ERROR);
    }

    snprintf(body, sizeof(body), "5113;ac=finish;id=%s", id);
    goblin_write_osc(body);
    (void) kitty_wait(id, "OK", NULL, 2000, NULL, 0);

    if(busy)
      cancel_busy_cue(-1);

    return(GOBLIN_DL_OK);
}


int
kitty_xfer_attachment(long int msgno, ATTACH_S *a)
{
    char     *src = NULL, *name, filename[MAILTMPLEN], id[64];
    long      nbytes;
    int	      rv;
    static unsigned seq = 1;

    filename[0] = '\0';
    (void) get_filename_parameter(filename, sizeof(filename), a->body, NULL);
    name = goblin_basename(filename[0] ? filename : NULL);
    kitty_safe_name(name);

    if(!goblin_extract_part(a, msgno, &src) || !src){
	fs_give((void **) &name);
	return(GOBLIN_DL_ERROR);
    }

    nbytes = name_file_size(src);
    if(nbytes < 0L){
	our_unlink(src);
	fs_give((void **) &src);
	fs_give((void **) &name);
	return(GOBLIN_DL_ERROR);
    }

    snprintf(id, sizeof(id), "a%lx-%u", (unsigned long) time(NULL), seq++);
    if(seq == 0)
      seq = 1;

    rv = kitty_send_file(id, name, src, nbytes);
    our_unlink(src);
    fs_give((void **) &src);
    fs_give((void **) &name);
    return(rv);
}


int
kitty_xfer_receive(char *remote_path, char **outp, long *np)
{
    char	  id[64], spec[1024], absname[1024], ftype[16];
    char	  body[2048], buf[16384], *b64, *tmp = NULL;
    KITTY_MSG	  k;
    FILE	 *fp = NULL;
    int		  got, busy = 0, n, elapsed, slice;
    unsigned long expected = 0, written = 0;
    static unsigned seq = 1;

    if(outp)
      *outp = NULL;

    if(np)
      *np = 0;

    if(!kitty_xfer_available() || !remote_path || !remote_path[0] || !outp)
      return(GOBLIN_DL_UNSUPPORTED);

    if(!isatty(STDOUT_FILENO) || !isatty(STDIN_FILENO))
      return(GOBLIN_DL_UNSUPPORTED);

    kitty_remote_path(remote_path, spec, sizeof(spec));
    if(!spec[0])
      return(GOBLIN_DL_ERROR);

    snprintf(id, sizeof(id), "b%lx-%u", (unsigned long) time(NULL), seq++);
    if(seq == 0)
      seq = 1;

    snprintf(body, sizeof(body), "5113;ac=receive;id=%s;sz=1", id);
    goblin_write_osc(body);

    b64 = goblin_b64((unsigned char *) spec, (unsigned long) strlen(spec));
    if(!b64)
      return(GOBLIN_DL_ERROR);

    snprintf(body, sizeof(body), "5113;ac=file;id=%s;fid=0;n=%s", id, b64);
    goblin_write_osc(body);
    fs_give((void **) &b64);

    busy = busy_cue(_("Allow the transfer in kitty"), NULL, 0);
    got = kitty_wait(id, "OK", NULL, KITTY_XFER_PERM_MS, NULL, 0);
    if(busy)
      cancel_busy_cue(-1);

    if(got < 0){
	kitty_cancel(id);
	return(GOBLIN_DL_CANCEL);
    }

    if(got == 0){
	kitty_cancel(id);
	return(GOBLIN_DL_UNSUPPORTED);
    }

    absname[0] = '\0';
    ftype[0] = '\0';
    elapsed = 0;
    while(elapsed < 15000){
	slice = 15000 - elapsed;
	if(slice > 200)
	  slice = 200;

	n = goblin_read_osc(buf, sizeof(buf), slice);
	elapsed += slice;
	ALARM_BLIP();
	if(n <= 0)
	  continue;

	memset(&k, 0, sizeof(k));
	if(!kitty_parse_msg(buf, id, &k))
	  continue;

	if(!strcmp(k.action, "file")){
	    if(k.name[0])
	      strncpy(absname, k.name, sizeof(absname) - 1);

	    if(k.ftype[0])
	      strncpy(ftype, k.ftype, sizeof(ftype) - 1);

	    expected = k.size;
	    kitty_msg_free(&k);
	    continue;
	}

	if(!strcmp(k.action, "status")){
	    if(k.fid[0]){
		kitty_msg_free(&k);
		kitty_cancel(id);
		return(GOBLIN_DL_ERROR);
	    }

	    if(!strncmp(k.status, "OK", 2)){
		kitty_msg_free(&k);
		break;
	    }

	    kitty_msg_free(&k);
	    kitty_cancel(id);
	    return(GOBLIN_DL_ERROR);
	}

	kitty_msg_free(&k);
    }

    if(!absname[0]){
	kitty_cancel(id);
	return(GOBLIN_DL_ERROR);
    }

    if(ftype[0] && strcmp(ftype, "regular") != 0){
	kitty_cancel(id);
	return(GOBLIN_DL_ERROR);
    }

    tmp = temp_nam(NULL, "ku");
    if(!tmp){
	kitty_cancel(id);
	return(GOBLIN_DL_ERROR);
    }

    fp = our_fopen(tmp, "wb");
    if(!fp){
	our_unlink(tmp);
	fs_give((void **) &tmp);
	kitty_cancel(id);
	return(GOBLIN_DL_ERROR);
    }

    b64 = goblin_b64((unsigned char *) absname, (unsigned long) strlen(absname));
    if(!b64){
	fclose(fp);
	our_unlink(tmp);
	fs_give((void **) &tmp);
	kitty_cancel(id);
	return(GOBLIN_DL_ERROR);
    }

    snprintf(body, sizeof(body), "5113;ac=file;id=%s;fid=f1;n=%s", id, b64);
    goblin_write_osc(body);
    fs_give((void **) &b64);

    g_sent = 0;
    g_total = expected > 0 ? (long) expected : 1;
    busy = busy_cue(_("Uploading via kitty"), goblin_percent, 0);

    elapsed = 0;
    while(elapsed < 300000){
	slice = 2000;
	n = goblin_read_osc(buf, sizeof(buf), slice);
	elapsed += slice;
	ALARM_BLIP();
	if(n <= 0)
	  continue;

	memset(&k, 0, sizeof(k));
	if(!kitty_parse_msg(buf, id, &k))
	  continue;

	if(!strcmp(k.action, "data") || !strcmp(k.action, "end_data")){
	    if(k.data && k.datalen){
		if(fwrite(k.data, 1, (size_t) k.datalen, fp) != k.datalen){
		    kitty_msg_free(&k);
		    fclose(fp);
		    our_unlink(tmp);
		    fs_give((void **) &tmp);
		    if(busy)
		      cancel_busy_cue(-1);

		    kitty_cancel(id);
		    return(GOBLIN_DL_ERROR);
		}

		written += k.datalen;
		g_sent = (long) written;
	    }

	    if(!strcmp(k.action, "end_data")){
		kitty_msg_free(&k);
		break;
	    }

	    kitty_msg_free(&k);
	    elapsed = 0;
	    continue;
	}

	if(!strcmp(k.action, "status")
	   && (k.status[0] == 'E' || !strncmp(k.status, "CANCELED", 8))){
	    kitty_msg_free(&k);
	    fclose(fp);
	    our_unlink(tmp);
	    fs_give((void **) &tmp);
	    if(busy)
	      cancel_busy_cue(-1);

	    kitty_cancel(id);
	    return(GOBLIN_DL_ERROR);
	}

	kitty_msg_free(&k);
    }

    fclose(fp);
    snprintf(body, sizeof(body), "5113;ac=finish;id=%s", id);
    goblin_write_osc(body);
    (void) kitty_wait(id, "OK", NULL, 2000, NULL, 0);
    if(busy)
      cancel_busy_cue(-1);

    if(written == 0 && expected > 0){
	our_unlink(tmp);
	fs_give((void **) &tmp);
	return(GOBLIN_DL_ERROR);
    }

    *outp = tmp;
    if(np)
      *np = (long) written;

    return(GOBLIN_DL_OK);
}
#endif /* !_WINDOWS */


int
goblin_dl_attachment(long int msgno, ATTACH_S *a)
{
#ifdef	_WINDOWS
    (void) msgno;
    (void) a;
    return(GOBLIN_DL_UNSUPPORTED);
#else
    char     *src = NULL, *name, filename[MAILTMPLEN];
    int	      chunk = GOBLIN_MAX_CHUNK, rv;
    long      nbytes;
    unsigned  id;

    if(!(a && a->number && ps_global && ps_global->mail_stream))
      return(GOBLIN_DL_ERROR);

    if(!isatty(STDOUT_FILENO) || !isatty(STDIN_FILENO))
      return(GOBLIN_DL_UNSUPPORTED);

    if(kitty_xfer_available()){
	rv = kitty_xfer_attachment(msgno, a);
	if(rv != GOBLIN_DL_UNSUPPORTED)
	  return(rv);
    }

    if(!goblin_query(&chunk))
      return(GOBLIN_DL_UNSUPPORTED);

    filename[0] = '\0';
    (void) get_filename_parameter(filename, sizeof(filename), a->body, NULL);
    name = goblin_basename(filename[0] ? filename : NULL);

    if(!goblin_extract_part(a, msgno, &src) || !src){
	fs_give((void **) &name);
	return(GOBLIN_DL_ERROR);
    }

    nbytes = name_file_size(src);
    if(nbytes <= 0L){
	our_unlink(src);
	fs_give((void **) &src);
	fs_give((void **) &name);
	return(GOBLIN_DL_ERROR);
    }

    id = goblin_new_id();
    rv = goblin_send_file(id, name, src, nbytes, chunk);

    our_unlink(src);
    fs_give((void **) &src);
    fs_give((void **) &name);
    return(rv);
#endif
}


int
kitty_xfer_send_attachment(long int msgno, ATTACH_S *a)
{
#ifdef	_WINDOWS
    (void) msgno;
    (void) a;
    return(GOBLIN_DL_UNSUPPORTED);
#else
    if(!kitty_xfer_available())
      return(GOBLIN_DL_UNSUPPORTED);

    if(!(a && a->number && ps_global && ps_global->mail_stream))
      return(GOBLIN_DL_ERROR);

    if(!isatty(STDOUT_FILENO) || !isatty(STDIN_FILENO))
      return(GOBLIN_DL_UNSUPPORTED);

    return(kitty_xfer_attachment(msgno, a));
#endif
}


int
kitty_xfer_send_path(char *src, char *destname)
{
#ifdef	_WINDOWS
    (void) src;
    (void) destname;
    return(GOBLIN_DL_UNSUPPORTED);
#else
    char id[64], *base;
    long n;
    static unsigned seq = 1;

    if(!kitty_xfer_available() || !src || !src[0])
      return(GOBLIN_DL_UNSUPPORTED);

    if(!isatty(STDOUT_FILENO) || !isatty(STDIN_FILENO))
      return(GOBLIN_DL_UNSUPPORTED);

    n = name_file_size(src);
    if(n < 0L)
      return(GOBLIN_DL_ERROR);

    base = destname && destname[0] ? destname : last_cmpnt(src);
    if(!base || !*base)
      base = "alpine.bin";

    snprintf(id, sizeof(id), "c%lx-%u", (unsigned long) time(NULL), seq++);
    return(kitty_send_file(id, base, src, n));
#endif
}


void
kitty_notify(char *title, char *body)
{
#ifndef	_WINDOWS
    char buf[1024];

    if(!kitty_xfer_available())
      return;

    snprintf(buf, sizeof(buf), "99;i=n:d=0:p=body;%s%s%s",
	     title && title[0] ? title : "Alpine",
	     (body && body[0]) ? ": " : "",
	     (body && body[0]) ? body : "");
    goblin_write_osc(buf);
#else
    (void) title;
    (void) body;
#endif
}


#ifdef	_WINDOWS
int
kitty_xfer_available(void)
{
    return(0);
}


int
kitty_xfer_receive(char *remote_path, char **outp, long *np)
{
    if(outp)
      *outp = NULL;

    if(np)
      *np = 0;

    (void) remote_path;
    return(GOBLIN_DL_UNSUPPORTED);
}
#endif
