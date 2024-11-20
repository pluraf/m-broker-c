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
 * # Plugin global variables
 * #
 * ################################################################ */

struct dynsec__group *dynsec_anonymous_group = NULL;


/* ################################################################
 * #
 * # Function declarations
 * #
 * ################################################################ */

static int dynsec__remove_all_clients_from_group(struct dynsec__group *group);
static int dynsec__remove_all_roles_from_group(struct dynsec__group *group);
static cJSON *add_group_to_json(struct dynsec__group *group);


/* ################################################################
 * #
 * # Local variables
 * #
 * ################################################################ */

static struct dynsec__group *local_groups = NULL;


/* ################################################################
 * #
 * # Utility functions
 * #
 * ################################################################ */

static void group__kick_all(struct dynsec__group *group)
{
    if(group == dynsec_anonymous_group){
        mosquitto_kick_client_by_username(NULL, false);
    }
    dynsec_channellist__kick_all(group->channellist);
}


static int group_cmp(void *a, void *b)
{
    struct dynsec__group *group_a = a;
    struct dynsec__group *group_b = b;

    return strcmp(group_a->groupname, group_b->groupname);
}


struct dynsec__group *dynsec_groups__find(const char *groupname)
{
    struct dynsec__group *group = NULL;

    if(groupname){
        HASH_FIND(hh, local_groups, groupname, strlen(groupname), group);
    }
    return group;
}

static void group__free_item(struct dynsec__group *group)
{
    struct dynsec__group *found_group = NULL;

    if(group == NULL) return;

    found_group = dynsec_groups__find(group->groupname);
    if(found_group){
        HASH_DEL(local_groups, found_group);
    }
    dynsec__remove_all_clients_from_group(group);
    mosquitto_free(group->text_name);
    mosquitto_free(group->text_description);
    mosquitto_free(group->groupname);
    dynsec_rolelist__cleanup(&group->rolelist);
    mosquitto_free(group);
}

int dynsec_groups__process_add_role(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
    char *groupname, *rolename;
    struct dynsec__group *group;
    struct dynsec__role *role;
    int priority;
    const char *admin_clientid, *admin_username;
    int rc;

    if(json_get_string(command, "groupname", &groupname, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "addGroupRole", "Invalid/missing groupname", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(mosquitto_validate_utf8(groupname, (int)strlen(groupname)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "addGroupRole", "Group name not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    if(json_get_string(command, "rolename", &rolename, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "addGroupRole", "Invalid/missing rolename", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(mosquitto_validate_utf8(rolename, (int)strlen(rolename)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "addGroupRole", "Role name not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    json_get_int(command, "priority", &priority, true, -1);

    group = dynsec_groups__find(groupname);
    if(group == NULL){
        dynsec__command_reply(j_responses, context, "addGroupRole", "Group not found", correlation_data);
        return MOSQ_ERR_SUCCESS;
    }

    role = dynsec_roles__find(rolename);
    if(role == NULL){
        dynsec__command_reply(j_responses, context, "addGroupRole", "Role not found", correlation_data);
        return MOSQ_ERR_SUCCESS;
    }

    admin_clientid = mosquitto_client_id(context);
    admin_username = mosquitto_client_username(context);

    rc = dynsec_rolelist__group_add(group, role, priority);
    if(rc == MOSQ_ERR_SUCCESS){
        /* Continue */
    }else if(rc == MOSQ_ERR_ALREADY_EXISTS){
        dynsec__command_reply(j_responses, context, "addGroupRole", "Group is already in this role", correlation_data);
        return MOSQ_ERR_ALREADY_EXISTS;
    }else{
        dynsec__command_reply(j_responses, context, "addGroupRole", "Internal error", correlation_data);
        return MOSQ_ERR_UNKNOWN;
    }

    mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: %s/%s | addGroupRole | groupname=%s | rolename=%s | priority=%d",
            admin_clientid, admin_username, groupname, rolename, priority);

    dynsec__config_save();
    dynsec__command_reply(j_responses, context, "addGroupRole", NULL, correlation_data);

    /* Enforce any changes */
    group__kick_all(group);

    return MOSQ_ERR_SUCCESS;
}


void dynsec_groups__cleanup(void)
{
    struct dynsec__group *group, *group_tmp = NULL;

    HASH_ITER(hh, local_groups, group, group_tmp){
        group__free_item(group);
    }
}


/* ################################################################
 * #
 * # Config file load
 * #
 * ################################################################ */

int dynsec_groups__config_load(cJSON *tree)
{
    cJSON *j_groups, *j_group;
    cJSON *j_channellist, *j_channel;
    cJSON *j_roles, *j_role;

    struct dynsec__group *group;
    struct dynsec__role *role;
    char *groupname;
    int priority;

    j_groups = cJSON_GetObjectItem(tree, "groups");
    if(j_groups == NULL){
        return 0;
    }

    if(cJSON_IsArray(j_groups) == false){
        return 1;
    }

    cJSON_ArrayForEach(j_group, j_groups){
        if(cJSON_IsObject(j_group) == true){
            /* Group name */
            if(json_get_string(j_group, "groupname", &groupname, false) != MOSQ_ERR_SUCCESS){
                continue;
            }
            group = dynsec_groups__find(groupname);
            if(group){
                continue;
            }

            group = mosquitto_calloc(1, sizeof(struct dynsec__group));
            if(group == NULL){
                return MOSQ_ERR_NOMEM;
            }

            group->groupname = strdup(groupname);
            if(group->groupname == NULL){
                mosquitto_free(group);
                continue;
            }

            /* Text name */
            char *textname;
            if(json_get_string(j_group, "textname", &textname, false) == MOSQ_ERR_SUCCESS){
                if(textname){
                    group->text_name = strdup(textname);
                    if(group->text_name == NULL){
                        mosquitto_free(group->groupname);
                        mosquitto_free(group);
                        continue;
                    }
                }
            }

            /* Text description */
            char *textdescription;
            if(json_get_string(j_group, "textdescription", &textdescription, false) == MOSQ_ERR_SUCCESS){
                if(textdescription){
                    group->text_description = strdup(textdescription);
                    if(group->text_description == NULL){
                        mosquitto_free(group->text_name);
                        mosquitto_free(group->groupname);
                        mosquitto_free(group);
                        continue;
                    }
                }
            }

            /* Roles */
            j_roles = cJSON_GetObjectItem(j_group, "roles");
            if(j_roles && cJSON_IsArray(j_roles)){
                cJSON_ArrayForEach(j_role, j_roles){
                    if(cJSON_IsObject(j_role)){
                        char *rolename;
                        json_get_string(j_role, "rolename", &rolename, false);
                        if(rolename){
                            json_get_int(j_role, "priority", &priority, true, -1);
                            role = dynsec_roles__find(rolename);
                            dynsec_rolelist__group_add(group, role, priority);
                        }
                    }
                }
            }

            /* This must go before clients are loaded, otherwise the group won't be found */
            HASH_ADD_KEYPTR(hh, local_groups, group->groupname, strlen(group->groupname), group);

            /* Channels */
            j_channellist = cJSON_GetObjectItem(j_group, "channels");
            if(j_channellist && cJSON_IsArray(j_channellist)){
                cJSON_ArrayForEach(j_channel, j_channellist){
                    if(cJSON_IsObject(j_channel)){
                        char *chanid;
                        if (json_get_string(j_channel, "chanid", &chanid, false) == MOSQ_ERR_SUCCESS){
                            json_get_int(j_channel, "priority", &priority, true, -1);
                            struct dynsec__channel * channel = dynsec_channels__get(chanid);
                            if(channel) dynsec_groups__add_client(channel, group->groupname, priority, false);
                        }
                    }
                }
            }
        }
    }
    HASH_SORT(local_groups, group_cmp);

    json_get_string(tree, "anonymousGroup", &groupname, false);
    if(groupname){
        dynsec_anonymous_group = dynsec_groups__find(groupname);
    }

    return 0;
}


/* ################################################################
 * #
 * # Config load and save
 * #
 * ################################################################ */


static int dynsec__config_add_groups(cJSON *j_groups)
{
    struct dynsec__group *group, *group_tmp = NULL;
    cJSON *j_group, *j_channels, *j_roles;

    HASH_ITER(hh, local_groups, group, group_tmp){
        j_group = cJSON_CreateObject();
        if(j_group == NULL) return 1;
        cJSON_AddItemToArray(j_groups, j_group);

        if(cJSON_AddStringToObject(j_group, "groupname", group->groupname) == NULL
                || (group->text_name && cJSON_AddStringToObject(j_group, "textname", group->text_name) == NULL)
                || (group->text_description && cJSON_AddStringToObject(j_group, "textdescription", group->text_description) == NULL)
                ){

            return 1;
        }

        j_roles = dynsec_rolelist__all_to_json(group->rolelist);
        if(j_roles == NULL){
            return 1;
        }
        cJSON_AddItemToObject(j_group, "roles", j_roles);

        j_channels = dynsec_channellist__all_to_json(group->channellist);
        if(j_channels == NULL){
            return 1;
        }
        cJSON_AddItemToObject(j_group, "channels", j_channels);
    }

    return 0;
}


int dynsec_groups__config_save(cJSON *tree)
{
    cJSON *j_groups;

    j_groups = cJSON_CreateArray();
    if(j_groups == NULL){
        return 1;
    }
    cJSON_AddItemToObject(tree, "groups", j_groups);
    if(dynsec__config_add_groups(j_groups)){
        return 1;
    }

    if(dynsec_anonymous_group
            && cJSON_AddStringToObject(tree, "anonymousGroup", dynsec_anonymous_group->groupname) == NULL){

        return 1;
    }

    return 0;
}


int dynsec_groups__process_create(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
    char *groupname, *text_name, *text_description;
    struct dynsec__group *group = NULL;
    int rc = MOSQ_ERR_SUCCESS;
    const char *admin_clientid, *admin_username;

    if(json_get_string(command, "groupname", &groupname, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "createGroup", "Invalid/missing groupname", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(mosquitto_validate_utf8(groupname, (int)strlen(groupname)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "createGroup", "Group name not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    if(json_get_string(command, "textname", &text_name, true) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "createGroup", "Invalid/missing textname", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    if(json_get_string(command, "textdescription", &text_description, true) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "createGroup", "Invalid/missing textdescription", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    group = dynsec_groups__find(groupname);
    if(group){
        dynsec__command_reply(j_responses, context, "createGroup", "Group already exists", correlation_data);
        return MOSQ_ERR_SUCCESS;
    }

    group = mosquitto_calloc(1, sizeof(struct dynsec__group));
    if(group == NULL){
        dynsec__command_reply(j_responses, context, "createGroup", "Internal error", correlation_data);
        return MOSQ_ERR_NOMEM;
    }
    group->groupname = strdup(groupname);
    if(group->groupname == NULL){
        dynsec__command_reply(j_responses, context, "createGroup", "Internal error", correlation_data);
        group__free_item(group);
        return MOSQ_ERR_NOMEM;
    }
    if(text_name){
        group->text_name = strdup(text_name);
        if(group->text_name == NULL){
            dynsec__command_reply(j_responses, context, "createGroup", "Internal error", correlation_data);
            group__free_item(group);
            return MOSQ_ERR_NOMEM;
        }
    }
    if(text_description){
        group->text_description = strdup(text_description);
        if(group->text_description == NULL){
            dynsec__command_reply(j_responses, context, "createGroup", "Internal error", correlation_data);
            group__free_item(group);
            return MOSQ_ERR_NOMEM;
        }
    }

    rc = dynsec_rolelist__load_from_json(command, &group->rolelist);
    if(rc == MOSQ_ERR_SUCCESS || rc == ERR_LIST_NOT_FOUND){
    }else if(rc == MOSQ_ERR_NOT_FOUND){
        dynsec__command_reply(j_responses, context, "createGroup", "Role not found", correlation_data);
        group__free_item(group);
        return MOSQ_ERR_INVAL;
    }else{
        dynsec__command_reply(j_responses, context, "createGroup", "Internal error", correlation_data);
        group__free_item(group);
        return MOSQ_ERR_INVAL;
    }

    HASH_ADD_KEYPTR_INORDER(hh, local_groups, group->groupname, strlen(group->groupname), group, group_cmp);

    admin_clientid = mosquitto_client_id(context);
    admin_username = mosquitto_client_username(context);
    mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: %s/%s | createGroup | groupname=%s",
            admin_clientid, admin_username, groupname);

    dynsec__config_save();
    dynsec__command_reply(j_responses, context, "createGroup", NULL, correlation_data);
    return MOSQ_ERR_SUCCESS;
}


int dynsec_groups__process_delete(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
    char *groupname;
    struct dynsec__group *group;
    const char *admin_clientid, *admin_username;

    if(json_get_string(command, "groupname", &groupname, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "deleteGroup", "Invalid/missing groupname", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(mosquitto_validate_utf8(groupname, (int)strlen(groupname)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "deleteGroup", "Group name not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    group = dynsec_groups__find(groupname);
    if(group){
        if(group == dynsec_anonymous_group){
            dynsec__command_reply(j_responses, context, "deleteGroup", "Deleting the anonymous group is forbidden", correlation_data);
            return MOSQ_ERR_INVAL;
        }

        /* Enforce any changes */
        group__kick_all(group);

        dynsec__remove_all_roles_from_group(group);
        group__free_item(group);
        dynsec__config_save();
        dynsec__command_reply(j_responses, context, "deleteGroup", NULL, correlation_data);

        admin_clientid = mosquitto_client_id(context);
        admin_username = mosquitto_client_username(context);
        mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: %s/%s | deleteGroup | groupname=%s",
                admin_clientid, admin_username, groupname);

        return MOSQ_ERR_SUCCESS;
    }else{
        dynsec__command_reply(j_responses, context, "deleteGroup", "Group not found", correlation_data);
        return MOSQ_ERR_SUCCESS;
    }
}


int dynsec_groups__add_client(struct dynsec__channel * channel, const char *groupname, int priority, bool update_config)
{
    struct dynsec__channellist * channellist;
    struct dynsec__group *group;
    int rc;

    if(channel == NULL){
        return ERR_USER_NOT_FOUND;
    }

    group = dynsec_groups__find(groupname);
    if(group == NULL){
        return ERR_GROUP_NOT_FOUND;
    }

    HASH_FIND(hh, group->channellist, channel->chanid, strlen(channel->chanid), channellist);
    if(channellist != NULL){
        /* channel is already in the group */
        return MOSQ_ERR_ALREADY_EXISTS;
    }

    rc = dynsec_channellist__add(&group->channellist, channel, priority);
    if(rc){
        return rc;
    }
    rc = dynsec_grouplist__add(&channel->grouplist, group, priority);
    if(rc){
        dynsec_channellist__remove(&group->channellist, channel);
        return rc;
    }

    if(update_config){
        dynsec__config_save();
    }

    return MOSQ_ERR_SUCCESS;
}


int dynsec_groups__process_add_channel(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
    char *chanid, *groupname;
    int rc;
    int priority;
    const char *admin_clientid, *admin_username;

    if(json_get_string(command, "chanid", &chanid, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "addGroupChannel", "Invalid/missing chanid", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(mosquitto_validate_utf8(chanid, (int)strlen(chanid)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "addGroupChannel", "chanid not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    if(json_get_string(command, "groupname", &groupname, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "addGroupChannel", "Invalid/missing groupname", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(mosquitto_validate_utf8(groupname, (int)strlen(groupname)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "addGroupChannel", "Group name not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    json_get_int(command, "priority", &priority, true, -1);

    struct dynsec__channel * channel = dynsec_channels__get(chanid);
    if(channel == NULL){
        dynsec__command_reply(j_responses, context, "addGroupChannel", "Channel not found", correlation_data);
    }else{
        rc = dynsec_groups__add_client(channel, groupname, priority, true);
        if(rc == MOSQ_ERR_SUCCESS){
            admin_clientid = mosquitto_client_id(context);
            admin_username = mosquitto_client_username(context);
            mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: %s/%s | addGroupChannel | groupname=%s | chanid=%s | priority=%d",
                    admin_clientid, admin_username, groupname, chanid, priority);

            dynsec__command_reply(j_responses, context, "addGroupChannel", NULL, correlation_data);
        }else if(rc == ERR_GROUP_NOT_FOUND){
            dynsec__command_reply(j_responses, context, "addGroupChannel", "Group not found", correlation_data);
        }else if(rc == MOSQ_ERR_ALREADY_EXISTS){
            dynsec__command_reply(j_responses, context, "addGroupChannel", "Channel is already in this group", correlation_data);
        }else{
            dynsec__command_reply(j_responses, context, "addGroupChannel", "Internal error", correlation_data);
        }
    }

    /* Enforce any changes */
    if (channel->clientid) {
        mosquitto_kick_client_by_clientid(channel->clientid, false);
    }else{
        mosquitto_kick_client_by_username(channel->username, false);
    }

    return rc;
}


static int dynsec__remove_all_clients_from_group(struct dynsec__group *group)
{
    struct dynsec__channellist * channellist, * channellist_tmp = NULL;

    HASH_ITER(hh, group->channellist, channellist, channellist_tmp){
        /* Remove channel stored group reference */
        dynsec_grouplist__remove(&channellist->channel->grouplist, group);

        HASH_DELETE(hh, group->channellist, channellist);
        mosquitto_free(channellist);
    }

    return MOSQ_ERR_SUCCESS;
}

static int dynsec__remove_all_roles_from_group(struct dynsec__group *group)
{
    struct dynsec__rolelist *rolelist, *rolelist_tmp = NULL;

    HASH_ITER(hh, group->rolelist, rolelist, rolelist_tmp){
        dynsec_rolelist__group_remove(group, rolelist->role);
    }

    return MOSQ_ERR_SUCCESS;
}

int dynsec_groups__remove_client(const char *chanid, const char *groupname, bool update_config)
{
    struct dynsec__channel * channel;
    struct dynsec__group *group;

    channel = dynsec_channels__get(chanid);
    if(channel == NULL){
        return ERR_USER_NOT_FOUND;
    }

    group = dynsec_groups__find(groupname);
    if(group == NULL){
        return ERR_GROUP_NOT_FOUND;
    }

    dynsec_channellist__remove(&group->channellist, channel);
    dynsec_grouplist__remove(&channel->grouplist, group);

    if(update_config){
        dynsec__config_save();
    }
    return MOSQ_ERR_SUCCESS;
}

int dynsec_groups__process_remove_client(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
    char *chanid, *groupname;
    int rc;
    const char *admin_clientid, *admin_username;

    if(json_get_string(command, "chanid", &chanid, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "removeGroupChannel", "Invalid/missing chanid", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(mosquitto_validate_utf8(chanid, (int)strlen(chanid)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "removeGroupChannel", "chanid not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    if(json_get_string(command, "groupname", &groupname, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "removeGroupChannel", "Invalid/missing groupname", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(mosquitto_validate_utf8(groupname, (int)strlen(groupname)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "removeGroupChannel", "Group name not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    rc = dynsec_groups__remove_client(chanid, groupname, true);
    if(rc == MOSQ_ERR_SUCCESS){
        admin_clientid = mosquitto_client_id(context);
        admin_username = mosquitto_client_username(context);
        mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: %s/%s | removeGroupChannel | groupname=%s | chanid=%s",
                admin_clientid, admin_username, groupname, chanid);

        dynsec__command_reply(j_responses, context, "removeGroupChannel", NULL, correlation_data);
    }else if(rc == ERR_USER_NOT_FOUND){
        dynsec__command_reply(j_responses, context, "removeGroupChannel", "Channel not found", correlation_data);
    }else if(rc == ERR_GROUP_NOT_FOUND){
        dynsec__command_reply(j_responses, context, "removeGroupChannel", "Group not found", correlation_data);
    }else{
        dynsec__command_reply(j_responses, context, "removeGroupChannel", "Internal error", correlation_data);
    }

    /* Enforce any changes */
    struct dynsec__channel * conn = dynsec_channels__get(chanid);
    if (conn->clientid) {
        mosquitto_kick_client_by_clientid(conn->clientid, false);
    }else{
        mosquitto_kick_client_by_username(conn->username, false);
    }

    return rc;
}


static cJSON *add_group_to_json(struct dynsec__group *group)
{
    cJSON *j_group, *jtmp, *j_channellist, *j_channel, *j_rolelist;
    struct dynsec__channellist * channellist, * channellist_tmp = NULL;

    j_group = cJSON_CreateObject();
    if(j_group == NULL){
        return NULL;
    }

    if(cJSON_AddStringToObject(j_group, "groupname", group->groupname) == NULL
            || (group->text_name && cJSON_AddStringToObject(j_group, "textname", group->text_name) == NULL)
            || (group->text_description && cJSON_AddStringToObject(j_group, "textdescription", group->text_description) == NULL)
            || (j_channellist = cJSON_AddArrayToObject(j_group, "channels")) == NULL
            ){

        cJSON_Delete(j_group);
        return NULL;
    }

    HASH_ITER(hh, group->channellist, channellist, channellist_tmp){
        j_channel = cJSON_CreateObject();
        if(j_channel == NULL){
            cJSON_Delete(j_group);
            return NULL;
        }
        cJSON_AddItemToArray(j_channellist, j_channel);

        jtmp = cJSON_CreateStringReference(channellist->channel->chanid);
        if(jtmp == NULL){
            cJSON_Delete(j_group);
            return NULL;
        }
        cJSON_AddItemToObject(j_channel, "chanid", jtmp);
    }

    j_rolelist = dynsec_rolelist__all_to_json(group->rolelist);
    if(j_rolelist == NULL){
        cJSON_Delete(j_group);
        return NULL;
    }
    cJSON_AddItemToObject(j_group, "roles", j_rolelist);

    return j_group;
}


int dynsec_groups__process_list(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
    bool verbose;
    cJSON *tree, *j_groups, *j_group, *j_data;
    struct dynsec__group *group, *group_tmp = NULL;
    int i, count, offset;
    const char *admin_clientid, *admin_username;

    json_get_bool(command, "verbose", &verbose, true, false);
    json_get_int(command, "count", &count, true, -1);
    json_get_int(command, "offset", &offset, true, 0);

    tree = cJSON_CreateObject();
    if(tree == NULL){
        dynsec__command_reply(j_responses, context, "listGroups", "Internal error", correlation_data);
        return MOSQ_ERR_NOMEM;
    }

    if(cJSON_AddStringToObject(tree, "command", "listGroups") == NULL
            || (j_data = cJSON_AddObjectToObject(tree, "data")) == NULL
            || cJSON_AddIntToObject(j_data, "totalCount", (int)HASH_CNT(hh, local_groups)) == NULL
            || (j_groups = cJSON_AddArrayToObject(j_data, "groups")) == NULL
            || (correlation_data && cJSON_AddStringToObject(tree, "correlationData", correlation_data) == NULL)
            ){

        cJSON_Delete(tree);
        dynsec__command_reply(j_responses, context, "listGroups", "Internal error", correlation_data);
        return MOSQ_ERR_NOMEM;
    }

    i = 0;
    HASH_ITER(hh, local_groups, group, group_tmp){
        if(i>=offset){
            if(verbose){
                j_group = add_group_to_json(group);
                if(j_group == NULL){
                    cJSON_Delete(tree);
                    dynsec__command_reply(j_responses, context, "listGroups", "Internal error", correlation_data);
                    return MOSQ_ERR_NOMEM;
                }
                cJSON_AddItemToArray(j_groups, j_group);

            }else{
                j_group = cJSON_CreateString(group->groupname);
                if(j_group){
                    cJSON_AddItemToArray(j_groups, j_group);
                }else{
                    cJSON_Delete(tree);
                    dynsec__command_reply(j_responses, context, "listGroups", "Internal error", correlation_data);
                    return MOSQ_ERR_NOMEM;
                }
            }

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
    mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: %s/%s | listGroups | verbose=%s | count=%d | offset=%d",
            admin_clientid, admin_username, verbose?"true":"false", count, offset);

    return MOSQ_ERR_SUCCESS;
}


int dynsec_groups__process_get(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
    char *groupname;
    cJSON *tree, *j_group, *j_data;
    struct dynsec__group *group;
    const char *admin_clientid, *admin_username;

    if(json_get_string(command, "groupname", &groupname, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "getGroup", "Invalid/missing groupname", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(mosquitto_validate_utf8(groupname, (int)strlen(groupname)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "getGroup", "Group name not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    tree = cJSON_CreateObject();
    if(tree == NULL){
        dynsec__command_reply(j_responses, context, "getGroup", "Internal error", correlation_data);
        return MOSQ_ERR_NOMEM;
    }

    if(cJSON_AddStringToObject(tree, "command", "getGroup") == NULL
            || (j_data = cJSON_AddObjectToObject(tree, "data")) == NULL
            || (correlation_data && cJSON_AddStringToObject(tree, "correlationData", correlation_data) == NULL)
            ){

        cJSON_Delete(tree);
        dynsec__command_reply(j_responses, context, "getGroup", "Internal error", correlation_data);
        return MOSQ_ERR_NOMEM;
    }

    group = dynsec_groups__find(groupname);
    if(group){
        j_group = add_group_to_json(group);
        if(j_group == NULL){
            cJSON_Delete(tree);
            dynsec__command_reply(j_responses, context, "getGroup", "Internal error", correlation_data);
            return MOSQ_ERR_NOMEM;
        }
        cJSON_AddItemToObject(j_data, "group", j_group);
    }else{
        cJSON_Delete(tree);
        dynsec__command_reply(j_responses, context, "getGroup", "Group not found", correlation_data);
        return MOSQ_ERR_NOMEM;
    }

    cJSON_AddItemToArray(j_responses, tree);

    admin_clientid = mosquitto_client_id(context);
    admin_username = mosquitto_client_username(context);
    mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: %s/%s | getGroup | groupname=%s",
            admin_clientid, admin_username, groupname);

    return MOSQ_ERR_SUCCESS;
}


int dynsec_groups__process_remove_role(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
    char *groupname, *rolename;
    struct dynsec__group *group;
    struct dynsec__role *role;
    const char *admin_clientid, *admin_username;

    if(json_get_string(command, "groupname", &groupname, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "removeGroupRole", "Invalid/missing groupname", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(mosquitto_validate_utf8(groupname, (int)strlen(groupname)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "removeGroupRole", "Group name not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    if(json_get_string(command, "rolename", &rolename, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "removeGroupRole", "Invalid/missing rolename", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(mosquitto_validate_utf8(rolename, (int)strlen(rolename)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "removeGroupRole", "Role name not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    group = dynsec_groups__find(groupname);
    if(group == NULL){
        dynsec__command_reply(j_responses, context, "removeGroupRole", "Group not found", correlation_data);
        return MOSQ_ERR_SUCCESS;
    }

    role = dynsec_roles__find(rolename);
    if(role == NULL){
        dynsec__command_reply(j_responses, context, "removeGroupRole", "Role not found", correlation_data);
        return MOSQ_ERR_SUCCESS;
    }

    dynsec_rolelist__group_remove(group, role);
    dynsec__config_save();
    dynsec__command_reply(j_responses, context, "removeGroupRole", NULL, correlation_data);

    /* Enforce any changes */
    group__kick_all(group);

    admin_clientid = mosquitto_client_id(context);
    admin_username = mosquitto_client_username(context);
    mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: %s/%s | removeGroupRole | groupname=%s | rolename=%s",
            admin_clientid, admin_username, groupname, rolename);

    return MOSQ_ERR_SUCCESS;
}


int dynsec_groups__process_modify(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
    char *groupname = NULL;
    char *text_name = NULL, *text_description = NULL;
    struct dynsec__channel * channel = NULL;
    struct dynsec__group *group = NULL;
    struct dynsec__rolelist *rolelist = NULL;
    bool have_text_name = false, have_text_description = false, have_rolelist = false;
    int rc;
    int priority;
    cJSON *j_channel, *j_channels;
    char *chanid;
    char *textname;
    char *textdescription;
    const char *admin_clientid, *admin_username;

    if(json_get_string(command, "groupname", &groupname, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "modifyGroup", "Invalid/missing groupname", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(mosquitto_validate_utf8(groupname, (int)strlen(groupname)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "modifyGroup", "Group name not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    group = dynsec_groups__find(groupname);
    if(group == NULL){
        dynsec__command_reply(j_responses, context, "modifyGroup", "Group not found", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    if(json_get_string(command, "textname", &textname, false) == MOSQ_ERR_SUCCESS){
        have_text_name = true;
        text_name = mosquitto_strdup(textname);
        if(text_name == NULL){
            dynsec__command_reply(j_responses, context, "modifyGroup", "Internal error", correlation_data);
            rc = MOSQ_ERR_NOMEM;
            goto error;
        }
    }

    if(json_get_string(command, "textdescription", &textdescription, false) == MOSQ_ERR_SUCCESS){
        have_text_description = true;
        text_description = mosquitto_strdup(textdescription);
        if(text_description == NULL){
            dynsec__command_reply(j_responses, context, "modifyGroup", "Internal error", correlation_data);
            rc = MOSQ_ERR_NOMEM;
            goto error;
        }
    }

    rc = dynsec_rolelist__load_from_json(command, &rolelist);
    if(rc == MOSQ_ERR_SUCCESS){
        /* Apply changes below */
        have_rolelist = true;
    }else if(rc == ERR_LIST_NOT_FOUND){
        /* There was no list in the JSON, so no modification */
        rolelist = NULL;
    }else if(rc == MOSQ_ERR_NOT_FOUND){
        dynsec__command_reply(j_responses, context, "modifyGroup", "Role not found", correlation_data);
        rc = MOSQ_ERR_INVAL;
        goto error;
    }else{
        if(rc == MOSQ_ERR_INVAL){
            dynsec__command_reply(j_responses, context, "modifyGroup", "'roles' not an array or missing/invalid rolename", correlation_data);
        }else{
            dynsec__command_reply(j_responses, context, "modifyGroup", "Internal error", correlation_data);
        }
        rc = MOSQ_ERR_INVAL;
        goto error;
    }

    j_channels = cJSON_GetObjectItem(command, "channels");
    if(j_channels && cJSON_IsArray(j_channels)){
        /* Iterate over array to check clients are valid before proceeding */
        cJSON_ArrayForEach(j_channel, j_channels){
            if(cJSON_IsObject(j_channel)){
                if(json_get_string(j_channel, "chanid", &chanid, false) == MOSQ_ERR_SUCCESS){
                    channel = dynsec_channels__get(chanid);
                    if(channel == NULL){
                        dynsec__command_reply(j_responses, context, "modifyGroup", "'clients' contains an object with a 'chanid' that does not exist", correlation_data);
                        rc = MOSQ_ERR_INVAL;
                        goto error;
                    }
                }else{
                    dynsec__command_reply(j_responses, context, "modifyGroup", "'clients' contains an object with an invalid 'chanid'", correlation_data);
                    rc = MOSQ_ERR_INVAL;
                    goto error;
                }
            }
        }

        /* Kick all clients in the *current* group */
        group__kick_all(group);
        dynsec__remove_all_clients_from_group(group);

        /* Now we can add the new clients to the group */
        cJSON_ArrayForEach(j_channel, j_channels){
            if(cJSON_IsObject(j_channel)){
                if (json_get_string(j_channel, "chanid", &chanid, false) == MOSQ_ERR_SUCCESS){
                    json_get_int(j_channel, "priority", &priority, true, -1);
                    channel = dynsec_channels__get(chanid);
                    if(channel) dynsec_groups__add_client(channel, groupname, priority, false);
                }
            }
        }
    }

    /* Apply remaining changes to group, note that user changes are already applied */
    if(have_text_name){
        mosquitto_free(group->text_name);
        group->text_name = text_name;
    }

    if(have_text_description){
        mosquitto_free(group->text_description);
        group->text_description = text_description;
    }

    if(have_rolelist){
        dynsec_rolelist__cleanup(&group->rolelist);
        group->rolelist = rolelist;
    }

    /* And save */
    dynsec__config_save();

    dynsec__command_reply(j_responses, context, "modifyGroup", NULL, correlation_data);

    /* Enforce any changes - kick any clients in the *new* group */
    group__kick_all(group);

    admin_clientid = mosquitto_client_id(context);
    admin_username = mosquitto_client_username(context);
    mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: %s/%s | modifyGroup | groupname=%s",
            admin_clientid, admin_username, groupname);

    return MOSQ_ERR_SUCCESS;
error:
    mosquitto_free(text_name);
    mosquitto_free(text_description);
    dynsec_rolelist__cleanup(&rolelist);

    admin_clientid = mosquitto_client_id(context);
    admin_username = mosquitto_client_username(context);
    mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: %s/%s | modifyGroup | groupname=%s",
            admin_clientid, admin_username, groupname);

    return rc;
}


int dynsec_groups__process_set_anonymous_group(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
    char *groupname;
    struct dynsec__group *group = NULL;
    const char *admin_clientid, *admin_username;

    if(json_get_string(command, "groupname", &groupname, false) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "setAnonymousGroup", "Invalid/missing groupname", correlation_data);
        return MOSQ_ERR_INVAL;
    }
    if(mosquitto_validate_utf8(groupname, (int)strlen(groupname)) != MOSQ_ERR_SUCCESS){
        dynsec__command_reply(j_responses, context, "setAnonymousGroup", "Group name not valid UTF-8", correlation_data);
        return MOSQ_ERR_INVAL;
    }

    group = dynsec_groups__find(groupname);
    if(group == NULL){
        dynsec__command_reply(j_responses, context, "setAnonymousGroup", "Group not found", correlation_data);
        return MOSQ_ERR_SUCCESS;
    }

    dynsec_anonymous_group = group;

    dynsec__config_save();
    dynsec__command_reply(j_responses, context, "setAnonymousGroup", NULL, correlation_data);

    /* Enforce any changes */
    mosquitto_kick_client_by_username(NULL, false);

    admin_clientid = mosquitto_client_id(context);
    admin_username = mosquitto_client_username(context);
    mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: %s/%s | setAnonymousGroup | groupname=%s",
            admin_clientid, admin_username, groupname);

    return MOSQ_ERR_SUCCESS;
}

int dynsec_groups__process_get_anonymous_group(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
    cJSON *tree, *j_data, *j_group;
    const char *groupname;
    const char *admin_clientid, *admin_username;

    UNUSED(command);

    tree = cJSON_CreateObject();
    if(tree == NULL){
        dynsec__command_reply(j_responses, context, "getAnonymousGroup", "Internal error", correlation_data);
        return MOSQ_ERR_NOMEM;
    }

    if(dynsec_anonymous_group){
        groupname = dynsec_anonymous_group->groupname;
    }else{
        groupname = "";
    }

    if(cJSON_AddStringToObject(tree, "command", "getAnonymousGroup") == NULL
            || (j_data = cJSON_AddObjectToObject(tree, "data")) == NULL
            || (j_group = cJSON_AddObjectToObject(j_data, "group")) == NULL
            || cJSON_AddStringToObject(j_group, "groupname", groupname) == NULL
            || (correlation_data && cJSON_AddStringToObject(tree, "correlationData", correlation_data) == NULL)
            ){

        cJSON_Delete(tree);
        dynsec__command_reply(j_responses, context, "getAnonymousGroup", "Internal error", correlation_data);
        return MOSQ_ERR_NOMEM;
    }

    cJSON_AddItemToArray(j_responses, tree);

    admin_clientid = mosquitto_client_id(context);
    admin_username = mosquitto_client_username(context);
    mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: %s/%s | getAnonymousGroup",
            admin_clientid, admin_username);

    return MOSQ_ERR_SUCCESS;
}
