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
 * Program:	Write-on-exit offline cache for remote folders
 *
 * RAM holds envelopes, flags, headers, and any bodies already fetched.
 * Disk is read once at startup and written once at exit (if dirty).
 */

#include "../pith/headers.h"
#include "../pith/offcache.h"
#include "../pith/state.h"
#include "../pith/conf.h"
#include "../pith/status.h"
#include "../pith/store.h"
#include "../pith/util.h"
#include "../pith/stream.h"
#include "../pith/osdep/creatdir.h"
#include "../pith/osdep/rename.h"
#include "../pith/charconv/filesys.h"

#ifndef	_WINDOWS
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#endif
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>


#define	OC_MAGIC		"AOC1\n"
#define	OC_MAX_BODY		(32L * 1024L * 1024L)
#define	OC_STUB \
"\r\n[Alpine: this message body is not in the offline cache.\r\n" \
" Re-open the folder while connected to fetch it.]\r\n"

#define	OCF_SEEN		0x01
#define	OCF_DELETED		0x02
#define	OCF_FLAGGED		0x04
#define	OCF_ANSWERED		0x08
#define	OCF_DRAFT		0x10
#define	OCF_RECENT		0x20


typedef struct oc_msg {
    unsigned long uid;
    unsigned long size;
    unsigned long flags;
    unsigned long user_flags;
    unsigned int  day:5, month:4, year:7, hours:5, minutes:6, seconds:6;
    char	 *hdr;
    unsigned long hdrlen;
    char	 *body;
    unsigned long bodylen;
    unsigned	  flags_dirty:1;
} OC_MSG;

typedef struct oc_folder {
    char	 *mailbox;
    unsigned long uidvalidity;
    unsigned long uidlast;
    unsigned long n;
    unsigned long alloc;
    OC_MSG	 *msgs;
    unsigned long body_bytes;
    unsigned	  dirty:1;
    struct oc_folder *next;
} OC_FOLDER;

typedef struct oc_local {
    OC_FOLDER	 *folder;
    char	 *scratch;
    unsigned long scratchlen;
} OCLOCAL;


static OC_FOLDER *oc_folders = NULL;
static char	  oc_dir[MAILTMPLEN];
static int	  oc_dir_ok = 0;
static int	  oc_live = 0;
static MAILSTREAM offcacheproto;

#define	LOCAL	((OCLOCAL *) stream->local)


/* driver dispatch */
DRIVER *offcache_valid(char *);
void   *offcache_parameters(long, void *);
void	offcache_scan(MAILSTREAM *, char *, char *, char *);
void	offcache_list(MAILSTREAM *, char *, char *);
void	offcache_lsub(MAILSTREAM *, char *, char *);
long	offcache_create(MAILSTREAM *, char *);
long	offcache_delete(MAILSTREAM *, char *);
long	offcache_rename(MAILSTREAM *, char *, char *);
MAILSTREAM *offcache_open(MAILSTREAM *);
void	offcache_close(MAILSTREAM *, long);
char   *offcache_header(MAILSTREAM *, unsigned long, unsigned long *, long);
long	offcache_text(MAILSTREAM *, unsigned long, STRING *, long);
void	offcache_flagmsg(MAILSTREAM *, MESSAGECACHE *);
long	offcache_ping(MAILSTREAM *);
void	offcache_check(MAILSTREAM *);
long	offcache_expunge(MAILSTREAM *, char *, long);
long	offcache_copy(MAILSTREAM *, char *, char *, long);
long	offcache_append(MAILSTREAM *, char *, append_t, void *);


DRIVER offcachedriver = {
    "offcache",
    DR_LOCAL | DR_MAIL | DR_NONEWMAIL,
    (DRIVER *) NIL,
    offcache_valid,
    offcache_parameters,
    offcache_scan,
    offcache_list,
    offcache_lsub,
    NIL, NIL,
    offcache_create,
    offcache_delete,
    offcache_rename,
    mail_status_default,
    offcache_open,
    offcache_close,
    NIL, NIL, NIL, NIL,
    offcache_header,
    offcache_text,
    NIL, NIL, NIL,
    NIL,
    offcache_flagmsg,
    NIL, NIL, NIL,
    offcache_ping,
    offcache_check,
    offcache_expunge,
    offcache_copy,
    offcache_append,
    NIL, NIL
};


static void	 oc_free_msg(OC_MSG *);
static OC_FOLDER *oc_find(char *);
static OC_FOLDER *oc_get(char *);
static OC_MSG	 *oc_find_uid(OC_FOLDER *, unsigned long);
static OC_MSG	 *oc_add_msg(OC_FOLDER *);
static unsigned	 oc_hash(char *);
static void	 oc_path_for(char *, size_t, char *);
static char	 *oc_dup(char *, unsigned long);
static char	 *oc_header_from_env(ENVELOPE *, unsigned long *);
static void	 oc_copy_elt_flags(OC_MSG *, MESSAGECACHE *);
static void	 oc_apply_flags(MESSAGECACHE *, OC_MSG *);
static int	 oc_write_folder(OC_FOLDER *);
static int	 oc_read_folder(char *);
static int	 oc_write_u32(FILE *, unsigned long);
static int	 oc_read_u32(FILE *, unsigned long *);
static int	 oc_write_bytes(FILE *, char *, unsigned long);
static char	 *oc_read_bytes(FILE *, unsigned long);
static void	 oc_so_addr(STORE_S *, char *, ADDRESS *);


int
offcache_enabled(void)
{
    if(!ps_global)
      return(0);

    return(F_ON(F_OFFLINE_CACHE, ps_global));
}


int
offcache_is_name(char *mailbox)
{
    return(mailbox && !strncmp(mailbox, OFFCACHE_PREFIX,
			       sizeof(OFFCACHE_PREFIX) - 1)
	   && mailbox[sizeof(OFFCACHE_PREFIX) - 1]);
}


int
offcache_is_stream(MAILSTREAM *stream)
{
    return(stream && stream->dtb && stream->dtb->name
	   && !strcmp(stream->dtb->name, "offcache"));
}


int
offcache_has_folder(char *mailbox)
{
    OC_FOLDER *f;

    if(!mailbox || !offcache_enabled())
      return(0);

    if(offcache_is_name(mailbox))
      mailbox += sizeof(OFFCACHE_PREFIX) - 1;

    f = oc_find(mailbox);
    return(f && f->n > 0);
}


int
offcache_network_ok(void)
{
    return(oc_live);
}


void
offcache_note_live(void)
{
    oc_live = 1;
}


char *
offcache_name_for(char *mailbox)
{
    char *s;
    size_t n;

    if(!mailbox)
      return(NULL);

    if(offcache_is_name(mailbox))
      return(cpystr(mailbox));

    n = sizeof(OFFCACHE_PREFIX) + strlen(mailbox);
    s = (char *) fs_get(n);
    snprintf(s, n, "%s%s", OFFCACHE_PREFIX, mailbox);
    return(s);
}


char *
offcache_real_name(char *mailbox)
{
    if(offcache_is_name(mailbox))
      return(mailbox + sizeof(OFFCACHE_PREFIX) - 1);

    return(mailbox);
}


int
offcache_skip_remote_probe(char *mbox)
{
    if(!offcache_enabled() || oc_live || !mbox || !mbox[0])
      return(0);

    if(IS_REMOTE(mbox))
      return(1);

    return(0);
}


void
offcache_link_driver(void)
{
    mail_link(&offcachedriver);
    offcacheproto.dtb = &offcachedriver;
}


void
offcache_init(void)
{
    char *home;

    oc_dir[0] = '\0';
    oc_dir_ok = 0;
    if(!ps_global)
      return;

    home = ps_global->home_dir;
    if(!home || !home[0])
      return;

    snprintf(oc_dir, sizeof(oc_dir), "%s%s.alpine-offcache",
	     home, home[strlen(home) - 1] == '/' ? "" : "/");
    oc_dir_ok = 1;
}


void
offcache_load_all(void)
{
#ifndef	_WINDOWS
    DIR *dp;
    struct dirent *de;
    char path[MAILTMPLEN];
    size_t len;

    if(!offcache_enabled() || !oc_dir_ok || !oc_dir[0])
      return;

    dp = opendir(oc_dir);
    if(!dp)
      return;

    len = strlen(oc_dir);
    while((de = readdir(dp)) != NULL){
	char *n = de->d_name;
	size_t nl;

	if(!n || n[0] == '.')
	  continue;

	nl = strlen(n);
	if(nl < 5 || strcmp(n + nl - 4, ".aoc"))
	  continue;

	if(len + 1 + nl >= sizeof(path))
	  continue;

	snprintf(path, sizeof(path), "%s/%s", oc_dir, n);
	(void) oc_read_folder(path);
    }

    closedir(dp);
#endif
}


void
offcache_save_all(void)
{
    OC_FOLDER *f;
    int wrote = 0;

    if(!offcache_enabled() || !oc_dir_ok)
      return;

    for(f = oc_folders; f; f = f->next){
	if(f->dirty){
	    if(oc_write_folder(f) == 0){
		f->dirty = 0;
		wrote++;
	    }
	}
    }

    if(wrote)
      dprint((2, "offcache: wrote %d folder(s) to %s\n", wrote, oc_dir));
}


void
offcache_learn_stream(MAILSTREAM *stream)
{
    OC_FOLDER *f;
    unsigned long i, kept = 0;
    char *mbx;
    OC_MSG *keep = NULL;
    unsigned long kalloc = 0, kn = 0, body_bytes = 0;

    if(!offcache_enabled() || !stream || offcache_is_stream(stream))
      return;

    mbx = stream->original_mailbox ? stream->original_mailbox : stream->mailbox;
    if(!mbx || !IS_REMOTE(mbx))
      return;

    f = oc_get(mbx);
    if(!f)
      return;

    if(stream->uid_validity && f->uidvalidity
       && stream->uid_validity != f->uidvalidity){
	unsigned long j;

	for(j = 0; j < f->n; j++)
	  oc_free_msg(&f->msgs[j]);

	f->n = 0;
	f->body_bytes = 0;
	f->dirty = 1;
    }

    f->uidvalidity = stream->uid_validity;
    f->uidlast = stream->uid_last;

    /*
     * Rebuild the cached set from the live folder.  Keep previously
     * learned headers/bodies for UIDs that are still present.
     */
    kalloc = stream->nmsgs + 8;
    keep = (OC_MSG *) fs_get(kalloc * sizeof(OC_MSG));
    memset(keep, 0, kalloc * sizeof(OC_MSG));

    for(i = 1; i <= stream->nmsgs; i++){
	MESSAGECACHE *elt = mail_elt(stream, i);
	OC_MSG *old, *nm;
	ENVELOPE *env;
	SIZEDTEXT *hdrt, *fullt, *textt;
	unsigned long uid;

	if(!elt)
	  continue;

	uid = elt->private.uid;
	if(!uid)
	  continue;

	nm = &keep[kn];
	memset(nm, 0, sizeof(*nm));
	nm->uid = uid;
	nm->size = elt->rfc822_size;
	oc_copy_elt_flags(nm, elt);
	nm->day = elt->day;
	nm->month = elt->month;
	nm->year = elt->year;
	nm->hours = elt->hours;
	nm->minutes = elt->minutes;
	nm->seconds = elt->seconds;

	old = oc_find_uid(f, uid);
	hdrt = &elt->private.msg.header.text;
	fullt = &elt->private.msg.full.text;
	textt = &elt->private.msg.text.text;
	env = elt->private.msg.env;

	if(fullt->data && fullt->size){
	    char *p, *split = NULL;
	    unsigned long hl;

	    p = (char *) fullt->data;
	    if((split = strstr(p, "\r\n\r\n")) != NULL)
	      split += 4;
	    else if((split = strstr(p, "\n\n")) != NULL)
	      split += 2;

	    if(split && split > p && (unsigned long) (split - p) < fullt->size){
		hl = (unsigned long) (split - p);
		nm->hdr = oc_dup(p, hl);
		nm->hdrlen = hl;
		nm->bodylen = fullt->size - hl;
		if(nm->bodylen && body_bytes + nm->bodylen <= OC_MAX_BODY){
		    nm->body = oc_dup(split, nm->bodylen);
		    body_bytes += nm->bodylen;
		}
		else
		  nm->bodylen = 0;
	    }
	    else{
		nm->hdr = oc_dup(p, fullt->size);
		nm->hdrlen = fullt->size;
	    }
	}
	else{
	    if(hdrt->data && hdrt->size){
		nm->hdr = oc_dup((char *) hdrt->data, hdrt->size);
		nm->hdrlen = hdrt->size;
	    }
	    else if(env){
		nm->hdr = oc_header_from_env(env, &nm->hdrlen);
	    }
	    else if(old && old->hdr){
		nm->hdr = oc_dup(old->hdr, old->hdrlen);
		nm->hdrlen = old->hdrlen;
	    }

	    if(textt->data && textt->size
	       && body_bytes + textt->size <= OC_MAX_BODY){
		nm->body = oc_dup((char *) textt->data, textt->size);
		nm->bodylen = textt->size;
		body_bytes += nm->bodylen;
	    }
	    else if(old && old->body && old->bodylen
		    && body_bytes + old->bodylen <= OC_MAX_BODY){
		nm->body = oc_dup(old->body, old->bodylen);
		nm->bodylen = old->bodylen;
		body_bytes += nm->bodylen;
	    }
	}

	if(old && old->flags_dirty){
	    /*
	     * Offline flag edits win until we have applied them
	     * to the live folder.  Keep the cached flag bits.
	     */
	    nm->flags = old->flags;
	    nm->user_flags = old->user_flags;
	    nm->flags_dirty = 1;
	}

	if(!nm->day && old){
	    nm->day = old->day;
	    nm->month = old->month;
	    nm->year = old->year;
	    nm->hours = old->hours;
	    nm->minutes = old->minutes;
	    nm->seconds = old->seconds;
	}

	kn++;
	kept++;
    }

    {
	unsigned long j;

	for(j = 0; j < f->n; j++)
	  oc_free_msg(&f->msgs[j]);

	if(f->msgs)
	  fs_give((void **) &f->msgs);
    }

    f->msgs = keep;
    f->n = kn;
    f->alloc = kalloc;
    f->body_bytes = body_bytes;
    f->dirty = 1;
    dprint((3, "offcache: learned %lu message(s) from %s\n",
	    kept, mbx));
}


void
offcache_apply_dirty_flags(MAILSTREAM *stream)
{
    OC_FOLDER *f;
    char *mbx;
    unsigned long i;

    if(!offcache_enabled() || !stream || offcache_is_stream(stream))
      return;

    mbx = stream->original_mailbox ? stream->original_mailbox : stream->mailbox;
    if(!mbx)
      return;

    f = oc_find(mbx);
    if(!f)
      return;

    for(i = 0; i < f->n; i++){
	OC_MSG *m = &f->msgs[i];
	unsigned long msgno;
	char seq[32];

	if(!m->flags_dirty || !m->uid)
	  continue;

	msgno = mail_msgno(stream, m->uid);
	if(!msgno)
	  continue;

	snprintf(seq, sizeof(seq), "%lu", m->uid);
	if(m->flags & OCF_SEEN)
	  mail_flag(stream, seq, "\\Seen", ST_SET | ST_UID);
	else
	  mail_flag(stream, seq, "\\Seen", ST_UID);

	if(m->flags & OCF_DELETED)
	  mail_flag(stream, seq, "\\Deleted", ST_SET | ST_UID);
	else
	  mail_flag(stream, seq, "\\Deleted", ST_UID);

	if(m->flags & OCF_FLAGGED)
	  mail_flag(stream, seq, "\\Flagged", ST_SET | ST_UID);
	else
	  mail_flag(stream, seq, "\\Flagged", ST_UID);

	m->flags_dirty = 0;
    }
}


DRIVER *
offcache_valid(char *name)
{
    if(offcache_is_name(name))
      return(&offcachedriver);

    return(NIL);
}


void *
offcache_parameters(long int function, void *value)
{
    (void) function;
    (void) value;
    return(NIL);
}


void
offcache_scan(MAILSTREAM *stream, char *ref, char *pat, char *contents)
{
    (void) stream;
    (void) ref;
    (void) pat;
    (void) contents;
}


void
offcache_list(MAILSTREAM *stream, char *ref, char *pat)
{
    (void) stream;
    (void) ref;
    (void) pat;
}


void
offcache_lsub(MAILSTREAM *stream, char *ref, char *pat)
{
    (void) stream;
    (void) ref;
    (void) pat;
}


long
offcache_create(MAILSTREAM *stream, char *mailbox)
{
    (void) stream;
    (void) mailbox;
    return(NIL);
}


long
offcache_delete(MAILSTREAM *stream, char *mailbox)
{
    (void) stream;
    (void) mailbox;
    return(NIL);
}


long
offcache_rename(MAILSTREAM *stream, char *old, char *newname)
{
    (void) stream;
    (void) old;
    (void) newname;
    return(NIL);
}


MAILSTREAM *
offcache_open(MAILSTREAM *stream)
{
    OC_FOLDER *f;
    OCLOCAL *local;
    char *real;
    unsigned long i, recent = 0;

    if(!stream)
      return(&offcacheproto);

    if(stream->local){
	mm_log("offcache recycle stream", ERROR);
	return(NIL);
    }

    real = offcache_real_name(stream->mailbox);
    f = oc_find(real);
    if(!f)
      f = oc_get(real);

    local = (OCLOCAL *) fs_get(sizeof(OCLOCAL));
    memset(local, 0, sizeof(*local));
    local->folder = f;
    stream->local = local;
    stream->inbox = 0;
    stream->rdonly = NIL;
    stream->perm_seen = stream->perm_deleted = T;
    stream->perm_flagged = stream->perm_answered = stream->perm_draft = T;
    stream->uid_validity = f ? f->uidvalidity : 1;
    stream->uid_last = f ? f->uidlast : 0;
    stream->sequence++;

    mail_exists(stream, f ? f->n : 0);
    for(i = 1; f && i <= f->n; i++){
	MESSAGECACHE *elt = mail_elt(stream, i);
	OC_MSG *m = &f->msgs[i - 1];

	elt->private.uid = m->uid;
	elt->rfc822_size = m->size;
	elt->valid = T;
	oc_apply_flags(elt, m);
	elt->day = m->day;
	elt->month = m->month;
	elt->year = m->year;
	elt->hours = m->hours;
	elt->minutes = m->minutes;
	elt->seconds = m->seconds;
	if(elt->recent)
	  recent++;
    }

    mail_recent(stream, recent);
    return(stream);
}


void
offcache_close(MAILSTREAM *stream, long int options)
{
    (void) options;
    if(!stream || !stream->local)
      return;

    if(LOCAL->scratch)
      fs_give((void **) &LOCAL->scratch);

    fs_give((void **) &stream->local);
}


char *
offcache_header(MAILSTREAM *stream, unsigned long msgno,
		unsigned long *length, long int flags)
{
    OC_MSG *m;
    unsigned long n;

    (void) flags;
    if(length)
      *length = 0;

    if(!stream || !LOCAL || !LOCAL->folder || msgno < 1
       || msgno > LOCAL->folder->n)
      return("");

    m = &LOCAL->folder->msgs[msgno - 1];
    if(!m->hdr || !m->hdrlen)
      return("");

    n = m->hdrlen;
    if(!LOCAL->scratch || LOCAL->scratchlen < n + 1){
	if(LOCAL->scratch)
	  fs_give((void **) &LOCAL->scratch);

	LOCAL->scratchlen = n + 1;
	LOCAL->scratch = (char *) fs_get(LOCAL->scratchlen);
    }

    memcpy(LOCAL->scratch, m->hdr, n);
    LOCAL->scratch[n] = '\0';
    if(length)
      *length = n;

    return(LOCAL->scratch);
}


long
offcache_text(MAILSTREAM *stream, unsigned long msgno, STRING *bs, long int flags)
{
    OC_MSG *m;
    char *p;
    unsigned long n;
    MESSAGECACHE *elt;

    if(flags & FT_UID)
      return(NIL);

    if(!stream || !LOCAL || !LOCAL->folder || msgno < 1
       || msgno > LOCAL->folder->n)
      return(NIL);

    m = &LOCAL->folder->msgs[msgno - 1];
    elt = mail_elt(stream, msgno);
    if(!(flags & FT_PEEK) && elt && !elt->seen){
	elt->seen = T;
	m->flags |= OCF_SEEN;
	m->flags_dirty = 1;
	LOCAL->folder->dirty = 1;
	MM_FLAGS(stream, msgno);
    }

    if(m->body && m->bodylen){
	p = m->body;
	n = m->bodylen;
    }
    else{
	p = OC_STUB;
	n = strlen(p);
    }

    INIT(bs, mail_string, (void *) p, n);
    return(LONGT);
}


void
offcache_flagmsg(MAILSTREAM *stream, MESSAGECACHE *elt)
{
    OC_MSG *m;

    if(!stream || !LOCAL || !LOCAL->folder || !elt || !elt->valid)
      return;

    m = oc_find_uid(LOCAL->folder, elt->private.uid);
    if(!m)
      return;

    oc_copy_elt_flags(m, elt);
    m->flags_dirty = 1;
    LOCAL->folder->dirty = 1;
}


long
offcache_ping(MAILSTREAM *stream)
{
    return(stream ? LONGT : NIL);
}


void
offcache_check(MAILSTREAM *stream)
{
    (void) stream;
}


long
offcache_expunge(MAILSTREAM *stream, char *sequence, long int options)
{
    OC_FOLDER *f;
    unsigned long i, d = 0;

    (void) sequence;
    (void) options;
    if(!stream || !LOCAL || !LOCAL->folder)
      return(LONGT);

    f = LOCAL->folder;
    for(i = 1; i <= stream->nmsgs; ){
	MESSAGECACHE *elt = mail_elt(stream, i);

	if(elt && elt->deleted){
	    unsigned long idx = i - 1;

	    if(idx < f->n){
		if(f->msgs[idx].bodylen > f->body_bytes)
		  f->body_bytes = 0;
		else
		  f->body_bytes -= f->msgs[idx].bodylen;

		oc_free_msg(&f->msgs[idx]);
		if(idx + 1 < f->n)
		  memmove(&f->msgs[idx], &f->msgs[idx + 1],
			  (f->n - idx - 1) * sizeof(OC_MSG));

		f->n--;
		f->dirty = 1;
	    }

	    mail_expunged(stream, i);
	    d++;
	}
	else
	  i++;
    }

    return(d ? LONGT : LONGT);
}


long
offcache_copy(MAILSTREAM *stream, char *sequence, char *mailbox, long int options)
{
    (void) stream;
    (void) sequence;
    (void) mailbox;
    (void) options;
    mm_log("Copy from the offline cache needs a live connection", ERROR);
    return(NIL);
}


long
offcache_append(MAILSTREAM *stream, char *mailbox, append_t af, void *data)
{
    (void) stream;
    (void) mailbox;
    (void) af;
    (void) data;
    mm_log("Append to the offline cache needs a live connection", ERROR);
    return(NIL);
}


static void
oc_free_msg(OC_MSG *m)
{
    if(!m)
      return;

    if(m->hdr)
      fs_give((void **) &m->hdr);

    if(m->body)
      fs_give((void **) &m->body);

    memset(m, 0, sizeof(*m));
}


static OC_FOLDER *
oc_find(char *mailbox)
{
    OC_FOLDER *f;

    if(!mailbox)
      return(NULL);

    for(f = oc_folders; f; f = f->next)
      if(f->mailbox && !strcmp(f->mailbox, mailbox))
	return(f);

    return(NULL);
}


static OC_FOLDER *
oc_get(char *mailbox)
{
    OC_FOLDER *f;

    f = oc_find(mailbox);
    if(f)
      return(f);

    f = (OC_FOLDER *) fs_get(sizeof(OC_FOLDER));
    memset(f, 0, sizeof(*f));
    f->mailbox = cpystr(mailbox);
    f->next = oc_folders;
    oc_folders = f;
    return(f);
}


static OC_MSG *
oc_find_uid(OC_FOLDER *f, unsigned long uid)
{
    unsigned long i;

    if(!f || !uid)
      return(NULL);

    for(i = 0; i < f->n; i++)
      if(f->msgs[i].uid == uid)
	return(&f->msgs[i]);

    return(NULL);
}


static OC_MSG *
oc_add_msg(OC_FOLDER *f)
{
    if(!f)
      return(NULL);

    if(f->n >= f->alloc){
	unsigned long na = f->alloc ? f->alloc * 2 : 32;
	OC_MSG *nm = (OC_MSG *) fs_get(na * sizeof(OC_MSG));

	memset(nm, 0, na * sizeof(OC_MSG));
	if(f->msgs){
	    memcpy(nm, f->msgs, f->n * sizeof(OC_MSG));
	    fs_give((void **) &f->msgs);
	}

	f->msgs = nm;
	f->alloc = na;
    }

    memset(&f->msgs[f->n], 0, sizeof(OC_MSG));
    return(&f->msgs[f->n++]);
}


static unsigned
oc_hash(char *s)
{
    unsigned h = 2166136261u;

    while(s && *s){
	h ^= (unsigned char) *s++;
	h *= 16777619u;
    }

    return(h);
}


static void
oc_path_for(char *dst, size_t dstlen, char *mailbox)
{
    snprintf(dst, dstlen, "%s/%08x.aoc", oc_dir, oc_hash(mailbox));
}


static char *
oc_dup(char *s, unsigned long n)
{
    char *d;

    if(!s)
      return(NULL);

    d = (char *) fs_get(n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return(d);
}


static void
oc_so_addr(STORE_S *so, char *name, ADDRESS *a)
{
    char tmp[MAILTMPLEN];

    if(!so || !name || !a)
      return;

    tmp[0] = '\0';
    rfc822_write_address(tmp, a);
    if(!tmp[0])
      return;

    so_puts(so, name);
    so_puts(so, tmp);
    so_puts(so, "\r\n");
}


static char *
oc_header_from_env(ENVELOPE *env, unsigned long *lenp)
{
    STORE_S *so;
    char *s;

    if(lenp)
      *lenp = 0;

    if(!env)
      return(NULL);

    so = so_get(CharStar, NULL, EDIT_ACCESS);
    if(!so)
      return(NULL);

    if(env->date){
	so_puts(so, "Date: ");
	so_puts(so, (char *) env->date);
	so_puts(so, "\r\n");
    }

    oc_so_addr(so, "From: ", env->from);
    oc_so_addr(so, "Sender: ", env->sender);
    oc_so_addr(so, "Reply-To: ", env->reply_to);
    oc_so_addr(so, "To: ", env->to);
    oc_so_addr(so, "Cc: ", env->cc);
    if(env->subject){
	so_puts(so, "Subject: ");
	so_puts(so, env->subject);
	so_puts(so, "\r\n");
    }

    if(env->message_id){
	so_puts(so, "Message-ID: ");
	so_puts(so, env->message_id);
	so_puts(so, "\r\n");
    }

    if(env->in_reply_to){
	so_puts(so, "In-Reply-To: ");
	so_puts(so, env->in_reply_to);
	so_puts(so, "\r\n");
    }

    if(env->references){
	so_puts(so, "References: ");
	so_puts(so, env->references);
	so_puts(so, "\r\n");
    }

    so_puts(so, "\r\n");
    s = cpystr((char *) so_text(so));
    if(lenp)
      *lenp = s ? strlen(s) : 0;

    so_give(&so);
    return(s);
}


static void
oc_copy_elt_flags(OC_MSG *m, MESSAGECACHE *elt)
{
    m->flags = 0;
    if(elt->seen)
      m->flags |= OCF_SEEN;

    if(elt->deleted)
      m->flags |= OCF_DELETED;

    if(elt->flagged)
      m->flags |= OCF_FLAGGED;

    if(elt->answered)
      m->flags |= OCF_ANSWERED;

    if(elt->draft)
      m->flags |= OCF_DRAFT;

    if(elt->recent)
      m->flags |= OCF_RECENT;

    m->user_flags = elt->user_flags;
}


static void
oc_apply_flags(MESSAGECACHE *elt, OC_MSG *m)
{
    elt->seen = (m->flags & OCF_SEEN) ? T : NIL;
    elt->deleted = (m->flags & OCF_DELETED) ? T : NIL;
    elt->flagged = (m->flags & OCF_FLAGGED) ? T : NIL;
    elt->answered = (m->flags & OCF_ANSWERED) ? T : NIL;
    elt->draft = (m->flags & OCF_DRAFT) ? T : NIL;
    elt->recent = (m->flags & OCF_RECENT) ? T : NIL;
    elt->user_flags = m->user_flags;
}


static int
oc_write_u32(FILE *fp, unsigned long v)
{
    unsigned char b[4];

    b[0] = (unsigned char) ((v >> 24) & 0xff);
    b[1] = (unsigned char) ((v >> 16) & 0xff);
    b[2] = (unsigned char) ((v >> 8) & 0xff);
    b[3] = (unsigned char) (v & 0xff);
    return(fwrite(b, 1, 4, fp) == 4);
}


static int
oc_read_u32(FILE *fp, unsigned long *vp)
{
    unsigned char b[4];

    if(fread(b, 1, 4, fp) != 4)
      return(0);

    *vp = ((unsigned long) b[0] << 24) | ((unsigned long) b[1] << 16)
	  | ((unsigned long) b[2] << 8) | (unsigned long) b[3];
    return(1);
}


static int
oc_write_bytes(FILE *fp, char *s, unsigned long n)
{
    if(!n)
      return(1);

    return(s && fwrite(s, 1, n, fp) == n);
}


static char *
oc_read_bytes(FILE *fp, unsigned long n)
{
    char *s;

    if(!n)
      return(NULL);

    s = (char *) fs_get(n + 1);
    if(fread(s, 1, n, fp) != n){
	fs_give((void **) &s);
	return(NULL);
    }

    s[n] = '\0';
    return(s);
}


static int
oc_write_folder(OC_FOLDER *f)
{
    FILE *fp;
    char path[MAILTMPLEN], tmp[MAILTMPLEN];
    unsigned long i, mlen;

    if(!f || !f->mailbox || !oc_dir_ok)
      return(-1);

    if(create_mail_dir(oc_dir) < 0 && errno != EEXIST)
      return(-1);

    oc_path_for(path, sizeof(path), f->mailbox);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fp = our_fopen(tmp, "wb");
    if(!fp)
      return(-1);

    if(fputs(OC_MAGIC, fp) == EOF)
      goto fail;

    mlen = strlen(f->mailbox);
    if(!oc_write_u32(fp, mlen) || !oc_write_bytes(fp, f->mailbox, mlen)
       || !oc_write_u32(fp, f->uidvalidity)
       || !oc_write_u32(fp, f->uidlast)
       || !oc_write_u32(fp, f->n))
      goto fail;

    for(i = 0; i < f->n; i++){
	OC_MSG *m = &f->msgs[i];
	unsigned long packed;

	packed = (m->flags & 0xff)
		 | ((m->day & 0x1f) << 8)
		 | ((m->month & 0x0f) << 13)
		 | ((m->year & 0x7f) << 17);
	if(!oc_write_u32(fp, m->uid)
	   || !oc_write_u32(fp, m->size)
	   || !oc_write_u32(fp, packed)
	   || !oc_write_u32(fp, m->user_flags)
	   || !oc_write_u32(fp, ((m->hours & 0x1f) << 16)
				| ((m->minutes & 0x3f) << 8)
				| (m->seconds & 0x3f))
	   || !oc_write_u32(fp, m->hdrlen)
	   || !oc_write_u32(fp, m->bodylen)
	   || !oc_write_bytes(fp, m->hdr, m->hdrlen)
	   || !oc_write_bytes(fp, m->body, m->bodylen))
	  goto fail;
    }

    if(fflush(fp) != 0)
      goto fail;

#ifndef	_WINDOWS
    (void) fsync(fileno(fp));
#endif
    fclose(fp);
    if(rename_file(tmp, path) < 0){
	our_unlink(tmp);
	return(-1);
    }

    return(0);

  fail:
    fclose(fp);
    our_unlink(tmp);
    return(-1);
}


static int
oc_read_folder(char *path)
{
    FILE *fp;
    char magic[8], *mbx = NULL;
    unsigned long mlen, uidvalidity, uidlast, n, i, body_bytes = 0;
    OC_FOLDER *f;

    fp = our_fopen(path, "rb");
    if(!fp)
      return(-1);

    if(fread(magic, 1, 5, fp) != 5 || memcmp(magic, OC_MAGIC, 5)
       || !oc_read_u32(fp, &mlen) || mlen == 0 || mlen > MAILTMPLEN){
	fclose(fp);
	return(-1);
    }

    mbx = oc_read_bytes(fp, mlen);
    if(!mbx
       || !oc_read_u32(fp, &uidvalidity)
       || !oc_read_u32(fp, &uidlast)
       || !oc_read_u32(fp, &n)){
	if(mbx)
	  fs_give((void **) &mbx);

	fclose(fp);
	return(-1);
    }

    f = oc_get(mbx);
    fs_give((void **) &mbx);
    {
	unsigned long j;

	for(j = 0; j < f->n; j++)
	  oc_free_msg(&f->msgs[j]);

	if(f->msgs)
	  fs_give((void **) &f->msgs);

	f->n = 0;
	f->alloc = 0;
	f->body_bytes = 0;
    }

    f->uidvalidity = uidvalidity;
    f->uidlast = uidlast;
    if(n > 0){
	f->msgs = (OC_MSG *) fs_get((n + 8) * sizeof(OC_MSG));
	memset(f->msgs, 0, (n + 8) * sizeof(OC_MSG));
	f->alloc = n + 8;
    }

    for(i = 0; i < n; i++){
	OC_MSG *m = oc_add_msg(f);
	unsigned long packed, hms;

	if(!oc_read_u32(fp, &m->uid)
	   || !oc_read_u32(fp, &m->size)
	   || !oc_read_u32(fp, &packed)
	   || !oc_read_u32(fp, &m->user_flags)
	   || !oc_read_u32(fp, &hms)
	   || !oc_read_u32(fp, &m->hdrlen)
	   || !oc_read_u32(fp, &m->bodylen))
	  goto fail;

	m->flags = packed & 0xff;
	m->day = (packed >> 8) & 0x1f;
	m->month = (packed >> 13) & 0x0f;
	m->year = (packed >> 17) & 0x7f;
	m->hours = (hms >> 16) & 0x1f;
	m->minutes = (hms >> 8) & 0x3f;
	m->seconds = hms & 0x3f;

	if(m->hdrlen > 4L * 1024L * 1024L || m->bodylen > OC_MAX_BODY)
	  goto fail;

	if(m->hdrlen){
	    m->hdr = oc_read_bytes(fp, m->hdrlen);
	    if(!m->hdr)
	      goto fail;
	}

	if(m->bodylen){
	    if(body_bytes + m->bodylen > OC_MAX_BODY){
		if(fseek(fp, (long) m->bodylen, SEEK_CUR) != 0)
		  goto fail;

		m->bodylen = 0;
	    }
	    else{
		m->body = oc_read_bytes(fp, m->bodylen);
		if(!m->body)
		  goto fail;

		body_bytes += m->bodylen;
	    }
	}
    }

    f->body_bytes = body_bytes;
    f->dirty = 0;
    fclose(fp);
    return(0);

  fail:
    fclose(fp);
    return(-1);
}
