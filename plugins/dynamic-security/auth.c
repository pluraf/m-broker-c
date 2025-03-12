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

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "dynamic_security.h"
#include "mosquitto.h"
#include "mosquitto_broker.h"
#include "jwt/jwt.h"
#include "jwt/jwt_helpers.h"


/* ################################################################
 * #
 * # Base64 encoding/decoding
 * #
 * ################################################################ */

int dynsec_auth__base64_encode(unsigned char *in, int in_len, char **encoded)
{
    BIO *bmem, *b64;
    BUF_MEM *bptr = NULL;

    if(in_len < 0) return 1;

    b64 = BIO_new(BIO_f_base64());
    if(b64 == NULL) return 1;

    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bmem = BIO_new(BIO_s_mem());
    if(bmem == NULL){
        BIO_free_all(b64);
        return 1;
    }
    b64 = BIO_push(b64, bmem);
    BIO_write(b64, in, in_len);
    if(BIO_flush(b64) != 1){
        BIO_free_all(b64);
        return 1;
    }
    BIO_get_mem_ptr(b64, &bptr);
    *encoded = mosquitto_malloc(bptr->length+1);
    if(!(*encoded)){
        BIO_free_all(b64);
        return 1;
    }
    memcpy(*encoded, bptr->data, bptr->length);
    (*encoded)[bptr->length] = '\0';
    BIO_free_all(b64);

    return 0;
}

int dynsec_auth__base64_decode(char *in, unsigned char **decoded, int *decoded_len)
{
    BIO *bmem, *b64;
    size_t slen;

    slen = strlen(in);

    b64 = BIO_new(BIO_f_base64());
    if(!b64){
        return 1;
    }
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);

    bmem = BIO_new(BIO_s_mem());
    if(!bmem){
        BIO_free_all(b64);
        return 1;
    }
    b64 = BIO_push(b64, bmem);
    BIO_write(bmem, in, (int)slen);

    if(BIO_flush(bmem) != 1){
        BIO_free_all(b64);
        return 1;
    }
    *decoded = mosquitto_calloc(slen, 1);
    if(!(*decoded)){
        BIO_free_all(b64);
        return 1;
    }
    *decoded_len =  BIO_read(b64, *decoded, (int)slen);
    BIO_free_all(b64);

    if(*decoded_len <= 0){
        mosquitto_free(*decoded);
        *decoded = NULL;
        *decoded_len = 0;
        return 1;
    }

    return 0;
}

/* ################################################################
 * #
 * # Password functions
 * #
 * ################################################################ */

int dynsec_auth__pw_hash(struct dynsec__channel * channel, const char *password, unsigned char *password_hash, int password_hash_len, bool new_password)
{
    const EVP_MD *digest;
    int iterations;

    if(new_password){
        if(RAND_bytes(channel->pw.salt, sizeof(channel->pw.salt)) != 1){
            return MOSQ_ERR_UNKNOWN;
        }
        iterations = PW_DEFAULT_ITERATIONS;
    }else{
        iterations = channel->pw.iterations;
    }
    if(iterations < 1){
        return MOSQ_ERR_INVAL;
    }
    channel->pw.iterations = iterations;

    digest = EVP_get_digestbyname("sha512");
    if(!digest){
        return MOSQ_ERR_UNKNOWN;
    }

    return !PKCS5_PBKDF2_HMAC(password, (int)strlen(password),
            channel->pw.salt, sizeof(channel->pw.salt), iterations,
            digest, password_hash_len, password_hash);
}

/* ################################################################
 * #
 * # Username/password check
 * #
 * ################################################################ */

static int memcmp_const(const void *a, const void *b, size_t len)
{
    size_t i;
    int rc = 0;

    if(!a || !b) return 1;

    for(i=0; i<len; i++){
        rc |= ((char *)a)[i] ^ ((char *)b)[i];
    }
    return rc;
}

/* ################################################################
 * #
 * # Authentication selection functions
 * #
 * ################################################################ */

int dynsec_auth__authenticate(const struct dynsec__channel * channel, const struct mosquitto_evt_basic_auth *ed)
{
    if(strcmp(channel->authtype, MQTT_AUTH_PASSWORD) == 0){
        unsigned char password_hash[64];  // For SHA512
        if(channel->pw.valid && dynsec_auth__pw_hash(channel, ed->password, password_hash, sizeof(password_hash), false) == MOSQ_ERR_SUCCESS){
            if(memcmp_const(channel->pw.password_hash, password_hash, sizeof(password_hash)) == 0){
                return MOSQ_ERR_SUCCESS;
            }else{
                return MOSQ_ERR_AUTH;
            }
        }
    }else if(strcmp(channel->authtype, MQTT_AUTH_JWT_ES256) == 0){
        point_t key;
        int rc = public_key_from_pem(channel->jwtkey, &key);
        if(rc != 0){
            mosquitto_log_printf(MOSQ_LOG_WARNING, "Can't load public key");
            return MOSQ_ERR_AUTH;
        }
        if(jwt_verify(ed->password, &key) == 1) return MOSQ_ERR_SUCCESS;
    }
    return MOSQ_ERR_PLUGIN_DEFER;
}

/* ################################################################
 * #
 * # Plugin callback
 * #
 * ################################################################ */

int dynsec_auth__basic_auth_callback(int event, void *event_data, void *userdata)
{
    struct mosquitto_evt_basic_auth *ed = event_data;
    struct dynsec__channel * channel;
    const char * clientid;
    const char * authtype;

    UNUSED(event);
    UNUSED(userdata);

    if(ed->username == NULL && ed->password == NULL) return MOSQ_ERR_PLUGIN_DEFER;

    clientid = mosquitto_client_id(ed->client);
    channel = dynsec_channels__find(clientid, ed->username);

    if(channel){
        if(channel->enabled) return MOSQ_ERR_AUTH;
        return dynsec_auth__authenticate(channel, ed);
    }

    return MOSQ_ERR_PLUGIN_DEFER;
}
