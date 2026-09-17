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
 * Program:	Offline cache for remote IMAP folders
 *
 * RAM holds envelopes, flags, headers, and any bodies already fetched.
 * Disk is read once at startup and written when a folder is closed, at
 * exit, and from the signal cleanup path, and only if something changed.
 *
 * On-disk format (AOC2, big-endian 32-bit fields, FNV-1a checksum):
 *
 *   "AOC2\n"
 *   u32 mlen, mailbox name
 *   u32 uidvalidity, u32 uidlast, u32 last_used (time_t)
 *   u32 nkw, then nkw x (u32 index, u32 len, name)     keyword names
 *   u32 ntomb, then ntomb x u32 uid                     deleted offline
 *   u32 n, then n records:
 *       u32 uid, size, packed(flags|date), user_flags, hms,
 *           dirty, user_dirty, hdrlen, bodylen, hdr, body
 *   u32 checksum over everything before it
 *
 * AOC1 files (no dirty bits, keywords, tombstones or checksum) are
 * still read.
 */

#include "../pith/headers.h"
#include "../pith/offcache.h"
#include "../pith/state.h"
#include "../pith/conf.h"
#include "../pith/status.h"
#include "../pith/store.h"
#include "../pith/util.h"
#include "../pith/stream.h"
#include "../pith/busy.h"
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
#include <time.h>


#define	OC_MAGIC1		"AOC1\n"
#define	OC_MAGIC2		"AOC2\n"
#define	OC_MAGIC_LEN		5
#define	OC_MB			(1024UL * 1024UL)
#define	OC_DEF_FOLDER_MB	32L
#define	OC_DEF_TOTAL_MB		256L
#define	OC_DEF_MAX_AGE_DAYS	90L
#define	OC_MAX_MSG_BODY		(4UL * OC_MB)	/* largest single body kept */
#define	OC_MAX_HDR		(4UL * OC_MB)
#define	OC_REC_MIN_V1		28UL		/* 7 x u32 */
#define	OC_REC_MIN_V2		36UL		/* 9 x u32 */
#define	OC_LEARN_FAST_MAX	4000L		/* fetch missing UIDs up to this */
#define	OC_SEQ_CHUNK		100
#define	OC_FAST_CHUNK		500
#define	OC_PATH_TRIES		16

#define	OC_STUB \
"\r\n[Alpine: this message body is not in the offline cache.\r\n" \
" Use Sync, or re-open the folder while connected, to fetch it.]\r\n"

#define	OCF_SEEN		0x01
#define	OCF_DELETED		0x02
#define	OCF_FLAGGED		0x04
#define	OCF_ANSWERED		0x08
#define	OCF_DRAFT		0x10
#define	OCF_RECENT		0x20
#define	OCF_DIRTYABLE		(OCF_SEEN | OCF_DELETED | OCF_FLAGGED \
				 | OCF_ANSWERED | OCF_DRAFT)


typedef struct oc_msg {
    unsigned long uid;
    unsigned long size;
    unsigned long flags;
    unsigned long user_flags;
    unsigned long dirty;		/* OCF_ bits changed offline */
    unsigned long user_dirty;		/* keyword bits changed offline */
    unsigned int  day:5, month:4, year:7, hours:5, minutes:6, seconds:6;
    char	 *hdr;
    unsigned long hdrlen;
    char	 *body;
    unsigned long bodylen;
    unsigned	  wanted:1;		/* body should be fetched at learn */
    unsigned	  live:1;		/* scratch: present on the server */
} OC_MSG;

typedef struct oc_folder {
    char	 *mailbox;
    unsigned long uidvalidity;
    unsigned long uidlast;
    unsigned long last_used;
    unsigned long n;
    unsigned long alloc;
    OC_MSG	 *msgs;
    unsigned long body_bytes;
    char	 *keywords[NUSERFLAGS];
    unsigned long *tomb;
    unsigned long ntomb;
    unsigned long tomb_alloc;
    unsigned	  dirty:1;
    struct oc_folder *next;
} OC_FOLDER;

typedef struct oc_local {
    OC_FOLDER	 *folder;
    char	 *scratch;
    unsigned long scratchlen;
} OCLOCAL;

typedef struct oc_io {
    FILE	 *fp;
    unsigned long hash;
} OC_IO;


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
static void	 oc_free_folder_contents(OC_FOLDER *);
static OC_FOLDER *oc_find(char *);
static OC_FOLDER *oc_get(char *);
static OC_MSG	 *oc_find_uid(OC_FOLDER *, unsigned long);
static OC_MSG	 *oc_add_msg(OC_FOLDER *);
static void	 oc_remove_msg(OC_FOLDER *, unsigned long);
static void	 oc_add_tomb(OC_FOLDER *, unsigned long);
static unsigned	 oc_hash(char *);
static void	 oc_path_for(char *, size_t, char *);
static char	 *oc_file_mailbox(char *);
static char	 *oc_dup(char *, unsigned long);
static char	 *oc_header_from_env(ENVELOPE *, unsigned long *);
static void	 oc_copy_elt_flags(OC_MSG *, MESSAGECACHE *);
static void	 oc_apply_flags(MESSAGECACHE *, OC_MSG *);
static void	 oc_sync_keywords(OC_FOLDER *, MAILSTREAM *);
static void	 oc_apply_seq(MAILSTREAM *, unsigned long *, unsigned long,
			      char *, int);
static void	 oc_expunge_seq(MAILSTREAM *, unsigned long *, unsigned long);
static void	 oc_enforce_folder_cap(OC_FOLDER *);
static void	 oc_enforce_total_cap(void);
static void	 oc_learn(MAILSTREAM *, int);
static int	 oc_write_folder(OC_FOLDER *);
static int	 oc_read_folder(char *);
static void	 oc_hash_bytes(unsigned long *, unsigned char *, unsigned long);
static int	 oc_w32(OC_IO *, unsigned long);
static int	 oc_r32(OC_IO *, unsigned long *);
static int	 oc_wbytes(OC_IO *, char *, unsigned long);
static char	 *oc_rbytes(OC_IO *, unsigned long);
static void	 oc_so_addr(STORE_S *, char *, ADDRESS *);
static long	 oc_cfg_long(char *, long);
static unsigned long oc_folder_cap(void);
static unsigned long oc_total_cap(void);
static long	 oc_max_age_secs(void);


/*----------------------------------------------------------------------
   Configuration and naming
  ----*/

static long
oc_cfg_long(char *s, long dflt)
{
    if(s && *s){
	long v = atol(s);

	if(v >= 0)
	  return(v);
    }

    return(dflt);
}


static unsigned long
oc_folder_cap(void)
{
    return((unsigned long) oc_cfg_long(ps_global ? ps_global->VAR_OFFCACHE_FOLDER_MB
					 : NULL, OC_DEF_FOLDER_MB) * OC_MB);
}


static unsigned long
oc_total_cap(void)
{
    return((unsigned long) oc_cfg_long(ps_global ? ps_global->VAR_OFFCACHE_TOTAL_MB
					 : NULL, OC_DEF_TOTAL_MB) * OC_MB);
}


static long
oc_max_age_secs(void)
{
    return(oc_cfg_long(ps_global ? ps_global->VAR_OFFCACHE_MAX_AGE : NULL,
		       OC_DEF_MAX_AGE_DAYS) * 86400L);
}


int
offcache_enabled(void)
{
#ifdef	_WINDOWS
    return(0);
#else
    if(!ps_global)
      return(0);

    return(F_ON(F_OFFLINE_CACHE, ps_global));
#endif
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


/*
 * Only remote IMAP mailboxes are cached: not news, not POP, not local.
 */
int
offcache_cacheable_name(char *mailbox)
{
    NETMBX mb;

    if(!mailbox || *mailbox != '{')
      return(0);

    memset(&mb, 0, sizeof(mb));
    if(!mail_valid_net_parse(mailbox, &mb))
      return(0);

    return(!strcmp(mb.service, "imap"));
}


int
offcache_cacheable_stream(MAILSTREAM *stream)
{
    char *mbx;

    if(!stream || offcache_is_stream(stream) || stream->halfopen)
      return(0);

    if(!(stream->dtb && stream->dtb->name && !strcmp(stream->dtb->name, "imap")))
      return(0);

    mbx = stream->original_mailbox ? stream->original_mailbox : stream->mailbox;
    return(offcache_cacheable_name(mbx));
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

    if(F_OFF(F_OFFLINE_CACHE_FIRST, ps_global))
      return(0);

    if(IS_REMOTE(mbox))
      return(1);

    return(0);
}


/*
 * Number of offline changes waiting to be applied to the live folder.
 */
int
offcache_pending_count(char *mailbox)
{
    OC_FOLDER *f;
    unsigned long i;
    int n = 0;

    if(!mailbox)
      return(0);

    f = oc_find(offcache_real_name(mailbox));
    if(!f)
      return(0);

    for(i = 0; i < f->n; i++)
      if(f->msgs[i].dirty || f->msgs[i].user_dirty)
	n++;

    return(n + (int) f->ntomb);
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


/*----------------------------------------------------------------------
   Load and save
  ----*/

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
	if(oc_read_folder(path) < 0){
	    dprint((2, "offcache: ignoring unreadable cache file %s\n", path));
	}
    }

    closedir(dp);
    oc_enforce_total_cap();
#endif
}


void
offcache_save_all(void)
{
    OC_FOLDER *f;
    int wrote = 0, failed = 0;

    if(!offcache_enabled() || !oc_dir_ok)
      return;

    for(f = oc_folders; f; f = f->next){
	if(f->dirty){
	    if(oc_write_folder(f) == 0){
		f->dirty = 0;
		wrote++;
	    }
	    else
	      failed++;
	}
    }

    if(wrote || failed)
      dprint((2, "offcache: wrote %d folder(s) to %s, %d failed\n",
	      wrote, oc_dir, failed));
}


void
offcache_checkpoint(void)
{
    offcache_save_all();
}


/*
 * Called from the signal cleanup path: no network, no prompts.
 */
void
offcache_emergency_save(void)
{
    int i;

    if(!offcache_enabled() || !ps_global)
      return;

    for(i = 0; i < ps_global->s_pool.nstream; i++){
	MAILSTREAM *m = ps_global->s_pool.streams[i];

	if(m && offcache_cacheable_stream(m))
	  oc_learn(m, 0);
    }

    offcache_save_all();
}


/*----------------------------------------------------------------------
   Learning from a live stream
  ----*/

void
offcache_learn_stream(MAILSTREAM *stream)
{
    oc_learn(stream, 1);
}


void
offcache_learn_stream_quick(MAILSTREAM *stream)
{
    oc_learn(stream, 0);
}


/*
 * Remember that the viewer fetched part of this message, so its body
 * is worth caching when the folder is next learned.
 */
void
offcache_note_fetch(MAILSTREAM *stream, unsigned long msgno)
{
    MESSAGECACHE *elt;
    OC_FOLDER *f;
    OC_MSG *m;
    char *mbx;

    if(!offcache_enabled() || !stream || !offcache_cacheable_stream(stream))
      return;

    if(msgno < 1 || msgno > stream->nmsgs)
      return;

    elt = mail_elt(stream, msgno);
    if(!elt || !elt->private.uid)
      return;

    mbx = stream->original_mailbox ? stream->original_mailbox : stream->mailbox;
    f = oc_get(mbx);
    if(!f)
      return;

    if(f->uidvalidity && stream->uid_validity
       && f->uidvalidity != stream->uid_validity)
      return;				/* learn will reset this folder */

    m = oc_find_uid(f, elt->private.uid);
    if(!m){
	m = oc_add_msg(f);
	m->uid = elt->private.uid;
    }

    if(!m->body)
      m->wanted = 1;
}


static void
oc_learn(MAILSTREAM *stream, int allow_net)
{
    OC_FOLDER *f;
    unsigned long i, j, complete = 1, added = 0, fetched = 0, want = 0;
    unsigned long cap, msg_cap;
    char *mbx;
    int we_cancel = 0;

    if(!offcache_enabled() || !stream || !offcache_cacheable_stream(stream))
      return;

    mbx = stream->original_mailbox ? stream->original_mailbox : stream->mailbox;
    f = oc_get(mbx);
    if(!f)
      return;

    if(stream->uid_validity && f->uidvalidity
       && stream->uid_validity != f->uidvalidity){
	dprint((2, "offcache: UIDVALIDITY changed for %s, dropping cache\n", mbx));
	oc_free_folder_contents(f);
	f->dirty = 1;
    }

    f->uidvalidity = stream->uid_validity;
    f->uidlast = stream->uid_last;
    f->last_used = (unsigned long) time(NULL);
    oc_sync_keywords(f, stream);

    for(i = 0; i < f->n; i++)
      f->msgs[i].live = 0;

    /*
     * Make sure we know the UID of every message, cheaply, so that
     * stale cache entries can be recognised.  For big folders we only
     * learn what was touched this session and keep the rest.
     */
    if(allow_net && stream->nmsgs > 0 && stream->nmsgs <= OC_LEARN_FAST_MAX){
	char seq[OC_FAST_CHUNK * 12 + 8], *p;
	unsigned long start = 0, k = 0;

	p = seq;
	*p = '\0';
	for(i = 1; i <= stream->nmsgs + 1; i++){
	    MESSAGECACHE *elt = (i <= stream->nmsgs) ? mail_elt(stream, i) : NULL;
	    int missing = (elt && !elt->private.uid);

	    if(missing && !start)
	      start = i;

	    if(!missing && start){
		if(k)
		  *p++ = ',';

		if(start == i - 1)
		  p += snprintf(p, sizeof(seq) - (p - seq), "%lu", start);
		else
		  p += snprintf(p, sizeof(seq) - (p - seq), "%lu:%lu",
				start, i - 1);

		k += (i - start);
		start = 0;
		if(k >= OC_FAST_CHUNK || (p - seq) > (long) sizeof(seq) - 40){
		    mail_fetch_fast(stream, seq, NIL);
		    p = seq;
		    *p = '\0';
		    k = 0;
		}
	    }
	}

	if(k)
	  mail_fetch_fast(stream, seq, NIL);
    }

    cap = oc_folder_cap();
    msg_cap = OC_MAX_MSG_BODY;

    if(allow_net){
	for(i = 0; i < f->n; i++)
	  if(f->msgs[i].wanted && !f->msgs[i].body)
	    want++;

	if(want)
	  we_cancel = busy_cue(_("Updating offline cache"), NULL, 1);
    }

    for(i = 1; i <= stream->nmsgs; i++){
	MESSAGECACHE *elt = mail_elt(stream, i);
	OC_MSG *m;
	ENVELOPE *env;
	SIZEDTEXT *hdrt, *fullt, *textt;
	unsigned long uid, newflags;

	if(!elt)
	  continue;

	uid = elt->private.uid;
	if(!uid){
	    complete = 0;
	    continue;
	}

	m = oc_find_uid(f, uid);
	if(!m){
	    m = oc_add_msg(f);
	    m->uid = uid;
	    added++;
	}

	m->live = 1;
	m->size = elt->rfc822_size;

	/*
	 * Flags: the live folder wins, except for bits the user
	 * changed offline that have not been applied yet.
	 */
	{
	    OC_MSG tmp;

	    memset(&tmp, 0, sizeof(tmp));
	    oc_copy_elt_flags(&tmp, elt);
	    newflags = (tmp.flags & ~m->dirty) | (m->flags & m->dirty);
	    m->flags = newflags;
	    m->user_flags = (tmp.user_flags & ~m->user_dirty)
			    | (m->user_flags & m->user_dirty);
	}

	if(elt->day){
	    m->day = elt->day;
	    m->month = elt->month;
	    m->year = elt->year;
	    m->hours = elt->hours;
	    m->minutes = elt->minutes;
	    m->seconds = elt->seconds;
	}

	hdrt = &elt->private.msg.header.text;
	fullt = &elt->private.msg.full.text;
	textt = &elt->private.msg.text.text;
	env = elt->private.msg.env;

	if(fullt->data && fullt->size){
	    char *p = (char *) fullt->data, *split;
	    unsigned long hl;

	    if((split = strstr(p, "\r\n\r\n")) != NULL)
	      split += 4;
	    else if((split = strstr(p, "\n\n")) != NULL)
	      split += 2;

	    if(split && split > p && (unsigned long) (split - p) < fullt->size){
		hl = (unsigned long) (split - p);
		if(hl <= OC_MAX_HDR){
		    if(m->hdr)
		      fs_give((void **) &m->hdr);

		    m->hdr = oc_dup(p, hl);
		    m->hdrlen = hl;
		}

		if(!m->body && fullt->size - hl <= msg_cap
		   && f->body_bytes + (fullt->size - hl) <= cap){
		    m->body = oc_dup(split, fullt->size - hl);
		    m->bodylen = fullt->size - hl;
		    f->body_bytes += m->bodylen;
		}
	    }
	}
	else if(hdrt->data && hdrt->size && hdrt->size <= OC_MAX_HDR){
	    if(m->hdr)
	      fs_give((void **) &m->hdr);

	    m->hdr = oc_dup((char *) hdrt->data, hdrt->size);
	    m->hdrlen = hdrt->size;
	}
	else if(!m->hdr && env)
	  m->hdr = oc_header_from_env(env, &m->hdrlen);

	if(!m->body && textt->data && textt->size
	   && textt->size <= msg_cap && f->body_bytes + textt->size <= cap){
	    m->body = oc_dup((char *) textt->data, textt->size);
	    m->bodylen = textt->size;
	    f->body_bytes += m->bodylen;
	}

	if(m->wanted && !m->body && allow_net
	   && elt->rfc822_size <= msg_cap
	   && f->body_bytes + elt->rfc822_size <= cap){
	    char *t;
	    unsigned long len = 0;

	    if(!m->hdr){
		t = mail_fetch_header(stream, i, NIL, NIL, &len, FT_PEEK);
		if(t && len && len <= OC_MAX_HDR){
		    m->hdr = oc_dup(t, len);
		    m->hdrlen = len;
		}
	    }

	    len = 0;
	    t = mail_fetch_text(stream, i, NIL, &len, FT_PEEK);
	    if(t && len && len <= msg_cap){
		m->body = oc_dup(t, len);
		m->bodylen = len;
		f->body_bytes += len;
		fetched++;
	    }
	}

	if(m->body)
	  m->wanted = 0;
    }

    if(we_cancel)
      cancel_busy_cue(-1);

    /*
     * Drop entries the server no longer has, but only when we saw the
     * whole folder.  Entries with no header are useless either way.
     */
    for(j = 0; j < f->n; ){
	OC_MSG *m = &f->msgs[j];

	if((complete && !m->live) || !m->hdr)
	  oc_remove_msg(f, j);
	else
	  j++;
    }

    oc_enforce_folder_cap(f);
    f->dirty = 1;
    dprint((3, "offcache: learned %s: %lu cached, %lu new, %lu bodies fetched%s\n",
	    mbx, f->n, added, fetched, complete ? "" : " (partial)"));
}


/*----------------------------------------------------------------------
   Applying offline changes to the live folder
  ----*/

static void
oc_apply_seq(MAILSTREAM *stream, unsigned long *uids, unsigned long n,
	     char *flag, int set)
{
    char seq[OC_SEQ_CHUNK * 12 + 8], *p;
    unsigned long i = 0, k;

    while(i < n){
	p = seq;
	for(k = 0; i < n && k < OC_SEQ_CHUNK; i++, k++)
	  p += snprintf(p, sizeof(seq) - (p - seq), "%s%lu", k ? "," : "",
			uids[i]);

	mail_flag(stream, seq, flag, ST_UID | (set ? ST_SET : 0L));
    }
}


static void
oc_expunge_seq(MAILSTREAM *stream, unsigned long *uids, unsigned long n)
{
    char seq[OC_SEQ_CHUNK * 12 + 8], *p;
    unsigned long i = 0, k;

    while(i < n){
	p = seq;
	for(k = 0; i < n && k < OC_SEQ_CHUNK; i++, k++)
	  p += snprintf(p, sizeof(seq) - (p - seq), "%s%lu", k ? "," : "",
			uids[i]);

	mail_expunge_full(stream, seq, EX_UID);
    }
}


void
offcache_apply_pending(MAILSTREAM *stream)
{
    OC_FOLDER *f;
    char *mbx;
    unsigned long i, k, pending = 0;
    unsigned long *setu, *clru, nset, nclr;
    static struct { unsigned long bit; char *name; } sysflags[] = {
	{OCF_SEEN,	"\\Seen"},
	{OCF_DELETED,	"\\Deleted"},
	{OCF_FLAGGED,	"\\Flagged"},
	{OCF_ANSWERED,	"\\Answered"},
	{OCF_DRAFT,	"\\Draft"},
	{0, NULL}
    };

    if(!offcache_enabled() || !stream || !offcache_cacheable_stream(stream))
      return;

    mbx = stream->original_mailbox ? stream->original_mailbox : stream->mailbox;
    f = oc_find(mbx);
    if(!f)
      return;

    for(i = 0; i < f->n; i++)
      if(f->msgs[i].dirty || f->msgs[i].user_dirty)
	pending++;

    pending += f->ntomb;
    if(!pending)
      return;

    if(f->uidvalidity && stream->uid_validity
       && f->uidvalidity != stream->uid_validity){
	q_status_message1(SM_ORDER | SM_DING, 3, 5,
	  _("Folder was reset on the server; %s offline change(s) discarded"),
	  comatose((long) pending));
	oc_free_folder_contents(f);
	f->dirty = 1;
	return;
    }

    setu = (unsigned long *) fs_get((f->n + 1) * sizeof(unsigned long));
    clru = (unsigned long *) fs_get((f->n + 1) * sizeof(unsigned long));

    for(k = 0; sysflags[k].name; k++){
	nset = nclr = 0;
	for(i = 0; i < f->n; i++){
	    OC_MSG *m = &f->msgs[i];

	    if(!(m->dirty & sysflags[k].bit) || !m->uid)
	      continue;

	    if(m->flags & sysflags[k].bit)
	      setu[nset++] = m->uid;
	    else
	      clru[nclr++] = m->uid;
	}

	if(nset)
	  oc_apply_seq(stream, setu, nset, sysflags[k].name, 1);

	if(nclr)
	  oc_apply_seq(stream, clru, nclr, sysflags[k].name, 0);
    }

    for(k = 0; k < NUSERFLAGS; k++){
	unsigned long bit = 1UL << k;

	if(!f->keywords[k])
	  continue;

	nset = nclr = 0;
	for(i = 0; i < f->n; i++){
	    OC_MSG *m = &f->msgs[i];

	    if(!(m->user_dirty & bit) || !m->uid)
	      continue;

	    if(m->user_flags & bit)
	      setu[nset++] = m->uid;
	    else
	      clru[nclr++] = m->uid;
	}

	if(nset)
	  oc_apply_seq(stream, setu, nset, f->keywords[k], 1);

	if(nclr)
	  oc_apply_seq(stream, clru, nclr, f->keywords[k], 0);
    }

    for(i = 0; i < f->n; i++)
      f->msgs[i].dirty = f->msgs[i].user_dirty = 0;

    if(f->ntomb){
	oc_apply_seq(stream, f->tomb, f->ntomb, "\\Deleted", 1);
	if(LEVELUIDPLUS(stream)){
	    oc_expunge_seq(stream, f->tomb, f->ntomb);
	    q_status_message1(SM_ORDER, 0, 3,
			      _("Removed %s message(s) deleted offline"),
			      comatose((long) f->ntomb));
	}
	else
	  q_status_message1(SM_ORDER, 0, 4,
	    _("%s message(s) deleted offline are marked deleted; expunge to remove them"),
	    comatose((long) f->ntomb));

	for(i = 0; i < f->ntomb; i++){
	    OC_MSG *m = oc_find_uid(f, f->tomb[i]);

	    if(m)
	      oc_remove_msg(f, (unsigned long) (m - f->msgs));
	}

	f->ntomb = 0;
    }

    fs_give((void **) &setu);
    fs_give((void **) &clru);
    f->dirty = 1;
    dprint((2, "offcache: applied %lu offline change(s) to %s\n", pending, mbx));
}


/*----------------------------------------------------------------------
   c-client driver for "#offcache#{host}folder"
  ----*/

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
    f = oc_get(real);
    if(!f)
      return(NIL);

    f->last_used = (unsigned long) time(NULL);

    local = (OCLOCAL *) fs_get(sizeof(OCLOCAL));
    memset(local, 0, sizeof(*local));
    local->folder = f;
    stream->local = local;
    stream->inbox = 0;
    stream->rdonly = NIL;
    stream->perm_seen = stream->perm_deleted = T;
    stream->perm_flagged = stream->perm_answered = stream->perm_draft = T;
    stream->perm_user_flags = 0;
    stream->kwd_create = T;
    stream->uid_validity = f->uidvalidity ? f->uidvalidity : 1;
    stream->uid_last = f->uidlast;
    stream->sequence++;

    for(i = 0; i < NUSERFLAGS; i++){
	if(stream->user_flags[i])
	  fs_give((void **) &stream->user_flags[i]);

	if(f->keywords[i]){
	    stream->user_flags[i] = cpystr(f->keywords[i]);
	    stream->perm_user_flags |= (1UL << i);
	}
    }

    mail_exists(stream, f->n);
    for(i = 1; i <= f->n; i++){
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
    static unsigned long last_stub_uid = 0;

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
	m->dirty |= OCF_SEEN;
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
	m->wanted = 1;
	if(m->uid != last_stub_uid){
	    last_stub_uid = m->uid;
	    q_status_message(SM_ORDER, 0, 3,
		_("Body not in offline cache; Sync to fetch it"));
	}
    }

    INIT(bs, mail_string, (void *) p, n);
    return(LONGT);
}


void
offcache_flagmsg(MAILSTREAM *stream, MESSAGECACHE *elt)
{
    OC_MSG *m, tmp;

    if(!stream || !LOCAL || !LOCAL->folder || !elt || !elt->valid)
      return;

    m = oc_find_uid(LOCAL->folder, elt->private.uid);
    if(!m)
      return;

    oc_sync_keywords(LOCAL->folder, stream);
    memset(&tmp, 0, sizeof(tmp));
    oc_copy_elt_flags(&tmp, elt);
    m->dirty |= (m->flags ^ tmp.flags) & OCF_DIRTYABLE;
    m->user_dirty |= (m->user_flags ^ tmp.user_flags);
    m->flags = tmp.flags;
    m->user_flags = tmp.user_flags;
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
    unsigned long i;

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
		if(f->msgs[idx].uid)
		  oc_add_tomb(f, f->msgs[idx].uid);

		oc_remove_msg(f, idx);
		f->dirty = 1;
	    }

	    mail_expunged(stream, i);
	}
	else
	  i++;
    }

    return(LONGT);
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


/*----------------------------------------------------------------------
   In-memory helpers
  ----*/

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


static void
oc_free_folder_contents(OC_FOLDER *f)
{
    unsigned long j;

    if(!f)
      return;

    for(j = 0; j < f->n; j++)
      oc_free_msg(&f->msgs[j]);

    if(f->msgs)
      fs_give((void **) &f->msgs);

    if(f->tomb)
      fs_give((void **) &f->tomb);

    f->n = f->alloc = 0;
    f->ntomb = f->tomb_alloc = 0;
    f->body_bytes = 0;
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

    if(!mailbox || !*mailbox)
      return(NULL);

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


static void
oc_remove_msg(OC_FOLDER *f, unsigned long idx)
{
    if(!f || idx >= f->n)
      return;

    if(f->msgs[idx].bodylen > f->body_bytes)
      f->body_bytes = 0;
    else
      f->body_bytes -= f->msgs[idx].bodylen;

    oc_free_msg(&f->msgs[idx]);
    if(idx + 1 < f->n)
      memmove(&f->msgs[idx], &f->msgs[idx + 1],
	      (f->n - idx - 1) * sizeof(OC_MSG));

    f->n--;
    memset(&f->msgs[f->n], 0, sizeof(OC_MSG));
}


static void
oc_add_tomb(OC_FOLDER *f, unsigned long uid)
{
    unsigned long i;

    if(!f || !uid)
      return;

    for(i = 0; i < f->ntomb; i++)
      if(f->tomb[i] == uid)
	return;

    if(f->ntomb >= f->tomb_alloc){
	unsigned long na = f->tomb_alloc ? f->tomb_alloc * 2 : 16;
	unsigned long *nt = (unsigned long *) fs_get(na * sizeof(unsigned long));

	if(f->tomb){
	    memcpy(nt, f->tomb, f->ntomb * sizeof(unsigned long));
	    fs_give((void **) &f->tomb);
	}

	f->tomb = nt;
	f->tomb_alloc = na;
    }

    f->tomb[f->ntomb++] = uid;
}


/*
 * Keep the folder's keyword table in step with the stream's.
 */
static void
oc_sync_keywords(OC_FOLDER *f, MAILSTREAM *stream)
{
    unsigned long i;

    if(!f || !stream)
      return;

    for(i = 0; i < NUSERFLAGS; i++){
	char *name = stream->user_flags[i];

	if(name && (!f->keywords[i] || strcmp(f->keywords[i], name))){
	    if(f->keywords[i])
	      fs_give((void **) &f->keywords[i]);

	    f->keywords[i] = cpystr(name);
	    f->dirty = 1;
	}
    }
}


static void
oc_enforce_folder_cap(OC_FOLDER *f)
{
    unsigned long cap = oc_folder_cap(), i;

    if(!f)
      return;

    for(i = 0; i < f->n && f->body_bytes > cap; i++){
	OC_MSG *m = &f->msgs[i];

	if(m->body){
	    if(m->bodylen > f->body_bytes)
	      f->body_bytes = 0;
	    else
	      f->body_bytes -= m->bodylen;

	    fs_give((void **) &m->body);
	    m->bodylen = 0;
	    f->dirty = 1;
	}
    }
}


static void
oc_enforce_total_cap(void)
{
    unsigned long cap = oc_total_cap(), total;
    OC_FOLDER *f, *victim;

    for(;;){
	total = 0;
	victim = NULL;
	for(f = oc_folders; f; f = f->next){
	    total += f->body_bytes;
	    if(f->body_bytes
	       && (!victim || f->last_used < victim->last_used))
	      victim = f;
	}

	if(total <= cap || !victim)
	  break;

	{
	    unsigned long i;

	    for(i = 0; i < victim->n; i++){
		OC_MSG *m = &victim->msgs[i];

		if(m->body){
		    fs_give((void **) &m->body);
		    m->bodylen = 0;
		}
	    }

	    victim->body_bytes = 0;
	    victim->dirty = 1;
	    dprint((2, "offcache: dropped bodies of %s to stay under %lu MB\n",
		    victim->mailbox, cap / OC_MB));
	}
    }
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


/*
 * Read just the mailbox name stored in a cache file.
 */
static char *
oc_file_mailbox(char *path)
{
    FILE *fp;
    OC_IO io;
    char magic[8], *mbx;
    unsigned long mlen;

    fp = our_fopen(path, "rb");
    if(!fp)
      return(NULL);

    io.fp = fp;
    io.hash = 0;
    if(fread(magic, 1, OC_MAGIC_LEN, fp) != OC_MAGIC_LEN
       || (memcmp(magic, OC_MAGIC1, OC_MAGIC_LEN)
	   && memcmp(magic, OC_MAGIC2, OC_MAGIC_LEN))
       || !oc_r32(&io, &mlen) || mlen == 0 || mlen > MAILTMPLEN){
	fclose(fp);
	return(NULL);
    }

    mbx = oc_rbytes(&io, mlen);
    fclose(fp);
    return(mbx);
}


/*
 * The file name is a hash of the mailbox name; if another folder
 * already owns that name, use a numbered variant.
 */
static void
oc_path_for(char *dst, size_t dstlen, char *mailbox)
{
    unsigned h = oc_hash(mailbox);
    int k;

    for(k = 0; k < OC_PATH_TRIES; k++){
	char *owner;

	if(k == 0)
	  snprintf(dst, dstlen, "%s/%08x.aoc", oc_dir, h);
	else
	  snprintf(dst, dstlen, "%s/%08x-%d.aoc", oc_dir, h, k);

	if(!can_access(dst, ACCESS_EXISTS))
	  ;				/* exists: check the owner */
	else
	  return;

	owner = oc_file_mailbox(dst);
	if(owner){
	    int same = !strcmp(owner, mailbox);

	    fs_give((void **) &owner);
	    if(same)
	      return;
	}
	else
	  return;			/* unreadable: take it over */
    }
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


/*----------------------------------------------------------------------
   File I/O with a running FNV-1a checksum
  ----*/

static void
oc_hash_bytes(unsigned long *h, unsigned char *b, unsigned long n)
{
    while(n--){
	*h ^= *b++;
	*h = (*h * 16777619UL) & 0xffffffffUL;
    }
}


static int
oc_w32(OC_IO *io, unsigned long v)
{
    unsigned char b[4];

    b[0] = (unsigned char) ((v >> 24) & 0xff);
    b[1] = (unsigned char) ((v >> 16) & 0xff);
    b[2] = (unsigned char) ((v >> 8) & 0xff);
    b[3] = (unsigned char) (v & 0xff);
    oc_hash_bytes(&io->hash, b, 4);
    return(fwrite(b, 1, 4, io->fp) == 4);
}


static int
oc_r32(OC_IO *io, unsigned long *vp)
{
    unsigned char b[4];

    if(fread(b, 1, 4, io->fp) != 4)
      return(0);

    oc_hash_bytes(&io->hash, b, 4);
    *vp = ((unsigned long) b[0] << 24) | ((unsigned long) b[1] << 16)
	  | ((unsigned long) b[2] << 8) | (unsigned long) b[3];
    return(1);
}


static int
oc_wbytes(OC_IO *io, char *s, unsigned long n)
{
    if(!n)
      return(1);

    if(!s)
      return(0);

    oc_hash_bytes(&io->hash, (unsigned char *) s, n);
    return(fwrite(s, 1, n, io->fp) == n);
}


static char *
oc_rbytes(OC_IO *io, unsigned long n)
{
    char *s;

    if(!n)
      return(NULL);

    s = (char *) fs_get(n + 1);
    if(fread(s, 1, n, io->fp) != n){
	fs_give((void **) &s);
	return(NULL);
    }

    oc_hash_bytes(&io->hash, (unsigned char *) s, n);
    s[n] = '\0';
    return(s);
}


static int
oc_write_folder(OC_FOLDER *f)
{
    FILE *fp = NULL;
    OC_IO io;
    char path[MAILTMPLEN], tmp[MAILTMPLEN];
    unsigned long i, mlen, nkw = 0, nrec = 0;
    int fd;

    if(!f || !f->mailbox || !oc_dir_ok)
      return(-1);

    if(create_mail_dir(oc_dir) < 0 && errno != EEXIST)
      return(-1);

    oc_path_for(path, sizeof(path), f->mailbox);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
#ifdef	_WINDOWS
    fp = our_fopen(tmp, "wb");
#else
    fd = our_open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if(fd >= 0)
      fp = fdopen(fd, "wb");

    if(!fp && fd >= 0)
      close(fd);
#endif
    if(!fp)
      return(-1);

    io.fp = fp;
    io.hash = 2166136261UL;

    for(i = 0; i < NUSERFLAGS; i++)
      if(f->keywords[i])
	nkw++;

    for(i = 0; i < f->n; i++)
      if(f->msgs[i].hdr && f->msgs[i].hdrlen)
	nrec++;

    mlen = strlen(f->mailbox);
    if(!oc_wbytes(&io, OC_MAGIC2, OC_MAGIC_LEN)
       || !oc_w32(&io, mlen) || !oc_wbytes(&io, f->mailbox, mlen)
       || !oc_w32(&io, f->uidvalidity)
       || !oc_w32(&io, f->uidlast)
       || !oc_w32(&io, f->last_used)
       || !oc_w32(&io, nkw))
      goto fail;

    for(i = 0; i < NUSERFLAGS; i++){
	if(!f->keywords[i])
	  continue;

	if(!oc_w32(&io, i)
	   || !oc_w32(&io, strlen(f->keywords[i]))
	   || !oc_wbytes(&io, f->keywords[i], strlen(f->keywords[i])))
	  goto fail;
    }

    if(!oc_w32(&io, f->ntomb))
      goto fail;

    for(i = 0; i < f->ntomb; i++)
      if(!oc_w32(&io, f->tomb[i]))
	goto fail;

    if(!oc_w32(&io, nrec))
      goto fail;

    for(i = 0; i < f->n; i++){
	OC_MSG *m = &f->msgs[i];
	unsigned long packed;

	if(!(m->hdr && m->hdrlen))
	  continue;

	packed = (m->flags & 0xff)
		 | ((m->day & 0x1f) << 8)
		 | ((m->month & 0x0f) << 13)
		 | ((m->year & 0x7f) << 17);
	if(!oc_w32(&io, m->uid)
	   || !oc_w32(&io, m->size)
	   || !oc_w32(&io, packed)
	   || !oc_w32(&io, m->user_flags)
	   || !oc_w32(&io, ((m->hours & 0x1f) << 16)
			   | ((m->minutes & 0x3f) << 8)
			   | (m->seconds & 0x3f))
	   || !oc_w32(&io, m->dirty)
	   || !oc_w32(&io, m->user_dirty)
	   || !oc_w32(&io, m->hdrlen)
	   || !oc_w32(&io, m->body ? m->bodylen : 0)
	   || !oc_wbytes(&io, m->hdr, m->hdrlen)
	   || !oc_wbytes(&io, m->body, m->body ? m->bodylen : 0))
	  goto fail;
    }

    {
	unsigned long h = io.hash;

	if(!oc_w32(&io, h))
	  goto fail;
    }

    if(fflush(fp) != 0)
      goto fail;

#ifndef	_WINDOWS
    (void) fsync(fileno(fp));
#endif
    if(fclose(fp) != 0){
	fp = NULL;
	goto fail;
    }

    fp = NULL;
    if(rename_file(tmp, path) < 0){
	our_unlink(tmp);
	return(-1);
    }

    return(0);

  fail:
    if(fp)
      fclose(fp);

    our_unlink(tmp);
    return(-1);
}


static int
oc_read_folder(char *path)
{
    FILE *fp;
    OC_IO io;
    struct stat sb;
    char magic[8], *mbx = NULL;
    unsigned long mlen, uidvalidity, uidlast, last_used = 0, nkw = 0, ntomb = 0;
    unsigned long n, i, body_bytes = 0, cap, remaining, recmin;
    long max_age;
    int v2;
    OC_FOLDER *f = NULL;

    fp = our_fopen(path, "rb");
    if(!fp)
      return(-1);

    if(fstat(fileno(fp), &sb) < 0){
	fclose(fp);
	return(-1);
    }

    io.fp = fp;
    io.hash = 2166136261UL;
    if(fread(magic, 1, OC_MAGIC_LEN, fp) != OC_MAGIC_LEN){
	fclose(fp);
	return(-1);
    }

    oc_hash_bytes(&io.hash, (unsigned char *) magic, OC_MAGIC_LEN);
    if(!memcmp(magic, OC_MAGIC2, OC_MAGIC_LEN))
      v2 = 1;
    else if(!memcmp(magic, OC_MAGIC1, OC_MAGIC_LEN))
      v2 = 0;
    else{
	fclose(fp);
	return(-1);
    }

    if(!oc_r32(&io, &mlen) || mlen == 0 || mlen > MAILTMPLEN){
	fclose(fp);
	return(-1);
    }

    mbx = oc_rbytes(&io, mlen);
    if(!mbx
       || !oc_r32(&io, &uidvalidity)
       || !oc_r32(&io, &uidlast)
       || (v2 && (!oc_r32(&io, &last_used) || !oc_r32(&io, &nkw)))){
	if(mbx)
	  fs_give((void **) &mbx);

	fclose(fp);
	return(-1);
    }

    max_age = oc_max_age_secs();
    if(v2 && max_age > 0 && last_used
       && (unsigned long) time(NULL) > last_used
       && (long) ((unsigned long) time(NULL) - last_used) > max_age){
	dprint((2, "offcache: %s is older than the age limit, removing\n", mbx));
	fs_give((void **) &mbx);
	fclose(fp);
	our_unlink(path);
	return(0);
    }

    f = oc_get(mbx);
    fs_give((void **) &mbx);
    if(!f){
	fclose(fp);
	return(-1);
    }

    oc_free_folder_contents(f);
    for(i = 0; i < NUSERFLAGS; i++)
      if(f->keywords[i])
	fs_give((void **) &f->keywords[i]);

    f->uidvalidity = uidvalidity;
    f->uidlast = uidlast;
    f->last_used = last_used;

    if(v2){
	if(nkw > NUSERFLAGS)
	  goto fail;

	for(i = 0; i < nkw; i++){
	    unsigned long idx, len;
	    char *name;

	    if(!oc_r32(&io, &idx) || !oc_r32(&io, &len)
	       || idx >= NUSERFLAGS || len == 0 || len > MAILTMPLEN)
	      goto fail;

	    name = oc_rbytes(&io, len);
	    if(!name)
	      goto fail;

	    if(f->keywords[idx])
	      fs_give((void **) &f->keywords[idx]);

	    f->keywords[idx] = name;
	}

	if(!oc_r32(&io, &ntomb))
	  goto fail;

	remaining = (unsigned long) sb.st_size - (unsigned long) ftell(fp);
	if(ntomb > remaining / 4)
	  goto fail;

	for(i = 0; i < ntomb; i++){
	    unsigned long uid;

	    if(!oc_r32(&io, &uid))
	      goto fail;

	    oc_add_tomb(f, uid);
	}
    }

    if(!oc_r32(&io, &n))
      goto fail;

    recmin = v2 ? OC_REC_MIN_V2 : OC_REC_MIN_V1;
    remaining = (unsigned long) sb.st_size - (unsigned long) ftell(fp);
    if(n > remaining / recmin)
      goto fail;

    if(n > 0){
	f->msgs = (OC_MSG *) fs_get((n + 8) * sizeof(OC_MSG));
	memset(f->msgs, 0, (n + 8) * sizeof(OC_MSG));
	f->alloc = n + 8;
    }

    cap = oc_folder_cap();
    for(i = 0; i < n; i++){
	OC_MSG *m = oc_add_msg(f);
	unsigned long packed, hms;

	if(!oc_r32(&io, &m->uid)
	   || !oc_r32(&io, &m->size)
	   || !oc_r32(&io, &packed)
	   || !oc_r32(&io, &m->user_flags)
	   || !oc_r32(&io, &hms)
	   || (v2 && (!oc_r32(&io, &m->dirty) || !oc_r32(&io, &m->user_dirty)))
	   || !oc_r32(&io, &m->hdrlen)
	   || !oc_r32(&io, &m->bodylen))
	  goto fail;

	m->flags = packed & 0xff;
	m->day = (packed >> 8) & 0x1f;
	m->month = (packed >> 13) & 0x0f;
	m->year = (packed >> 17) & 0x7f;
	m->hours = (hms >> 16) & 0x1f;
	m->minutes = (hms >> 8) & 0x3f;
	m->seconds = hms & 0x3f;
	m->dirty &= OCF_DIRTYABLE;

	if(m->hdrlen > OC_MAX_HDR || m->bodylen > OC_MAX_MSG_BODY)
	  goto fail;

	if(m->hdrlen){
	    m->hdr = oc_rbytes(&io, m->hdrlen);
	    if(!m->hdr)
	      goto fail;
	}

	if(m->bodylen){
	    if(body_bytes + m->bodylen > cap){
		/* over the limit: skip the body but keep the checksum right */
		char *skip = oc_rbytes(&io, m->bodylen);

		if(!skip)
		  goto fail;

		fs_give((void **) &skip);
		m->bodylen = 0;
	    }
	    else{
		m->body = oc_rbytes(&io, m->bodylen);
		if(!m->body)
		  goto fail;

		body_bytes += m->bodylen;
	    }
	}
    }

    if(v2){
	unsigned long computed = io.hash, stored;

	if(!oc_r32(&io, &stored) || stored != computed){
	    dprint((1, "offcache: checksum mismatch in %s\n", path));
	    goto fail;
	}
    }

    f->body_bytes = body_bytes;
    f->dirty = 0;
    fclose(fp);
    return(0);

  fail:
    fclose(fp);
    if(f){
	oc_free_folder_contents(f);
	f->dirty = 0;
    }

    return(-1);
}
