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

#ifndef PITH_KITTYIMG_INCLUDED
#define PITH_KITTYIMG_INCLUDED


#include "../pith/handle.h"
#include "../pith/atttype.h"
#include "../pith/filttype.h"


/*
 * Inline image support for the message viewer.
 *
 * Images are decoded in-process (stb_image), resampled to a thumbnail
 * that fits a cell box (inline-image-max-size, default 32x8 cells) and
 * cached across repaints, re-views and resizes.  The formatted text
 * carries a selectable "[Image: ...]" handle line followed by the
 * reserved blank rows; each screen paint places the thumbnails on
 * those rows using the kitty graphics protocol, iTerm2's inline image
 * sequence, or sixels, whichever the terminal supports.
 */


/* exported prototypes */
int	  kitty_img_enabled(void);
int	  kitty_img_count(void);
void	  kitty_img_begin_message(void);
void	  kitty_img_end_message(void);
void	  kitty_img_shutdown(void);
void	  kitty_img_note_resize(void);
int	  kitty_img_from_part(ATTACH_S *, long, int *, int *);
int	  kitty_img_from_cid(char *, int *, int *);
int	  kitty_img_from_data_uri(char *, int *, int *);
int	  kitty_img_from_url(char *, int, int *, int *, const char **);
ATTACH_S *kitty_img_attach_for_cid(char *);
char	 *kitty_img_alt(char *, char *, size_t);
int	  kitty_img_emit_block(gf_o_t, HANDLE_S **, int, int, int, ATTACH_S *, char *);
void	  kitty_img_paint_lines(char **, short *, long, int, int, long, HANDLE_S *);


#endif /* PITH_KITTYIMG_INCLUDED */
