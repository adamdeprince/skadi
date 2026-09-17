/*
 * ========================================================================
 * Copyright 2026 Adam DePrince
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * ========================================================================
 *
 * Local outbox.  Messages that could not be sent are written, exactly
 * as a postponed message would be, to the local folder ~/.alpine-outbox.
 * When a live connection exists they are read back with the same code
 * that resumes a postponed message and handed to call_mailer(), so the
 * configured SMTP or sendmail path, roles, and Fcc all apply.
 *
 * A message that fails OUTBOX_MAX_TRIES times is left alone and reported;
 * the retry counts live in ~/.alpine-outbox-tries, keyed by Message-ID.
 */

#include "../pith/headers.h"
#include "../pith/outbox.h"
#include "../pith/state.h"
#include "../pith/conf.h"
#include "../pith/status.h"
#include "../pith/store.h"
#include "../pith/send.h"
#include "../pith/stream.h"
#include "../pith/busy.h"
#include "../pith/reply.h"
#include "../pith/pattern.h"
#include "../pith/util.h"
#include "../pith/offcache.h"
#include "../pith/charconv/filesys.h"

#ifndef	_WINDOWS
#include <unistd.h>
#include <fcntl.h>
#endif
#include <sys/stat.h>
#include <errno.h>
#include <time.h>


typedef struct ob_try {
    char	 *mid;
    int		  n;
    struct ob_try *next;
} OB_TRY;


static int	 ob_flushing = 0;
static OB_TRY	*ob_tries = NULL;
static int	 ob_tries_loaded = 0;


static char	*ob_home_file(char *, size_t, char *);
static char	*ob_tries_path(void);
static void	 ob_tries_load(void);
static void	 ob_tries_save(void);
static void	 ob_tries_free(void);
static int	 ob_tries_get(char *);
static void	 ob_tries_set(char *, int);
static void	 ob_tries_remove(char *);


static char *
ob_home_file(char *dst, size_t dstlen, char *leaf)
{
    char *home = (ps_global && ps_global->home_dir) ? ps_global->home_dir : "";

    snprintf(dst, dstlen, "%s%s%s",
	     home, (home[0] && home[strlen(home) - 1] == '/') ? "" : "/", leaf);
    dst[dstlen - 1] = '\0';
    return(dst);
}


char *
outbox_path(void)
{
    static char path[MAILTMPLEN];

    return(ob_home_file(path, sizeof(path), ".alpine-outbox"));
}


static char *
ob_tries_path(void)
{
    static char path[MAILTMPLEN];

    return(ob_home_file(path, sizeof(path), ".alpine-outbox-tries"));
}


/*----------------------------------------------------------------------
   Retry bookkeeping
  ----*/

static void
ob_tries_free(void)
{
    OB_TRY *t, *next;

    for(t = ob_tries; t; t = next){
	next = t->next;
	if(t->mid)
	  fs_give((void **) &t->mid);

	fs_give((void **) &t);
    }

    ob_tries = NULL;
}


static void
ob_tries_load(void)
{
    FILE *fp;
    char line[MAILTMPLEN];

    if(ob_tries_loaded)
      return;

    ob_tries_loaded = 1;
    ob_tries_free();
    fp = our_fopen(ob_tries_path(), "r");
    if(!fp)
      return;

    while(fgets(line, sizeof(line), fp)){
	char *p = line, *mid;
	int n;

	n = atoi(p);
	while(*p && *p != '\t')
	  p++;

	if(*p != '\t')
	  continue;

	mid = ++p;
	while(*p && *p != '\r' && *p != '\n')
	  p++;

	*p = '\0';
	if(*mid && n > 0)
	  ob_tries_set(mid, n);
    }

    fclose(fp);
}


static void
ob_tries_save(void)
{
    FILE *fp = NULL;
    OB_TRY *t;
    char *path = ob_tries_path();

    if(!ob_tries){
	our_unlink(path);
	return;
    }

#ifdef	_WINDOWS
    fp = our_fopen(path, "w");
#else
    {
	int fd = our_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	if(fd >= 0 && !(fp = fdopen(fd, "w")))
	  close(fd);
    }
#endif
    if(!fp)
      return;

    for(t = ob_tries; t; t = t->next)
      if(t->mid && t->n > 0)
	fprintf(fp, "%d\t%s\n", t->n, t->mid);

    fclose(fp);
}


static int
ob_tries_get(char *mid)
{
    OB_TRY *t;

    if(!mid)
      return(0);

    for(t = ob_tries; t; t = t->next)
      if(t->mid && !strcmp(t->mid, mid))
	return(t->n);

    return(0);
}


static void
ob_tries_set(char *mid, int n)
{
    OB_TRY *t;

    if(!mid)
      return;

    for(t = ob_tries; t; t = t->next)
      if(t->mid && !strcmp(t->mid, mid)){
	  t->n = n;
	  return;
      }

    t = (OB_TRY *) fs_get(sizeof(OB_TRY));
    memset(t, 0, sizeof(*t));
    t->mid = cpystr(mid);
    t->n = n;
    t->next = ob_tries;
    ob_tries = t;
}


static void
ob_tries_remove(char *mid)
{
    OB_TRY *t, **pp;

    if(!mid)
      return;

    for(pp = &ob_tries; (t = *pp) != NULL; pp = &t->next)
      if(t->mid && !strcmp(t->mid, mid)){
	  *pp = t->next;
	  fs_give((void **) &t->mid);
	  fs_give((void **) &t);
	  return;
      }
}


/*----------------------------------------------------------------------
   Queue and count
  ----*/

/*
 * Number of messages waiting in the outbox, or 0 if there is no outbox.
 */
int
outbox_count(void)
{
#ifdef	_WINDOWS
    return(0);
#else
    struct stat sb;
    MAILSTREAM *stream;
    unsigned long i;
    int n = 0;
    char *path = outbox_path();

    if(our_stat(path, &sb) < 0 || sb.st_size == 0)
      return(0);

    ps_global->noshow_error = 1;
    stream = pine_mail_open(NULL, path, OP_READONLY | OP_SILENT, NULL);
    ps_global->noshow_error = 0;
    if(!stream)
      return(0);

    for(i = 1; i <= stream->nmsgs; i++){
	MESSAGECACHE *elt = mail_elt(stream, i);

	if(elt && !elt->deleted)
	  n++;
    }

    pine_mail_close(stream);
    return(n);
#endif
}


/*
 * Write the message to the outbox folder.  Returns 0 on success.
 */
int
outbox_queue(METAENV *header, struct mail_bodystruct *body)
{
#ifdef	_WINDOWS
    (void) header;
    (void) body;
    return(-1);
#else
    if(!header || !body)
      return(-1);

    if(write_draft_to_folder(header, body, outbox_path(),
			     "message to outbox", 1) < 0){
	q_status_message(SM_ORDER | SM_DING, 3, 5,
			 _("Could not write the message to the outbox"));
	return(-1);
    }

    q_status_message1(SM_ORDER, 0, 4,
	_("Message queued in the outbox (%s); it will be sent when a connection is made"),
	outbox_path());
    return(0);
#endif
}


/*----------------------------------------------------------------------
   Flush
  ----*/

/*
 * Send whatever is queued.  Returns the number of messages sent.
 */
int
outbox_flush(void)
{
#ifdef	_WINDOWS
    return(0);
#else
    MAILSTREAM *stream;
    unsigned long *uids = NULL, nuids = 0, i;
    int sent = 0, failed = 0, stuck = 0, we_cancel = 0;
    char *path = outbox_path();
    struct stat sb;

    if(ob_flushing || !ps_global)
      return(0);

    if(our_stat(path, &sb) < 0 || sb.st_size == 0)
      return(0);

    ob_flushing = 1;
    ob_tries_load();

    ps_global->noshow_error = 1;
    stream = pine_mail_open(NULL, path, OP_SILENT, NULL);
    ps_global->noshow_error = 0;
    if(!stream){
	ob_flushing = 0;
	return(0);
    }

    if(stream->nmsgs){
	uids = (unsigned long *) fs_get(stream->nmsgs * sizeof(unsigned long));
	for(i = 1; i <= stream->nmsgs; i++){
	    MESSAGECACHE *elt = mail_elt(stream, i);

	    if(elt && !elt->deleted)
	      uids[nuids++] = mail_uid(stream, i);
	}
    }

    if(nuids)
      we_cancel = busy_cue(_("Sending queued mail"), NULL, 1);

    for(i = 0; i < nuids && stream; i++){
	unsigned long msgno;
	ENVELOPE *env, *outgoing = NULL;
	BODY *body = NULL;
	STORE_S *so;
	METAENV *header;
	PINEFIELD *custom = NULL;
	ACTION_S *role = NULL;
	CONTEXT_S *fcc_cntxt = NULL;
	char *fcc = NULL, *lcc = NULL, *mid = NULL, *alt_send = NULL;
	char **alt_smtp = NULL;
	int rv, result, tries;

	msgno = mail_msgno(stream, uids[i]);
	if(!msgno)
	  continue;

	env = pine_mail_fetchstructure(stream, msgno, NULL);
	if(env && env->message_id)
	  mid = cpystr(env->message_id);

	tries = ob_tries_get(mid);
	if(tries >= OUTBOX_MAX_TRIES){
	    stuck++;
	    if(mid)
	      fs_give((void **) &mid);

	    continue;
	}

	so = so_get(CharStar, NULL, EDIT_ACCESS);
	if(!so){
	    if(mid)
	      fs_give((void **) &mid);

	    break;
	}

	rv = redraft_work(&stream, (long) msgno, &outgoing, &body, &fcc, &lcc,
			  NULL, NULL, &custom, &role, REDRAFT_NONE, so);
	if(!rv || !stream || !outgoing || !body){
	    if(!body)
	      so_give(&so);

	    if(outgoing)
	      mail_free_envelope(&outgoing);

	    if(body)
	      pine_free_body(&body);

	    if(custom)
	      free_customs(custom);

	    if(role)
	      free_action(&role);

	    if(fcc)
	      fs_give((void **) &fcc);

	    if(lcc)
	      fs_give((void **) &lcc);

	    ob_tries_set(mid ? mid : "?", tries + 1);
	    if(mid)
	      fs_give((void **) &mid);

	    failed++;
	    break;
	}

	if(body->type == TYPETEXT)
	  set_mime_type_by_grope(body);

	header = pine_new_env(outgoing, &fcc, NULL, custom);

	if(role && role->send_server
	   && !(ps_global->FIX_SENDING_SERVER && ps_global->FIX_SENDING_SERVER[0]))
	  alt_send = role->send_server;

	if(role && role->smtp
	   && !(ps_global->FIX_SMTP_SERVER && ps_global->FIX_SMTP_SERVER[0]))
	  alt_smtp = role->smtp;

	if(!commence_fcc(fcc, &fcc_cntxt, 1)){
	    /* fcc folder unusable: send anyway, without a copy */
	    lmc.so = NULL;
	}

	result = call_mailer(header, body, alt_send, alt_smtp, 0, NULL, NULL);

	if(result > 0){
	    (void) wrapup_fcc(fcc, fcc_cntxt, NULL, body);
	    mail_flag(stream, long2string((long) msgno), "\\DELETED", ST_SET);
	    ob_tries_remove(mid);
	    sent++;
	}
	else{
	    if(lmc.so)
	      so_give(&lmc.so);

	    ob_tries_set(mid ? mid : "?", tries + 1);
	    if(tries + 1 >= OUTBOX_MAX_TRIES)
	      stuck++;
	    else
	      failed++;
	}

	pine_free_env(&header);
	mail_free_envelope(&outgoing);
	pine_free_body(&body);
	if(custom)
	  free_customs(custom);

	if(role)
	  free_action(&role);

	if(fcc)
	  fs_give((void **) &fcc);

	if(lcc)
	  fs_give((void **) &lcc);

	if(mid)
	  fs_give((void **) &mid);

	if(result <= 0)
	  break;			/* one failure usually means no link */
    }

    if(we_cancel)
      cancel_busy_cue(-1);

    if(uids)
      fs_give((void **) &uids);

    if(stream){
	if(sent){
	    ps_global->expunge_in_progress = 1;
	    mail_expunge(stream);
	    ps_global->expunge_in_progress = 0;
	}

	pine_mail_close(stream);
    }

    ob_tries_save();
    ob_flushing = 0;

    if(sent)
      q_status_message1(SM_ORDER, 0, 4,
			_("Sent %s queued message(s) from the outbox"),
			comatose((long) sent));

    if(failed)
      q_status_message1(SM_ORDER | SM_DING, 3, 5,
	_("Could not send %s queued message(s); will try again on the next connection"),
	comatose((long) failed));

    if(stuck)
      q_status_message2(SM_ORDER | SM_DING, 3, 6,
	_("%s message(s) are stuck in the outbox after %s attempts; open ~/.alpine-outbox to resend or delete them"),
	comatose((long) stuck), comatose((long) OUTBOX_MAX_TRIES));

    return(sent);
#endif
}


/*
 * Flush if there is something to flush and we believe the network is up.
 */
int
outbox_flush_pending(void)
{
#ifdef	_WINDOWS
    return(0);
#else
    if(ob_flushing || !offcache_network_ok())
      return(0);

    if(outbox_count() <= 0)
      return(0);

    return(outbox_flush());
#endif
}
