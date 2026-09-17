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

#ifndef PITH_HTMLBODY_INCLUDED
#define PITH_HTMLBODY_INCLUDED

int	 compose_text_is_html(struct mail_bodystruct *);
void	 htmlify_composed_text(struct mail_bodystruct *);
char	*html_to_plain(char *);
void	 html_wrap_alternative(struct mail_bodystruct *);
int	 html_is_simple(char *);
char	*html_simple_to_display(char *);

#endif
