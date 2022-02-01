/*
Copyright (c) 2009-2020 Roger Light <roger@atchoo.org>

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

#include "config.h"
#include <time.h>
#include "mosquitto_broker_internal.h"


static time_t last_keepalive_check = 0;

/* FIXME - this is the prototype for the future tree/trie based keepalive check implementation. */

int keepalive__add(struct mosquitto *context)
{
	return MOSQ_ERR_SUCCESS;
}


void keepalive__check(struct mosquitto_db *db, time_t now)
{
	struct mosquitto *context, *ctxt_tmp;

	if(last_keepalive_check + 5 < now){
		last_keepalive_check = now;

		/* FIXME - this needs replacing with something more efficient */
		HASH_ITER(hh_sock, db->contexts_by_sock, context, ctxt_tmp){
			if(context->sock != INVALID_SOCKET){
				/* Local bridges never time out in this fashion. */
				if(!(context->keepalive)
						|| context->bridge
						|| now - context->last_msg_in <= (time_t)(context->keepalive)*3/2){

				}else{
					/* Client has exceeded keepalive*1.5 */
					do_disconnect(db, context, MOSQ_ERR_KEEPALIVE);
				}
			}
		}
	}
}


int keepalive__remove(struct mosquitto *context)
{
	return MOSQ_ERR_SUCCESS;
}


void keepalive__remove_all(void)
{
}


int keepalive__update(struct mosquitto *context)
{
	context->last_msg_in = mosquitto_time();
	return MOSQ_ERR_SUCCESS;
}
