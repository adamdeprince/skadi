/* End-to-end c-client transport and batch checks, with an isolated test account. */
#include "../alpine/headers.h"
#include "../pith/conf.h"
#include "../pith/offcache.h"
#include "../pith/remoteimg.h"
#include <webp/decode.h>
#include "../alpine/status.h"
#undef NDEBUG
#include <assert.h>

extern DRIVER imapdriver;
static unsigned long received;

static void item(MAILSTREAM *stream, unsigned long msgno, void *context)
{
    MESSAGECACHE *elt = mail_elt(stream, msgno);
    int bodies = *(int *)context;
    assert(elt->private.uid == 100 + msgno);
    if(bodies){
        SIZEDTEXT *text = &elt->private.msg.full.text;
        assert(text->data && text->size && strstr((char *)text->data,"batch-body"));
        fs_give((void **)&text->data);
        text->size = 0;
    }
    else{
        BODY *body = elt->private.msg.body;
        assert(elt->private.msg.header.text.data);
        assert(elt->private.msg.env && elt->private.msg.env->subject);
        assert(body && body->type == TYPEMULTIPART);
        assert(body->nested.part && body->nested.part->next);
        assert(body->nested.part->next->body.size.bytes == 123456);
        assert(!elt->seen);
    }
    ++received;
}

int main(int argc, char **argv)
{
    MAILSTREAM *stream;
    NETMBX mb;
    int bodies = 0;
    char *uids, *next;
    unsigned long i;
    assert(argc == 3 || argc == 4);
    debug = 0;
    ps_global = new_pine_struct();
    ps_global->home_dir = cpystr(argv[1]);
    ps_global->maildomain = cpystr("example.invalid");
    process_feature_list(ps_global, NULL, 0, 0, 0);
    if(argc == 4){
        MAILSTREAM fake = {0};
        const unsigned char *data;
        const char *error;
        size_t length;
        int w,h;
        fake.mailbox = argv[2]; ps_global->mail_stream = &fake;
        assert(!remote_image_data(argv[3],&length,0,&error));
        data = remote_image_data(argv[3],&length,1,&error);
        assert(data && !error && WebPGetInfo(data,length,&w,&h));
        assert(w <= 1024 && h <= 1024);
        assert(remote_image_data(argv[3],&length,1,&error) == data);
        data = remote_image_data(argv[3],&length,2,&error);
        assert(data && !error && length > 3*1024*1024 && !memcmp(data,"\x89PNG",4));
        assert(remote_image_data(argv[3],&length,2,&error) == data);
        remote_image_clear();
        puts("PASS: lazy Njord images, WebP preview, exact original, repeated cache reads");
        return 0;
    }
    offcache_link_driver(); offcache_init(); offcache_load_all();
    assert(mail_valid_net_parse("{localhost/njord}INBOX", &mb));
    assert(mb.njordflag && mb.notlsflag && mb.port == 1143);
    assert(mail_valid_net_parse("{localhost/imap/notls}INBOX", &mb) && !mb.njordflag);
    assert(!mail_valid_net_parse("{localhost/njord/starttls}INBOX", &mb));
    mail_link(&imapdriver);
    mail_parameters(NULL, SET_OPENTIMEOUT, (void *)5);
    mail_parameters(NULL, SET_READTIMEOUT, (void *)5);
    stream = mail_open(NULL, argv[2], OP_SILENT | OP_READONLY);
    assert(stream && stream->uid_validity == 1234);
    uids = (char *)malloc(100000); /* Deliberately exceeds legacy MAXCOMMAND in this test. */
    assert(uids);
    next = uids;
    for(i = 101; i < 5101; ++i) next += sprintf(next,"%s%lu",i == 101 ? "" : ",",i);
    if(!imap_fetch_batch(stream,uids,0,item,&bodies)){
        fprintf(stderr,"Header batch failed: %s (unhealthy=%u, received=%lu)\n",
                last_message_queued(), stream->unhealthy, received);
        return 1;
    }
    free(uids);
    assert(received == 2);
    bodies = 1; received = 0;
    assert(imap_fetch_batch(stream,"101:102",1,item,&bodies));
    assert(received == 2);
    assert(offcache_sync_headers(stream));
    assert(offcache_sync_headers(stream));
    {
        unsigned long selected[] = {101,102,101};
        char *cached;
        assert(offcache_prepare_batch(stream,selected,3));
        assert(offcache_prepare_batch(stream,selected,3));
        cached = offcache_name_for(argv[2]);
        mail_close(stream);
        offcache_load_all();
        stream = mail_open(NULL,cached,OP_SILENT | OP_READONLY);
        assert(stream && offcache_is_stream(stream));
        {
            char *info = offcache_attachment_info(stream,1);
            assert(info && strstr(info,"payload.bin") && strstr(info,"123456 encoded bytes"));
            fs_give((void **)&info);
        }
        assert(offcache_body_cached(stream,1) && offcache_body_cached(stream,2));
        assert(offcache_prepare_message(stream,1));
        fs_give((void **)&cached);
    }
    mail_close(stream);
    puts("PASS: direct/optional Njord transport, one-pass headers, attachment sizes and batched bodies");
    return 0;
}
