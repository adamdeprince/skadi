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
     Move files between Alpine and the terminal's machine with kitty's
     file transfer protocol (OSC 5113).

     Every escape sequence goes out in one write().  Replies are read
     with poll() and a blocking read of the ready bytes.  While waiting
     for the terminal, ^C or ESC from the user cancels the transfer.

  ====*/


#include "../pith/headers.h"
#include "../pith/goblindl.h"
#include "../pith/state.h"
#include "../pith/conf.h"
#include "../pith/store.h"
#include "../pith/filter.h"
#include "../pith/detach.h"
#include "../pith/status.h"
#include "../pith/busy.h"
#include "../pith/util.h"
#include "../pith/mimedesc.h"
#include "../pith/osdep/temp_nam.h"
#include "../pith/osdep/filesize.h"

#include <stdarg.h>
#ifndef	_WINDOWS
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#endif


#define	KX_CHUNK	4096		/* file bytes per data command	   */
#define	KX_WINDOW	8		/* chunks in flight before we wait */
#define	KX_PERM_MS	60000		/* user answering kitty's dialog   */
#define	KX_REPLY_MS	8000		/* a protocol reply		   */
#define	KX_IDLE_MS	30000		/* silence while data is flowing   */
#define	KX_PROBE_MS	700		/* XTVERSION answer		   */
#define	KX_BYTE_MS	750		/* between bytes inside a reply	   */
#define	KX_LIST_MS	400		/* silence that ends a file list   */
#define	KX_CANCEL_MS	2000		/* CANCELED after our cancel	   */
#define	KX_MAX_FILES	200		/* files pulled from one path	   */
#define	KX_MAX_TOTAL	(256L * 1024L * 1024L)
#define	KX_REPLY_MAX	16384		/* one reply, base64 data and all  */
#define	KX_NAME_MAX	1024
#define	KX_DFLT_DIR	"~/Downloads"


typedef struct kx_msg {
    char	   action[16];
    char	   fid[64];
    char	   pr[64];
    char	   ftype[16];
    char	   status[256];		/* decoded */
    char	   name[KX_NAME_MAX];	/* decoded */
    unsigned long  size;
    unsigned char *data;
    unsigned long  datalen;
} KX_MSG;

typedef struct kx_item {
    char *src;				/* local file to send */
    char *dest;				/* path on the client */
    long  size;
} KX_ITEM;

typedef struct kx_sent_name {
    char		 *name;
    int			  count;
    struct kx_sent_name	 *next;
} KX_SENT_NAME;


static int	kx_avail = -1;		/* -1 unknown, 0 no, 1 yes */
static char	kx_termname[64];
static long	kx_sent, kx_total;
static int	kx_cancelled;
static KX_SENT_NAME *kx_names;


/* internal prototypes */
#ifndef	_WINDOWS
static char	*kx_b64(const unsigned char *, unsigned long);
static int	 kx_b64dec(const char *, unsigned char *, int);
static int	 kx_percent(void);
static int	 kx_is_error(const char *);
static long	 kx_now_ms(void);
static int	 kx_write_raw(const char *, size_t);
static int	 kx_write(const char *);
static int	 kx_writef(const char *, ...);
static int	 kx_getc(int);
static int	 kx_read_reply(char *, int, int, int *);
static int	 kx_parse(char *, const char *, KX_MSG *);
static void	 kx_msg_free(KX_MSG *);
static int	 kx_wait_status(const char *, const char *, const char *, int,
				char *, size_t, unsigned long *);
static void	 kx_cancel(const char *);
static void	 kx_new_id(char *, size_t, int);
static void	 kx_safe_name(char *);
static char	*kx_download_dir(void);
static char	*kx_dest_for(const char *);
static int	 kx_send_session(KX_ITEM *, int, int *);
static int	 kx_stream_file(const char *, const char *, KX_ITEM *, int);
static int	 kx_extract_part(ATTACH_S *, long, char **);
static char	*kx_part_name(ATTACH_S *);
static void	 kx_remote_spec(const char *, char *, size_t);
static int	 kx_receive_one(const char *, const char *, const char *,
				unsigned long, KITTY_FILE_S **);
#endif


void
kitty_free_files(KITTY_FILE_S **listp)
{
    KITTY_FILE_S *f, *next;

    if(!listp)
      return;

    for(f = *listp; f; f = next){
	next = f->next;
	if(f->path)
	  fs_give((void **) &f->path);

	if(f->name)
	  fs_give((void **) &f->name);

	fs_give((void **) &f);
    }

    *listp = NULL;
}


char *
kitty_xfer_terminal(void)
{
    return(kx_termname[0] ? kx_termname : NULL);
}


#ifndef	_WINDOWS

/*----------------------------------------------------------------------
    Base64, both directions.  The encoder never fails on valid input;
    the decoder returns the number of bytes produced or 0 on garbage.
  ----*/
static char *
kx_b64(const unsigned char *src, unsigned long srcl)
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


static int
kx_b64dec(const char *in, unsigned char *out, int outlen)
{
    static const char tbl[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned char d[4];
    int		  n, v, o = 0, pads;
    const char	 *p;

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


static int
kx_percent(void)
{
    if(kx_total <= 0)
      return(0);

    return((int) ((100L * kx_sent) / kx_total));
}


/* kitty error statuses look like EPERM:..., ENOENT:..., or CANCELED */
static int
kx_is_error(const char *st)
{
    if(!st || !st[0])
      return(0);

    if(!strncmp(st, "CANCELED", 8))
      return(1);

    return(st[0] == 'E' && isupper((unsigned char) st[1]));
}


static long
kx_now_ms(void)
{
    struct timespec ts;

    if(clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return((long) time(NULL) * 1000L);

    return((long) ts.tv_sec * 1000L + (long) (ts.tv_nsec / 1000000L));
}


/*
 * Everything we say to the terminal goes through here: one buffer,
 * one write() per escape sequence, after stdio has been flushed, so
 * nothing else (the busy cue included) can land inside a sequence.
 */
static int
kx_write_raw(const char *buf, size_t len)
{
    const char *p = buf;
    ssize_t	n;

    fflush(stdout);
    while(len > 0){
	n = write(STDOUT_FILENO, (char *) p, len);
	if(n < 0){
	    if(errno == EINTR)
	      continue;

	    if(errno == EAGAIN){
		struct pollfd pfd;

		pfd.fd = STDOUT_FILENO;
		pfd.events = POLLOUT;
		(void) poll(&pfd, 1, 1000);
		continue;
	    }

	    return(0);
	}

	p += n;
	len -= (size_t) n;
    }

    return(1);
}


static int
kx_write(const char *body)
{
    size_t bl;
    char  *buf;
    int	   rv;

    if(!body)
      return(0);

    bl = strlen(body);
    buf = (char *) fs_get(bl + 5);
    buf[0] = '\033';
    buf[1] = ']';
    memcpy(buf + 2, body, bl);
    buf[bl + 2] = '\033';
    buf[bl + 3] = '\\';
    rv = kx_write_raw(buf, bl + 4);
    fs_give((void **) &buf);
    return(rv);
}


static int
kx_writef(const char *fmt, ...)
{
    char    body[2048];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    body[sizeof(body) - 1] = '\0';
    return(kx_write(body));
}


/*
 * One byte from the terminal.  Returns the byte, -1 on timeout, -2 when
 * the tty is gone.
 */
static int
kx_getc(int ms)
{
    struct pollfd pfd;
    unsigned char c;
    long	  deadline = kx_now_ms() + ms;
    int		  rv;

    for(;;){
	long left = deadline - kx_now_ms();

	if(left < 0)
	  left = 0;

	pfd.fd = STDIN_FILENO;
	pfd.events = POLLIN;
	pfd.revents = 0;
	rv = poll(&pfd, 1, (int) left);
	if(rv < 0){
	    if(errno == EINTR)
	      continue;

	    return(-2);
	}

	if(rv == 0)
	  return(-1);

	if(pfd.revents & (POLLHUP | POLLERR | POLLNVAL))
	  return(-2);

	rv = (int) read(STDIN_FILENO, &c, 1);
	if(rv == 1)
	  return((int) c);

	if(rv == 0)
	  return(-2);

	if(errno == EINTR || errno == EAGAIN)
	  continue;

	return(-2);
    }
}


/*
 * Wait up to ms for a terminal reply.  OSC and DCS replies are returned
 * without their introducer and terminator; *kind tells them apart (']'
 * or 'P').  Bytes that are not part of a reply are dropped, except that
 * ^C, the Escape key (bare ESC or the kitty CSI u form) set kx_cancelled
 * and return -2.  Returns bytes stored, 0 on timeout, -1 if the tty is
 * gone.  Over-long replies are drained and skipped.
 */
static int
kx_read_reply(char *buf, int buflen, int ms, int *kind)
{
    long deadline = kx_now_ms() + ms;
    int	 c, n, intro, esc, overflow, first = 1;

    if(!buf || buflen < 2)
      return(0);

    for(;;){
	long left = deadline - kx_now_ms();

	/* ms == 0 means one look at what is already here */
	if(left <= 0 && !first)
	  return(0);

	first = 0;
	c = kx_getc(left > 0 ? (int) left : 0);
	if(c == -1)
	  return(0);

	if(c == -2)
	  return(-1);

	if(c == 0x03){
	    kx_cancelled = 1;
	    return(-2);
	}

	if(c != 0x1b)
	  continue;

	c = kx_getc(80);
	if(c == -1){				/* bare ESC: the Escape key */
	    kx_cancelled = 1;
	    return(-2);
	}

	if(c == -2)
	  return(-1);

	if(c == '['){
	    char seq[32];
	    int	 sn = 0;

	    for(;;){
		c = kx_getc(KX_BYTE_MS);
		if(c < 0)
		  break;

		if(sn < (int) sizeof(seq) - 1)
		  seq[sn++] = (char) c;

		if(c >= 0x40 && c <= 0x7e)
		  break;
	    }

	    seq[sn] = '\0';
	    if((!strncmp(seq, "27", 2) && (seq[2] == 'u' || seq[2] == ';'))
	       || (!strncmp(seq, "99", 2) && strstr(seq, ";5") && seq[sn-1] == 'u')
	       || !strcmp(seq, "27;5;99~")){
		kx_cancelled = 1;
		return(-2);
	    }

	    continue;
	}

	if(c != ']' && c != 'P' && c != '_')
	  continue;				/* Alt+key or noise */

	intro = c;
	esc = 0;
	n = 0;
	overflow = 0;
	for(;;){
	    c = kx_getc(KX_BYTE_MS);
	    if(c < 0){				/* cut off mid-reply */
		n = 0;
		break;
	    }

	    if(c == 0x07)
	      break;

	    if(esc){
		esc = 0;
		if(c == '\\')
		  break;

		if(n < buflen - 1)
		  buf[n++] = '\033';
		else
		  overflow = 1;
	    }

	    if(c == 0x1b){
		esc = 1;
		continue;
	    }

	    if(n < buflen - 1)
	      buf[n++] = (char) c;
	    else
	      overflow = 1;
	}

	buf[n] = '\0';
	if(overflow || intro == '_' || n == 0)
	  continue;

	if(kind)
	  *kind = intro;

	return(n);
    }
}


static void
kx_msg_free(KX_MSG *m)
{
    if(!m)
      return;

    if(m->data)
      fs_give((void **) &m->data);

    memset(m, 0, sizeof(*m));
}


/*
 * Parse "5113;k=v;k=v..." into m.  Only messages whose id matches
 * want_id count.  Returns 1 when the message is ours and has an action.
 */
static int
kx_parse(char *s, const char *want_id, KX_MSG *m)
{
    char *p, *semi, *eq, *rawst = NULL, *rawn = NULL, *rawd = NULL;
    int	  id_ok = 0, n;

    if(!m)
      return(0);

    memset(m, 0, sizeof(*m));
    if(!s || !want_id || !want_id[0])
      return(0);

    if(strncmp(s, "5113;", 5) != 0)
      return(0);

    p = s + 5;
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
	    else if(!strcmp(p, "pr") || !strcmp(p, "parent")){
		strncpy(m->pr, eq + 1, sizeof(m->pr) - 1);
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
	n = kx_b64dec(rawst, (unsigned char *) m->status,
		      (int) sizeof(m->status) - 1);
	if(n > 0)
	  m->status[n] = '\0';
	else{
	    strncpy(m->status, rawst, sizeof(m->status) - 1);
	    m->status[sizeof(m->status) - 1] = '\0';
	}
    }

    if(rawn && *rawn){
	n = kx_b64dec(rawn, (unsigned char *) m->name,
		      (int) sizeof(m->name) - 1);
	if(n > 0)
	  m->name[n] = '\0';
    }

    if(rawd && *rawd){
	int maxb = ((int) strlen(rawd) * 3) / 4 + 4;

	m->data = (unsigned char *) fs_get((size_t) maxb);
	n = kx_b64dec(rawd, m->data, maxb);
	if(n > 0)
	  m->datalen = (unsigned long) n;
	else{
	    fs_give((void **) &m->data);
	    if(strlen(rawd) > 3)
	      return(0);			/* garbage where data should be */
	}
    }

    return(id_ok && m->action[0]);
}


/*
 * Wait for a status reply for id (and fid when given; when fid is NULL
 * only session-level replies, those without a fid, count).
 *
 *  1  the wanted status arrived (or any status when want is NULL)
 *  0  timeout
 * -1  an error status arrived (copied to st)
 * -2  the user cancelled
 * -3  the tty is gone
 *
 * Other statuses (PROGRESS while waiting for OK) are skipped; the last
 * PROGRESS size is left in *sizep so a caller can track acked bytes.
 */
static int
kx_wait_status(const char *id, const char *fid, const char *want, int ms,
	       char *st, size_t stlen, unsigned long *sizep)
{
    char   buf[KX_REPLY_MAX];
    KX_MSG m;
    long   deadline = kx_now_ms() + ms;
    int	   n, kind, first = 1;

    if(st && stlen)
      st[0] = '\0';

    for(;;){
	long left = deadline - kx_now_ms();

	if(left <= 0 && !first)
	  return(0);

	first = 0;
	kind = 0;
	n = kx_read_reply(buf, sizeof(buf), left > 0 ? (int) left : 0, &kind);
	if(n == -2)
	  return(-2);

	if(n == -1)
	  return(-3);

	if(n == 0)
	  return(0);

	if(kind != ']' || !kx_parse(buf, id, &m))
	  continue;

	if(strcmp(m.action, "status") != 0){
	    kx_msg_free(&m);
	    continue;
	}

	if(fid && fid[0] && !m.fid[0]){
	    /* a session-level status while waiting on one file */
	    if(kx_is_error(m.status)){
		if(st && stlen){
		    strncpy(st, m.status, stlen - 1);
		    st[stlen - 1] = '\0';
		}

		kx_msg_free(&m);
		return(-1);
	    }

	    kx_msg_free(&m);
	    continue;
	}

	if((fid && fid[0] && strcmp(m.fid, fid) != 0) || (!fid && m.fid[0])){
	    kx_msg_free(&m);
	    continue;
	}

	if(st && stlen){
	    strncpy(st, m.status, stlen - 1);
	    st[stlen - 1] = '\0';
	}

	if(sizep && !strncmp(m.status, "PROGRESS", 8))
	  *sizep = m.size;

	if(kx_is_error(m.status)){
	    kx_msg_free(&m);
	    return(-1);
	}

	if(!want || !want[0] || !strncmp(m.status, want, strlen(want))){
	    if(sizep && !strncmp(m.status, "OK", 2) && m.size)
	      *sizep = m.size;

	    kx_msg_free(&m);
	    return(1);
	}

	kx_msg_free(&m);
    }
}


/*
 * Tell kitty to drop the session and wait for its CANCELED so buffered
 * replies do not end up on the screen.  Ignores the cancel keys.
 */
static void
kx_cancel(const char *id)
{
    char   buf[KX_REPLY_MAX], st[64];
    KX_MSG m;
    long   deadline;
    int	   n, kind;

    if(!id || !id[0])
      return;

    (void) kx_writef("5113;ac=cancel;id=%s", id);
    deadline = kx_now_ms() + KX_CANCEL_MS;
    for(;;){
	long left = deadline - kx_now_ms();

	if(left <= 0)
	  break;

	kind = 0;
	n = kx_read_reply(buf, sizeof(buf), (int) left, &kind);
	if(n == -1 || n == 0)
	  break;

	if(n < 0 || kind != ']' || !kx_parse(buf, id, &m))
	  continue;

	strncpy(st, m.status, sizeof(st) - 1);
	st[sizeof(st) - 1] = '\0';
	kx_msg_free(&m);
	if(!strncmp(st, "CANCELED", 8))
	  break;
    }
}


static void
kx_new_id(char *id, size_t idlen, int prefix)
{
    static unsigned seq = 1;

    snprintf(id, idlen, "%c%lx-%u", prefix, (unsigned long) time(NULL), seq++);
    if(seq == 0)
      seq = 1;
}


/* a name safe to put on the client: no separators, no control bytes */
static void
kx_safe_name(char *s)
{
    if(!s)
      return;

    for(; *s; s++){
	if(*s == '/' || *s == '\\' || *s == ':' || *s == '*' || *s == '?'
	   || *s == '"' || *s == '<' || *s == '>' || *s == '|'
	   || (unsigned char) *s < 0x20 || *s == 0x7f)
	  *s = '_';
    }
}


static char *
kx_download_dir(void)
{
    static char dir[KX_NAME_MAX];
    char       *v;
    size_t	l;

    v = (ps_global && ps_global->VAR_KITTY_DOWNLOAD_DIR
	 && ps_global->VAR_KITTY_DOWNLOAD_DIR[0])
	  ? ps_global->VAR_KITTY_DOWNLOAD_DIR : KX_DFLT_DIR;

    strncpy(dir, v, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    while((l = strlen(dir)) > 1 && dir[l - 1] == '/')
      dir[l - 1] = '\0';

    return(dir);
}


/*
 * Destination path on the client for an attachment called name.
 * kitty overwrites silently and Alpine cannot see the client's disk,
 * so unless the user asked for overwrites, a name that was already
 * sent this session gets a Finder-style " (2)", " (3)" suffix.
 */
static char *
kx_dest_for(const char *name)
{
    KX_SENT_NAME *sn;
    char	  base[KX_NAME_MAX], *dot, *dest;
    size_t	  need;
    int		  n = 1;

    strncpy(base, (name && name[0]) ? name : "attachment", sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';
    kx_safe_name(base);

    if(F_OFF(F_KITTY_DL_OVERWRITE, ps_global)){
	for(sn = kx_names; sn; sn = sn->next)
	  if(!strcmp(sn->name, base))
	    break;

	if(sn)
	  n = ++sn->count;
	else{
	    sn = (KX_SENT_NAME *) fs_get(sizeof(*sn));
	    sn->name = cpystr(base);
	    sn->count = 1;
	    sn->next = kx_names;
	    kx_names = sn;
	}
    }

    need = strlen(kx_download_dir()) + strlen(base) + 32;
    dest = (char *) fs_get(need);
    if(n > 1){
	dot = strrchr(base, '.');
	if(dot && dot != base && !strchr(dot, ' '))
	  snprintf(dest, need, "%s/%.*s (%d)%s", kx_download_dir(),
		   (int) (dot - base), base, n, dot);
	else
	  snprintf(dest, need, "%s/%s (%d)", kx_download_dir(), base, n);
    }
    else
      snprintf(dest, need, "%s/%s", kx_download_dir(), base);

    return(dest);
}


/*
 * Is there a kitty at the other end of the tty?  Asked once per
 * session.  tmux and screen do not pass OSC 5113 through, so they are
 * refused outright; otherwise XTVERSION has to name kitty.
 */
int
kitty_xfer_available(void)
{
    char buf[256];
    int	 n, kind = 0;

    if(kx_avail >= 0)
      return(kx_avail);

    kx_avail = 0;
    kx_termname[0] = '\0';
    if(!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
      return(0);

    if(getenv("TMUX") || getenv("STY"))
      return(0);

    if(!kx_write_raw("\033[>q", 4))
      return(0);

    n = kx_read_reply(buf, sizeof(buf), KX_PROBE_MS, &kind);
    if(n > 0 && kind == 'P' && !strncmp(buf, ">|", 2)){
	strncpy(kx_termname, buf + 2, sizeof(kx_termname) - 1);
	kx_termname[sizeof(kx_termname) - 1] = '\0';
	if(!strncmp(kx_termname, "kitty", 5))
	  kx_avail = 1;
    }
    else if(n == 0 && getenv("KITTY_WINDOW_ID")){
	/* a local kitty too old to answer XTVERSION */
	strncpy(kx_termname, "kitty", sizeof(kx_termname) - 1);
	kx_avail = 1;
    }

    kx_cancelled = 0;
    return(kx_avail);
}


/*----------------------------------------------------------------------
    Sending: one session, any number of files, one permission prompt.
  ----*/

/* stream one file's data for fid; returns a GOBLIN_DL_ code */
static int
kx_stream_file(const char *id, const char *fid, KX_ITEM *it, int busy)
{
    FILE	  *fp;
    unsigned char  raw[KX_CHUNK];
    char	  *b64, *body, st[256];
    size_t	   n, blen;
    unsigned long  acked = 0, sent = 0;
    int		   ended = 0, got;

    fp = our_fopen(it->src, "rb");
    if(!fp)
      return(GOBLIN_DL_ERROR);

    blen = 128 + strlen(id) + strlen(fid) + (KX_CHUNK * 4) / 3 + 8;
    body = (char *) fs_get(blen);

    for(;;){
	n = fread(raw, 1, sizeof(raw), fp);
	if(n == 0){
	    if(!ended){
		snprintf(body, blen, "5113;ac=end_data;id=%s;fid=%s", id, fid);
		if(!kx_write(body)){
		    fs_give((void **) &body);
		    fclose(fp);
		    return(GOBLIN_DL_ERROR);
		}

		ended = 1;
	    }

	    break;
	}

	if(ended)
	  break;				/* the file grew; ignore the rest */

	b64 = kx_b64(raw, (unsigned long) n);
	if(!b64){
	    fs_give((void **) &body);
	    fclose(fp);
	    return(GOBLIN_DL_ERROR);
	}

	sent += n;
	if(it->size >= 0 && sent >= (unsigned long) it->size){
	    snprintf(body, blen, "5113;ac=end_data;id=%s;fid=%s;d=%s",
		     id, fid, b64);
	    ended = 1;
	}
	else
	  snprintf(body, blen, "5113;ac=data;id=%s;fid=%s;d=%s", id, fid, b64);

	fs_give((void **) &b64);
	if(!kx_write(body)){
	    fs_give((void **) &body);
	    fclose(fp);
	    return(GOBLIN_DL_ERROR);
	}

	kx_sent = (long) sent;
	ALARM_BLIP();

	/*
	 * Keep at most KX_WINDOW chunks unacknowledged.  A zero wait just
	 * drains whatever replies are already here, so an error or a ^C
	 * is noticed without stalling the pipeline.
	 */
	for(;;){
	    int need_wait = (sent - acked) >= (unsigned long) (KX_WINDOW * KX_CHUNK);

	    got = kx_wait_status(id, fid, "PROGRESS",
				 need_wait ? KX_REPLY_MS : 0,
				 st, sizeof(st), &acked);
	    if(got == 1 && need_wait)
	      continue;

	    if(got == 1 || got == 0){
		if(need_wait && got == 0){
		    fs_give((void **) &body);
		    fclose(fp);
		    return(GOBLIN_DL_ERROR);	/* the terminal stopped acking */
		}

		break;
	    }

	    fs_give((void **) &body);
	    fclose(fp);
	    return(got == -2 ? GOBLIN_DL_CANCEL : GOBLIN_DL_ERROR);
	}
    }

    fs_give((void **) &body);
    fclose(fp);

    got = kx_wait_status(id, fid, "OK", KX_IDLE_MS, st, sizeof(st), &acked);
    if(got == 1)
      return(GOBLIN_DL_OK);

    if(got == -2)
      return(GOBLIN_DL_CANCEL);

    return(GOBLIN_DL_ERROR);
}


static int
kx_send_session(KX_ITEM *items, int n, int *nsent)
{
    char  id[64], fid[32], st[256], *b64, msg[128];
    int	  i, got, busy, rv = GOBLIN_DL_OK;

    if(nsent)
      *nsent = 0;

    if(!items || n <= 0)
      return(GOBLIN_DL_ERROR);

    kx_cancelled = 0;
    kx_new_id(id, sizeof(id), 'a');
    if(!kx_writef("5113;ac=send;id=%s", id))
      return(GOBLIN_DL_ERROR);

    busy = busy_cue(_("Answer kitty's transfer prompt (^C cancels)"), NULL, 0);
    got = kx_wait_status(id, NULL, "OK", KX_PERM_MS, st, sizeof(st), NULL);
    if(busy)
      cancel_busy_cue(-1);

    if(got == -2){
	kx_cancel(id);
	return(GOBLIN_DL_CANCEL);
    }

    if(got == -1)
      return(!strncmp(st, "EPERM", 5) ? GOBLIN_DL_CANCEL : GOBLIN_DL_ERROR);

    if(got == 0){
	kx_cancel(id);
	return(GOBLIN_DL_UNSUPPORTED);
    }

    if(got < 0)
      return(GOBLIN_DL_ERROR);

    for(i = 0; i < n; i++){
	snprintf(fid, sizeof(fid), "f%d", i + 1);
	b64 = kx_b64((unsigned char *) items[i].dest,
		     (unsigned long) strlen(items[i].dest));
	if(!b64){
	    rv = GOBLIN_DL_ERROR;
	    break;
	}

	got = kx_writef("5113;ac=file;id=%s;fid=%s;n=%s;sz=%ld;ft=regular",
			id, fid, b64, items[i].size);
	fs_give((void **) &b64);
	if(!got){
	    rv = GOBLIN_DL_ERROR;
	    break;
	}

	got = kx_wait_status(id, fid, "STARTED", KX_REPLY_MS, st, sizeof(st), NULL);
	if(got != 1){
	    rv = (got == -2) ? GOBLIN_DL_CANCEL : GOBLIN_DL_ERROR;
	    break;
	}

	kx_sent = 0;
	kx_total = items[i].size > 0 ? items[i].size : 1;
	if(n > 1)
	  snprintf(msg, sizeof(msg), _("Sending file %d of %d to the client"), i + 1, n);
	else
	  snprintf(msg, sizeof(msg), _("Sending to the client"));

	busy = busy_cue(msg, kx_percent, 0);
	rv = kx_stream_file(id, fid, &items[i], busy);
	if(busy)
	  cancel_busy_cue(-1);

	if(rv != GOBLIN_DL_OK)
	  break;

	if(nsent)
	  (*nsent)++;
    }

    if(rv != GOBLIN_DL_OK){
	kx_cancel(id);
	return(rv);
    }

    (void) kx_writef("5113;ac=finish;id=%s", id);
    (void) kx_wait_status(id, NULL, "OK", 2000, st, sizeof(st), NULL);
    return(GOBLIN_DL_OK);
}


/* decode a part into a temp file; caller unlinks and frees */
static int
kx_extract_part(ATTACH_S *a, long int msgno, char **pathp)
{
    STORE_S *so;
    gf_o_t   pc;
    char    *err, *path;

    if(!pathp)
      return(0);

    *pathp = NULL;
    if(!(a && a->number && ps_global && ps_global->mail_stream))
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

    if(err || name_file_size(path) < 0L){
	our_unlink(path);
	fs_give((void **) &path);
	return(0);
    }

    *pathp = path;
    return(1);
}


/* the name a part should have on the client, always non-empty */
static char *
kx_part_name(ATTACH_S *a)
{
    char  filename[MAILTMPLEN], *p, *name;

    filename[0] = '\0';
    if(a && a->body)
      (void) get_filename_parameter(filename, sizeof(filename), a->body, NULL);

    p = filename[0] ? last_cmpnt(filename) : NULL;
    if(!p)
      p = filename;

    while(*p == '/' || *p == '\\')
      p++;

    if(!*p){
	const char *ext = (a && a->body && a->body->type == TYPETEXT)
			    ? ".txt" : ".bin";

	snprintf(filename, sizeof(filename), "attachment-%s%s",
		 (a && a->number) ? a->number : "1", ext);
	p = filename;
    }

    name = cpystr(p);
    kx_safe_name(name);
    return(name);
}


int
kitty_xfer_send_attachments(long int msgno, ATTACH_S **list, int n, int *nsent)
{
    KX_ITEM *items;
    char    *name;
    int	     i, m = 0, rv;

    if(nsent)
      *nsent = 0;

    if(!kitty_xfer_available())
      return(GOBLIN_DL_UNSUPPORTED);

    if(!list || n <= 0 || !ps_global || !ps_global->mail_stream)
      return(GOBLIN_DL_ERROR);

    items = (KX_ITEM *) fs_get((size_t) n * sizeof(KX_ITEM));
    memset(items, 0, (size_t) n * sizeof(KX_ITEM));
    for(i = 0; i < n; i++){
	if(!list[i] || !list[i]->number)
	  continue;

	if(!kx_extract_part(list[i], msgno, &items[m].src))
	  continue;

	name = kx_part_name(list[i]);
	items[m].dest = kx_dest_for(name);
	fs_give((void **) &name);
	items[m].size = name_file_size(items[m].src);
	m++;
    }

    rv = m ? kx_send_session(items, m, nsent) : GOBLIN_DL_ERROR;

    for(i = 0; i < m; i++){
	if(items[i].src){
	    our_unlink(items[i].src);
	    fs_give((void **) &items[i].src);
	}

	if(items[i].dest)
	  fs_give((void **) &items[i].dest);
    }

    fs_give((void **) &items);
    return(rv);
}


int
kitty_xfer_send_attachment(long int msgno, ATTACH_S *a)
{
    ATTACH_S *list[1];

    list[0] = a;
    return(kitty_xfer_send_attachments(msgno, list, 1, NULL));
}


int
kitty_xfer_send_path(char *src, char *destname)
{
    KX_ITEM it;
    char   *base;
    int	    rv;

    if(!kitty_xfer_available())
      return(GOBLIN_DL_UNSUPPORTED);

    if(!src || !src[0])
      return(GOBLIN_DL_ERROR);

    it.size = name_file_size(src);
    if(it.size < 0L)
      return(GOBLIN_DL_ERROR);

    base = (destname && destname[0]) ? destname : last_cmpnt(src);
    if(!base || !*base)
      base = src;

    it.src = src;
    it.dest = kx_dest_for(base);
    rv = kx_send_session(&it, 1, NULL);
    fs_give((void **) &it.dest);
    return(rv);
}


/*----------------------------------------------------------------------
    Receiving: ask for a path on the client, take every regular file
    kitty lists for it (a directory yields its contents) into temp files.
  ----*/

/* turn what the user typed or pasted into a path spec for kitty */
static void
kx_remote_spec(const char *in, char *out, size_t outlen)
{
    char  *p;
    size_t l;

    if(!out || outlen < 3)
      return;

    out[0] = '\0';
    if(!in)
      return;

    while(*in == ' ' || *in == '\t')
      in++;

    strncpy(out, in, outlen - 1);
    out[outlen - 1] = '\0';
    l = strlen(out);
    while(l > 0 && (out[l-1] == ' ' || out[l-1] == '\t' || out[l-1] == '\r'
		    || out[l-1] == '\n'))
      out[--l] = '\0';

    if(l >= 2 && ((out[0] == '"' && out[l-1] == '"')
		  || (out[0] == '\'' && out[l-1] == '\''))){
	out[l-1] = '\0';
	memmove(out, out + 1, l - 1);
    }

    for(p = out; *p; p++)
      if(*p == '\\')
	*p = '/';

    if(!out[0] || out[0] == '/' || (out[0] == '~' && (out[1] == '/' || !out[1])))
      return;

    {
	char tmp[KX_NAME_MAX];

	strncpy(tmp, out, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = '\0';
	snprintf(out, outlen, "~/%s", tmp);
    }
}


/* pull one listed file into a temp file and append it to *listp */
static int
kx_receive_one(const char *id, const char *fid, const char *abspath,
	       unsigned long expected, KITTY_FILE_S **listp)
{
    char	  buf[KX_REPLY_MAX], *b64, *tmp, *base;
    KX_MSG	  k;
    FILE	 *fp;
    KITTY_FILE_S *f, **fp2;
    unsigned long written = 0;
    long	  deadline;
    int		  n, kind, busy, done = 0, rv = GOBLIN_DL_ERROR;

    b64 = kx_b64((unsigned char *) abspath, (unsigned long) strlen(abspath));
    if(!b64)
      return(GOBLIN_DL_ERROR);

    tmp = temp_nam(NULL, "ku");
    if(!tmp){
	fs_give((void **) &b64);
	return(GOBLIN_DL_ERROR);
    }

    fp = our_fopen(tmp, "wb");
    if(!fp){
	fs_give((void **) &b64);
	our_unlink(tmp);
	fs_give((void **) &tmp);
	return(GOBLIN_DL_ERROR);
    }

    n = kx_writef("5113;ac=file;id=%s;fid=%s;n=%s", id, fid, b64);
    fs_give((void **) &b64);
    if(!n){
	fclose(fp);
	our_unlink(tmp);
	fs_give((void **) &tmp);
	return(GOBLIN_DL_ERROR);
    }

    kx_sent = 0;
    kx_total = expected > 0 ? (long) expected : 1;
    busy = busy_cue(_("Receiving from the client (^C cancels)"), kx_percent, 0);

    deadline = kx_now_ms() + KX_IDLE_MS;
    while(!done){
	long left = deadline - kx_now_ms();

	if(left <= 0)
	  break;				/* the client went quiet */

	kind = 0;
	n = kx_read_reply(buf, sizeof(buf), (int) left, &kind);
	if(n == -2){
	    rv = GOBLIN_DL_CANCEL;
	    break;
	}

	if(n == -1 || n == 0)
	  break;

	if(kind != ']' || !kx_parse(buf, id, &k))
	  continue;

	if(!strcmp(k.action, "data") || !strcmp(k.action, "end_data")){
	    if(strcmp(k.fid, fid) != 0){
		kx_msg_free(&k);
		continue;
	    }

	    if(k.data && k.datalen){
		if(fwrite(k.data, 1, (size_t) k.datalen, fp) != k.datalen){
		    kx_msg_free(&k);
		    break;
		}

		written += k.datalen;
		kx_sent = (long) written;
	    }

	    if(!strcmp(k.action, "end_data"))
	      done = 1;

	    deadline = kx_now_ms() + KX_IDLE_MS;
	    ALARM_BLIP();
	}
	else if(!strcmp(k.action, "status")
		&& (!k.fid[0] || !strcmp(k.fid, fid))
		&& kx_is_error(k.status)){
	    kx_msg_free(&k);
	    break;
	}

	kx_msg_free(&k);
    }

    if(busy)
      cancel_busy_cue(-1);

    if(fflush(fp) != 0 || fclose(fp) != 0)
      done = 0;

    if(done && (expected == 0 || written == expected)){
	base = last_cmpnt((char *) abspath);
	if(!base || !*base)
	  base = (char *) abspath;

	f = (KITTY_FILE_S *) fs_get(sizeof(*f));
	memset(f, 0, sizeof(*f));
	f->path = tmp;
	f->name = cpystr(base);
	kx_safe_name(f->name);
	f->size = (long) written;
	for(fp2 = listp; *fp2; fp2 = &(*fp2)->next)
	  ;

	*fp2 = f;
	return(GOBLIN_DL_OK);
    }

    our_unlink(tmp);
    fs_give((void **) &tmp);
    return(rv);
}


int
kitty_xfer_receive(char *remote_path, KITTY_FILE_S **listp)
{
    char	  id[64], spec[KX_NAME_MAX], buf[KX_REPLY_MAX], *b64, st[256];
    char	  fid[32];
    KX_MSG	  k;
    struct {
	char	     *name;
	unsigned long size;
    }		 *entries = NULL;
    unsigned long total = 0;
    long	  deadline, quiet_until;
    int		  n, kind, got, busy, nent = 0, alloc = 0, i, rv, ok_seen = 0;

    if(listp)
      *listp = NULL;

    if(!listp || !remote_path || !remote_path[0])
      return(GOBLIN_DL_ERROR);

    if(!kitty_xfer_available())
      return(GOBLIN_DL_UNSUPPORTED);

    kx_remote_spec(remote_path, spec, sizeof(spec));
    if(!spec[0])
      return(GOBLIN_DL_ERROR);

    kx_cancelled = 0;
    kx_new_id(id, sizeof(id), 'b');
    b64 = kx_b64((unsigned char *) spec, (unsigned long) strlen(spec));
    if(!b64)
      return(GOBLIN_DL_ERROR);

    got = kx_writef("5113;ac=receive;id=%s;sz=1", id)
	  && kx_writef("5113;ac=file;id=%s;fid=r0;n=%s", id, b64);
    fs_give((void **) &b64);
    if(!got)
      return(GOBLIN_DL_ERROR);

    busy = busy_cue(_("Answer kitty's transfer prompt (^C cancels)"), NULL, 0);

    /*
     * kitty answers the permission with a session OK and then lists
     * what the path holds, one ac=file per entry, with nothing marking
     * the end of the list.  Collect until the list has been quiet.
     */
    deadline = kx_now_ms() + KX_PERM_MS;
    quiet_until = 0;
    rv = GOBLIN_DL_ERROR;
    for(;;){
	long now = kx_now_ms(), left;

	if(ok_seen){
	    left = (nent ? quiet_until : deadline) - now;
	    if(left <= 0){
		if(nent || now >= deadline){
		    rv = nent ? GOBLIN_DL_OK : GOBLIN_DL_ERROR;
		    break;
		}
	    }
	}
	else
	  left = deadline - now;

	if(left <= 0){
	    rv = ok_seen ? GOBLIN_DL_ERROR : GOBLIN_DL_UNSUPPORTED;
	    break;
	}

	kind = 0;
	n = kx_read_reply(buf, sizeof(buf), (int) left, &kind);
	if(n == -2){
	    rv = GOBLIN_DL_CANCEL;
	    break;
	}

	if(n == -1){
	    rv = GOBLIN_DL_ERROR;
	    break;
	}

	if(n == 0)
	  continue;

	if(kind != ']' || !kx_parse(buf, id, &k))
	  continue;

	if(!strcmp(k.action, "status")){
	    if(kx_is_error(k.status)){
		rv = !strncmp(k.status, "EPERM", 5) ? GOBLIN_DL_CANCEL
						    : GOBLIN_DL_ERROR;
		kx_msg_free(&k);
		break;
	    }

	    if(!k.fid[0] && !strncmp(k.status, "OK", 2)){
		ok_seen = 1;
		if(busy){
		    cancel_busy_cue(-1);
		    busy = 0;
		}

		deadline = kx_now_ms() + KX_REPLY_MS;
	    }
	}
	else if(!strcmp(k.action, "file")){
	    ok_seen = 1;
	    if(busy){
		cancel_busy_cue(-1);
		busy = 0;
	    }

	    if(k.name[0] && !strcmp(k.ftype[0] ? k.ftype : "regular", "regular")
	       && nent < KX_MAX_FILES && total + k.size <= (unsigned long) KX_MAX_TOTAL){
		if(nent >= alloc){
		    alloc = alloc ? alloc * 2 : 8;
		    fs_resize((void **) &entries, (size_t) alloc * sizeof(*entries));
		}

		entries[nent].name = cpystr(k.name);
		entries[nent].size = k.size;
		total += k.size;
		nent++;
	    }

	    quiet_until = kx_now_ms() + KX_LIST_MS;
	}

	kx_msg_free(&k);
    }

    if(busy)
      cancel_busy_cue(-1);

    if(rv != GOBLIN_DL_OK){
	kx_cancel(id);
	goto done;
    }

    for(i = 0; i < nent; i++){
	snprintf(fid, sizeof(fid), "g%d", i + 1);
	rv = kx_receive_one(id, fid, entries[i].name, entries[i].size, listp);
	if(rv != GOBLIN_DL_OK)
	  break;
    }

    if(rv != GOBLIN_DL_OK){
	kx_cancel(id);
	for(; *listp; ){
	    KITTY_FILE_S *f = *listp;

	    if(f->path)
	      our_unlink(f->path);

	    *listp = f->next;
	    f->next = NULL;
	    kitty_free_files(&f);
	}
    }
    else{
	(void) kx_writef("5113;ac=finish;id=%s", id);
	(void) kx_wait_status(id, NULL, "OK", 2000, st, sizeof(st), NULL);
    }

done:
    for(i = 0; i < nent; i++)
      fs_give((void **) &entries[i].name);

    if(entries)
      fs_give((void **) &entries);

    return(rv);
}


/* desktop notification through kitty (OSC 99); text is plain, no controls */
void
kitty_notify(char *title, char *body)
{
    char  buf[1024], *p;
    int	  n;

    if(!kitty_xfer_available())
      return;

    n = snprintf(buf, sizeof(buf), "99;i=n:d=0:p=body;%s%s%s",
		 title && title[0] ? title : "Alpine",
		 (body && body[0]) ? ": " : "",
		 (body && body[0]) ? body : "");
    if(n < 0)
      return;

    buf[sizeof(buf) - 1] = '\0';
    p = strchr(buf + 3, ';');		/* the metadata/payload separator */
    for(p = p ? p + 1 : buf; *p; p++)
      if((unsigned char) *p < 0x20 || *p == 0x7f || *p == ';')
	*p = ' ';

    (void) kx_write(buf);
}

#else /* _WINDOWS */

int
kitty_xfer_available(void)
{
    return(0);
}


int
kitty_xfer_send_attachments(long int msgno, ATTACH_S **list, int n, int *nsent)
{
    (void) msgno;
    (void) list;
    (void) n;
    if(nsent)
      *nsent = 0;

    return(GOBLIN_DL_UNSUPPORTED);
}


int
kitty_xfer_send_attachment(long int msgno, ATTACH_S *a)
{
    (void) msgno;
    (void) a;
    return(GOBLIN_DL_UNSUPPORTED);
}


int
kitty_xfer_send_path(char *src, char *destname)
{
    (void) src;
    (void) destname;
    return(GOBLIN_DL_UNSUPPORTED);
}


int
kitty_xfer_receive(char *remote_path, KITTY_FILE_S **listp)
{
    (void) remote_path;
    if(listp)
      *listp = NULL;

    return(GOBLIN_DL_UNSUPPORTED);
}


void
kitty_notify(char *title, char *body)
{
    (void) title;
    (void) body;
}

#endif /* _WINDOWS */
