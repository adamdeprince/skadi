/*
 * Copyright 2026 Adam DePrince. Licensed under the Apache License, Version 2.0.
 * Download images only on explicit activation. Retain bounded original image
 * data so repaints, revisits and thumbnail resizing do not contact the server.
 */
#include "../pith/headers.h"
#include "../pith/remoteimg.h"
#include "../pith/state.h"
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>

#ifndef _WINDOWS
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#endif


typedef struct remote_image {
    char *url;
    unsigned char *data;
    size_t len;
    int original;
    struct remote_image *next;
} REMOTE_IMAGE;

static REMOTE_IMAGE *remote_images;

int
remote_image_url(const char *url)
{
    const char *host, *p;
    size_t n;

    if(!url)
      return(0);
    if(!struncmp((char *) url, "https://", 8))
      host = url + 8;
    else if(!struncmp((char *) url, "http://", 7))
      host = url + 7;
    else
      return(0);

    n = strcspn(host, "/?#");
    if(!n || memchr(host, '@', n))
      return(0);
    for(p = url; *p; p++)
      if((unsigned char) *p <= 0x20 || (unsigned char) *p == 0x7f)
        return(0);
    return(1);
}

int
remote_image_link(const char *url)
{
    static const char *exts[] = {
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".pnm", ".ppm", ".pgm",
        ".webp", ".avif", ".svg", ".tif", ".tiff", NULL
    };
    const char *path;
    size_t n, len;
    int i;

    if(!remote_image_url(url))
      return(0);
    path = strstr(url, "://") + 3;
    path += strcspn(path, "/?#");
    if(*path != '/')
      return(0);
    n = strcspn(path, "?#");
    for(i = 0; exts[i]; i++){
        len = strlen(exts[i]);
        if(n >= len && !struncmp((char *) path + n - len, (char *) exts[i], len))
          return(1);
    }
    return(0);
}

static void
remote_image_free(REMOTE_IMAGE *image)
{
    fs_give((void **) &image->url);
    fs_give((void **) &image->data);
    fs_give((void **) &image);
}

void
remote_image_clear(void)
{
    REMOTE_IMAGE *image;

    while((image = remote_images) != NULL){
        remote_images = image->next;
        remote_image_free(image);
    }
}

static unsigned char *
remote_image_download(const char *url, size_t *len, const char **error)
{
#ifndef _WINDOWS
    int fd[2], status = 0, failed = 0;
    pid_t pid, waited;
    unsigned char *data;
    ssize_t n;
    size_t capacity = 0;
    unsigned char *grown;

    *len = 0;
    *error = _("Could not download image (HTTP error or allocation failure)");
    if(pipe(fd) < 0)
      return(NULL);
    fcntl(fd[0], F_SETFD, FD_CLOEXEC);
    fcntl(fd[1], F_SETFD, FD_CLOEXEC);
    pid = fork();
    if(pid == 0){
        int nullfd = open("/dev/null", O_RDWR);

        close(fd[0]);
        if(nullfd < 0 || dup2(nullfd, STDIN_FILENO) < 0
           || dup2(nullfd, STDERR_FILENO) < 0
           || dup2(fd[1], STDOUT_FILENO) < 0)
          _exit(126);
        if(nullfd > STDERR_FILENO)
          close(nullfd);
        if(fd[1] > STDERR_FILENO)
          close(fd[1]);
        /* No shell, curlrc, cookies, or inherited mail credentials. */
        execlp("curl", "curl", "-q", "--silent", "--fail", "--location",
               "--proto", "=http,https", "--proto-redir", "=http,https",
               "--max-redirs", "-1",
               "--output", "-", "--", url, (char *) NULL);
        _exit(127);
    }

    close(fd[1]);
    if(pid < 0){
        close(fd[0]);
        return(NULL);
    }

    data = NULL;
    for(;;){
        if(*len == capacity){
            size_t next = capacity ? (capacity <= SIZE_MAX / 2 ? capacity * 2 : SIZE_MAX) : (size_t)getpagesize();
            if(next <= capacity || !(grown = realloc(data,next))){ failed = 1; break; }
            data = grown; capacity = next;
        }
        n = read(fd[0], data + *len, capacity - *len > SSIZE_MAX ? SSIZE_MAX : capacity - *len);
        if(n > 0)
          *len += (size_t) n;
        else if(n < 0 && errno == EINTR)
          continue;
        else{
            failed = n < 0;
            break;
        }
    }
    close(fd[0]);
    if(failed)
      kill(pid, SIGKILL);
    do {
        waited = waitpid(pid, &status, 0);
    } while(waited < 0 && errno == EINTR);

    if(waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status)
       || failed || !*len){
        if(waited > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 127)
          *error = _("Install curl to download linked images");
        fs_give((void **) &data);
        *len = 0;
        return(NULL);
    }

    fs_resize((void **) &data, *len);
    *error = NULL;
    return(data);
#else
    *error = _("Linked image downloads are not supported on this platform");
    return(NULL);
#endif
}

static unsigned char *
remote_image_fetch(const char *url, size_t *len, int original, const char **error)
{
#ifndef _WINDOWS
    NETMBX mb;
    MAILSTREAM *mail = ps_global ? ps_global->mail_stream : NULL;
    const char *mailbox = mail ? (mail->original_mailbox ? mail->original_mailbox : mail->mailbox) : NULL;
    if(mailbox && mail_valid_net_parse((char *)mailbox,&mb) && mb.njordflag){
        NETSTREAM *connection = NULL;
        char *line = NULL, *end;
        unsigned char *data = NULL;
        uintmax_t source_size, size;
        *len = 0;
        *error = _("Njord image download failed");
        if(strncmp(url,"https://",8)){
            *error = _("Njord fetches HTTPS images only"); return NULL;
        }
        connection = net_njord_image(&mb);
        if(!connection) return NULL;
        if(!net_soutr(connection,original ? "ORIGINAL " : "PREVIEW ") ||
           !net_soutr(connection,(char *)url) || !net_soutr(connection,"\r\n")) goto done;
        line = net_getline(connection);
        if(!line || strncmp(line,"OK ",3)) goto done;
        errno = 0;
        source_size = strtoumax(line+3,&end,10);
        if(errno || end == line+3 || *end != ' ') goto done;
        size = strtoumax(end+1,&end,10);
        if(errno || !size || size >= SIZE_MAX || size > ULONG_MAX || strncmp(end," image/",7)) goto done;
        data = (unsigned char *)malloc((size_t)size+1);
        if(!data){ *error = _("Could not allocate image download"); goto done; }
        if(!net_getbuffer(connection,(unsigned long)size,(char *)data)){
            free(data); data = NULL; goto done;
        }
        *len = (size_t)size;
        *error = NULL;
        (void)source_size;
done:
        if(line) fs_give((void **)&line);
        net_close(connection);
        return data;
    }
#endif
    return remote_image_download(url,len,error);
}

const unsigned char *
remote_image_data(const char *url, size_t *len, int fetch, const char **error)
{
    REMOTE_IMAGE *image, **pp;
    unsigned char *data;
    const char *err = NULL;

    *len = 0;
    if(error)
      *error = NULL;
    if(!remote_image_url(url)){
        if(error)
          *error = _("Image address must be an HTTP or HTTPS URL");
        return(NULL);
    }

    for(pp = &remote_images; (image = *pp) != NULL; pp = &image->next)
      if(!strcmp(image->url, url) && (fetch != 2 || image->original)){
          *pp = image->next;
          image->next = remote_images;
          remote_images = image;
          *len = image->len;
          return(image->data);
      }

    if(!fetch)
      return(NULL);
    data = remote_image_fetch(url, len, fetch == 2, &err);
    if(!data){
        if(error)
          *error = err;
        return(NULL);
    }

    image = (REMOTE_IMAGE *) fs_get(sizeof(*image));
    image->url = cpystr(url);
    image->data = data;
    image->len = *len;
    image->original = fetch == 2;
    image->next = remote_images;
    remote_images = image;
    return(data);
}
