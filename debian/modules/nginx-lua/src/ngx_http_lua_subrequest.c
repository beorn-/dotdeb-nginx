#ifndef DDEBUG
#define DDEBUG 0
#endif
#include "ddebug.h"

#include "ngx_http_lua_subrequest.h"
#include "ngx_http_lua_util.h"
#include "ngx_http_lua_contentby.h"


#define ngx_http_lua_method_name(m) { sizeof(m) - 1, (u_char *) m " " }

static ngx_str_t  ngx_http_lua_get_method = ngx_http_lua_method_name("GET");
static ngx_str_t  ngx_http_lua_put_method = ngx_http_lua_method_name("PUT");
static ngx_str_t  ngx_http_lua_post_method = ngx_http_lua_method_name("POST");
static ngx_str_t  ngx_http_lua_head_method = ngx_http_lua_method_name("HEAD");
static ngx_str_t  ngx_http_lua_delete_method =
        ngx_http_lua_method_name("DELETE");


static ngx_str_t  ngx_http_lua_content_length_header_key =
        ngx_string("Content-Length");


static ngx_int_t ngx_http_lua_set_content_length_header(ngx_http_request_t *r,
        off_t len);
static ngx_int_t ngx_http_lua_adjust_subrequest(ngx_http_request_t *sr,
        ngx_uint_t method, ngx_http_request_body_t *body,
        unsigned share_all_vars);
static int ngx_http_lua_ngx_location_capture(lua_State *L);
static int ngx_http_lua_ngx_location_capture_multi(lua_State *L);


/* ngx.location.capture is just a thin wrapper around
 * ngx.location.capture_multi */
static int
ngx_http_lua_ngx_location_capture(lua_State *L)
{
    int                 n;

    n = lua_gettop(L);

    if (n != 1 && n != 2) {
        return luaL_error(L, "expecting one or two arguments");
    }

    lua_createtable(L, n, 0); /* uri opts? table  */
    lua_insert(L, 1); /* table uri opts? */
    if (n == 1) { /* table uri */
        lua_rawseti(L, 1, 1); /* table */

    } else { /* table uri opts */
        lua_rawseti(L, 1, 2); /* table uri */
        lua_rawseti(L, 1, 1); /* table */
    }

    lua_createtable(L, 1, 0); /* table table' */
    lua_insert(L, 1);   /* table' table */
    lua_rawseti(L, 1, 1); /* table' */

    return ngx_http_lua_ngx_location_capture_multi(L);
}


static int
ngx_http_lua_ngx_location_capture_multi(lua_State *L)
{
    ngx_http_request_t              *r;
    ngx_http_request_t              *sr; /* subrequest object */
    ngx_http_post_subrequest_t      *psr;
    ngx_http_lua_ctx_t              *sr_ctx;
    ngx_http_lua_ctx_t              *ctx;
    ngx_str_t                        uri;
    ngx_str_t                        args;
    ngx_str_t                        extra_args;
    ngx_uint_t                       flags;
    u_char                          *p;
    u_char                          *q;
    size_t                           len;
    size_t                           nargs;
    int                              rc;
    int                              n;
    ngx_uint_t                       method;
    ngx_http_request_body_t         *body;
    int                              type;
    ngx_buf_t                       *b;
    unsigned                         share_all_vars;
    ngx_uint_t                       nsubreqs;
    ngx_uint_t                       index;
    size_t                           sr_statuses_len;
    size_t                           sr_headers_len;
    size_t                           sr_bodies_len;

    n = lua_gettop(L);
    if (n != 1) {
        return luaL_error(L, "only one argument is expected, but got %d", n);
    }

    luaL_checktype(L, 1, LUA_TTABLE);

    nsubreqs = lua_objlen(L, 1);
    if (nsubreqs == 0) {
        return luaL_error(L, "at least one subrequest should be specified");
    }

    lua_getglobal(L, GLOBALS_SYMBOL_REQUEST);
    r = lua_touserdata(L, -1);
    lua_pop(L, 1);

    if (r == NULL) {
        return luaL_error(L, "no request object found");
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module);
    if (ctx == NULL) {
        return luaL_error(L, "no ctx found");
    }

    sr_statuses_len = nsubreqs * sizeof(ngx_int_t);
    sr_headers_len  = nsubreqs * sizeof(ngx_http_headers_out_t *);
    sr_bodies_len   = nsubreqs * sizeof(ngx_str_t);

    p = ngx_pcalloc(r->pool, sr_statuses_len + sr_headers_len +
            sr_bodies_len);

    if (p == NULL) {
        return luaL_error(L, "out of memory");
    }

    ctx->sr_statuses = (void *) p;
    p += sr_statuses_len;

    ctx->sr_headers = (void *) p;
    p += sr_headers_len;

    ctx->sr_bodies = (void *) p;

    ctx->nsubreqs = nsubreqs;

    n = lua_gettop(L);
    dd("top before loop: %d", n);

    ctx->done = 0;
    ctx->waiting = 0;

    for (index = 0; index < nsubreqs; index++) {
        ctx->waiting++;

        lua_rawgeti(L, 1, index + 1);
        if (lua_isnil(L, -1)) {
            return luaL_error(L, "only array-like tables are allowed");
        }

        if (lua_type(L, -1) != LUA_TTABLE) {
            return luaL_error(L, "the query argument %d is not a table, "
                    "but a %s",
                    index, lua_typename(L, lua_type(L, -1)));
        }

        dd("lua top so far: %d", lua_gettop(L));

        nargs = lua_objlen(L, -1);

        if (nargs != 1 && nargs != 2) {
            return luaL_error(L, "query argument %d expecting one or "
                    "two arguments", index);
        }

        lua_rawgeti(L, 2, 1); /* queries query uri */

        dd("first arg in first query: %s", lua_typename(L, lua_type(L, -1)));

        body = NULL;

        extra_args.data = NULL;
        extra_args.len = 0;

        share_all_vars = 0;

        if (nargs == 2) {
            /* check out the options table */

            lua_rawgeti(L, 2, 2);

            if (lua_type(L, 4) != LUA_TTABLE) {
                return luaL_error(L, "expecting table as the 2nd argument for "
                        "subrequest %d", index);
            }

            /* check the args option */

            lua_getfield(L, 4, "args");

            type = lua_type(L, -1);

            switch (type) {
            case LUA_TTABLE:
                ngx_http_lua_process_args_option(r, L, -1, &extra_args);
                break;

            case LUA_TNIL:
                /* do nothing */
                break;

            case LUA_TNUMBER:
            case LUA_TSTRING:
                extra_args.data = (u_char *) lua_tolstring(L, -1, &len);
                extra_args.len = len;

                break;

            default:
                return luaL_error(L, "Bad args option value");
            }

            lua_pop(L, 1);

            /* check the share_all_vars option */

            lua_getfield(L, 4, "share_all_vars");

            type = lua_type(L, -1);

            if (type == LUA_TNIL) {
                /* do nothing */

            } else {
                if (type != LUA_TBOOLEAN) {
                    return luaL_error(L, "Bad share_all_vars option value");
                }

                share_all_vars = lua_toboolean(L, -1);
            }

            lua_pop(L, 1);

            /* check the method option */

            lua_getfield(L, 4, "method");

            type = lua_type(L, -1);

            if (type == LUA_TNIL) {
                method = NGX_HTTP_GET;

            } else {
                if (type != LUA_TNUMBER) {
                    return luaL_error(L, "Bad http request method");
                }

                method = (ngx_uint_t) lua_tonumber(L, -1);
            }

            lua_pop(L, 1);

            /* check the body option */

            lua_getfield(L, 4, "body");

            type = lua_type(L, -1);

            if (type != LUA_TNIL) {
                if (type != LUA_TSTRING && type != LUA_TNUMBER) {
                    return luaL_error(L, "Bad http request body");
                }

                body = ngx_pcalloc(r->pool,
                        sizeof(ngx_http_request_body_t));

                if (body == NULL) {
                    return luaL_error(L, "out of memory");
                }

                q = (u_char *) lua_tolstring(L, -1, &len);

                dd("request body: [%.*s]", (int) len, q);

                if (len) {
                    b = ngx_create_temp_buf(r->pool, len);
                    if (b == NULL) {
                        return luaL_error(L, "out of memory");
                    }

                    b->last = ngx_copy(b->last, q, len);

                    body->bufs = ngx_alloc_chain_link(r->pool);
                    if (body->bufs == NULL) {
                        return luaL_error(L, "out of memory");
                    }

                    body->bufs->buf = b;
                    body->bufs->next = NULL;

                    body->buf = b;
                }
            }

            lua_pop(L, 2); /* pop body and opts table */
        } else {
            method = NGX_HTTP_GET;
        }

        n = lua_gettop(L);
        dd("top size so far: %d", n);

        p = (u_char *) luaL_checklstring(L, 3, &len);

        uri.data = ngx_palloc(r->pool, len);
        if (uri.data == NULL) {
            return luaL_error(L, "memory allocation error");
        }

        ngx_memcpy(uri.data, p, len);

        uri.len = len;

        args.data = NULL;
        args.len = 0;

        flags = 0;

        rc = ngx_http_parse_unsafe_uri(r, &uri, &args, &flags);
        if (rc != NGX_OK) {
            dd("rc = %d", (int) rc);

            return luaL_error(L, "unsafe uri in argument #1: %s", p);
        }

        if (args.len == 0) {
            args = extra_args;

        } else if (extra_args.len) {
            /* concatenate the two parts of args together */
            len = args.len + (sizeof("&") - 1) + extra_args.len;

            p = ngx_palloc(r->pool, len);
            if (p == NULL) {
                return luaL_error(L, "out of memory");
            }

            q = ngx_copy(p, args.data, args.len);
            *q++ = '&';
            q = ngx_copy(q, extra_args.data, extra_args.len);

            args.data = p;
            args.len = len;
        }

        psr = ngx_palloc(r->pool, sizeof(ngx_http_post_subrequest_t));
        if (psr == NULL) {
            return luaL_error(L, "memory allocation error");
        }

        sr_ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_lua_ctx_t));
        if (sr_ctx == NULL) {
            return luaL_error(L, "out of memory");
        }

        /* set by ngx_pcalloc:
         *      sr_ctx->run_post_subrequest = 0
         *      sr_ctx->free = NULL
         */

        sr_ctx->cc_ref = LUA_NOREF;
        sr_ctx->ctx_ref = LUA_NOREF;

        sr_ctx->capture = 1;

        sr_ctx->index = index;

        psr->handler = ngx_http_lua_post_subrequest;
        psr->data = sr_ctx;

        rc = ngx_http_subrequest(r, &uri, &args, &sr, psr, 0);

        if (rc != NGX_OK) {
            return luaL_error(L, "failed to issue subrequest: %d", (int) rc);
        }

        ngx_http_set_ctx(sr, sr_ctx, ngx_http_lua_module);

        rc = ngx_http_lua_adjust_subrequest(sr, method, body, share_all_vars);

        if (rc != NGX_OK) {
            return luaL_error(L, "failed to adjust the subrequest: %d",
                    (int) rc);
        }

        lua_pop(L, 2); /* pop the subrequest argument and uri */
    }

    return lua_yield(L, 0);
}


static ngx_int_t
ngx_http_lua_adjust_subrequest(ngx_http_request_t *sr, ngx_uint_t method,
        ngx_http_request_body_t *body, unsigned share_all_vars)
{
    ngx_http_request_t          *r;
    ngx_int_t                    rc;
    ngx_http_core_main_conf_t   *cmcf;

    r = sr->parent;

    sr->header_in = r->header_in;

#if 1
    /* XXX work-around a bug in ngx_http_subrequest */
    if (r->headers_in.headers.last == &r->headers_in.headers.part) {
        sr->headers_in.headers.last = &sr->headers_in.headers.part;
    }
#endif

    if (body) {
        sr->request_body = body;

        rc = ngx_http_lua_set_content_length_header(sr,
                body->buf ? ngx_buf_size(body->buf) : 0);

        if (rc != NGX_OK) {
            return NGX_ERROR;
        }
    }

    sr->method = method;

    switch (method) {
        case NGX_HTTP_GET:
            sr->method_name = ngx_http_lua_get_method;
            break;

        case NGX_HTTP_POST:
            sr->method_name = ngx_http_lua_post_method;
            break;

        case NGX_HTTP_PUT:
            sr->method_name = ngx_http_lua_put_method;
            break;

        case NGX_HTTP_HEAD:
            sr->method_name = ngx_http_lua_head_method;
            break;

        case NGX_HTTP_DELETE:
            sr->method_name = ngx_http_lua_delete_method;
            break;

        default:
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                    "unsupported HTTP method: %u", (unsigned) method);

            return NGX_ERROR;
    }

    /* XXX work-around a bug in ngx_http_subrequest */
    if (r->headers_in.headers.last == &r->headers_in.headers.part) {
        sr->headers_in.headers.last = &sr->headers_in.headers.part;
    }

    if (! share_all_vars) {
        /* we do not inherit the parent request's variables */
        cmcf = ngx_http_get_module_main_conf(sr, ngx_http_core_module);

        sr->variables = ngx_pcalloc(sr->pool, cmcf->variables.nelts
                                * sizeof(ngx_http_variable_value_t));

        if (sr->variables == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
    }

    return NGX_OK;
}


ngx_int_t
ngx_http_lua_post_subrequest(ngx_http_request_t *r, void *data, ngx_int_t rc)
{
    ngx_http_request_t            *pr;
    ngx_http_lua_ctx_t            *pr_ctx;
    ngx_http_lua_ctx_t            *ctx = data;
    size_t                         len;
    ngx_str_t                     *body_str;
    u_char                        *p;
    ngx_chain_t                   *cl;

    if (ctx->run_post_subrequest) {
        return rc;
    }

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
            "lua run post subrequest handler: rc:%d, waiting:%d, done:%d",
            rc, ctx->waiting, ctx->done);

    ctx->run_post_subrequest = 1;

#if 0
    ngx_http_lua_dump_postponed(r);
#endif

    pr = r->parent;

    pr_ctx = ngx_http_get_module_ctx(pr, ngx_http_lua_module);
    if (pr_ctx == NULL) {
        return NGX_ERROR;
    }

    pr_ctx->waiting--;

    if (pr_ctx->waiting == 0) {
        pr_ctx->done = 1;
    }

    if (pr_ctx->entered_content_phase) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                "lua restoring write event handler");

        pr->write_event_handler = ngx_http_lua_content_wev_handler;
    }

    dd("status rc = %d", (int) rc);
    dd("status headers_out.status = %d", (int) r->headers_out.status);
    dd("uri: %.*s", (int) r->uri.len, r->uri.data);

    /*  capture subrequest response status */
    if (rc == NGX_ERROR) {
        pr_ctx->sr_statuses[ctx->index] = NGX_HTTP_INTERNAL_SERVER_ERROR;

    } else if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        dd("HERE");
        pr_ctx->sr_statuses[ctx->index] = rc;

    } else {
        dd("THERE");
        pr_ctx->sr_statuses[ctx->index] = r->headers_out.status;
    }

    if (pr_ctx->sr_statuses[ctx->index] == 0) {
        pr_ctx->sr_statuses[ctx->index] = NGX_HTTP_OK;
    }

    dd("pr_ctx status: %d", (int) pr_ctx->sr_statuses[ctx->index]);

    /* copy subrequest response headers */

    pr_ctx->sr_headers[ctx->index] = &r->headers_out;

    /* copy subrequest response body */

    body_str = &pr_ctx->sr_bodies[ctx->index];

    if (ctx->body && ctx->body->next == NULL) {
        /* optimize for the single buf body */

        cl = ctx->body;

        len = cl->buf->last - cl->buf->pos;

        body_str->len = len;

        if (len == 0) {
            body_str->data = NULL;

        } else {
            body_str->data = cl->buf->pos;
        }

    } else {
        len = 0;
        for (cl = ctx->body; cl; cl = cl->next) {
            /*  ignore all non-memory buffers */
            len += cl->buf->last - cl->buf->pos;
        }

        body_str->len = len;

        if (len == 0) {
            body_str->data = NULL;

        } else {
            p = ngx_palloc(r->pool, len);
            if (p == NULL) {
                return NGX_ERROR;
            }

            body_str->data = p;

            /* copy from and then free the data buffers */

            for (cl = ctx->body; cl; cl = cl->next) {
                p = ngx_copy(p, cl->buf->pos,
                        cl->buf->last - cl->buf->pos);

                dd("free bod chain link buf ASAP");
                ngx_pfree(r->pool, cl->buf->start);
            }
        }
    }

    if (ctx->body) {
        /* free the ctx->body chain such that it can be reused by
         * other subrequests */

        if (pr_ctx->free == NULL) {
            pr_ctx->free = ctx->body;

        } else {
            for (cl = pr_ctx->free; cl->next; cl = cl->next) { /* void */ }

            cl->next = ctx->body;
        }
    }

    /* work-around issues in nginx's event module */

    if (r != r->connection->data && r->postponed &&
            (r->main->posted_requests == NULL ||
            r->main->posted_requests->request != pr))
    {
#if defined(nginx_version) && nginx_version >= 8012
        ngx_http_post_request(pr, NULL);
#else
        ngx_http_post_request(pr);
#endif
    }

    return rc;
}


static ngx_int_t
ngx_http_lua_set_content_length_header(ngx_http_request_t *r, off_t len)
{
    ngx_table_elt_t                 *h, *header;
    u_char                          *p;
    ngx_list_part_t                 *part;
    ngx_http_request_t              *pr;
    ngx_uint_t                       i;

    r->headers_in.content_length_n = len;

    if (ngx_list_init(&r->headers_in.headers, r->pool, 20,
                sizeof(ngx_table_elt_t)) != NGX_OK) {
        return NGX_ERROR;
    }

    h = ngx_list_push(&r->headers_in.headers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    h->key = ngx_http_lua_content_length_header_key;
    h->lowcase_key = ngx_pnalloc(r->pool, h->key.len);
    if (h->lowcase_key == NULL) {
        return NGX_ERROR;
    }

    ngx_strlow(h->lowcase_key, h->key.data, h->key.len);

    r->headers_in.content_length = h;

    p = ngx_palloc(r->pool, NGX_OFF_T_LEN);
    if (p == NULL) {
        return NGX_ERROR;
    }

    h->value.data = p;

    h->value.len = ngx_sprintf(h->value.data, "%O", len) - h->value.data;

    h->hash = 1;

    dd("r content length: %.*s",
            (int)r->headers_in.content_length->value.len,
            r->headers_in.content_length->value.data);

    pr = r->parent;

    if (pr == NULL) {
        return NGX_OK;
    }

    /* forward the parent request's all other request headers */

    part = &pr->headers_in.headers.part;
    header = part->elts;

    for (i = 0; /* void */; i++) {

        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            header = part->elts;
            i = 0;
        }

        if (header[i].key.len == sizeof("Content-Length") - 1 &&
                ngx_strncasecmp(header[i].key.data, (u_char *) "Content-Length",
                sizeof("Content-Length") - 1) == 0)
        {
            continue;
        }

        h = ngx_list_push(&r->headers_in.headers);
        if (h == NULL) {
            return NGX_ERROR;
        }

        *h = header[i];
    }

    /* XXX maybe we should set those built-in header slot in
     * ngx_http_headers_in_t too? */

    return NGX_OK;
}


void
ngx_http_lua_handle_subreq_responses(ngx_http_request_t *r,
        ngx_http_lua_ctx_t *ctx)
{
    ngx_uint_t                   index;
    lua_State                   *cc;
    ngx_str_t                   *body_str;
    ngx_table_elt_t             *header;
    ngx_list_part_t             *part;
    ngx_http_headers_out_t      *sr_headers;
    ngx_uint_t                   i;

    u_char                  buf[sizeof("Mon, 28 Sep 1970 06:00:00 GMT") - 1];

    for (index = 0; index < ctx->nsubreqs; index++) {
        dd("summary: reqs %d, subquery %d, waiting %d, req %.*s",
                (int) ctx->nsubreqs,
                (int) index,
                (int) ctx->waiting,
                (int) r->uri.len, r->uri.data);

        cc = ctx->cc;

        /*  {{{ construct ret value */
        lua_newtable(cc);

        /*  copy captured status */
        lua_pushinteger(cc, ctx->sr_statuses[index]);
        lua_setfield(cc, -2, "status");

        /*  copy captured body */

        body_str = &ctx->sr_bodies[index];

        lua_pushlstring(cc, (char *) body_str->data, body_str->len);
        lua_setfield(cc, -2, "body");

        if (body_str->data) {
            dd("free body buffer ASAP");
            ngx_pfree(r->pool, body_str->data);
        }

        /* copy captured headers */

        lua_newtable(cc); /* res.header */

        sr_headers = ctx->sr_headers[index];

        dd("saving subrequest response headers");

        part = &sr_headers->headers.part;
        header = part->elts;

        for (i = 0; /* void */; i++) {

            if (i >= part->nelts) {
                if (part->next == NULL) {
                    break;
                }

                part = part->next;
                header = part->elts;
                i = 0;
            }

            dd("checking sr header %.*s", (int) header[i].key.len,
                    header[i].key.data);

#if 1
            if (header[i].hash == 0) {
                continue;
            }
#endif

            header[i].hash = 0;

            dd("pushing sr header %.*s", (int) header[i].key.len,
                    header[i].key.data);

            lua_pushlstring(cc, (char *) header[i].key.data,
                    header[i].key.len); /* header key */
            lua_pushvalue(cc, -1); /* stack: table key key */

            /* check if header already exists */
            lua_rawget(cc, -3); /* stack: table key value */

            if (lua_isnil(cc, -1)) {
                lua_pop(cc, 1); /* stack: table key */

                lua_pushlstring(cc, (char *) header[i].value.data,
                        header[i].value.len); /* stack: table key value */

                lua_rawset(cc, -3); /* stack: table */

            } else {
                if (! lua_istable(cc, -1)) { /* already inserted one value */
                    lua_createtable(cc, 4, 0);
                        /* stack: table key value table */

                    lua_insert(cc, -2); /* stack: table key table value */
                    lua_rawseti(cc, -2, 1); /* stack: table key table */

                    lua_pushlstring(cc, (char *) header[i].value.data,
                            header[i].value.len);
                        /* stack: table key table value */

                    lua_rawseti(cc, -2, lua_objlen(cc, -2) + 1);
                        /* stack: table key table */

                    lua_rawset(cc, -3); /* stack: table */

                } else {
                    lua_pushlstring(cc, (char *) header[i].value.data,
                            header[i].value.len);
                        /* stack: table key table value */

                    lua_rawseti(cc, -2, lua_objlen(cc, -2) + 1);
                        /* stack: table key table */

                    lua_pop(cc, 2); /* stack: table */
                }
            }
        }

        if (sr_headers->content_type.len) {
            lua_pushliteral(cc, "Content-Type"); /* header key */
            lua_pushlstring(cc, (char *) sr_headers->content_type.data,
                    sr_headers->content_type.len); /* head key value */
            lua_rawset(cc, -3); /* head */
        }

        if (sr_headers->content_length == NULL
            && sr_headers->content_length_n >= 0)
        {
            lua_pushliteral(cc, "Content-Length"); /* header key */

            lua_pushnumber(cc, sr_headers->content_length_n);
                /* head key value */

            lua_rawset(cc, -3); /* head */
        }

        /* to work-around an issue in ngx_http_static_module
         * (github issue #41) */
        if (sr_headers->location && sr_headers->location->value.len) {
            lua_pushliteral(cc, "Location"); /* header key */
            lua_pushlstring(cc, (char *) sr_headers->location->value.data,
                    sr_headers->location->value.len); /* head key value */
            lua_rawset(cc, -3); /* head */
        }

        if (sr_headers->last_modified_time != -1) {
            if (sr_headers->status != NGX_HTTP_OK
                && sr_headers->status != NGX_HTTP_PARTIAL_CONTENT
                && sr_headers->status != NGX_HTTP_NOT_MODIFIED
                && sr_headers->status != NGX_HTTP_NO_CONTENT)
            {
                sr_headers->last_modified_time = -1;
                sr_headers->last_modified = NULL;
            }
        }

        if (sr_headers->last_modified == NULL
            && sr_headers->last_modified_time != -1)
        {
            (void) ngx_http_time(buf, sr_headers->last_modified_time);

            lua_pushliteral(cc, "Last-Modified"); /* header key */
            lua_pushlstring(cc, (char *) buf, sizeof(buf)); /* head key value */
            lua_rawset(cc, -3); /* head */
        }

        lua_setfield(cc, -2, "header");

        /*  }}} */
    }
}


void
ngx_http_lua_inject_subrequest_api(lua_State *L)
{
    lua_createtable(L, 0, 2 /* nrec */); /* .location */

    lua_pushcfunction(L, ngx_http_lua_ngx_location_capture);
    lua_setfield(L, -2, "capture");

    lua_pushcfunction(L, ngx_http_lua_ngx_location_capture_multi);
    lua_setfield(L, -2, "capture_multi");

    lua_setfield(L, -2, "location");
}

