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

#include <time.h>

#include "config.h"

#include "dynamic_security.h"
#include "mosquitto.h"
#include "mosquitto_broker.h"
#include "mosquitto_plugin.h"

typedef int (*MOSQ_FUNC_acl_check)(struct mosquitto_evt_acl_check *, struct dynsec__rolelist *);

/* FIXME - CACHE! */

/* ################################################################
 * #
 * # ACL check - publish broker to client
 * #
 * ################################################################ */

static int acl_check_publish_c_recv(struct mosquitto_evt_acl_check *ed, struct dynsec__rolelist *base_rolelist)
{
	struct dynsec__rolelist *rolelist, *rolelist_tmp = NULL;
	struct dynsec__acl *acl, *acl_tmp = NULL;
	bool result;

	HASH_ITER(hh, base_rolelist, rolelist, rolelist_tmp){
		HASH_ITER(hh, rolelist->role->acls.publish_c_recv, acl, acl_tmp){
			mosquitto_topic_matches_sub(acl->topic, ed->topic, &result);
			if(result){
				if(acl->allow){
					return MOSQ_ERR_SUCCESS;
				}else{
					return MOSQ_ERR_ACL_DENIED;
				}
			}
		}
	}
	return MOSQ_ERR_NOT_FOUND;
}


/* ################################################################
 * #
 * # ACL check - publish client to broker
 * #
 * ################################################################ */

static int acl_check_publish_c_send(struct mosquitto_evt_acl_check *ed, struct dynsec__rolelist *base_rolelist)
{
	struct dynsec__rolelist *rolelist, *rolelist_tmp = NULL;
	struct dynsec__acl *acl, *acl_tmp = NULL;
	bool result;

	HASH_ITER(hh, base_rolelist, rolelist, rolelist_tmp){
		HASH_ITER(hh, rolelist->role->acls.publish_c_send, acl, acl_tmp){
			mosquitto_topic_matches_sub(acl->topic, ed->topic, &result);
			if(result){
				if(acl->allow){
					return MOSQ_ERR_SUCCESS;
				}else{
					return MOSQ_ERR_ACL_DENIED;
				}
			}
		}
	}
	return MOSQ_ERR_NOT_FOUND;
}


/* ################################################################
 * #
 * # ACL check - subscribe
 * #
 * ################################################################ */

static int acl_check_subscribe(struct mosquitto_evt_acl_check *ed, struct dynsec__rolelist *base_rolelist)
{
	struct dynsec__rolelist *rolelist, *rolelist_tmp = NULL;
	struct dynsec__acl *acl, *acl_tmp = NULL;
	size_t len;

	len = strlen(ed->topic);

	HASH_ITER(hh, base_rolelist, rolelist, rolelist_tmp){
		HASH_FIND(hh, rolelist->role->acls.subscribe_literal, ed->topic, len, acl);
		if(acl){
			if(acl->allow){
				return MOSQ_ERR_SUCCESS;
			}else{
				return MOSQ_ERR_ACL_DENIED;
			}
		}
		HASH_ITER(hh, rolelist->role->acls.subscribe_pattern, acl, acl_tmp){
			if(sub_acl_check(acl->topic, ed->topic)){
				if(acl->allow){
					return MOSQ_ERR_SUCCESS;
				}else{
					return MOSQ_ERR_ACL_DENIED;
				}
			}
		}
	}
	return MOSQ_ERR_NOT_FOUND;
}


/* ################################################################
 * #
 * # ACL check - unsubscribe
 * #
 * ################################################################ */

static int acl_check_unsubscribe(struct mosquitto_evt_acl_check *ed, struct dynsec__rolelist *base_rolelist)
{
	struct dynsec__rolelist *rolelist, *rolelist_tmp = NULL;
	struct dynsec__acl *acl, *acl_tmp = NULL;
	size_t len;

	len = strlen(ed->topic);

	HASH_ITER(hh, base_rolelist, rolelist, rolelist_tmp){
		HASH_FIND(hh, rolelist->role->acls.unsubscribe_literal, ed->topic, len, acl);
		if(acl){
			if(acl->allow){
				return MOSQ_ERR_SUCCESS;
			}else{
				return MOSQ_ERR_ACL_DENIED;
			}
		}
		HASH_ITER(hh, rolelist->role->acls.unsubscribe_pattern, acl, acl_tmp){
			if(sub_acl_check(acl->topic, ed->topic)){
				if(acl->allow){
					return MOSQ_ERR_SUCCESS;
				}else{
					return MOSQ_ERR_ACL_DENIED;
				}
			}
		}
	}
	return MOSQ_ERR_NOT_FOUND;
}


/* ################################################################
 * #
 * # ACL check - generic check
 * #
 * ################################################################ */

static int acl_check(struct mosquitto_evt_acl_check *ed, MOSQ_FUNC_acl_check check, bool acl_default_access)
{
	struct dynsec__channel * channel;
	struct dynsec__grouplist *grouplist, *grouplist_tmp = NULL;
	const char * username;
	const char * clientid;
	int rc;

	username = mosquitto_client_username(ed->client);
	clientid = mosquitto_client_clientid(ed->client);

	// First check channels assigned to clientid or username
	channel = dynsec_channels__find(clientid, username);
	if(channel != NULL){
		if(check == acl_check_publish_c_send){
			++channel->msg_received;
			channel->msg_timestamp = time(NULL);
		}
		rc = check(ed, channel->rolelist);
		if(rc != MOSQ_ERR_NOT_FOUND){
			return rc;
		}
		HASH_ITER(hh, channel->grouplist, grouplist, grouplist_tmp){
			rc = check(ed, grouplist->group->rolelist);
			if(rc != MOSQ_ERR_NOT_FOUND){
				return rc;
			}
		}
		// Fallback to a defaultACLAccess check
		if(acl_default_access == false){
			return MOSQ_ERR_ACL_DENIED;
		}else{
			if(!strncmp(ed->topic, "$CONTROL", strlen("$CONTROL"))){
				/* We never give fall through access to $CONTROL topics, they must
				* be granted explicitly. */
				return MOSQ_ERR_ACL_DENIED;
			}else{
				return MOSQ_ERR_SUCCESS;
			}
		}
	}
	// No assigned channels. Now, we check if public access is available
	// Check if a special anonymous group is configured
	if(dynsec_anonymous_group){
		/* If we have a group for anonymous users, use that for checking. */
		rc = check(ed, dynsec_anonymous_group->rolelist);
		if(rc != MOSQ_ERR_NOT_FOUND){
			return rc;
		}
	}

	return MOSQ_ERR_ACL_DENIED;
}


/* ################################################################
 * #
 * # ACL check - plugin callback
 * #
 * ################################################################ */

int dynsec__acl_check_callback(int event, void *event_data, void *userdata)
{
	struct mosquitto_evt_acl_check *ed = event_data;

	UNUSED(event);
	UNUSED(userdata);

	/* ACL checks are made in the order below until a match occurs, at which
	 * point the decision is made.
	 *
	 * User roles in priority order highest to lowest.
	 *    Roles have their ACLs checked in priority order, highest to lowest
	 * Groups are processed in priority order highest to lowest
	 *    Group roles are processed in priority order, highest to lowest
	 *       Roles have their ACLs checked in priority order, highest to lowest
	 */

	switch(ed->access){
		case MOSQ_ACL_SUBSCRIBE:
			return acl_check(event_data, acl_check_subscribe, default_access.subscribe);
		case MOSQ_ACL_UNSUBSCRIBE:
			return acl_check(event_data, acl_check_unsubscribe, default_access.unsubscribe);
		case MOSQ_ACL_WRITE: /* channel to broker */
			return acl_check(event_data, acl_check_publish_c_send, default_access.publish_c_send);
		case MOSQ_ACL_READ:
			return acl_check(event_data, acl_check_publish_c_recv, default_access.publish_c_recv);
		default:
			return MOSQ_ERR_PLUGIN_DEFER;
	}
	return MOSQ_ERR_PLUGIN_DEFER;
}
