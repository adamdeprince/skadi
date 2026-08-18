/*
 * ========================================================================
 * Copyright 2026 Eduardo Chappa
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
 * Kitty (and compatible) inline image support for the message viewer.
 *
 * Images are decoded to a temporary PNG, transmitted once with the
 * kitty graphics protocol, and then re-placed on each screen paint
 * at the reserved blank lines in the formatted text.
 */


/* exported prototypes */
int	 kitty_img_enabled(void);
int	 kitty_img_count(void);
void	 kitty_img_begin_message(void);
void	 kitty_img_end_message(void);
int	 kitty_img_from_part(ATTACH_S *, long, int *, int *);
int	 kitty_img_from_cid(char *, int *, int *);
int	 kitty_img_from_data_uri(char *, int *, int *);
int	 kitty_img_emit_block(gf_o_t, HANDLE_S **, int, int, int, char *);
void	 kitty_img_paint_lines(char **, short *, long, int, int, HANDLE_S *);
void	 kitty_img_paint_text(char *, int, int, HANDLE_S *);


#endif /* PITH_KITTYIMG_INCLUDED */
