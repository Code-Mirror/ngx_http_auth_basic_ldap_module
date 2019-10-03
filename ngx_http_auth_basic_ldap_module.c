#include <openldap.h>
#include <ngx_http.h>

typedef struct {
    ngx_http_complex_value_t *bind;
    ngx_http_complex_value_t *header;
    ngx_http_complex_value_t *realm;
    ngx_http_complex_value_t *url;
} ngx_http_auth_basic_ldap_location_conf_t;

typedef struct {
    int msgid;
    LDAP *ldap;
    LDAPMessage *result;
    LDAPURLDesc *lud;
    ngx_int_t rc;
    ngx_peer_connection_t peer_connection;
    ngx_str_t realm;
} ngx_http_auth_basic_ldap_context_t;

ngx_module_t ngx_http_auth_basic_ldap_module;

static ngx_command_t ngx_http_auth_basic_ldap_commands[] = {
  { .name = ngx_string("auth_basic_ldap_bind"),
    .type = NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    .set = ngx_http_set_complex_value_slot,
    .conf = NGX_HTTP_LOC_CONF_OFFSET,
    .offset = offsetof(ngx_http_auth_basic_ldap_location_conf_t, bind),
    .post = NULL },
  { .name = ngx_string("auth_basic_ldap_header"),
    .type = NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    .set = ngx_http_set_complex_value_slot,
    .conf = NGX_HTTP_LOC_CONF_OFFSET,
    .offset = offsetof(ngx_http_auth_basic_ldap_location_conf_t, header),
    .post = NULL },
  { .name = ngx_string("auth_basic_ldap_realm"),
    .type = NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    .set = ngx_http_set_complex_value_slot,
    .conf = NGX_HTTP_LOC_CONF_OFFSET,
    .offset = offsetof(ngx_http_auth_basic_ldap_location_conf_t, realm),
    .post = NULL },
  { .name = ngx_string("auth_basic_ldap_url"),
    .type = NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    .set = ngx_http_set_complex_value_slot,
    .conf = NGX_HTTP_LOC_CONF_OFFSET,
    .offset = offsetof(ngx_http_auth_basic_ldap_location_conf_t, url),
    .post = NULL },
    ngx_null_command
};

static ngx_int_t ngx_http_auth_basic_ldap_set_realm(ngx_http_request_t *r) {
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s", __func__);
    if (!(r->headers_out.www_authenticate = ngx_list_push(&r->headers_out.headers))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: %s:%d", __FILE__, __LINE__); return NGX_HTTP_INTERNAL_SERVER_ERROR; }
    ngx_http_auth_basic_ldap_context_t *context = ngx_http_get_module_ctx(r, ngx_http_auth_basic_ldap_module);
    size_t len = sizeof("Basic realm=\"\"") - 1 + context->realm.len;
    u_char *basic = ngx_pnalloc(r->pool, len);
    if (!basic) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: %s:%d", __FILE__, __LINE__); r->headers_out.www_authenticate->hash = 0; r->headers_out.www_authenticate = NULL; return NGX_HTTP_INTERNAL_SERVER_ERROR; }
    u_char *p = ngx_cpymem(basic, "Basic realm=\"", sizeof("Basic realm=\"") - 1);
    p = ngx_cpymem(p, context->realm.data, context->realm.len);
    *p = '"';
    r->headers_out.www_authenticate->hash = 1;
    ngx_str_set(&r->headers_out.www_authenticate->key, "WWW-Authenticate");
    r->headers_out.www_authenticate->value.data = basic;
    r->headers_out.www_authenticate->value.len = len;
    return NGX_HTTP_UNAUTHORIZED;
}

static void ngx_http_auth_basic_ldap_bind(ngx_http_request_t *r) {
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s", __func__);
    ngx_http_auth_basic_ldap_context_t *context = ngx_http_get_module_ctx(r, ngx_http_auth_basic_ldap_module);
    if (!context->lud->lud_dn) { context->rc = NGX_OK; return; }
    int rc = ldap_search_ext(context->ldap, context->lud->lud_dn, context->lud->lud_scope, context->lud->lud_filter, context->lud->lud_attrs, 0, NULL, NULL, NULL, 0, &context->msgid);
    if (rc != LDAP_SUCCESS) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: ldap_search_ext failed: %s", ldap_err2string(rc)); goto ngx_http_auth_basic_ldap_set_realm; }
    return;
ngx_http_auth_basic_ldap_set_realm:
    context->rc = ngx_http_auth_basic_ldap_set_realm(r);
    return;
}

static void ngx_http_auth_basic_ldap_search_entry(ngx_http_request_t *r) {
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s", __func__);
    ngx_http_auth_basic_ldap_context_t *context = ngx_http_get_module_ctx(r, ngx_http_auth_basic_ldap_module);
    BerElement *ber = NULL;
    char *attr = NULL;
    struct berval **vals = NULL;
    int rc = ldap_count_entries(context->ldap, context->result);
    if (rc <= 0) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: ldap_count_entries failed: %i", rc); goto ngx_http_auth_basic_ldap_set_realm; }
    ngx_http_auth_basic_ldap_location_conf_t *location_conf = ngx_http_get_module_loc_conf(r, ngx_http_auth_basic_ldap_module);
    ngx_str_t header;
    if (ngx_http_complex_value(r, location_conf->header, &header) != NGX_OK) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: %s:%d", __FILE__, __LINE__); goto rc_NGX_ERROR; }
    for (LDAPMessage *entry = ldap_first_entry(context->ldap, context->result); entry; entry = ldap_next_entry(context->ldap, entry)) {
        for (attr = ldap_first_attribute(context->ldap, entry, &ber); attr; ldap_memfree(attr), attr = ldap_next_attribute(context->ldap, entry, ber)) {
            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ldap: attr = %s", attr);
            if (!(vals = ldap_get_values_len(context->ldap, entry, attr))) { ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "ldap: %s:%d", __FILE__, __LINE__); goto ngx_http_auth_basic_ldap_set_realm; }
            int cnt = ldap_count_values_len(vals);
            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ldap: ldap_count_values = %i", cnt);
            ngx_str_t key;
            key.len = ngx_strlen(attr);
            if (header.len){
                key.len += header.len;
                if (!(key.data = ngx_pnalloc(r->pool, key.len))) { ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "ldap: %s:%d", __FILE__, __LINE__); goto rc_NGX_HTTP_INTERNAL_SERVER_ERROR; }
                if (ngx_snprintf(key.data, key.len, "%V%s", &header, attr) != key.data + key.len) { ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "ldap: %s:%d", __FILE__, __LINE__); goto rc_NGX_HTTP_INTERNAL_SERVER_ERROR; }
            } else {
                if (!(key.data = ngx_pnalloc(r->pool, key.len))) { ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "ldap: %s:%d", __FILE__, __LINE__); goto rc_NGX_HTTP_INTERNAL_SERVER_ERROR; }
                ngx_memcpy(key.data, attr, key.len);
            }
            for (int i = 0; i < cnt; i++) {
                struct berval *val = vals[i];
                ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ldap: vals[%i] = %*.s", i, (int)val->bv_len, val->bv_val);
                ngx_str_t value;
                value.len = val->bv_len;
                if (!(value.data = ngx_pnalloc(r->pool, value.len))) { ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "ldap: %s:%d", __FILE__, __LINE__); goto rc_NGX_HTTP_INTERNAL_SERVER_ERROR; }
                ngx_memcpy(value.data, val->bv_val, value.len);
                ngx_table_elt_t *table_elt = ngx_list_push(&r->headers_in.headers);
                if (!table_elt) { ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "ldap: %s:%d", __FILE__, __LINE__); goto rc_NGX_HTTP_INTERNAL_SERVER_ERROR; }
                table_elt->key = key;
                table_elt->value = value;
            }
            ldap_value_free_len(vals);
        }
        ber_free(ber, 0);
    }
    context->rc = NGX_OK;
    return;
ngx_http_auth_basic_ldap_set_realm:
    context->rc = ngx_http_auth_basic_ldap_set_realm(r);
free:
    if (vals) ldap_value_free_len(vals);
    if (attr) ldap_memfree(attr);
    if (ber) ber_free(ber, 0);
    return;
rc_NGX_HTTP_INTERNAL_SERVER_ERROR:
    context->rc = NGX_HTTP_INTERNAL_SERVER_ERROR;
    goto free;
rc_NGX_ERROR:
    context->rc = NGX_ERROR;
    goto free;
}

static void ngx_http_auth_basic_ldap_read_handler(ngx_event_t *ev) {
    int rc;
    ngx_connection_t *c = ev->data;
    ngx_http_request_t *r = c->data;
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s", __func__);
    ngx_http_auth_basic_ldap_context_t *context = ngx_http_get_module_ctx(r, ngx_http_auth_basic_ldap_module);
    if (!context->ldap) return;
    if (context->rc != NGX_AGAIN) return;
    char *errmsg = NULL;
    struct timeval timeout = {0, 0};
    if ((rc = ldap_result(context->ldap, context->msgid, 0, &timeout, &context->result)) < 0) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: ldap_result failed: %s", ldap_err2string(rc)); goto rc_NGX_HTTP_INTERNAL_SERVER_ERROR; }
    if (!rc) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: ldap_result = 0"); goto ngx_http_auth_basic_ldap_set_realm; }
    int errcode;
    switch ((rc = ldap_parse_result(context->ldap, context->result, &errcode, NULL, &errmsg, NULL, NULL, 0))) {
        case LDAP_SUCCESS: case LDAP_NO_RESULTS_RETURNED: break;
        default: ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: ldap_parse_result failed:  %s", ldap_err2string(rc)); goto rc_NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    switch ((rc = ldap_msgtype(context->result))) {
        case LDAP_RES_BIND: {
            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ldap: LDAP_RES_BIND");
            if (errcode != LDAP_SUCCESS) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: bind failed: %s [%s]", ldap_err2string(errcode), errmsg ? errmsg : "-"); goto ngx_http_auth_basic_ldap_set_realm; }
            ngx_http_auth_basic_ldap_bind(r);
        } break;
        case LDAP_RES_SEARCH_ENTRY: {
            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ldap: LDAP_RES_SEARCH_ENTRY");
            if (errcode != LDAP_SUCCESS) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: search entry failed: %s [%s]", ldap_err2string(errcode), errmsg ? errmsg : "-"); goto ngx_http_auth_basic_ldap_set_realm; }
            ngx_http_auth_basic_ldap_search_entry(r);
        } break;
        case LDAP_RES_SEARCH_REFERENCE: ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "ldap: LDAP_RES_SEARCH_REFERENCE"); goto ngx_http_auth_basic_ldap_set_realm;
        case LDAP_RES_SEARCH_RESULT: {
            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ldap: LDAP_RES_SEARCH_RESULT");
            if (errcode != LDAP_SUCCESS) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: search result failed: %s [%s]", ldap_err2string(errcode), errmsg ? errmsg : "-"); goto ngx_http_auth_basic_ldap_set_realm; }
            goto ngx_http_auth_basic_ldap_set_realm;
        } break;
        case LDAP_RES_MODIFY: ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "ldap: LDAP_RES_MODIFY"); goto ngx_http_auth_basic_ldap_set_realm;
        case LDAP_RES_ADD: ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "ldap: LDAP_RES_ADD"); goto ngx_http_auth_basic_ldap_set_realm;
        case LDAP_RES_DELETE: ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "ldap: LDAP_RES_DELETE"); goto ngx_http_auth_basic_ldap_set_realm;
        case LDAP_RES_MODDN: ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "ldap: LDAP_RES_MODDN"); goto ngx_http_auth_basic_ldap_set_realm;
        case LDAP_RES_COMPARE: ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "ldap: LDAP_RES_COMPARE"); goto ngx_http_auth_basic_ldap_set_realm;
        case LDAP_RES_EXTENDED: ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "ldap: LDAP_RES_EXTENDED"); goto ngx_http_auth_basic_ldap_set_realm;
        case LDAP_RES_INTERMEDIATE: ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "ldap: LDAP_RES_INTERMEDIATE"); goto ngx_http_auth_basic_ldap_set_realm;
        default: ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: unknown ldap_msgtype %d", rc); goto ngx_http_auth_basic_ldap_set_realm;
    }
ngx_http_core_run_phases:
    if (errmsg) ldap_memfree(errmsg);
    ngx_http_core_run_phases(r);
    return;
ngx_http_auth_basic_ldap_set_realm:
    context->rc = ngx_http_auth_basic_ldap_set_realm(r);
    goto ngx_http_core_run_phases;
rc_NGX_HTTP_INTERNAL_SERVER_ERROR:
    context->rc = NGX_HTTP_INTERNAL_SERVER_ERROR;
    goto ngx_http_core_run_phases;
}

static void ngx_http_auth_basic_ldap_write_handler(ngx_event_t *ev) {
    ngx_connection_t *c = ev->data;
    ngx_http_request_t *r = c->data;
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s", __func__);
    ngx_http_auth_basic_ldap_context_t *context = ngx_http_get_module_ctx(r, ngx_http_auth_basic_ldap_module);
    if (context->ldap) return;
    if (context->rc != NGX_AGAIN) return;
    int rc = ldap_init_fd(c->fd, LDAP_PROTO_TCP, NULL, &context->ldap);
    if (rc != LDAP_SUCCESS) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: ldap_init_fd failed: %s", ldap_err2string(rc)); goto ngx_http_auth_basic_ldap_set_realm; }
    ngx_http_auth_basic_ldap_location_conf_t *location_conf = ngx_http_get_module_loc_conf(r, ngx_http_auth_basic_ldap_module);
    ngx_str_t bind;
    if (ngx_http_complex_value(r, location_conf->bind, &bind) != NGX_OK) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: %s:%d", __FILE__, __LINE__); goto rc_NGX_ERROR; }
    u_char *who = ngx_pnalloc(r->pool, bind.len + 1);
    if (!who) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: %s:%d", __FILE__, __LINE__); goto rc_NGX_HTTP_INTERNAL_SERVER_ERROR; }
    (void) ngx_cpystrn(who, bind.data, bind.len + 1);
    struct berval cred = {r->headers_in.passwd.len, (char *)r->headers_in.passwd.data};
    if ((rc = ldap_sasl_bind(context->ldap, (const char *)who, LDAP_SASL_SIMPLE, &cred, NULL, NULL, &context->msgid)) != LDAP_SUCCESS) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: ldap_sasl_bind failed: %s", ldap_err2string(rc)); goto ngx_http_auth_basic_ldap_set_realm; }
ngx_http_core_run_phases:
    ngx_http_core_run_phases(r);
    return;
ngx_http_auth_basic_ldap_set_realm:
    context->rc = ngx_http_auth_basic_ldap_set_realm(r);
    goto ngx_http_core_run_phases;
rc_NGX_HTTP_INTERNAL_SERVER_ERROR:
    context->rc = NGX_HTTP_INTERNAL_SERVER_ERROR;
    goto ngx_http_core_run_phases;
rc_NGX_ERROR:
    context->rc = NGX_ERROR;
    goto ngx_http_core_run_phases;
}

static ngx_int_t ngx_http_auth_basic_ldap_handler(ngx_http_request_t *r) {
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s", __func__);
    ngx_http_auth_basic_ldap_location_conf_t *location_conf = ngx_http_get_module_loc_conf(r, ngx_http_auth_basic_ldap_module);
    if (!location_conf->realm) return NGX_DECLINED;
    ngx_http_auth_basic_ldap_context_t *context = ngx_http_get_module_ctx(r, ngx_http_auth_basic_ldap_module);
    if (!context) {
        context = ngx_pcalloc(r->pool, sizeof(ngx_http_auth_basic_ldap_context_t));
        if (!context) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: %s:%d", __FILE__, __LINE__); return NGX_HTTP_INTERNAL_SERVER_ERROR; }
        if (ngx_http_complex_value(r, location_conf->realm, &context->realm) != NGX_OK) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: %s:%d", __FILE__, __LINE__); return NGX_ERROR; }
        if (context->realm.len == sizeof("off") - 1 && ngx_strncasecmp(context->realm.data, (u_char *)"off", sizeof("off") - 1) == 0) return NGX_DECLINED;
        switch (ngx_http_auth_basic_user(r)) {
            case NGX_DECLINED: ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: no user/password was provided for basic authentication"); return ngx_http_auth_basic_ldap_set_realm(r);
            case NGX_ERROR: return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        if (!r->headers_in.passwd.len) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: no password was provided for basic authentication"); return ngx_http_auth_basic_ldap_set_realm(r); }
        ngx_str_t url;
        if (ngx_http_complex_value(r, location_conf->url, &url) != NGX_OK) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: %s:%d", __FILE__, __LINE__); return NGX_ERROR; }
        u_char *urlc = ngx_pnalloc(r->pool, url.len + 1);
        if (!urlc) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: %s:%d", __FILE__, __LINE__); return NGX_HTTP_INTERNAL_SERVER_ERROR; }
        (void) ngx_cpystrn(urlc, url.data, url.len + 1);
        ngx_url_t ngx_url;
        ngx_memzero(&ngx_url, sizeof(ngx_url_t));
        int rc = ldap_url_parse((const char *)urlc, &context->lud);
        if (rc != LDAP_SUCCESS) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: ldap_url_parse failed: %s", ldap_err2string(rc)); return NGX_ERROR; }
        ngx_url.url.data = (u_char *) context->lud->lud_host;
        ngx_url.url.len = ngx_strlen(context->lud->lud_host);
        ngx_url.default_port = context->lud->lud_port;
        if (ngx_parse_url(r->pool, &ngx_url) != NGX_OK) {
            if (ngx_url.err) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: %s in LDAP hostname \"%V\"", ngx_url.err, &ngx_url.url); }
            else { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: %s:%d", __FILE__, __LINE__); }
            ldap_free_urldesc(context->lud);
            return NGX_ERROR;
        }
        ngx_addr_t *addr = &ngx_url.addrs[ngx_random() % ngx_url.naddrs];
        context->peer_connection.sockaddr = addr->sockaddr;
        context->peer_connection.socklen = addr->socklen;
        context->peer_connection.name = &addr->name;
        context->peer_connection.get = ngx_event_get_peer;
        context->peer_connection.log = r->connection->log;
        context->peer_connection.log_error = r->connection->log_error;
        {
            ngx_int_t rc = ngx_event_connect_peer(&context->peer_connection);
            if (rc == NGX_ERROR || rc == NGX_BUSY || rc == NGX_DECLINED) {
                if (context->peer_connection.connection) ngx_close_connection(context->peer_connection.connection);
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ldap: Unable to connect to LDAP server \"%V\"", &addr->name);
                ldap_free_urldesc(context->lud);
                return NGX_ERROR;
            }
        }
        context->peer_connection.connection->log = r->connection->log;
        context->peer_connection.connection->log_error = r->connection->log_error;
        context->peer_connection.connection->read->handler = ngx_http_auth_basic_ldap_read_handler;
        context->peer_connection.connection->write->handler = ngx_http_auth_basic_ldap_write_handler;
        context->peer_connection.connection->read->log = r->connection->log;
        context->peer_connection.connection->write->log = r->connection->log;
        context->peer_connection.connection->data = r;
        context->rc = NGX_AGAIN;
        ngx_http_set_ctx(r, context, ngx_http_auth_basic_ldap_module);
    } else if (context->rc != NGX_AGAIN) {
        if (context->lud) { ldap_free_urldesc(context->lud); context->lud = NULL; }
        if (context->result) { ldap_msgfree(context->result); context->result = NULL; }
        if (context->peer_connection.connection) { ngx_close_connection(context->peer_connection.connection); context->peer_connection.connection = NULL; }
        if (context->ldap) { ldap_unbind_ext(context->ldap, NULL, NULL); context->ldap = NULL; }
    }
    return context->rc;
}

static ngx_int_t ngx_http_auth_basic_ldap_postconfiguration(ngx_conf_t *cf) {
    ngx_http_core_main_conf_t *core_main_conf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    ngx_http_handler_pt *handler = ngx_array_push(&core_main_conf->phases[NGX_HTTP_ACCESS_PHASE].handlers);
    if (!handler) return NGX_ERROR;
    *handler = ngx_http_auth_basic_ldap_handler;
    return NGX_OK;
}

static void *ngx_http_auth_basic_ldap_create_loc_conf(ngx_conf_t *cf) {
    ngx_http_auth_basic_ldap_location_conf_t *location_conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_auth_basic_ldap_location_conf_t));
    if (!location_conf) return NULL;
    return location_conf;
}

static char *ngx_http_auth_basic_ldap_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child) {
    ngx_http_auth_basic_ldap_location_conf_t *prev = parent;
    ngx_http_auth_basic_ldap_location_conf_t *conf = child;
    if (!conf->bind) conf->bind = prev->bind;
    if (!conf->header) conf->header = prev->header;
    if (!conf->realm) conf->realm = prev->realm;
    if (!conf->url) conf->url = prev->url;
    return NGX_CONF_OK;
}

static ngx_http_module_t ngx_http_auth_basic_ldap_ctx = {
    .preconfiguration = NULL,
    .postconfiguration = ngx_http_auth_basic_ldap_postconfiguration,
    .create_main_conf = NULL,
    .init_main_conf = NULL,
    .create_srv_conf = NULL,
    .merge_srv_conf = NULL,
    .create_loc_conf = ngx_http_auth_basic_ldap_create_loc_conf,
    .merge_loc_conf = ngx_http_auth_basic_ldap_merge_loc_conf
};

ngx_module_t ngx_http_auth_basic_ldap_module = {
    NGX_MODULE_V1,
    .ctx = &ngx_http_auth_basic_ldap_ctx,
    .commands = ngx_http_auth_basic_ldap_commands,
    .type = NGX_HTTP_MODULE,
    .init_master = NULL,
    .init_module = NULL,
    .init_process = NULL,
    .init_thread = NULL,
    .exit_thread = NULL,
    .exit_process = NULL,
    .exit_master = NULL,
    NGX_MODULE_V1_PADDING
};
