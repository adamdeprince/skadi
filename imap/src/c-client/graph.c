/* 
 * Copyright 2021-2026 Eduardo Chappa
 *
 * Created: June 24, 2021.
 * Last Edited: April 26, 2026
 *
 */

#include <ctype.h>
#include <stdio.h>
#include <time.h>
#include "c-client.h"
#include "json.h"

#define GRAPHMINCOUNTMSGS  (10)		/* minimum number of messages to fetch */
#define GRAPHMAXCOUNTMSGS  (1000)	/* maximum number of messages to fetch */
#define GRAPHCOUNTMESSAGES (20)		/* get these many messages per fetch   */
#define GRAPHCOUNTFOLDERS  (20)		/* get these many folders in first fetch */
#define GRAPHCOUNTSEARCHITEMS	(50)	/* how many messages to get each search */
#define MAXBATCHIDLOAD 		(12)	/* maximum number of requests when requesting Id */
#define MAXBATCHLOAD		(20)	/* maximum number of requests in a batch */

#define GRAPHENVELOPE "sentDateTime,subject,internetMessageId,from,sender,toRecipients,ccRecipients,bccRecipients,replyTo,isRead,isDraft"
#define GRAPHATTACHMENT "isInline,name,size,id,contentType,lastModifiedDateTime"

/* list of folders whose names are reserved and do no need a special id to access them */
#define WELLKNOWN ",inbox,drafts,sentitems,junkemail,archive,searchfolders,archive,deleteditems,outbox,clutter,conversationhistory,conflicts,localfailures,msffolderroot,recoverableitemsdeletions,scheduled,serverfailures,syncissues,"

/* separator between different levels of subfolders */
#define GSEPC '/'
#define GSEPS "/"

/* These are the type of checks that are made on every ping */
#define GRAPH_NEWMSGS	(0)
#define GRAPH_UPDMSGS	(1)
#define GRAPH_DELMSGS	(2)

typedef enum 	{ HTTPGet,			/* The operations below use HTTP "GET" */
		      GetAttachmentFile,
		      GetAttachmentList,
		      GetBodyText,
		      GetEnvelope,
		      GetFoldersList,
		      GetFolderMessages,
		      GetMimeMsg,
		      MessageSearch,
		  HTTPPost,			/* The operations below use HTTP "POST" */
		      CopyMessage,
		      CreateFolder,
		      DeleteFolder,
		      FlagMsg,
		      GraphSendMail,
		      GetInitialSync,
		      GetMailboxChanges,
		      ExpungeMsg,
		  HTTPPatch,			/* The operations below use HTTP "PATCH" */
		      GraphRename,
		      GraphUpdate
		} GraphOperation;

/* graph parameters */
static long envelopeonly = NIL;		/* sets if only the envelope is downloaded, or all headers */
static long preferplaintext = NIL;	/* use graph default setting */
static GraphOperation GraphOp = GetFoldersList;	/* default operation on graph_open() */
static long GraphCountMessages = (long) GRAPHCOUNTMESSAGES; /* number of messages to get at the time */
static long GraphMoveOnExpunge = 0L;   /* sets default to not to move messages to Trash on expunge */

/* not all odata fields apply to all objects, but we put them all together */
typedef struct odata_s {
  unsigned char *context;
  unsigned char *nextlink;
  unsigned char *deltalink;
  unsigned char *etag;
  unsigned char *type;
  unsigned char *mediaContentType;
} ODATA_S;

typedef struct graph_attachment_s {
  ODATA_S odata;		/* type, mediaContentType */
  unsigned char *contentType;
  unsigned char *contentLocation;
  unsigned char *contentBytes;
  unsigned char *contentId;
  unsigned char *lastModifiedDateTime;
  unsigned char *id;
  unsigned int isInline:1;	/* boolean */
  unsigned char *name;
  unsigned long size;
  struct graph_attachment_s *next;
} GRAPH_ATTACHMENT;

typedef struct emailaddress_s {
  unsigned char *name;
  unsigned char *address;
  struct emailaddress_s *next;
} GRAPH_ADDRESS_S;

typedef struct graph_parameter_s {
   unsigned char *name;
   unsigned char *value;
   struct graph_parameter_s *next;
} GRAPH_PARAMETER;

typedef struct graph_list_s {
  unsigned char *elt;
  struct graph_list_s *next;
} GRAPH_LIST_S;

typedef struct graph_message_s {
  struct {
    unsigned int searched:1;
  } internal;
  ODATA_S odata;			/* etag, type, etc. */
  unsigned char *id;
  unsigned char *createdDateTime;
  unsigned char *lastModifiedDateTime;
  unsigned char *changeKey;
  GRAPH_LIST_S *categories;		/* Keywords */
  unsigned char *receivedDateTime;
  unsigned char *sentDateTime;
  unsigned char *internetMessageId;
  unsigned int hasAttachments:1;	/* boolean */
  unsigned int gotAttachmentList:1;	/* boolean */
  GRAPH_ATTACHMENT *attachments;	/* null if not any */
  unsigned char *subject;
  unsigned char *bodyPreview;
  unsigned char *importance;
  unsigned char *parentFolderId;
  unsigned char *conversationId;
  unsigned char *conversationIndex;
  unsigned char *isDeliveryReceiptRequested;
  unsigned int isReadReceiptRequested:1;
  unsigned int isRead:1;
  unsigned int isDraft:1;
  unsigned char *weblink;
  unsigned char *interferenceClassification;
  struct bodymsg_s {
    unsigned char *contentType;
    unsigned char *content;
  } body;
  GRAPH_ADDRESS_S *sender;
  GRAPH_ADDRESS_S *from;
  GRAPH_ADDRESS_S *toRecipients;
  GRAPH_ADDRESS_S *ccRecipients;
  GRAPH_ADDRESS_S *bccRecipients;
  GRAPH_ADDRESS_S *replyTo;
  struct flag_s {
     unsigned char *flagStatus;
  } flag;
  GRAPH_PARAMETER *internetMessageHeaders;
  unsigned long msgno;
  unsigned long rfc822_size;	/* only an approximation */
  unsigned int valid;
  unsigned char *mimetext;
  struct graph_message_s *next;
} GRAPH_MESSAGE;

typedef struct graph_folder_s {
   unsigned char *id;
   unsigned char *displayName;
   unsigned char *parentFolderId;
   unsigned long childFolderCount;
   unsigned long unreadItemCount;
   unsigned long totalItemCount;
   unsigned long sizeInBytes;
   unsigned int  isHidden;
   struct graph_folder_s *next;
} GRAPH_USER_FOLDERS;

typedef struct graph_local {
  NETSTREAM *netstream;		/* TCP I/O stream */
  HTTPSTREAM *http_stream;	/* stream to graph.microsoft.com */
  int status;			/* return status of http transaction */
  int cflags;			/* connection flags HTTP_XXX */
  int purpose;			/* a flag that indicates what we are trying to get */
  char *access_token;		/* our most current access_token */
  char *basename;		/* the base for POST and GET requests */
  unsigned char *resource;	/* resource we want, typically "message" */
  char *nextlink;		/* link given to us for next data. Short lived */
  GRAPH_USER_FOLDERS *folders;	/* users folders */
  unsigned long folderscount;	/* number of folders the user has */
  GraphOperation op;		/* Operation being performed */
  unsigned long countmessages;	/* count of the number of messages to be fetched */
  unsigned long topmsg;		/* msgno of last message fetched */
  GRAPH_PARAMETER *http_header;	/* additional headers in HTTP GET request */
  HTTP_PARAM_S *params;		/* http parameters "?$top=20,$count=true" etc */
  HTTP_PARAM_S *eparam;		/* extra http parameters "?$select=id" etc */
  char *urltail;		/* alternative urltail, when needed */
  struct {
    unsigned char *displayName;	/* display name of folder */
    unsigned char *id;		/* id of folder */
  } folder;
  void *private;		/* structure that a function wants to know */
  unsigned int tls1 : 1;	/* using TLSv1 over SSL */
  unsigned int tls1_1 : 1;	/* using TLSv1_1 over SSL */
  unsigned int tls1_2 : 1;	/* using TLSv1_2 over SSL */
  unsigned int tls1_3 : 1;	/* using TLSv1_3 over SSL */
  unsigned int novalidate : 1;	/* certificate not validated */
  unsigned int loser : 1;	/* server is a loser */
  unsigned int saslcancel : 1;	/* SASL cancelled by protocol */
  unsigned long auth;		/* authenticator list (only bearer is needed) */
  char *user;			/* logged-in user */
  char tmp[MAILTMPLEN];		/* temporary buffer */
  unsigned int sensitive: 1;
  unsigned int sync: 1;		/* has the initial sync been done? */
  struct {
	unsigned char *nextlink;
	unsigned char *deltalink;
  } created;
  struct {
	unsigned char *nextlink;
	unsigned char *deltalink;
  } updated;
  struct {
	unsigned char *nextlink;
	unsigned char *deltalink;
  } deleted;
  unsigned char *srchnextlink;	/* link to next search */
} GRAPHLOCAL;

typedef struct request_s {
   time_t req_time;
   struct request_s *next;
} REQUEST_S;

/* validity of information */
#define GPH_NONE	0x000
#define GPH_ENVELOPE	0x001
#define GPH_BODY	0x002
#define GPH_ATTACHMENTS	0x004
#define GPH_MIME	0x008
#define GPH_ID		0x010
#define GPH_STATUS	0x020
#define GPH_RFC822_BODY	0x040
#define GPH_ALL_HEADERS	0x080

#define LOCAL ((GRAPHLOCAL *) stream->local)
//#define GDPP  ((GRAPH_MESSAGE *) elt->private.driverp)
#define GDPQP	((GRAPH_MESSAGE **) &elt->private.driverp)
#define GDPQ	((GRAPH_MESSAGE *) elt->private.driverp)
#define GDPP(X)  ((GRAPH_MESSAGE *) DPP(X))
#define GRAPHBASE "https://graph.microsoft.com/v1.0/me"
#define GRAPHSITELEN (27)	/* strlen("https://graph.microsoft.com") */
#define GRAPHBASELEN (36)	/* strlen("https://graph.microsoft.com/v1.0/me/") */


void graph_parse_copy(MAILSTREAM *, JSON_S *);
void graph_parse_expunge(MAILSTREAM *, JSON_S *);
void graph_extract_next_link(JSON_S *, unsigned char **, unsigned char **);
void graph_parse_headers_by_id(MAILSTREAM *, JSON_S *, unsigned long);
long graph_get_message_header_text(MAILSTREAM *, unsigned long, int);
void graph_parse_body_text(MAILSTREAM *, JSON_S *, unsigned long *);
void graph_get_message_body_text(MAILSTREAM *, unsigned long);
unsigned char *graph_quoted(unsigned char *);
unsigned long graph_estimate_msg_size(GRAPH_MESSAGE *);
int folder_is_wellknown(unsigned char *);
int graph_initial_sync (MAILSTREAM *);
void graph_parse_message_search (MAILSTREAM *, JSON_S *);
void graph_parse_mailbox_changes (MAILSTREAM *, JSON_S *);
long graph_check_mailbox_changes (MAILSTREAM *);
long graph_text (MAILSTREAM *,unsigned long,STRING *,long);
char *graph_header (MAILSTREAM *, unsigned long, unsigned long *, long);
int graph_mark_msg_seen (MAILSTREAM *, unsigned long, int);
void graph_promote_body (BODY *, BODY *, char *);
GRAPH_USER_FOLDERS *graph_parse_folders (MAILSTREAM *, JSON_S *);
GRAPH_USER_FOLDERS *graph_parse_folders_work (MAILSTREAM *, JSON_S *);
GRAPH_USER_FOLDERS *graph_folder_and_base(MAILSTREAM *, char *, unsigned char **);
void graph_parse_initial_sync (MAILSTREAM *, JSON_S *);
GRAPH_ATTACHMENT *graph_parse_attachment_list (MAILSTREAM *, JSON_S *);
GRAPH_ATTACHMENT *graph_parse_attachment_list_work (JSON_S *);
void graph_parse_message_list (MAILSTREAM *, JSON_S *, unsigned long *);
GRAPH_LIST_S *graph_list_assign (JSON_S *, char *);
GRAPH_LIST_S *graph_list_assign_work (JSON_S *);
GRAPH_ADDRESS_S *graph_emailaddress (JSON_S *, char *, JObjType);
GRAPH_ADDRESS_S *graph_emailaddress_work (JSON_S *, JObjType);
void graph_send_command (MAILSTREAM *);
void graph_parse_fast (MAILSTREAM *, unsigned long, unsigned long, int);
void graph_parse_flags (MAILSTREAM *,MESSAGECACHE *);
GRAPH_PARAMETER *graph_headers_parse (JSON_S *);
void graph_parse_envelope (MAILSTREAM *,ENVELOPE **, GRAPH_MESSAGE *);
ENVELOPE *graph_structure (MAILSTREAM *, unsigned long, BODY **, long);
void graph_free_folders (GRAPH_USER_FOLDERS **);
void graph_free_params (GRAPH_PARAMETER **);
ADDRESS *graph_msg_to_address (GRAPH_ADDRESS_S *);
char *graph_transform_date (char *);
char *graph_filter_string (char *, long *);
GRAPH_MESSAGE *graph_fetch_msg (MAILSTREAM *, unsigned long);
long graph_get_attachment_list (MAILSTREAM *, GRAPH_MESSAGE *);
GRAPH_USER_FOLDERS *graph_get_folder_list (MAILSTREAM *, unsigned char *);
GRAPH_USER_FOLDERS *graph_get_folder_info (MAILSTREAM *, unsigned char *);
void graph_parse_body_structure (MAILSTREAM *,BODY *, GRAPH_MESSAGE *, char *);
void graph_update_message (GRAPH_MESSAGE **, JSON_S *);
void graph_free_address (GRAPH_ADDRESS_S **);
void graph_list_free (GRAPH_LIST_S **);
void graph_assign_msglist_messages (MAILSTREAM *, JSON_S *, unsigned long *);
GRAPH_MESSAGE *graph_message_new(void);
void graph_message_free(GRAPH_MESSAGE **);
void free_req_list(REQUEST_S **);


DRIVER *graph_valid (char *name);
void graph_scan (MAILSTREAM *stream,char *ref,char *pat,char *contents);
void graph_list (MAILSTREAM *stream,char *ref,char *pat);
void graph_lsub (MAILSTREAM *stream,char *ref,char *pat);
long graph_subscribe (MAILSTREAM *stream,char *mailbox);
long graph_unsubscribe (MAILSTREAM *stream,char *mailbox);
long graph_create (MAILSTREAM *stream,char *mailbox);
long graph_delete (MAILSTREAM *stream,char *mailbox);
long graph_rename (MAILSTREAM *stream,char *old,char *newname);
long graph_status (MAILSTREAM *stream,char *mbx,long flags);
long graph_renew (MAILSTREAM *stream,MAILSTREAM *m);
MAILSTREAM *graph_open (MAILSTREAM *stream);
HTTPSTREAM *graph_open_netstream (MAILSTREAM *stream);

long graph_auth (MAILSTREAM *stream,NETMBX *mb);
void *graph_challenge (void *stream,unsigned long *len);
long graph_response (void *stream,char *base,char *s,unsigned long size);
long graph_response_get (void *stream,char *base,char *s,unsigned long size);
long graph_response_post (void *stream,char *base,char *s,unsigned long size);
long graph_response_patch (void *stream,char *base,char *s,unsigned long size);

void graph_close (MAILSTREAM *stream,long options);
void graph_close_netstream (MAILSTREAM *stream);

void graph_fast (MAILSTREAM *stream,char *sequence,long flags);
long graph_overview (MAILSTREAM *,overview_t);
long graph_msgdata (MAILSTREAM *stream,unsigned long msgno,char *section,
		   unsigned long first,unsigned long last,STRINGLIST *lines,
		   long flags);
void graph_flag (MAILSTREAM *stream, char *sequence, char *flag, long flags);
long graph_search (MAILSTREAM *stream,char *charset,SEARCHPGM *pgm,long flags);
unsigned long *graph_sort (MAILSTREAM *stream,char *charset,SEARCHPGM *spg,
			  SORTPGM *pgm,long flags);
THREADNODE *graph_thread (MAILSTREAM *stream,char *type,char *charset,
			 SEARCHPGM *spg,long flags);
long graph_ping (MAILSTREAM *stream);
void graph_check (MAILSTREAM *stream);
long graph_expunge (MAILSTREAM *stream,char *sequence,long options);
long graph_copy (MAILSTREAM *stream,char *sequence,char *mailbox,long options);
long graph_append (MAILSTREAM *stream,char *mailbox,append_t af,void *data);
void graph_gc (MAILSTREAM *stream,long gcflags);
void graph_gc_body (BODY *body);
long graph_fetch_range (MAILSTREAM *stream, unsigned long *, unsigned long, int);
GRAPH_PARAMETER *graph_text_preference(void);

/* Driver dispatch used by MAIL */

DRIVER graphdriver = {
  "graph",			/* driver name */
				/* driver flags */
  DR_MAIL|DR_CRLF|DR_RECYCLE|DR_HALFOPEN|DR_SHORTLIVED,
  (DRIVER *) NIL,		/* next driver */
  graph_valid,			/* mailbox is valid for us */
  graph_parameters,		/* manipulate parameters */
  graph_scan,			/* scan mailboxes */
  graph_list,			/* find mailboxes */
  graph_lsub,			/* find subscribed mailboxes */
  graph_subscribe,		/* subscribe to mailbox */
  graph_unsubscribe,		/* unsubscribe from mailbox */
  graph_create,			/* create mailbox */
  graph_delete,			/* delete mailbox */
  graph_rename,			/* rename mailbox */
  graph_status,			/* status of mailbox */
  graph_open,			/* open mailbox */
  graph_close,			/* close mailbox */
  NIL,				/* fetch message "fast" attributes */
  NIL,				/* fetch message flags */
  graph_overview,		/* fetch overview */
  graph_structure,		/* fetch message envelopes */
  NIL,				/* fetch message header */
  NIL,				/* fetch message body */
  graph_msgdata,		/* fetch partial message */
  NIL,				/* unique identifier */
  NIL,				/* message number */
  graph_flag,			/* modify flags */
  NIL,				/* per-message modify flags */
  graph_search,			/* search for message based on criteria */
  graph_sort,			/* sort messages */
  graph_thread,			/* thread messages */
  graph_ping,			/* ping mailbox to see if still alive */
  graph_check,			/* check for new messages */
  graph_expunge,		/* expunge deleted messages */
  graph_copy,			/* copy messages to another mailbox */
  graph_append,			/* append string message to mailbox */
  graph_gc,			/* garbage collect stream */
  graph_renew			/* renew stream */
};


/* Driver dispatch used by MAIL */

DRIVER graphloserdriver = {
  "graphloser",			/* driver name */
				/* driver flags */
  DR_MAIL|DR_CRLF|DR_RECYCLE|DR_HALFOPEN|DR_SHORTLIVED,
  (DRIVER *) NIL,		/* next driver */
  graph_valid,			/* mailbox is valid for us */
  graph_parameters,		/* manipulate parameters */
  graph_scan,			/* scan mailboxes */
  graph_list,			/* find mailboxes */
  graph_lsub,			/* find subscribed mailboxes */
  graph_subscribe,		/* subscribe to mailbox */
  graph_unsubscribe,		/* unsubscribe from mailbox */
  graph_create,			/* create mailbox */
  graph_delete,			/* delete mailbox */
  graph_rename,			/* rename mailbox */
  graph_status,			/* status of mailbox */
  graph_open,			/* open mailbox */
  graph_close,			/* close mailbox */
  NIL,				/* fetch message "fast" attributes */
  NIL,				/* fetch message flags */
  graph_overview,		/* fetch overview */
  NIL,				/* fetch message envelopes */
  graph_header,			/* fetch message header */
  graph_text,			/* fetch message body */
  NIL,				/* fetch partial message */
  NIL,				/* unique identifier */
  NIL,				/* message number */
  graph_flag,			/* modify flags */
  NIL,				/* per-message modify flags */
  graph_search,			/* search for message based on criteria */
  graph_sort,			/* sort messages */
  graph_thread,			/* thread messages */
  graph_ping,			/* ping mailbox to see if still alive */
  graph_check,			/* check for new messages */
  graph_expunge,		/* expunge deleted messages */
  graph_copy,			/* copy messages to another mailbox */
  graph_append,			/* append string message to mailbox */
  graph_gc,			/* garbage collect stream */
  graph_renew			/* renew stream */
};

#define GRAPHTCPPORT (unsigned long) 80
#define GRAPHSSLPORT (unsigned long) 443
				/* prototype stream */
MAILSTREAM graphproto = {&graphdriver};

DRIVER *
graph_valid (char *name)
{
  return mail_valid_net (name,&graphdriver,NIL,NIL);
}

void *
graph_parameters (long function, void *value)
{
  void *ret = NIL;

  switch ((int) function) {
	case SET_GRAPHENVELOPEONLY: envelopeonly = (long) value;
	case GET_GRAPHENVELOPEONLY: ret = (void *) (long) envelopeonly;
				  break;

	case SET_PREFERPLAINTEXT: preferplaintext = (long) value;
	case GET_PREFERPLAINTEXT: ret = (void *) (long) preferplaintext;
				  break;

	case SET_GRAPHOPENOPERATION: GraphOp = (long) value;
	case GET_GRAPHOPENOPERATION: ret = (void *) (long) GraphOp;
				  break;

	case SET_GRAPHCOUNTMESSAGES: GraphCountMessages = (long) value;
	case GET_GRAPHCOUNTMESSAGES: ret = (void *) (GraphCountMessages < GRAPHMAXCOUNTMSGS
					? (GraphCountMessages < GRAPHMINCOUNTMSGS
						? GRAPHMINCOUNTMSGS : GraphCountMessages)
					: GRAPHMAXCOUNTMSGS);
				  break;

	case SET_GRAPHMOVEONEXPUNGE: GraphMoveOnExpunge = (long) value;
	case GET_GRAPHMOVEONEXPUNGE: ret = (void *) (long) GraphMoveOnExpunge;
				  break;

 	 default:
    		ret = NIL;		/* error case */
    		break;
  }
  return ret;
}

/*
 *  pass the text preference of the client to the graph server.
 */
GRAPH_PARAMETER *graph_text_preference(void)
{
   char tmp[MAILTMPLEN];
   unsigned long ppt;
   GRAPH_PARAMETER *gtp;

   gtp = fs_get(sizeof(GRAPH_PARAMETER));
   gtp->name = cpystr("Prefer");
   ppt = (unsigned long) mail_parameters(NIL, GET_PREFERPLAINTEXT, NIL);
   sprintf(tmp, "outlook.body-content-type=\"%s\"",  ppt ? "text" : "html");
   gtp->value = cpystr(tmp);
   gtp->next = NIL;
   return gtp;
}

GRAPH_MESSAGE *
graph_message_new(void)
{
  GRAPH_MESSAGE *msg;
  msg = fs_get(sizeof(GRAPH_MESSAGE));
  memset((void *) msg, 0, sizeof(GRAPH_MESSAGE));
  return msg;
}

void graph_message_free(GRAPH_MESSAGE **msgp)
{
   if(!msgp || !*msgp) return;

   if((*msgp)->odata.etag) fs_give((void **) &(*msgp)->odata.etag);
   if((*msgp)->odata.type) fs_give((void **) &(*msgp)->odata.type);
   if((*msgp)->id) fs_give((void **) &(*msgp)->id);
   if((*msgp)->createdDateTime) fs_give((void **) &(*msgp)->createdDateTime);
   if((*msgp)->lastModifiedDateTime) fs_give((void **) &(*msgp)->lastModifiedDateTime);
   if((*msgp)->changeKey) fs_give((void **) &(*msgp)->changeKey);
   if((*msgp)->receivedDateTime) fs_give((void **) &(*msgp)->receivedDateTime);
   if((*msgp)->sentDateTime) fs_give((void **) &(*msgp)->sentDateTime);
   if((*msgp)->internetMessageId) fs_give((void **) &(*msgp)->internetMessageId);
   if((*msgp)->subject) fs_give((void **) &(*msgp)->subject);
   if((*msgp)->bodyPreview) fs_give((void **) &(*msgp)->bodyPreview);
   if((*msgp)->importance) fs_give((void **) &(*msgp)->importance);
   if((*msgp)->parentFolderId) fs_give((void **) &(*msgp)->parentFolderId);
   if((*msgp)->conversationId) fs_give((void **) &(*msgp)->conversationId);
   if((*msgp)->conversationIndex) fs_give((void **) &(*msgp)->conversationIndex);
   if((*msgp)->isDeliveryReceiptRequested) fs_give((void **) &(*msgp)->isDeliveryReceiptRequested);
   if((*msgp)->weblink) fs_give((void **) &(*msgp)->weblink);
   if((*msgp)->interferenceClassification) fs_give((void **) &(*msgp)->interferenceClassification);
   if((*msgp)->categories) graph_list_free(&(*msgp)->categories);
   if((*msgp)->body.contentType) fs_give((void **) &(*msgp)->body.contentType);
   if((*msgp)->body.content) fs_give((void **) &(*msgp)->body.content);
   if((*msgp)->flag.flagStatus) fs_give((void **) &(*msgp)->flag.flagStatus);
   if((*msgp)->sender) graph_free_address(&(*msgp)->sender);
   if((*msgp)->from) graph_free_address(&(*msgp)->from);
   if((*msgp)->toRecipients) graph_free_address(&(*msgp)->toRecipients);
   if((*msgp)->ccRecipients) graph_free_address(&(*msgp)->ccRecipients);
   if((*msgp)->bccRecipients) graph_free_address(&(*msgp)->bccRecipients);
   if((*msgp)->replyTo) graph_free_address(&(*msgp)->replyTo);
   if((*msgp)->mimetext) fs_give((void **) &(*msgp)->mimetext);
   if((*msgp)->internetMessageHeaders) graph_free_params (&(*msgp)->internetMessageHeaders);
   fs_give((void **) msgp);
}


int
folder_is_wellknown(unsigned char *s)
{
   int i;
   unsigned char *t;
   char tmp[30 + 5];

   if(!s || !*s || strlen(s) > 30) return NIL;

   t = cpystr(s);	/* make a copy, in case "s" is a constant string */
   if(t != NULL){
      for(i = 0; s[i] != '\0'; i++) t[i] = tolower(t[i]);
      sprintf(tmp,",%s,", t);
      i =  strstr(WELLKNOWN, tmp) != NULL ? 1 : 0;
      fs_give((void **) &t);
   } else i = 0;
   return i;
}


void
graph_list_free(GRAPH_LIST_S **listp)
{
   if(listp == NULL || *listp == NULL) return;

   if((*listp)->elt) fs_give((void **) &(*listp)->elt);
   if((*listp)->next) graph_list_free(&(*listp)->next);
   fs_give((void **) listp); 
}

void
graph_free_address(GRAPH_ADDRESS_S **addrp)
{
  if(!addrp || !*addrp) return;

  if((*addrp)->name) fs_give((void **) &(*addrp)->name);
  if((*addrp)->address) fs_give((void **) &(*addrp)->address);
  if((*addrp)->next) graph_free_address(&(*addrp)->next);
  fs_give((void **) addrp);
}

/*
 * memory freed by caller. Text sent for searches
 * must duplicate a single quote, as in let''s go.
 */
unsigned char *
graph_quoted(unsigned char *s)
{
  unsigned char *q, *t, *u;
  int quotes;

  if(!s) return NULL;
  for(quotes = 0, t = s; *t; *t++)
     if(*t == '\'') quotes++;
  q = fs_get(strlen(s) + quotes + 1);
  for(t = s, u = q; *t; *t++){
     *u++ = *t;
     if(*t == '\'') *u++ = '\'';
  }
  *u = '\0';
  return q;
}

GRAPH_USER_FOLDERS *
graph_get_folder_list(MAILSTREAM *stream, unsigned char *fldid)
{
  GRAPH_USER_FOLDERS *gfolder = NIL, *gf;
  int i = 0; /* parameter counter */
  unsigned long total, request;

  if (!stream || !LOCAL) return NIL;

  LOCAL->op = GetFoldersList;
  total = 0L;
  request = GRAPHCOUNTFOLDERS;
  if(LOCAL->resource) fs_give((void **) &LOCAL->resource);
  buffer_add(&LOCAL->resource, "mailFolders");
  if (fldid) buffer_add(&LOCAL->resource, fldid);
  do {
			/* add parameters */
     LOCAL->params = fs_get(4*sizeof(HTTP_PARAM_S));
     memset((void *) LOCAL->params, 0, 4*sizeof(HTTP_PARAM_S));
			/* parameter 1: $top */
     LOCAL->params[i = 0].name = cpystr("$top");
     if(LOCAL->folderscount > 2*request){
	if(2*request > GRAPHMAXCOUNTMSGS)
	   request = GRAPHMAXCOUNTMSGS;
	else
	   request *= 2;
     }
     sprintf(LOCAL->tmp, "%lu", request);
     LOCAL->params[i++].value = cpystr(LOCAL->tmp);
			/* parameter 2: $skip */
     LOCAL->params[i].name = cpystr("$skip");
     sprintf(LOCAL->tmp, "%lu", total);
     LOCAL->params[i++].value = cpystr(LOCAL->tmp);
			/* parameter 3: $count */
     LOCAL->params[i].name = cpystr("$count");
     LOCAL->params[i++].value = cpystr("true");

     graph_send_command(stream);

     if(stream && LOCAL){
	if(!gfolder){	/* first time around set gfolder */
	   for(gf = LOCAL->folders; gf; gf = gf->next, total++);
	   gfolder = gf = LOCAL->folders;
	}
	else{		/* second time around, move to the end of gf */
	   for(; gf && gf->next; gf = gf->next, total++);
	   if(gf && !gf->next) total++;
	   gf->next = stream && LOCAL ? LOCAL->folders : NULL;
	}
     }
  } while (stream && LOCAL && total < LOCAL->folderscount);

  return (stream && LOCAL && gfolder) ? gfolder : NIL;
}

GRAPH_USER_FOLDERS *
graph_get_folder_info(MAILSTREAM *stream, unsigned char *fname)
{
  GRAPH_USER_FOLDERS *gfolder;

  if(LOCAL->folders) graph_free_folders(&LOCAL->folders);
  LOCAL->folders = graph_get_folder_list(stream, fname);

  if(stream && LOCAL && LOCAL->folders){
     for(gfolder = LOCAL->folders; gfolder; gfolder = gfolder->next)
         if((stream->inbox && !compare_cstring(fname, gfolder->displayName))
	     || !strcmp(fname, gfolder->displayName))
	   break;
  }
  return gfolder;
}

void graph_scan (MAILSTREAM *stream,char *ref,char *pat,char *contents)
{
}

GRAPH_USER_FOLDERS *
graph_folder_and_base(MAILSTREAM *stream, char *mailbox, unsigned char **basep)
{
   GRAPH_USER_FOLDERS *gf;
   unsigned char *base, *b, *e, *f;

   if(!stream || !LOCAL || !mailbox) return NIL;

   base = f = NULL;
   buffer_add(&f, GSEPS);
   if(mailbox && *mailbox)
      buffer_add(&f, mailbox);
   else
      buffer_add(&f, GSEPS);
   if(mailbox && *mailbox && mailbox[strlen(mailbox) - 1] != GSEPC)
      buffer_add(&f, GSEPS);
   b = f;
   e = strchr(++b, GSEPC);
   do {
      *e = '\0';
      if(LOCAL->folders) graph_free_folders(&LOCAL->folders);
      LOCAL->folders = graph_get_folder_list(stream, base);
      for(gf = LOCAL->folders; b && *b && gf; gf = gf->next){
	if(!compare_cstring(gf->displayName, b)){
	   buffer_add(&base, "/");
	   buffer_add(&base, gf->id);
	   buffer_add(&base, "/childFolders");
	   break;
	}
      }
      if(!gf) break;
      *e++ = GSEPC;
      b = e;
      e = strchr(b+1, GSEPC);
   } while (e != NULL);

   if(f) fs_give((void **) &f);

   if(basep) *basep = base;
   else if (base) fs_give((void **) &base);

   return gf;
}

void
graph_list (MAILSTREAM *stream, char *ref, char *pat)
{
    char prefix[MAILTMPLEN], *s;
    unsigned char *base = NIL;
    int pl;
    GRAPH_USER_FOLDERS *gf;

    if (ref && *ref) {            /* have a reference? */
       if ((stream && LOCAL)
	   || (stream = mail_open (NIL, ref, OP_HALFOPEN|OP_SILENT))){
	   pl = strchr (ref,'}') + 1 - ref;
	   strncpy (prefix,ref,pl);    /* build prefix */
	   prefix[pl] = '\0';          /* tie off prefix */
	   ref += pl;                  /* update reference */
       }
       else return;
    }
    else {
       if ((stream && LOCAL)
	   || (stream = mail_open (NIL, pat, OP_HALFOPEN|OP_SILENT))){
	   pl = strchr (pat,'}') + 1 - pat;
	   strncpy (prefix,pat,pl);    /* build prefix */
	   prefix[pl] = '\0';          /* tie off prefix */
	   pat += pl;                  /* update reference */
       }
       else return;
   }

   gf = graph_folder_and_base(stream, ref, &base);
   if(LOCAL->folders) graph_free_folders(&LOCAL->folders);
   LOCAL->folders = graph_get_folder_list(stream, base);
   if(base) fs_give((void **) &base);

   if(stream && LOCAL){
      for(gf = LOCAL->folders; gf; gf = gf->next){
	  unsigned long i;
			/* how can we display hidden folders? */
	  if(gf->isHidden || !gf->displayName) continue;

	  i = 0;
	  i |= gf->childFolderCount
		? LATT_HASCHILDREN : LATT_HASNOCHILDREN;

	  i |= gf->unreadItemCount
		? LATT_MARKED : LATT_UNMARKED;

	  if (prefix && ref && ((strlen (prefix) + strlen(ref) + strlen(gf->displayName)) < MAILTMPLEN))
	     sprintf (s = LOCAL->tmp,"%s%s%s",prefix, ref, gf->displayName);
	  else s = gf->displayName;

	  mm_list(stream, GSEPC, s, i);	/* every folder can have children */
      }
   }
}

void graph_lsub (MAILSTREAM *stream,char *ref,char *pat)
{
}

long graph_subscribe (MAILSTREAM *stream,char *mailbox)
{
  return 0L;
}

long graph_unsubscribe (MAILSTREAM *stream,char *mailbox)
{
  return 0L;
}

long graph_create (MAILSTREAM *stream,char *mailbox)
{
  char *json;
  char mbx[MAILTMPLEN], *f;
  unsigned char *base;
  GRAPH_USER_FOLDERS *gfolder;

  if(!mailbox || !*mailbox) return NIL;

  if(mail_valid_net(mailbox, &graphdriver, NIL, mbx)
     && stream && LOCAL){

     f = strrchr(mbx, GSEPC);
     if(f){
	*f++ = '\0';
	gfolder = graph_folder_and_base(stream, mbx, &base);
     }
     else{
	f = mbx;
	base = NIL;
     }

     LOCAL->op = CreateFolder;
     if(LOCAL->resource) fs_give((void **) &LOCAL->resource);
     LOCAL->resource = cpystr("mailFolders");
     if(base){
	buffer_add(&LOCAL->resource, base);
	fs_give((void **) &base);
     }
     json = fs_get(35 + strlen(mbx) + 1);
     sprintf(json, "{\"displayName\":\"%s\",\"isHidden\":false}", f);
     LOCAL->private = (void *) json;
     graph_send_command(stream);
     if(LOCAL->private) fs_give((void **) &LOCAL->private);
  }
  return stream && LOCAL && LOCAL->status == HTTP_OK_CREATED ? LONGT : NIL;
}

/* delete a folder */
long graph_delete (MAILSTREAM *stream, char *mailbox)
{
   GRAPH_USER_FOLDERS *gfolder;
   char mbx[MAILTMPLEN];

   if(!mailbox || !*mailbox 
      || !mail_valid_net(mailbox, &graphdriver, NIL, mbx)
      || !stream || !LOCAL) return NIL;

   gfolder = graph_folder_and_base(stream, mbx, NULL);

   if(!gfolder) return NIL;

   LOCAL->op = DeleteFolder;
   if(!LOCAL->private) LOCAL->private = cpystr("");	/* no body */
   if(LOCAL->resource) fs_give((void **) &LOCAL->resource);
   LOCAL->resource = fs_get(11 + 15 + strlen(gfolder->id) + 2 + 1);
   sprintf(LOCAL->resource, "mailFolders/%s/permanentDelete", gfolder->id);
   graph_send_command(stream);

   if(stream && LOCAL && LOCAL->private) fs_give((void **) &LOCAL->private);
   return stream && LOCAL && LOCAL->status == HTTP_OK_NO_CONTENT ? LONGT : NIL;
}

long graph_rename (MAILSTREAM *stream, char *old, char *newname)
{
   NETMBX omb, nmb;
   GRAPH_USER_FOLDERS *gf;
   unsigned char *text = NIL, *s;

   if(!stream || !LOCAL) return NIL;

   mail_valid_net_parse(old, &omb);
   gf = graph_folder_and_base(stream, omb.mailbox, NULL);
   if(!gf) return NIL;

   mail_valid_net_parse(newname, &nmb);
   if((s = strrchr(nmb.mailbox, GSEPC)) != NULL) *s++= '\0';
   else s = nmb.mailbox;
   

   LOCAL->op = GraphRename;
   if(LOCAL->resource) fs_give((void **) &LOCAL->resource);
   buffer_add(&LOCAL->resource, "mailFolders/");
   buffer_add(&LOCAL->resource, gf->id);

   buffer_add(&text, "{");
      buffer_add(&text, "\"displayName\":\"");
      buffer_add(&text, s);
      buffer_add(&text, "\"");
   buffer_add(&text, "}");

   LOCAL->private = (void *) text;
   graph_send_command(stream);


   if(LOCAL->private) fs_give((void **) &LOCAL->private);
   return stream && LOCAL && LOCAL->status == HTTP_OK ? LONGT : NIL;
}

long graph_status (MAILSTREAM *stream,char *mbx,long flags)
{
  MAILSTATUS status;
  long ret = NIL;
  MAILSTREAM *tstream =
    (stream && LOCAL->netstream && mail_usable_network_stream (stream,mbx)) ?
      stream : mail_open (NIL,mbx,OP_SILENT);
  if (tstream) {                /* have a usable stream? */
    status.flags = flags;       /* return status values */
    status.messages = tstream->nmsgs;
    status.recent = tstream->recent;
    if (flags & SA_UNSEEN) status.unseen = LOCAL->folders->unreadItemCount;
    status.uidnext = tstream->uid_last + 1;
    status.uidvalidity = tstream->uid_validity;
                                /* pass status to main program */
    mm_status (tstream,mbx,&status);
    if (stream != tstream) mail_close (tstream);
    ret = LONGT;
  }
  return ret;                   /* success */
}

long graph_renew (MAILSTREAM *stream, MAILSTREAM *m)
{
  GRAPHLOCAL *MLOCAL = m ? (GRAPHLOCAL *) m->local : NIL;
  NETSTREAM *xnetstream;

  if(!m) return LONGT;

  if(!stream){
      stream = m;
      return NIL;
  }

  if(!LOCAL){
     graph_close(stream, NIL);
     stream = m;
     return NIL;
  }

  xnetstream = LOCAL->netstream;
  LOCAL->netstream = MLOCAL->netstream;
  MLOCAL->netstream = xnetstream;

  return NIL;
}

/*
 * transform a reply by id to a reply by range, then call
 */
void 
graph_parse_headers_by_id(MAILSTREAM *stream, JSON_S *json, unsigned long topmsg)
{
   JSON_S *jx, *jy;

   jx = json_new();
   jx->jtype = JArray;
   jy = json_new();
   jx->name = cpystr("value");
   jx->value = (void *) jy;
   jy->value = (void *) json;

   graph_parse_message_list(stream, jx, &topmsg);
}


HTTPSTREAM *
graph_open_netstream (MAILSTREAM *stream)
{
    NETMBX mb;
    HTTPSTREAM *http;
    char *s;
    NETDRIVER *ssld = (NETDRIVER *) mail_parameters (NIL,GET_SSLDRIVER,NIL);
    unsigned long defprt = HTTPTCPPORT;
    unsigned long sslport = HTTPSSLPORT;

    memset((void *) &mb, 0, sizeof(NETMBX));
    if(http_valid_net_parse (LOCAL->basename, &mb) == 0)
	return NIL;

    http = fs_get(sizeof(HTTPSTREAM));
    memset((void *) http, 0, sizeof(HTTPSTREAM));

    s = strchr((char *) LOCAL->basename + 8 + 1, '/'); /* 8 = strlen("https://") + 1 */
    http->url     = cpystr(LOCAL->basename);
    http->urlhost = cpystr(mb.orighost);
    http->urltail = cpystr(s ? (char *) s : "/");
    http->debug   = stream->debug;
					/* recycle stream, if possible */
    if(!LOCAL->netstream){
	long open_timeout, read_timeout;
	void *tcpto;

	/* Get already set values */
	open_timeout = (long) mail_parameters(NIL, GET_OPENTIMEOUT, NIL);
	read_timeout = (long) mail_parameters(NIL, GET_READTIMEOUT, NIL);
	tcpto = mail_parameters(NIL, GET_TIMEOUT, NIL);

       /* set all timeouts low and DO NOT ask about timeouts */
	mail_parameters(NULL, SET_OPENTIMEOUT, (void *) (long) 2);
	mail_parameters(NULL, SET_READTIMEOUT, (void *) (long) 2);
	mail_parameters(NULL, SET_TIMEOUT, NIL);

	/* Now go get a stream with these conditions */
       if(ssld)
          LOCAL->netstream = net_open (&mb,NIL,defprt,ssld,"https", sslport);

	/* restore timeouts */
	mail_parameters(NULL, SET_OPENTIMEOUT, (void *) open_timeout);
	mail_parameters(NULL, SET_READTIMEOUT, (void *) read_timeout);
	mail_parameters(NULL, SET_TIMEOUT, (void *) tcpto);
    }
    http->netstream = LOCAL->netstream;

    return http;
}

MAILSTREAM *
graph_open (MAILSTREAM *stream)
{
  unsigned long i;
  char tmp[MAILTMPLEN], usr[NETMAXUSER];
  NETMBX mb;
  GraphOperation op;
  GRAPH_USER_FOLDERS *gf;


  if (!stream) return &graphproto;

  mail_valid_net_parse (stream->mailbox,&mb);
  if(compare_cstring("graph.microsoft.com", mb.orighost))
    return NIL;

  if (mb.loser) stream->dtb = &graphloserdriver;	/* switch driver from the beginning */
  usr[0] = '\0';
  if(mb.user && mb.user[0]) strcpy(usr, mb.user);
				/* copy flags from name */
  if (mb.dbgflag) stream->debug = T;
  if (mb.readonlyflag) stream->rdonly = T;
  if (mb.secflag) stream->secure = T;
  if (mb.trysslflag) stream->tryssl = T;

  stream->perm_seen = T;
  stream->perm_deleted = stream->perm_answered = stream->perm_draft = NIL;

  stream->sequence++;		/* bump sequence number */
  if (!LOCAL){
    stream->local = (void *) memset (fs_get (sizeof (GRAPHLOCAL)),0,sizeof (GRAPHLOCAL));

    if (mb.loser) LOCAL->loser = T;
    /* only BEARER and XOAUTH2 are supported */
    if ((i = mail_lookup_auth_name (BEARERNAME,AU_SECURE)) &&
            (--i < MAXAUTHENTICATORS)) LOCAL->auth |= (1 << i);

    LOCAL->basename = cpystr(GRAPHBASE);
				/* save state for future recycling */
    if (mb.tls1) LOCAL->tls1 = T;
    if (mb.tls1_1) LOCAL->tls1_1 = T;
    if (mb.tls1_2) LOCAL->tls1_2 = T;
    if (mb.tls1_3) LOCAL->tls1_3 = T;
    if (mb.novalidate) LOCAL->novalidate = T;
    if (mb.loser) LOCAL->loser = T;
  }

  /* some operations do not actually require us to get a connection before we can start
   * work. For example, sending an email does not require us to get a stream before we
   * prepare the message, but we need to set up the LOCAL variable so we can use the flow
   * of LOCAL->op, LOCAL->resource, graph_auth, etc. to set up everything we need. Because
   * of this, we stop the set up here, and return the already set up stream, ready for work
   */
  op = (GraphOperation) mail_parameters(NIL, GET_GRAPHOPENOPERATION, NIL);
  if(op == GraphSendMail){
     mail_parameters(NIL, SET_GRAPHOPENOPERATION, (void *)(long) GetFoldersList);
     return stream;
  }

  LOCAL->http_stream  = graph_open_netstream(stream);
  if (LOCAL && LOCAL->netstream) {	/* still have a connection? */
      sprintf (tmp,"{%s",(long) mail_parameters (NIL,GET_TRUSTDNS,NIL) ?
	     net_host (LOCAL->netstream) : mb.host);
      if (!((i = net_port (LOCAL->netstream)) & 0xffff0000))
	 sprintf (tmp + strlen (tmp),":%lu",i);
      strcat (tmp,"/graph");
      if (LOCAL->tls1) strcat (tmp,"/tls1");
      if (LOCAL->tls1_1) strcat (tmp,"/tls1_1");
      if (LOCAL->tls1_2) strcat (tmp,"/tls1_2");
      if (LOCAL->tls1_3) strcat (tmp,"/tls1_3");
      if (LOCAL->novalidate) strcat (tmp,"/novalidate-cert");
      if (LOCAL->loser) strcat (tmp,"/loser");
      if (stream->secure) strcat (tmp,"/secure");
      if (stream->rdonly) strcat (tmp,"/readonly");
      if (stream->anonymous) strcat (tmp,"/anonymous");
      else {			/* record user name */
         if (!LOCAL->user && usr[0]) LOCAL->user = cpystr (usr);
         if (LOCAL->user) sprintf (tmp + strlen (tmp),"/user=\"%s\"",
				LOCAL->user);
      }
      strcat (tmp,"}");
      if(stream->mailbox) fs_give((void **) &stream->mailbox);
      strcat (tmp,mb.mailbox);/* mailbox name */
      stream->mailbox = cpystr(tmp);
      stream->inbox = !compare_cstring (mb.mailbox,"INBOX");

      if(folder_is_wellknown(mb.mailbox)){
	 GRAPH_USER_FOLDERS *folders;
	 LOCAL->folder.id = cpystr(mb.mailbox);
	 LOCAL->folder.displayName = cpystr(mb.mailbox);
	 if(LOCAL->folders) graph_free_folders(&LOCAL->folders);
	 folders = graph_get_folder_list(stream, NULL);
	 if(stream && LOCAL)
	    for(gf = LOCAL->folders = folders; gf; gf = gf->next){
		if((stream->inbox && !compare_cstring(mb.mailbox, gf->displayName))
		   || !strcmp(mb.mailbox, gf->displayName))
		break;
	    }
	 else{
	    graph_close(stream, NIL);
	    return NIL;
	 }
      }
      else
	gf = graph_folder_and_base(stream, mb.mailbox, NULL);

      if(gf){
	 if(!LOCAL->folder.id) LOCAL->folder.id = cpystr(gf->id);
	 if(!LOCAL->folder.displayName) LOCAL->folder.displayName = cpystr(gf->displayName);
	 stream->nmsgs = gf->totalItemCount;
	 stream->unseen = gf->unreadItemCount;
	 mail_exists(stream, stream->nmsgs);
	 for (i = 1; i <= stream->nmsgs;i++) mail_elt (stream, i)->private.uid = i;
      }
  }
  else
    graph_close(stream, NIL);

  return stream && LOCAL ? stream : NIL;
}

/* We ask for the id of every message in the folder.
 * We do this as a batch request.
 * Every batch of about 5000 messages is about 1 MB in size,
 * and the server does not like to send more that 4 MB, so we
 * play it safe by creating batches of about 12000 messages each.
 * No more than 20 requests can be made in each batch.
 */
int
graph_initial_sync (MAILSTREAM *stream)
{
  unsigned long len, nmsgs, skip, id;
  int i, j, nreq;
  char tmp[MAILTMPLEN];
  HTTP_PARAM_S *params = NIL;
  unsigned char *resource, *t, *s;

  if(stream && LOCAL){
		/* for the post request */
     LOCAL->op = GetInitialSync;
     if(LOCAL->resource) fs_give((void **) &LOCAL->resource);
     LOCAL->resource = cpystr("$batch");
     LOCAL->urltail = cpystr("/v1.0");
		/* for each get request in the batch */
     resource = fs_get(2 + 11 + 8 + strlen(LOCAL->folder.id) + 4 + 1);
     sprintf(resource, "/me/mailFolders/%s/messages", LOCAL->folder.id);

     skip = len = 0;
     nmsgs = stream->nmsgs;
		/* count the number of requests of GRAPHMAXCOUNTMSGS messages */
     nreq = nmsgs/GRAPHMAXCOUNTMSGS;
     if(nreq*GRAPHMAXCOUNTMSGS < nmsgs) nreq++;

     s = NIL;
     for(j = 0; j < nreq; j++){
	id = j % MAXBATCHIDLOAD;
	if((j % MAXBATCHIDLOAD) == 0){
	   buffer_add(&s, "{");			/* start main json object */
	   buffer_add(&s, "\"requests\":[");	/* start array of requests */
	   LOCAL->countmessages = nmsgs < GRAPHMAXCOUNTMSGS ? nmsgs : GRAPHMAXCOUNTMSGS;
	}
	if((j % MAXBATCHIDLOAD) > 0) buffer_add(&s, ","); /* add a new element to the list */
	buffer_add(&s, "{");		/* start json request in array */
	buffer_add(&s, "\"id\":");	/* add id element */
	sprintf(tmp, "\"%lu\"", j);
	buffer_add(&s, tmp);
	buffer_add(&s, ",");
	buffer_add(&s, "\"method\":\"GET\",");	/* add method */
	buffer_add(&s, "\"url\":");		/* add url */

			/* create url by collecting all the information */
	i = 3;		 	/* add $top, $select, and extra blank */
	if((j % MAXBATCHIDLOAD) > 0) i++;		/* add $skip */
				/* add parameters */
	params = fs_get(i*sizeof(HTTP_PARAM_S));
	memset((void *) params, 0, i*sizeof(HTTP_PARAM_S));
				/* parameter 1, $top */
	params[i = 0].name = cpystr("$top");
	sprintf(tmp, "%lu", LOCAL->countmessages);
	params[i++].value = cpystr(tmp);
				/* parameter 2, $skip */
	if ((j % MAXBATCHIDLOAD) > 0){
	    params[i].name = cpystr("$skip");
	    sprintf(tmp, "%ld", skip);
	    params[i++].value = cpystr(tmp);
	}
				/* parameter 3, $select */
	params[i].name = cpystr("$select");
	params[i].value = cpystr("id");

	t = (void *) http_get_param_url(resource, params);
	buffer_add(&s, "\""); buffer_add(&s, t); buffer_add(&s, "\"");
	fs_give((void **) &t); http_param_free(&params);
	buffer_add(&s, "}");		/* end json request in array */
	nmsgs -= LOCAL->countmessages;
	skip += LOCAL->countmessages;

	if((((j + 1) % MAXBATCHIDLOAD) == 0) || ((j + 1) == nreq)){
	   buffer_add(&s, "]");		/* end array of requests */
	   buffer_add(&s, "}");		/* end main json object */
	   LOCAL->private = (char *) s;	/* set up body for graph_response */
	   graph_send_command(stream);
	   fs_give((void **) &s);
	}
     }
     if(resource) fs_give((void **) &resource);
  }
  if(stream && LOCAL && LOCAL->urltail) fs_give((void **) &LOCAL->urltail);
  if(stream && LOCAL) LOCAL->private = NIL;
  return stream && LOCAL ? LONGT : 0;
}

long
graph_auth (MAILSTREAM *stream,NETMBX *mb)
{
  unsigned long trial,ua,uasaved = NIL;
  int ok;
  char tmp[MAILTMPLEN], usr[MAILTMPLEN];
  char *lsterr = NIL, *base;
  AUTHENTICATOR *at, *atsaved;
  for (ua = LOCAL->auth, LOCAL->saslcancel = NIL; LOCAL->netstream && ua &&
       (at = mail_lookup_auth (find_rightmost_bit (&ua) + 1));) {
    if(mb && *mb->auth){
       if(!compare_cstring(at->name, mb->auth))
	  atsaved = at;
       else{
          uasaved = ua;
          continue;
      }
    }
    if (lsterr) {		/* previous authenticator failed? */
      sprintf (tmp,"Retrying using %s authentication after %.80s",
	       at->name,lsterr);
      mm_log (tmp,NIL);
      fs_give ((void **) &lsterr);
    }
    trial = 0;			/* initial trial count */
    tmp[0] = '\0';		/* no error */
    do {			/* gensym a new tag */
      if (lsterr) {		/* previous attempt with this one failed? */
	sprintf (tmp,"Retrying %s authentication after %.80s",at->name,lsterr);
	mm_log (tmp,WARN);
	fs_give ((void **) &lsterr);
      }
      LOCAL->saslcancel = NIL;
      strcpy (tmp,"Bearer");
      base = (char *) tmp;
      if (base) {
	LOCAL->sensitive = T;
	ok = (*at->client) (graph_challenge,graph_response,base,"graph",mb,stream,
			    net_port(LOCAL->netstream),&trial,usr);
	LOCAL->sensitive = NIL;

	if(base && !trial){	/* do it now, instead of later */
	  mm_log ("GRAPH Authentication cancelled",ERROR);
	  return NIL;
	}
				/* good if SASL ok and success response */
	if (ok){
	   if(stream->auth.name) fs_give((void **) &stream->auth.name);
	   stream->auth.name = cpystr(at->name);	/* save method name */
	   return T;
	}
	if (!trial) {		/* if main program requested cancellation */
	  mm_log ("GRAPH Authentication cancelled",ERROR);
	  return NIL;
	}
      }
    }
    while (LOCAL->netstream && trial && (trial < 3));
  }
  if (lsterr) {			/* previous authenticator failed? */
    if (!LOCAL->saslcancel) {	/* don't do this if a cancel */
      sprintf (tmp,"Can not authenticate to GRAPH server: %.80s",lsterr);
      mm_log (tmp,ERROR);
    }
    fs_give ((void **) &lsterr);
  }
  if(mb && *mb->auth){
     if(!uasaved) sprintf (tmp,"Failed to login using %.80s authenticator",mb->auth);
     else if (!atsaved) sprintf (tmp,"GRAPH server does not support AUTH=%.80s authenticator",mb->auth);
     if (!uasaved || !atsaved) mm_log (tmp,ERROR);
  }
  return NIL;			/* ran out of authenticators */
}

/* Here we parse information from the server. The reason we do this here
 * is because we need to send the Authorize: Bearer XXXXX header for every
 * request and we need to use/renew the access token as needed, so we take
 * advantage of this and do all of this at the same time. It is strange,
 * but it is the way it is.
 */
void *
graph_challenge (void *s, unsigned long *len)
{
  MAILSTREAM *stream = (MAILSTREAM *) s;
  unsigned char *ct, *u;
  unsigned char *t = http_response_from_reply(LOCAL->http_stream, &ct);
  int status = LOCAL->status;
  JSON_S *json = NIL;
  void *ret = NIL;
  int is_json, is_plain;

  if(t && *t){
     if(ct == NULL)
        return (void *) cpystr("graph_challenge: No Content-Type found. Returning.");

     if((u = strchr(ct, '/')) != NULL){
        u++;
        for(; isalpha(*u); u++);
        *u = '\0';
     }
     else{
        fs_give((void **) &ct);
        return (void *) cpystr("graph_challenge: Malformed Content-Type.");
     }

     is_json = is_plain = 0;
     if(!compare_cstring(ct, "application/json")){
        if(t) json = json_parse(t);
        if(json == NIL)
	   return (void *) cpystr("graph_challenge: Error in parsing JSON data.");
        is_json++;
     }
  }

  if(!compare_cstring(ct, "text/plain")) is_plain++;

  if(status >= 400){
    JSON_S *j;
    char *message;
    json_assign ((void **) &j, json, "error", JObject);
    json_assign ((void **) &message, j, "message", JString);
    if(message){
       mm_log(message, ERROR);
       fs_give((void **) &message);
       json_free(&json);
    }
    return (void *) cpystr("graph_challenge: Error Received");
  }

  switch(LOCAL->op){
     case CreateFolder:
     case DeleteFolder:
     case FlagMsg:
     case GraphRename:
	break;

     case CopyMessage:
	graph_parse_copy(stream, json);
	break;

     case ExpungeMsg:
	graph_parse_expunge(stream, json);
	break;

     case GetAttachmentFile:
	json_assign ((void **) &LOCAL->private, json, "contentBytes", JString);
	break;

     case GetAttachmentList:
	graph_parse_attachment_list(stream, json);
	break;

     case GetBodyText:
	graph_parse_body_text(stream, json, &LOCAL->topmsg);
	break;

     case GetEnvelope:
	graph_parse_headers_by_id(stream, json, LOCAL->topmsg);
	break;

     case GetFoldersList:
	LOCAL->folders = graph_parse_folders(stream, json);
	break;

     case GetInitialSync:
	graph_parse_initial_sync(stream, json);
	break;

     case GetMailboxChanges:
	graph_parse_mailbox_changes(stream, json);
	break;

     case GraphUpdate:
     case GetFolderMessages:
	graph_parse_message_list(stream, json, &LOCAL->topmsg);
	break;

     case GraphSendMail:
	if(status != 202)
	   ret = (void *) cpystr("Sending Failed");
	 break;

     case GetMimeMsg:
	if(is_plain){
	   MESSAGECACHE *elt = mail_elt(stream, LOCAL->topmsg);
	   if(!GDPQ) DPP(elt) = graph_message_new();
	   GDPQ->mimetext = cpystr(t);
	   GDPQ->valid |= GPH_MIME;
	}
	else
	   ret = (void *) cpystr("Not a mime part");
	break;

     case MessageSearch:
	graph_parse_message_search(stream, json);
	break;

    default :
	ret = (void *) cpystr("Bad Data");
	break;
  }

  if(json) json_free(&json);
  if(ct) fs_give((void **) &ct);

  return ret;
}

void
graph_parse_body_text(MAILSTREAM *stream, JSON_S *json, unsigned long *msgnop)
{
   JSON_S *j;
   MESSAGECACHE *elt;

   if(!stream || !json) return;

   elt = mail_elt(stream, *msgnop);

   json_assign ((void **) &j, json, "body", JObject);
   if(j){
     if(GDPQ->body.contentType) fs_give((void **) &GDPQ->body.contentType);
     if(GDPQ->body.content) fs_give((void **) &GDPQ->body.content);
     json_assign((void **) &GDPQ->body.contentType, j, "contentType", JString);
     json_assign((void **) &GDPQ->body.content, j, "content", JString);
   }
}

void
graph_parse_expunge(MAILSTREAM *stream, JSON_S *json)
{
   unsigned long id, receivedstatus, expectedstatus;
   JSON_S *jw, *jx, *jy;
   unsigned char *idstring;
   MESSAGECACHE *elt;
   unsigned long move;

		/* note that JSON_S *jx is used in json_value_type */
   if(!json) return;

   move = (unsigned long) mail_parameters(NIL, GET_GRAPHMOVEONEXPUNGE, NIL);
   expectedstatus = move ? HTTP_OK_CREATED : HTTP_OK_NO_CONTENT;

   /* This is a two step process. The first part clears out the GRAPH_MESSAGE
    * structure from any message expunged. Since the responses from the server
    * might not be in order, we cannot expunge them from the folder yet, until
    * we can do it in order.
    */
   if(json->jtype == JArray && !compare_cstring(json->name, "responses")){
	for(jw = (JSON_S *) json->value; jw; jw = jw->next){
	    if(jw->jtype != JObject) continue;
	    jy = (JSON_S *) jw->value;
	    json_assign((void **) &idstring, jy, "id", JString);
	    id = strtoul(idstring, NIL, 10);
	    receivedstatus = json_value_type(jy, "status", JLong);
	    if(receivedstatus == expectedstatus){
	       LOCAL->status = receivedstatus;
	       elt = mail_elt(stream, id);
	       graph_message_free((GRAPH_MESSAGE **) &elt->private.driverp);
	    }
	    else LOCAL->status = 0;
	}
   }
}


void
graph_parse_copy(MAILSTREAM *stream, JSON_S *json)
{
   unsigned long id, status;
   JSON_S *jw, *jx, *jy;
   unsigned char *idstring;

		/* note that JSON_S *jx is used in json_value_type */
   if(!json) return;

   if(json->jtype == JArray && !compare_cstring(json->name, "responses")){
	LOCAL->status = HTTP_OK_CREATED;
	for(jw = (JSON_S *) json->value; jw; jw = jw->next){
	    if(jw->jtype != JObject) continue;
	    jy = (JSON_S *) jw->value;
	    json_assign((void **) &idstring, jy, "id", JString);
	    id = strtoul(idstring, NIL, 10);
	    status = json_value_type(jy, "status", JLong);
	    if(status != HTTP_OK_CREATED)
	       LOCAL->status = 0;
	}
   }
}

void
graph_parse_initial_sync(MAILSTREAM *stream, JSON_S *json)
{
   unsigned long id, status;
   JSON_S *jw, *jx, *jy, *jheaders, *jbody;
   unsigned char *ctype, *u, *idstring;

		/* note that JSON_S *jx is used in json_value_type */
   if(!json) return;

   if(json->jtype == JArray && !compare_cstring(json->name, "responses")){
	for(jw = (JSON_S *) json->value; jw; jw = jw->next){
	    if(jw->jtype != JObject) continue;
	    jy = (JSON_S *) jw->value;
	    json_assign((void **) &idstring, jy, "id", JString);
	    id = strtoul(idstring, NIL, 10);
	    fs_give((void **) &idstring);
	    if(id*GRAPHMAXCOUNTMSGS < stream->nmsgs)
	       LOCAL->topmsg = stream->nmsgs - id*GRAPHMAXCOUNTMSGS;
	    status = json_value_type(jy, "status", JLong);
	    if(status == HTTP_OK){
	       json_assign((void **) &jheaders, jy, "headers", JObject);
	       json_assign((void **) &ctype, jheaders, "Content-Type", JString);
	       if((u = strchr(ctype, '/')) != NULL){
		  u++;
		  for(; isalpha(*u); u++);
		  *u = '\0';
	       }
	       else{
		  fs_give((void **) &ctype);
		  return;
	       }

	       if(ctype){
		  if(!compare_cstring(ctype, "application/json")){
		     json_assign((void **) &jbody, jy, "body", JObject);
		     graph_parse_message_list(stream, jbody, &LOCAL->topmsg);
		  }
		  fs_give((void **) &ctype);
	       }
	    }
	}
   }
}

void
graph_extract_next_link(JSON_S *json, unsigned char **nextlinkp, unsigned char **deltalinkp)
{
   int status;
   JSON_S *jheaders, *jbody, *jx;
   unsigned char *ctype, *u;

   status = json_value_type(json, "status", JLong);
   if(status == HTTP_OK){
      json_assign((void **) &jheaders, json, "headers", JObject);
      json_assign((void **) &ctype, jheaders, "Content-Type", JString);
      if((u = strchr(ctype, '/')) != NULL){
	  u++;
	  for(; isalpha(*u); u++);
	  *u = '\0';
      }
      else{
	 fs_give((void **) &ctype);
	 return;
     }
     if(ctype){
	if(!compare_cstring(ctype, "application/json")){
	   json_assign((void **) &jbody, json, "body", JObject);
	   if(nextlinkp){
	      if(*nextlinkp) fs_give((void **) nextlinkp);
	      json_assign((void **) nextlinkp, jbody, "@odata.nextlink", JString);
	   }
	   if(deltalinkp){
	      if(*deltalinkp) fs_give((void **) deltalinkp);
	      json_assign((void **) deltalinkp, jbody, "@odata.deltalink", JString);
	   }
	}
	fs_give((void **) &ctype);
     }
   }
}

void
graph_parse_message_search(MAILSTREAM *stream, JSON_S *json)
{
   unsigned long i;
   JSON_S *jw, *j;
   MESSAGECACHE *elt;
   unsigned char *msgid;

   if(!json) return;

   if(LOCAL->srchnextlink) fs_give((void **) &LOCAL->srchnextlink);
   json_assign((void **) &LOCAL->srchnextlink, json, "@odata.nextlink", JString);
   json_assign((void **) &j, json, "value", JArray);
   LOCAL->nextlink = LOCAL->srchnextlink;
   i = stream->nmsgs;
   for (; j && j->value; j = j->next){
	jw = (JSON_S *) j->value;
	json_assign((void **) &msgid, jw, "id", JString);
	for(; i > 0; i--){
	    elt = mail_elt(stream, i);
	    if(!strcmp(GDPQ->id, msgid)){
	       elt->searched = T;
	       if(!stream->silent) mm_searched(stream, i);
	       break;
	    }
	}
	fs_give((void **) &msgid);
   }
}

GRAPH_ADDRESS_S *
graph_emailaddress(JSON_S *json, char *s, JObjType jtype)
{
   JSON_S *j;

   json_assign ((void **) &j, json, s, jtype);
   return graph_emailaddress_work(j, jtype);
}

GRAPH_ADDRESS_S *
graph_emailaddress_work(JSON_S *j, JObjType jtype)
{
   GRAPH_ADDRESS_S *rv = NIL;
   JSON_S *j2;

   if(j){
     if(jtype == JArray)
	j2 = j->value ? (JSON_S *) j->value : NIL;	 /* this is the json object in the array */
     else
	j2 = j;
     if(j2){
	rv = (GRAPH_ADDRESS_S *) memset ((void *) fs_get(sizeof (GRAPH_ADDRESS_S)), 0, sizeof(GRAPH_ADDRESS_S));
	json_assign ((void **) &j2, j2, "emailAddress", JObject);
	json_assign ((void **) &rv->name, j2, "name", JString);
	json_assign ((void **) &rv->address, j2, "address", JString);
	rv->next = graph_emailaddress_work(j->next, jtype);
     }
   }
   return rv;
}

GRAPH_PARAMETER *
graph_headers_parse_work(JSON_S *json)
{
   GRAPH_PARAMETER *rv;

   if(json){
	rv = (GRAPH_PARAMETER *) memset ((void *) fs_get(sizeof (GRAPH_PARAMETER)), 0, sizeof(GRAPH_PARAMETER));
	json_assign ((void **) &rv->name,  (JSON_S *) json->value, "name", JString);
	json_assign ((void **) &rv->value, (JSON_S *) json->value, "value", JString);
	rv->next = graph_headers_parse_work(json->next);
   }
   return json ? rv : NIL;
}

GRAPH_PARAMETER *
graph_headers_parse(JSON_S *json)
{
  JSON_S *j = json_by_name_and_type(json, "internetMessageHeaders", JArray);
  return j ? graph_headers_parse_work((JSON_S *)j->value) : NIL;
}

#define MSG_ASSIGN_STRING(X, Y, Z)				\
	do 							\
	{ void *v = NULL;					\
	  json_assign(&v, (Y), (Z), JString);			\
	  if(v != NULL){					\
	     if ((X)) fs_give((void **) &(X));			\
	     (X) = (unsigned char *) v;				\
	  }							\
	 } while(0)


#define MSG_ASSIGN_BOOLEAN(X, Y, Z)				\
	do							\
	{ JSON_S *jx;						\
	  jx = json_body_value((Y), (Z));			\
	  if(jx && jx->jtype == JBoolean && jx->value)		\
	     (X) = json_value_type((Y), (Z), JBoolean);		\
	} while(0)

void
graph_parse_mailbox_changes(MAILSTREAM *stream, JSON_S *json)
{
   unsigned long id, i;
   JSON_S *jw, *jx, *jy, *jbody, *jvalue, *jentry;
   JSON_S **jdata;
   unsigned char *msgid, *idstr;
   MESSAGECACHE *elt;
   GRAPH_MESSAGE *msg, *nmsg = NIL, *nmsg2 = NIL;
   int newmsgs = 0, j;

		/* note that JSON_S *jx is used in json_value_type */
   if(!json) return;

   jdata = fs_get(3*sizeof(JSON_S *));
   if(json->jtype == JArray && !compare_cstring(json->name, "responses")){
      for(jw = (JSON_S *) json->value; jw; jw = jw->next){
	  if(jw->jtype != JObject) continue;
	  jy = (JSON_S *) jw->value;
	  json_assign((void **) &idstr, jy, "id", JString);
	  id = strtoul((char *) idstr, NULL, 10);
	  fs_give((void **) &idstr);
	  if(id >= 0 && id < 3) jdata[id] = jy;
	  else fatal("Graph server sent unrecognized response");
      }
   }

   graph_extract_next_link(jdata[GRAPH_NEWMSGS], &LOCAL->created.nextlink, &LOCAL->created.deltalink);
   graph_extract_next_link(jdata[GRAPH_UPDMSGS], &LOCAL->updated.nextlink, &LOCAL->updated.deltalink);
   graph_extract_next_link(jdata[GRAPH_DELMSGS], &LOCAL->deleted.nextlink, &LOCAL->deleted.deltalink);

   for(j = 0; j < 3; j++){
       for(i = 1; i <= stream->nmsgs; i++){
	   elt = mail_elt(stream, i);
	   if(GDPQ) GDPQ->internal.searched = NIL;
       }
       json_assign((void **) &jbody, jdata[j], "body", JObject);
       json_assign((void **) &jvalue, jbody, "value", JArray);
       for(jentry = jvalue; jentry && jentry->value; jentry = jentry->next){
	   json_assign((void **) &msgid, (JSON_S *) jentry->value, "id", JString);
	   if(msgid){
	      for(i = stream->nmsgs; i > 0; i--){
		  elt = mail_elt(stream, i);
		  if(GDPQ && !GDPQ->internal.searched && !strcmp(GDPQ->id, msgid)) break;
	      }
	      fs_give((void **) &msgid);
	      switch(j){
		case GRAPH_NEWMSGS:
			if(i == 0){	/* collect information, process later */
			   if(!nmsg) nmsg2 = nmsg = graph_message_new();
			   else nmsg = nmsg->next = graph_message_new();
			   graph_update_message(&nmsg, (JSON_S *) jentry->value);
			   nmsg->rfc822_size = graph_estimate_msg_size(nmsg);
			   newmsgs++;
			}
			else{
			   msg = GDPP(elt);
			   graph_update_message(&msg, (JSON_S *) jentry->value);
			   graph_parse_fast(stream, i, i, GPH_ENVELOPE);
			   msg->internal.searched = T;
			}
			break;

		case GRAPH_UPDMSGS:
			if(i == 0){
			   /* we cannot update a message we still do not see */
			}
			else{
			   MSG_ASSIGN_BOOLEAN(elt->seen, (JSON_S *) jentry->value, "isRead");
			   MM_FLAGS(stream, i);
			}
			break;

		case GRAPH_DELMSGS:
			if(i == 0){
			   /* we are deleting a message we never saw */
			}
			else{
			   JSON_S *jx;
			   json_assign((void **) &jx, (JSON_S *) jentry->value, "@removed", JObject);
			   if(jx){
			      graph_message_free(GDPQP);
			      mail_expunged(stream, i);
			   }
			}
			break;

		default: fatal("graph_parse_mailbox_changes: increase in number of processed items");
			break;
	      }
	   }
	   if(newmsgs){
	      MESSAGECACHE *elt;
	      nmsg = nmsg2;
	      mail_exists(stream, stream->nmsgs + newmsgs);
	      mail_recent(stream, newmsgs);
	      for(i = stream->nmsgs; i > 0 && nmsg != NULL; i--){
		  elt = mail_elt(stream, i);
		  elt->private.driverp = (void *) nmsg;
		  elt->recent = T;
		  elt->private.uid = i;
		  graph_parse_fast(stream, i, i, GPH_ENVELOPE);
		  nmsg2 = nmsg;
		  nmsg = nmsg->next;
		  nmsg2->next = NULL;	/* unlink these messages */
	      }
	      newmsgs = 0;
	   }
       }
       jdata[j] = NIL;
   }
   fs_give((void **) &jdata);
}

/*
 * Make sure when you call this function that LOCAL->purpose
 * is set to a correct GPH_* value
 */
void
graph_update_message(GRAPH_MESSAGE **msgp, JSON_S *json)
{
   JSON_S *jx;
   void *v = NULL;

   if(!msgp || !*msgp || !json) return;

   MSG_ASSIGN_STRING((*msgp)->odata.etag, json, "@odata.etag");
   MSG_ASSIGN_STRING((*msgp)->odata.type, json, "@odata.type");
   MSG_ASSIGN_STRING((*msgp)->id, json, "id");
   MSG_ASSIGN_STRING((*msgp)->createdDateTime, json, "createdDateTime");
   MSG_ASSIGN_STRING((*msgp)->lastModifiedDateTime, json, "lastModifiedDateTime");
   MSG_ASSIGN_STRING((*msgp)->changeKey, json, "changeKey");
   MSG_ASSIGN_STRING((*msgp)->receivedDateTime, json, "receivedDateTime");
   MSG_ASSIGN_STRING((*msgp)->sentDateTime, json, "sentDateTime");
   MSG_ASSIGN_STRING((*msgp)->internetMessageId, json, "internetMessageId");
   MSG_ASSIGN_STRING((*msgp)->subject, json, "subject");
   MSG_ASSIGN_STRING((*msgp)->bodyPreview, json, "bodyPreview");
   MSG_ASSIGN_STRING((*msgp)->importance, json, "importance");
   MSG_ASSIGN_STRING((*msgp)->parentFolderId, json, "parentFolderId");
   MSG_ASSIGN_STRING((*msgp)->conversationId, json, "conversationId");
   MSG_ASSIGN_STRING((*msgp)->conversationIndex, json, "conversationIndex");
   MSG_ASSIGN_STRING((*msgp)->isDeliveryReceiptRequested, json, "isDeliveryReceiptRequested");
   MSG_ASSIGN_STRING((*msgp)->weblink, json, "weblink");
   MSG_ASSIGN_STRING((*msgp)->interferenceClassification, json, "interferenceClassification");

   v = (void *) graph_list_assign(json, "categories");
   if(v){
     if((*msgp)->categories) graph_list_free(&(*msgp)->categories);
     (*msgp)->categories = (GRAPH_LIST_S *) v;
   }

   MSG_ASSIGN_BOOLEAN((*msgp)->hasAttachments, json, "hasAttachments");
   MSG_ASSIGN_BOOLEAN((*msgp)->isReadReceiptRequested, json, "isReadReceiptRequested");
   MSG_ASSIGN_BOOLEAN((*msgp)->isRead, json, "isRead");
   MSG_ASSIGN_BOOLEAN((*msgp)->isDraft, json, "isDraft");

   json_assign ((void **) &jx, json, "body", JObject);
   if(jx){
     if((*msgp)->body.contentType) fs_give((void **) &(*msgp)->body.contentType);
     if((*msgp)->body.content) fs_give((void **) &(*msgp)->body.content);
     json_assign((void **) &(*msgp)->body.contentType, jx, "contentType", JString);
     json_assign((void **) &(*msgp)->body.content, jx, "content", JString);
   }

   if((jx = json_by_name_and_type(json, "flag", JObject)) != NULL){
      if((*msgp)->flag.flagStatus) fs_give((void **) &(*msgp)->flag.flagStatus);
      json_assign((void **) &(*msgp)->flag.flagStatus, jx, "flagStatus", JString);
   }

   v = (void *) graph_emailaddress(json, "sender", JObject);
   if(v != NULL){
      if((*msgp)->sender) graph_free_address(&(*msgp)->sender);
      (*msgp)->sender = (GRAPH_ADDRESS_S *) v;
   }

   v = (void *) graph_emailaddress(json, "from", JObject);
   if(v != NULL){
      if((*msgp)->from) graph_free_address(&(*msgp)->from);
      (*msgp)->from = (GRAPH_ADDRESS_S *) v;
   }

   v = (void *) graph_emailaddress(json, "toRecipients", JArray);
   if(v != NULL){
      if((*msgp)->toRecipients) graph_free_address(&(*msgp)->toRecipients);
      (*msgp)->toRecipients = (GRAPH_ADDRESS_S *) v;
   }

   v = (void *) graph_emailaddress(json, "ccRecipients", JArray);
   if(v != NULL){
      if((*msgp)->ccRecipients) graph_free_address(&(*msgp)->ccRecipients);
      (*msgp)->ccRecipients = (GRAPH_ADDRESS_S *) v;
   }

   v = (void *) graph_emailaddress(json, "bccRecipients", JArray);
   if(v != NULL){
      if((*msgp)->bccRecipients) graph_free_address(&(*msgp)->bccRecipients);
      (*msgp)->bccRecipients = (GRAPH_ADDRESS_S *) v;
   }

   v = (void *) graph_emailaddress(json, "replyTo", JArray);
   if(v != NULL){
      if((*msgp)->replyTo) graph_free_address(&(*msgp)->replyTo);
      (*msgp)->replyTo = (GRAPH_ADDRESS_S *) v;
   }
}


void
graph_assign_msglist_messages(MAILSTREAM *stream, JSON_S *j, unsigned long *first)
{
   JSON_S *json;
   MESSAGECACHE *elt;

   if(!j) return;
   j = json_by_name_and_type(j, "value", JArray);	/* this is an array */
   if(!j) return;
   j = (JSON_S *) j->value;	/* this is the first object of the array */

   for(; j && j->value; j = j->next,(*first)--){
      elt = mail_elt(stream, *first);
      if(!GDPQ) DPP(elt) = graph_message_new();
      if(GDPQ->valid & LOCAL->purpose) continue;

      json = (JSON_S *) j->value;
      graph_update_message(GDPQP, json);
      GDPQ->internetMessageHeaders = graph_headers_parse(json);
      GDPQ->rfc822_size = graph_estimate_msg_size(GDPQ);
      if(LOCAL->purpose & GPH_STATUS) graph_parse_flags(stream, elt);
      GDPQ->valid |= LOCAL->purpose;
    }
}

/* estimate rfc822_size */
unsigned long
graph_estimate_msg_size(GRAPH_MESSAGE *msg)
{
   unsigned long rfc822_size = 0L;

   if(msg->internetMessageHeaders && (msg->rfc822_size == 0)){
       GRAPH_PARAMETER *p;
       for(p = msg->internetMessageHeaders; p; p = p->next) rfc822_size += strlen(p->name) + strlen(p->value) + 2 + 2;
       return rfc822_size;
   }

   if(msg->sentDateTime)
      rfc822_size += strlen(msg->sentDateTime) + 6 + 2; /* strlen("Date: ") + "\r\n" */
   if(msg->internetMessageId)
      rfc822_size += strlen(msg->internetMessageId) + 12 + 2; /* strlen("Message-Id: ") + "\r\n" */
   if(msg->subject)
      rfc822_size += strlen(msg->subject) + 9 + 2; /* strlen("Subject: ") + "\r\n" */
   if(msg->body.content)
      rfc822_size += strlen(msg->body.content) + 2; /* "\r\n" */
   if(msg->importance)
      rfc822_size += strlen(msg->importance) + 12 + 2; /* strlen("Importance: ")"\r\n" */
   if(msg->sender)
      rfc822_size += (msg->sender->name ? strlen(msg->sender->name) : 0)
			  + (msg->sender->address ? strlen(msg->sender->address) : 0)
			  +  7 + 2; /* strlen("Sender:")"\r\n" */
   if(msg->from)
      rfc822_size += (msg->from->name ? strlen(msg->from->name) : 0)
			  + (msg->from->address ? strlen(msg->from->address) : 0)
			  +  7 + 2; /* strlen("Sender:") + "\r\n" */

   if(msg->toRecipients){
      GRAPH_ADDRESS_S *addr;
      for(addr = msg->toRecipients; addr; addr = addr->next)
	  rfc822_size += (addr->name ? strlen(addr->name) : 0)
			 + (addr->address ? strlen(addr->address) : 0) + 2;
      rfc822_size +=  4; /* strlen("To: ") */
   }

   if(msg->ccRecipients){
	GRAPH_ADDRESS_S *addr;
	for(addr = msg->ccRecipients; addr; addr = addr->next)
	    rfc822_size += (addr->name ? strlen(addr->name) : 0)
			   + (addr->address ? strlen(addr->address) : 0) + 2;
	rfc822_size +=  4; /* strlen("Cc: ") */
   }

   if(msg->bccRecipients){
      GRAPH_ADDRESS_S *addr;
      for(addr = msg->bccRecipients; addr; addr = addr->next)
	  rfc822_size += (addr->name ? strlen(addr->name) : 0)
			      + (addr->address ? strlen(addr->address) : 0) + 2;
      rfc822_size +=  5; /* strlen("Bcc: ") */
   }

   if(msg->replyTo){
      GRAPH_ADDRESS_S *addr;
      for(addr = msg->replyTo; addr; addr = addr->next)
	  rfc822_size += (addr->name ? strlen(addr->name) : 0)
			 + (addr->address ? strlen(addr->address) : 0) + 2;
      rfc822_size +=  10; /* strlen("reply-to: ") */
    }

    return rfc822_size;
}

void
graph_parse_message_list(MAILSTREAM *stream, JSON_S *json, unsigned long *topmsg)
{
   graph_assign_msglist_messages(stream, json, topmsg);
}

void
graph_free_params(GRAPH_PARAMETER **param)
{
  if(param == NULL || *param == NULL) return;

  if((*param)->name) fs_give((void **) &(*param)->name);
  if((*param)->value) fs_give((void **) &(*param)->value);
  if((*param)->next) graph_free_params(&(*param)->next);
  fs_give((void **) param);
}


void
graph_free_folders(GRAPH_USER_FOLDERS **foldersp)
{
   if(!foldersp || !*foldersp) return;

   if((*foldersp)->id) fs_give((void **) &(*foldersp)->id);
   if((*foldersp)->displayName) fs_give((void **) &(*foldersp)->displayName);
   if((*foldersp)->parentFolderId) fs_give((void **) &(*foldersp)->parentFolderId);
   if((*foldersp)->next) graph_free_folders(&(*foldersp)->next);
   fs_give((void **) foldersp);
}

GRAPH_USER_FOLDERS *
graph_parse_folders(MAILSTREAM *stream, JSON_S *json)
{
  JSON_S *jx;

  if(!stream || !LOCAL) return NIL;

  LOCAL->folderscount = json_value_type(json, "@odata.count", JLong);
  jx = json_body_value(json, "value");
  if(!jx || jx->jtype != JArray)
     return NIL;

  return graph_parse_folders_work(stream, (JSON_S *)jx->value);
}

GRAPH_USER_FOLDERS *
graph_parse_folders_work(MAILSTREAM *stream, JSON_S *json)
{
  JSON_S *jx;
  JSON_S *j;
  GRAPH_USER_FOLDERS *gfolders = NIL;

  if(json == NIL) return NIL;

  j = (JSON_S *) (json->value);

  if(!gfolders){
     gfolders  = fs_get(sizeof(GRAPH_USER_FOLDERS));
     memset((void *) gfolders, 0, sizeof(GRAPH_USER_FOLDERS));

     json_assign ((void **) &gfolders->id, j, "id", JString);
     json_assign ((void **) &gfolders->displayName, j, "displayName", JString);
     json_assign ((void **) &gfolders->parentFolderId, j, "parentFolderId", JString);
     gfolders->childFolderCount = json_value_type(j, "childFolderCount", JLong);
     gfolders->unreadItemCount = json_value_type(j, "unreadItemCount", JLong);
     gfolders->totalItemCount = json_value_type(j, "totalItemCount", JLong);
     gfolders->sizeInBytes = json_value_type(j, "sizeInBytes", JLong);
     gfolders->isHidden = json_value_type(j, "isHidden", JBoolean);
  }
  if(json->next) gfolders->next = graph_parse_folders_work(stream, json->next);

  return gfolders;
}

/* Transform an array to a Graph list */
GRAPH_LIST_S *
graph_list_assign(JSON_S *json, char *s)
{
   JSON_S *j;

   j = json_by_name_and_type(json, s, JArray);
   return j ? graph_list_assign_work((JSON_S *) j->value) : NIL;
}

GRAPH_LIST_S *
graph_list_assign_work(JSON_S *j)
{
   GRAPH_LIST_S *rv = NIL;
   if(j && j->jtype == JObject) j = (JSON_S *) j->value;
   if(j){
	rv = (GRAPH_LIST_S *) memset((void *) fs_get(sizeof(GRAPH_LIST_S)), 0, sizeof(GRAPH_LIST_S));
	if(j->jtype == JString)
	    rv->elt = cpystr((unsigned char *)j->value);
	rv->next = graph_list_assign_work(j->next);
   }
   return rv;
}

GRAPH_ATTACHMENT *
graph_parse_attachment_list(MAILSTREAM *stream, JSON_S *json)
{
  JSON_S *jx;
  GRAPH_MESSAGE *msg = (GRAPH_MESSAGE *) LOCAL->private;

  jx = json_body_value(json, "value");
  if(!jx || jx->jtype != JArray)
    return NIL;

  if(msg) msg->attachments = graph_parse_attachment_list_work((JSON_S *)jx->value);

  return msg ? msg->attachments : NIL;
}

GRAPH_ATTACHMENT *
graph_parse_attachment_list_work (JSON_S *json)
{
  JSON_S *jx;
  JSON_S *j;
  GRAPH_ATTACHMENT *gatt = NIL;

  if(json == NIL) return NIL;

  j = (JSON_S *) (json->value);

  if(!j) return NIL;

  if(!gatt){
     gatt  = fs_get(sizeof(GRAPH_ATTACHMENT));
     memset((void *) gatt, 0, sizeof(GRAPH_ATTACHMENT));

     json_assign ((void **) &gatt->odata.type, j, "@odata.type", JString);
     json_assign ((void **) &gatt->odata.mediaContentType, j, "@odata.mediaContentType", JString);
     json_assign ((void **) &gatt->contentType, j, "contentType", JString);
     json_assign ((void **) &gatt->contentLocation, j, "contentLocation", JString);
     json_assign ((void **) &gatt->contentBytes, j, "contentBytes", JString);
     json_assign ((void **) &gatt->contentId, j, "contentId", JString);
     json_assign ((void **) &gatt->lastModifiedDateTime, j, "lastModifiedDateTime", JString);
     json_assign ((void **) &gatt->id, j, "id", JString);
     json_assign ((void **) &gatt->name, j, "name", JString);
     gatt->isInline = json_value_type(j, "isInline", JBoolean);
     gatt->size = json_value_type(j, "size", JLong);
     gatt->next = graph_parse_attachment_list_work(json->next);
  }

  return gatt;
}

void
free_req_list(REQUEST_S **reqp)
{
    if(reqp == NULL || *reqp == NULL) return;
    if((*reqp)->next) free_req_list(&(*reqp)->next);
    fs_give((void **) reqp);
}

#define MAX_REQ (100)
/* Direct traffic to the correct location based on operation */
long
graph_response (void *s, char *base, char *response, unsigned long size)
{
   static REQUEST_S *req = NIL, *current;
   static int n = 0;
   MAILSTREAM *stream = (MAILSTREAM *) s;

   /* special call to free memory */
   if(s == NIL && base == NIL && response == NIL && size == 0L){
      if(!req) return LONGT;
      req = current->next; current->next = NIL; current = req;
      free_req_list(&current);
      req = NIL;
      return LONGT;
   }

   /* create a circular list of MAX_REQ requests */
   if(!req){
      int i;
      current = fs_get(sizeof(REQUEST_S));
      memset((void *) current, 0, sizeof(REQUEST_S));
      for(req = current, i = 0; i < MAX_REQ - 1; i++, req = req->next){
	  req->next = fs_get(sizeof(REQUEST_S));
	  memset((void *) req->next, 0, sizeof(REQUEST_S));
      }
      req->next = current;
   }

   current->req_time = time(0);
   if(current->req_time == current->next->req_time){
      char tmp[MAILTMPLEN];
      if((n % 5) == 0) n = 1;
      sprintf(tmp, "Too many requests. Sleeping %d seconds", n);
      mm_log (tmp, WARN);
      sleep(n++);
   }
   current = current->next;

   if (stream && LOCAL){
	if (LOCAL->op == HTTPGet
	    || LOCAL->op == HTTPPost
	    || LOCAL->op == HTTPPatch) /* should not happen */
	    return NIL;
	if (LOCAL->op > HTTPPatch)	/* HTTPPatch > HTTPPost > HTTPGet */
	    return graph_response_patch (s, base, response, size);
	else if(LOCAL->op > HTTPPost)	/* Is it HTTPPost? */
	    return graph_response_post (s, base, response, size);
	else				/* Must be HTTPGet */
	    return graph_response_get (s, base, response, size);
   }
   return NIL;
}


long
graph_response_get (void *s, char *base, char *response, unsigned long size)
{
  MAILSTREAM *stream = (MAILSTREAM *) s;
  HTTP_REQUEST_S *http_request = NIL;
  long rv;

  if(response){
     if(LOCAL->access_token) fs_give((void **) &LOCAL->access_token);
     LOCAL->access_token = cpystr(response);
	/* we do not need to do this by hand, but we want to mimic the
	 * _response, _challenge paradigm, so we do not process, nor get
	 * the reply from the http server here, otherwise, a simple
	 * http_get would do the work here.
	 */
     if(LOCAL->netstream){
        unsigned char *auth, *tail;

	if(LOCAL->nextlink){
	   tail = cpystr(LOCAL->nextlink + GRAPHSITELEN);
	   LOCAL->nextlink = NIL;	/* forget the source */
	}
	else {
	   if(LOCAL->resource){
	      tail = fs_get(strlen(LOCAL->http_stream->urltail) + strlen(LOCAL->resource) + 1 + 1);
	      sprintf(tail, "%s/%s", LOCAL->http_stream->urltail, LOCAL->resource);
	   }
	   else
	      tail = cpystr(LOCAL->http_stream->urltail);

	   if(LOCAL->params){
	      char *ntail = http_get_param_url(tail, LOCAL->params);
	      if(tail) fs_give((void **) &tail);
	      tail = ntail;
	      http_param_free(&LOCAL->params);
	   }
	}

	http_request = http_request_get();
	http_request->request = http_request_line("GET", tail, HTTP_1_1_VERSION);
	if (tail) fs_give((void **) &tail);

	http_add_header(&http_request, "Host", LOCAL->http_stream->urlhost);
	if(response){
	   auth = fs_get(size + strlen(base) + 1 + 1);
	   if(auth){
	      sprintf(auth, "%s %s", base, response);
	      http_add_header(&http_request, "Authorization", auth);
	      fs_give((void **) &auth);
	   }
	}
	if(LOCAL->http_header){
	   GRAPH_PARAMETER *header;
	   for(header = LOCAL->http_header; header && header->name && header->value; header = header->next)
	       http_add_header(&http_request, header->name, header->value);
	   graph_free_params(&LOCAL->http_header);
	}
     }
  }
  LOCAL->http_stream->cflags = LOCAL->cflags;
  rv = LOCAL && LOCAL->http_stream
	  ? http_send(LOCAL->http_stream, http_request) : NIL;

  LOCAL->status = LOCAL && LOCAL->http_stream && LOCAL->http_stream->status
		   ? LOCAL->http_stream->status->code : -1;

  http_request_free(&http_request);

  return rv;
}

/* Management of update of message information on the server */
long
graph_response_patch (void *s, char *base, char *response, unsigned long size)
{
  MAILSTREAM *stream = (MAILSTREAM *) s;
  HTTP_REQUEST_S *http_request;
  long rv;

  if(response){
     if(LOCAL->access_token) fs_give((void **) &LOCAL->access_token);
     LOCAL->access_token = cpystr(response);
     if(LOCAL->netstream){
        unsigned char *auth, *tail;

	if(LOCAL->resource){
	   tail = fs_get(strlen(LOCAL->http_stream->urltail) + strlen(LOCAL->resource) + 1 + 1);
	   sprintf(tail, "%s/%s", LOCAL->http_stream->urltail, LOCAL->resource);
	}
	else
	   tail = cpystr(LOCAL->http_stream->urltail);

	if(LOCAL->params){
	   char *ntail = http_get_param_url(tail, LOCAL->params);
	   if(tail) fs_give((void **) &tail);
	   tail = ntail;
	   http_param_free(&LOCAL->params);
	}

	http_request = http_request_get();
	http_request->request = http_request_line("PATCH", tail, HTTP_1_1_VERSION);
	if (tail) fs_give((void **) &tail);

	http_add_header(&http_request, "Host", LOCAL->http_stream->urlhost);
	switch(LOCAL->op){
	    case GraphRename:
	    case GraphUpdate:
		http_add_header(&http_request, "Content-Type", "application/json");
		break;

		default: break;
	}

	auth = fs_get(size + strlen(base) + 1 + 1);
	if(auth){
	   sprintf(auth, "%s %s", base, response);
	   http_add_header(&http_request, "Authorization", auth);
	   fs_give((void **) &auth);
	}

	if(LOCAL->http_header){
	   GRAPH_PARAMETER *header;
	   for(header = LOCAL->http_header; header && header->name && header->value; header = header->next)
	       http_add_header(&http_request, header->name, header->value);
 	   graph_free_params(&LOCAL->http_header);
	}

	if(LOCAL->private)
	   http_add_body(&http_request, (unsigned char *) LOCAL->private);
     }
  }
  LOCAL->http_stream->cflags = LOCAL->cflags;
  rv = response && LOCAL && LOCAL->http_stream
	  ? http_send(LOCAL->http_stream, http_request) : NIL;

  LOCAL->status = LOCAL && LOCAL->http_stream && LOCAL->http_stream->status
		   ? LOCAL->http_stream->status->code : -1;

  http_request_free(&http_request);

  return rv;
}

/* Similar to graph_response_get but different so we can modify it as needed
 * Maybe they will be combined later once the code becomes stable.
 */
long
graph_response_post (void *s, char *base, char *response, unsigned long size)
{
  MAILSTREAM *stream = (MAILSTREAM *) s;
  HTTP_REQUEST_S *http_request;
  long rv;

  if(response){
     if(LOCAL->access_token) fs_give((void **) &LOCAL->access_token);
     LOCAL->access_token = cpystr(response);
     if(LOCAL->netstream){
        unsigned char *auth, *tail;

	if(LOCAL->resource){
	   tail = fs_get(strlen(LOCAL->http_stream->urltail) + strlen(LOCAL->resource) + 1 + 1);
	   sprintf(tail, "%s/%s", LOCAL->urltail ? LOCAL->urltail : LOCAL->http_stream->urltail, LOCAL->resource);
	}
	else
	   tail = cpystr(LOCAL->http_stream->urltail);

	if(LOCAL->params){
	   char *ntail = http_get_param_url(tail, LOCAL->params);
	   if(tail) fs_give((void **) &tail);
	   tail = ntail;
	   http_param_free(&LOCAL->params);
	}

	http_request = http_request_get();
	http_request->request = http_request_line("POST", tail, HTTP_1_1_VERSION);
	if (tail) fs_give((void **) &tail);

	http_add_header(&http_request, "Host", LOCAL->http_stream->urlhost);
	switch(LOCAL->op){
	    case DeleteFolder:
	    case GraphSendMail:
		http_add_header(&http_request, "Content-Type", "text/plain");
		break;

	    case CopyMessage:
	    case CreateFolder:
	    case ExpungeMsg:
	    case FlagMsg:
	    case GetInitialSync:
	    case GetMailboxChanges:
		http_add_header(&http_request, "Content-Type", "application/json");
		break;

		default: break;
	}

	auth = fs_get(size + strlen(base) + 1 + 1);
	if(auth){
	   sprintf(auth, "%s %s", base, response);
	   http_add_header(&http_request, "Authorization", auth);
	   fs_give((void **) &auth);
	}

	if(LOCAL->http_header){
	   GRAPH_PARAMETER *header;
	   for(header = LOCAL->http_header; header && header->name && header->value; header = header->next)
	       http_add_header(&http_request, header->name, header->value);
 	   graph_free_params(&LOCAL->http_header);
	}

	if(LOCAL->private)
	   http_add_body(&http_request, (unsigned char *) LOCAL->private);
     }
  }
  LOCAL->http_stream->cflags = LOCAL->cflags;
  rv = response && LOCAL && LOCAL->http_stream
	  ? http_send(LOCAL->http_stream, http_request) : NIL;

  LOCAL->status = LOCAL && LOCAL->http_stream && LOCAL->http_stream->status
		   ? LOCAL->http_stream->status->code : -1;

  http_request_free(&http_request);

  return rv;
}

void
graph_close (MAILSTREAM *stream,long options)
{
  unsigned long i;
  GRAPH_MESSAGE *msg;

  if (stream && LOCAL) {	/* send "LOGOUT" */
    if (LOCAL->netstream) net_close (LOCAL->netstream);
    LOCAL->netstream = NIL;
    if(LOCAL->http_stream) LOCAL->http_stream->netstream = NIL;
    if(LOCAL->folders) graph_free_folders(&LOCAL->folders);
				/* free up memory */
    if(LOCAL->folder.displayName) fs_give((void **) &LOCAL->folder.displayName);
    if(LOCAL->folder.id) fs_give((void **) &LOCAL->folder.id);
    if(LOCAL->access_token) fs_give((void **) &LOCAL->access_token);
    if(LOCAL->basename) fs_give((void **) &LOCAL->basename);
    if(LOCAL->resource) fs_give((void **) &LOCAL->resource);
    if(LOCAL->nextlink) fs_give((void **) &LOCAL->nextlink);
    if(LOCAL->folders) graph_free_folders(&LOCAL->folders);
    if(LOCAL->http_header) graph_free_params(&LOCAL->http_header);
    if(LOCAL->params) http_param_free(&LOCAL->params);
    if(LOCAL->eparam) http_param_free(&LOCAL->eparam);
    if(LOCAL->urltail) fs_give((void **) &LOCAL->urltail);
    if(LOCAL->user) fs_give((void **) &LOCAL->user);
    if(LOCAL->created.nextlink) fs_give((void **) &LOCAL->created.nextlink);
    if(LOCAL->created.deltalink) fs_give((void **) &LOCAL->created.deltalink);
    if(LOCAL->updated.nextlink) fs_give((void **) &LOCAL->updated.nextlink);
    if(LOCAL->updated.deltalink) fs_give((void **) &LOCAL->updated.deltalink);
    if(LOCAL->deleted.nextlink) fs_give((void **) &LOCAL->deleted.nextlink);
    if(LOCAL->deleted.deltalink) fs_give((void **) &LOCAL->deleted.deltalink);
    if(LOCAL->srchnextlink) fs_give((void **) &LOCAL->srchnextlink);
    graph_response (NIL, NIL, NIL, 0);	/* free list of requests */

    for(i = 1; i <= stream->nmsgs; i++){
 	msg = GDPP(mail_elt(stream, i));
 	graph_message_free(&msg);
    }
    fs_give ((void **) &stream->local);
  }
}

void
graph_close_netstream (MAILSTREAM *stream)
{
  if (stream && LOCAL){
    if (LOCAL->netstream) net_close (LOCAL->netstream);
    LOCAL->netstream = NIL;
    if(LOCAL->http_stream && LOCAL->http_stream->netstream)
	LOCAL->http_stream->netstream = NIL;
    http_close(LOCAL->http_stream);
    LOCAL->http_stream = NIL;
    LOCAL->cflags = HTTP_RESET;
  }
}


void graph_fast (MAILSTREAM *stream,char *sequence,long flags)
{
  unsigned long first, last;
  MESSAGECACHE *elt;

  if(stream && LOCAL && ((flags & FT_UID) ?
                          mail_uid_sequence (stream,sequence) :
                          mail_sequence (stream,sequence))){
     for(last = stream->nmsgs; last > 0; last--){
	 elt = mail_elt(stream, last);
	 if(elt->sequence) break;
     }
     for(first = last - 1; first > 0; first--){
	 elt = mail_elt(stream, first);
	 if(!elt->sequence) break;
     }
     first++;
     graph_fetch_range(stream, &last, first, GPH_ID | GPH_STATUS);
  }
}

long graph_overview (MAILSTREAM *stream,overview_t ofn)
{
  MESSAGECACHE *elt;
  unsigned int count = 0;
  unsigned long i, bottom, start, top;
  OVERVIEW ov;
  int flag;
  long eo = (long) mail_parameters(NIL, GET_GRAPHENVELOPEONLY, NIL);

  if(!stream || !LOCAL) return NIL;

  LOCAL->countmessages = (unsigned long) mail_parameters(NIL, GET_GRAPHCOUNTMESSAGES, NIL);
  top = stream->nmsgs;
  for(start = stream->nmsgs; start > 0; top = start = bottom - 1){
     for(i = top; i > 0; i--)
	if ((elt = mail_elt (stream,i))->sequence
           && !elt->private.msg.env){
           top = i;
           break;
	}

     if (i == 0) break; /* if no matches, leave */

     for(i = top; i > 0; i--){
	if (((elt = mail_elt (stream,i))->sequence) && !elt->private.msg.env)
	   bottom = i;
	else break;
     }

     if(LOCAL->countmessages > top || LOCAL->countmessages + bottom < top + 1) LOCAL->countmessages = top - bottom + 1;
     if(LOCAL->countmessages > GRAPHMAXCOUNTMSGS) LOCAL->countmessages = GRAPHMAXCOUNTMSGS;
     LOCAL->topmsg = top;

     flag = (LOCAL->loser ? 0 : (eo ? GPH_ENVELOPE : GPH_ALL_HEADERS)) | GPH_STATUS;

     if(graph_fetch_range(stream, &top, bottom, flag)){
         ENVELOPE *env;

         ov.optional.lines = 0;        /* now overview each message */   
         ov.optional.xref = NIL;
         if (ofn) for (i = top; i >= bottom ; i--)
	     if (((elt = mail_elt (stream,i))->sequence) && 
                 (env = mail_fetch_structure (stream,i,NIL,NIL))) {
	        ov.subject = env->subject;
	        ov.from = env->from;
	        ov.date = env->date;
	        ov.message_id = env->message_id;
	        ov.references = env->references;
	        ov.optional.octets = elt->rfc822_size;
	        (*ofn) (stream,mail_uid (stream,i),&ov,i);
	     }
      }
  }
  return LONGT;
}

long
graph_get_message_header_text(MAILSTREAM *stream, unsigned long msgno, int flag)
{
  MESSAGECACHE *elt = mail_elt(stream, msgno);
  GRAPH_MESSAGE *msg = GDPP(elt);

  if(stream && LOCAL && msg && msg->id){
     LOCAL->op = GetEnvelope,		/* set up operation */
     LOCAL->purpose = flag;
     LOCAL->topmsg = msgno;
     if(LOCAL->resource) fs_give((void **) &LOCAL->resource);
		/* set up link to resource */
     LOCAL->resource = fs_get(11 + 8 + strlen(LOCAL->folder.id) + strlen(msg->id) + 3 + 1);
     sprintf(LOCAL->resource, "mailFolders/%s/messages/%s", LOCAL->folder.id,msg->id);
		/* set up extra parameters */
     LOCAL->params = fs_get(2*sizeof(HTTP_PARAM_S));
     memset((void *) LOCAL->params, 0, 2*sizeof(HTTP_PARAM_S));
     LOCAL->params[0].name = cpystr("$select");
     switch(flag){
	case GPH_ENVELOPE:
		LOCAL->params[0].value = cpystr(GRAPHENVELOPE);
		break;

	case GPH_ALL_HEADERS:
		LOCAL->params[0].value = cpystr("internetMessageHeaders");
		break;

	default: fatal("graph_get_message_header_text got a flag it could not understand");
     }
     graph_send_command(stream);
		/* free resource. Always check that LOCAL is valid after a send_command */
     if(stream && LOCAL && LOCAL->resource) fs_give((void **) &LOCAL->resource);
  }
  return (stream && LOCAL) ? LONGT : NIL;
}


void 
graph_get_message_body_text(MAILSTREAM *stream, unsigned long msgno)
{
  MESSAGECACHE *elt = mail_elt(stream, msgno);
  GRAPH_MESSAGE *msg = GDPP(elt);

  if(stream && LOCAL && msg && msg->id){
		/* set up operation */
     LOCAL->op = GetBodyText;
     LOCAL->topmsg = msgno;
     LOCAL->purpose = GPH_BODY;
     if(LOCAL->resource) fs_give((void **) &LOCAL->resource);
		/* set up link to resource */
     LOCAL->resource = fs_get(11 + 8 + strlen(LOCAL->folder.id) + strlen(msg->id) + 3 + 1);
     sprintf(LOCAL->resource, "mailFolders/%s/messages/%s", LOCAL->folder.id,msg->id);
		/* set up extra parameters */
     LOCAL->params = fs_get(2*sizeof(HTTP_PARAM_S));
     memset((void *) LOCAL->params, 0, 2*sizeof(HTTP_PARAM_S));
     LOCAL->params[0].name = cpystr("$select");
     LOCAL->params[0].value = cpystr("body");
     LOCAL->http_header = graph_text_preference();
		/* now send the command */
     graph_send_command(stream);
		/* free resource. Always check that LOCAL is valid after a send_command */
     if(stream && LOCAL && LOCAL->resource) fs_give((void **) &LOCAL->resource);
  }
}


/* example of how to download information from the graph server */
long
graph_get_attachment_list(MAILSTREAM *stream, GRAPH_MESSAGE *msg)
{
  if(stream && LOCAL && msg && msg->id){
		/* set up operation */
     LOCAL->op = GetAttachmentList;
     LOCAL->private = (void *) msg;
     if(LOCAL->resource) fs_give((void **) &LOCAL->resource);
		/* set up link to resource */
     LOCAL->resource = fs_get(strlen("messages") + strlen(msg->id) + strlen("attachments") + 2 + 1);
     sprintf(LOCAL->resource, "messages/%s/attachments", msg->id);
		/* set up extra parameters */
     LOCAL->params = fs_get(2*sizeof(HTTP_PARAM_S));
     memset((void *) LOCAL->params, 0, 2*sizeof(HTTP_PARAM_S));
     LOCAL->params[0].name = cpystr("$select");
     LOCAL->params[0].value = cpystr(GRAPHATTACHMENT);
		/* now send the command */
     graph_send_command(stream);
     if(stream && LOCAL) LOCAL->private = NIL;
		/* free resource. Always check that LOCAL is valid after a send_command */
     if(stream && LOCAL && LOCAL->resource) fs_give((void **) &LOCAL->resource);
  }
  return stream && LOCAL && LOCAL->status == HTTP_OK ? LONGT : NIL;
}

ENVELOPE *
graph_structure (MAILSTREAM *stream, unsigned long msgno, BODY **body, long flags)
{
  char *s;
  unsigned long i;
  MESSAGECACHE *elt;
  BODY **b;
  ENVELOPE **env;
  GRAPH_MESSAGE *msg;

  if (flags & FT_UID){           /* see if can find msgno from UID */
    for (i = 1; i <= stream->nmsgs; i++)
      if ((elt = mail_elt (stream,i))->private.uid == msgno) {
        msgno = i;              /* found msgno, use it from now on */
        flags &= ~FT_UID;       /* no longer a UID fetch */
      }

   if(!graph_get_message_header_text(stream, i, GPH_ENVELOPE))
     return NIL;

    for (i = 1; i <= stream->nmsgs; i++)
      if ((elt = mail_elt (stream,i))->private.uid == msgno) {
        if (body) *body = elt->private.msg.body;
        return elt->private.msg.env;
      }
    if (body) *body = NIL;      /* can't find the UID */
    return NIL;
  }
  elt = mail_elt (stream,msgno);/* get cache pointer */
  if (stream->scache) {         /* short caching? */
    env = &stream->env;         /* use temporaries on the stream */
    b = &stream->body;
    if (msgno != stream->msgno){/* flush old poop if a different message */
      mail_free_envelope (env);
      mail_free_body (b);
      stream->msgno = msgno;    /* this is now the current short cache msg */
    }
  }
  else {                        /* normal cache */  
    env = &elt->private.msg.env;/* get envelope and body pointers */
    b = &elt->private.msg.body;
  }

  msg = graph_fetch_msg(stream, msgno);
  if(!msg) return NIL;

  if(!*env) graph_parse_envelope (stream, env, msg);
  if(msg && stream && LOCAL && !LOCAL->loser && (msg->valid & GPH_RFC822_BODY)){
     mail_free_body(b);
     msg->valid &= ~GPH_RFC822_BODY;
     b = &elt->private.msg.body;
  }

  if(stream && LOCAL && LOCAL->loser && !*env && !(msg->valid & GPH_MIME)){
     LOCAL->op = GetMimeMsg;
     LOCAL->topmsg = msgno;	/* for graph_challenge */
     if (LOCAL->resource) fs_give((void **) &LOCAL->resource);
			/* 11 = strlen("mailFolders"), 8 = strlen("messages") */
     LOCAL->resource = fs_get(11 + 8 + 8 + strlen(LOCAL->folder.id) + strlen(msg->id) + 4 + 1);
     sprintf(LOCAL->resource, "mailFolders/%s/messages/%s/%%24value", LOCAL->folder.id, msg->id);
     graph_send_command(stream);
     if(msg->mimetext){
        char     *bstart;
	STRING    s;

	elt->rfc822_size = strlen(msg->mimetext);
	bstart = strstr(msg->mimetext, "\r\n\r\n");
	if(bstart){
	   bstart += 4;
	   INIT(&s, mail_string, bstart, strlen(bstart));
	   rfc822_parse_msg_full(env, &elt->private.msg.body, msg->mimetext, bstart - (char *) msg->mimetext - 2, &s, BADHOST, 0, 0);
	   return *env;
	}
     }
  }

  if(body){
	int mixed = 0, inln = 0;

	if(!*b && msg->odata.type
	   && !compare_cstring(msg->odata.type, "#microsoft.graph.eventMessageRequest")
	   && !(msg->valid & GPH_MIME)){
	   if(stream && LOCAL){
	      LOCAL->op = GetMimeMsg;
	      LOCAL->topmsg = msgno;	/* for graph_challenge */
	      if (LOCAL->resource) fs_give((void **) &LOCAL->resource);
	      LOCAL->resource = fs_get(11 + 8 + 8 + strlen(LOCAL->folder.id) + strlen(msg->id) + 4 + 1);
	      sprintf(LOCAL->resource, "mailFolders/%s/messages/%s/%%24value", LOCAL->folder.id, msg->id);
	      graph_send_command(stream);
	   }

	   if(msg && msg->mimetext){
	      char     *bstart;
	      STRING    s;

	      elt->private.msg.full.text.data = cpystr(msg->mimetext);
	      elt->private.msg.full.text.size = strlen(msg->mimetext);
	      elt->rfc822_size = elt->private.msg.full.text.size;
	      bstart = strstr(msg->mimetext, "\r\n\r\n");
	      if(bstart){
		 bstart += 4;
		 INIT(&s, mail_string, bstart, strlen(bstart));
		 mail_free_envelope (&elt->private.msg.env);
		 mail_free_body (&elt->private.msg.body);

		 elt->private.msg.header.text.size = bstart - (char *) msg->mimetext;
		 elt->private.msg.header.text.data = fs_get(elt->private.msg.header.text.size + 1);
		 strncpy(elt->private.msg.header.text.data, msg->mimetext, elt->private.msg.header.text.size);
		 elt->private.msg.header.text.data[elt->private.msg.header.text.size] = '\0';

		 elt->private.msg.text.offset = elt->private.msg.header.text.size;
		 elt->private.msg.text.text.data = cpystr(msg->mimetext + elt->private.msg.text.offset);
		 elt->private.msg.text.text.size = strlen(elt->private.msg.text.text.data);

		 rfc822_parse_msg_full(&elt->private.msg.env, &elt->private.msg.body, msg->mimetext, elt->private.msg.header.text.size, &s, BADHOST, 0, 0);
		 *body = elt->private.msg.body;
		 return elt->private.msg.env;
	      }
	   }
	   else return NIL;
	}

	if(!(msg->valid & GPH_ATTACHMENTS)
	     && graph_get_attachment_list(stream, msg)){
	   msg->gotAttachmentList = msg->attachments ? 1 : 0;
	   msg->valid |= GPH_ATTACHMENTS;
	}
	if(msg->attachments){
	  GRAPH_ATTACHMENT *gatt;
	  for(gatt = msg->attachments; gatt; gatt = gatt->next)
	     if(gatt->isInline) inln++; else mixed++;
	}
	if(!*b){
	  BODY *tb; 		   /* text body */

	  if(stream && LOCAL) msg = graph_fetch_msg (stream, msgno);
	  else return NIL;
	  if(stream && LOCAL && msg && !msg->body.content && msg->id){
	     graph_get_message_body_text(stream, msgno);
	     msg->valid |= GPH_BODY;
	  }
				/* set up the body part of the message first */
	  tb = NIL;
	  if(mixed){
	    tb = mail_initbody (mail_newbody ());
	    tb->type = TYPEMULTIPART;
	    tb->subtype = cpystr("MIXED");
	    tb->parameter = mail_newbody_parameter ();
	    tb->parameter->attribute = cpystr("boundary");
	    tb->parameter->value = cpystr("91323:234234");
	    tb->contents.text.data = NIL;
	    tb->contents.text.size = 0;
	    tb->size.bytes = 0;
	    tb->size.lines = 0;
	  }
	  if(inln){
	     if(tb){
		tb->nested.part = mail_newbody_part();
		tb->nested.part->body =  *mail_initbody (mail_newbody ());
		tb->nested.part->body.type = TYPEMULTIPART;
		tb->nested.part->body.subtype = cpystr("RELATED");
		tb->nested.part->body.parameter = mail_newbody_parameter ();
		tb->nested.part->body.parameter->attribute = cpystr("boundary");
		tb->nested.part->body.parameter->value = cpystr("91323:234234");
		tb->nested.part->body.contents.text.data = NIL;
		tb->nested.part->body.contents.text.size = 0;
		tb->nested.part->body.size.bytes = 0;
		tb->nested.part->body.size.lines = 0;
	     }
	     else{
		tb = mail_initbody (mail_newbody ());
		tb->type = TYPEMULTIPART;
		tb->subtype = cpystr("RELATED");
		tb->parameter = mail_newbody_parameter ();
		tb->parameter->attribute = cpystr("boundary");
		tb->parameter->value = cpystr("91323:234235");
		tb->contents.text.data = NIL;
		tb->contents.text.size = 0;
		tb->size.bytes = 0;
		tb->size.lines = 0;
	     }
	  }
	  if(tb){
	     if(tb->nested.part){
		tb->nested.part->body.nested.part = mail_newbody_part();
		tb->nested.part->body.nested.part->body = *mail_initbody(mail_newbody());
		tb->nested.part->body.nested.part->body.type = TYPETEXT;
		if(!compare_cstring(msg->body.contentType, "text"))
		   tb->nested.part->body.nested.part->body.subtype = cpystr("PLAIN");
		else if(!compare_cstring(msg->body.contentType, "HTML"))
		   tb->nested.part->body.nested.part->body.subtype = cpystr("HTML");
		else
		   tb->nested.part->body.nested.part->body.subtype = cpystr(msg->body.contentType);
		tb->nested.part->body.nested.part->body.parameter = mail_newbody_parameter();
		tb->nested.part->body.nested.part->body.parameter->attribute = cpystr("CHARSET");
		tb->nested.part->body.nested.part->body.parameter->value = cpystr("UTF-8");
		s = mail_fetch_text(stream, msgno, NIL, &i, flags);
		tb->nested.part->body.nested.part->body.size.bytes = i;
		while (i--) if(*s++ == '\n') tb->nested.part->body.nested.part->body.size.lines++;
	     }
	     else{
		tb->nested.part = mail_newbody_part();
		tb->nested.part->body = *mail_initbody(mail_newbody());
		tb->nested.part->body.type = TYPETEXT;
		if(!compare_cstring(msg->body.contentType, "text"))
		   tb->nested.part->body.subtype = cpystr("PLAIN");
		else if(!compare_cstring(msg->body.contentType, "HTML"))
		   tb->nested.part->body.subtype = cpystr("HTML");
		else
		   tb->nested.part->body.subtype = cpystr(msg->body.contentType);
		tb->nested.part->body.parameter = mail_newbody_parameter();
		tb->nested.part->body.parameter->attribute = cpystr("CHARSET");
		tb->nested.part->body.parameter->value = cpystr("UTF-8");
		s = mail_fetch_text(stream, msgno, NIL, &i, flags);
		tb->nested.part->body.size.bytes = i;
		while (i--) if(*s++ == '\n') tb->nested.part->body.size.lines++;
	     }
	  }
	  else{
	     tb = mail_initbody(mail_newbody());
	     tb->type = TYPETEXT;
	     if(!compare_cstring(msg->body.contentType, "text"))
		tb->subtype = cpystr("PLAIN");
	     else if(!compare_cstring(msg->body.contentType, "HTML"))
		tb->subtype = cpystr("HTML");
	     else
		tb->subtype = cpystr(msg->body.contentType);
	     tb->parameter = mail_newbody_parameter();
	     tb->parameter->attribute = cpystr("CHARSET");
	     tb->parameter->value = cpystr("UTF-8");
	     s = mail_fetch_text(stream, msgno, NIL, &i, flags);
	     tb->size.bytes = i;
	     while (i--) if(*s++ == '\n') tb->size.lines++;
	  }
	  if(mixed || inln){
	    PART *part = NIL, *pt;
	    GRAPH_ATTACHMENT *matt;
	    int k;

	    if(mixed){
	       pt = part = tb->nested.part;
	       for(matt = msg->attachments; matt;
			matt = matt->next){
		  if(matt->isInline) continue;
		  part = part->next = mail_newbody_part();
		  part->body = *mail_newbody();
		  if(matt->odata.mediaContentType){
		     char *s;
		     s = strchr(matt->odata.mediaContentType, '/');
		     if(s){
		        *s = '\0';
			for(k = 0; (k <= TYPEMAX) && body_types[k]
				&& compare_cstring(matt->odata.mediaContentType, body_types[k]); k++);
			if (k <= TYPEMAX) {	/* only if found a slot */
			   part->body.type = k;	/* set body type */
			   if(!body_types[k]) {	/* assign empty slot */
			      body_types[k] = matt->contentType;
			      matt->odata.mediaContentType = NIL;    /* don't free this string */
			   }
			}
			part->body.subtype = ucase(cpystr(++s));
		     }
		  }
		  else stream->unhealthy = T;
		  if(matt->id){
		     part->body.id = fs_get(strlen(matt->id) + 2 + 1 + 8 + 1);
		     sprintf(part->body.id, "<%s@graph.id>", matt->id);
		  }
		  if(matt->name){
		     part->body.parameter = mail_newbody_parameter ();
		     part->body.parameter->attribute = cpystr("name");
		     part->body.parameter->value = cpystr(matt->name);
		     part->body.description = cpystr(matt->name);
		  }
		  for(k = 0;
		    (k <= ENCMAX) && body_encodings[k] && strcmp("BASE64", body_encodings[k]);
		    k++);
		  part->body.encoding = k;
		  part->body.size.bytes = matt->size;
		  part->body.size.lines = 1;
		  part->body.disposition.type = cpystr("ATTACHMENT");
		  part->body.disposition.parameter = mail_newbody_parameter ();
		  part->body.disposition.parameter->attribute = cpystr("FILENAME");
		  part->body.disposition.parameter->value = cpystr(matt->name);
	      }
	      tb->nested.part = pt;
	    }

	    if(inln){
	       pt = part = mixed ? tb->nested.part->body.nested.part : tb->nested.part;
	       for(matt = msg->attachments; matt;
			matt = matt->next){
		  if(!matt->isInline) continue;
		  part = part->next = mail_newbody_part();
		  part->body = *mail_newbody();
		  if(matt->odata.mediaContentType){
		     char *s;
		     int k;
		     s = strchr(matt->odata.mediaContentType, '/');
		     if(s){
		        *s = '\0';
			for(k = 0; (k <= TYPEMAX) && body_types[k]
				&& compare_cstring(matt->odata.mediaContentType, body_types[k]); k++);
			if (k <= TYPEMAX) {				/* only if found a slot */
			   part->body.type = k;				/* set body type */
			   if(!body_types[k]) {				/* assign empty slot */
			      body_types[k] = matt->contentType;
			      matt->odata.mediaContentType = NIL;	/* don't free this string */
			   }
			}
			part->body.subtype = ucase(cpystr(++s));
		     }
		  }
		  else stream->unhealthy = T;
		  if(matt->id){
		     part->body.id = fs_get(strlen(matt->id) + 2 + 1 + 8 + 1);
		     sprintf(part->body.id, "<%s@graph.id>", matt->id);
		  }
		  if(matt->name){
		     part->body.parameter = mail_newbody_parameter ();
		     part->body.parameter->attribute = cpystr("name");
		     part->body.parameter->value = cpystr(matt->name);
		     part->body.description = cpystr(matt->name);
		  }
		  for(k = 0;
		    (k <= ENCMAX) && body_encodings[k] && strcmp("BASE64", body_encodings[k]);
		    k++);
		  part->body.encoding = k;
		  part->body.size.bytes = matt->size;
		  part->body.size.lines = 1;
		  part->body.disposition.type = cpystr("INLINE");
		  part->body.disposition.parameter = mail_newbody_parameter ();
		  part->body.disposition.parameter->attribute = cpystr("FILENAME");
		  part->body.disposition.parameter->value = cpystr(matt->name);
	      }
	      if(mixed)
		 tb->nested.part->body.nested.part  = pt;
	      else
		 tb->nested.part = pt;

	    }
	  }
	  *b = tb;
	}
	*body = *b;
  }

  return *env;
}

/* status: 1 means message must be marked seen. 0 means it must be
 * marked unseen
 */
int
graph_mark_msg_seen(MAILSTREAM *stream, unsigned long msgno, int status)
{
     MESSAGECACHE *elt;
     GRAPH_MESSAGE *msg;
     GRAPH_USER_FOLDERS *gfolder = NIL;
     int rv = 0;

     elt = mail_elt(stream, msgno);
     if(((msg = GDPP(elt)) == NULL) || (msg->id == NULL)){
	LOCAL->purpose = 0;
	graph_fetch_range(stream, &msgno, msgno, GPH_ID);
	msg = GDPP(elt);
     }

     if(stream && LOCAL && LOCAL->folder.id
	&& (elt = mail_elt(stream, msgno)) && ((msg = GDPP(elt)) != NULL)){
	LOCAL->topmsg = msgno;
	LOCAL->op = GraphUpdate;
	if(status)
	   LOCAL->private = (void *) cpystr("{\"isRead\":true}");
	else
	   LOCAL->private = (void *) cpystr("{\"isRead\":false}");
	if(LOCAL->resource) fs_give((void **) &LOCAL->resource);
		/* 11 = strlen("mailFolders"), 8 = strlen("messages") */
	LOCAL->resource = fs_get(11 + 8 + strlen(LOCAL->folder.id) + strlen(msg->id) + 3 + 1);
	sprintf(LOCAL->resource, "mailFolders/%s/messages/%s", LOCAL->folder.id, msg->id);
	graph_send_command (stream);
	if(stream && LOCAL){
	   rv = 1;
	   if(LOCAL->private) fs_give((void **) &LOCAL->private);
	}
     }
     return rv;
}


long
graph_msgdata (MAILSTREAM *stream, unsigned long msgno, char *section,
		unsigned long first, unsigned long last, STRINGLIST *lines,
		long flags)
{
  MESSAGECACHE *elt = mail_elt(stream, msgno);
  GRAPH_MESSAGE *msg;

  if(stream && LOCAL && !LOCAL->loser){
     if(!compare_cstring(section, "HEADER") && !first && !last){
	long eo = (long) mail_parameters(NIL, GET_GRAPHENVELOPEONLY, NIL);
	msg = GDPP(elt);
	if(!(msg->valid & GPH_ALL_HEADERS) && eo && !lines){
	   graph_fetch_range (stream, &msgno, msgno, GPH_ALL_HEADERS);
	   if(msg && msg->internetMessageHeaders){
	      STRING s;
	      GRAPH_PARAMETER *p;
	      unsigned char *u;

	      for(p = msg->internetMessageHeaders; p; p = p->next){
		  buffer_add(&elt->private.msg.header.text.data, p->name);
		  buffer_add(&elt->private.msg.header.text.data, ": ");
		  u = graph_filter_string(p->value, NULL);
		  buffer_add(&elt->private.msg.header.text.data, u);
		  fs_give((void **) &u);
		  buffer_add(&elt->private.msg.header.text.data, "\r\n");
	      }
	      elt->private.msg.header.text.size = strlen(elt->private.msg.header.text.data);
	      mail_free_envelope(&elt->private.msg.env);
	      rfc822_parse_msg_full(&elt->private.msg.env, NULL, elt->private.msg.header.text.data, elt->private.msg.header.text.size, &s, BADHOST, 0, 0);
	   }
	   msg->valid |= GPH_ALL_HEADERS;
	}
     }
     else if(!compare_cstring(section, "TEXT") && !first && !last){
	SIZEDTEXT text;
	msg = graph_fetch_msg (stream, msgno);
	if(msg->odata.type
	   && !compare_cstring(msg->odata.type, "#microsoft.graph.eventMessageRequest")
	   && !(msg->valid & GPH_MIME)){
	   if(stream && LOCAL){
	      LOCAL->op = GetMimeMsg;
	      LOCAL->topmsg = msgno;	/* for graph_challenge */
	      if (LOCAL->resource) fs_give((void **) &LOCAL->resource);
	      LOCAL->resource = fs_get(11 + 8 + 8 + strlen(LOCAL->folder.id) + strlen(msg->id) + 4 + 1);
	      sprintf(LOCAL->resource, "mailFolders/%s/messages/%s/%%24value", LOCAL->folder.id, msg->id);
	      graph_send_command(stream);
	   }

	   if(msg && msg->mimetext){
	      char     *bstart;
	      STRING    s;

	      elt->private.msg.full.text.data = cpystr(msg->mimetext);
	      elt->private.msg.full.text.size = strlen(msg->mimetext);
	      elt->rfc822_size = elt->private.msg.full.text.size;
	      bstart = strstr(msg->mimetext, "\r\n\r\n");
	      if(bstart){
		 bstart += 4;
		 INIT(&s, mail_string, bstart, strlen(bstart));
		 mail_free_envelope (&elt->private.msg.env);
		 mail_free_body (&elt->private.msg.body);
		 rfc822_parse_msg_full(&elt->private.msg.env, &elt->private.msg.body, msg->mimetext, bstart - (char *) msg->mimetext, &s, BADHOST, 0, 0);

		 elt->private.msg.header.text.size = bstart - (char *) msg->mimetext;
		 elt->private.msg.header.text.data = fs_get(elt->private.msg.header.text.size + 1);
		 strncpy(elt->private.msg.header.text.data, msg->mimetext, elt->private.msg.header.text.size);
		 elt->private.msg.header.text.data[elt->private.msg.header.text.size] = '\0';
		 elt->private.msg.text.offset = elt->private.msg.header.text.size;

		 elt->private.msg.text.offset = elt->private.msg.header.text.size;
		 elt->private.msg.text.text.size = elt->rfc822_size - elt->private.msg.text.offset;
		 elt->private.msg.text.text.data = cpystr(msg->mimetext + elt->private.msg.text.offset);
		 return LONGT;
	      }
	   }
	   else return NIL;
	}
	if(!msg->body.content && msg->id){
	   graph_get_message_body_text(stream, msgno);
	   msg->valid |= GPH_BODY;
	}
	if(msg->body.content){
	   text.data = graph_filter_string(msg->body.content, &text.size);
	   elt->private.msg.text.text.data = cpystr(text.data);
	   elt->private.msg.text.text.size = text.size;
	}
	if(!elt->seen){
	   graph_mark_msg_seen(stream, msgno, 1);
	   elt->seen = T;
	}
	MM_FLAGS(stream, msgno);
     }
     else if (!strcmp (section,"1") || !strcmp (section,"1.1")) {
       if (elt->private.msg.text.text.data) {
	  BODY *b = mail_body (stream, msgno, section);
	  if(b->contents.text.data)
	    fs_give((void **)&b->contents.text.data);
	  if(b->contents.text.size){
	     unsigned char *s;
	     s = elt->private.msg.full.text.data 
		 + elt->private.msg.header.text.size + b->contents.offset;
	     b->contents.text.data = fs_get(b->contents.text.size + 1);
	     strncpy(b->contents.text.data, s, b->contents.text.size);
	     b->contents.text.data[b->contents.text.size] = '\0';
	     return LONGT;
	  }
	  b->contents.text.data = cpystr(elt->private.msg.text.text.data);
	  b->contents.text.size = elt->private.msg.text.text.size;
       }
       return LONGT;
     }
     else if(isdigit(*section)){	/* get the attachment */
	BODY *b;
	GRAPH_MESSAGE *msg = graph_fetch_msg(stream, msgno);
	char *att_id;
	b = mail_body (stream, msgno, section);
	if(b->contents.text.data)
	  fs_give((void **)&b->contents.text.data);
	if(b->contents.text.size){
	   unsigned char *s;
	   s = elt->private.msg.full.text.data 
		+ elt->private.msg.header.text.size + b->contents.offset;
	   b->contents.text.data = fs_get(b->contents.text.size + 1);
	   strncpy(b->contents.text.data, s, b->contents.text.size);
	   b->contents.text.data[b->contents.text.size] = '\0';
	   return LONGT;
	}
	att_id = strrchr(b->id, '@');
	*att_id = '\0';
	LOCAL->op = GetAttachmentFile;
	if(LOCAL->resource) fs_give((void **) &LOCAL->resource);
	LOCAL->resource = fs_get(strlen("messages") + strlen(msg->id) + strlen("attachments") + strlen(b->id+1) + 3 + 1);
	sprintf(LOCAL->resource, "messages/%s/attachments/%s", msg->id, b->id+1);
	*att_id = '@';
	graph_send_command(stream);
	if(LOCAL->private){
	  b->contents.text.data = (char *) LOCAL->private;
	  b->contents.text.size = strlen((char *) LOCAL->private);
	}
	LOCAL->private = NIL;
     }
     else return NIL;
  }
  else{
     msg = graph_fetch_msg (stream, msgno);
     if(msg && !(msg->valid & GPH_MIME)){
	if(stream && LOCAL){
	  LOCAL->op = GetMimeMsg;
	  LOCAL->topmsg = msgno;	/* for graph_challenge */
	  if (LOCAL->resource) fs_give((void **) &LOCAL->resource);
			/* 11 = strlen("mailFolders"), 8 = strlen("messages") */
	  LOCAL->resource = fs_get(11 + 8 + 8 + strlen(LOCAL->folder.id) + strlen(msg->id) + 4 + 1);
	  sprintf(LOCAL->resource, "mailFolders/%s/messages/%s/%%24value", LOCAL->folder.id, msg->id);
	  graph_send_command(stream);
	}
     }
  }
  return LONGT;
}

#define ADD_GRAPH_SEARCH(X, Y, Z, W)					\
  do {									\
    if((Y)){								\
       STRINGLIST *sl;							\
       int addquotes = 0;						\
       unsigned char *q;						\
       buffer_add(&(X), "\"");						\
       if((W)) buffer_add(&(X), "-");					\
       for (sl = (Y); sl; sl = sl->next){				\
	  addquotes = 0;						\
	  buffer_add(&(X), (Z));					\
	  if(strchr((Y)->text.data, ' ')) addquotes++;			\
	  if (addquotes) buffer_add(&(X), "\\\"");			\
	  buffer_add(&(X), q = graph_quoted((Y)->text.data));		\
	  if(q) fs_give((void **) &q);					\
	  if(addquotes) buffer_add(&(X), "\\\"");				\
	  if(sl->next) buffer_add(&(X), " ");				\
       }								\
       buffer_add(&(X), "\" ");						\
    }									\
  } while(0)

long graph_search (MAILSTREAM *stream,char *charset,SEARCHPGM *pgm,long flags)
{
  unsigned char  *s = NIL;
  SEARCHPGM *spgm;
  int isand, isor, isnot;

  if(!stream || !LOCAL) return NIL;

  if (flags & SE_NOSERVER){
	if((flags & SE_NOLOCAL) ||
        !mail_search_default (stream,charset,pgm,flags | SE_NOSERVER))
      return NIL;
  }

  if(stream && LOCAL && !LOCAL->sync){
    int sync;
    unsigned long i;
    MESSAGECACHE *elt;

    LOCAL->purpose = 0;
    for(i = 1; i <= stream->nmsgs; i++){
	elt = mail_elt(stream, i);
	if(!GDPP(elt)) break;
    }
    if(!GDPP(elt)){
        sync = graph_initial_sync(stream);
	if(stream && LOCAL) LOCAL->sync = sync;
	else return NIL;
    }
  }

  isand = isor = isnot = 0;
  if(pgm->not){
     isnot++;
     spgm = pgm->not->pgm;
  }
  else{
      isand++;
      spgm = pgm;
  }
  ADD_GRAPH_SEARCH(s, spgm->from, "from:", isnot);
  ADD_GRAPH_SEARCH(s, spgm->body, "body:", isnot);
  ADD_GRAPH_SEARCH(s, spgm->subject, "subject:", isnot);
  ADD_GRAPH_SEARCH(s, spgm->bcc, "bcc:", isnot);
  ADD_GRAPH_SEARCH(s, spgm->cc, "cc:", isnot);
  ADD_GRAPH_SEARCH(s, spgm->to, "to:", isnot);
  if(spgm->text){
    unsigned char *q;
    buffer_add(&s, "\"");
    if(isnot) buffer_add(&s, "-");
    buffer_add(&s, q = graph_quoted(spgm->text->text.data));
    if(q) fs_give((void **) &q);
    buffer_add(&s, "\" ");
  }
			/* in graph this search includes Bcc: */
  if(spgm->or && spgm->or->first  && spgm->or->first->to
	     && spgm->or->second && spgm->or->second->cc)
     ADD_GRAPH_SEARCH(s, spgm->or->first->to, "recipients:", isnot);

			/* in graph this search includes Bcc: */
  if(spgm->or && spgm->or->first  && spgm->or->first->to
	      && spgm->or->second && spgm->or->second->or
	      && spgm->or->second->or->first
	      && spgm->or->second->or->first->cc
	      && spgm->or->second->or->second->from)
     ADD_GRAPH_SEARCH(s, spgm->or->first->to, "participants:", isnot);

  if(stream && LOCAL && s){
     int i;
     LOCAL->op = MessageSearch;
     if (LOCAL->resource) fs_give((void **) &LOCAL->resource);
     LOCAL->resource = fs_get(11 + 8 + strlen(LOCAL->folder.id) + 2 + 1);
     sprintf(LOCAL->resource, "mailFolders/%s/messages", LOCAL->folder.id);
     /* count parameters: $search, $select, $top, and blank */
     i = 4;			/* add parameters */
     LOCAL->params = fs_get(i*sizeof(HTTP_PARAM_S));
     memset((void *) LOCAL->params, 0, i*sizeof(HTTP_PARAM_S));
			/* parameter 1, $search */
     LOCAL->params[i = 0].name = cpystr("$search");
     LOCAL->params[i++].value = s;
			/* parameter 2, $select */
     LOCAL->params[i].name = cpystr("$select");
     LOCAL->params[i++].value = cpystr("id");
			/* parameter 3, $top */
     LOCAL->params[i].name = cpystr("$top");
     sprintf(LOCAL->tmp, "%lu", GRAPHCOUNTSEARCHITEMS);
     LOCAL->params[i++].value = cpystr(LOCAL->tmp);

     do {
        graph_send_command(stream);
     } while (stream && LOCAL && LOCAL->srchnextlink);
  }
  return stream && LOCAL ? LONGT : NIL;
}

unsigned long *graph_sort (MAILSTREAM *stream,char *charset,SEARCHPGM *spg,
			  SORTPGM *pgm,long flags)
{
  return 0L;
}

THREADNODE *graph_thread (MAILSTREAM *stream,char *type,char *charset,
			 SEARCHPGM *spg,long flags)
{
   return NIL;
}

long graph_ping (MAILSTREAM *stream)
{
  return graph_check_mailbox_changes(stream);
}

void graph_check (MAILSTREAM *stream)
{
  graph_check_mailbox_changes(stream);
}

long graph_expunge (MAILSTREAM *stream,char *sequence,long options)
{
  unsigned long i, total, totalex, counter;
  MESSAGECACHE *elt;
  unsigned char *body = NIL, *folderbody = NIL;
  unsigned long move;

  if(!stream || !LOCAL) return NIL;

  if(move = (unsigned long) mail_parameters(NIL, GET_GRAPHMOVEONEXPUNGE, NIL))
     folderbody = cpystr("{\"destinationId\":\"deleteditems\"}");

  /*
   * Given that we expunge one message at the time, if we expunge number 1 first
   * then number 2 becomes number 1, which makes it not possible to expunge
   * number 2 because it disappeared. If we have a list of 3 messages, then
   * expunging number 3 first does not change the numbers of messages 1 and 2,
   * so after we are done expunging the last message, we can safely go to
   * the previous one.
   */
  for(i = stream->nmsgs, total = 0L; i > 0; i--){
      elt = mail_elt(stream, i);
      if(elt->valid && elt->deleted) total++;
  }

  if(total == 0) return NIL;
  totalex = total;	/* a copy we need when we actually expunge the messages */
  /*
   * regardless of how many messages need to be deleted, we expunge
   * at most 20 at the time in a batch operation.
   */
		/* for the batch/post request */
   LOCAL->op = ExpungeMsg;
   if(LOCAL->resource) fs_give((void **) &LOCAL->resource);
   LOCAL->resource = cpystr("$batch");
   LOCAL->urltail = cpystr("/v1.0");
   LOCAL->status = HTTP_OK;		/* fake some non-zero code that is not success */

   for(i = stream->nmsgs; stream && LOCAL && LOCAL->status != 0 && total > 0; total -= counter){
	buffer_add(&body, "{");			/* start the list of requests */
	buffer_add(&body, "\"requests\":[");	/* requests is an array */
	for(counter = 0; counter < MAXBATCHLOAD && i > 0; i--){
	    elt = mail_elt(stream, i);
	    if(!elt->valid || !elt->deleted) continue;

	    if(counter > 0) buffer_add(&body, ",");	/* separate requests */
	    buffer_add(&body, "{");		/* start this request */
		buffer_add(&body, "\"id\":");	/* id is message number */
		sprintf(LOCAL->tmp, "\"%lu\"", i);
		buffer_add(&body, LOCAL->tmp);
		buffer_add(&body, ",");		/* separate with next element */

		buffer_add(&body, "\"method\":\"POST\"");	/* expunging is a POST? */
		buffer_add(&body, ",");		/* separate with next element */

		buffer_add(&body, "\"url\":\"/me/mailFolders/");      /* add the url */
		buffer_add(&body, LOCAL->folder.id);
		buffer_add(&body, "/messages/");
		buffer_add(&body, GDPP(elt)->id);
		if(move)
		   buffer_add(&body, "/move\"");
		else
		   buffer_add(&body, "/permanentDelete\"");
		buffer_add(&body, ",");		/* separate with next element */

		buffer_add(&body, "\"headers\":");/* this is a json object named "headers" */
		buffer_add(&body, "{");		/* begin of headers */
		if(move){
		   buffer_add(&body, "\"Content-Type\":\"application/json\""); buffer_add(&body, ",");
		   buffer_add(&body, "\"Content-Length\":");
		   sprintf(LOCAL->tmp, "%lu", strlen(folderbody));
		   buffer_add(&body, LOCAL->tmp);
		}
		else{
		   buffer_add(&body, "\"Content-Type\":\"text/plain\""); buffer_add(&body, ",");
		   buffer_add(&body, "\"Content-Length\":0");
		}
		buffer_add(&body, "}");		/* end of the headers object */
		buffer_add(&body, ",");		/* separate with next element */

		buffer_add(&body, "\"body\":");/* this is a json object named "body" */
		buffer_add(&body, move ? folderbody : (unsigned char *) "\"\"");
	    buffer_add(&body, "}");	/* end this request */
	    counter++;			/* count it! */
	}
	buffer_add(&body, "]");	/* end the array */
	buffer_add(&body, "}");	/* end the requests */

	LOCAL->private = (void *) body;
	graph_send_command(stream);
	if(body) fs_give((void **) &body);
   }
   /* Explanation of the flow: The previous code expunges all deleted messages from the
    * server. For each expunged message the *msg part is freed and nulled, but since
    * sequence numbers change every time we call mail_expunged, we postpone calling it
    * until we have nulled all the *msg parts, so the call to mail_expunged will work
    * well.
    */

   /* Now we are ready to expunge messages from the folder.
    * This code must be aligned to how graph_expunge selects
    * the messages that will be expunged above.
    */
   for(i = stream->nmsgs, counter = 0; i > 0 && counter < totalex; i--){
       elt = mail_elt(stream, i);
       if(!elt->valid || !elt->deleted) continue;
       if(!GDPP(elt)){
	   mail_expunged(stream, i);
	   counter++;
       }
   }

  if(folderbody) fs_give((void **) &folderbody);
  if(stream && LOCAL && LOCAL->private) LOCAL->private = NIL;
  return stream && LOCAL && LOCAL->status == HTTP_OK_NO_CONTENT ? LONGT : NIL;
}

long graph_copy (MAILSTREAM *stream,char *sequence,char *mailbox,long options)
{
  unsigned long i, total, counter;
  MESSAGECACHE *elt;
  GRAPH_MESSAGE *msg;
  unsigned char *body = NIL, *brequest = NIL, *clen = NIL;
  GRAPH_USER_FOLDERS *gf = NIL;

  gf = graph_folder_and_base(stream, mailbox, NULL);

  if(!gf || !stream || !LOCAL)  return NIL;

  if ((options & CP_UID) ? mail_uid_sequence (stream, sequence) :
        mail_sequence (stream,sequence))

  for(i = stream->nmsgs, total = 0L; i > 0; i--) if(mail_elt(stream, i)->sequence) total++;

  if(total == 0) return NIL;

  /* All requests have the same body */
  brequest = fs_get(1 + 13 + 1 + 1 + 1 + strlen(gf->id) + 1 + 1 + 1);
  sprintf(brequest, "{\"destinationId\":\"%s\"}", gf->id);
  sprintf(LOCAL->tmp, "%lu", strlen(brequest));
  buffer_add(&clen, "\"Content-Length\":"); buffer_add(&clen, LOCAL->tmp);

  /*
   * regardless of how many messages need to be copied, we copy
   * at most 20 at the time in a batch operation.
   */
		/* for the batch/post request */
   LOCAL->op = CopyMessage;
   if(LOCAL->resource) fs_give((void **) &LOCAL->resource);
   LOCAL->resource = cpystr("$batch");
   LOCAL->urltail = cpystr("/v1.0");
   LOCAL->status = HTTP_OK;		/* fake non zero local->status */

   for(i = stream->nmsgs; stream && LOCAL && LOCAL->status != 0 && total > 0; total -= counter){
	buffer_add(&body, "{");			/* start the list of requests */
	buffer_add(&body, "\"requests\":[");	/* requests is an array */
	for(counter = 0; counter < MAXBATCHLOAD && i > 0; i--){
	    elt = mail_elt(stream, i);
	    if(!elt->sequence) continue;
	    msg = GDPP(elt);

	    if(counter > 0) buffer_add(&body, ",");	/* separate requests */
	    buffer_add(&body, "{");		/* start this request */
		buffer_add(&body, "\"id\":\"");
		sprintf(LOCAL->tmp, "%lu", counter + 1);
		buffer_add(&body, LOCAL->tmp);
		buffer_add(&body, "\"");
		buffer_add(&body, ",");		/* separate with next element */

		buffer_add(&body, "\"method\":\"POST\"");
		buffer_add(&body, ",");		/* separate with next element */

		buffer_add(&body, "\"url\":\"/me/mailFolders/");      /* add the url */
		buffer_add(&body, LOCAL->folder.id);
		buffer_add(&body, "/messages/");
		buffer_add(&body, msg->id);
		buffer_add(&body, "/copy\"");
		buffer_add(&body, ",");		/* separate with next element */

		buffer_add(&body, "\"headers\":");/* this is a json object named "headers" */
		buffer_add(&body, "{");		/* begin of headers */
		   buffer_add(&body, "\"Content-Type\":\"application/json\""); buffer_add(&body, ",");
		   buffer_add(&body, clen);
		buffer_add(&body, "}");		/* end of the headers object */
		buffer_add(&body, ",");		/* separate with next element */

		buffer_add(&body, "\"body\":");
		buffer_add(&body, brequest);
	    buffer_add(&body, "}");	/* end this request */
	    counter++;			/* count it! */
	}
	buffer_add(&body, "]");	/* end the array */
	buffer_add(&body, "}");	/* end the requests */

	LOCAL->private = (void *) body;
	graph_send_command(stream);
	if(body) fs_give((void **) &body);
   }

  if(stream && LOCAL && LOCAL->private) LOCAL->private = NIL;
  return stream && LOCAL && LOCAL->status == HTTP_OK_CREATED ? LONGT : NIL;
}

long graph_append (MAILSTREAM *stream,char *mailbox,append_t af,void *data)
{
  return 0L;
}

void graph_gc (MAILSTREAM *stream,long gcflags)
{
}

void graph_gc_body (BODY *body)
{
  PART *part;
  if (body) {			/* have a body? */
    if (body->mime.text.data)	/* flush MIME data */
      fs_give ((void **) &body->mime.text.data);
				/* flush text contents */
    if (body->contents.text.data)
      fs_give ((void **) &body->contents.text.data);
    body->mime.text.size = body->contents.text.size = 0;
				/* multipart? */
    if (body->type == TYPEMULTIPART)
      for (part = body->nested.part; part; part = part->next)
	graph_gc_body (&part->body);
				/* MESSAGE/RFC822? */
    else if ((body->type == TYPEMESSAGE) && !strcmp (body->subtype,"RFC822")) {
      graph_gc_body (body->nested.msg->body);
      if (body->nested.msg->full.text.data)
	fs_give ((void **) &body->nested.msg->full.text.data);
      if (body->nested.msg->header.text.data)
	fs_give ((void **) &body->nested.msg->header.text.data);
      if (body->nested.msg->text.text.data)
	fs_give ((void **) &body->nested.msg->text.text.data);
      body->nested.msg->full.text.size = body->nested.msg->header.text.size =
	body->nested.msg->text.text.size = 0;
    }
  }
}

char *
graph_filter_string(char *orig, long *len)
{
   char *s = fs_get(6*strlen(orig)+1), *rv;
   char *t, *u, buf[5], *start;
   int escaped, converted, in_progress;
   unsigned long ucs4, hs, ls;

   buf[4] = '\0';
   hs = ls = 0;		/* default values so we can detect errors */
   for(t = orig, escaped = converted = in_progress = 0, u = s; *t ; t++){
      if(*t == '\\'){
	 escaped++;
	 if(escaped == 2){
	    *s++ = '\\';
	    converted++;
	    in_progress = escaped = 0;
	 }
	 continue;
      }
      else if(escaped == 1){
	 if(*t == 'u'){
	    if(!in_progress) start = t - 1; /* save starting position */
	    escaped++;
	    continue;
	 } else if (*t == 'r'){
	    if(in_progress){
	       while(start < t - 1) *s++ = *start++;
	       hs = ls = 0;
	       in_progress = 0;
	    }
	    *s++ = '\r';
	    converted++;
	 } else if (*t == 'n'){
	    if(in_progress){
	       while(start < t - 1) *s++ = *start++;
	       hs = ls = 0;
	       in_progress = 0;
	    }
	    *s++ = '\n';
	    converted++;
	 } else if (*t == '"'){
	    if(in_progress){
	       while(start < t - 1) *s++ = *start++;
	       hs = ls = 0;
	       in_progress = 0;
	    }
	    *s++ = '"';
	    converted++;
	 }
	 else {
	   if(in_progress){
	      while(start < t - 1) *s++ = *start++;
	      hs = ls = 0;
	      in_progress = 0;
	   }
	   *s++ = '\\';
	   *s++ = *t;
	 }
	 escaped = 0;
	 continue;
      } else if(escaped > 1){
	  buf[escaped++ - 2] = *t;
	  if(escaped == 6){
	     char *cerror;
	     int error;
	     error = 0;
	     ucs4 = strtoul(buf, &cerror, 16);
	     if(cerror && *cerror) error++;
	     if(!error){
		if(ucs4 >= 0xd800 && ucs4 <= 0xdfff){        /* surrogate*/
		   if(ucs4 >= 0xd800 && ucs4 <= 0xdbff){
		      hs = ucs4;
		      in_progress++;
		   } else {
		      if(in_progress){
			 ls = ucs4;
			 ucs4 = 0x10000 + ((hs - 0xD800) << 10) + (ls - 0xDC00);
			 s = utf8_put(s, ucs4);
			 in_progress = 0;
			 hs = ls = 0;
		      }
		      else
			error++;
		   }
	        }
		else
		  s = utf8_put(s, ucs4);
	     }
	     if(error){
		while(start <= t) *s++ = *start++;
		hs = ls = 0;
		in_progress = 0;
	     }
	     escaped = 0;
	     converted++;
	  }
	  continue;
      }
      else {
	if(in_progress){
	   while(start < t - 1) *s++ = *start++;
	   hs = ls = 0;
	   in_progress = 0;
	}
	*s++ = *t;
      }
   }
   *s = '\0';
   rv = cpystr(converted ? u : orig);
   if(len) *len = converted ? s - u : strlen(orig);
   fs_give((void **) &u);

   return rv;
}

ADDRESS *
graph_msg_to_address(GRAPH_ADDRESS_S *msg)
{
  char fakedomain[] = "@";
  ADDRESS *rv = NIL, *addr, *addr2;

  if(msg){
    for(; msg; msg = msg->next){
	addr = NIL;
        rfc822_parse_adrlist (&addr, msg->address, fakedomain);
	if(msg->name && msg->address && strcmp(msg->address, msg->name))
	   addr->personal = graph_filter_string(msg->name, NIL);
	if(!rv) rv = addr;
	else{
	   for(addr2 = rv; addr2 && addr2->next; addr2 = addr2->next);
	   addr2->next = addr;
	}
    }
  }
  return rv;
}

char *
graph_transform_date(char *date)
{
  int i;
  char rv[28], *s, *t;
  char *months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul",
			"Aug", "Sep", "Oct", "Nov", "Dec"};
  char *day, *month, *year, *hourtime;

  if(!date || date[strlen(date) - 1] != 'Z') return NIL;

  t = s = cpystr(date);
  for(year = s; *s && *s != '-'; s++);
  *s++ = '\0';
  for(month = s; *s && *s != '-'; s++);
  *s++ = '\0';
  i = atoi(month) - 1;
  month = months[i];
  for(day = s; *s && *s != 'T'; s++);
  *s++ = '\0';
  for(hourtime = s; *s && *s != 'Z'; s++);
  *s++ = '\0';

  sprintf(rv,"%s-%s-%s %s +0000", day, month, year, hourtime);
  fs_give((void **) &t);
  return cpystr(rv);
}

void
graph_parse_envelope (MAILSTREAM *stream,ENVELOPE **envp, GRAPH_MESSAGE *msg)
{
  ENVELOPE *env;

  if(!(msg->from || msg->sender || msg->toRecipients || msg->ccRecipients
	|| msg->bccRecipients || msg->replyTo || msg->subject
	|| msg->sentDateTime || msg->internetMessageId
	|| msg->internetMessageHeaders)) return;

  if(!*envp)
     env = mail_newenvelope();
  else
     env = *envp;
  env->incomplete = msg->valid & GPH_ALL_HEADERS ? NIL : T;
  if(!env->date) env->date = graph_transform_date(msg->sentDateTime);
  if(!env->subject && msg->subject) env->subject = graph_filter_string(msg->subject, NIL);
  if(!env->from && msg->from) env->from = graph_msg_to_address(msg->from);
  if(!env->sender && msg->sender) env->sender = graph_msg_to_address(msg->sender);
  if(!env->reply_to && msg->replyTo) env->reply_to = graph_msg_to_address(msg->replyTo);
  if(!env->to && msg->toRecipients) env->to = graph_msg_to_address(msg->toRecipients);
  if(!env->cc && msg->ccRecipients) env->cc = graph_msg_to_address(msg->ccRecipients);
  if(!env->bcc && msg->bccRecipients) env->bcc = graph_msg_to_address(msg->bccRecipients);
  if(!env->in_reply_to){
     GRAPH_PARAMETER *p;
     for(p = msg->internetMessageHeaders; p; p = p->next){
	 if(!compare_cstring(p->name, "in-reply-to")){
	   env->in_reply_to = cpystr(p->value);
	   break;
	 }
     }
  }
  if(!env->message_id && msg->internetMessageId) env->message_id = cpystr(msg->internetMessageId);
  if(msg->internetMessageHeaders) env->imapenvonly = T;	/* assume only the above data is available */
  if(!env->newsgroups){
     GRAPH_PARAMETER *p;
     for(p = msg->internetMessageHeaders; p; p = p->next){
	 if(!compare_cstring(p->name, "newsgroups")){
	   env->newsgroups = cpystr(p->value);
	   break;
	 }
     }
  }
  if(!env->followup_to){
     GRAPH_PARAMETER *p;
     for(p = msg->internetMessageHeaders; p; p = p->next){
	 if(!compare_cstring(p->name, "followup-to")){
	   env->followup_to = cpystr(p->value);
	   break;
	 }
     }
  }
  if(!env->references){
     GRAPH_PARAMETER *p;
     for(p = msg->internetMessageHeaders; p; p = p->next){
	 if(!compare_cstring(p->name, "references")){
	   env->references = cpystr(p->value);
	   break;
	 }
     }
  }
  if(!env->ngpathexists){
     GRAPH_PARAMETER *p;
     for(p = msg->internetMessageHeaders; p; p = p->next){
	 if(!compare_cstring(p->name, "path")){
	   env->ngpathexists = T;
	   break;
	 }
     }
  }
  if(env->newsgroups || env->followup_to || env->references || env->ngpathexists)
     env->imapenvonly = NIL;

  if(msg->sentDateTime) fs_give((void **) &msg->sentDateTime);
  if(msg->subject) fs_give((void **) &msg->subject);
  if(msg->internetMessageId) fs_give((void **) &msg->internetMessageId);
  if(msg->from) graph_free_address(&msg->from);
  if(msg->sender) graph_free_address(&msg->sender);
  if(msg->toRecipients) graph_free_address(&msg->toRecipients);
  if(msg->ccRecipients) graph_free_address(&msg->ccRecipients);
  if(msg->bccRecipients) graph_free_address(&msg->bccRecipients);
  if(msg->replyTo) graph_free_address(&msg->replyTo);

  *envp = env;
}

void graph_parse_flags (MAILSTREAM *stream, MESSAGECACHE *elt)
{
  GRAPH_MESSAGE *msg = GDPP(elt);
  struct {			/* old flags */
    unsigned int valid : 1;
    unsigned int seen : 1;
    unsigned int draft : 1;
    unsigned int deleted : 1;
  } old;
  old.valid = elt->valid; old.seen = elt->seen; old.draft = elt->draft; 
  old.deleted = elt->deleted;
  elt->valid = T;
  elt->user_flags = NIL;
  elt->seen = msg->isRead ? T : NIL;
  elt->draft = msg->isDraft ? T : NIL;
  /* Graph does not have any of these flags:  elt->flagged, elt->answered, elt->recent = NIL; */
  if (!old.valid || (old.seen != elt->seen) || (old.draft != elt->draft)
	|| (old.deleted != elt->deleted)) mm_flags (stream,elt->msgno);
}

long graph_cache (MAILSTREAM *stream,unsigned long msgno,char *seg,
		 STRINGLIST *stl,SIZEDTEXT *text)
{
  return 0L;
}

void
graph_promote_body(BODY *newbody, BODY *oldbody, char *type)
{
  newbody = mail_newbody();
  newbody->type = oldbody->type;
  newbody->subtype = oldbody->subtype;
  newbody->parameter = oldbody->parameter;
  newbody->contents.text.data = oldbody->contents.text.data;
  newbody->contents.text.size = oldbody->contents.text.size;
  newbody->size.bytes = oldbody->size.bytes;
  newbody->size.lines = oldbody->size.lines;
  newbody->nested.part = oldbody->nested.part;

  oldbody->type = TYPEMULTIPART;
  oldbody->subtype = cpystr(type);
  oldbody->parameter = mail_newbody_parameter ();
  oldbody->parameter->attribute = cpystr("boundary");
  oldbody->parameter->value = cpystr("91323:234234");
  oldbody->contents.text.data = NIL;
  oldbody->contents.text.size = 0;
  oldbody->size.bytes = 0;
  oldbody->size.lines = 0;
  if(oldbody->nested.part == NULL)
     oldbody->nested.part = mail_newbody_part();
  oldbody->nested.part->body = *newbody;
}


void
graph_parse_body_structure (MAILSTREAM *stream,BODY *body, GRAPH_MESSAGE *msg, char *type)
{
  PART *part = NIL;
  GRAPH_ATTACHMENT *matt;
  int i, mixed, inln;

  part = mail_newbody_part();
  graph_promote_body(&part->body, body, type);
  mixed  = (compare_cstring(type, "MIXED") == 0);
  inln = (compare_cstring(type, "RELATED") == 0);

  for(matt = msg->attachments; matt;
			matt = matt->next){
     if((mixed && matt->isInline) || (inln && !matt->isInline))
	continue;
     part = part->next = mail_newbody_part();
     part->body = *mail_newbody();
     if(matt->odata.mediaContentType){
	char *s;
	s = strchr(matt->odata.mediaContentType, '/');
	if(s){
	   *s = '\0';
	   for(i = 0; 
		(i <= TYPEMAX) && body_types[i]
		&& compare_cstring(matt->odata.mediaContentType, body_types[i]); i++);
	   if (i <= TYPEMAX) {		/* only if found a slot */
	      part->body.type = i;	/* set body type */
	      if (!body_types[i]) {	/* assign empty slot */
		 body_types[i] = matt->contentType;
		 matt->odata.mediaContentType = NIL;    /* don't free this string */
	      }
	   }
	   part->body.subtype = ucase(cpystr(++s));
	}
     }
     else stream->unhealthy = T;
     if(matt->id){
	part->body.id = fs_get(strlen(matt->id) + 2 + 1 + 8 + 1);
	sprintf(part->body.id, "<%s@graph.id>", matt->id);
     }
     if(matt->name){
	part->body.parameter = mail_newbody_parameter ();
	part->body.parameter->attribute = cpystr("name");
	part->body.parameter->value = cpystr(matt->name);
	part->body.description = cpystr(matt->name);
     }
     for(i = 0;
	 (i <= ENCMAX) && body_encodings[i] && strcmp("BASE64", body_encodings[i]);
	 i++);
     part->body.encoding = i;
     part->body.size.bytes = matt->size;
     part->body.size.lines = 1;
     if(compare_cstring(type, "MIXED") == 0)
	part->body.disposition.type = cpystr("ATTACHMENT");
     else if(compare_cstring(type, "RELATED") == 0)
	part->body.disposition.type = cpystr("INLINE");
     part->body.disposition.parameter = mail_newbody_parameter ();
     part->body.disposition.parameter->attribute = cpystr("FILENAME");
     part->body.disposition.parameter->value = cpystr(matt->name);
  }
}


void
graph_send_command (MAILSTREAM *stream)
{
  NETMBX mb;

  mail_valid_net_parse (stream->original_mailbox,&mb);

  if(!LOCAL->http_stream)
     LOCAL->http_stream = graph_open_netstream(stream);

  if (!graph_auth (stream,&mb)){
      LOCAL->netstream = NIL;
      graph_close(stream, NIL);
  }

  if(stream && LOCAL)
     graph_close_netstream(stream);
}

void
graph_parse_fast(MAILSTREAM *stream, unsigned long start, unsigned long end, int flag)
{
   unsigned long msgno;
   GRAPH_MESSAGE *msg;
   MESSAGECACHE *elt;

   for(msgno = start; msgno >= end; msgno--){
      elt = mail_elt (stream, msgno);
      if(elt){
	 msg = GDPP(elt);
	 if(!msg) continue;
	 if ((msg->valid & GPH_ALL_HEADERS) && !(msg->valid & GPH_ENVELOPE)
	    && (flag & GPH_ENVELOPE)){
	     msg->valid |= GPH_ENVELOPE;
	     continue;
	 }
	 elt->rfc822_size = msg->rfc822_size;
	 graph_parse_flags(stream, elt);

	 if(!(msg->valid & GPH_ALL_HEADERS) 
	    && msg->internetMessageHeaders
	    && elt->private.msg.header.text.size == 0){
	    STRING    s;
	    GRAPH_PARAMETER *p;
	    unsigned char *u;

	    for(p = msg->internetMessageHeaders; p; p = p->next){
		buffer_add(&elt->private.msg.header.text.data, p->name);
		buffer_add(&elt->private.msg.header.text.data, ": ");
		u = graph_filter_string(p->value, NULL);
		buffer_add(&elt->private.msg.header.text.data, u);
		fs_give((void **) &u);
		buffer_add(&elt->private.msg.header.text.data, "\r\n");
	    }
	    elt->private.msg.header.text.size = strlen(elt->private.msg.header.text.data);
	    rfc822_parse_msg_full(&elt->private.msg.env, NULL, elt->private.msg.header.text.data, elt->private.msg.header.text.size, &s, BADHOST, 0, 0);
	    msg->valid |= GPH_ALL_HEADERS;
	 }
	 else graph_parse_envelope(stream, &elt->private.msg.env, msg);
	 if (elt->private.msg.env
		&& elt->private.msg.env->date
		&& !mail_parse_date (elt, elt->private.msg.env->date)) {
	     stream->unhealthy = T;
	     mail_parse_date (elt,"01-Jan-1970 00:00:00 +0000");
	 }
      }
   }
}

GRAPH_MESSAGE *
graph_fetch_msg (MAILSTREAM *stream, unsigned long msgno)
{
  GRAPH_MESSAGE *msg;
  MESSAGECACHE *elt;
  long eo = (long) mail_parameters(NIL, GET_GRAPHENVELOPEONLY, NIL);
  unsigned long top, bottom;

  if(!stream || !LOCAL) return NIL;

  elt = mail_elt(stream, msgno);
  if(((msg = GDPP(elt)) == NULL) || (msg->id == NULL)){
     top = bottom = msgno;
     LOCAL->purpose = 0;
     graph_fetch_range(stream, &top, bottom, GPH_ID);
     if(top != bottom) return NIL;
     msg = GDPP(elt);
  }

  if (eo && !(msg->valid & GPH_ENVELOPE)){
      graph_get_message_header_text(stream, msgno, GPH_ENVELOPE);
      graph_parse_fast(stream, msgno, msgno, GPH_ENVELOPE);
  }

  if (!eo && !(msg->valid & GPH_ALL_HEADERS)){
     graph_fetch_range (stream, &msgno, msgno, GPH_ALL_HEADERS);
     graph_parse_fast(stream, msgno, msgno, GPH_ALL_HEADERS);
  }

  return msg;
}

/* fetch message information */
long
graph_fetch_range (MAILSTREAM *stream, unsigned long *top, unsigned long bottom, int flag)
{
  int i, j;
  GRAPH_USER_FOLDERS *gfolders = NIL;
  char string[MAILTMPLEN];
  unsigned char *s = NIL;
  int silent;

  if (stream->debug){
      sprintf(string, "graph_fetch_range(stream, %lu, %lu, %d)", *top, bottom, flag);
      mm_dlog (string);
  }
  LOCAL->purpose = flag;
  LOCAL->topmsg = *top;	/* for graph_challenge */
  LOCAL->countmessages = *top - bottom + 1;
  if (LOCAL->countmessages > GRAPHMAXCOUNTMSGS){
      LOCAL->countmessages = GRAPHMAXCOUNTMSGS;
      bottom = *top - GRAPHMAXCOUNTMSGS + 1;
  }

  if (LOCAL->resource) fs_give((void **) &LOCAL->resource);
  if(LOCAL->nextlink){
     LOCAL->resource = cpystr(LOCAL->nextlink + GRAPHBASELEN);
     graph_send_command(stream);
     if(stream && LOCAL && (LOCAL->purpose & (GPH_ENVELOPE|GPH_ALL_HEADERS)))
	graph_parse_fast(stream, *top, bottom, LOCAL->purpose);
  }
  else{
     LOCAL->resource = fs_get(11 + 8 + strlen(LOCAL->folder.id) + 2 + 1);
     sprintf(LOCAL->resource, "mailFolders/%s/messages", LOCAL->folder.id);
				/* count the number of parameters */
     i = 2; j = 0;		 	/* add $top */
     if(*top < stream->nmsgs) i++;	/* add $skip */
     if(flag & GPH_ATTACHMENTS) j++;	/* Op = GetAttachmentList */
     if(flag & (GPH_ENVELOPE|GPH_ID|GPH_STATUS|GPH_ALL_HEADERS)) j++; /* Op = GetFolderMessages */
     if(j > 1)
        fatal ("graph_fetch_range: found more than one operations in the same command.");
     else if (j == 1) i++;
			/* add parameters */
     LOCAL->params = fs_get(i*sizeof(HTTP_PARAM_S));
     memset((void *) LOCAL->params, 0, i*sizeof(HTTP_PARAM_S));
			/* parameter 1, $top */
     LOCAL->params[i = 0].name = cpystr("$top");
     sprintf(LOCAL->tmp, "%lu", LOCAL->countmessages);
     LOCAL->params[i++].value = cpystr(LOCAL->tmp);
			/* parameter 2, $skip */
     if (*top < stream->nmsgs){
	LOCAL->params[i].name = cpystr("$skip");
	sprintf(LOCAL->tmp, "%ld", stream->nmsgs - *top);
	LOCAL->params[i++].value = cpystr(LOCAL->tmp);
     }
			/* parameter 3, $select */
     if(j == 1) LOCAL->params[i].name = cpystr("$select");
     if(flag & GPH_ATTACHMENTS){
	LOCAL->op = GetAttachmentList;
	LOCAL->params[i].value = cpystr(GRAPHATTACHMENT);
     }
     else{
	unsigned long len = 0L;
	LOCAL->op = GetFolderMessages;
	j = 0;			/* count the number of flags given to us */
	if(flag & GPH_ENVELOPE){
	    j++;
	    len += strlen(GRAPHENVELOPE);
	}
	if(flag & GPH_ID){
	    j++;
	    len += strlen("id");
	}
	if(flag & GPH_STATUS){
	    j++;
	    len += strlen("isRead");
	}
	if(flag & GPH_ALL_HEADERS){
	    j++;
	    len += strlen("internetMessageHeaders");
	}
	LOCAL->params[i].value = fs_get(len + j);
	LOCAL->params[i].value[0] = '\0';
	if(flag & GPH_ENVELOPE){
	   if(strlen(LOCAL->params[i].value) > 0) strcat(LOCAL->params[i].value, ",");
	   strcat(LOCAL->params[i].value, GRAPHENVELOPE);
	   LOCAL->http_header = graph_text_preference();
	}
	if(flag & GPH_ID){
	   if(strlen(LOCAL->params[i].value) > 0) strcat(LOCAL->params[i].value, ",");
	   strcat(LOCAL->params[i].value, "id");
	}
	if(flag & GPH_STATUS){
	   if(strlen(LOCAL->params[i].value) > 0) strcat(LOCAL->params[i].value, ",");
	   strcat(LOCAL->params[i].value, "isRead");
	}
	if(flag & GPH_ALL_HEADERS){
	   if(strlen(LOCAL->params[i].value) > 0) strcat(LOCAL->params[i].value, ",");
	   strcat(LOCAL->params[i].value, "internetMessageHeaders");
	}
     }

     if(stream->debug){
	sprintf(string, "graph_fetch: $top=%lu$skip=%lu", LOCAL->countmessages, stream->nmsgs - *top);
	mm_dlog (string);
     }

     graph_send_command(stream);

     /* remove from our index all messages for which we received no data */
     silent = stream->silent;
     stream->silent = NIL;	/* inform upper levels */
     for(; stream && LOCAL && LOCAL->topmsg >= bottom; (*top)--)
	mail_expunged(stream, LOCAL->topmsg--);
     stream->silent = silent;

     if (j == 0) LOCAL->purpose = GPH_ENVELOPE;
     if(stream && LOCAL && (LOCAL->purpose & (GPH_ENVELOPE|GPH_ALL_HEADERS)))
	graph_parse_fast(stream, *top, bottom, LOCAL->purpose);
  }

  return stream && LOCAL ? LONGT : NIL;
}

void graph_flag (MAILSTREAM *stream, char *sequence, char *flag, long flags)
{
  MESSAGECACHE *elt;
  unsigned char *body = NIL, *bodycontent = NIL, *bcontentlen;
  unsigned long i, uf, total, totalid, counter, top, bottom;
  unsigned long aseen = 0;  /* alter seen counting number */
  unsigned long last_seen, *alterSeenSeq = NIL;
  long f;
  short nf;

  /* note we ignore user flags (uf) below */
  if (stream && LOCAL
	&& ((flags & ST_UID) ? mail_uid_sequence (stream,sequence) :
	     mail_sequence (stream,sequence))
	&& (f = mail_parse_flags (stream,flag,&uf))){
     /*
      * This part of the code will evolve over time. The point is to
      * get the id of each message that will be flagged instead of all
      * the id of all the messages.
      */
     top = bottom = 0;
     for(i = stream->nmsgs, total = 0L, totalid = 0; i > 0; i--){
         if((elt = mail_elt(stream, i))->sequence){
	     total++;
	     if(GDPQ->id){
		 if(top == 0) top = i;
		 bottom = i;
		 totalid++;
	     }
	 }
     }
     if(total > 0 && totalid != total)
	LOCAL->sync = graph_initial_sync(stream);

     if(total == 0) return;

     /* Some flags get altered but not recorded in the server. For example
      * the answered flag is local to the session. The only flag that
      * gets altered at the server level is the seen flag. So the plan
      * is as follows:
      * 1. Count the number of changes of the seen flag, and record the
      *    last message that need that change.
      *    a. if there is only one change, we will use graph_mark_msg_seen()
      *       to handle that. No internal update (call mm_flags) is done.
      *    b. if there is more than one change in the seen flag, we use
      *       a batch operation to change the seen flag. No internal update
      *       is done.
      * 2. Once that we have completed step 1 we go back and check all the
      *    changes, and make the internal update of flags (call mm_flags).
      */

     nf = (flags & ST_SET) ? T : NIL;
      /* Step 1 */
     if(f & fSEEN){
       for (i = 1; i <= stream->nmsgs; i++)
           if (((elt = mail_elt (stream,i))->sequence) && elt->seen != nf){
	      aseen++;
	      last_seen = i;	/* record the number in case it is unique */
           }
     }
     /* Step 1a */
     if(aseen == 1) graph_mark_msg_seen(stream, last_seen, nf);
     /* Step 1b */
     else if(aseen > 1){
			/* set up the bodycontent now */
	buffer_add(&bodycontent, "{");
	buffer_add(&bodycontent, "\"isRead\":");
	buffer_add(&bodycontent, nf ? "true" : "false");
	buffer_add(&bodycontent, "}");
	sprintf(LOCAL->tmp, "%lu", strlen(bodycontent));
	bcontentlen = cpystr(LOCAL->tmp);

	alterSeenSeq = fs_get((aseen+1)*sizeof(unsigned long));
	alterSeenSeq[aseen] = 0;	/* tie if off */
	for(i = 1, aseen = 0; i <= stream->nmsgs; i++)
	    if(((elt = mail_elt (stream,i))->sequence) && elt->seen != nf)
		alterSeenSeq[aseen++] = i;
        /*
         * regardless of how many messages need to be flagged, we flag
         * at most 20 at the time in a batch operation.
         */
		/* set up the batch/post request */
	LOCAL->op = FlagMsg;	/* since it is batch, it must be sent through a POST */
	if(LOCAL->resource) fs_give((void **) &LOCAL->resource);
	LOCAL->resource = cpystr("$batch");
	LOCAL->urltail = cpystr("/v1.0");
	LOCAL->status = HTTP_OK;

	for(i = 0; stream && LOCAL && LOCAL->status == HTTP_OK && aseen > 0; aseen -= counter){
	    buffer_add(&body, "{");			/* start the list of requests */
	    buffer_add(&body, "\"requests\":[");	/* requests is an array */
	    for(counter = 0; counter < MAXBATCHLOAD && alterSeenSeq[i] != 0; i++){
	        elt = mail_elt(stream, alterSeenSeq[i]);
	        if(counter > 0) buffer_add(&body, ",");	/* separate requests */

	        buffer_add(&body, "{");		/* start this request */
		  buffer_add(&body, "\"id\":");	/* id is message number */
		  sprintf(LOCAL->tmp, "\"%lu\"", alterSeenSeq[i]);
		  buffer_add(&body, LOCAL->tmp);
		  buffer_add(&body, ",");	/* separate with next element */

		  buffer_add(&body, "\"method\":\"PATCH\"");	/* each flagging is a PATCH */
		  buffer_add(&body, ",");	/* separate with next element */

		  buffer_add(&body, "\"url\":\"/me/mailFolders/");
		  buffer_add(&body, LOCAL->folder.id);
		  buffer_add(&body, "/messages/");
		  buffer_add(&body, GDPQ->id);
		  buffer_add(&body, "\"");
		  buffer_add(&body, ",");	/* separate with next element */

		  buffer_add(&body, "\"headers\":");
		  buffer_add(&body, "{");
		    buffer_add(&body, "\"Content-Type\":\"application/json\""); buffer_add(&body, ",");
		    buffer_add(&body, "\"Content-Length\":"); buffer_add(&body, bcontentlen);
		  buffer_add(&body, "}");
		  buffer_add(&body, ",");	/* separate with next element */

		  buffer_add(&body, "\"body\":");
		  buffer_add(&body, bodycontent);

	        buffer_add(&body, "}");	/* end this request */
	        counter++;		/* count it! */
	    }
	    buffer_add(&body, "]");	/* end the array */
	    buffer_add(&body, "}");	/* end the requests */
	    LOCAL->private = (void *) body;
	    graph_send_command(stream);
	    if(body) fs_give((void **) &body);
	}
	if(alterSeenSeq) fs_give((void **) &alterSeenSeq);
	if(bodycontent) fs_give((void **) &bodycontent);
	if(bcontentlen) fs_give((void **) &bcontentlen);
     }

     if(!stream || !LOCAL) return;

     /* Step 2 */
     for (i = 1; i <= stream->nmsgs; i++)
       if ((elt = mail_elt (stream,i))->sequence){
	  struct {		/* old flags */
	     unsigned int valid : 1;
	     unsigned int seen : 1;
	     unsigned int deleted : 1;
	     unsigned int flagged : 1;
	     unsigned int answered : 1;
	     unsigned int draft : 1;
	     unsigned long user_flags;
	  } old;
	  old.valid = elt->valid; old.seen = elt->seen;
	  old.deleted = elt->deleted; old.flagged = elt->flagged;
	  old.answered = elt->answered; old.draft = elt->draft;
	  old.user_flags = elt->user_flags;
	  if (f & fSEEN) elt->seen = nf;
	  if (f & fDELETED) elt->deleted = nf;
	  if (f & fFLAGGED) elt->flagged = nf;
	  if (f & fANSWERED) elt->answered = nf;
	  if (f & fDRAFT) elt->draft = nf;
	  elt->valid = T;		/* flags now altered */
	  if ((old.valid != elt->valid) || (old.seen != elt->seen) ||
	      (old.deleted != elt->deleted) || (old.flagged != elt->flagged) ||
	      (old.answered != elt->answered) || (old.draft != elt->draft) ||
	      (old.user_flags != elt->user_flags))
	    MM_FLAGS (stream,elt->msgno);
       }
  }
}

/*
 * check for changes in the mailbox. We batch three requests for changes
 * in additions, updates, and deletions of messages
 */
long
graph_check_mailbox_changes(MAILSTREAM *stream)
{
  int i, j, nreq, usedelta;
  HTTP_PARAM_S *params = NIL;
  unsigned char *resource = NIL, *t, *s, *base;

  if(!stream || !LOCAL) return NIL;

	/* for the post request */
  LOCAL->op = GetMailboxChanges;
  if(LOCAL->resource) fs_give((void **) &LOCAL->resource);
  LOCAL->resource = cpystr("$batch");
  LOCAL->urltail = cpystr("/v1.0");
  /* there are three requests: new message, updates, and deleted messages */
  nreq = 3; 	/* see the switch code below to see what to do when changing this value */
  do {
		/* create the resources part, starting with a new one */
     if(resource) fs_give((void **) &resource);
     if(!LOCAL->created.nextlink && !LOCAL->created.deltalink
	&& !LOCAL->updated.nextlink && !LOCAL->updated.deltalink
	&& !LOCAL->deleted.nextlink && !LOCAL->deleted.deltalink){
	resource = fs_get(2 + 11 + 8 + 5 + strlen(LOCAL->folder.id) + 5 + 1);
	sprintf(resource, "/me/mailFolders/%s/messages/delta", LOCAL->folder.id);
	usedelta = 0;
     }
     else {
	unsigned char *u;
	usedelta = 1;
	if(LOCAL->created.nextlink || LOCAL->created.deltalink)
	   base = LOCAL->created.nextlink ? LOCAL->created.nextlink : LOCAL->created.deltalink;
	else if(LOCAL->updated.nextlink || LOCAL->updated.deltalink)
	   base = LOCAL->updated.nextlink ? LOCAL->updated.nextlink : LOCAL->updated.deltalink;
	else if(LOCAL->deleted.nextlink || LOCAL->deleted.deltalink)
	   base = LOCAL->deleted.nextlink ? LOCAL->deleted.nextlink : LOCAL->deleted.deltalink;
	base += GRAPHSITELEN + strlen(LOCAL->urltail);
	if((u = strchr(base, '?')) != NULL){
	   *u = '\0';
	   resource = cpystr(base);
	   *u = '?';
	}
     }
     s = NIL;
     buffer_add(&s, "{");		/* start main json object */
     buffer_add(&s, "\"requests\":[");	/* start array of requests */
     for(j = 0; j < nreq; j++){
	if(j > 0) buffer_add(&s, ","); /* add a new element to the list */
	buffer_add(&s, "{");		/* start json request in array */
	   buffer_add(&s, "\"id\":");	/* add id element */
	   sprintf(LOCAL->tmp, "\"%lu\"", j);
	   buffer_add(&s, LOCAL->tmp);
	   buffer_add(&s, ",");

	   buffer_add(&s, "\"method\":\"GET\"");	/* add method */
	   buffer_add(&s, ",");

	   buffer_add(&s, "\"url\":");		/* add url */

	   if(!usedelta){	/* first time around */
	      i = 3;		/* for changeType, $select=id or $top, and extra blank */
				/* create parameters */
	      params = fs_get(i*sizeof(HTTP_PARAM_S));
	      memset((void *) params, 0, i*sizeof(HTTP_PARAM_S));
			/* parameter 1, changeType */
	      params[i = 0].name = cpystr("changeType");
	      switch(j){
		  case GRAPH_NEWMSGS: params[i++].value = cpystr("created"); break;
		  case GRAPH_UPDMSGS: params[i++].value = cpystr("updated"); break;
		  case GRAPH_DELMSGS: params[i++].value = cpystr("deleted"); break;
		  default: fatal ("graph_check_mailbox_changes: Found non-coded changetype in changetype."); break;
	      }
	      switch(j){
		  case GRAPH_NEWMSGS: params[i].name = cpystr("$top");
				      params[i].value = cpystr("1");
				      break;
		  case GRAPH_UPDMSGS: params[i].name = cpystr("$select");
				      params[i++].value = cpystr("isRead"); break;
		  case GRAPH_DELMSGS: params[i].name = cpystr("$select");
				      params[i++].value = cpystr("id"); break;
		  default: fatal ("graph_check_mailbox_changes: Found non-coded changetype in $select."); break;
	      }
	   } else {
	      i = 2;		/* for $skiptoken or $deltatoken, and extra blank */
				/* create parameters */
	      params = fs_get(i*sizeof(HTTP_PARAM_S));
	      memset((void *) params, 0, i*sizeof(HTTP_PARAM_S));
			/* parameter 1, $skiptoken  or $deltatoken */
	      switch(j){
		   case GRAPH_NEWMSGS:
			if(LOCAL->created.nextlink || LOCAL->created.deltalink)
			   base = LOCAL->created.nextlink ? LOCAL->created.nextlink : LOCAL->created.deltalink;
			params[i = 0].name = base == LOCAL->created.nextlink
					     ? cpystr("$skiptoken") : cpystr("$deltatoken");
			break;

		   case GRAPH_UPDMSGS:
			if(LOCAL->updated.nextlink || LOCAL->updated.deltalink)
			   base = LOCAL->updated.nextlink ? LOCAL->updated.nextlink : LOCAL->updated.deltalink;
			params[i = 0].name = base == LOCAL->updated.nextlink
					     ? cpystr("$skiptoken") : cpystr("$deltatoken");
			break;

		   case GRAPH_DELMSGS:
			if(LOCAL->deleted.nextlink || LOCAL->deleted.deltalink)
			   base = LOCAL->deleted.nextlink ? LOCAL->deleted.nextlink : LOCAL->deleted.deltalink;
			params[i = 0].name = base == LOCAL->deleted.nextlink
					     ? cpystr("$skiptoken") : cpystr("$deltatoken");
			break;

		   default: fatal ("graph_check_mailbox_changes: too many requests."); break;
	      }
	      params[i].value = cpystr(base + GRAPHSITELEN + strlen(LOCAL->urltail) + strlen(resource) + 1 + strlen(params[i].name) + 1);
	      i++;
	   }
	   t = http_get_param_url(resource, params);
	   buffer_add(&s, "\""); buffer_add(&s, t); buffer_add(&s, "\"");
	buffer_add(&s, "}");		/* end json request in array */

	fs_give((void **) &t);
	http_param_free(&params);

     }
     buffer_add(&s, "]");		/* end array of requests */
     buffer_add(&s, "}");		/* end main json object */
     LOCAL->private = (char *) s;	/* set up body for graph_response */
     graph_send_command(stream);
     if(stream && LOCAL && s) fs_give((void **) &s);
     if(resource) fs_give((void **) &resource);
  } while (stream && LOCAL && (LOCAL->created.nextlink || LOCAL->updated.nextlink || LOCAL->deleted.nextlink));
  if(stream && LOCAL && LOCAL->urltail) fs_give((void **) &LOCAL->urltail);
  if(stream && LOCAL) LOCAL->private = NIL;
  return stream && LOCAL ? LONGT : 0;
}


/*************  GRAPH SEND ******************/

MAILSTREAM *
graph_send_open (ADDRESS *from)
{
  char tmp[MAILTMPLEN];

  mail_parameters(NIL, SET_GRAPHOPENOPERATION, (void *)(long) GraphSendMail);
  sprintf(tmp, "{graph.microsoft.com/graph/user=%s@%s}<none>", from->mailbox, from->host);
  return mail_open(NIL, tmp, 0);
}

long
graph_send_soutr(void *s, char *text)
{
  MAILSTREAM *stream = (MAILSTREAM *) s;

  if(!text) return NIL;

  if(stream && LOCAL){
     char *st = (char *) LOCAL->private;
     size_t len = st ? strlen(st) : 0;
     fs_resize((void **) &st, len + strlen(text) + 1);
     st[len] = '\0';
     strcat(st, text);
     LOCAL->private = (void *) st;
  }
  return stream && LOCAL ? LONGT : NIL;
}

int
graph_send_mail(MAILSTREAM *stream)
{
  unsigned char *s;
  unsigned long len;

  LOCAL->op = GraphSendMail;
  if(LOCAL->resource) fs_give((void **) &LOCAL->resource);
  LOCAL->resource = cpystr("sendMail");
  s = rfc822_binary(LOCAL->private, strlen((char*) LOCAL->private), &len);
  if(s[len-2] < ' ') s[len-2] = '\0';
  fs_give(&LOCAL->private);		/* we do not need the original text anymore */
  LOCAL->private = (void *) s;		/* transfer the encoded text instead */
  graph_send_command(stream);		/* this uses LOCAL->private in graph_challenge_post */
  if(stream && LOCAL && LOCAL->private)
    fs_give((void **) &LOCAL->private);	/* we do not need the base64 text anymore */
  return stream && LOCAL && (LOCAL->status == HTTP_OK) ? 1 : 0;
}

/************** Loser Function Support ****************/


char *graph_header (MAILSTREAM *stream,unsigned long msgno,
                unsigned long *length, long flags)
{
  char *s = NULL;
  MESSAGECACHE *elt;
  GRAPH_MESSAGE *msg;

  if (length) *length = 0;
  if (flags & FT_UID || !LOCAL) return "";      /* UID call "impossible" */
  elt = mail_elt (stream, msgno);
  if(elt->private.msg.header.text.data){
     *length = elt->private.msg.header.text.size;
     return elt->private.msg.header.text.data;
  }

  if((msg = GDPP(elt)) == NULL){
     LOCAL->purpose = 0;
     graph_fetch_range(stream, &msgno, msgno, GPH_ID);
     msg = GDPP(elt);
  }

  if(stream && LOCAL && msg && !(msg->valid & GPH_MIME)){
     LOCAL->op = GetMimeMsg;
     LOCAL->topmsg = msgno;	/* for graph_challenge */
     if (LOCAL->resource) fs_give((void **) &LOCAL->resource);
     LOCAL->resource = fs_get(11 + 8 + 8 + strlen(LOCAL->folder.id) + strlen(msg->id) + 4 + 1);
     sprintf(LOCAL->resource, "mailFolders/%s/messages/%s/%%24value", LOCAL->folder.id, msg->id);
     graph_send_command(stream);
  }

  if(!(msg && msg->mimetext)) return NIL;
  msg->valid |= GPH_MIME|GPH_RFC822_BODY|GPH_ALL_HEADERS;

  elt->private.msg.full.text.data = cpystr(msg->mimetext);
  elt->private.msg.full.text.size = strlen(msg->mimetext);
  elt->rfc822_size = elt->private.msg.full.text.size;

  if(elt->private.msg.header.text.size == 0){
     char *h = strstr(msg->mimetext, "\r\n\r\n");
     elt->private.msg.header.text.size = h - (char *) msg->mimetext + 4;
     elt->private.msg.header.text.data = fs_get(elt->private.msg.header.text.size + 1);
     strncpy(elt->private.msg.header.text.data, msg->mimetext, elt->private.msg.header.text.size);
     elt->private.msg.header.text.data[elt->private.msg.header.text.size] = '\0';
  }

  *length = elt->private.msg.header.text.size;
  elt->private.msg.text.offset = elt->private.msg.header.text.size;
  elt->private.msg.text.text.data = cpystr(msg->mimetext + elt->private.msg.text.offset);
  elt->private.msg.text.text.size = strlen(elt->private.msg.text.text.data);
  fs_give((void **) &msg->mimetext);
  return elt->private.msg.header.text.data;
}

long graph_text (MAILSTREAM *stream, unsigned long msgno, STRING *bs, long flags)
{
  MESSAGECACHE *elt;
  char *s;
  GRAPH_MESSAGE *msg;

  INIT (bs, mail_string, "", 0);
                                /* UID call "impossible" */
  if (flags & FT_UID || !LOCAL) return NIL;
  elt = mail_elt (stream, msgno);

  if (!(flags & FT_PEEK) && !elt->seen){
    elt->seen = T;
    graph_mark_msg_seen(stream, msgno, 1);
    MM_FLAGS(stream, msgno);
  }

  INIT (bs, mail_string, elt->private.msg.text.text.data, elt->private.msg.text.text.size);
  return LONGT;
}
