/*
 * ========================================================================
 * Copyright 2013-2026 Eduardo Chappa
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *      
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * ========================================================================
 */
  

/* OAUTH2 support code goes here. This is necessary because
 * 1. it helps to coordinate two different methods, such as XOAUTH2 and
 *    OAUTHBEARER, which use the same code, so it can all go in one place
 *
 * 2. It helps with coordinating with the client when the server requires
 *    the deviceinfo method.
 */

/* http.h is supposed to be included, typically by including c-client.h */
#include "json.h"
#include "oauth2_aux.h"

#if !defined(_MSC_VER) && !defined(_WINDOWS)
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#endif

/*
 * Loopback listener for the OAuth2 authorization redirect.
 *
 * Providers that register "http://localhost" or "http://127.0.0.1" as
 * the redirect URI accept any port on the loopback interface.  We
 * listen on an ephemeral port there (IPv4, and IPv6 too when the same
 * port is free), hand that address to the provider as redirect_uri,
 * and pick the authorization code out of the browser's redirect.  The
 * state parameter in the redirect has to match the one we sent.
 *
 * Nothing here blocks for longer than the caller asks: Alpine polls
 * with oauth2_loopback_poll() from its keyboard wait so the screen
 * stays responsive and the user can cancel at any time.
 */

#define OA2_LB_MAXFD	2
#define OA2_LB_REQMAX	16384
#define OA2_LB_LINEMAX	4096

static int	oa2_lb_fd[OA2_LB_MAXFD] = { -1, -1 };
static int	oa2_lb_nfd = 0;
static unsigned	oa2_lb_port = 0;
static char    *oa2_lb_state = NULL;	/* state we expect back */
static char    *oa2_lb_redirect = NULL;	/* redirect_uri we installed */
static char	oa2_lb_error[256];
static int	oa2_lb_done = 0;

/* LOOPBACK-PARSE-BEGIN: libc only, so a harness can compile this region */

static int
oa2_lb_hexval(int c)
{
    if(c >= '0' && c <= '9') return(c - '0');
    if(c >= 'a' && c <= 'f') return(c - 'a' + 10);
    if(c >= 'A' && c <= 'F') return(c - 'A' + 10);
    return(-1);
}


/*
 * Decode %XX and '+' in place.
 */
static void
oa2_lb_url_decode(char *s)
{
    char *d, *p;
    int hi, lo;

    if(!s)
      return;

    for(d = p = s; *p; ){
	if(*p == '%' && (hi = oa2_lb_hexval((unsigned char) p[1])) >= 0
	   && (lo = oa2_lb_hexval((unsigned char) p[2])) >= 0){
	    *d++ = (char) ((hi << 4) | lo);
	    p += 3;
	}
	else if(*p == '+'){
	    *d++ = ' ';
	    p++;
	}
	else
	  *d++ = *p++;
    }

    *d = '\0';
}


/*
 * Split the request line "METHOD SP TARGET SP HTTP/x.y" of req (which
 * is modified in place).  TARGET is split at '?' into path and query.
 * Returns 1 when the line is well formed, 0 otherwise.
 */
static int
oa2_lb_parse_request(char *req, char **method, char **path, char **query)
{
    char *eol, *sp1, *sp2, *q;

    *method = *path = *query = NULL;
    if(!req)
      return(0);

    eol = strpbrk(req, "\r\n");
    if(!eol || (eol - req) > OA2_LB_LINEMAX)
      return(0);

    *eol = '\0';

    sp1 = strchr(req, ' ');
    if(!sp1 || sp1 == req)
      return(0);

    *sp1 = '\0';
    sp2 = strchr(sp1 + 1, ' ');
    if(!sp2 || sp2 == sp1 + 1)
      return(0);

    *sp2 = '\0';
    if(strncmp(sp2 + 1, "HTTP/", 5) != 0)
      return(0);

    *method = req;
    *path = sp1 + 1;
    q = strchr(*path, '?');
    if(q){
	*q = '\0';
	*query = q + 1;
    }
    else
      *query = "";

    if(**path != '/')
      return(0);

    return(1);
}


/*
 * Copy the decoded value of the query parameter called name into dst.
 * Returns dst, or NULL if the parameter is absent.
 */
static char *
oa2_lb_query_value(const char *query, const char *name, char *dst, size_t dstlen)
{
    const char *p, *amp, *eq;
    char key[128];
    size_t klen, vlen;

    if(!query || !name || !dst || dstlen < 1)
      return(NULL);

    for(p = query; *p; p = amp && *amp ? amp + 1 : p + strlen(p)){
	amp = strchr(p, '&');
	if(!amp)
	  amp = p + strlen(p);

	eq = memchr(p, '=', (size_t) (amp - p));
	klen = (size_t) ((eq ? eq : amp) - p);
	if(klen >= sizeof(key))
	  continue;

	memcpy(key, p, klen);
	key[klen] = '\0';
	oa2_lb_url_decode(key);
	if(strcmp(key, name) != 0)
	  continue;

	vlen = eq ? (size_t) (amp - (eq + 1)) : 0;
	if(vlen >= dstlen)
	  vlen = dstlen - 1;

	memcpy(dst, eq ? eq + 1 : "", vlen);
	dst[vlen] = '\0';
	oa2_lb_url_decode(dst);
	return(dst);
    }

    return(NULL);
}


/*
 * Is uri "http://localhost" or "http://127.0.0.1", optionally followed
 * by a port, a path or a query?  Those are the redirect URIs that let
 * a native app pick any loopback port.
 */
static int
oa2_lb_is_loopback_uri(const char *uri)
{
    static const char *hosts[] = { "http://localhost", "http://127.0.0.1", NULL };
    int i;
    size_t n;

    if(!uri)
      return(0);

    for(i = 0; hosts[i]; i++){
	n = strlen(hosts[i]);
	if(strncasecmp(uri, hosts[i], n) == 0
	   && (uri[n] == '\0' || uri[n] == '/' || uri[n] == ':' || uri[n] == '?'))
	  return(1);
    }

    return(0);
}

/* LOOPBACK-PARSE-END */


#if !defined(_MSC_VER) && !defined(_WINDOWS)

static void
oa2_lb_set_error(const char *msg)
{
    if(!msg)
      oa2_lb_error[0] = '\0';
    else{
	strncpy(oa2_lb_error, msg, sizeof(oa2_lb_error) - 1);
	oa2_lb_error[sizeof(oa2_lb_error) - 1] = '\0';
    }
}


static int
oa2_lb_listen(int family, unsigned port)
{
    int fd, on = 1;
    struct sockaddr_storage ss;
    socklen_t alen;

    memset(&ss, 0, sizeof(ss));
    if(family == AF_INET){
	struct sockaddr_in *a = (struct sockaddr_in *) &ss;

	a->sin_family = AF_INET;
	a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a->sin_port = htons((unsigned short) port);
	alen = sizeof(*a);
    }
    else{
	struct sockaddr_in6 *a = (struct sockaddr_in6 *) &ss;

	a->sin6_family = AF_INET6;
	a->sin6_addr = in6addr_loopback;
	a->sin6_port = htons((unsigned short) port);
	alen = sizeof(*a);
    }

    fd = socket(family, SOCK_STREAM, 0);
    if(fd < 0)
      return(-1);

    (void) fcntl(fd, F_SETFD, FD_CLOEXEC);
    if(family == AF_INET6)
      (void) setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));

    if(bind(fd, (struct sockaddr *) &ss, alen) < 0 || listen(fd, 4) < 0){
	close(fd);
	return(-1);
    }

    return(fd);
}


static unsigned
oa2_lb_port_of(int fd)
{
    struct sockaddr_storage ss;
    socklen_t alen = sizeof(ss);

    if(getsockname(fd, (struct sockaddr *) &ss, &alen) < 0)
      return(0);

    if(ss.ss_family == AF_INET)
      return((unsigned) ntohs(((struct sockaddr_in *) &ss)->sin_port));

    return((unsigned) ntohs(((struct sockaddr_in6 *) &ss)->sin6_port));
}


/*
 * Start listening.  Returns the port, or 0 when the provider's
 * redirect_uri is not loopback style or no socket could be opened.
 */
int
oauth2_loopback_start(OAUTH2_S *oauth2)
{
    char redir[128], *old, *host;
    int fd4, fd6;

    oauth2_loopback_stop();
    oa2_lb_done = 0;
    oa2_lb_set_error(NULL);

    old = oauth2->param[OA2_Redirect].value;
    if(old == oa2_lb_redirect && oa2_lb_redirect)	/* ours from a previous try */
      old = NULL;

    if(!oa2_lb_is_loopback_uri(old))
      return(0);

    host = (strncasecmp(old, "http://127.0.0.1", 16) == 0) ? "127.0.0.1" : "localhost";

    fd4 = oa2_lb_listen(AF_INET, 0);
    if(fd4 < 0)
      return(0);

    oa2_lb_port = oa2_lb_port_of(fd4);
    if(oa2_lb_port == 0){
	close(fd4);
	return(0);
    }

    oa2_lb_fd[oa2_lb_nfd++] = fd4;

    /* localhost may resolve to ::1 first; serve that too when we can */
    fd6 = oa2_lb_listen(AF_INET6, oa2_lb_port);
    if(fd6 >= 0)
      oa2_lb_fd[oa2_lb_nfd++] = fd6;

    snprintf(redir, sizeof(redir), "http://%s:%u/", host, oa2_lb_port);

    if(oa2_lb_redirect)
      fs_give((void **) &oa2_lb_redirect);

    oa2_lb_redirect = cpystr(redir);
    /* the provider's default is a static string; never free that */
    oauth2->param[OA2_Redirect].value = oa2_lb_redirect;

    if(!oauth2->param[OA2_State].value)
      oauth2->param[OA2_State].value = oauth2_generate_state();

    if(oa2_lb_state)
      fs_give((void **) &oa2_lb_state);

    oa2_lb_state = cpystr(oauth2->param[OA2_State].value);

    return((int) oa2_lb_port);
}


void
oauth2_loopback_stop(void)
{
    int i;

    for(i = 0; i < oa2_lb_nfd; i++)
      if(oa2_lb_fd[i] >= 0){
	  close(oa2_lb_fd[i]);
	  oa2_lb_fd[i] = -1;
      }

    oa2_lb_nfd = 0;
    oa2_lb_port = 0;
    if(oa2_lb_state)
      fs_give((void **) &oa2_lb_state);
    /* oa2_lb_redirect stays: the token request still needs it */
}


int
oauth2_loopback_active(void)
{
    return(oa2_lb_nfd > 0 && !oa2_lb_done);
}


unsigned
oauth2_loopback_port(void)
{
    return(oa2_lb_nfd > 0 ? oa2_lb_port : 0);
}


char *
oauth2_loopback_redirect(void)
{
    return(oa2_lb_nfd > 0 ? oa2_lb_redirect : NULL);
}


/*
 * The last problem seen on the listener ("state mismatch", "access
 * denied"), or NULL.  Cleared by oauth2_loopback_start.
 */
const char *
oauth2_loopback_error(void)
{
    return(oa2_lb_error[0] ? oa2_lb_error : NULL);
}


static void
oa2_lb_send(int cfd, const char *status, const char *body)
{
    char hdr[256];
    int  flags = 0;

#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif

    snprintf(hdr, sizeof(hdr),
	     "HTTP/1.1 %s\r\nContent-Type: text/html; charset=utf-8\r\n"
	     "Content-Length: %lu\r\nConnection: close\r\n\r\n",
	     status, (unsigned long) strlen(body));
    (void) send(cfd, hdr, strlen(hdr), flags);
    (void) send(cfd, body, strlen(body), flags);
}


static const char *oa2_lb_page_ok =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Alpine</title></head>"
    "<body style=\"font-family:sans-serif;padding:2em\">"
    "<h2>Alpine is authorized</h2>"
    "<p>You can close this tab and return to Alpine.</p></body></html>";

static const char *oa2_lb_page_denied =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Alpine</title></head>"
    "<body style=\"font-family:sans-serif;padding:2em\">"
    "<h2>Authorization was not granted</h2>"
    "<p>Return to Alpine to try again or cancel.</p></body></html>";

static const char *oa2_lb_page_bad =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Alpine</title></head>"
    "<body style=\"font-family:sans-serif;padding:2em\">"
    "<h2>Request not accepted</h2>"
    "<p>This redirect did not match what Alpine is waiting for.</p></body></html>";

static const char *oa2_lb_page_404 =
    "<!DOCTYPE html><html><body>Not found</body></html>";


/*
 * Read one HTTP request from cfd (headers only, at most two seconds)
 * and answer it.  Returns the authorization code when the request is
 * the redirect we are waiting for, NULL otherwise.
 */
static char *
oa2_lb_handle(int cfd)
{
    char  *req, *method, *path, *query, *code = NULL;
    char   val[OA2_LB_LINEMAX];
    int    got = 0, n;
    time_t deadline = time(0) + 2;

#ifdef SO_NOSIGPIPE
    { int on = 1; (void) setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)); }
#endif
    (void) fcntl(cfd, F_SETFD, FD_CLOEXEC);

    req = (char *) fs_get(OA2_LB_REQMAX + 1);
    req[0] = '\0';

    while(got < OA2_LB_REQMAX){
	fd_set rfds;
	struct timeval tv;
	time_t now = time(0);

	if(now >= deadline)
	  break;

	FD_ZERO(&rfds);
	FD_SET(cfd, &rfds);
	tv.tv_sec = deadline - now;
	tv.tv_usec = 0;
	if(select(cfd + 1, &rfds, NULL, NULL, &tv) <= 0){
	    if(errno == EINTR)
	      continue;

	    break;
	}

	n = (int) recv(cfd, req + got, (size_t) (OA2_LB_REQMAX - got), 0);
	if(n <= 0)
	  break;

	got += n;
	req[got] = '\0';
	if(strstr(req, "\r\n\r\n") || strstr(req, "\n\n"))
	  break;
    }

    if(got == 0 || !oa2_lb_parse_request(req, &method, &path, &query)){
	oa2_lb_send(cfd, "400 Bad Request", oa2_lb_page_bad);
	goto out;
    }

    if(strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0){
	oa2_lb_send(cfd, "405 Method Not Allowed", oa2_lb_page_404);
	goto out;
    }

    if(strcmp(path, "/") != 0){
	oa2_lb_send(cfd, "404 Not Found", oa2_lb_page_404);
	goto out;
    }

    if(oa2_lb_query_value(query, "error", val, sizeof(val))){
	char msg[256];

	snprintf(msg, sizeof(msg), "Authorization was refused: %.120s", val);
	oa2_lb_set_error(msg);
	oa2_lb_send(cfd, "200 OK", oa2_lb_page_denied);
	goto out;
    }

    if(!oa2_lb_query_value(query, "code", val, sizeof(val)) || !val[0]){
	oa2_lb_send(cfd, "404 Not Found", oa2_lb_page_404);
	goto out;
    }

    if(oa2_lb_state){
	char st[OA2_LB_LINEMAX];

	if(!oa2_lb_query_value(query, "state", st, sizeof(st))
	   || strcmp(st, oa2_lb_state) != 0){
	    oa2_lb_set_error("Redirect ignored: state parameter did not match");
	    oa2_lb_send(cfd, "400 Bad Request", oa2_lb_page_bad);
	    goto out;
	}
    }

    code = cpystr(val);
    oa2_lb_done = 1;
    oa2_lb_set_error(NULL);
    oa2_lb_send(cfd, "200 OK", oa2_lb_page_ok);

out:
    fs_give((void **) &req);
    close(cfd);
    return(code);
}


/*
 * Wait up to wait_ms milliseconds (0 = just look) for the browser to
 * come back.  Returns the authorization code (caller frees) or NULL.
 */
char *
oauth2_loopback_poll(int wait_ms)
{
    fd_set rfds;
    struct timeval tv;
    int i, maxfd = -1, res, cfd;

    if(!oauth2_loopback_active())
      return(NULL);

    FD_ZERO(&rfds);
    for(i = 0; i < oa2_lb_nfd; i++){
	FD_SET(oa2_lb_fd[i], &rfds);
	if(oa2_lb_fd[i] > maxfd)
	  maxfd = oa2_lb_fd[i];
    }

    if(wait_ms < 0)
      wait_ms = 0;

    tv.tv_sec = wait_ms / 1000;
    tv.tv_usec = (wait_ms % 1000) * 1000;
    res = select(maxfd + 1, &rfds, NULL, NULL, &tv);
    if(res <= 0)
      return(NULL);

    for(i = 0; i < oa2_lb_nfd; i++){
	if(!FD_ISSET(oa2_lb_fd[i], &rfds))
	  continue;

	cfd = accept(oa2_lb_fd[i], NULL, NULL);
	if(cfd < 0)
	  continue;

	{ char *code = oa2_lb_handle(cfd);

	  if(code)
	    return(code);
	}
    }

    return(NULL);
}


/*
 * Blocking form for callers without a user interface.
 */
char *
oauth2_loopback_wait(int timeout_sec)
{
    time_t deadline;
    char *code = NULL;

    if(!oauth2_loopback_active())
      return(NULL);

    if(timeout_sec <= 0)
      timeout_sec = 180;

    deadline = time(0) + timeout_sec;
    while(!code && time(0) < deadline)
      code = oauth2_loopback_poll(1000);

    return(code);
}

#else /* _WINDOWS */

int oauth2_loopback_start(OAUTH2_S *oauth2) { (void) oauth2; return(0); }
void oauth2_loopback_stop(void) { }
int oauth2_loopback_active(void) { return(0); }
unsigned oauth2_loopback_port(void) { return(0); }
char *oauth2_loopback_redirect(void) { return(NULL); }
const char *oauth2_loopback_error(void) { return(NULL); }
char *oauth2_loopback_poll(int wait_ms) { (void) wait_ms; return(NULL); }
char *oauth2_loopback_wait(int timeout_sec) { (void) timeout_sec; return(NULL); }

#endif /* _WINDOWS */

OA2_type oauth2_find_extra_parameter(OAUTH2_S *, char *);
JSON_S *oauth2_json_reply(OAUTH2_SERVER_METHOD_S, OAUTH2_S *, int *);
char *xoauth2_server(char *, char *);

#define LOAD_HTTP_PARAMS(X, Y)	{				\
     int i;							\
     for(i = 0; (X).params[i] != OA2_End; i++){			\
	OA2_type j = (X).params[i];				\
	(Y)[i].name  = oauth2->param[j].name;			\
	(Y)[i].value = oauth2->param[j].value;			\
     }								\
     (Y)[i].name = (Y)[i].value = NULL;				\
}

#define OAUTH2_CLEAR_EXTRA(X, Y) do {				\
     OA2_type i = oauth2_find_extra_parameter((X), (Y));	\
     if(i < OA2_End && (X)->param[i].value)			\
	fs_give((void **) &(X)->param[i].value);		\
} while(0)

void oauth2_free_extra_values(OAUTH2_S *oauth2)
{
  if(oauth2->param[OA2_Id].value) fs_give((void **) &oauth2->param[OA2_Id].value);
  if(oauth2->param[OA2_Secret].value) fs_give((void **) &oauth2->param[OA2_Secret].value);
  if(oauth2->param[OA2_Tenant].value) fs_give((void **) &oauth2->param[OA2_Tenant].value);
  if(oauth2->param[OA2_State].value) fs_give((void **) &oauth2->param[OA2_State].value);
  if(oauth2->param[OA2_RefreshToken].value) fs_give((void **) &oauth2->param[OA2_RefreshToken].value);
  if(oauth2->param[OA2_DeviceCode].value) fs_give((void **) &oauth2->param[OA2_DeviceCode].value);
  if(oauth2->access_token) fs_give((void **) &oauth2->access_token);

  if(oauth2->devicecode.device_code) fs_give((void **) &oauth2->devicecode.device_code);
  if(oauth2->devicecode.user_code) fs_give((void **) &oauth2->devicecode.user_code);
  if(oauth2->devicecode.verification_uri) fs_give((void **) &oauth2->devicecode.verification_uri);
  if(oauth2->devicecode.message) fs_give((void **) &oauth2->devicecode.message);

  /* free extra parameters generated by us */
  OAUTH2_CLEAR_EXTRA(oauth2, "code_verifier");
  OAUTH2_CLEAR_EXTRA(oauth2, "code_challenge");
  OAUTH2_CLEAR_EXTRA(oauth2, "login_hint");
}

OA2_type oauth2_find_extra_parameter(OAUTH2_S *oauth2, char *name)
{
   OA2_type i;

   if(!name) return OA2_End;

   for(i = OA2_Extra1; i < OA2_End; i++){
      if(oauth2->param[i].name
	 && !compare_cstring(oauth2->param[i].name, name))
	 break;
   }
   return i;
}

/* code_challenge generator */
void oauth2_code_challenge(OAUTH2_S *oauth2)
{
    OA2_type i, j, k;
    char *cv, *cv1, *cv2;

    i = oauth2_find_extra_parameter(oauth2, "code_verifier");

    if(i == OA2_End) return;

    j = oauth2_find_extra_parameter(oauth2, "code_challenge");

    if(j == OA2_End) return;

    k = oauth2_find_extra_parameter(oauth2, "code_challenge_method");

    cv1 = oauth2_generate_state();
    cv2 = oauth2_generate_state();

    if(!cv1 || !cv2) return;

    if(oauth2->param[i].value) fs_give((void **) &oauth2->param[i].value);
    if(oauth2->param[j].value) fs_give((void **) &oauth2->param[j].value);

    cv = fs_get(strlen(cv1) + strlen(cv2) + 1 + 1);
    if(cv){
      sprintf(cv, "%s-%s", cv1, cv2);
      fs_give((void **) &cv1);
      fs_give((void **) &cv2);
      oauth2->param[i].value = cv;
      if(k == OA2_End
	|| !compare_cstring(oauth2->param[k].value, "plain"))
        oauth2->param[j].value = cpystr(cv);
      else if(!compare_cstring(oauth2->param[k].value, "S256")){
	unsigned char *t, *u, *v;
	char *s = hash_from_sizedtext("SHA256", cv, strlen(cv), &t);
	if(s){
	   u = v = t;
	   if(t){
	      for(; *v != '\0'; v++){
		  if(*v < 0x20) continue;
		  *u++ = *v;
	      }
	      *u = '\0';
	      oauth2->param[j].value = t;
	   }
	   fs_give((void **) &s);
	}
      }
    }
}

/* code_challenge generator */
void oauth2_login_hint(OAUTH2_S *oauth2, char *user)
{
    OA2_type i, j, k;
    char *cv, *cv1, *cv2;

    if(!user || !*user) return;

    i = oauth2_find_extra_parameter(oauth2, "login_hint");

    if(i == OA2_End) return;

    if(oauth2->param[i].value) fs_give((void **) &oauth2->param[i].value);
    oauth2->param[i].value = cpystr(user);
}

/* we generate something like a guid, but not care about
 * anything, but that it is really random.
 */
char *oauth2_generate_state(void)
{
  char rv[37];
  int i;

  rv[0] = '\0';
  for(i = 0; i < 4; i++)
     sprintf(rv + strlen(rv), "%02x", (unsigned int) (random() % 256));
  sprintf(rv + strlen(rv), "%c", '-');
  for(i = 0; i < 2; i++)
     sprintf(rv + strlen(rv), "%02x", (unsigned int) (random() % 256));
  sprintf(rv + strlen(rv), "%c", '-');
  for(i = 0; i < 2; i++)
     sprintf(rv + strlen(rv), "%02x", (unsigned int) (random() % 256));
  sprintf(rv + strlen(rv), "%c", '-');
  for(i = 0; i < 2; i++)
     sprintf(rv + strlen(rv), "%02x", (unsigned int) (random() % 256));
  sprintf(rv + strlen(rv), "%c", '-');
  for(i = 0; i < 6; i++)
     sprintf(rv + strlen(rv), "%02x", (unsigned int) (random() % 256));
  rv[36] = '\0';
  return cpystr(rv);
}

char *
xoauth2_server(char *server, char *tenant)
{
    char *rv = NULL;

    if (server == NULL) return NULL;

    if(tenant){
	char *s = cpystr(server);
	char *t = s, *u;
	int i;
	for(i = 0; t != NULL; i++){
	   t = strchr(t, '\001');
	   if(t != NULL) t++;
	}
	rv = fs_get((strlen(s) + i*(strlen(tenant)-1) + 1)*sizeof(char));
	*rv = '\0';
	for(u = t = s; t != NULL; i++){
	   t = strchr(t, '\001');
	   if (t != NULL) *t = '\0';
	   strcat(rv, u);
	   if(t != NULL){
	      strcat(rv, tenant);
	      *t++ = '\001';
	   }
	   u = t;
	}
	fs_give((void **) &s);
    }
    else
      rv = cpystr(server);

    return rv;
}

JSON_S *
oauth2_json_reply(OAUTH2_SERVER_METHOD_S RefreshMethod, OAUTH2_S *oauth2, int *status)
{
    JSON_S *json = NULL;
    HTTP_PARAM_S params[OAUTH2_PARAM_NUMBER];
    HTTPSTREAM *stream = NIL;
    unsigned char *s;
    char *server = NULL;

    LOAD_HTTP_PARAMS(RefreshMethod, params);
    *status = 0;
    server = xoauth2_server(RefreshMethod.urlserver, oauth2->param[OA2_Tenant].value);
    if(strcmp(RefreshMethod.name, "POST") == 0
	&& ((stream = http_open(server)) != NULL)
	&& ((s = http_post_param(stream, params)) != NULL)){
	json = json_parse(s);
	fs_give((void **) &s);
    }
    *status = stream && stream->status ? stream->status->code : -1;
    if(stream) http_close(stream);
    if(server)
	fs_give((void **) &server);

    return json;
}


void 
mm_login_oauth2_c_client_method (NETMBX *mb, char *user, char *method,
			OAUTH2_S *oauth2, unsigned long trial, int *tryanother)
{
   int status;
   char *s = NULL;
   JSON_S *json = NULL;

   mm_log("mm_login_oauth2_c_client_method()", (long) NIL);

   if(oauth2->param[OA2_Id].value == NULL
	|| (oauth2->require_secret && oauth2->param[OA2_Secret].value == NULL)){
     XOAUTH2_INFO_S *x;
     oauth2clientinfo_t ogci =
		(oauth2clientinfo_t) mail_parameters (NIL, GET_OA2CLIENTINFO, NIL);

     mm_log("Setting up for next call. Attempting to ask client for client-id and client-secret.", (long) NIL);

     if(ogci && (x = (*ogci)(oauth2->name, user)) != NULL){
	 if(oauth2->param[OA2_Id].value) fs_give((void **) &oauth2->param[OA2_Id].value);
	 oauth2->param[OA2_Id].value = cpystr(x->client_id);
	 if(oauth2->param[OA2_Secret].value) fs_give((void **) &oauth2->param[OA2_Secret].value);
	 oauth2->param[OA2_Secret].value = x->client_secret ? cpystr(x->client_secret) : NULL;
	 if(oauth2->param[OA2_Tenant].value) fs_give((void **) &oauth2->param[OA2_Tenant].value);
	 oauth2->param[OA2_Tenant].value = x->tenant ? cpystr(x->tenant) : NULL;
	 free_xoauth2_info(&x);
     }
   }

   if(oauth2->param[OA2_Id].value == NULL
	|| (oauth2->require_secret && oauth2->param[OA2_Secret].value == NULL)){
      *tryanother = 1;
      oauth2_free_extra_values(oauth2);
      mm_log("could not get client-id or client-secret required and empty.", (long) NIL);
      return;
   }

   mm_log("Got a client-id/client-secret to use.", (long) NIL);

   /* Do we have a method to execute? */
   if (oauth2->first_time && oauth2->server_mthd[OA2_GetDeviceCode].name){
     oauth2deviceinfo_t ogdi;

     mm_log("Attempting DEVICE method.", (long) NIL);
     json = oauth2_json_reply(oauth2->server_mthd[OA2_GetDeviceCode], oauth2, &status);

     if(json != NULL){
	JSON_S *jx;

        json_assign ((void **) &oauth2->devicecode.device_code, json, "device_code", JString);
        json_assign ((void **) &oauth2->devicecode.user_code, json, "user_code", JString);
        json_assign ((void **) &oauth2->devicecode.verification_uri, json, "verification_uri", JString);
	if((jx = json_body_value(json, "expires_in")) != NULL){
	   char tmp[MAILTMPLEN];

	   switch(jx->jtype){
	      case JString: oauth2->devicecode.expires_in = atoi((char *) jx->value);
			    break;
	      case JLong  : oauth2->devicecode.expires_in = *(long *) jx->value;
			    break;
		default   : break;
	   }
	   sprintf(tmp, "Authorization code is valid for the next %lu minutes only", oauth2->devicecode.expires_in/60);
	   mm_log(tmp, WARN);	   
	}

	if((jx = json_body_value(json, "interval")) != NULL)
	   switch(jx->jtype){
	      case JString: oauth2->devicecode.interval = atoi((char *) jx->value);
			    break;
	      case JLong  : oauth2->devicecode.interval = *(long *) jx->value;
			    break;
		default   : break;
	   }

        json_assign ((void **) &oauth2->devicecode.message, json, "message", JString);
	json_free(&json);

	if(oauth2->devicecode.verification_uri && oauth2->devicecode.user_code){
	   ogdi = (oauth2deviceinfo_t) mail_parameters (NIL, GET_OA2DEVICEINFO, NIL);
	   if(ogdi) (*ogdi)(oauth2, method, mb);
	}
	mm_log("Got Json reply. Completed parsing.", (long) NIL);
     }
     return;
   }

   /* else check if we have a refresh token, and in that case use it */

   if(oauth2->param[OA2_RefreshToken].value){

     mm_log("Attempting to get access token with known refresh token.", (long) NIL);

     json = oauth2_json_reply(oauth2->server_mthd[OA2_GetAccessTokenFromRefreshToken], oauth2, &status);

     if(json != NULL){
	JSON_S *jx;
	char *refresh_token;

	switch(status){
	   case HTTP_UNAUTHORIZED:
			mm_log("Client not authorized (wrong client-id?)", ERROR);
			oauth2->cancel_refresh_token++;
			break;
	   case HTTP_OK: if(oauth2->access_token)
			    fs_give((void **) &oauth2->access_token);
			 json_assign ((void **) &oauth2->access_token, json, "access_token", JString);
			 if((jx = json_body_value(json, "expires_in")) != NULL)
			 switch(jx->jtype){
			      case JString: oauth2->expiration = time(0) + atol((char *) jx->value);
				    break;
			      case JLong  : oauth2->expiration = time(0) + *(long *) jx->value;
				    break;
				default   : break;
			 }
			 oauth2->cancel_refresh_token = 0;	/* do not cancel this token. It is good */
			 mm_log("Got new access token.", (long) NIL);
			 json_assign ((void **) &refresh_token, json, "refresh_token", JString);
			 if(refresh_token){
			    if(oauth2->param[OA2_RefreshToken].value)
			       fs_give((void **) &oauth2->param[OA2_RefreshToken].value);
			    oauth2->param[OA2_RefreshToken].value = refresh_token;
			    mm_log("Got new refresh token.", (long) NIL);
			 }
			 break;

	     default :  { char tmp[200];
			  char *err, *err_desc;
			  jx = json_body_value(json, "error");
			  err = cpystr(jx && jx->jtype == JString ? (char *) jx->value : "Unknown error");
			  jx = json_body_value(json, "error_description");
			  err_desc = cpystr(jx && jx->jtype == JString ? (char *) jx->value : "No description");
			  sprintf(tmp, "Code %d: %.80s: %.80s", status, err, err_desc);
			  mm_log (tmp, ERROR);
			  if(err) fs_give((void **) &err);
			  if(err_desc) fs_give((void **) &err_desc);
			  oauth2->cancel_refresh_token++;
			}
			break;
	}
	json_free(&json);
     }
     return;
   }
   /* 
    * else, we do not have a refresh token, nor an access token.
    * We need to start the process to get an access code. We use this
    * to get an access token and refresh token.
    */
   { OAUTH2_SERVER_METHOD_S RefreshMethod = oauth2->server_mthd[OA2_GetAccessCode];
     HTTP_PARAM_S params[OAUTH2_PARAM_NUMBER + 2];

     mm_log("Starting AUTHORIZE method. No refresh token nor access token found.", (long) NIL);

     if(strcmp(RefreshMethod.name, "GET") == 0)
       (void) oauth2_loopback_start(oauth2);

     LOAD_HTTP_PARAMS(RefreshMethod, params);

     /*
      * The redirect is verified against the state parameter, so make
      * sure the authorization URL carries one even for providers whose
      * parameter list leaves it out.
      */
     if(oauth2_loopback_active() && oauth2->param[OA2_State].value){
	int i, have = 0;

	for(i = 0; params[i].name; i++)
	  if(!strcmp(params[i].name, "state"))
	    have = 1;

	if(!have && i + 1 < OAUTH2_PARAM_NUMBER + 2){
	   params[i].name = "state";
	   params[i].value = oauth2->param[OA2_State].value;
	   params[i+1].name = params[i+1].value = NULL;
	}
     }

     if(strcmp(RefreshMethod.name, "GET") == 0){
	char *server = xoauth2_server(RefreshMethod.urlserver, oauth2->param[OA2_Tenant].value);
	char *url = http_get_param_url(server, params);
	oauth2getaccesscode_t ogac = 
	(oauth2getaccesscode_t) mail_parameters (NIL, GET_OA2CLIENTGETACCESSCODE, NIL);

	/*
	 * The client polls the loopback listener while it shows the
	 * URL, so when it comes back without a code the user cancelled;
	 * do not wait any longer here.
	 */
	if(ogac)
	  oauth2->param[OA2_Code].value = (*ogac)(url, method, oauth2, mb, tryanother);

	oauth2_loopback_stop();

	if(server) fs_give((void **) &server);
	if(url) fs_give((void **) &url);
     }

     if(oauth2->param[OA2_Code].value){
	json = oauth2_json_reply(oauth2->server_mthd[OA2_GetAccessTokenFromAccessCode], oauth2, &status);

	if(json != NULL){
	   JSON_S *jx;

	  switch(status){
	     case HTTP_OK : if(oauth2->param[OA2_RefreshToken].value)
			       fs_give((void **) &oauth2->param[OA2_RefreshToken].value);
			    json_assign ((void **) &oauth2->param[OA2_RefreshToken].value, json, "refresh_token", JString);
			    if(oauth2->access_token)
			       fs_give((void **) &oauth2->access_token);
			    json_assign ((void **) &oauth2->access_token, json, "access_token", JString);

			    if((jx = json_body_value(json, "expires_in")) != NULL)
			      switch(jx->jtype){
				case JString: oauth2->expiration = time(0) + atol((char *) jx->value);
				    break;
				case JLong  : oauth2->expiration = time(0) + *(long *) jx->value;
				    break;
				default   : break;
			      }
			    oauth2->cancel_refresh_token = 0;	/* do not cancel this token. It is good */
			    mm_log("Got new refresh and access token.", (long) NIL);
			    break;

	     case HTTP_BAD :
		default    :  { char tmp[200];
			        char *err, *err_desc;
			        jx = json_body_value(json, "error");
			        err = cpystr(jx && jx->jtype == JString ? (char *) jx->value : "Unknown error");
			        jx = json_body_value(json, "error_description");
			        err_desc = cpystr(jx && jx->jtype == JString ? (char *) jx->value : "No description");
			        sprintf(tmp, "Code %d: %.80s: %.80s", status, err, err_desc);
			        mm_log (tmp, ERROR);
			        if(err) fs_give((void **) &err);
			        if(err_desc) fs_give((void **) &err_desc);
			        oauth2->cancel_refresh_token++;
			      }
	  }
	  json_free(&json);
	}
     }
     else
        mm_log("Failed to obtain authorization code. Cancelled by user?", (long) NIL);
     return;
   }
}

void
oauth2deviceinfo_get_accesscode(void *inp, void *outp)
{
  OAUTH2_DEVICEPROC_S *oad = (OAUTH2_DEVICEPROC_S *) inp;
  OAUTH2_S *oauth2 = oad->xoauth2;
  OAUTH2_DEVICECODE_S *dcode = &oauth2->devicecode;
  int done = 0, status, rv;
  JSON_S *json;

  mm_log("oauth2deviceinfo: getting accesscode.", (long) NIL);

  if(dcode->device_code && oauth2->param[OA2_DeviceCode].value == NULL)
     oauth2->param[OA2_DeviceCode].value = cpystr(dcode->device_code);

  rv = OA2_CODE_WAIT;	/* wait by default */
  json = oauth2_json_reply(oauth2->server_mthd[OA2_GetAccessTokenFromAccessCode], oauth2, &status);

  if(json != NULL){
     JSON_S *jx;
     char *error = NIL;

     switch(status){
	case HTTP_BAD : json_assign ((void **) &error, json, "error", JString);
		        if(!error) break;
			if(compare_cstring(error, "authorization_pending") == 0)
			   rv = OA2_CODE_WAIT;
			else if(compare_cstring(error, "authorization_declined") == 0)
			   rv = OA2_CODE_FAIL;
			else if(compare_cstring(error, "bad_verification_code") == 0)
			   rv = OA2_CODE_FAIL;
			else if(compare_cstring(error, "expired_token") == 0)
			   rv = OA2_CODE_FAIL;
			else	/* keep waiting? */
			   rv = OA2_CODE_WAIT;
			mm_log(rv == OA2_CODE_FAIL ? error : "waiting for process to end.",
			       rv == OA2_CODE_FAIL ? ERROR : (long) NIL);
			if(error) fs_give((void **) &error);
			break;

	case HTTP_OK :   if(oauth2->param[OA2_RefreshToken].value)
			   fs_give((void **) &oauth2->param[OA2_RefreshToken].value);
			 json_assign ((void **) &oauth2->param[OA2_RefreshToken].value, json, "refresh_token", JString);
			 if(oauth2->access_token)
			       fs_give((void **) &oauth2->access_token);
			 json_assign ((void **) &oauth2->access_token, json, "access_token", JString);

			if((jx = json_body_value(json, "expires_in")) != NULL)
			  switch(jx->jtype){
				case JString: oauth2->expiration = time(0) + atol((char *) jx->value);
				   break;
				case JLong  : oauth2->expiration = time(0) + *(long *) jx->value;
				   break;
				default   : break;
			  }

			rv = OA2_CODE_SUCCESS;
			oauth2->cancel_refresh_token = 0;	/* do not cancel this token. It is good */
			mm_log("Got new refresh and access token.", (long) NIL);
			break;

	     default :  { char tmp[100];
			  sprintf(tmp, "Oauth device Received Code %d.", status);
			  mm_log (tmp, ERROR);
			  oauth2->cancel_refresh_token++;
			}
     }
     json_free(&json);
  }

  *(int *)outp = rv;
}

XOAUTH2_INFO_S *
new_xoauth2_info(void)
{
  XOAUTH2_INFO_S *rv = fs_get(sizeof(XOAUTH2_INFO_S));
  memset((void *) rv, 0, sizeof(XOAUTH2_INFO_S));
  return rv;
}

void
free_xoauth2_info(XOAUTH2_INFO_S **xp)
{
  if(xp == NULL || *xp == NULL) return;

  if((*xp)->name) fs_give((void **) &(*xp)->name);
  if((*xp)->client_id) fs_give((void **) &(*xp)->client_id);
  if((*xp)->client_secret) fs_give((void **) &(*xp)->client_secret);
  if((*xp)->tenant) fs_give((void **) &(*xp)->tenant);
  if((*xp)->flow) fs_give((void **) &(*xp)->flow);
  if((*xp)->users) fs_give((void **) &(*xp)->users);
  fs_give((void **) xp);
}

XOAUTH2_INFO_S *
copy_xoauth2_info(XOAUTH2_INFO_S *x)
{
  XOAUTH2_INFO_S *y;

  if(x == NULL) return NULL;
  y = new_xoauth2_info();
  if(x->name) y->name = cpystr(x->name);
  if(x->client_id) y->client_id = cpystr(x->client_id);
  if(x->client_secret) y->client_secret = cpystr(x->client_secret);
  if(x->tenant) y->tenant = cpystr(x->tenant);
  if(x->flow) y->flow = cpystr(x->flow);
  if(x->users) y->users = cpystr(x->users);
  return y;
}

int
same_xoauth2_info(XOAUTH2_INFO_S x, XOAUTH2_INFO_S y)
{
   int rv = 0;
   if(x.name && y.name && !strcmp((char *) x.name, (char *) y.name)
	&& x.client_id && y.client_id && !strcmp(x.client_id, y.client_id)
	&& ((!x.client_secret && !y.client_secret)
		|| (x.client_secret && y.client_secret && !strcmp(x.client_secret, y.client_secret)))
	&& ((!x.tenant && !y.tenant) || (x.tenant && y.tenant && !strcmp(x.tenant, y.tenant))))
	rv = 1;
   return rv;
}

void
free_xoauth2_info_list(XOAUTH2_INFO_S ***xp)
{
  int i;

  if(xp == NULL || *xp == NULL || **xp == NULL) return;
  for(i = 0; (*xp)[i] != NULL; i++) free_xoauth2_info(&(*xp)[i]);
  fs_give((void **) xp);
}


/* This function does not create a refresh token and and
 * an access token, but uses an already known refresh token
 * to generate a refresh token on an ALREADY OPEN stream.
 * The assumption is that the user has already unlocked all
 * passwords and the app can access them from some source
 * (key chain/credentials/memory) to go through this
 * process seamlessly.
 */
void renew_accesstoken(MAILSTREAM *stream)
{
    OAUTH2_S oauth2;
    NETMBX mb;
    char user[MAILTMPLEN];
    int tryanother;
    unsigned long trial = 0;

    mm_log("renew_accesstoken().", (long) NIL);

    memset((void *) &oauth2, 0, sizeof(OAUTH2_S));
    mail_valid_net_parse(stream->original_mailbox, &mb);
    user[0] = '\0';
    mm_login_method (&mb, user, (void *) &oauth2, trial, stream->auth.name);

    if(oauth2.access_token)	/* we need a new one */
      fs_give((void **) &oauth2.access_token);

    if(stream->auth.expiration == 0){
       stream->auth.expiration = oauth2.expiration;
       if(oauth2.param[OA2_RefreshToken].value) fs_give((void **) &oauth2.param[OA2_RefreshToken].value);
       return;
    }

    oauth2.param[OA2_State].value = oauth2_generate_state();

    mm_login_oauth2_c_client_method (&mb, user, stream->auth.name, &oauth2, trial, &tryanother);

    if(oauth2.access_token)
	mm_login_method (&mb, user, (void *) &oauth2, trial, stream->auth.name);

    stream->auth.expiration = oauth2.expiration;
    oauth2_free_extra_values(&oauth2);
}
