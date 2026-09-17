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

int	 outbox_count(void);
int	 outbox_queue_file(char *, char *);
int	 outbox_queue_store(STORE_S *, char *);
void	 outbox_flush(void);
char	*outbox_dir(void);
char	*local_postponed_path(void);

#endif
