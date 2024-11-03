/* SPDX-License-Identifier: BSD-3-Clause */

/*
Copyright (c) 2020 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of Eclipse Distribution License v1.0
which accompany this distribution.

The Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php

Contributors:
   Roger Light - initial implementation and documentation.
*/

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

===============================================================================

Contributors:
   Konstantin Tyurin <konstantin@pluraf.com>
*/


#ifndef __DYNAMIC_SECURITY_H__
#define __DYNAMIC_SECURITY_H__

#include <cjson/cJSON.h>
#include <uthash.h>
#include "mosquitto.h"
#include "password_mosq.h"

/* ################################################################
 * #
 * # ACL types
 * #
 * ################################################################ */

#define ACL_TYPE_PUB_C_RECV "publishClientReceive"
#define ACL_TYPE_PUB_C_SEND "publishClientSend"
#define ACL_TYPE_SUB_GENERIC "subscribe"
#define ACL_TYPE_SUB_LITERAL "subscribeLiteral"
#define ACL_TYPE_SUB_PATTERN "subscribePattern"
#define ACL_TYPE_UNSUB_GENERIC "unsubscribe"
#define ACL_TYPE_UNSUB_LITERAL "unsubscribeLiteral"
#define ACL_TYPE_UNSUB_PATTERN "unsubscribePattern"

/* ################################################################
 * #
 * # Authentication types
 * #
 * ################################################################ */

#define MQTT_AUTH_KEY_PREFIX "jwt_"

#define MQTT_AUTH_PASSWORD "password"
#define MQTT_AUTH_JWT_ES256 (MQTT_AUTH_KEY_PREFIX "es256")
#define MQTT_AUTH_JWT_RS256 (MQTT_AUTH_KEY_PREFIX "rs256")
#define MQTT_AUTH_NONE "none"
#define MQTT_AUTH_INVALID "invalid"

/* ################################################################
 * #
 * # Error codes
 * #
 * ################################################################ */

#define ERR_USER_NOT_FOUND 10000
#define ERR_GROUP_NOT_FOUND 10001
#define ERR_LIST_NOT_FOUND 10002

/* ################################################################
 * #
 * # Datatypes
 * #
 * ################################################################ */

struct dynsec__channellist{
	UT_hash_handle hh;
	struct dynsec__channel * channel;
	int priority;
};

struct dynsec__grouplist{
	UT_hash_handle hh;
	struct dynsec__group *group;
	int priority;
};

struct dynsec__rolelist{
	UT_hash_handle hh;
	char *rolename;
	struct dynsec__role *role;
	int priority;
};

struct dynsec__channel{
	UT_hash_handle hh;
	UT_hash_handle hh_clientid;
	UT_hash_handle hh_username;
	struct mosquitto_pw pw;
	struct dynsec__rolelist *rolelist;
	struct dynsec__grouplist *grouplist;
	char * chanid;  // unique channel id
	char * username;
	char * clientid;
	char * authtype;
	char * jwtkey;
	char * text_name;
	char * text_description;
	bool disabled;
	unsigned long msg_received;
	time_t msg_timestamp;
};

struct dynsec__group{
	UT_hash_handle hh;
	struct dynsec__rolelist *rolelist;
	struct dynsec__channellist * channellist;
	char *groupname;
	char *text_name;
	char *text_description;
};


struct dynsec__acl{
	UT_hash_handle hh;
	char *topic;
	int priority;
	bool allow;
};

struct dynsec__acls{
	struct dynsec__acl *publish_c_send;
	struct dynsec__acl *publish_c_recv;
	struct dynsec__acl *subscribe_literal;
	struct dynsec__acl *subscribe_pattern;
	struct dynsec__acl *unsubscribe_literal;
	struct dynsec__acl *unsubscribe_pattern;
};

struct dynsec__role{
	UT_hash_handle hh;
	struct dynsec__acls acls;
	struct dynsec__channellist * channellist;
	struct dynsec__grouplist *grouplist;
	char *rolename;
	char *text_name;
	char *text_description;
};

struct dynsec__acl_default_access{
	bool publish_c_send;
	bool publish_c_recv;
	bool subscribe;
	bool unsubscribe;
};

extern struct dynsec__group *dynsec_anonymous_group;
extern struct dynsec__acl_default_access default_access;

/* ################################################################
 * #
 * # Plugin Functions
 * #
 * ################################################################ */

void dynsec__config_save(void);
int dynsec__handle_control(cJSON *j_responses, struct mosquitto *context, cJSON *commands);
void dynsec__command_reply(cJSON *j_responses, struct mosquitto *context, const char *command, const char *error, const char *correlation_data);


/* ################################################################
 * #
 * # ACL Functions
 * #
 * ################################################################ */

int dynsec__acl_check_callback(int event, void *event_data, void *userdata);
bool sub_acl_check(const char *acl, const char *sub);


/* ################################################################
 * #
 * # Auth Functions
 * #
 * ################################################################ */

int dynsec_auth__base64_encode(unsigned char *in, int in_len, char **encoded);
int dynsec_auth__base64_decode(char *in, unsigned char **decoded, int *decoded_len);
int dynsec_auth__pw_hash(struct dynsec__channel * channel, const char *password, unsigned char *password_hash, int password_hash_len, bool new_password);
int dynsec_auth__basic_auth_callback(int event, void *event_data, void *userdata);


/* ################################################################
 * #
 * # Client Functions
 * #
 * ################################################################ */

void dynsec_channels__cleanup(void);
int dynsec_channels__config_load(cJSON *tree);
int dynsec_channels__config_save(cJSON *tree);
int dynsec_channels__process_add_role(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_channels__process_create(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_channels__process_delete(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_channels__process_disable(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_channels__process_enable(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_channels__process_get(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_channels__process_list(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_channels__process_modify(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_channels__process_remove_role(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_channels__process_set_id(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_channels__process_set_password(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
struct dynsec__channel * dynsec_channels__find(const char * clientid, const char * username);
struct dynsec__channel * dynsec_channels__get(const char * chanid);
int dynsec_channels__add(struct dynsec__channel * channel);
int dynsec_channels__add_inorder(struct dynsec__channel * channel);


/* ################################################################
 * #
 * # Client List Functions
 * #
 * ################################################################ */

cJSON *dynsec_channellist__all_to_json(struct dynsec__channellist *base_channellist);
int dynsec_channellist__add(struct dynsec__channellist **base_channellist, struct dynsec__channel * channel, int priority);
void dynsec_channellist__cleanup(struct dynsec__channellist **base_channellist);
void dynsec_channellist__remove(struct dynsec__channellist **base_channellist, struct dynsec__channel * channel);
void dynsec_channellist__kick_all(struct dynsec__channellist *base_channellist);


/* ################################################################
 * #
 * # Group Functions
 * #
 * ################################################################ */

void dynsec_groups__cleanup(void);
int dynsec_groups__config_load(cJSON *tree);
int dynsec_groups__add_client(struct dynsec__channel * channel, const char *groupname, int priority, bool update_config);
int dynsec_groups__config_save(cJSON *tree);
int dynsec_groups__process_add_channel(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_groups__process_add_role(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_groups__process_create(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_groups__process_delete(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_groups__process_get(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_groups__process_list(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_groups__process_modify(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_groups__process_remove_client(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_groups__process_remove_role(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_groups__process_get_anonymous_group(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_groups__process_set_anonymous_group(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_groups__remove_client(const char *username, const char *groupname, bool update_config);
struct dynsec__group *dynsec_groups__find(const char *groupname);


/* ################################################################
 * #
 * # Group List Functions
 * #
 * ################################################################ */

cJSON *dynsec_grouplist__all_to_json(struct dynsec__grouplist *base_grouplist);
int dynsec_grouplist__add(struct dynsec__grouplist **base_grouplist, struct dynsec__group *group, int priority);
void dynsec_grouplist__cleanup(struct dynsec__grouplist **base_grouplist);
void dynsec_grouplist__remove(struct dynsec__grouplist **base_grouplist, struct dynsec__group *group);


/* ################################################################
 * #
 * # Role Functions
 * #
 * ################################################################ */

void dynsec_roles__cleanup(void);
int dynsec_roles__config_load(cJSON *tree);
int dynsec_roles__config_save(cJSON *tree);
int dynsec_roles__process_add_acl(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_roles__process_create(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_roles__process_delete(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_roles__process_get(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_roles__process_list(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_roles__process_modify(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
int dynsec_roles__process_remove_acl(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data);
struct dynsec__role *dynsec_roles__find(const char *rolename);


/* ################################################################
 * #
 * # Role List Functions
 * #
 * ################################################################ */

int dynsec_rolelist__channel_add(struct dynsec__channel * channel, struct dynsec__role *role, int priority);
int dynsec_rolelist__channel_remove(struct dynsec__channel * channel, struct dynsec__role *role);
int dynsec_rolelist__group_add(struct dynsec__group *group, struct dynsec__role *role, int priority);
void dynsec_rolelist__group_remove(struct dynsec__group *group, struct dynsec__role *role);
int dynsec_rolelist__load_from_json(cJSON *command, struct dynsec__rolelist **rolelist);
void dynsec_rolelist__cleanup(struct dynsec__rolelist **base_rolelist);
cJSON *dynsec_rolelist__all_to_json(struct dynsec__rolelist *base_rolelist);

#endif  // __DYNAMIC_SECURITY_H__
