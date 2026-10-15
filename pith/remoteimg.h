/* Copyright 2026 Adam DePrince. Licensed under the Apache License, Version 2.0. */
#ifndef PITH_REMOTEIMG_INCLUDED
#define PITH_REMOTEIMG_INCLUDED

#include <stddef.h>

#define REMOTE_IMAGE_PREVIEW 1
#define REMOTE_IMAGE_ORIGINAL 2

int remote_image_url(const char *);
int remote_image_link(const char *);
const unsigned char *remote_image_data(const char *, size_t *, int, const char **);
void remote_image_clear(void);

#endif
