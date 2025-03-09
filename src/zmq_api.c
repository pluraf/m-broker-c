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


#include "mosquitto_broker.h"
#include "mosquitto_broker_internal.h"

#include <cjson/cJSON.h>
#include <cbor.h>
#include <string.h>
#include <stdio.h>

#include "zmq_api.h"


struct payload_t make_payload_dynamic(void * data, size_t len){
    return (struct payload_t){(unsigned char *)data, len, 1};
}


struct payload_t make_payload_static(void * data, size_t len){
    return (struct payload_t){(unsigned char *)data, len, 0};
}


char * mqbc_execute_command(char const * command);
struct payload_t zmq_channel_get(char const * path, unsigned char const * payload, size_t payload_len);
struct payload_t zmq_channel_put(char const * path, unsigned char const * payload, size_t payload_len);
struct payload_t zmq_channel_post(char const * path, unsigned char const * payload, size_t payload_len);
struct payload_t zmq_channel_delete(char const * path, unsigned char const * payload, size_t payload_len);


void free_payload(struct payload_t * payload)
{
    if(payload->dynamic){
        free(payload->data);
        struct payload_t empty = {0};
        *payload = empty;
    }
}


struct payload_t describe_error(char const * descr){
    struct payload_t description = {0};
    if(descr != NULL){
        size_t len = strlen(descr);
        description.data = malloc(len);
        description.dynamic = 1;
        memcpy(description.data, descr, len);
        description.len = len;
    }else {
        description.data = (unsigned char *)"Error";
        description.len = 5;
        description.dynamic = 0;
    }
    return description;
}


struct payload_t execute_request(char const * method, char const * path, unsigned char const * payload, size_t payload_len){
    printf("%s - %s\n", method, path);
    if(strcmp(method, "GET") == 0 || strcmp(method, "get") == 0){
        return zmq_channel_get(path, payload, payload_len);
    }else if(strcmp(method, "PUT") == 0 || strcmp(method, "put") == 0){
        return zmq_channel_put(path, payload, payload_len);
    }else if(strcmp(method, "POST") == 0 || strcmp(method, "post") == 0){
        return zmq_channel_post(path, payload, payload_len);
    }else if(strcmp(method, "DELETE") == 0 || strcmp(method, "delete") == 0){
        return zmq_channel_delete(path, payload, payload_len);
    }else{
        return describe_error("Unknown method");
    }
}


struct payload_t zmq_api_handler(unsigned char * buffer, size_t len)
{
    struct cbor_load_result result;
    cbor_item_t * request = cbor_load(buffer, len, &result);

    if(request == NULL) return describe_error(NULL);
    if(! cbor_isa_array(request)){
        cbor_decref(&request);
        return describe_error(NULL);
    }

    size_t array_size = cbor_array_size(request);
    if(array_size < 2){
        cbor_decref(&request);
        return describe_error(NULL);
    }
    cbor_item_t ** items = cbor_array_handle(request);
    // Retrieve method
    cbor_item_t * item = items[0];
    if(! cbor_isa_string(item)){
        cbor_decref(&request);
        return describe_error(NULL);
    }
    char method[20] = {0};
    if(cbor_string_length(item) >= 20){
        cbor_decref(&request);
        return describe_error(NULL);
    }
    strncpy(method, (char *)cbor_string_handle(item), cbor_string_length(item));
    // Retrieve path
    item = items[1];
    if(! cbor_isa_string(item)){
        cbor_decref(&request);
        return describe_error(NULL);
    }
    char path[200] = {0};
    if(cbor_string_length(item) >= 200){
        cbor_decref(&request);
        return describe_error("Path longer than 200 bytes");
    }
    strncpy(path, (char *)cbor_string_handle(item), cbor_string_length(item));
    // Retrieve payload
    unsigned char * payload = NULL;
    size_t payload_len = 0;
    if(array_size > 2){
        item = items[2];
        if(cbor_isa_bytestring(item)){
            int d = cbor_bytestring_is_definite(item);
            payload = cbor_bytestring_handle(item);
            payload_len = cbor_bytestring_length(item);
        }else if(cbor_isa_string(item)){
            payload = cbor_string_handle(item);
            payload_len = cbor_string_length(item);
        }else{
            cbor_decref(&request);
            return describe_error(NULL);
        }
    }

    struct payload_t response = execute_request(method, path, payload, payload_len);
    cbor_decref(&request);
    return response;
}


struct payload_t zmq_channel_get(char const * path, unsigned char const * payload, size_t payload_len)
{
    struct payload_t response = {0};
    cJSON * pipeline_data;
    const char * last_segment = strrchr(path, '/');
    if(last_segment && strlen(last_segment) > 1){
        char const * chanid = last_segment + 1;
        char command[200];
        snprintf(
            command,
            sizeof(command),
            "{\"commands\":[{\"command\":\"getChannel\",\"chanid\":\"%s\"}]}", chanid
        );
        char * result = mqbc_execute_command(command);
        cJSON * j_result = cJSON_Parse(result);
        cJSON * j_tmp = cJSON_GetArrayItem(cJSON_GetObjectItem(j_result, "responses") , 0);
        if(cJSON_GetObjectItem(j_tmp, "error")){
            response = describe_error("Not Found");
        }else{
            cJSON * channel = cJSON_GetObjectItem(cJSON_GetObjectItem(j_tmp, "data"), "channel");
            char * payload = cJSON_PrintUnformatted(channel);
            response = make_payload_dynamic(payload, strlen(payload));
        }
        free(result);
        cJSON_Delete(j_result);
    }else{
        char * command = "{\"commands\":[{\"command\":\"listChannels\",\"verbose\":false}]}";
        char * result = mqbc_execute_command(command);
        cJSON * j_result = cJSON_Parse(result);
        cJSON * channels = cJSON_GetObjectItem(
            cJSON_GetObjectItem(
                cJSON_GetArrayItem(cJSON_GetObjectItem(j_result, "responses") , 0),
                "data"
            ),
            "channels"
        );
        char * payload = cJSON_PrintUnformatted(channels);
        response = make_payload_dynamic(payload, strlen(payload));
        free(result);
        cJSON_Delete(j_result);
    }
    return response;
}


struct payload_t zmq_channel_put(char const * path, unsigned char const * payload, size_t payload_len)
{
    struct payload_t response = {0};
    cJSON * pipeline_data;

    const char * last_segment = strrchr(path, '/');
    if(last_segment && strlen(last_segment) > 1){
        char const * chanid = last_segment + 1;
        uint8_t buf[1024] = {0};
        cJSON * j_payload = cJSON_ParseWithLength((char const *)payload, payload_len);
        if(j_payload){
            cJSON * j_wrapper = cJSON_CreateObject();
            cJSON * j_commands =  cJSON_AddArrayToObject(j_wrapper, "commands");
            cJSON_AddStringToObject(j_payload, "command", "modifyChannel");
            cJSON_AddStringToObject(j_payload, "chanid", chanid);
            cJSON_AddItemToArray(j_commands, j_payload);
            char * command = cJSON_PrintUnformatted(j_wrapper);
            mosquitto_log_printf(MOSQ_LOG_ERR, command);
            char * result = mqbc_execute_command(command);
            free(command);
            free(j_wrapper);
            cJSON * j_result = cJSON_Parse(result);
            cJSON * j_tmp = cJSON_GetArrayItem(cJSON_GetObjectItem(j_result, "responses") , 0);
            cJSON * error = cJSON_GetObjectItem(j_tmp, "error");
            if(error){
                char const * error_str = cJSON_GetStringValue(error);
                response = describe_error(error_str);
            }
            free(result);
            cJSON_Delete(j_result);
        }else{
            response = describe_error("Invalid Request");
        }
    }else{
        response = describe_error("Invalid Request");
    }
    return response;
}


struct payload_t zmq_channel_post(char const * path, unsigned char const * payload, size_t payload_len)
{
    struct payload_t response = {0};
    cJSON * pipeline_data;

    const char * last_segment = strrchr(path, '/');
    if(last_segment && strlen(last_segment) > 1){
        char const * chanid = last_segment + 1;
        cJSON * j_payload = cJSON_ParseWithLength((char const *)payload, payload_len);
        if(j_payload){
            cJSON * j_wrapper = cJSON_CreateObject();
            cJSON * j_commands =  cJSON_AddArrayToObject(j_wrapper, "commands");
            cJSON_AddStringToObject(j_payload, "command", "createChannel");
            cJSON_AddStringToObject(j_payload, "chanid", chanid);
            cJSON_AddItemToArray(j_commands, j_payload);
            char * command = cJSON_PrintUnformatted(j_wrapper);
            mosquitto_log_printf(MOSQ_LOG_ERR, command);
            char * result = mqbc_execute_command(command);
            free(command);
            free(j_wrapper);
            cJSON * j_result = cJSON_Parse(result);
            cJSON * j_tmp = cJSON_GetArrayItem(cJSON_GetObjectItem(j_result, "responses") , 0);
            cJSON * error = cJSON_GetObjectItem(j_tmp, "error");
            if(error){
                char const * error_str = cJSON_GetStringValue(error);
                response = describe_error(error_str);
            }
            free(result);
            cJSON_Delete(j_result);
        }else{
            response = describe_error("Invalid Request");
        }
    }else{
        response = describe_error("Invalid Request");
    }
    return response;
}


struct payload_t zmq_channel_delete(char const * path, unsigned char const * payload, size_t payload_len)
{
    struct payload_t response = {0};
    cJSON * pipeline_data;

    const char * last_segment = strrchr(path, '/');
    if(last_segment && strlen(last_segment) > 1){
        char const * chanid = last_segment + 1;
        cJSON * j_wrapper = cJSON_CreateObject();
        cJSON * j_commands =  cJSON_AddArrayToObject(j_wrapper, "commands");

        cJSON * j_command = cJSON_CreateObject();
        cJSON_AddItemReferenceToObject(
            j_command, "channels", cJSON_CreateStringArray(& chanid, 1)
        );
        cJSON_AddStringToObject(j_command, "command", "deleteChannels");
        cJSON_AddItemReferenceToArray(j_commands, j_command);
        char * command = cJSON_PrintUnformatted(j_wrapper);
        char * result = mqbc_execute_command(command);
        free(command);
        free(j_wrapper);
        cJSON * j_result = cJSON_Parse(result);
        cJSON * j_tmp = cJSON_GetArrayItem(cJSON_GetObjectItem(j_result, "responses") , 0);
        cJSON * error = cJSON_GetObjectItem(j_tmp, "error");
        if(error){
            char const * error_str = cJSON_GetStringValue(error);
            response = describe_error(error_str);
        }
        free(result);
        cJSON_Delete(j_result);
    }else{
        response = describe_error("Invalid Request");
    }
    return response;
}


char * mqbc_execute_command(char const * command)
{
    struct mosquitto__callback *cb_found;
    struct mosquitto_evt_control event_data;
    struct mosquitto__security_options *opts = &db.config->security_options;
    mosquitto_property *properties = NULL;
    char * response = NULL;

    const char * topic = "$CONTROL/dynamic-security/v1";
    HASH_FIND(hh, opts->plugin_callbacks.control, topic, strlen(topic), cb_found);
    if(cb_found){
        memset(&event_data, 0, sizeof(event_data));
        event_data.client = NULL;
        event_data.topic = topic;
        event_data.payload = command;
        event_data.payloadlen = strlen(command);
        event_data.qos = 0;
        event_data.retain = 0;
        event_data.properties = NULL;
        event_data.reason_code = 0;
        event_data.reason_string = NULL;
        int rc = cb_found->cb(MOSQ_EVT_CONTROL, &event_data, &response);
        free(event_data.reason_string);
        return response;
    }
    return NULL;
}
