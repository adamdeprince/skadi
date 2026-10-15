/* Copyright 2026 Adam DePrince. Apache-2.0. Included by offcache.c.
 * Batch fetches use UID identity and commit each complete message separately.
 * Explicitly downloaded bodies live in individual files, outside the bounded
 * opportunistic body cache. Only the body being read needs to be in memory.
 */
#include "../c-client/sha.h"
#include <stdint.h>

typedef struct oc_uidset {
    char *text;
    size_t length, capacity;
} OC_UIDSET;

static int oc_uid_compare(const void *a, const void *b)
{
    unsigned long x = *(const unsigned long *)a, y = *(const unsigned long *)b;
    return x < y ? -1 : x > y;
}

static int oc_uid_range(OC_UIDSET *set, unsigned long first, unsigned long last)
{
    int n = snprintf(NULL, 0, "%s%lu:%lu", set->length ? "," : "", first, last);
    size_t needed, capacity;
    void *p;
    if(n < 0 || (size_t)n >= SIZE_MAX - set->length) return 0;
    needed = set->length + (size_t)n + 1;
    if(needed > set->capacity){
        capacity = set->capacity <= SIZE_MAX / 2 ? set->capacity * 2 : needed;
        if(capacity < needed) capacity = needed;
        if(!(p = realloc(set->text, capacity))) return 0;
        set->text = (char *)p;
        set->capacity = capacity;
    }
    set->length += (size_t)sprintf(set->text + set->length, "%s%lu:%lu",
                                  set->length ? "," : "", first, last);
    return 1;
}

static char *oc_njord_path(OC_FOLDER *f, unsigned long uid, const char *suffix)
{
    SHA256Context hash;
    unsigned char digest[SHA256HashSize];
    char hex[2 * SHA256HashSize + 1]; /* Fixed by SHA-256, not a filename limit. */
    char *result;
    size_t left, take;
    const unsigned char *p;
    int n, i;
    if(!f || !f->uidvalidity || !uid || !oc_dir_ok) return NULL;
    SHA256Reset(&hash);
    for(left = strlen(f->mailbox), p = (const unsigned char *)f->mailbox; left; left -= take, p += take){
        take = left > UINT_MAX ? UINT_MAX : left; /* SHA256Input's unsigned-int API. */
        if(SHA256Input(&hash,p,(unsigned int)take)) return NULL;
    }
    if(SHA256Result(&hash,digest)) return NULL;
    for(i = 0; i < SHA256HashSize; ++i) sprintf(hex + 2 * i, "%02x", digest[i]);
    n = snprintf(NULL,0,"%s/%s-%lu-%lu%s",oc_dir,hex,f->uidvalidity,uid,suffix);
    if(n < 0 || !(result = (char *)malloc((size_t)n + 1))) return NULL;
    sprintf(result,"%s/%s-%lu-%lu%s",oc_dir,hex,f->uidvalidity,uid,suffix);
    return result;
}

static int oc_njord_has_body(OC_FOLDER *f, unsigned long uid)
{
    char *path = oc_njord_path(f,uid,".eml");
    struct stat st;
    int result = path && !stat(path,&st) && S_ISREG(st.st_mode) && st.st_size > 0;
    free(path);
    return result;
}

static int oc_njord_load(OC_FOLDER *f, unsigned long uid, SIZEDTEXT *raw, const char *suffix)
{
    char *path = oc_njord_path(f,uid,suffix);
    FILE *file = path ? our_fopen(path,"rb") : NULL;
    struct stat st;
    size_t size;
    free(path);
    if(!file) return 0;
    if(fstat(fileno(file),&st) || st.st_size <= 0 ||
       (uintmax_t)st.st_size >= SIZE_MAX || (uintmax_t)st.st_size > ULONG_MAX){ fclose(file); return 0; }
    size = (size_t)st.st_size;
    raw->data = (unsigned char *)malloc(size + 1);
    if(!raw->data){ fclose(file); return 0; }
    if(fread(raw->data,1,size,file) != size){
        free(raw->data); raw->data = NULL; fclose(file); return 0;
    }
    fclose(file);
    raw->size = (unsigned long)size;
    raw->data[size] = 0;
    return 1;
}

static int oc_njord_store(OC_FOLDER *f, unsigned long uid, SIZEDTEXT *raw, const char *suffix)
{
    char *path = oc_njord_path(f,uid,suffix), *temporary = NULL;
    size_t n;
    int fd = -1, result = 0;
    FILE *file = NULL;
    if(!path || !raw->data || !raw->size) goto done;
    if(create_mail_dir(oc_dir) < 0 && errno != EEXIST) goto done;
    n = strlen(path);
    if(n > SIZE_MAX - sizeof(".tmp.XXXXXX")) goto done;
    temporary = (char *)malloc(n + sizeof(".tmp.XXXXXX"));
    if(!temporary) goto done;
    sprintf(temporary,"%s.tmp.XXXXXX",path);
    if((fd = mkstemp(temporary)) < 0) goto done;
    file = fdopen(fd,"wb");
    if(!file){ close(fd); goto done; }
    if(fwrite(raw->data,1,raw->size,file) == raw->size && !fflush(file) && !fsync(fileno(file)))
        result = 1;
    if(fclose(file)) result = 0;
    if(result && rename_file(temporary,path) < 0) result = 0;
done:
    if(temporary){ if(!result) unlink(temporary); free(temporary); }
    free(path);
    return result;
}

static int oc_njord_load_body(OC_FOLDER *f, unsigned long uid, SIZEDTEXT *raw)
{ return oc_njord_load(f,uid,raw,".eml"); }

static int oc_info_part(STORE_S *out, BODY *body, const char *prefix)
{
    PART *part;
    PARAMETER *parameter;
    const char *name = NULL, *type;
    char *line, *number;
    unsigned long index = 0;
    int n, result;
    if(!body) return 1;
    if(body->type == TYPEMULTIPART){
        for(part = body->nested.part; part; part = part->next){
            n = snprintf(NULL,0,"%s%s%lu",prefix,*prefix ? "." : "",++index);
            if(n < 0 || !(number = malloc((size_t)n+1))) return 0;
            sprintf(number,"%s%s%lu",prefix,*prefix ? "." : "",index);
            result = oc_info_part(out,&part->body,number);
            free(number);
            if(!result) return 0;
        }
        return 1;
    }
    for(parameter = body->disposition.parameter; parameter; parameter = parameter->next)
        if(!strucmp(parameter->attribute,"FILENAME")) name = parameter->value;
    for(parameter = body->parameter; !name && parameter; parameter = parameter->next)
        if(!strucmp(parameter->attribute,"NAME")) name = parameter->value;
    type = body->type <= TYPEMAX ? body_types[body->type] : "OTHER";
    n = snprintf(NULL,0,"Part %s  %s/%s\n  %lu encoded bytes before transport compression\n  Name: ",
                 *prefix ? prefix : "1",type ? type : "OTHER",body->subtype ? body->subtype : "UNKNOWN",body->size.bytes);
    if(n < 0 || !(line = malloc((size_t)n+1))) return 0;
    sprintf(line,"Part %s  %s/%s\n  %lu encoded bytes before transport compression\n  Name: ",
            *prefix ? prefix : "1",type ? type : "OTHER",body->subtype ? body->subtype : "UNKNOWN",body->size.bytes);
    result = so_puts(out,line); free(line);
    if(!result) return 0;
    if(!name) name = "(unnamed)";
    for(; *name; ++name)
        if(!so_writec(((unsigned char)*name < 32 || (unsigned char)*name == 127) ? '?' : *name,out)) return 0;
    return so_puts(out,"\n\n");
}

static char *oc_info(BODY *body)
{
    STORE_S *out = so_get(CharStar,NULL,EDIT_ACCESS);
    char *text = NULL;
    if(out && so_puts(out,"Attachment sizes from IMAP BODYSTRUCTURE\n\nThese are transfer-encoded sizes, not decoded file sizes. Base64 and quoted-printable add overhead; zstd can reduce the transfer. No attachment contents are downloaded for this list.\n\n") &&
       oc_info_part(out,body,"")) text = cpystr(so_text(out));
    if(out) so_give(&out);
    return text;
}

char *offcache_attachment_info(MAILSTREAM *stream, unsigned long msgno)
{
    OC_FOLDER *f;
    SIZEDTEXT text = {0};
    BODY *body = NULL;
    if(!stream || !msgno || msgno > stream->nmsgs) return NULL;
    if(offcache_is_stream(stream)){
        f = LOCAL->folder;
        if(f && msgno <= f->n && oc_njord_load(f,f->msgs[msgno-1].uid,&text,".mime"))
            return (char *)text.data;
        return NULL;
    }
    /* IMAP's structure fetch does not retrieve any MIME part contents. */
    if(!mail_fetchstructure(stream,msgno,&body)) return NULL;
    return oc_info(body);
}

typedef struct oc_batch {
    OC_FOLDER *folder;
    unsigned long validity;
    unsigned long *uids;
    unsigned char *received;
    size_t count;
    int bodies, failed;
} OC_BATCH;

static void oc_batch_item(MAILSTREAM *stream, unsigned long msgno, void *value)
{
    OC_BATCH *batch = (OC_BATCH *)value;
    MESSAGECACHE *elt = mail_elt(stream,msgno);
    OC_MSG *m;
    SIZEDTEXT *text;
    unsigned long *wanted;
    if(stream->uid_validity != batch->validity){ batch->failed = 1; return; }
    if(!elt || !elt->private.uid) return;
    if(batch->bodies){
        wanted = (unsigned long *)bsearch(&elt->private.uid,batch->uids,batch->count,sizeof(*wanted),oc_uid_compare);
        if(!wanted) return;
        text = &elt->private.msg.full.text;
        if(!text->data) return; /* Unsolicited FLAGS updates are not body replies. */
        if(!oc_njord_store(batch->folder,elt->private.uid,text,".eml")) batch->failed = 1;
        else batch->received[wanted - batch->uids] = 1;
        if(text->data) fs_give((void **)&text->data);
        text->size = 0; /* Release each message before receiving the next one. */
        return;
    }
    text = &elt->private.msg.header.text;
    if(!text->data || !text->size || elt->private.msg.lines) return;
    m = oc_find_uid(batch->folder,elt->private.uid);
    if(!m){ m = oc_add_msg(batch->folder); m->uid = elt->private.uid; }
    if(m->hdr) fs_give((void **)&m->hdr);
    m->hdr = oc_dup((char *)text->data,text->size);
    m->hdrlen = text->size;
    m->full_header = 1;
    m->size = elt->rfc822_size;
    batch->folder->dirty = 1;
}

int offcache_sync_headers(MAILSTREAM *stream)
{
    OC_FOLDER *f;
    OC_BATCH batch;
    OC_UIDSET set = {0};
    unsigned long *known, next = 1;
    size_t count = 0, i;
    int result = 0, exhausted = 0;
    if(!offcache_enabled() || !offcache_cacheable_stream(stream) || offcache_is_stream(stream)) return 0;
    f = oc_get(stream->original_mailbox ? stream->original_mailbox : stream->mailbox);
    if(!f || !stream->uid_validity || f->n > SIZE_MAX / sizeof(*known)) return 0;
    if(f->uidvalidity && f->uidvalidity != stream->uid_validity) oc_free_folder_contents(f);
    f->uidvalidity = stream->uid_validity;
    known = f->n ? (unsigned long *)malloc(f->n * sizeof(*known)) : NULL;
    if(f->n && !known) return 0;
    for(i = 0; i < f->n; ++i)
        if(f->msgs[i].full_header && f->msgs[i].hdr && f->msgs[i].uid && f->msgs[i].uid <= UINT32_MAX)
            known[count++] = f->msgs[i].uid;
    if(count) qsort(known,count,sizeof(*known),oc_uid_compare);
    for(i = 0; i < count; ++i){
        if(known[i] < next) continue;
        if(known[i] > next && !oc_uid_range(&set,next,known[i] - 1)) goto done;
        if(known[i] == UINT32_MAX){ exhausted = 1; break; }
        next = known[i] + 1;
    }
    if(!exhausted && !oc_uid_range(&set,next,UINT32_MAX)) goto done;
    memset(&batch,0,sizeof(batch)); batch.folder = f; batch.validity = f->uidvalidity;
    result = imap_fetch_batch(stream,set.text ? set.text : "1:*",
                              set.text ? IMAP_BATCH_HEADERS : IMAP_BATCH_METADATA,
                              oc_batch_item,&batch) && !batch.failed;
    if(result){
        for(i = 1; i <= stream->nmsgs; ++i){
            MESSAGECACHE *elt = mail_elt(stream,i);
            char *info = elt->private.msg.body ? oc_info(elt->private.msg.body) : NULL;
            if(info){
                SIZEDTEXT text;
                text.data = (unsigned char *)info; text.size = strlen(info);
                if(!oc_njord_store(f,elt->private.uid,&text,".mime")) result = 0;
                fs_give((void **)&info);
            }
            else result = 0;
        }
    }
    oc_learn(stream,0);
    offcache_save_all();
done:
    free(known); free(set.text);
    return result;
}

int offcache_prepare_batch(MAILSTREAM *stream, const unsigned long *uids, size_t count)
{
    OC_FOLDER *f;
    OC_BATCH batch;
    OC_UIDSET set = {0};
    MAILSTREAM *remote = stream;
    size_t i, used = 0, last;
    int result = 0, cached = offcache_is_stream(stream);
    if(!count) return 1;
    if(!offcache_enabled() || !stream || count > SIZE_MAX / sizeof(*batch.uids)) return 0;
    f = cached ? LOCAL->folder : oc_get(stream->original_mailbox ? stream->original_mailbox : stream->mailbox);
    if(!f) return 0;
    if(!cached){
        if(f->uidvalidity && f->uidvalidity != stream->uid_validity) oc_free_folder_contents(f);
        f->uidvalidity = stream->uid_validity;
    }
    memset(&batch,0,sizeof(batch)); batch.folder = f; batch.validity = f->uidvalidity; batch.bodies = 1;
    batch.uids = (unsigned long *)malloc(count * sizeof(*batch.uids));
    batch.received = (unsigned char *)calloc(count,1);
    if(!batch.uids || !batch.received) goto done;
    for(i = 0; i < count; ++i)
        if(uids[i] && !oc_njord_has_body(f,uids[i])) batch.uids[used++] = uids[i];
    if(!used){ result = 1; goto done; }
    qsort(batch.uids,used,sizeof(*batch.uids),oc_uid_compare);
    for(i = 0; i < used; ++i)
        if(!batch.count || batch.uids[i] != batch.uids[batch.count - 1]) batch.uids[batch.count++] = batch.uids[i];
    for(i = 0; i < batch.count; i = last + 1){
        for(last = i; last + 1 < batch.count && batch.uids[last] < ULONG_MAX &&
            batch.uids[last + 1] == batch.uids[last] + 1; ++last) ;
        if(!oc_uid_range(&set,batch.uids[i],batch.uids[last])) goto done;
    }
    if(cached) remote = mail_open(NULL,f->mailbox,OP_READONLY | OP_SILENT);
    if(!remote || !batch.validity || remote->uid_validity != batch.validity) goto done;
    result = imap_fetch_batch(remote,set.text,IMAP_BATCH_BODIES,oc_batch_item,&batch) && !batch.failed;
    for(i = 0; i < batch.count; ++i) if(!batch.received[i]) result = 0;
done:
    if(cached && remote && remote != stream) mail_close(remote);
    free(batch.uids); free(batch.received); free(set.text);
    return result;
}
