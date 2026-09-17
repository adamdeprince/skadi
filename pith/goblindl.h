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

#ifndef PITH_GOBLINDL_INCLUDED
#define PITH_GOBLINDL_INCLUDED


#include "../pith/atttype.h"


/*
 * Move attachments through the terminal with kitty's file transfer
 * protocol (OSC 5113).  Download is a kitty send (Alpine to the
 * machine running kitty).  Upload is a kitty receive (local file
 * into Alpine).  If kitty is not available, download falls back to
 * OSC 777 goblin-download, then Save.
 */

#define	GOBLIN_DL_OK		0
#define	GOBLIN_DL_UNSUPPORTED	1
#define	GOBLIN_DL_ERROR		2
#define	GOBLIN_DL_CANCEL	3


/* exported prototypes */
int	 goblin_dl_attachment(long, ATTACH_S *);
int	 kitty_xfer_available(void);
int	 kitty_xfer_send_attachment(long, ATTACH_S *);
int	 kitty_xfer_send_path(char *, char *);
int	 kitty_xfer_receive(char *, char **, long *);
void	 goblin_write_osc(const char *);
void	 kitty_notify(char *, char *);


#endif /* PITH_GOBLINDL_INCLUDED */
