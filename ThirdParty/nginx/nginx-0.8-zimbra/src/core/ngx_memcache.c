/* asynchronous memcache module for nginx */

#include <ngx_memcache.h>

#define MC_INVALID_HASH ((ngx_uint_t) - 1)
#define MC_REQ_POOL_SIZE 1024

static int mc_sndbuf_len = 256 * 1024;

/* config-related function prototypes */
static char *ngx_memcache_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static void *ngx_memcache_create_conf(ngx_cycle_t *cycle);
static char *ngx_memcache_init_conf(ngx_cycle_t *cycle, void *conf);
static ngx_int_t ngx_memcache_init_process(ngx_cycle_t *cycle);
static char *
ngx_memcache_servers (ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

/* memcache protocol response handler prototype */
typedef ngx_int_t (*mcp_handler)
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed);

/* memcache protocol response processing functions */
static ngx_int_t ngx_memcache_process_add_ok
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed);
static ngx_int_t ngx_memcache_process_add_failed
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed);
static ngx_int_t ngx_memcache_process_delete_ok
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed);
static ngx_int_t ngx_memcache_process_delete_failed
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed);
static ngx_int_t ngx_memcache_process_get_ok
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed);
static ngx_int_t ngx_memcache_process_get_failed
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed);
static ngx_int_t ngx_memcache_process_incr_ok
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed);
static ngx_int_t ngx_memcache_process_incr_failed
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed);
static ngx_int_t ngx_memcache_process_decr_ok
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed);
static ngx_int_t ngx_memcache_process_decr_failed
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed);
static ngx_int_t ngx_memcache_chomp_bad_line
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed);
static ngx_int_t ngx_memcache_process_any_response
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed);

/* hashing functions to elect a memcached server for caching */
static ngx_uint_t ngx_memcache_hash (u_char *key, size_t len);
static ngx_uint_t ngx_memcache_perl_hash (u_char *key, size_t len);

/* generic event handler to read any memcache response */
static void ngx_memcache_any_read_handler (ngx_event_t *rev);
static void ngx_memcache_dummy_write_handler (ngx_event_t *wev);
static void ngx_memcache_reconnection_handler (ngx_event_t *ev);

/* Workqueue and connection maintenance functions */
static inline mc_workqueue_t *ngx_memcache_wq_front (mc_workqueue_t *head);
static inline ngx_int_t ngx_memcache_wq_isempty (mc_workqueue_t *head);
static void ngx_memcache_purge_connection_workqueue (mc_context_t *mcctx, mc_response_code_t res);
static void ngx_memcache_reestablish_connection (mc_context_t *mcctx);
static inline void ngx_memcache_prepare_reconnection (mc_context_t *mcctx);

static ngx_command_t ngx_memcache_commands[] =
{
    { ngx_string("memcache"),
      NGX_MAIN_CONF|NGX_CONF_BLOCK|NGX_CONF_NOARGS,
      ngx_memcache_block,
      0,
      0,
      NULL },

    { ngx_string("servers"),
      NGX_DIRECT_CONF|NGX_MEMCACHE_CONF|NGX_CONF_1MORE,
      ngx_memcache_servers,
      0,
      0,
      NULL },

    { ngx_string("timeout"),
      NGX_DIRECT_CONF|NGX_MEMCACHE_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      0,
      offsetof(ngx_memcache_conf_t, timeout),
      NULL },

    { ngx_string("reconnect"),
      NGX_DIRECT_CONF|NGX_MEMCACHE_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      0,
      offsetof(ngx_memcache_conf_t, reconnect),
      NULL },

    { ngx_string("ttl"),
      NGX_DIRECT_CONF|NGX_MEMCACHE_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      0,
      offsetof(ngx_memcache_conf_t, ttl),
      NULL },

    { ngx_string("allow_unqualified"),
      NGX_DIRECT_CONF|NGX_MEMCACHE_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      0,
      offsetof(ngx_memcache_conf_t, allow_unqualified),
      NULL },

    ngx_null_command
};

static ngx_core_module_t ngx_memcache_module_ctx =
{
    ngx_string("memcache"),
    ngx_memcache_create_conf,
    ngx_memcache_init_conf
};

ngx_module_t ngx_memcache_module =
{
    NGX_MODULE_V1,
    &ngx_memcache_module_ctx,           /* module context */
    ngx_memcache_commands,              /* module directives */
    NGX_CORE_MODULE,                    /* module type */
    NULL,                               /* init master */
    NULL,                               /* init module */
    ngx_memcache_init_process,          /* init process */
    NULL,                               /* init thread */
    NULL,                               /* exit thread */
    NULL,                               /* exit process */
    NULL,                               /* exit master */
    NGX_MODULE_V1_PADDING
};

static void *ngx_memcache_create_conf(ngx_cycle_t *cycle)
{
    ngx_memcache_conf_t *mcf;
    ngx_pool_t          *pool;
    ngx_log_t           *log;

    log = cycle->log;
    pool = ngx_create_pool (8 * ngx_pagesize, cycle->log);

    mcf = ngx_pcalloc (pool, sizeof(ngx_memcache_conf_t));
    if (mcf == NULL) {
        return NGX_CONF_ERROR;
    }

    mcf->cpool = pool;
    mcf->log = log;

    if(ngx_array_init (&mcf->servers, mcf->cpool, 4, sizeof(ngx_addr_t*))
        != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    mcf->timeout = NGX_CONF_UNSET_MSEC;
    mcf->reconnect = NGX_CONF_UNSET_MSEC;
    mcf->ttl = NGX_CONF_UNSET_MSEC;
    mcf->allow_unqualified = NGX_CONF_UNSET;

    ngx_log_error(NGX_LOG_DEBUG_CORE,cycle->log,0,
        "memcache - created configuration:%p",mcf);
    return mcf;
}

static char *ngx_memcache_init_conf(ngx_cycle_t *cycle, void *conf)
{
    ngx_memcache_conf_t *mcf = conf;

    ngx_conf_init_msec_value(mcf->timeout, 5000);
    ngx_conf_init_msec_value(mcf->reconnect, 60000);
    ngx_conf_init_msec_value(mcf->ttl, 0);
    ngx_conf_init_value(mcf->allow_unqualified, 0);

    ngx_log_error(NGX_LOG_DEBUG_CORE,cycle->log, 0,
        "memcache - initialized config defaults:%p", mcf);
    return NGX_CONF_OK;
}

/* per-process initialization routine */
static ngx_int_t ngx_memcache_init_process(ngx_cycle_t *cycle)
{
    ngx_memcache_conf_t     *mcf;
    ngx_log_t               *log;
    ngx_pool_t              *pool;
    ngx_addr_t              *peer,
                           **peers;
    ngx_peer_connection_t   *peercxn;
    mc_context_t            *mcctx;
    mc_workqueue_t          *mcwq;
    ngx_int_t                rc;
    ngx_uint_t               npeers,i;
    ngx_buf_t               *buff;

    mcf = (ngx_memcache_conf_t*) ngx_get_conf(cycle->conf_ctx, ngx_memcache_module);
    log = cycle->log;
    // pool = ngx_create_pool(8*ngx_pagesize,log);
    pool = mcf->cpool;

    npeers = mcf->servers.nelts;
    peers = (ngx_addr_t **)mcf->servers.elts;

    rc = ngx_array_init(&mcf->contexts, pool, npeers > 0 ? npeers : 1, sizeof(mc_context_t));
    if (rc != NGX_OK) {
        return rc;
    }

    for ( i = 0; i < npeers; ++i)
    {
        peer = peers[i];
        peercxn = ngx_pcalloc(pool, sizeof(ngx_peer_connection_t));
        peercxn->sockaddr = peer->sockaddr; /* XXX peer->sockaddr is on cf->pool */
        peercxn->socklen = peer->socklen;
        peercxn->name = &peer->name;        /* XXX peer->name is on cf->pool */
        peercxn->get = ngx_event_get_peer;
        peercxn->log = log;
        peercxn->log_error = NGX_ERROR_ERR;

        rc = ngx_event_connect_peer(peercxn);

        if (rc == NGX_ERROR || rc == NGX_BUSY || rc == NGX_DECLINED) {
            ngx_log_error (NGX_LOG_ERR, log, 0, 
                "cannot connect to memcached server %V (rc:%d)",
                &peer->name, rc);

            if (peercxn->connection) {
                ngx_close_connection (peercxn->connection);
                continue;
            }
        }

        /* nginx sockets are non-blocking, so connect() returns EINPROGRESS */
        peercxn->connection->read->handler = ngx_memcache_any_read_handler;
        peercxn->connection->write->handler = ngx_memcache_dummy_write_handler;

        mcctx = ngx_array_push (&mcf->contexts);
        ngx_memzero (mcctx, sizeof (mc_context_t));
        buff = ngx_create_temp_buf (pool, ngx_pagesize);    /* circular buffer */
        mcctx->readbuffer = buff;

        /* reconnection event */
        mcctx->reconnect_ev = ngx_palloc (pool, sizeof (ngx_event_t));
        ngx_memzero (mcctx->reconnect_ev, sizeof (ngx_event_t));
        mcctx->reconnect_ev->handler = ngx_memcache_reconnection_handler;
        mcctx->reconnect_ev->log = log;

        /* Initialize the head of the work queue doubly-linked list */
        mcwq = &mcctx->wq_head;
        mcwq->w.request_code = mcreq_noop;
        mcwq->pool = pool;
        mcwq->prev = mcwq;
        mcwq->next = mcwq;

        peercxn->connection->data = mcctx;
        peercxn->connection->log = log;

        mcctx->srvconn  = peercxn;
        mcctx->srvaddr  = peer;
        mcctx->status   = mcchan_good;
        mcctx->timeout  = mcf->timeout;
        mcctx->cxn_interval = mcf->reconnect;
        setsockopt(peercxn->connection->fd, SOL_SOCKET, SO_SNDBUF,
	    (void *) &mc_sndbuf_len, sizeof (mc_sndbuf_len));
    }

    ngx_log_error(NGX_LOG_INFO, log, 0,
        "memcache: %d/%d connections initialized",
        mcf->contexts.nelts, mcf->servers.nelts);

    return NGX_OK;
}

static char *ngx_memcache_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_conf_t  ocf;
    char        *rc;

    ocf = *cf;

    cf->ctx = cf->cycle->conf_ctx;
    cf->module_type = NGX_CORE_MODULE;
    cf->cmd_type = NGX_MEMCACHE_CONF;

    rc = ngx_conf_parse(cf, NULL);

    *cf = ocf;

    return rc;
}

static char *
ngx_memcache_servers (ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_memcache_conf_t     *mcf = conf;
    ngx_addr_t              **server;
    ngx_uint_t               i;
    ngx_url_t                u;

    for (i = 1; i < cf->args->nelts; ++i)
    {
        server = ngx_array_push(&mcf->servers);
        if (server == NULL) {
            return NGX_CONF_ERROR;
        }

        *server = NULL;

        ngx_memzero(&u, sizeof(u));
        u.url = ((ngx_str_t *)cf->args->elts)[i];
        u.default_port = 11211;
        u.uri_part = 1;
        u.one_addr = 1;

        /* note - since ngx_parse_url uses pools from cf, therefore all address
           structures in *server will be allocated on the config pool instead
           of the memcached pool
         */
        if (ngx_parse_url(cf->pool, &u) != NGX_OK) {
            if (u.err) {
                ngx_log_error(NGX_LOG_ERR, cf->cycle->log, 0,
                    "%s in memcache:server %V", u.err, &u.url);
            }
            return NGX_CONF_ERROR;
        }

        *server = u.addrs;
    }

    return NGX_CONF_OK;
}

/* Work queue manipulation functions */
static inline mc_workqueue_t *ngx_memcache_wq_front (mc_workqueue_t *head)
{
    return head->next;
}

mc_workqueue_t *ngx_memcache_wq_enqueue (mc_workqueue_t *head, mc_workqueue_t *wqe)
{
    wqe->prev = head->prev;
    wqe->next = head;
    wqe->prev->next = wqe;
    wqe->next->prev = wqe;
    return wqe;
}

mc_workqueue_t *ngx_memcache_wq_dequeue (mc_workqueue_t *head)
{
    mc_workqueue_t  *wqe;

    wqe = head->next;

    if (wqe != head)
    {
        wqe->prev->next = wqe->next;
        wqe->next->prev = wqe->prev;
        wqe->prev = NULL;
        wqe->next = NULL;
    }

    return wqe;
}

static inline ngx_int_t ngx_memcache_wq_isempty (mc_workqueue_t *head)
{
    return (ngx_memcache_wq_front(head) == head);
}

/* Post a memcached request onto the workqueue of a memcached server 
   w    work request describing the task
        (also contains on_success/on_failure handlers)
   k    opaque key which will be used for calculating the server hash
   pdu  the data which should be sent to the memcached server
   p    the pool from which additional memory may be allocated as needed
   l    log object for debug/informational messages
 */
void ngx_memcache_post (
     mc_work_t      *w,
     ngx_str_t       k,
     ngx_str_t       pdu,
     ngx_pool_t     *p,
     ngx_log_t      *l
    )
{
    ngx_uint_t       h;
    size_t           t;
    ssize_t          n;
    mc_context_t    *mcctx;
    mc_workqueue_t  *r;
    ngx_memcache_conf_t  *mcf;
    mc_context_t    *contexts;
    ngx_flag_t      locked = 0;
    ngx_flag_t      reclaim = 0;

    mcf = (ngx_memcache_conf_t *)ngx_get_conf(ngx_cycle->conf_ctx, ngx_memcache_module);
    contexts = (mc_context_t *)mcf->contexts.elts;
    h = ngx_memcache_hash(k.data, k.len);

    if (h == MC_INVALID_HASH)
    {
        ngx_log_error (NGX_LOG_ERR, l, 0,
            "no memcache server available, cannot post request");
        w->response_code = mcres_failure_unavailable;
        w->on_failure(w);
        return;
    }

    ngx_log_debug1 (NGX_LOG_DEBUG_CORE, l, 0,
        "posting memcache request to cache server #%d", h);
    mcctx =  contexts + h;

    if (p == NULL) {
        p = ngx_create_pool(MC_REQ_POOL_SIZE, l);
        if (p == NULL) {
            w->response_code = mcres_failure_unavailable;
            w->on_failure(w);
            return;
        }
        reclaim = 1;
    }

    /* build up the request to enqueue on the workqueue
       we build up the request earlier on, so that if 
       memory errors occur, we would not have posted a pdu
       on the memcached channel that we cannot handle
     */

    r = ngx_pcalloc(p, sizeof(mc_workqueue_t));
    if (r == NULL) {
        w->response_code = mcres_failure_unavailable;
        w->on_failure(w);
        return;
    }

    if (ngx_log_tid && mcctx->lock != ngx_log_tid) {
        ngx_spinlock(&mcctx->lock, ngx_log_tid, 40);
        locked = 1;
    }
    t = 0;
    while (t < pdu.len)
    {
        n = ngx_send (mcctx->srvconn->connection, pdu.data + t, pdu.len - t);
        if (n > 0) {
            t += n;
            if (mcctx->status == mcchan_reconnect) {
                mcctx->status = mcchan_good;
            }
        } else {
            if (locked)
                ngx_unlock(&mcctx->lock);
            ngx_log_debug0 (NGX_LOG_DEBUG_CORE, l, 0,
                "error sending pdu to memcache server");
            mcctx->status = mcchan_bad;
            ngx_memcache_purge_connection_workqueue
                            (mcctx, mcres_failure_again);
            ngx_memcache_prepare_reconnection(mcctx);
            w->response_code = mcres_failure_again;
            w->on_failure(w);
            return;
        }
    }

    if (t == pdu.len)
    {
        /* set the read timeout on the server channel *only* if there is no 
           outstanding timer set already (this is to opportunistically catch
           any responses before the stipulated timeout
         */

        if (!mcctx->srvconn->connection->read->timer_set) {
            ngx_add_timer (mcctx->srvconn->connection->read, mcctx->timeout);
        }
    }

    r->pool = p;
    r->reclaim = reclaim;
    r->w = *w;
    r->w.response_code = mcres_unknown;

    ngx_memcache_wq_enqueue (&mcctx->wq_head, r);

    if (locked)
        ngx_unlock(&mcctx->lock);
    ngx_log_debug2 (NGX_LOG_DEBUG_CORE, l, 0, "posted request(%p) on server #%d",
                    r, h);

    return;
}

/* memcache protocol handling routines */

/* process successful add response */
static ngx_int_t ngx_memcache_process_add_ok
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed)
{
    size_t minimum_len;

    minimum_len = sizeof ("STORED") - 1 +
                  sizeof (CRLF) - 1;

    *consumed = 0;

    if (size < minimum_len)
    {
        return NGX_AGAIN;
    }

    if (ngx_memcmp (stream, "STORED" CRLF, sizeof("STORED" CRLF) - 1))
    {
        return ngx_memcache_chomp_bad_line(stream, size, wq, consumed);
    }

    *consumed = sizeof ("STORED" CRLF) - 1;
    wq->w.response_code = mcres_success;

    return NGX_OK;
}

/* process unsuccessful add response */
static ngx_int_t ngx_memcache_process_add_failed
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed)
{
    size_t minimum_len;

    minimum_len = sizeof ("NOT_STORED") - 1 +
                  sizeof (CRLF) - 1;

    *consumed = 0;

    if (size < minimum_len)
    {
        return NGX_AGAIN;
    }

    if (ngx_memcmp (stream, "NOT_STORED" CRLF, sizeof("NOT_STORED" CRLF) - 1))
    {
        return ngx_memcache_chomp_bad_line(stream,size,wq,consumed);
    }

    *consumed = sizeof ("NOT_STORED" CRLF) - 1;
    wq->w.response_code = mcres_failure_normal;

    return NGX_OK;
}

/* process successful delete response */
static ngx_int_t ngx_memcache_process_delete_ok
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed)
{
    size_t minimum_len;

    minimum_len = sizeof ("DELETED") - 1 +
                  sizeof (CRLF) - 1;

    *consumed = 0;

    if (size < minimum_len)
    {
        return NGX_AGAIN;
    }

    if (ngx_memcmp (stream, "DELETED" CRLF, sizeof("DELETED" CRLF) - 1))
    {
        return ngx_memcache_chomp_bad_line(stream,size,wq,consumed);
    }

    *consumed = sizeof ("DELETED" CRLF) - 1;
    wq->w.response_code = mcres_success;

    return NGX_OK;
}

/* handle unsuccessful delete response */
static ngx_int_t ngx_memcache_process_delete_failed
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed)
{
    size_t minimum_len;

    minimum_len = sizeof ("NOT_FOUND") - 1 +
                  sizeof (CRLF) - 1;

    *consumed = 0;

    if (size < minimum_len)
    {
        return NGX_AGAIN;
    }

    if (ngx_memcmp (stream, "NOT_FOUND" CRLF, sizeof("NOT_FOUND" CRLF) - 1))
    {
        return ngx_memcache_chomp_bad_line(stream,size,wq,consumed);
    }

    *consumed = sizeof ("NOT_FOUND" CRLF) - 1;
    wq->w.response_code = mcres_failure_normal;

    return NGX_OK;
}

/* process successful get response */
static ngx_int_t ngx_memcache_process_get_ok
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed)
{
    /* A successful `get' response looks like:

       VALUE keyname flag size
       <size bytes of data>
       END

       A successful `get' response must be at least as long as:

       VALUE K F S
       b
       END
     */

    enum {
                rr_value_read,
                rr_key_read,
                rr_flag_read,
                rr_size_read,
                rr_data_read,
                rr_end_read
    } state;

    size_t      minimum_len;
    u_char      *p;
    size_t      tokenpos;
    ngx_str_t   key = ngx_string ("");
    ngx_str_t   value = ngx_string ("");
    size_t      value_size;
    uint16_t    flag;

    minimum_len =
                sizeof ("VALUE ") - 1 +
                sizeof ("k ") - 1 + 
                sizeof ("0 ") - 1 +
                sizeof ("0 ") - 1 +
                sizeof (CRLF) - 1 +
                sizeof (CRLF) - 1 +
                sizeof ("END") - 1 +
                sizeof (CRLF) -1;

    p = stream;
    *consumed = 0;
    value_size = 0;

    /* If there is not enough space to even hold a bare minimum response,
     then we cannot proceed */

    if (size < minimum_len)
    {
        return NGX_AGAIN;
    }

    if (ngx_memcmp (stream, "VALUE ", sizeof ("VALUE ") - 1))
    {
        return ngx_memcache_chomp_bad_line(stream,size,wq,consumed);
    }

    p += (sizeof ("VALUE ") - 1);
    *consumed += (sizeof ("VALUE ") - 1);
    state = rr_value_read;

    for (;(p < stream + size) && (state != rr_end_read); ++p, ++*consumed)
    {
        switch (state)
        {
            case rr_value_read:
                /* now we're expecting the key name */
                tokenpos = *consumed;

                while ((p < stream + size) && (*p != ' '))
                {
                    ++p;
                    ++*consumed;
                }

                if (p == stream + size)
                {
                    /* end of stream */
                    --p; --*consumed;
                }
                else
                {
                    /* The key is constructed */
                    key.data = stream + tokenpos;
                    key.len = *consumed - tokenpos;

                    state = rr_key_read;
                }

                break;

            case rr_key_read:
                /* now we're expecting a flag, which is a short integer
                   which corresponds to the `flags' argument to mc_add
                   To avoid looping more than is necessary, we'll play a 
                   simple trick, which is to initialize the flag to 0, 
                   and then keep *10 + the current digit encountered
                 */

                flag = 0;

                while ((p < stream + size) && (*p != ' '))
                {
                   flag = (flag * 10) + *p;
                   ++p;
                   ++*consumed;
                }

                if (p == stream + size)
                {
                   --p; --*consumed;
                }
                else
                {
                   /* The flag is constructed */

                   state = rr_flag_read;
                }

                break;

            case rr_flag_read:
                /* now we're expecting the size of the data 
                 */
                value_size = 0;

                while ((p < stream + size)
                       && (*p != ' ')      /* wasteful ? */
                       && (*p != CR)
                       && (*p != LF)       /* wasteful ? */
                      )
                {
                   value_size = (value_size * 10) + (*p - '0');
                   ++p;
                   ++*consumed;
                }

                if (p == stream + size)
                {
                   --p; --*consumed;
                }
                else
                {
                   /* The size is constructed */
                   state = rr_size_read;
                }

                break;

            case rr_size_read:
                /* now we're looking out for the data
                   the '\r' has already been consumed in the for loop
                   so we must look beyond the '\n', and then read 
                   value_size bytes more from the stream
                */

                while ((p < stream + size) && (*p != LF))
                {
                    ++p; ++*consumed;
                }

                if (p == stream + size)
                {
                    --p; --*consumed;
                }
                else
                {
                    /* now p is pointing at the line feed preceding the data */

                    ++p; ++*consumed;

                    if (p == stream + size)
                    {
                        --p; --*consumed;
                    }
                    else
                    {
                        minimum_len = 
                            value_size +
                            sizeof (CRLF) - 1 +
                            sizeof ("END") - 1 +
                            sizeof (CRLF) - 1;


                        if (size - *consumed < minimum_len)
                        {
                            /* request cannot be completed here */
                        }
                        else
                        {
                            /* we have the value, starting at p */
                            value.data = p;
                            value.len = value_size;

                            /* now just advance in one shot till the end */

                            p+= value_size; *consumed += value_size;

                            /* And now, just consume the following CR too
                               so that the END context lands bingo at END
                             */

                            state = rr_data_read;

                            if (*p == CR)     /* this is superfluous */
                            {
                                ++p; ++*consumed;
                            }
                        }
                    }
                }

                break;

            case rr_data_read:
                /* we know that sufficient bytes are present, and that we
                   should be looking at "END"
                 */

                if (ngx_memcmp (p, "END" CRLF, sizeof ("END" CRLF) - 1))
                {
                   /* not possible. try logging here */
                }
                else
                {
                   state = rr_end_read;

                   p += (sizeof ("END") - 1);
                   *consumed += (sizeof ("END") - 1);

                   /* again, we will consume the CR so that we break out at
                      once */

                   if (*p == CR)    /* superfluous */
                   {
                       ++p;
                       ++*consumed;
                   }
                }

                break;

             default:
                break;
        }
    }

    if (state != rr_end_read)
    {
        return NGX_AGAIN;       /* This means there wasn't enough data */
    }

    /* we've finished processing the get response */

    wq->w.payload.data = ngx_pstrdup (wq->pool, &value);
    wq->w.payload.len = value.len;
    wq->w.response_code = mcres_success;

    return NGX_OK;
}

/* process unsuccessful get response */
static ngx_int_t ngx_memcache_process_get_failed
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed)
{
    size_t minimum_len;

    minimum_len = sizeof ("END") - 1 +
                  sizeof (CRLF) - 1;

    *consumed = 0;

    if (size < minimum_len)
    {
        return NGX_AGAIN;
    }

    if (ngx_memcmp (stream, "END" CRLF, sizeof("END" CRLF) - 1))
    {
        return ngx_memcache_chomp_bad_line(stream,size,wq,consumed);
    }

    *consumed = sizeof ("END" CRLF) - 1;
    wq->w.response_code = mcres_failure_normal;

    return NGX_OK;
}

static ngx_int_t ngx_memcache_process_incr_ok
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed)
{
    u_char *p;

    p = stream;

    while ((p < stream + size) && (*p != CR) && (*p != LF))
    {
        if (!(*p >= '0' && *p <= '9')) {
            return ngx_memcache_chomp_bad_line(stream, size, wq, consumed);
        }

        ++p;
        ++*consumed;
    }

    if (p == stream + size) {
        return NGX_AGAIN;
    } else {
        if (size - *consumed >= 2)
        {
            if (*p == CR && *(p + 1) == LF)
            {
                *consumed += 2;
                wq->w.payload.len = p - stream;
                wq->w.payload.data = ngx_palloc(wq->pool, wq->w.payload.len);
                ngx_memcpy (wq->w.payload.data,stream, wq->w.payload.len);
                wq->w.response_code = mcres_success;
                return NGX_OK;
            }
            else
            {
                return ngx_memcache_chomp_bad_line(stream, size, wq, consumed);
            }
        }
        else
        {
            return NGX_AGAIN;
        }
    }
}

static ngx_int_t ngx_memcache_process_incr_failed
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed)
{
    size_t minimum_len;

    minimum_len = sizeof ("NOT_FOUND") - 1 +
                  sizeof (CRLF) - 1;

    *consumed = 0;

    if (size < minimum_len)
    {
        return NGX_AGAIN;
    }

    if (ngx_memcmp (stream, "NOT_FOUND" CRLF, sizeof ("NOT_FOUND" CRLF) - 1))
    {
        return ngx_memcache_chomp_bad_line(stream, size, wq, consumed);
    }

    *consumed = sizeof ("NOT_FOUND" CRLF) - 1;
    wq->w.response_code = mcres_failure_normal;

    return NGX_OK;
}

static ngx_int_t ngx_memcache_process_decr_ok
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed)
{
    u_char *p;

    p = stream;

    while ((p < stream+size) && (*p != CR) && (*p != LF))
    {
        if (!(*p>='0' && *p<='9')) {
            return ngx_memcache_chomp_bad_line(stream, size, wq, consumed);
        }

        ++p;
        ++*consumed;
    }

    if (p == stream + size) {
        return NGX_AGAIN;
    } else {
        if (size - *consumed >= 2)
        {
            if (*p == CR && *(p+1) == LF)
            {
                *consumed += 2;
                wq->w.payload.len = p-stream;
                wq->w.payload.data = ngx_palloc(wq->pool,wq->w.payload.len);
                ngx_memcpy (wq->w.payload.data,stream,wq->w.payload.len);
                wq->w.response_code = mcres_success;
                return NGX_OK;
            }
            else
            {
                return ngx_memcache_chomp_bad_line(stream, size, wq, consumed);
            }
        }
        else
        {
            return NGX_AGAIN;
        }
    }
}

static ngx_int_t ngx_memcache_process_decr_failed
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed)
{
    size_t minimum_len;

    minimum_len = sizeof ("NOT_FOUND") - 1 +
                  sizeof (CRLF) - 1;

    *consumed = 0;

    if (size < minimum_len)
    {
        return NGX_AGAIN;
    }

    if (ngx_memcmp (stream, "NOT_FOUND" CRLF, sizeof ("NOT_FOUND" CRLF) - 1))
    {
        return ngx_memcache_chomp_bad_line(stream, size, wq, consumed);
    }

    *consumed = sizeof ("NOT_FOUND" CRLF) - 1;
    wq->w.response_code = mcres_failure_normal;

    return NGX_OK;
}

/* consume a line of unknown (unexpected) memcached response */
static ngx_int_t ngx_memcache_chomp_bad_line
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed)
{
    u_char *p;

    p = stream;
    *consumed = 0;

    while ((p < stream + size) && (*p != LF))
    {
        ++p; ++*consumed;
    }

    if (p < stream + size)
    {
        ++p;
        ++*consumed;
    }

    return NGX_ERROR;
}

/* process response to any memcached command 
   with the advent of more supported operations, we need more context in 
   order to process the response to a previously submitted command
   that is because responses to some memcached commands are identical

   see docs/MEMCACHE-PROTOCOL for details
 */
ngx_int_t ngx_memcache_process_any_response
    (u_char *stream, size_t size, mc_workqueue_t *wq, size_t *consumed)
{
    mcp_handler         handler;
    mc_request_code_t   op;

    *consumed = 0;
    if (size == 0) { return NGX_AGAIN; }

    op = wq->w.request_code;

    switch (op)
    {
        case mcreq_add:
            switch (*stream)
            {
                case 'S':   /* STORED */
                    handler = ngx_memcache_process_add_ok;
                    break;
                case 'N':   /* NOT_STORED */
                    handler = ngx_memcache_process_add_failed;
                    break;
                default:
                    handler = ngx_memcache_chomp_bad_line;
                    break;
            }
            break;
        case mcreq_get:
            switch (*stream)
            {
                case 'V':   /* VALUE */
                    handler = ngx_memcache_process_get_ok;
                    break;
                case 'E':   /* END */
                    handler = ngx_memcache_process_get_failed;
                    break;
                default:
                    handler = ngx_memcache_chomp_bad_line;
                    break;
            }
            break;
        case mcreq_delete:
            switch (*stream)
            {
                case 'D':   /* DELETED */
                    handler = ngx_memcache_process_delete_ok;
                    break;
                case 'N':   /* NOT_FOUND */
                    handler = ngx_memcache_process_delete_failed;
                    break;
                default:
                    handler = ngx_memcache_chomp_bad_line;
                    break;
            }
            break;
        case mcreq_incr:
            if (*stream == 'N') /* NOT_FOUND */ {
                handler = ngx_memcache_process_incr_failed;
            } else {
                handler = ngx_memcache_process_incr_ok;
            }
            break;
        case mcreq_decr:
            if (*stream == 'N') /* NOT_FOUND */ {
                handler = ngx_memcache_process_decr_failed;
            } else {
                handler = ngx_memcache_process_decr_ok;
            }
            break;
        default:
            handler = ngx_memcache_chomp_bad_line;
            break;
    }

    return handler (stream, size, wq, consumed);
}

static void ngx_memcache_dummy_write_handler (ngx_event_t *wev)
{
    ngx_log_debug0(NGX_LOG_DEBUG_CORE, wev->log, 0,
                   "dummy memcache write-event handler");
}

/* Generic memcache response event handler (see docs/MEMCACHE-PROTOCOL) */
static void ngx_memcache_any_read_handler(ngx_event_t *rev)
{
    ngx_connection_t                *c;
    mc_context_t                    *ctx;
    size_t                           available, consumed;
    ssize_t                          n;
    ngx_buf_t                       *readbuffer;
    /* volatile */ mc_workqueue_t   *wq_head;
    mc_workqueue_t                  *wq_entry;
    ngx_int_t                        rc;
    mc_chain_handler                 success, failure;
    ngx_flag_t                       reclaim;

    c           = rev->data;
    ctx         = c->data;
    readbuffer  = ctx->readbuffer;
    wq_head     = &ctx->wq_head;

    ngx_log_debug1 (NGX_LOG_DEBUG_CORE, ngx_cycle->log, 0,
                "memcached read event:%V", ctx->srvconn->name);

    if (ctx->status == mcchan_bad) {
        ngx_log_error (NGX_LOG_ERR, ngx_cycle->log, 0,
                "ngx_memcache_any_read_handler should always be "
                "callback when channel status is \"good\" or "
                "\"reconnect\"");
        return;
    }

    /* nginx buffer

       [[s]......[p]......[l].......[e]]

       s = start
       p = pos
       l = last
       e = end

     */

    /* TODO Why here is not locked ??? */
    available = readbuffer->end - readbuffer->last;

    if (available == 0)
    {
        /* no space in circular buffer to read the responses */

        ngx_log_debug1 (NGX_LOG_DEBUG_CORE, ngx_cycle->log, 0, 
            "recycle circular buffer:%d bytes", 
            readbuffer->pos - readbuffer->start
        );

        memmove (readbuffer->start, readbuffer->pos,
                    readbuffer->last - readbuffer->pos);

        readbuffer->last -= (readbuffer->pos - readbuffer->start);
        readbuffer->pos = readbuffer->start;

        available = readbuffer->end - readbuffer->last;

        if (available == 0)
        {
            ngx_log_error (NGX_LOG_CRIT, ngx_cycle->log, 0,
                "cannot recycle circular buffer");

            /* TODO how to recover ? */
            return;
        }

        /* note: recursive call has been removed */
    }

    if (ngx_log_tid && ctx->lock == ngx_log_tid) {
        ngx_log_error (NGX_LOG_CRIT, ngx_cycle->log, 0,
            "memcached loop");
        return;
    } else {
        ngx_spinlock(&ctx->lock, ngx_log_tid, 40);
    }

    if (rev->timedout)
    {
        /* read timed out */
        ngx_log_error (NGX_LOG_INFO, ngx_cycle->log, 0,
            "channel:%V timed out", ctx->srvconn->name);

        ctx->status = mcchan_bad;
        ngx_memcache_purge_connection_workqueue (ctx, mcres_failure_again);
        ngx_memcache_prepare_reconnection (ctx); // schedule reconnection event

        ngx_unlock(&ctx->lock);
        return;
    }

    n = ngx_recv (c, readbuffer->last, available);

    if (rev->eof)
    {
        /* recv() zero bytes implies peer orderly shutdown (recv(2)) */
        ngx_log_error (NGX_LOG_NOTICE, ngx_cycle->log, 0,
            "channel:%V orderly shutdown", ctx->srvconn->name);

        /* channel was hitherto good, don't reconnect immediately, rather
           wait for cxn_interval before reconnecting
         */
        ctx->status = mcchan_bad;
        ngx_memcache_purge_connection_workqueue (ctx, mcres_failure_again);
        ngx_log_debug2 (NGX_LOG_DEBUG_MAIL, ngx_cycle->log, 0,
            "bad channel:%V, reconnect:%d ms",
            ctx->srvconn->name, ctx->cxn_interval);
        ngx_memcache_prepare_reconnection(ctx);

        if (rev->timer_set) {
            ngx_del_timer (rev);
        }
        ngx_unlock(&ctx->lock);
        return;

    }
    else if (n == NGX_AGAIN)
    {
        /* EAGAIN should have resulted in a timeout which has been 
           handled previously
         */
        ngx_log_error (NGX_LOG_WARN, ngx_cycle->log, 0, 
            "ignoring *AGAIN on channel %V", ctx->srvconn->name);

        ngx_unlock(&ctx->lock);
        return;

    }
    else if (n == NGX_ERROR)
    {
        /* After trying to reconnection, and then reach here, it indicates
         * that the reconnection fails because target server is not available
         */
        if (ctx->status == mcchan_reconnect) {
            ctx->status = mcchan_bad;
            ngx_log_error (NGX_LOG_ERR, ngx_cycle->log, 0,
                    "reconnect to channel %V fails", ctx->srvconn->name);
            ngx_memcache_prepare_reconnection(ctx);
            ngx_unlock(&ctx->lock);
            return;
        }
        /* There was an error reading from this socket */
        ngx_log_debug1 (NGX_LOG_DEBUG_CORE, ngx_cycle->log, 0,
            "channel %V:error in recv, purging queue",
            ctx->srvconn->name
            );
        ctx->status = mcchan_bad;
        ngx_memcache_purge_connection_workqueue (ctx, mcres_failure_again);
        ngx_log_debug2 (NGX_LOG_DEBUG_MAIL, ngx_cycle->log, 0,
            "bad channel:%V, reconnect:%d ms",
            ctx->srvconn->name, ctx->cxn_interval);
        ngx_memcache_prepare_reconnection(ctx);
        if (rev->timer_set) {
            ngx_del_timer (rev);
        }
        ngx_unlock(&ctx->lock);
        return;
    }

    ngx_log_debug2 (NGX_LOG_DEBUG_CORE, ngx_cycle->log, 0,
                "channel %V:read %d bytes", ctx->srvconn->name, n);

    readbuffer->last += n;

    if (ngx_memcache_wq_isempty (wq_head))
    {
        ngx_unlock(&ctx->lock);
        ngx_log_error (NGX_LOG_WARN, ngx_cycle->log, 0, 
          "channel %V:discard %d bytes(bad data)",
          ctx->srvconn->name, n);
        if (rev->timer_set) {
            ngx_del_timer (rev);
        }
        return;
    }

    while ((!ngx_memcache_wq_isempty (wq_head)) && ((readbuffer->last - readbuffer->pos) > 0))
    {
        wq_entry = ngx_memcache_wq_front (wq_head);
        consumed = 0;

        rc = ngx_memcache_process_any_response (
                readbuffer->pos,
                readbuffer->last - readbuffer->pos,
                wq_entry,
                &consumed);

        ngx_log_debug2 (NGX_LOG_DEBUG_CORE, ngx_cycle->log, 0,
            "memcache proto-handler consumed:%d,rc:%d", consumed, rc
            );

        if (rc == NGX_OK)
        {
            readbuffer->pos += consumed;

            /* correct response, so dequeue entry */
            wq_entry = ngx_memcache_wq_dequeue (wq_head);

            ngx_log_debug1 (NGX_LOG_DEBUG_CORE, ngx_cycle->log, 0,
                "proto-handler ok, dequeue request (%p)",
                wq_entry);

            if (wq_entry->w.response_code == mcres_success) {
                ngx_log_debug1 (NGX_LOG_DEBUG_CORE, ngx_cycle->log, 0, 
                    "processing request (%p) succeeded", wq_entry
                );

                reclaim = wq_entry->reclaim;
                success = wq_entry->w.on_success;
                success (&wq_entry->w);
            } else {
                ngx_log_debug1 (NGX_LOG_DEBUG_CORE, ngx_cycle->log, 0, 
                    "cache request (%p) failed", wq_entry
                );

                reclaim = wq_entry->reclaim;
                failure = wq_entry->w.on_failure;
                failure (&wq_entry->w);
            }

            if (reclaim) { // free the memory if allocated from memcache request pool
                ngx_destroy_pool(wq_entry->pool);
            }
        }
        else if (rc == NGX_ERROR)
        {
            ngx_log_error (NGX_LOG_NOTICE, ngx_cycle->log, 0,
                "channel:%V ignoring %d bad bytes",
                ctx->srvconn->name, consumed);
            readbuffer->pos += consumed;

            reclaim = wq_entry->reclaim;
            failure = wq_entry->w.on_failure;
            failure (&wq_entry->w);

            if (reclaim) { // free the memory if allocated from memcache request pool
                ngx_destroy_pool(wq_entry->pool);
            }

        }
        else if (rc == NGX_AGAIN)
        {
            /* The response handler has indicated that there isn't sufficient
               space to read the data, so we must move bytes over to the start
             */

            ngx_log_debug1 (NGX_LOG_DEBUG_CORE, ngx_cycle->log, 0, 
                "proto-handler got partial response, recycling [pos=%d]",
                readbuffer->pos - readbuffer->start
            );

            memmove (readbuffer->start, readbuffer->pos,
                        readbuffer->last - readbuffer->pos);

            readbuffer->last -= (readbuffer->pos - readbuffer->start);
            readbuffer->pos = readbuffer->start;

            ngx_unlock(&ctx->lock);
            ngx_memcache_any_read_handler (rev);
            ngx_spinlock(&ctx->lock, ngx_log_tid, 40);

            break;
        }
    } /* while */

    if (rev->timer_set) {
        ngx_del_timer (rev);
    }

    if (!ngx_memcache_wq_isempty (wq_head)) {
        ngx_add_timer (rev, ctx->timeout);
    }

    ngx_unlock(&ctx->lock);
}

/* purge all outstanding connections from memcached work-queue */

static void ngx_memcache_purge_connection_workqueue (mc_context_t *mcctx, mc_response_code_t res)
{
    mc_workqueue_t             *head, *entry;
    mc_chain_handler            failure_handler;
    ngx_flag_t                  reclaim;
    ngx_uint_t                  count;

    head = &mcctx->wq_head;
    if (ngx_memcache_wq_isempty(head))
        return;

    /*the mcchannel status of current context has to be set as
      "mcchan_bad" before this function's invoke because the user
      in the upper layer may retry connection. If status is
      mcchan_good, the retry may incur an endless recursion */
    if (mcctx->status == mcchan_good)
        mcctx->status = mcchan_bad;

    ngx_log_debug1 (NGX_LOG_DEBUG_CORE, ngx_cycle->log, 0,
        "channel:%V, purging all entries",
        mcctx->srvconn->name);

    count = 0;
    while (!ngx_memcache_wq_isempty(head)) {
        entry = ngx_memcache_wq_dequeue (head);
        failure_handler = entry->w.on_failure;
        reclaim = entry->reclaim;
        entry->w.response_code = res;
        failure_handler (&entry->w);
        if (reclaim) {
            ngx_destroy_pool(entry->pool);
        }
        count++;
    }

    ngx_log_error (NGX_LOG_INFO, ngx_cycle->log, 0,
        "channel:%V, purged all %d entries",
        mcctx->srvconn->name, count);

    return;
}

/* This is the routine for re-establishing a memcache connection that has 
   broken down for some reason. Usually, we *expect* this to happen in 
   cases when a memcache server crashes.

   There are interesting effects observed on linux how a connected TCP 
   socket, which is marked in non-blocking mode (as nginx does), behaves 
   when the peer end goes down. 

   Firstly, when a connect() is invoked on a socket that's non-blocking, 
   the return code is EINPROGRESS even if the peer is up, or whether it's 
   down at the time (which is the reason why nginx's connection logic 
   in ngx_event_connect_peer explicitly checks for EINPROGRESS, and why we
   explicitly ignore it)

   Now, there are two very distinct fates of the subsequent send() and recv()
   calls on the socket, in case something goes(or went) wrong with the peer:

   (*) In the case when the peer was down at the time of connect(), then any
       send() and recv() immediately fails with ECONNREFUSED.

   (*) In the case when the peer went down at some point after the connect(), 
       then the first send() after this time *appears* to succeed, whereas
       subsequent send()s fail with EPIPE (broken pipe). recv(), however, as
       usual, succeeds, but with 0 bytes read

   What this means for us, is that if a memcache connection goes sour because
   of peer problems, then we can expect ngx_send()s to start failing with 
   EPIPE, whereas our ngx_recv(), which is indirectly invoked via the event
   handling mechanism, will receive 0 bytes from ngx_recv()

   In both cases, we need to purge the corresponding work queues ASAP, because
   these may contain pending cache-get requests. A pending cache-get request
   corresponds to a email connection freshly initiated by a client, and which 
   is requesting for upstream server information in order to initiate the proxy.
   This is a high priority request, because the proxy initiation is waiting for 
   the result of this operation.

   On the other hand, a pending cache-add request corresponds to a proxy session
   already initiated (and possibly, already finished), which just needs to cache
   the upstream info, which was previously retrieved from the http-auth servers.
   In this situation, no client is really waiting for the add operation to 
   complete, but the memory still needs to be freed. Hence, it's lower priority.

   So, purging a work queue is an action that is highest priority, and must be
   triggered whenever anything goes amiss with the ngx_send() or ngx_recv().

   On the other hand, we don't need to be very aggressive in trying to re-
   establish the connection to a broken peer, as long as we mark that memcache
   connection as `bad', so that our memcache server hashing algorithm is smart 
   enough to ignore the bad connection. 

   We can just maintain a counter, which will signify the number of times that 
   a connection could have been used, but was ignored because the server was
   down. This counter will start at 0, and will be incremented whenever any 
   ngx_send() or ngx_recv() failed, or whenever the hashing algorithm
   originally selected that channel, but selected another because the channel 
   was marked as `bad'. 

   A channel is `bad' when the counter is greater than zero, and it is good if
   the counter is 0. 

   The counter is incremented by one (and the channel is marked bad) whenever
   there is an error on ngx_send and/or ngx_recv, and it is also incremented by
   one whenever the hashing algorithm elects the channel, but finds it bad. 

   The counter is reset to zero (and the channel marked good), after we re-
   establish the connection. The re-establishment is attempted when the counter
   reaches the config-file-defined threshold. A threshold of 1 indicates 
   aggressive re-connection, whereas larger values signify more lethargic 
   attempts at re-connection

   (important note)
   As an alternative to the channel ageing algorithm used above, we can use
   a simple timeout to indicate how long nginx will do without a bad memcache
   connection before attempting to reconnect. Therefore, all references to 
   memcache channel age are superseded by the `reconnect interval'

 */

static void ngx_memcache_reestablish_connection (mc_context_t *mcctx)
{
    ngx_int_t       rc;

    if (mcctx->srvconn->connection) {
        ngx_close_connection (mcctx->srvconn->connection);
        mcctx->srvconn->connection = NULL;
    }

    ngx_memzero (mcctx->srvconn, sizeof (ngx_peer_connection_t));

    mcctx->srvconn->sockaddr    =  mcctx->srvaddr->sockaddr;
    mcctx->srvconn->socklen     =  mcctx->srvaddr->socklen;
    mcctx->srvconn->name        = &mcctx->srvaddr->name;
    mcctx->srvconn->get         =  ngx_event_get_peer;
    mcctx->srvconn->log         =  ngx_cycle->log;
    mcctx->srvconn->log_error   = NGX_ERROR_ERR;

    rc = ngx_event_connect_peer (mcctx->srvconn);

    if (mcctx->srvconn->connection)
    {
        mcctx->srvconn->connection->read->handler = ngx_memcache_any_read_handler;
        mcctx->srvconn->connection->write->handler = ngx_memcache_dummy_write_handler;
        mcctx->srvconn->connection->data = mcctx;
        mcctx->srvconn->connection->log = ngx_cycle->log;
    }

    if (rc == NGX_ERROR || rc == NGX_BUSY || rc == NGX_DECLINED) {
        ngx_log_error (NGX_LOG_WARN, ngx_cycle->log, 0, 
            "cannot re-establish connection to channel %V", 
            mcctx->srvconn->name);
        mcctx->status = mcchan_bad;
    } else {
        /* In this case, the connection may return NGX_AGAIN (-2).
         * If Then the read event is comming and recv return NGX_ERROR (-1),
         * the reconnection fails. Otherwise, the reconnection is successful.
         */
        ngx_log_error (NGX_LOG_NOTICE, ngx_cycle->log, 0, 
            "reconnect to memcached channel %V (rc: %d)",
            mcctx->srvconn->name, rc);
        mcctx->status = mcchan_reconnect;
        setsockopt(mcctx->srvconn->connection->fd, SOL_SOCKET, SO_SNDBUF,
	    (void *) &mc_sndbuf_len, sizeof (mc_sndbuf_len));
    }
}

/* mc_hash
 *
 * hash an opaque key onto an available memcached channel number
 * if the memcached channel is currently bad, then fail-over to the next 
 * server in declarative order, until all channels are exhausted
 * 
 * return MC_INVALID_HASH on failure
 */
static ngx_uint_t ngx_memcache_hash (u_char *key, size_t len)
{
    ngx_uint_t               h, r, i;
    mc_context_t            *mcctx;
    ngx_memcache_conf_t     *mcf;

    mcf = (ngx_memcache_conf_t*) ngx_get_conf(ngx_cycle->conf_ctx, ngx_memcache_module);

    if (mcf->contexts.nelts == 0) {
        r = MC_INVALID_HASH;
    } else {
        h = ngx_memcache_perl_hash (key, len);
        r = h % mcf->contexts.nelts;
        i = r;

        do {
            mcctx = ((mc_context_t *)mcf->contexts.elts) + i;

            if (mcctx->status == mcchan_bad) {
                i = (i + 1) % mcf->contexts.nelts;
            }
        } while ((mcctx->status == mcchan_bad) && (i != r));

        if (mcctx->status == mcchan_bad)
            r = MC_INVALID_HASH;
        else
            r = i;
    }

    return r;
}

static ngx_uint_t ngx_memcache_perl_hash (u_char *key, size_t len)
{
    size_t          i;
    ngx_uint_t      h;
    u_char          *p;

    p = key;
    i = len;
    h = 0;

    while (i--)
    {
        h += *p++;
        h += (h << 10);
        h ^= (h >> 6);
    }

    h += (h << 3);
    h ^= (h >> 11);
    h += (h << 15);

    return h;
}

/* Utility function to check if a (login) name has the zimbra 
   supported `special' extensions
   The test is to see if the name ends with /tb, /wm, or /ni
 */
ngx_flag_t has_zimbra_extensions (ngx_str_t l)
{
    ngx_flag_t  f = 0;

    if ((l.len > 3) &&
        (!ngx_memcmp (l.data + (l.len - 3), "/tb", 3) || 
         !ngx_memcmp (l.data + (l.len - 3), "/wm", 3) || 
         !ngx_memcmp (l.data + (l.len - 3), "/ni", 3)
        )
       ) {
       f = 1;
    }

    return f;
}

/* Strip off any zimbra `special' extensions from a (login) name
   Returns a shallow copy of the original name, with the length
   shortened by 3 to strip off the trailing characters
 */
ngx_str_t strip_zimbra_extensions (ngx_str_t l)
{
    ngx_str_t   t=l;

    if (has_zimbra_extensions (l)) {
        t.len -= 3;
    }

    return t;
}

ngx_str_t get_zimbra_extension (ngx_str_t l)
{
    ngx_str_t   e = ngx_string("");

    if (has_zimbra_extensions(l)) {
        e.data = l.data + (l.len-3);
        e.len = 3;
    }

    return e;
}

/* get configuration */
ngx_memcache_conf_t *ngx_memcache_get_conf()
{
    return (ngx_memcache_conf_t *) ngx_get_conf(ngx_cycle->conf_ctx,ngx_memcache_module);
}


ngx_str_t ngx_memcache_get_route_key (
    ngx_pool_t      *pool,
    ngx_log_t       *log,
    ngx_str_t        proto,
    ngx_str_t        user,
    ngx_str_t        ip,
    ngx_flag_t       qualified,
    ngx_flag_t       allowpartial
)
{
    ngx_str_t       k;
    size_t          l;
    u_char         *p,*q,*r;

    l = sizeof("route:") - 1 +
        sizeof("proto=") - 1 +
        proto.len +
        sizeof(";") - 1 +
        sizeof("user=") - 1 +
        user.len +
        sizeof("@255.255.255.255") - 1;

    k.data = ngx_palloc(pool, l);
    if (k.data == NULL) {
        k.len = 0;
        return k;
    }

    p = k.data;
    p = ngx_cpymem(p, "route:", sizeof("route:") - 1);
    p = ngx_cpymem(p, "proto=", sizeof("proto=") - 1);
    p = ngx_cpymem(p, proto.data, proto.len);
    *p++ = ';';
    p = ngx_cpymem(p, "user=", sizeof("user=") - 1);

    q = user.data;
    r = q + user.len;

    while (q < r) {
        if (*q == ' ') { *p++ = '\t'; }
        else { *p++ = *q; }
        ++q;
    }

    if (!qualified) {
        /* check whether we need @IP suffix */
        if (!allowpartial) {
            *p++ = '@';
            p = ngx_cpymem(p, ip.data, ip.len);
        }
    }

    k.len = p - k.data;
    return k;
}

ngx_str_t   ngx_memcache_get_alias_key (
    ngx_pool_t      *pool,
    ngx_log_t       *log,
    ngx_str_t        user,
    ngx_str_t        ip
)
{
    ngx_str_t       k;
    size_t          l;
    u_char         *p, *q, *r;

    l = sizeof("alias:") - 1 +
        sizeof("user=") - 1 +
        user.len +
        sizeof(";") - 1 +
        sizeof("ip=") - 1 +
        sizeof("255.255.255.255") - 1;

    k.data = ngx_palloc(pool, l);
    if (k.data == NULL) {
        k.len = 0;
        return k;
    }

    p = k.data;
    p = ngx_cpymem(p, "alias:", sizeof("alias:") - 1);
    p = ngx_cpymem(p, "user=", sizeof("user=") - 1);

    q = user.data;
    r = q + user.len;

    while (q < r) {
        if (*q == ' ') { *p++ = '\t'; }
        else { *p++ = *q; }
        ++q;
    }

    *p++ = ';';
    p = ngx_cpymem(p,"ip=", sizeof("ip=") - 1);
    p = ngx_cpymem(p, ip.data, ip.len);

    k.len = p - k.data;
    return k;
}

ngx_str_t   ngx_memcache_get_user_throttle_key (
    ngx_pool_t      *pool,
    ngx_log_t       *log,
    ngx_str_t        user
)
{
    ngx_str_t       k;
    u_char         *p, *q, *r;
    size_t          l;

    l = sizeof("throttle:") - 1 +
        sizeof("user=") - 1 +
        user.len;

    k.data = ngx_palloc(pool, l);
    if (k.data == NULL)
    {
        k.len = 0;
        return k;
    }

    p = k.data;
    p = ngx_cpymem(p, "throttle:", sizeof("throttle:") - 1);
    p = ngx_cpymem(p, "user=", sizeof("user=") - 1);

    q = user.data;
    r = q + user.len;

    while (q < r) {
        if (*q == ' ') { *p++ = '\t'; }
        else { *p++ = *q; }
        ++q;
    }

    k.len = p - k.data;
    return k;
}

ngx_str_t   ngx_memcache_get_ip_throttle_key (
    ngx_pool_t      *pool,
    ngx_log_t       *log,
    ngx_str_t        ip
)
{
    ngx_str_t   k;
    size_t      l;
    u_char     *p;

    l = sizeof("throttle:") - 1 +
        sizeof("ip=") - 1 +
        ip.len;

    k.data = ngx_palloc(pool, l);
    if (k.data == NULL)
    {
        k.len = 0;
        return k;
    }

    p = k.data;
    p = ngx_cpymem(p, "throttle:", sizeof("throttle:") - 1);
    p = ngx_cpymem(p, "ip=", sizeof("ip=") - 1);
    p = ngx_cpymem(p, ip.data, ip.len);

    k.len = p - k.data;
    return k;
}

ngx_str_t   ngx_memcache_get_http_alias_key (
    ngx_pool_t          *pool,
    ngx_log_t           *log,
    ngx_str_t            user,
    ngx_str_t            vhost
)
{
    ngx_str_t       k;
    size_t          l;
    u_char         *p,*q,*r;

    l = sizeof("alias:") - 1 +
        sizeof("user=") - 1 +
        user.len +
        sizeof(";") - 1 +
        sizeof("vhost=") - 1 +
        vhost.len;

    k.data = ngx_palloc(pool, l);
    if (k.data == NULL) {
        k.len = 0;
        return k;
    }

    p = k.data;
    p = ngx_cpymem(p, "alias:", sizeof("alias:") - 1);
    p = ngx_cpymem(p, "user=", sizeof("user=") - 1);

    q = user.data;
    r = q + user.len;

    while (q<r) {
        if (*q == ' ') { *p++ = '\t'; }
        else { *p++ = *q; }
        ++q;
    }

    *p++ = ';';
    p = ngx_cpymem(p, "vhost=", sizeof("vhost=") - 1);
    p = ngx_cpymem(p, vhost.data, vhost.len);

    k.len = p - k.data;
    return k;

}

ngx_str_t ngx_memcache_get_http_route_key (
    ngx_pool_t      *pool,
    ngx_log_t       *log,
    ngx_str_t        user
)
{
    ngx_str_t       k;
    size_t          l;
    u_char         *p,*q,*r;

    l = sizeof("route:") - 1 +
        sizeof("proto=http;") - 1 +
        sizeof("user=") - 1 +
        user.len;

    k.data = ngx_palloc(pool, l);
    if (k.data == NULL) {
        k.len = 0;
        return k;
    }

    p = k.data;
    p = ngx_cpymem(p, "route:", sizeof("route:") - 1);
    p = ngx_cpymem(p, "proto=http;", sizeof("proto=http;") - 1);
    p = ngx_cpymem(p, "user=", sizeof("user=") - 1);

    q = user.data;
    r = q + user.len;

    while (q < r) {
        if (*q == ' ') { *p++ = '\t'; }
        else { *p++ = *q; }
        ++q;
    }

    k.len = p - k.data;
    return k;
}

ngx_str_t   ngx_memcache_get_http_id_route_key (
    ngx_pool_t      *pool,
    ngx_log_t       *log,
    ngx_str_t        id
)
{
    ngx_str_t       k;
    size_t          l;
    u_char         *p,*q,*r;

    l = sizeof("route:") - 1 +
        sizeof("proto=http;") - 1 +
        sizeof("id=") - 1 +
        id.len;

    k.data = ngx_palloc(pool, l);
    if (k.data == NULL) {
        k.len = 0;
        return k;
    }

    p = k.data;
    p = ngx_cpymem(p, "route:", sizeof("route:") - 1);
    p = ngx_cpymem(p, "proto=http;", sizeof("proto=http;") - 1);
    p = ngx_cpymem(p, "id=", sizeof("id=") - 1);

    q = id.data;
    r = q + id.len;

    while (q<r) {
        if (*q == ' ') { *p++ = '\t'; }
        else { *p++ = *q; }
        ++q;
    }

    k.len = p - k.data;
    return k;
}

static inline void ngx_memcache_prepare_reconnection (mc_context_t * mcctx) {
    ngx_event_t * ev = mcctx->reconnect_ev;
    ev->data = mcctx;

    if (!ev->timer_set) {
        ngx_add_timer(ev, mcctx->cxn_interval);
        ngx_log_error (NGX_LOG_NOTICE, ngx_cycle->log, 0,
               "channel:%V down, reconnect after:%d ms",
               mcctx->srvconn->name, mcctx->cxn_interval);
    }
}

static void ngx_memcache_reconnection_handler (ngx_event_t * ev) {
    mc_context_t * ctx = (mc_context_t *) ev->data;

    if (ctx->status == mcchan_good) return;

    ev->timedout = 0;
    //ev->timer_set has been set to 0 before this handler are invoked.

    /* unconditionally re-connect bad channel if this event is fired */
    ngx_log_debug2 (NGX_LOG_DEBUG_MAIL, ngx_cycle->log, 0,
        "bad channel:%V, reconnect:%d ms",
        ctx->srvconn->name, ctx->cxn_interval);

    ngx_memcache_reestablish_connection (ctx);
    ngx_unlock(&ctx->lock);

    if (ctx->status == mcchan_bad) { //schedule next retry
        ngx_memcache_prepare_reconnection (ctx);
    }

    return;
}
