/*
Copyright (c) 2010-2020 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License v1.0
and Eclipse Distribution License v1.0 which accompany this distribution.
 
The Eclipse Public License is available at
   http://www.eclipse.org/legal/epl-v10.html
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.
 
Contributors:
   Roger Light - initial implementation and documentation.
*/
#ifndef PACKET_MOSQ_H
#define PACKET_MOSQ_H

#include "mosquitto_internal.h"
#include "mosquitto.h"

#ifdef WITH_BROKER
struct mosquitto_db;
#endif

int packet__alloc(struct mosquitto__packet *packet);
void packet__cleanup(struct mosquitto__packet *packet);
void packet__cleanup_all(struct mosquitto *mosq);
int packet__queue(struct mosquitto *mosq, struct mosquitto__packet *packet);

int packet__check_oversize(struct mosquitto *mosq, uint32_t remaining_length);

int packet__read_byte(struct mosquitto__packet *packet, uint8_t *byte);
int packet__read_bytes(struct mosquitto__packet *packet, void *bytes, uint32_t count);
int packet__read_binary(struct mosquitto__packet *packet, uint8_t **data, uint16_t *length);
int packet__read_string(struct mosquitto__packet *packet, char **str, uint16_t *length);
int packet__read_uint16(struct mosquitto__packet *packet, uint16_t *word);
int packet__read_uint32(struct mosquitto__packet *packet, uint32_t *word);
int packet__read_varint(struct mosquitto__packet *packet, uint32_t *word, uint8_t *bytes);

void packet__write_byte(struct mosquitto__packet *packet, uint8_t byte);
void packet__write_bytes(struct mosquitto__packet *packet, const void *bytes, uint32_t count);
void packet__write_string(struct mosquitto__packet *packet, const char *str, uint16_t length);
void packet__write_uint16(struct mosquitto__packet *packet, uint16_t word);
void packet__write_uint32(struct mosquitto__packet *packet, uint32_t word);
int packet__write_varint(struct mosquitto__packet *packet, uint32_t word);

unsigned int packet__varint_bytes(uint32_t word);

int packet__write(struct mosquitto *mosq);
#ifdef WITH_BROKER
int packet__read(struct mosquitto_db *db, struct mosquitto *mosq);
#else
int packet__read(struct mosquitto *mosq);
#endif

#endif
