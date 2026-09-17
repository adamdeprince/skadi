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

#ifndef PITH_OFFCACHE_INCLUDED
#define PITH_OFFCACHE_INCLUDED


/*
 * Offline cache for remote IMAP folders.
 *
 * What we have already fetched lives in RAM for the session.  Disk is
 * read once at startup and written when a folder is closed, at exit,
 * and from the signal cleanup path -- and only for folders that
 * changed.  There are no periodic writes.
 *
 * Flag changes, keyword changes and expunges made while reading the
 * cache are remembered per message (and per deleted UID) and applied
 * to the live folder the next time it is opened, provided its
 * UIDVALIDITY is unchanged.
 */


#define	OFFCACHE_PREFIX		"#offcache#"


int	 offcache_enabled(void);
int	 offcache_is_name(char *);
int	 offcache_is_stream(MAILSTREAM *);
int	 offcache_cacheable_name(char *);
int	 offcache_cacheable_stream(MAILSTREAM *);
int	 offcache_has_folder(char *);
int	 offcache_network_ok(void);
void	 offcache_note_live(void);
char	*offcache_name_for(char *);
char	*offcache_real_name(char *);
int	 offcache_skip_remote_probe(char *);
int	 offcache_pending_count(char *);

void	 offcache_link_driver(void);
void	 offcache_init(void);
void	 offcache_load_all(void);
void	 offcache_save_all(void);
void	 offcache_checkpoint(void);
void	 offcache_emergency_save(void);
void	 offcache_learn_stream(MAILSTREAM *);
void	 offcache_learn_stream_quick(MAILSTREAM *);
void	 offcache_apply_pending(MAILSTREAM *);
void	 offcache_note_fetch(MAILSTREAM *, unsigned long);


#endif /* PITH_OFFCACHE_INCLUDED */
