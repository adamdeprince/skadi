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
 * Write-on-exit offline cache for remote folders.
 *
 * What we have already fetched lives in RAM for the session.  Disk is
 * touched once at startup (load) and once at exit (save, and only if
 * something changed).  That is the I/O budget for cheap SSDs.
 */


#define	OFFCACHE_PREFIX		"#offcache#"


int	 offcache_enabled(void);
int	 offcache_is_name(char *);
int	 offcache_is_stream(MAILSTREAM *);
int	 offcache_has_folder(char *);
int	 offcache_network_ok(void);
void	 offcache_note_live(void);
char	*offcache_name_for(char *);
char	*offcache_real_name(char *);
int	 offcache_skip_remote_probe(char *);

void	 offcache_link_driver(void);
void	 offcache_init(void);
void	 offcache_load_all(void);
void	 offcache_save_all(void);
void	 offcache_learn_stream(MAILSTREAM *);
void	 offcache_apply_dirty_flags(MAILSTREAM *);


#endif /* PITH_OFFCACHE_INCLUDED */
