/*
 * This file is part of the Soletta (TM) Project
 *
 * Copyright (C) 2015 Intel Corporation. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <errno.h>
#include <float.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <ctype.h>

#define SOL_LOG_DOMAIN &_lwm2m_bs_server_domain

#include "sol-log-internal.h"
#include "sol-util-internal.h"
#include "sol-list.h"
#include "sol-lwm2m.h"
#include "sol-lwm2m-common.h"
#include "sol-lwm2m-bs-server.h"
#include "sol-macros.h"
#include "sol-mainloop.h"
#include "sol-monitors.h"
#include "sol-random.h"
#include "sol-str-slice.h"
#include "sol-str-table.h"
#include "sol-util.h"
#include "sol-http.h"

SOL_LOG_INTERNAL_DECLARE_STATIC(_lwm2m_bs_server_domain, "lwm2m-bs-server");

struct sol_lwm2m_bootstrap_server {
    struct sol_coap_server *coap;
    struct sol_ptr_vector clients;
    struct sol_monitors bootstrap;
    const char **known_clients;
};

struct sol_lwm2m_bootstrap_client_info {
    char *name;
    struct sol_network_link_addr cliaddr;
};

enum bootstrap_type {
    BOOTSTRAP_DELETE,
    BOOTSTRAP_WRITE
};

struct bootstrap_ctx {
    enum bootstrap_type type;
    struct sol_lwm2m_bootstrap_server *server;
    struct sol_lwm2m_bootstrap_client_info *cinfo;
    char *path;
    void *cb;
    const void *data;
};

static void
dispatch_bootstrap_event_to_server(struct sol_lwm2m_bootstrap_server *server,
    struct sol_lwm2m_bootstrap_client_info *bs_cinfo)
{
    uint16_t i;
    struct sol_monitors_entry *m;

    SOL_MONITORS_WALK (&server->bootstrap, m, i)
        ((void (*)(void *, struct sol_lwm2m_bootstrap_server *,
        struct sol_lwm2m_bootstrap_client_info *))m->cb)((void *)m->data, server,
            bs_cinfo);
}

static void
bootstrap_client_info_del(struct sol_lwm2m_bootstrap_client_info *bs_cinfo)
{
    free(bs_cinfo->name);
    free(bs_cinfo);
}

static int
extract_bootstrap_client_info(struct sol_coap_packet *req,
    struct sol_str_slice *client_name)
{
    struct sol_str_slice query;
    int r;

    r = sol_coap_find_options(req, SOL_COAP_OPTION_URI_QUERY, &query,
        LWM2M_BOOTSTRAP_QUERY_PARAMS);
    SOL_INT_CHECK(r, < 0, r);

    struct sol_str_slice key, value;
    const char *sep;

    SOL_DBG("Query:%.*s", SOL_STR_SLICE_PRINT(query));
    sep = memchr(query.data, '=', query.len);

    if (!sep) {
        SOL_WRN("Could not find the separator '=' at: %.*s",
            SOL_STR_SLICE_PRINT(query));
        return -EINVAL;
    }

    key.data = query.data;
    key.len = sep - query.data;
    value.data = sep + 1;
    value.len = query.len - key.len - 1;

    if (sol_str_slice_str_eq(key, "ep")) {
        //Required info
        *client_name = value;
    } else {
        SOL_WRN("The client did not provide its name!");
        return -EINVAL;
    }

    return 0;
}

static int
new_bootstrap_client_info(struct sol_lwm2m_bootstrap_client_info **bs_cinfo,
    const struct sol_network_link_addr *cliaddr, const struct sol_str_slice client_name)
{
    *bs_cinfo = calloc(1, sizeof(struct sol_lwm2m_bootstrap_client_info));
    SOL_NULL_CHECK(bs_cinfo, -ENOMEM);

    memcpy(&(*bs_cinfo)->cliaddr, cliaddr, sizeof(struct sol_network_link_addr));

    (*bs_cinfo)->name = sol_str_slice_to_str(client_name);
    SOL_NULL_CHECK_GOTO((*bs_cinfo)->name, err_no_name);
    return 0;

err_no_name:
    free(*bs_cinfo);
    return -ENOMEM;
}

static int
bootstrap_request(void *data, struct sol_coap_server *coap,
    const struct sol_coap_resource *resource,
    struct sol_coap_packet *req,
    const struct sol_network_link_addr *cliaddr)
{
    struct sol_lwm2m_bootstrap_client_info *bs_cinfo;
    struct sol_lwm2m_bootstrap_server *server = data;
    struct sol_coap_packet *response;
    struct sol_str_slice client_name = SOL_STR_SLICE_EMPTY;
    int r;
    size_t i;
    bool know_client = false;

    SOL_DBG("Client Bootstrap Request received");

    response = sol_coap_packet_new(req);
    SOL_NULL_CHECK(response, -ENOMEM);

    r = extract_bootstrap_client_info(req, &client_name);
    SOL_INT_CHECK_GOTO(r, < 0, err_exit);

    for (i = 0; server->known_clients[i]; i++) {
        if (sol_str_slice_str_eq(client_name, server->known_clients[i]))
            know_client = true;
    }

    if (!know_client) {
        SOL_WRN("Client %.*s bootstrap request received, but this Bootstrap Server"
            " doesn't have Bootstrap Information for this client.",
            SOL_STR_SLICE_PRINT(client_name));
        goto err_exit;
    }

    r = new_bootstrap_client_info(&bs_cinfo, cliaddr, client_name);
    SOL_INT_CHECK_GOTO(r, < 0, err_exit);

    r = sol_ptr_vector_append(&server->clients, bs_cinfo);
    SOL_INT_CHECK_GOTO(r, < 0, err_exit_del_client);


    r = sol_coap_header_set_code(response, SOL_COAP_RESPONSE_CODE_CHANGED);
    SOL_INT_CHECK_GOTO(r, < 0, err_exit_del_client_list);

    SOL_DBG("Client %s bootstrap request received."
        " Bootstrap Process will start now.", bs_cinfo->name);

    r = sol_coap_send_packet(coap, response, cliaddr);
    dispatch_bootstrap_event_to_server(server, bs_cinfo);

    return r;

err_exit_del_client_list:
    sol_ptr_vector_remove(&server->clients, bs_cinfo);
err_exit_del_client:
    bootstrap_client_info_del(bs_cinfo);
err_exit:
    sol_coap_header_set_code(response, SOL_COAP_RESPONSE_CODE_BAD_REQUEST);
    sol_coap_send_packet(coap, response, cliaddr);
    return r;
}

static const struct sol_coap_resource bootstrap_request_interface = {
    SOL_SET_API_VERSION(.api_version = SOL_COAP_RESOURCE_API_VERSION, )
    .post = bootstrap_request,
    .flags = SOL_COAP_FLAGS_NONE,
    .path = {
        SOL_STR_SLICE_LITERAL("bs"),
        SOL_STR_SLICE_EMPTY
    }
};

SOL_API struct sol_lwm2m_bootstrap_server *
sol_lwm2m_bootstrap_server_new(uint16_t port, const char **known_clients)
{
    struct sol_lwm2m_bootstrap_server *server;
    struct sol_network_link_addr servaddr = { .family = SOL_NETWORK_FAMILY_INET6,
                                              .port = port };
    int r;

    SOL_LOG_INTERNAL_INIT_ONCE;

    SOL_NULL_CHECK(known_clients, NULL);

    server = calloc(1, sizeof(struct sol_lwm2m_bootstrap_server));
    SOL_NULL_CHECK(server, NULL);

    server->coap = sol_coap_server_new(&servaddr, false);
    SOL_NULL_CHECK_GOTO(server->coap, err_coap);

    server->known_clients = known_clients;

    sol_ptr_vector_init(&server->clients);

    sol_monitors_init(&server->bootstrap, NULL);

    r = sol_coap_server_register_resource(server->coap,
        &bootstrap_request_interface, server);
    SOL_INT_CHECK_GOTO(r, < 0, err_register);

    return server;

err_register:
    sol_coap_server_unref(server->coap);
err_coap:
    free(server);
    return NULL;
}

SOL_API void
sol_lwm2m_bootstrap_server_del(struct sol_lwm2m_bootstrap_server *server)
{
    uint16_t i;
    struct sol_lwm2m_bootstrap_client_info *bs_cinfo;

    SOL_NULL_CHECK(server);

    sol_coap_server_unref(server->coap);

    SOL_PTR_VECTOR_FOREACH_IDX (&server->clients, bs_cinfo, i)
        bootstrap_client_info_del(bs_cinfo);

    sol_monitors_clear(&server->bootstrap);
    sol_ptr_vector_clear(&server->clients);
    free(server);
}

SOL_API int
sol_lwm2m_bootstrap_server_add_request_monitor(struct sol_lwm2m_bootstrap_server *server,
    void (*cb)(void *data,
    struct sol_lwm2m_bootstrap_server *server,
    struct sol_lwm2m_bootstrap_client_info *bs_cinfo),
    const void *data)
{
    SOL_NULL_CHECK(server, -EINVAL);

    return add_to_monitors(&server->bootstrap, (sol_monitors_cb_t)cb, data);
}

SOL_API int
sol_lwm2m_bootstrap_server_del_request_monitor(struct sol_lwm2m_bootstrap_server *server,
    void (*cb)(void *data,
    struct sol_lwm2m_bootstrap_server *server,
    struct sol_lwm2m_bootstrap_client_info *bs_cinfo),
    const void *data)
{
    SOL_NULL_CHECK(server, -EINVAL);

    return remove_from_monitors(&server->bootstrap, (sol_monitors_cb_t)cb, data);
}

SOL_API const char *
sol_lwm2m_bootstrap_client_info_get_name(const struct sol_lwm2m_bootstrap_client_info *client)
{
    SOL_NULL_CHECK(client, NULL);

    return client->name;
}

SOL_API const struct sol_network_link_addr *
sol_lwm2m_bootstrap_client_info_get_address(const struct sol_lwm2m_bootstrap_client_info *client)
{
    SOL_NULL_CHECK(client, NULL);

    return &client->cliaddr;
}

static bool
bootstrap_reply(void *data, struct sol_coap_server *server,
    struct sol_coap_packet *req, const struct sol_network_link_addr *cliaddr)
{
    struct bootstrap_ctx *ctx = data;
    uint8_t code = 0;

    if (!cliaddr && !req)
        code = SOL_COAP_RESPONSE_CODE_GATEWAY_TIMEOUT;

    if (!code)
        sol_coap_header_get_code(req, &code);
    ((void (*)(void *,
    struct sol_lwm2m_bootstrap_server *,
    struct sol_lwm2m_bootstrap_client_info *, const char *,
    enum sol_coap_response_code))ctx->cb)
        ((void *)ctx->data, ctx->server, ctx->cinfo, ctx->path, code);

    if (code != SOL_COAP_RESPONSE_CODE_GATEWAY_TIMEOUT)
        send_ack_if_needed(server, req, cliaddr);
    free(ctx->path);
    free(ctx);
    return false;
}

static int
send_bootstrap_packet(struct sol_lwm2m_bootstrap_server *server,
    struct sol_lwm2m_bootstrap_client_info *client, const char *path,
    enum bootstrap_type type, void *cb, const void *data,
    enum sol_coap_method method,
    struct sol_lwm2m_resource *resources,
    struct sol_lwm2m_resource **instances, size_t *instances_len,
    uint16_t *instances_ids, size_t len)
{
    int r;
    struct sol_coap_packet *pkt;
    struct bootstrap_ctx *ctx;

    r = setup_coap_packet(method, SOL_COAP_MESSAGE_TYPE_CON,
        NULL, path, NULL, NULL, resources, instances, instances_len,
        instances_ids, len, NULL, &pkt);
    SOL_INT_CHECK(r, < 0, r);

    if (!cb)
        return sol_coap_send_packet(server->coap, pkt, &client->cliaddr);

    ctx = malloc(sizeof(struct bootstrap_ctx));
    SOL_NULL_CHECK_GOTO(ctx, err_exit);

    ctx->path = strdup(path);
    SOL_NULL_CHECK_GOTO(ctx->path, err_exit);
    ctx->type = type;
    ctx->server = server;
    ctx->cinfo = client;
    ctx->data = data;
    ctx->cb = cb;

    return sol_coap_send_packet_with_reply(server->coap, pkt, &client->cliaddr,
        bootstrap_reply, ctx);

err_exit:
    free(ctx);
    sol_coap_packet_unref(pkt);
    return -ENOMEM;
}

SOL_API int
sol_lwm2m_bootstrap_server_write_object(struct sol_lwm2m_bootstrap_server *server,
    struct sol_lwm2m_bootstrap_client_info *client, const char *path,
    struct sol_lwm2m_resource **instances, size_t *instances_len,
    uint16_t *instances_ids, size_t len,
    void (*cb)(void *data,
    struct sol_lwm2m_bootstrap_server *server,
    struct sol_lwm2m_bootstrap_client_info *client, const char *path,
    enum sol_coap_response_code response_code),
    const void *data)
{
    SOL_NULL_CHECK(server, -EINVAL);
    SOL_NULL_CHECK(client, -EINVAL);
    SOL_NULL_CHECK(path, -EINVAL);

    return send_bootstrap_packet(server, client, path,
        BOOTSTRAP_WRITE, cb, data, SOL_COAP_METHOD_PUT, NULL, instances,
        instances_len, instances_ids, len);
}

SOL_API int
sol_lwm2m_bootstrap_server_write(struct sol_lwm2m_bootstrap_server *server,
    struct sol_lwm2m_bootstrap_client_info *client, const char *path,
    struct sol_lwm2m_resource *resources, size_t len,
    void (*cb)(void *data,
    struct sol_lwm2m_bootstrap_server *server,
    struct sol_lwm2m_bootstrap_client_info *client, const char *path,
    enum sol_coap_response_code response_code),
    const void *data)
{
    SOL_NULL_CHECK(server, -EINVAL);
    SOL_NULL_CHECK(client, -EINVAL);
    SOL_NULL_CHECK(path, -EINVAL);

    return send_bootstrap_packet(server, client, path,
        BOOTSTRAP_WRITE, cb, data, SOL_COAP_METHOD_PUT, resources, NULL, NULL, NULL, len);
}

SOL_API int
sol_lwm2m_bootstrap_server_delete_object_instance(struct sol_lwm2m_bootstrap_server *server,
    struct sol_lwm2m_bootstrap_client_info *client, const char *path,
    void (*cb)(void *data,
    struct sol_lwm2m_bootstrap_server *server,
    struct sol_lwm2m_bootstrap_client_info *client, const char *path,
    enum sol_coap_response_code response_code),
    const void *data)
{
    SOL_NULL_CHECK(server, -EINVAL);
    SOL_NULL_CHECK(client, -EINVAL);
    SOL_NULL_CHECK(path, -EINVAL);

    return send_bootstrap_packet(server, client, path,
        BOOTSTRAP_DELETE, cb, data, SOL_COAP_METHOD_DELETE, NULL, NULL, NULL, NULL, 0);
}

SOL_API int
sol_lwm2m_bootstrap_server_send_finish(struct sol_lwm2m_bootstrap_server *server,
    struct sol_lwm2m_bootstrap_client_info *client)
{
    struct sol_coap_packet *pkt;
    int r;

    SOL_NULL_CHECK(server, -EINVAL);
    SOL_NULL_CHECK(client, -EINVAL);

    pkt = sol_coap_packet_new_request(SOL_COAP_METHOD_POST, SOL_COAP_MESSAGE_TYPE_CON);
    r = -ENOMEM;
    SOL_NULL_CHECK_GOTO(pkt, err_exit);

    r = sol_coap_add_option(pkt, SOL_COAP_OPTION_URI_PATH, "bs", strlen("bs"));
    SOL_INT_CHECK_GOTO(r, < 0, err_coap);

    SOL_DBG("Sending Bootstrap Finish to LWM2M Client %s", client->name);
    r = sol_coap_send_packet(server->coap, pkt, &client->cliaddr);

    r = sol_ptr_vector_remove(&server->clients, client);
    if (r < 0)
        SOL_WRN("Could not remove the client %s from the clients list",
            client->name);
    bootstrap_client_info_del(client);

    return r;

err_coap:
    sol_coap_packet_unref(pkt);
err_exit:
    return r;
}
