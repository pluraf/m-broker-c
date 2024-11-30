/* SPDX-License-Identifier: BSD-3-Clause */

/*
Copyright (c) 2024 Pluraf Embedded AB <code@pluraf.com>

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS”
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
Contributors:
    Konstantin Tyurin <konstantin@pluraf.com>
    Gökçe Yetiser Vural <gokce@pluraf.com>
*/

#include "civetweb/civetweb.h"

#include "uthash.h"

#include "mosquitto_broker.h"
#include "mosquitto_broker_internal.h"

#include "jwt/jwt.h"
#include "jwt/jwt_helpers.h"

#include <string.h>


static int handler(struct mg_connection * conn, void * ignored)
{
    char *response;
    uint8_t buf[1024] = {0};

    mg_read(conn, buf, sizeof(buf));  // TODO: Read until 0 or -1

    struct mosquitto__callback *cb_found;
    struct mosquitto_evt_control event_data;
    struct mosquitto__security_options *opts = &db.config->security_options;
    mosquitto_property *properties = NULL;

    const char * topic = "$CONTROL/dynamic-security/v1";
    HASH_FIND(hh, opts->plugin_callbacks.control, topic, strlen(topic), cb_found);
    if(cb_found){
        memset(&event_data, 0, sizeof(event_data));
        event_data.client = NULL;
        event_data.topic = topic;
        event_data.payload = buf;
        event_data.payloadlen = strlen((char*)buf);
        event_data.qos = 0;
        event_data.retain = 0;
        event_data.properties = NULL;
        event_data.reason_code = 0;
        event_data.reason_string = NULL;
        int rc = cb_found->cb(MOSQ_EVT_CONTROL, &event_data, &response);
        free(event_data.reason_string);
    }

    unsigned long len = (unsigned long)strlen(response);

    mg_send_http_ok(conn, "application/json", len);

    mg_write(conn, response, len);
    free(response);

    return 200;  // HTTP state 200 = OK
}


int auth_handler(struct mg_connection * conn, void * cbdata)
{
    int authorized = 0;

    point_t *public_key = (point_t *)cbdata;

    char const * auth_token = mg_get_header(conn, "Authorization");  // TODO: Validate JWT Token
    if(auth_token != NULL && strlen(auth_token) > 7){
        const char *token = auth_token + 7;  // skip prefix (Bearer)

        if (jwt_verify(token, public_key) == 1) {
            authorized = 1;
        }
    }

    if(authorized) {
        return 1;
    } else {
        mg_send_http_error(conn, 401, "");
        return 0;
    }
}


struct mg_context * start_server()
{
    struct mg_context *ctx;
    ecc_init();

    char const * pem_public_key = read_file_content(db.config->http_api_pkey_file);
    if(pem_public_key == NULL){
        mosquitto_log_printf(MOSQ_LOG_ERR, "Failure reading http_api_pkey_file!");
        return NULL;
    }

    point_t *public_key = malloc(sizeof(point_t));
    public_key_from_pem(pem_public_key, public_key);
    free((void*)pem_public_key);

    mg_init_library(0);
    char const * options[] = {"listening_ports", "8001", NULL};
    ctx = mg_start(NULL, 0, options);

    mg_set_request_handler(ctx, "/command$", handler, NULL);
    mg_set_auth_handler(ctx, "/**", auth_handler, public_key);

    return ctx;
}


void stop_server(struct mg_context * ctx)
{
    if(ctx == NULL) return;

    point_t *public_key = (point_t *)mg_get_user_data(ctx);
    if(public_key){
        free(public_key);
    }

    /* Stop the server */
    mg_stop(ctx);

    /* Un-initialize the library */
    mg_exit_library();
}
