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


/*
 * Composer markup <-> text/html.
 *
 * The composer holds ordinary text plus a small set of HTML tags
 * (b, i, u, s, code, em, strong, a, img, ul, ol, li, blockquote,
 * h1-h3, pre, p, br, hr).  On send that text becomes a text/html
 * part inside multipart/alternative with a text/plain rendering.
 * Everything that is not one of those tags is escaped, and the
 * tags themselves are re-emitted without attributes (except a safe
 * href on links and a cid: src on inline images).
 */

/*
 * Set by redraft() and by reply quoting when the text handed to the
 * composer already carries markup; pine_send() reads and clears it
 * to start the composer in HTML mode.
 */
extern int	 html_compose_start_html;

int	 compose_text_is_html(struct mail_bodystruct *);
void	 htmlify_composed_text(struct mail_bodystruct *, char *);
void	 html_wrap_alternative(struct mail_bodystruct *);
char	*html_to_plain(char *);
char	*html_to_composer(char *);
char	*html_simple_to_markup(char *, int);
int	 html_is_simple(char *);
int	 html_text_has_markup(char *);
int	 html_find_cid_refs(char *, char ***);
int	 html_new_cid(char *, size_t);


int	 html_restore_composed_text(struct mail_bodystruct **);
void	 html_forget_composed_text(void);

#endif /* PITH_HTMLBODY_INCLUDED */
