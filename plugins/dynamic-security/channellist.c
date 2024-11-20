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

/* ################################################################
 * #
 * # Function declarations
 * #
 * ################################################################ */

/* ################################################################
 * #
 * # Local variables
 * #
 * ################################################################ */


/* ################################################################
 * #
 * # Utility functions
 * #
 * ################################################################ */

static int dynsec_channellist__cmp(void *a, void *b)
{
    struct dynsec__channellist * channellist_a = a;
    struct dynsec__channellist * channellist_b = b;

    return strcmp(channellist_a->channel->chanid, channellist_b->channel->chanid);
}


void dynsec_channellist__kick_all(struct dynsec__channellist *base_channellist)
{
    struct dynsec__channellist * channellist, * channellist_tmp;

    HASH_ITER(hh, base_channellist, channellist, channellist_tmp){
        if(channellist->channel->clientid){
            mosquitto_kick_client_by_clientid(channellist->channel->clientid, false);
        }else{
            mosquitto_kick_client_by_username(channellist->channel->username, false);
        }
    }
}

cJSON *dynsec_channellist__all_to_json(struct dynsec__channellist *base_channellist)
{
    struct dynsec__channellist * channellist, * channellist_tmp;
    cJSON *j_channels, *j_channel;

    j_channels = cJSON_CreateArray();
    if(j_channels == NULL) return NULL;

    HASH_ITER(hh, base_channellist, channellist, channellist_tmp){
        j_channel = cJSON_CreateObject();
        if(j_channel == NULL){
            cJSON_Delete(j_channels);
            return NULL;
        }
        cJSON_AddItemToArray(j_channels, j_channel);

        if(cJSON_AddStringToObject(j_channel, "chanid", channellist->channel->chanid) == NULL
                || (channellist->priority != -1 && cJSON_AddIntToObject(j_channel, "priority", channellist->priority) == NULL)
                ){

            cJSON_Delete(j_channels);
            return NULL;
        }
    }
    return j_channels;
}


int dynsec_channellist__add(struct dynsec__channellist **base_channellist, struct dynsec__channel * channel, int priority)
{
    struct dynsec__channellist * channellist;

    HASH_FIND(hh, *base_channellist, channel->chanid, strlen(channel->chanid), channellist);
    if(channellist != NULL){
        /* channel is already in the group */
        return MOSQ_ERR_SUCCESS;
    }

    channellist = mosquitto_malloc(sizeof(struct dynsec__channellist));
    if(channellist == NULL){
        return MOSQ_ERR_NOMEM;
    }

    channellist->channel = channel;
    channellist->priority = priority;
    HASH_ADD_KEYPTR_INORDER(hh, *base_channellist, channel->chanid, strlen(channel->chanid), channellist, dynsec_channellist__cmp);

    return MOSQ_ERR_SUCCESS;
}


void dynsec_channellist__cleanup(struct dynsec__channellist **base_channellist)
{
    struct dynsec__channellist * channellist, * channellist_tmp;

    HASH_ITER(hh, *base_channellist, channellist, channellist_tmp){
        HASH_DELETE(hh, *base_channellist, channellist);
        mosquitto_free(channellist);
    }
}


void dynsec_channellist__remove(struct dynsec__channellist **base_channellist, struct dynsec__channel * channel)
{
    struct dynsec__channellist * channellist;

    HASH_FIND(hh, *base_channellist, channel->chanid, strlen(channel->chanid), channellist);
    if(channellist){
        HASH_DELETE(hh, *base_channellist, channellist);
        mosquitto_free(channellist);
    }
}
