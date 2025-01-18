/* SPDX-License-Identifier: BSD-3-Clause */

/******************************************************************************
Copyright (c) 2020 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of Eclipse Distribution License v1.0
which accompany this distribution.

The Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php

Contributors:
   Roger Light - initial implementation and documentation.

******************************************************************************/

/******************************************************************************
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

===============================================================================

Contributors:
   Konstantin Tyurin <konstantin@pluraf.com>

******************************************************************************/


#include "config.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <uthash.h>

#include "mosquitto.h"
#include "mosquitto_broker.h"
#include "json_help.h"

#include "dynamic_security.h"

/* ################################################################
 * #
 * # Function declarations
 * #
 * ################################################################ */

static int dynsec__remove_channel_from_all_groups(struct dynsec__channel * channel);
static void channel__remove_all_roles(struct dynsec__channel * channel);

/* ################################################################
 * #
 * # Local variables
 * #
 * ################################################################ */

static struct dynsec__channel * local_username_channels = NULL;
static struct dynsec__channel * local_clientid_channels = NULL;
static struct dynsec__channel * local_channels = NULL;

/* ################################################################
 * #
 * # Utility functions
 * #
 * ################################################################ */

static int channel_cmp_by_chanid(void *a, void *b)
{
    struct dynsec__channel *channel_a = a;
    struct dynsec__channel *channel_b = b;

    return strcmp(channel_a->chanid, channel_b->chanid);
}

static int channel_cmp_by_clientid(void *a, void *b)
{
    struct dynsec__channel *channel_a = a;
    struct dynsec__channel *channel_b = b;

    return strcmp(channel_a->clientid, channel_b->clientid);
}

static int channel_cmp_by_username(void *a, void *b)
{
    struct dynsec__channel *channel_a = a;
    struct dynsec__channel *channel_b = b;

    return strcmp(channel_a->username, channel_b->username);
}


int dynsec_channels__add_check_uniqueness(
    const char * chanid, const char * clientid, const char * username, const char * selfid
){
    struct dynsec__channel * existing = NULL;

    if (chanid) {
        HASH_FIND(hh, local_channels, chanid, strlen(chanid), existing);
        if (existing) return MOSQ_ERR_ALREADY_EXISTS;
    }

    if(clientid){
        HASH_FIND(hh_clientid, local_clientid_channels, clientid, strlen(clientid), existing);
    }else if(username){
        HASH_FIND(hh_username, local_username_channels, username, strlen(username), existing);
    }

    if(existing){
        if(selfid && (strcmp(existing->chanid, selfid) == 0)){
            return MOSQ_ERR_SUCCESS;
        }
        return MOSQ_ERR_ALREADY_EXISTS;
    }
    return MOSQ_ERR_SUCCESS;
}


int dynsec_channels__add_inorder(struct dynsec__channel * channel)
{
    if(dynsec_channels__add_check_uniqueness(channel->chanid, channel->clientid, channel->username, NULL) == MOSQ_ERR_SUCCESS){
        HASH_ADD_KEYPTR_INORDER(hh, local_channels, channel->chanid, strlen(channel->chanid), channel, channel_cmp_by_chanid);
        if(channel->clientid){
            HASH_ADD_KEYPTR_INORDER(hh_clientid, local_clientid_channels, channel->clientid, strlen(channel->clientid), channel, channel_cmp_by_clientid);
        }else if(channel->username){
            HASH_ADD_KEYPTR_INORDER(hh_username, local_username_channels, channel->username, strlen(channel->username), channel, channel_cmp_by_username);
        }
        return MOSQ_ERR_SUCCESS;
    }
    return MOSQ_ERR_ALREADY_EXISTS;
}


int dynsec_channels__add(struct dynsec__channel * channel)
{
    if(dynsec_channels__add_check_uniqueness(channel->chanid, channel->clientid, channel->username, NULL) == MOSQ_ERR_SUCCESS){
        HASH_ADD_KEYPTR(hh, local_channels, channel->chanid, strlen(channel->chanid), channel);
        if(channel->clientid){
            HASH_ADD_KEYPTR(hh_clientid, local_clientid_channels, channel->clientid, strlen(channel->clientid), channel);
        }else if(channel->username){
            HASH_ADD_KEYPTR(hh_username, local_username_channels, channel->username, strlen(channel->username), channel);
        }
        return MOSQ_ERR_SUCCESS;
    }
    return MOSQ_ERR_ALREADY_EXISTS;
}


struct dynsec__channel * dynsec_channels__find(const char * clientid, const char *username)
{
    struct dynsec__channel * channel = NULL;

    if(clientid){
        HASH_FIND(hh_clientid, local_clientid_channels, clientid, strlen(clientid), channel);
        if(channel) return channel;
    }

    if(username){
        HASH_FIND(hh_username, local_username_channels, username, strlen(username), channel);
        if(channel) return channel;
    }

    return NULL;
}

struct dynsec__channel *dynsec_channels__get(const char * chanid)
{
    struct dynsec__channel * channel = NULL;
    if(chanid) HASH_FIND(hh, local_channels, chanid, strlen(chanid), channel);
    return channel;
}


static void channel__unallocate_item(struct dynsec__channel * channel)
{
    if (channel->rolelist) dynsec_rolelist__cleanup(&channel->rolelist);
    if (channel->grouplist) dynsec__remove_channel_from_all_groups(channel);
    mosquitto_free(channel->text_name);
    mosquitto_free(channel->text_description);
    mosquitto_free(channel->clientid);
    mosquitto_free(channel->username);
    mosquitto_free(channel->jwtkey);
    mosquitto_free(channel->chanid);
    mosquitto_free(channel->authtype);
    mosquitto_free(channel);
}


static void channel__free_item(struct dynsec__channel * channel)
{
    struct dynsec__channel *channel_found;
    if(channel == NULL) return;

    channel_found = dynsec_channels__get(channel->chanid);
    if(channel_found){
        HASH_DELETE(hh,local_channels, channel_found);
        if(channel->clientid){
            HASH_DELETE(hh_clientid, local_clientid_channels, channel_found);
        }else if(channel->username){
            HASH_DELETE(hh_username, local_username_channels, channel_found);
        }
    }
    channel__unallocate_item(channel);
}


void dynsec_channels__cleanup(void)
{
    struct dynsec__channel * channel, *channel_tmp;

    HASH_ITER(hh, local_channels, channel, channel_tmp){
        channel__free_item(channel);
    }

    HASH_ITER(hh_clientid, local_clientid_channels, channel, channel_tmp){
        channel__free_item(channel);
    }
    HASH_ITER(hh_username, local_username_channels, channel, channel_tmp){
        channel__free_item(channel);
    }
}


void dynsec_channels__kick_channels(struct dynsec__channel * channel)
{
    if (channel) {
        if(channel->clientid){
            mosquitto_kick_client_by_clientid(channel->clientid, false);
        }else if(channel->username){
            mosquitto_kick_client_by_username(channel->username, false);
        }
    }
}


static void channel__drop_aid_hash(struct dynsec__channel * channel)
{
    if(channel){
        if(channel->clientid){
            HASH_DELETE(hh_clientid, local_clientid_channels, channel);
        }else if(channel->username){
            HASH_DELETE(hh_username, local_username_channels, channel);
        }
    }
}


static void channel__add_aid_hash(struct dynsec__channel * channel)
{
    if(channel){
        if(channel->clientid){
            HASH_ADD_KEYPTR_INORDER(hh_clientid, local_clientid_channels, channel->clientid, strlen(channel->clientid), channel, channel_cmp_by_clientid);
        }else if(channel->username){
            HASH_ADD_KEYPTR_INORDER(hh_username, local_username_channels, channel->username, strlen(channel->username), channel, channel_cmp_by_username);
        }
    }
}


/* ################################################################
 * #
 * # Config file load and save
 * #
 * ################################################################ */

int dynsec_channels__config_load(cJSON *tree)
{
    cJSON *j_channels, *j_channel, *j_roles, *j_role, *j_password;
    struct dynsec__channel * channel;
    struct dynsec__role *role;
    unsigned char *buf;
    int buf_len;
    int priority;

    j_channels = cJSON_GetObjectItem(tree, "channels");
    if(j_channels == NULL){
        return 0;
    }

    if(cJSON_IsArray(j_channels) == false){
        return 1;
    }

    cJSON_ArrayForEach(j_channel, j_channels){
        if(cJSON_IsObject(j_channel) == true){
            // chanid
            char *chanid;
             if (json_get_string(j_channel, "chanid", &chanid, false) != MOSQ_ERR_SUCCESS)continue;

            char *authtype;
            if (json_get_string(j_channel, "authtype", &authtype, false) != MOSQ_ERR_SUCCESS)continue;

            // username
            char *username;
            json_get_string(j_channel, "username", &username, true);

            // clientid
            char * clientid;
            json_get_string(j_channel, "clientid", &clientid, true);

            channel = dynsec_channels__get(chanid);
            if(channel)continue;

            // Create channel
            channel = mosquitto_calloc(1, sizeof(struct dynsec__channel));
            if(channel == NULL){
                return MOSQ_ERR_NOMEM;
            }

            channel->chanid = mosquitto_strdup(chanid);
            if(channel->chanid == NULL){
                channel__unallocate_item(channel);
                continue;
            }

            if(clientid){
                channel->clientid = mosquitto_strdup(clientid);
                if(channel->clientid == NULL){
                    channel__unallocate_item(channel);
                    continue;
                }
            }

            if(username){
                channel->username = mosquitto_strdup(username);
                if(channel->username == NULL){
                    channel__unallocate_item(channel);
                    continue;
                }
            }

            // Time to check for uniqueness
            if(dynsec_channels__add(channel) != MOSQ_ERR_SUCCESS){
                channel__unallocate_item(channel);
                continue;
            }

            // authtype
            channel->authtype = mosquitto_strdup(authtype);
            if(channel->authtype == NULL){
                channel__unallocate_item(channel);
                continue;
            }

            bool disabled;
            if(json_get_bool(j_channel, "disabled", &disabled, false, false) == MOSQ_ERR_SUCCESS){
                channel->disabled = disabled;
            }



            // Password
            if(strcmp(channel->authtype, MQTT_AUTH_PASSWORD) == 0
                    && json_get_object(j_channel, "secret", &j_password, true) == MOSQ_ERR_SUCCESS){
                char *salt, *password;
                int iterations;
                json_get_string(j_password, "salt", &salt, false);
                json_get_string(j_password, "hash", &password, false);
                json_get_int(j_password, "iterations", &iterations, false, -1);

                if(salt && password && iterations > 0){
                    channel->pw.iterations = iterations;

                    if(dynsec_auth__base64_decode(salt, &buf, &buf_len) != MOSQ_ERR_SUCCESS
                            || buf_len != sizeof(channel->pw.salt)){
                        channel__unallocate_item(channel);
                        continue;
                    }
                    memcpy(channel->pw.salt, buf, (size_t)buf_len);
                    mosquitto_free(buf);

                    if(dynsec_auth__base64_decode(password, &buf, &buf_len) != MOSQ_ERR_SUCCESS
                            || buf_len != sizeof(channel->pw.password_hash)){
                        channel__unallocate_item(channel);
                        continue;
                    }
                    memcpy(channel->pw.password_hash, buf, (size_t)buf_len);
                    mosquitto_free(buf);
                    channel->pw.valid = true;
                }else{
                    channel->pw.valid = false;
                }
            }

            // JWT key
            char * jwtkey = NULL;
            if(strncmp(channel->authtype, MQTT_AUTH_KEY_PREFIX, strlen(MQTT_AUTH_KEY_PREFIX)) == 0
                    && json_get_string(j_channel, "secret", &jwtkey, true), jwtkey){
                channel->jwtkey = mosquitto_strdup(jwtkey);
                if(channel->jwtkey == NULL){
                    channel__unallocate_item(channel);
                    continue;
                }
            }

            /* Text name */
            char *textname;
            json_get_string(j_channel, "textname", &textname, false);
            if(textname){
                channel->text_name = mosquitto_strdup(textname);
                if(channel->text_name == NULL){
                    channel__unallocate_item(channel);
                    continue;
                }
            }

            /* Text description */
            char *textdescription;
            json_get_string(j_channel, "textdescription", &textdescription, false);
            if(textdescription){
                channel->text_description = mosquitto_strdup(textdescription);
                if(channel->text_description == NULL){
                    channel__unallocate_item(channel);
                    continue;
                }
            }

            /* Roles */
            j_roles = cJSON_GetObjectItem(j_channel, "roles");
            if(j_roles && cJSON_IsArray(j_roles)){
                cJSON_ArrayForEach(j_role, j_roles){
                    if(cJSON_IsObject(j_role)){
                        char *rolename;
                        json_get_string(j_role, "rolename", &rolename, false);
                        if(rolename){
                            json_get_int(j_role, "priority", &priority, true, -1);
                            role = dynsec_roles__find(rolename);
                            dynsec_rolelist__channel_add(channel, role, priority);
                        }
                    }
                }
            }
        }
    }

    HASH_SRT(hh, local_channels, channel_cmp_by_chanid);
    HASH_SRT(hh_clientid, local_clientid_channels, channel_cmp_by_clientid);
    HASH_SRT(hh_username, local_username_channels, channel_cmp_by_username);

    return 0;
}


static int dynsec__config_add_channels(cJSON *j_channels, struct dynsec__channel * local_channels)
{
    struct dynsec__channel * channel, *channel_tmp;
    cJSON *j_channel, *j_roles, *jtmp;
    char *buf;

    HASH_ITER(hh, local_channels, channel, channel_tmp){
        j_channel = cJSON_CreateObject();
        if(j_channel == NULL) return 1;
        cJSON_AddItemToArray(j_channels, j_channel);

        if((channel->chanid && cJSON_AddStringToObject(j_channel, "chanid", channel->chanid) == NULL)
                || (channel->authtype && cJSON_AddStringToObject(j_channel, "authtype", channel->authtype) == NULL)
                || (channel->username && (cJSON_AddStringToObject(j_channel, "username", channel->username) == NULL))
                || (channel->clientid && (cJSON_AddStringToObject(j_channel, "clientid", channel->clientid) == NULL))
                || (channel->text_name && (cJSON_AddStringToObject(j_channel, "textname", channel->text_name) == NULL))
                || (channel->text_description && (cJSON_AddStringToObject(j_channel, "textdescription", channel->text_description) == NULL))
                || (channel->disabled && (cJSON_AddBoolToObject(j_channel, "disabled", true) == NULL))){
            return 1;
        }

        // Add roles
        j_roles = dynsec_rolelist__all_to_json(channel->rolelist);
        if(j_roles == NULL){
            return 1;
        }
        cJSON_AddItemToObject(j_channel, "roles", j_roles);

        // Add password
        if(channel->pw.valid){
            cJSON * j_password = cJSON_CreateObject();

            // password hash
            if(dynsec_auth__base64_encode(channel->pw.password_hash, sizeof(channel->pw.password_hash), &buf) != MOSQ_ERR_SUCCESS){
                return 1;
            }
            jtmp = cJSON_CreateString(buf);
            mosquitto_free(buf);
            if(jtmp == NULL) return 1;
            cJSON_AddItemToObject(j_password, "hash", jtmp);

            // password salt
            if(dynsec_auth__base64_encode(channel->pw.salt, sizeof(channel->pw.salt), &buf) != MOSQ_ERR_SUCCESS){
                return 1;
            }
            jtmp = cJSON_CreateString(buf);
            mosquitto_free(buf);
            if(jtmp == NULL) return 1;
            cJSON_AddItemToObject(j_password, "salt", jtmp);

            // password iterations
            if(cJSON_AddIntToObject(j_password, "iterations", channel->pw.iterations) == NULL){
                return 1;
            }

            cJSON_AddItemToObject(j_channel, "password", j_password);
        }

        // Add JWT public key
        if(channel->jwtkey && cJSON_AddStringToObject(j_channel, "jwtkey", channel->jwtkey) == NULL){
            return 1;
        }
    }

    return 0;
}


int dynsec_channels__config_save(cJSON *tree)
{
    cJSON *j_channels;

    if((j_channels = cJSON_AddArrayToObject(tree, "channels")) == NULL) return 1;
    if(dynsec__config_add_channels(j_channels, local_channels)) return 1;

    return 0;
}


int dynsec_channels__process_create(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
    char * username, * secret, * clientid, * chanid;
    char * text_name, * text_description, * authtype;
    struct dynsec__channel * channel;
    int rc;
    cJSON * j_groups, * j_group;
    int priority;
    const char *admin_clientid, *admin_username;

    if(json_get_string(command, "chanid", &chanid, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "createChannel", "Invalid/missing chanid", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    // Check the channel doesn't exist already
    channel = dynsec_channels__get(chanid);
    if(channel){
        dynsec__command_reply(j_responses, context, "createChannel", "Channel already exists", correlation_data);
        return MOSQ_ERR_SUCCESS;
    }

    if(json_get_string(command, "username", &username, true) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "createChannel", "Invalid/missing username", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    if(json_get_string(command, "clientid", &clientid, true) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "createChannel", "Invalid/missing clientid", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    // Check username and clientid are valid UTF8
    if(username && mosquitto_validate_utf8(username, (int)strlen(username)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "createChannel", "Username not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(clientid && mosquitto_validate_utf8(clientid, (int)strlen(clientid)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "createChannel", "Channel ID not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    if(json_get_string(command, "authtype", &authtype, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "createChannel", "Invalid/missing authtype", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    if(json_get_string(command, "secret", & secret, strcmp(authtype, MQTT_AUTH_NONE) == 0) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "createChannel", "Invalid/missing secret", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    if(json_get_string(command, "textname", &text_name, true) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "createChannel", "Invalid/missing textname", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    if(json_get_string(command, "textdescription", &text_description, true) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "createChannel", "Invalid/missing textdescription", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    // Create channel
    channel = mosquitto_calloc(1, sizeof(struct dynsec__channel));
    if(channel == NULL){
        dynsec__command_reply(j_responses, context, "createChannel", "Internal error", correlation_data);
        return MOSQ_ERR_NOMEM;
    }

    channel->chanid = mosquitto_strdup(chanid);
    if(channel->chanid == NULL){
        dynsec__command_reply(j_responses, context, "createChannel", "Internal error", correlation_data);
        channel__free_item(channel);
        return MOSQ_ERR_NOMEM;
    }

    if(username){
        channel->username = mosquitto_strdup(username);
        if(channel->username == NULL){
            dynsec__command_reply(j_responses, context, "createChannel", "Internal error", correlation_data);
            channel__free_item(channel);
            return MOSQ_ERR_NOMEM;
        }
    }

    if(clientid){
        channel->clientid = mosquitto_strdup(clientid);
        if(channel->clientid == NULL){
            dynsec__command_reply(j_responses, context, "createChannel", "Internal error", correlation_data);
            channel__free_item(channel);
            return MOSQ_ERR_NOMEM;
        }
    }

    json_get_bool(command, "disabled", & channel->disabled, false, false);

    // Time to check for uniqueness
    // (must add user before groups, otherwise adding groups will fail)
    if(dynsec_channels__add_inorder(channel) != MOSQ_ERR_SUCCESS){
        channel__unallocate_item(channel);
        dynsec__command_reply(j_responses, context, "createChannel", "Connector ambiguous", correlation_data);
        return MOSQ_ERR_SUCCESS;
    }

    if (channel->authtype = mosquitto_strdup(authtype), ! channel->authtype){
        dynsec__command_reply(j_responses, context, "createChannel", "Internal error", correlation_data);
        channel__free_item(channel);
        return MOSQ_ERR_NOMEM;
    }

    if (strcmp(channel->authtype, MQTT_AUTH_PASSWORD) == 0){
        if(dynsec_auth__pw_hash(channel, secret, channel->pw.password_hash, sizeof(channel->pw.password_hash), true)){
            dynsec__command_reply(j_responses, context, "createChannel", "Internal error", correlation_data);
            channel__free_item(channel);
            return MOSQ_ERR_NOMEM;
        }
        channel->pw.valid = true;
    }else if(strncmp(MQTT_AUTH_KEY_PREFIX, channel->authtype, strlen(MQTT_AUTH_KEY_PREFIX)) == 0){
        channel->jwtkey = mosquitto_strdup(secret);
    }

    if(text_name){
        channel->text_name = mosquitto_strdup(text_name);
        if(channel->text_name == NULL){
            dynsec__command_reply(j_responses, context, "createChannel", "Internal error", correlation_data);
            channel__free_item(channel);
            return MOSQ_ERR_NOMEM;
        }
    }

    if(text_description){
        channel->text_description = mosquitto_strdup(text_description);
        if(channel->text_description == NULL){
            dynsec__command_reply(j_responses, context, "createChannel", "Internal error", correlation_data);
            channel__free_item(channel);
            return MOSQ_ERR_NOMEM;
        }
    }

    rc = dynsec_rolelist__load_from_json(command, &channel->rolelist);
    if(rc == MOSQ_ERR_SUCCESS || rc == ERR_LIST_NOT_FOUND){
    }else if(rc == MOSQ_ERR_NOT_FOUND){
        dynsec__command_reply(j_responses, context, "createChannel", "Role not found", correlation_data);
        channel__free_item(channel);
        return MOSQ_ERR_INVAL;
    }else{
        if(rc == MOSQ_ERR_INVAL){
            dynsec__command_reply(j_responses, context, "createChannel", "'roles' not an array or missing/invalid rolename", correlation_data);
        }else{
            dynsec__command_reply(j_responses, context, "createChannel", "Internal error", correlation_data);
        }
        channel__free_item(channel);
        return MOSQ_ERR_INVAL;
    }

    j_groups = cJSON_GetObjectItem(command, "groups");
    if(j_groups && cJSON_IsArray(j_groups)){
        cJSON_ArrayForEach(j_group, j_groups){
            if(cJSON_IsObject(j_group)){
                char *groupname;
                json_get_string(j_group, "groupname", &groupname, false);
                if(groupname){
                    json_get_int(j_group, "priority", &priority, true, -1);
                    rc = dynsec_groups__add_client(channel, groupname, priority, false);
                    if(rc == ERR_GROUP_NOT_FOUND){
                        dynsec__command_reply(j_responses, context, "createChannel", "Group not found", correlation_data);
                        channel__free_item(channel);
                        return MOSQ_ERR_INVAL;
                    }else if(rc != MOSQ_ERR_SUCCESS){
                        dynsec__command_reply(j_responses, context, "createChannel", "Internal error", correlation_data);
                        channel__free_item(channel);
                        return MOSQ_ERR_INVAL;
                    }
                }
            }
        }
    }

    dynsec__config_save();

    dynsec__command_reply(j_responses, context, "createChannel", NULL, correlation_data);

    admin_clientid = mosquitto_client_id(context);
    admin_username = mosquitto_client_username(context);
    mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: %s/%s | createChannel | username=%s | secret=%s",
            admin_clientid, admin_username, username, secret?"*****":"no secret");

    return MOSQ_ERR_SUCCESS;
}


int dynsec_channels__process_delete(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
    cJSON * j_channels;
    cJSON * j_response;
    cJSON * j_deleted;
    struct dynsec__channel * channel;
    const char *admin_clientid, *admin_username;

    if(json_get_array(command, "channels", &j_channels, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "deleteChannels", "Invalid/missing channel_ids", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    j_response = cJSON_CreateObject();
    if(j_response == NULL) return MOSQ_ERR_NOMEM;

    if(cJSON_AddStringToObject(j_response, "command", "deleteChannels") == NULL){
        cJSON_Delete(j_response);
        return MOSQ_ERR_NOMEM;
    }

    j_deleted = cJSON_AddArrayToObject(j_response, "deleted");
    if(j_deleted == NULL){
        cJSON_Delete(j_response);
        return MOSQ_ERR_NOMEM;
    }

    int conn_count = cJSON_GetArraySize(j_channels);
    for(int i=0; i<conn_count; i++){
        char * chanid = cJSON_GetStringValue(cJSON_GetArrayItem(j_channels, i));
        if(chanid == NULL)continue;

        channel = dynsec_channels__get(chanid);
        if(channel){
            cJSON * j_deleted_item = cJSON_CreateString(chanid);
            if(j_deleted_item == NULL)continue;

            dynsec__remove_channel_from_all_groups(channel);
            channel__remove_all_roles(channel);
            channel__free_item(channel);

            cJSON_AddItemToArray(j_deleted, j_deleted_item);

            // Enforce any changes
            dynsec_channels__kick_channels(channel);
        }
    }
    dynsec__config_save();
    cJSON_AddItemToArray(j_responses, j_response);

    admin_clientid = mosquitto_client_id(context);
    admin_username = mosquitto_client_username(context);
    mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: %s/%s | deleteChannels",
            admin_clientid, admin_username);

    return MOSQ_ERR_SUCCESS;
}


int dynsec_channels__process_disable(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
    char *chanid;
    struct dynsec__channel * channel;
    const char *admin_clientid, *admin_username;

    if(json_get_string(command, "chanid", &chanid, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "disableChannel", "Invalid/missing username", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(mosquitto_validate_utf8(chanid, (int)strlen(chanid)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "disableChannel", "Username not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    channel = dynsec_channels__get(chanid);
    if(channel == NULL){
        dynsec__command_reply(j_responses, context, "disableChannel", "Channel not found", correlation_data);
        return MOSQ_ERR_SUCCESS;
    }

    channel->disabled = true;

    dynsec_channels__kick_channels(channel);

    dynsec__config_save();
    dynsec__command_reply(j_responses, context, "disableChannel", NULL, correlation_data);

    admin_clientid = mosquitto_client_id(context);
    admin_username = mosquitto_client_username(context);
    mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: %s/%s | disableChannel | chanid=%s",
            admin_clientid, admin_username, chanid);

    return MOSQ_ERR_SUCCESS;
}


int dynsec_channels__process_enable(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
    char *chanid;
    struct dynsec__channel * channel;
    const char *admin_clientid, *admin_username;

    if(json_get_string(command, "chanid", &chanid, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "enableChannel", "Invalid/missing username", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(mosquitto_validate_utf8(chanid, (int)strlen(chanid)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "enableChannel", "chanid not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    channel = dynsec_channels__get(chanid);
    if(channel == NULL){
        dynsec__command_reply(j_responses, context, "enableChannel", "Channel not found", correlation_data);
        return MOSQ_ERR_SUCCESS;
    }

    channel->disabled = false;

    dynsec__config_save();
    dynsec__command_reply(j_responses, context, "enableChannel", NULL, correlation_data);

    admin_clientid = mosquitto_client_id(context);
    admin_username = mosquitto_client_username(context);
    mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: %s/%s | enableChannel | username=%s",
            admin_clientid, admin_username, chanid);

    return MOSQ_ERR_SUCCESS;
}


int dynsec_channels__process_set_id(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
    char *chanid, * clientid, * clientid_heap = NULL;
    struct dynsec__channel * channel;
    size_t slen;
    const char *admin_clientid, *admin_username;

    if(json_get_string(command, "chanid", &chanid, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "setChannelId", "Invalid/missing chanid", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(mosquitto_validate_utf8(chanid, (int)strlen(chanid)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "setChannelId", "chanid not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    if(json_get_string(command, "clientid", &clientid, true) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "setChannelId", "Invalid/missing client ID", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(clientid){
        slen = strlen(clientid);
        if(mosquitto_validate_utf8(clientid, (int)slen) != MOSQ_ERR_SUCCESS){
            dynsec__command_reply(j_responses, context, "setChannelId", "Client ID not valid UTF-8", correlation_data);
            return MOSQ_ERR_INVAL;
        }
        if(slen > 0){
            clientid_heap = mosquitto_strdup(clientid);
            if(clientid_heap == NULL){
                dynsec__command_reply(j_responses, context, "setChannelId", "Internal error", correlation_data);
                return MOSQ_ERR_NOMEM;
            }
        }else{
            clientid_heap = NULL;
        }
    }

    channel = dynsec_channels__get(chanid);
    if(channel == NULL){
        mosquitto_free(clientid_heap);
        dynsec__command_reply(j_responses, context, "setChannelId", "Channel not found", correlation_data);
        return MOSQ_ERR_SUCCESS;
    }

    mosquitto_free(channel->clientid);
    channel->clientid = clientid_heap;

    dynsec__config_save();
    dynsec__command_reply(j_responses, context, "setChannelId", NULL, correlation_data);

    /* Enforce any changes */
    dynsec_channels__kick_channels(channel);

    admin_clientid = mosquitto_client_id(context);
    admin_username = mosquitto_client_username(context);
    mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: %s/%s | setChannelId | chanid=%s | clientid=%s",
            admin_clientid, admin_username, chanid, channel->clientid);

    return MOSQ_ERR_SUCCESS;
}


static int channel__set_secret(struct dynsec__channel * channel, const char * secret)
{
    if(strncmp(channel->authtype, MQTT_AUTH_KEY_PREFIX, strlen(MQTT_AUTH_KEY_PREFIX)) == 0){
        channel->jwtkey = mosquitto_strdup(secret);
        if(channel->jwtkey == NULL){
            return MOSQ_ERR_NOMEM;
        }
        channel->pw.valid = false;
        return MOSQ_ERR_SUCCESS;
    }else{
        if(dynsec_auth__pw_hash(channel, secret, channel->pw.password_hash, sizeof(channel->pw.password_hash), true) == MOSQ_ERR_SUCCESS){
            channel->pw.valid = true;
            mosquitto_free(channel->jwtkey);
            channel->jwtkey = NULL;
            return MOSQ_ERR_SUCCESS;
        }else{
            channel->pw.valid = false;
            /* FIXME - this should fail safe without modifying the existing secret */
            return MOSQ_ERR_NOMEM;
        }
    }
}

int dynsec_channels__process_set_password(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
    char *chanid, *password;
    struct dynsec__channel * channel;
    int rc;
    const char *admin_clientid, *admin_username;

    if(json_get_string(command, "chanid", &chanid, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "setChannelPassword", "Invalid/missing chanid", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(mosquitto_validate_utf8(chanid, (int)strlen(chanid)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "setChannelPassword", "chanid not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    if(json_get_string(command, "secret", &password, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "setChannelPassword", "Invalid/missing secret", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(strlen(password) == 0){
        dynsec__command_reply(j_responses, context, "setChannelPassword", "Empty password is not allowed", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    channel = dynsec_channels__get(chanid);
    if(channel == NULL){
        dynsec__command_reply(j_responses, context, "setChannelPassword", "Channel not found", correlation_data);
        return MOSQ_ERR_SUCCESS;
    }
    rc = channel__set_secret(channel, password);
    if(rc == MOSQ_ERR_SUCCESS){
        dynsec__config_save();
        dynsec__command_reply(j_responses, context, "setChannelPassword", NULL, correlation_data);

        /* Enforce any changes */
        dynsec_channels__kick_channels(channel);

        admin_clientid = mosquitto_client_id(context);
        admin_username = mosquitto_client_username(context);
        mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: %s/%s | setChannelPassword | chanid=%s | password=******",
                admin_clientid, admin_username, chanid);
    }else{
        dynsec__command_reply(j_responses, context, "setChannelPassword", "Internal error", correlation_data);
    }
    return rc;
}


static void channel__add_new_roles(struct dynsec__channel * channel, struct dynsec__rolelist *base_rolelist)
{
    struct dynsec__rolelist *rolelist, *rolelist_tmp;

    HASH_ITER(hh, base_rolelist, rolelist, rolelist_tmp){
        dynsec_rolelist__channel_add(channel, rolelist->role, rolelist->priority);
    }
}

static void channel__remove_all_roles(struct dynsec__channel * channel)
{
    struct dynsec__rolelist *rolelist, *rolelist_tmp;

    HASH_ITER(hh, channel->rolelist, rolelist, rolelist_tmp){
        dynsec_rolelist__channel_remove(channel, rolelist->role);
    }
}

int dynsec_channels__process_modify(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
    char * chanid;
    char * clientid = NULL;
    char * username = NULL;
    char * authtype = NULL;
    char * secret = NULL;
    char * text_name = NULL;
    char * text_description = NULL;
    bool have_clientid = false, have_text_name = false, have_text_description = false, have_rolelist = false, have_secret = false;
    bool have_username = false;
    struct dynsec__channel * channel;
    struct dynsec__group *group;
    struct dynsec__rolelist *rolelist = NULL;
    char * str;
    int rc;
    int priority;
    cJSON *j_group, *j_groups;
    const char *admin_clientid, *admin_username;

    if(json_get_string(command, "chanid", &chanid, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "modifyChannel", "Invalid/missing chanid", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(mosquitto_validate_utf8(chanid, (int)strlen(chanid)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "modifyChannel", "chanid not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    channel = dynsec_channels__get(chanid);
    if(channel == NULL){
        dynsec__command_reply(j_responses, context, "modifyChannel", "Channel not found", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    json_get_string_allow_empty(command, "clientid", &clientid, true);
    json_get_string_allow_empty(command, "username", &username, true);

    // We need to check uniqueness of clientid and username
    if(dynsec_channels__add_check_uniqueness(NULL, clientid, username, chanid) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "modifyChannel", "Connector will become ambiguous", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    if(clientid != NULL){
        have_clientid = true;
        if(strlen(clientid) > 0){
            clientid = mosquitto_strdup(clientid);
            if(clientid == NULL){
                dynsec__command_reply(j_responses, context, "modifyChannel", "Internal error", correlation_data);
                rc = MOSQ_ERR_NOMEM;
                goto error;
            }
        }else{
            clientid = NULL;
        }
    }

    if(username != NULL){
        have_username = true;
        if(strlen(username) > 0){
            username = mosquitto_strdup(username);
            if(username == NULL){
                dynsec__command_reply(j_responses, context, "modifyChannel", "Internal error", correlation_data);
                rc = MOSQ_ERR_NOMEM;
                goto error;
            }
        }else{
            username = NULL;
        }
    }

    json_get_bool(command, "disabled", & channel->disabled, false, false);

    if(json_get_string(command, "secret", & secret, false) == MOSQ_ERR_SUCCESS){
        if(json_get_string(command, "authtype", & authtype, false) != MOSQ_ERR_SUCCESS){
            dynsec__command_reply(j_responses, context, "modifyChannel", "Invalid/missing authtype", correlation_data);
            return MOSQ_ERR_INVAL;
        }
        have_secret = true;
    }

    if(json_get_string_allow_empty(command, "textname", &str, false) == MOSQ_ERR_SUCCESS){
        have_text_name = true;
        text_name = mosquitto_strdup(str);
        if(text_name == NULL){
            dynsec__command_reply(j_responses, context, "modifyChannel", "Internal error", correlation_data);
            rc = MOSQ_ERR_NOMEM;
            goto error;
        }
    }

    if(json_get_string_allow_empty(command, "textdescription", &str, false) == MOSQ_ERR_SUCCESS){
        have_text_description = true;
        text_description = mosquitto_strdup(str);
        if(text_description == NULL){
            dynsec__command_reply(j_responses, context, "modifyChannel", "Internal error", correlation_data);
            rc = MOSQ_ERR_NOMEM;
            goto error;
        }
    }

    rc = dynsec_rolelist__load_from_json(command, &rolelist);
    if(rc == MOSQ_ERR_SUCCESS){
        have_rolelist = true;
    }else if(rc == ERR_LIST_NOT_FOUND){
        /* There was no list in the JSON, so no modification */
    }else if(rc == MOSQ_ERR_NOT_FOUND){
        dynsec__command_reply(j_responses, context, "modifyChannel", "Role not found", correlation_data);
        rc = MOSQ_ERR_INVAL;
        goto error;
    }else{
        if(rc == MOSQ_ERR_INVAL){
            dynsec__command_reply(j_responses, context, "modifyChannel", "'roles' not an array or missing/invalid rolename", correlation_data);
        }else{
            dynsec__command_reply(j_responses, context, "modifyChannel", "Internal error", correlation_data);
        }
        rc = MOSQ_ERR_INVAL;
        goto error;
    }

    j_groups = cJSON_GetObjectItem(command, "groups");
    if(j_groups && cJSON_IsArray(j_groups)){
        /* Iterate through list to check all groups are valid */
        cJSON_ArrayForEach(j_group, j_groups){
            if(cJSON_IsObject(j_group)){
                char *groupname;
                json_get_string(j_group, "groupname", &groupname, false);
                if(groupname){
                    group = dynsec_groups__find(groupname);
                    if(group == NULL){
                        dynsec__command_reply(j_responses, context, "modifyChannel", "'groups' contains an object with a 'groupname' that does not exist", correlation_data);
                        rc = MOSQ_ERR_INVAL;
                        goto error;
                    }
                }else{
                    dynsec__command_reply(j_responses, context, "modifyChannel", "'groups' contains an object with an invalid 'groupname'", correlation_data);
                    rc = MOSQ_ERR_INVAL;
                    goto error;
                }
            }
        }

        dynsec__remove_channel_from_all_groups(channel);
        cJSON_ArrayForEach(j_group, j_groups){
            if(cJSON_IsObject(j_group)){
                char *groupname;
                json_get_string(j_group, "groupname", &groupname, false);
                if(groupname){
                    json_get_int(j_group, "priority", &priority, true, -1);
                    dynsec_groups__add_client(channel, groupname, priority, false);
                }
            }
        }
    }

    if(have_secret){
        /* FIXME - This is the one call that will result in modification on internal error - note that groups have already been modified */
        mosquitto_free(channel->authtype);
        channel->authtype = mosquitto_strdup(authtype);
        if(channel->authtype == NULL){
            rc = MOSQ_ERR_NOMEM;
            goto error;
        }

        rc = channel__set_secret(channel, secret);
        if(rc != MOSQ_ERR_SUCCESS){
            dynsec__command_reply(j_responses, context, "modifyChannel", "Internal error", correlation_data);
            dynsec_channels__kick_channels(channel);
            /* If this fails we have the situation that the secret is set as
             * invalid, but the config isn't saved, so restarting the broker
             * *now* will mean the channel can log in again. This might be
             * "good", but is inconsistent, so save the config to be
             * consistent. */
            dynsec__config_save();
            rc = MOSQ_ERR_NOMEM;
            goto error;
        }
    }

    if(have_clientid || have_username){
        channel__drop_aid_hash(channel);

        if(have_clientid){
            mosquitto_free(channel->clientid);
            channel->clientid = clientid;
        }

        if(have_username){
            mosquitto_free(channel->username);
            channel->username = username;
        }

        channel__add_aid_hash(channel);
    }

    if(have_text_name){
        mosquitto_free(channel->text_name);
        channel->text_name = text_name;
    }

    if(have_text_description){
        mosquitto_free(channel->text_description);
        channel->text_description = text_description;
    }

    if(have_rolelist){
        channel__remove_all_roles(channel);
        channel__add_new_roles(channel, rolelist);
        dynsec_rolelist__cleanup(&rolelist);
    }

    dynsec__config_save();
    dynsec__command_reply(j_responses, context, "modifyChannel", NULL, correlation_data);

    /* Enforce any changes */
    dynsec_channels__kick_channels(channel);

    admin_clientid = mosquitto_client_id(context);
    admin_username = mosquitto_client_username(context);
    mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: %s/%s | modifyChannel | chanid=%s",
            admin_clientid, admin_username, chanid);
    return MOSQ_ERR_SUCCESS;
error:
    mosquitto_free(clientid);
    mosquitto_free(text_name);
    mosquitto_free(text_description);
    dynsec_rolelist__cleanup(&rolelist);
    return rc;
}


static int dynsec__remove_channel_from_all_groups(struct dynsec__channel * channel)
{
    struct dynsec__grouplist *grouplist, *grouplist_tmp;

    if(channel){
        HASH_ITER(hh, channel->grouplist, grouplist, grouplist_tmp){
            dynsec_groups__remove_client(channel->chanid, grouplist->group->groupname, false);
        }
    }

    return MOSQ_ERR_SUCCESS;
}


static cJSON *add_channel_to_json(struct dynsec__channel * channel, bool verbose)
{
    cJSON *j_channel = NULL, *j_groups, *j_roles;

    if(verbose){
        j_channel = cJSON_CreateObject();
        if(j_channel == NULL){
            return NULL;
        }

        if((channel->chanid && cJSON_AddStringToObject(j_channel, "chanid", channel->chanid) == NULL)
                || (channel->authtype && cJSON_AddStringToObject(j_channel, "authtype", channel->authtype) == NULL)
                || (channel->username && (cJSON_AddStringToObject(j_channel, "username", channel->username) == NULL))
                || (channel->clientid && (cJSON_AddStringToObject(j_channel, "clientid", channel->clientid) == NULL))
                || (channel->jwtkey && (cJSON_AddStringToObject(j_channel, "jwtkey", channel->jwtkey) == NULL))
                || (channel->text_name && (cJSON_AddStringToObject(j_channel, "textname", channel->text_name) == NULL))
                || (channel->text_description && (cJSON_AddStringToObject(j_channel, "textdescription", channel->text_description) == NULL))
                || (cJSON_AddBoolToObject(j_channel, "disabled", channel->disabled) == NULL)
                || (cJSON_AddNumberToObject(j_channel, "msg_received", channel->msg_received) == NULL)
                || (cJSON_AddNumberToObject(j_channel, "msg_timestamp", channel->msg_timestamp) == NULL)){
            cJSON_Delete(j_channel);
            return NULL;
        }

        j_roles = dynsec_rolelist__all_to_json(channel->rolelist);
        if(j_roles == NULL){
            cJSON_Delete(j_channel);
            return NULL;
        }
        cJSON_AddItemToObject(j_channel, "roles", j_roles);

        j_groups = dynsec_grouplist__all_to_json(channel->grouplist);
        if(j_groups == NULL){
            cJSON_Delete(j_channel);
            return NULL;
        }
        cJSON_AddItemToObject(j_channel, "groups", j_groups);
    }else{
        j_channel = cJSON_CreateObject();
        if(j_channel == NULL){
            return NULL;
        }
        if((channel->chanid && cJSON_AddStringToObject(j_channel, "chanid", channel->chanid) == NULL)
                || (cJSON_AddBoolToObject(j_channel, "disabled", channel->disabled) == NULL)
                || (cJSON_AddNumberToObject(j_channel, "msg_received", channel->msg_received) == NULL)
                || (cJSON_AddNumberToObject(j_channel, "msg_timestamp", channel->msg_timestamp) == NULL)){
            cJSON_Delete(j_channel);
            return NULL;
        }
    }
    return j_channel;
}


int dynsec_channels__process_get(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
    char *chanid;
    struct dynsec__channel * channel;
    cJSON *tree, *j_channel, *j_data;
    const char *admin_clientid, *admin_username;

    if(json_get_string(command, "chanid", &chanid, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "getChannel", "Invalid/missing chanid", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(mosquitto_validate_utf8(chanid, (int)strlen(chanid)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "getChannel", "chanid not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    channel = dynsec_channels__get(chanid);
    if(channel == NULL){
        dynsec__command_reply(j_responses, context, "getChannel", "Channel not found", correlation_data);
        return MOSQ_ERR_SUCCESS;
    }

    tree = cJSON_CreateObject();
    if(tree == NULL){
        dynsec__command_reply(j_responses, context, "getChannel", "Internal error", correlation_data);
        return MOSQ_ERR_NOMEM;
    }

    if(cJSON_AddStringToObject(tree, "command", "getChannel") == NULL
            || (j_data = cJSON_AddObjectToObject(tree, "data")) == NULL
            || (correlation_data && cJSON_AddStringToObject(tree, "correlationData", correlation_data) == NULL)
            ){

        cJSON_Delete(tree);
        dynsec__command_reply(j_responses, context, "getChannel", "Internal error", correlation_data);
        return MOSQ_ERR_NOMEM;
    }

    j_channel = add_channel_to_json(channel, true);
    if(j_channel == NULL){
        cJSON_Delete(tree);
        dynsec__command_reply(j_responses, context, "getChannel", "Internal error", correlation_data);
        return MOSQ_ERR_NOMEM;
    }
    cJSON_AddItemToObject(j_data, "channel", j_channel);
    cJSON_AddItemToArray(j_responses, tree);

    admin_clientid = mosquitto_client_id(context);
    admin_username = mosquitto_client_username(context);
    mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: %s/%s | getChannel | chanid=%s",
            admin_clientid, admin_username, chanid);

    return MOSQ_ERR_SUCCESS;
}


int dynsec_channels__process_list(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
    bool verbose;
    struct dynsec__channel * channel, *channel_tmp;
    cJSON *tree, *j_channels, *j_channel, *j_data;
    int i, count, offset;
    const char *admin_clientid, *admin_username;

    json_get_bool(command, "verbose", &verbose, true, false);
    json_get_int(command, "count", &count, true, -1);
    json_get_int(command, "offset", &offset, true, 0);

    tree = cJSON_CreateObject();
    if(tree == NULL){
        dynsec__command_reply(j_responses, context, "listChannels", "Internal error", correlation_data);
        return MOSQ_ERR_NOMEM;
    }

    int channels_count = HASH_COUNT(local_channels);

    if(cJSON_AddStringToObject(tree, "command", "listChannels") == NULL
            || (j_data = cJSON_AddObjectToObject(tree, "data")) == NULL
            || cJSON_AddIntToObject(j_data, "totalCount", channels_count) == NULL
            || (j_channels = cJSON_AddArrayToObject(j_data, "channels")) == NULL
            || (correlation_data && cJSON_AddStringToObject(tree, "correlationData", correlation_data) == NULL)
            ){

        cJSON_Delete(tree);
        dynsec__command_reply(j_responses, context, "listChannels", "Internal error", correlation_data);
        return MOSQ_ERR_NOMEM;
    }

    i = 0;
    HASH_ITER(hh, local_channels, channel, channel_tmp){
        if(i>=offset){
            j_channel = add_channel_to_json(channel, verbose);
            if(j_channel == NULL){
                cJSON_Delete(tree);
                dynsec__command_reply(j_responses, context, "listChannels", "Internal error", correlation_data);
                return MOSQ_ERR_NOMEM;
            }
            cJSON_AddItemToArray(j_channels, j_channel);

            if(count >= 0){
                count--;
                if(count <= 0){
                    break;
                }
            }
        }
        i++;
    }

    cJSON_AddItemToArray(j_responses, tree);

    admin_clientid = mosquitto_client_id(context);
    admin_username = mosquitto_client_username(context);
    mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: %s/%s | listChannels | verbose=%s | count=%d | offset=%d",
            admin_clientid, admin_username, verbose?"true":"false", count, offset);

    return MOSQ_ERR_SUCCESS;
}


int dynsec_channels__process_add_role(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
    char *chanid, *rolename;
    struct dynsec__channel * channel;
    struct dynsec__role *role;
    int priority;
    const char *admin_clientid, *admin_username;

    if(json_get_string(command, "chanid", &chanid, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "addChannelRole", "Invalid/missing chanid", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(mosquitto_validate_utf8(chanid, (int)strlen(chanid)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "addChannelRole", "chanid not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    if(json_get_string(command, "rolename", &rolename, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "addChannelRole", "Invalid/missing rolename", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(mosquitto_validate_utf8(rolename, (int)strlen(rolename)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "addChannelRole", "Role name not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    json_get_int(command, "priority", &priority, true, -1);

    channel = dynsec_channels__get(chanid);
    if(channel == NULL){
        dynsec__command_reply(j_responses, context, "addChannelRole", "Channel not found", correlation_data);
        return MOSQ_ERR_SUCCESS;
    }

    role = dynsec_roles__find(rolename);
    if(role == NULL){
        dynsec__command_reply(j_responses, context, "addChannelRole", "Role not found", correlation_data);
        return MOSQ_ERR_SUCCESS;
    }

    if(dynsec_rolelist__channel_add(channel, role, priority) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "addChannelRole", "Internal error", correlation_data);
        return MOSQ_ERR_UNKNOWN;
    }
    dynsec__config_save();
    dynsec__command_reply(j_responses, context, "addChannelRole", NULL, correlation_data);

    /* Enforce any changes */
    dynsec_channels__kick_channels(channel);

    admin_clientid = mosquitto_client_id(context);
    admin_username = mosquitto_client_username(context);
    mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: %s/%s | addChannelRole | chanid=%s | rolename=%s | priority=%d",
            admin_clientid, admin_username, chanid, rolename, priority);

    return MOSQ_ERR_SUCCESS;
}


int dynsec_channels__process_remove_role(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
    char *chanid, *rolename;
    struct dynsec__channel * channel;
    struct dynsec__role *role;
    const char *admin_clientid, *admin_username;

    if(json_get_string(command, "chanid", &chanid, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "removeChannelRole", "Invalid/missing chanid", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(mosquitto_validate_utf8(chanid, (int)strlen(chanid)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "removeChannelRole", "chanid not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    if(json_get_string(command, "rolename", &rolename, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "removeChannelRole", "Invalid/missing rolename", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(mosquitto_validate_utf8(rolename, (int)strlen(rolename)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "removeChannelRole", "Role name not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }


    channel = dynsec_channels__get(chanid);
    if(channel == NULL){
        dynsec__command_reply(j_responses, context, "removeChannelRole", "Channel not found", correlation_data);
        return MOSQ_ERR_SUCCESS;
    }

    role = dynsec_roles__find(rolename);
    if(role == NULL){
        dynsec__command_reply(j_responses, context, "removeChannelRole", "Role not found", correlation_data);
        return MOSQ_ERR_SUCCESS;
    }

    dynsec_rolelist__channel_remove(channel, role);
    dynsec__config_save();
    dynsec__command_reply(j_responses, context, "removeChannelRole", NULL, correlation_data);

    /* Enforce any changes */
    dynsec_channels__kick_channels(channel);

    admin_clientid = mosquitto_client_id(context);
    admin_username = mosquitto_client_username(context);
    mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: %s/%s | removeChannelRole | chanid=%s | rolename=%s",
            admin_clientid, admin_username, chanid, rolename);

    return MOSQ_ERR_SUCCESS;
}
