/*
Copyright (c) 2020 Roger Light <roger@atchoo.org>

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
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mosquitto.h"
#include "mosquitto_ctrl.h"
#include "get_password.h"
#include "password_mosq.h"

int dynsec_client__create(int argc, char *argv[], cJSON *j_command)
{
	char *username = NULL, *password = NULL;
	char prompt[200], verify_prompt[200];
	char password_buf[200];
	int rc;

	if(argc == 1){
		username = argv[0];

		printf("Enter new password for %s. Press return for no password (user will be unable to login).\n", username);
		snprintf(prompt, sizeof(prompt), "New password for %s: ", username);
		snprintf(verify_prompt, sizeof(verify_prompt), "Reenter password for %s: ", username);
		rc = get_password(prompt, verify_prompt, true, password_buf, sizeof(password_buf));
		if(rc == 0){
			password = password_buf;
		}else{
			password = NULL;
			printf("\n");
		}
	}else if(argc == 2){
		username = argv[0];
		password = argv[1];
	}else{
		return MOSQ_ERR_INVAL;
	}

	if(cJSON_AddStringToObject(j_command, "command", "createClient") == NULL
			|| cJSON_AddStringToObject(j_command, "username", username) == NULL
			|| (password && cJSON_AddStringToObject(j_command, "password", password) == NULL)
			){

		return MOSQ_ERR_NOMEM;
	}else{
		return MOSQ_ERR_SUCCESS;
	}
}

int dynsec_client__delete(int argc, char *argv[], cJSON *j_command)
{
	char *username = NULL;

	if(argc == 1){
		username = argv[0];
	}else{
		return MOSQ_ERR_INVAL;
	}

	if(cJSON_AddStringToObject(j_command, "command", "deleteClient") == NULL
			|| cJSON_AddStringToObject(j_command, "username", username) == NULL
			){

		return MOSQ_ERR_NOMEM;
	}else{
		return MOSQ_ERR_SUCCESS;
	}
}

int dynsec_client__enable_disable(int argc, char *argv[], cJSON *j_command, const char *command)
{
	char *username = NULL;

	if(argc == 1){
		username = argv[0];
	}else{
		return MOSQ_ERR_INVAL;
	}

	if(cJSON_AddStringToObject(j_command, "command", command) == NULL
			|| cJSON_AddStringToObject(j_command, "username", username) == NULL
			){

		return MOSQ_ERR_NOMEM;
	}else{
		return MOSQ_ERR_SUCCESS;
	}
}

int dynsec_client__set_password(int argc, char *argv[], cJSON *j_command)
{
	char *username = NULL, *password = NULL;
	char prompt[200], verify_prompt[200];
	char password_buf[200];
	int rc;

	if(argc == 2){
		username = argv[0];
		password = argv[1];
	}else if(argc == 1){
		username = argv[0];

		snprintf(prompt, sizeof(prompt), "New password for %s: ", username);
		snprintf(verify_prompt, sizeof(verify_prompt), "Reenter password for %s: ", username);
		rc = get_password(prompt, verify_prompt, false, password_buf, sizeof(password_buf));
		if(rc){
			return MOSQ_ERR_INVAL;
		}
		password = password_buf;
	}else{
		return MOSQ_ERR_INVAL;
	}

	if(cJSON_AddStringToObject(j_command, "command", "setClientPassword") == NULL
			|| cJSON_AddStringToObject(j_command, "username", username) == NULL
			|| cJSON_AddStringToObject(j_command, "password", password) == NULL
			){

		return MOSQ_ERR_NOMEM;
	}else{
		return MOSQ_ERR_SUCCESS;
	}
}

int dynsec_client__get(int argc, char *argv[], cJSON *j_command)
{
	char *username = NULL;

	if(argc == 1){
		username = argv[0];
	}else{
		return MOSQ_ERR_INVAL;
	}

	if(cJSON_AddStringToObject(j_command, "command", "getClient") == NULL
			|| cJSON_AddStringToObject(j_command, "username", username) == NULL
			){

		return MOSQ_ERR_NOMEM;
	}else{
		return MOSQ_ERR_SUCCESS;
	}
}

int dynsec_client__add_remove_role(int argc, char *argv[], cJSON *j_command, const char *command)
{
	char *username = NULL, *rolename = NULL;
	int priority = -1;

	if(argc == 2){
		username = argv[0];
		rolename = argv[1];
	}else if(argc == 3){
		username = argv[0];
		rolename = argv[1];
		priority = atoi(argv[2]);
	}else{
		return MOSQ_ERR_INVAL;
	}

	if(cJSON_AddStringToObject(j_command, "command", command) == NULL
			|| cJSON_AddStringToObject(j_command, "username", username) == NULL
			|| cJSON_AddStringToObject(j_command, "rolename", rolename) == NULL
			|| (priority != -1 && cJSON_AddIntToObject(j_command, "priority", priority) == NULL)
			){

		return MOSQ_ERR_NOMEM;
	}else{
		return MOSQ_ERR_SUCCESS;
	}
}

int dynsec_client__list_all(int argc, char *argv[], cJSON *j_command)
{
	int count = -1, offset = -1;

	if(argc == 0){
		/* All clients */
	}else if(argc == 1){
		count = atoi(argv[0]);
	}else if(argc == 2){
		count = atoi(argv[0]);
		offset = atoi(argv[1]);
	}else{
		return MOSQ_ERR_INVAL;
	}

	if(cJSON_AddStringToObject(j_command, "command", "listClients") == NULL
			|| (count > 0 && cJSON_AddIntToObject(j_command, "count", count) == NULL)
			|| (offset > 0 && cJSON_AddIntToObject(j_command, "offset", offset) == NULL)
			){

		return MOSQ_ERR_NOMEM;
	}else{
		return MOSQ_ERR_SUCCESS;
	}
}
