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
*/


#include <pthread.h>
#include <zmq.h>

#include "API_VERSION.h"
#include "zmq_listener.h"
#include "zmq_api.h"
#undef WITH_BROKER  // FIXME:
#include "mosquitto_broker_internal.h"



void * g_context;
pthread_t g_zmq_th;


struct payload_t handle_request(unsigned char * command, size_t command_len){
    return zmq_api_handler(command, command_len);
}


void * zmq_listener(void * arg){
    g_context = zmq_ctx_new();
    void * responder = zmq_socket(g_context, ZMQ_REP);
    int rc = zmq_bind(responder, "ipc:///tmp/mqbc-zmq.sock");
    if(rc != 0) return NULL;

    unsigned char in_buffer[700];
    unsigned char out_buffer[100];
    while(1){
        int num = zmq_recv(responder, in_buffer, sizeof(in_buffer) - 1, 0);
        if(num == -1 && zmq_errno() == ETERM){
            break;
        }
        if(num == 0){
            zmq_send(responder, &db.config->security_options.allow_anonymous, 1, 0);
        }else if(num == 1){
            config__update_allow_anonymous(db.config, in_buffer[0]);
            config__write(db.config);
            zmq_send(responder, NULL, 0, 0);
        }else{
            in_buffer[num] = '\0';
            struct payload_t response = handle_request(in_buffer, num);
            zmq_send(responder, response.data, response.len, 0);
            free_payload(& response);
        }
    }
    zmq_close(responder);
    return NULL;
}


void start_zmq_listener(){
    if(pthread_create(&g_zmq_th, 0, zmq_listener, 0)){
        printf("Error creating listener thread\n");
    }
}


void stop_zmq_listener(){
    zmq_ctx_term(g_context);
    pthread_join(g_zmq_th, NULL);
}