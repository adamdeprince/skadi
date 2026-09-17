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
 * Move files through the terminal with kitty's file transfer protocol
 * (OSC 5113).  "Send" moves data from the machine running Alpine to
 * the machine running kitty; "receive" pulls a client file into Alpine.
 * Availability is probed once per session with XTVERSION.
 */

#define	GOBLIN_DL_OK		0
#define	GOBLIN_DL_UNSUPPORTED	1
#define	GOBLIN_DL_ERROR		2
#define	GOBLIN_DL_CANCEL	3


/* a file received from the client: local temp copy plus its client name */
typedef struct kitty_file {
    char	     *path;		/* local temporary file, caller unlinks */
    char	     *name;		/* base name on the client */
    long	      size;
    struct kitty_file *next;
} KITTY_FILE_S;


/* exported prototypes */
int	 kitty_xfer_available(void);
char	*kitty_xfer_terminal(void);
int	 kitty_xfer_send_attachment(long, ATTACH_S *);
int	 kitty_xfer_send_attachments(long, ATTACH_S **, int, int *);
int	 kitty_xfer_send_path(char *, char *);
int	 kitty_xfer_receive(char *, KITTY_FILE_S **);
void	 kitty_free_files(KITTY_FILE_S **);
void	 kitty_notify(char *, char *);


#endif /* PITH_GOBLINDL_INCLUDED */
