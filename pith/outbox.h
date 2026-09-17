/*
 * ========================================================================
 * Copyright 2026 Adam DePrince
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * ========================================================================
 */

#ifndef PITH_OUTBOX_INCLUDED
#define PITH_OUTBOX_INCLUDED

#include "../pith/store.h"
#include "../pith/send.h"


/*
 * Local outbox: a mail folder (~/.alpine-outbox) holding messages that
 * could not be sent.  They are sent through the normal sending
 * pipeline (roles, SMTP or sendmail, Fcc) when a live connection is
 * next made, and again on every Sync until they go out or reach the
 * retry limit.
 */

#define	OUTBOX_MAX_TRIES	5

char	*outbox_path(void);
int	 outbox_count(void);
int	 outbox_queue(METAENV *, struct mail_bodystruct *);
int	 outbox_flush(void);
int	 outbox_flush_pending(void);

#endif
