/*
 * ========================================================================
 * Copyright 2026 Adam DePrince
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * ========================================================================
 *
 * Local outbox: RFC822 files written on send failure or while offline.
 * Flushed when a live connection exists.
 */

#include "../pith/headers.h"
#include "../pith/outbox.h"
#include "../pith/state.h"
#include "../pith/conf.h"
#include "../pith/status.h"
#include "../pith/store.h"
#include "../pith/osdep/creatdir.h"
#include "../pith/osdep/rename.h"
#include "../pith/charconv/filesys.h"
#include "../pith/offcache.h"
#include "../pith/stream.h"
#include "../c-client/smtp.h"

#ifndef	_WINDOWS
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#endif
#include <errno.h>


static char *
outbox_home(char *dst, size_t dstlen, char *leaf)
{
    char *home = ps_global && ps_global->home_dir ? ps_global->home_dir : "";

    if(leaf)
      snprintf(dst, dstlen, "%s%s%s/%s",
	       home, home[0] && home[strlen(home)-1] == '/' ? "" : "/",
	       leaf[0] == '.' ? leaf : leaf, "");
    else
      snprintf(dst, dstlen, "%s%s.alpine-outbox",
	       home, home[0] && home[strlen(home)-1] == '/' ? "" : "/");

    return(dst);
}


char *
outbox_dir(void)
{
    static char dir[MAILTMPLEN];

    outbox_home(dir, sizeof(dir), NULL);
    return(dir);
}


char *
local_postponed_path(void)
{
    static char path[MAILTMPLEN];
    char *home = ps_global && ps_global->home_dir ? ps_global->home_dir : "";

    snprintf(path, sizeof(path), "%s%s.alpine-postponed",
	     home, home[0] && home[strlen(home)-1] == '/' ? "" : "/");
    return(path);
}


int
outbox_count(void)
{
#ifndef	_WINDOWS
    DIR *dp;
    struct dirent *de;
    int n = 0;
    char *dir = outbox_dir();

    dp = opendir(dir);
    if(!dp)
      return(0);

    while((de = readdir(dp)) != NULL){
	size_t l = strlen(de->d_name);

	if(l > 4 && !strcmp(de->d_name + l - 4, ".eml"))
	  n++;
    }

    closedir(dp);
    return(n);
#else
    return(0);
#endif
}


int
outbox_queue_store(STORE_S *so, char *note)
{
    FILE *fp;
    char  dir[MAILTMPLEN], path[MAILTMPLEN], *s;
    static unsigned seq = 1;

    if(!so)
      return(-1);

    strncpy(dir, outbox_dir(), sizeof(dir)-1);
    dir[sizeof(dir)-1] = '\0';
    if(create_mail_dir(dir) < 0 && errno != EEXIST)
      return(-1);

    snprintf(path, sizeof(path), "%s/q%lx-%u.eml", dir,
	     (unsigned long) time(NULL), seq++);
    fp = our_fopen(path, "wb");
    if(!fp)
      return(-1);

    so_seek(so, 0L, 0);
    if(so->src == CharStar && (s = (char *) so_text(so)))
      fputs(s, fp);
    else{
	unsigned char c;

	so_seek(so, 0L, 0);
	while(so_readc(&c, so))
	  fputc(c, fp);
    }

    fclose(fp);
    if(note)
      q_status_message1(SM_ORDER, 0, 4, _("Queued in local outbox: %s"), note);

    return(0);
}


int
outbox_queue_file(char *src, char *note)
{
    STORE_S *so;
    int	     rv;

    so = so_get(FileStar, src, READ_ACCESS);
    if(!so)
      return(-1);

    rv = outbox_queue_store(so, note);
    so_give(&so);
    return(rv);
}


void
outbox_flush(void)
{
#ifndef	_WINDOWS
    DIR *dp;
    struct dirent *de;
    char dir[MAILTMPLEN], path[MAILTMPLEN], *servers[2];
    SENDSTREAM *ss = NULL;
    int sent = 0, failed = 0;

    if(!offcache_network_ok())
      return;

    strncpy(dir, outbox_dir(), sizeof(dir)-1);
    dir[sizeof(dir)-1] = '\0';
    dp = opendir(dir);
    if(!dp)
      return;

    if(ps_global->VAR_SMTP_SERVER && ps_global->VAR_SMTP_SERVER[0]
       && ps_global->VAR_SMTP_SERVER[0][0])
      servers[0] = ps_global->VAR_SMTP_SERVER[0];
    else
      servers[0] = "localhost";

    servers[1] = NULL;

    while((de = readdir(dp)) != NULL){
	MAILSTREAM *ms;
	ENVELOPE *env;
	BODY *body = NULL;
	size_t l = strlen(de->d_name);

	if(l < 5 || strcmp(de->d_name + l - 4, ".eml"))
	  continue;

	snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
	ms = pine_mail_open(NULL, path, 0L, NULL);
	if(!ms){
	    failed++;
	    continue;
	}

	env = pine_mail_fetchstructure(ms, 1L, &body);
	if(!(env && body)){
	    pine_mail_actually_close(ms);
	    failed++;
	    continue;
	}

	if(!ss)
	  ss = smtp_open(servers, 0L);

	if(ss && smtp_mail(ss, "MAIL", env, body)){
	    pine_mail_actually_close(ms);
	    our_unlink(path);
	    sent++;
	}
	else{
	    pine_mail_actually_close(ms);
	    failed++;
	    break;
	}
    }

    closedir(dp);
    if(ss)
      smtp_close(ss);

    if(sent)
      q_status_message1(SM_ORDER, 0, 3,
			_("Sent %s queued message(s) from the outbox"),
			comatose((long) sent));
#else
    (void) 0;
#endif
}
