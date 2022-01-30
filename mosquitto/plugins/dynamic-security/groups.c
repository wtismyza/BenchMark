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

#include "config.h"

#include <cJSON.h>
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

static int group_cmp(void *a, void *b)
{
	struct dynsec__group *group_a = a;
	struct dynsec__group *group_b = b;

	return strcmp(group_a->groupname, group_b->groupname);
}

int dynsec_grouplist__cmp(void *a, void *b)
{
	int prio;
	struct dynsec__grouplist *grouplist_a = a;
	struct dynsec__grouplist *grouplist_b = b;

	prio = grouplist_b->priority - grouplist_a->priority;
	if(prio == 0){
		return strcmp(grouplist_a->groupname, grouplist_b->groupname);
	}else{
		return prio;
	}
}

void dynsec_clientlist__kick_all(struct dynsec__clientlist *base_clientlist)
{
	struct dynsec__clientlist *clientlist, *clientlist_tmp;

	HASH_ITER(hh, base_clientlist, clientlist, clientlist_tmp){
		mosquitto_kick_client_by_username(clientlist->client->username, false);
	}
}

cJSON *dynsec_clientlists__all_to_json(struct dynsec__clientlist *base_clientlist)
{
	struct dynsec__clientlist *clientlist, *clientlist_tmp;
	cJSON *j_clients, *j_client;

	j_clients = cJSON_CreateArray();
	if(j_clients == NULL) return NULL;

	HASH_ITER(hh, base_clientlist, clientlist, clientlist_tmp){
		j_client = cJSON_CreateObject();
		if(j_client == NULL){
			cJSON_Delete(j_clients);
			return NULL;
		}
		cJSON_AddItemToArray(j_clients, j_client);

		if(cJSON_AddStringToObject(j_client, "username", clientlist->client->username) == NULL
				|| (clientlist->priority != -1 && cJSON_AddIntToObject(j_client, "priority", clientlist->priority) == NULL)
				){

			cJSON_Delete(j_clients);
			return NULL;
		}
	}
	return j_clients;
}


cJSON *dynsec_grouplists__all_to_json(struct dynsec__grouplist *base_grouplist)
{
	struct dynsec__grouplist *grouplist, *grouplist_tmp;
	cJSON *j_groups, *j_group;

	j_groups = cJSON_CreateArray();
	if(j_groups == NULL) return NULL;

	HASH_ITER(hh, base_grouplist, grouplist, grouplist_tmp){
		j_group = cJSON_CreateObject();
		if(j_group == NULL){
			cJSON_Delete(j_groups);
			return NULL;
		}
		cJSON_AddItemToArray(j_groups, j_group);

		if(cJSON_AddStringToObject(j_group, "groupname", grouplist->group->groupname) == NULL
				|| (grouplist->priority != -1 && cJSON_AddIntToObject(j_group, "priority", grouplist->priority) == NULL)
				){

			cJSON_Delete(j_groups);
			return NULL;
		}
	}
	return j_groups;
}


static void group__free_item(struct dynsec__group *group)
{
	if(group == NULL) return;

	HASH_DEL(local_groups, group);
	dynsec__remove_all_clients_from_group(group);
	mosquitto_free(group->text_name);
	mosquitto_free(group->text_description);
	mosquitto_free(group->groupname);
	dynsec_rolelists__free_all(&group->rolelist);
	mosquitto_free(group);
}

struct dynsec__group *dynsec_groups__find(const char *groupname)
{
	struct dynsec__group *group = NULL;

	if(groupname){
		HASH_FIND(hh, local_groups, groupname, strlen(groupname), group);
	}
	return group;
}

int dynsec_groups__process_add_role(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
	char *groupname, *rolename;
	struct dynsec__group *group;
	struct dynsec__role *role;
	int priority;

	if(json_get_string(command, "groupname", &groupname, false) != MOSQ_ERR_SUCCESS){
		dynsec__command_reply(j_responses, context, "addGroupRole", "Invalid/missing groupname", correlation_data);
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(groupname, (int)strlen(groupname)) != MOSQ_ERR_SUCCESS){
		dynsec__command_reply(j_responses, context, "addGroupRole", "Group name not valid UTF-8", correlation_data);
		return MOSQ_ERR_INVAL;
	}

	if(json_get_string(command, "roleName", &rolename, false) != MOSQ_ERR_SUCCESS){
		dynsec__command_reply(j_responses, context, "addGroupRole", "Invalid/missing roleName", correlation_data);
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

	dynsec_rolelists__group_add_role(group, role, priority);
	dynsec__config_save();
	dynsec__command_reply(j_responses, context, "addGroupRole", NULL, correlation_data);
	return MOSQ_ERR_SUCCESS;
}


void dynsec_groups__cleanup(void)
{
	struct dynsec__group *group, *group_tmp;

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
	cJSON *j_clientlist, *j_client, *j_username;
	cJSON *j_roles, *j_role, *j_rolename;

	struct dynsec__group *group;
	struct dynsec__role *role;
	char *str;
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
			group = mosquitto_calloc(1, sizeof(struct dynsec__group));
			if(group == NULL){
				// FIXME log
				return MOSQ_ERR_NOMEM;
			}

			/* Group name */
			if(json_get_string(j_group, "groupname", &str, false) != MOSQ_ERR_SUCCESS){
				// FIXME log
				mosquitto_free(group);
				continue;
			}
			group->groupname = strdup(str);
			if(group->groupname == NULL){
				// FIXME log
				mosquitto_free(group);
				continue;
			}

			/* Text name */
			if(json_get_string(j_group, "textname", &str, true) == MOSQ_ERR_SUCCESS){
				if(str){
					group->text_name = strdup(str);
					if(group->text_name == NULL){
						// FIXME log
						mosquitto_free(group->groupname);
						mosquitto_free(group);
						continue;
					}
				}
			}

			/* Text description */
			if(json_get_string(j_group, "textdescription", &str, true) == MOSQ_ERR_SUCCESS){
				if(str){
					group->text_description = strdup(str);
					if(group->text_description == NULL){
						// FIXME log
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
						j_rolename = cJSON_GetObjectItem(j_role, "roleName");
						if(j_rolename && cJSON_IsString(j_rolename)){
							json_get_int(j_role, "priority", &priority, true, -1);
							role = dynsec_roles__find(j_rolename->valuestring);
							dynsec_rolelists__group_add_role(group, role, priority);
						}
					}
				}
			}

			/* This must go before clients are loaded, otherwise the group won't be found */
			HASH_ADD_KEYPTR(hh, local_groups, group->groupname, strlen(group->groupname), group);

			/* Clients */
			j_clientlist = cJSON_GetObjectItem(j_group, "clients");
			if(j_clientlist && cJSON_IsArray(j_clientlist)){
				cJSON_ArrayForEach(j_client, j_clientlist){
					if(cJSON_IsObject(j_client)){
						j_username = cJSON_GetObjectItem(j_client, "username");
						if(j_username && cJSON_IsString(j_username)){
							json_get_int(j_client, "priority", &priority, true, -1);
							dynsec_groups__add_client(j_username->valuestring, group->groupname, priority, false);
						}
					}
				}
			}
		}
	}
	HASH_SORT(local_groups, group_cmp);

	j_group = cJSON_GetObjectItem(tree, "anonymousGroup");
	if(j_group && cJSON_IsString(j_group)){
		dynsec_anonymous_group = dynsec_groups__find(j_group->valuestring);
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
	struct dynsec__group *group, *group_tmp;
	cJSON *j_group, *j_clients, *j_roles;

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

		j_roles = dynsec_rolelists__all_to_json(group->rolelist);
		if(j_roles == NULL){
			return 1;
		}
		cJSON_AddItemToObject(j_group, "roles", j_roles);

		j_clients = dynsec_clientlists__all_to_json(group->clientlist);
		if(j_clients == NULL){
			return 1;
		}
		cJSON_AddItemToObject(j_group, "clients", j_clients);
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

	rc = dynsec_rolelists__load_from_json(command, &group->rolelist);
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

	dynsec__config_save();
	dynsec__command_reply(j_responses, context, "createGroup", NULL, correlation_data);
	return MOSQ_ERR_SUCCESS;
}


int dynsec_groups__process_delete(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
	char *groupname;
	struct dynsec__group *group;

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
		/* Enforce any changes */
		if(group == dynsec_anonymous_group){
			mosquitto_kick_client_by_username(NULL, false);
		}
		dynsec_clientlist__kick_all(group->clientlist);

		group__free_item(group);
		dynsec__config_save();
		dynsec__command_reply(j_responses, context, "deleteGroup", NULL, correlation_data);

		return MOSQ_ERR_SUCCESS;
	}else{
		dynsec__command_reply(j_responses, context, "deleteGroup", "Group not found", correlation_data);
		return MOSQ_ERR_SUCCESS;
	}
}


int dynsec_groups__add_client(const char *username, const char *groupname, int priority, bool update_config)
{
	struct dynsec__client *client;
	struct dynsec__clientlist *clientlist;
	struct dynsec__group *group;
	struct dynsec__grouplist *grouplist;

	client = dynsec_clients__find(username);
	if(client == NULL){
		return ERR_USER_NOT_FOUND;
	}

	group = dynsec_groups__find(groupname);
	if(group == NULL){
		return ERR_GROUP_NOT_FOUND;
	}

	HASH_FIND(hh, group->clientlist, username, strlen(username), clientlist);
	if(clientlist != NULL){
		/* Client is already in the group */
		return MOSQ_ERR_SUCCESS;
	}

	clientlist = mosquitto_malloc(sizeof(struct dynsec__clientlist));
	grouplist = mosquitto_malloc(sizeof(struct dynsec__grouplist));
	if(clientlist == NULL || grouplist == NULL){
		mosquitto_free(clientlist);
		mosquitto_free(grouplist);
		return MOSQ_ERR_UNKNOWN;
	}

	clientlist->username = client->username;
	clientlist->client = client;
	clientlist->priority = priority;
	HASH_ADD_KEYPTR_INORDER(hh, group->clientlist, clientlist->username, strlen(clientlist->username), clientlist, dynsec_clientlist__cmp);

	grouplist->groupname = group->groupname;
	grouplist->group = group;
	grouplist->priority = priority;
	HASH_ADD_KEYPTR_INORDER(hh, client->grouplist, grouplist->groupname, strlen(grouplist->groupname), grouplist, dynsec_grouplist__cmp);

	if(update_config){
		dynsec__config_save();
	}

	return MOSQ_ERR_SUCCESS;
}


int dynsec_groups__process_add_client(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
	char *username, *groupname;
	int rc;
	int priority;

	if(json_get_string(command, "username", &username, false) != MOSQ_ERR_SUCCESS){
		dynsec__command_reply(j_responses, context, "addGroupClient", "Invalid/missing username", correlation_data);
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(username, (int)strlen(username)) != MOSQ_ERR_SUCCESS){
		dynsec__command_reply(j_responses, context, "addGroupClient", "Username not valid UTF-8", correlation_data);
		return MOSQ_ERR_INVAL;
	}

	if(json_get_string(command, "groupname", &groupname, false) != MOSQ_ERR_SUCCESS){
		dynsec__command_reply(j_responses, context, "addGroupClient", "Invalid/missing groupname", correlation_data);
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(groupname, (int)strlen(groupname)) != MOSQ_ERR_SUCCESS){
		dynsec__command_reply(j_responses, context, "addGroupClient", "Group name not valid UTF-8", correlation_data);
		return MOSQ_ERR_INVAL;
	}

	json_get_int(command, "priority", &priority, true, -1);

	rc = dynsec_groups__add_client(username, groupname, priority, true);
	if(rc == MOSQ_ERR_SUCCESS){
		dynsec__command_reply(j_responses, context, "addGroupClient", NULL, correlation_data);
	}else if(rc == ERR_USER_NOT_FOUND){
		dynsec__command_reply(j_responses, context, "addGroupClient", "Client not found", correlation_data);
	}else if(rc == ERR_GROUP_NOT_FOUND){
		dynsec__command_reply(j_responses, context, "addGroupClient", "Group not found", correlation_data);
	}else{
		dynsec__command_reply(j_responses, context, "addGroupClient", "Internal error", correlation_data);
	}

	/* Enforce any changes */
	mosquitto_kick_client_by_username(username, false);

	return rc;
}


static void dynsec_clientlists__remove(struct dynsec__clientlist **base_clientlist, const char *username)
{
	struct dynsec__clientlist *clientlist;

	HASH_FIND(hh, *base_clientlist, username, strlen(username), clientlist);
	if(clientlist){
		HASH_DELETE(hh, *base_clientlist, clientlist);
		mosquitto_free(clientlist);
	}
}

static void dynsec_grouplists__remove(struct dynsec__grouplist **base_grouplist, const char *groupname)
{
	struct dynsec__grouplist *grouplist;

	HASH_FIND(hh, *base_grouplist, groupname, strlen(groupname), grouplist);
	if(grouplist){
		HASH_DELETE(hh, *base_grouplist, grouplist);
		mosquitto_free(grouplist);
	}
}

static int dynsec__remove_all_clients_from_group(struct dynsec__group *group)
{
	struct dynsec__clientlist *clientlist, *clientlist_tmp;

	HASH_ITER(hh, group->clientlist, clientlist, clientlist_tmp){
		/* Remove client stored group reference */
		dynsec_grouplists__remove(&clientlist->client->grouplist, group->groupname);

		HASH_DELETE(hh, group->clientlist, clientlist);
		mosquitto_free(clientlist);
	}

	return MOSQ_ERR_SUCCESS;
}

int dynsec_groups__remove_client(const char *username, const char *groupname, bool update_config)
{
	struct dynsec__client *client;
	struct dynsec__group *group;

	client = dynsec_clients__find(username);
	if(client == NULL){
		return ERR_USER_NOT_FOUND;
	}

	group = dynsec_groups__find(groupname);
	if(group == NULL){
		return ERR_GROUP_NOT_FOUND;
	}

	dynsec_clientlists__remove(&group->clientlist, username);
	dynsec_grouplists__remove(&client->grouplist, groupname);

	if(update_config){
		dynsec__config_save();
	}
	return MOSQ_ERR_SUCCESS;
}

int dynsec_groups__process_remove_client(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
	char *username, *groupname;
	int rc;

	if(json_get_string(command, "username", &username, false) != MOSQ_ERR_SUCCESS){
		dynsec__command_reply(j_responses, context, "removeGroupClient", "Invalid/missing username", correlation_data);
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(username, (int)strlen(username)) != MOSQ_ERR_SUCCESS){
		dynsec__command_reply(j_responses, context, "removeGroupClient", "Username not valid UTF-8", correlation_data);
		return MOSQ_ERR_INVAL;
	}

	if(json_get_string(command, "groupname", &groupname, false) != MOSQ_ERR_SUCCESS){
		dynsec__command_reply(j_responses, context, "removeGroupClient", "Invalid/missing groupname", correlation_data);
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(groupname, (int)strlen(groupname)) != MOSQ_ERR_SUCCESS){
		dynsec__command_reply(j_responses, context, "removeGroupClient", "Group name not valid UTF-8", correlation_data);
		return MOSQ_ERR_INVAL;
	}

	rc = dynsec_groups__remove_client(username, groupname, true);
	if(rc == MOSQ_ERR_SUCCESS){
		dynsec__command_reply(j_responses, context, "removeGroupClient", NULL, correlation_data);
	}else if(rc == ERR_USER_NOT_FOUND){
		dynsec__command_reply(j_responses, context, "removeGroupClient", "Client not found", correlation_data);
	}else if(rc == ERR_GROUP_NOT_FOUND){
		dynsec__command_reply(j_responses, context, "removeGroupClient", "Group not found", correlation_data);
	}else{
		dynsec__command_reply(j_responses, context, "removeGroupClient", "Internal error", correlation_data);
	}

	/* Enforce any changes */
	mosquitto_kick_client_by_username(username, false);

	return rc;
}


static cJSON *add_group_to_json(struct dynsec__group *group)
{
	cJSON *j_group, *jtmp, *j_clientlist, *j_client, *j_rolelist;
	struct dynsec__clientlist *clientlist, *clientlist_tmp;

	j_group = cJSON_CreateObject();
	if(j_group == NULL){
		return NULL;
	}

	if(cJSON_AddStringToObject(j_group, "groupname", group->groupname) == NULL
			|| (group->text_name && cJSON_AddStringToObject(j_group, "textname", group->text_name) == NULL)
			|| (group->text_description && cJSON_AddStringToObject(j_group, "textdescription", group->text_description) == NULL)
			|| (j_clientlist = cJSON_AddArrayToObject(j_group, "clients")) == NULL
			){

		cJSON_Delete(j_group);
		return NULL;
	}

	HASH_ITER(hh, group->clientlist, clientlist, clientlist_tmp){
		j_client = cJSON_CreateObject();
		if(j_client == NULL){
			cJSON_Delete(j_group);
			return NULL;
		}
		cJSON_AddItemToArray(j_clientlist, j_client);

		jtmp = cJSON_CreateStringReference(clientlist->username);
		if(jtmp == NULL){
			cJSON_Delete(j_group);
			return NULL;
		}
		cJSON_AddItemToObject(j_client, "username", jtmp);
	}

	j_rolelist = dynsec_rolelists__all_to_json(group->rolelist);
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
	cJSON *tree, *j_groups, *j_group, *jtmp, *j_data;
	struct dynsec__group *group, *group_tmp;
	int i, count, offset;

	json_get_bool(command, "verbose", &verbose, true, false);
	json_get_int(command, "count", &count, true, -1);
	json_get_int(command, "offset", &offset, true, 0);

	tree = cJSON_CreateObject();
	if(tree == NULL){
		dynsec__command_reply(j_responses, context, "listGroups", "Internal error", correlation_data);
		return MOSQ_ERR_NOMEM;
	}

	jtmp = cJSON_CreateString("listGroups");
	if(jtmp == NULL){
		cJSON_Delete(tree);
		dynsec__command_reply(j_responses, context, "listGroups", "Internal error", correlation_data);
		return MOSQ_ERR_NOMEM;
	}
	cJSON_AddItemToObject(tree, "command", jtmp);

	j_data = cJSON_CreateObject();
	if(j_data == NULL){
		cJSON_Delete(tree);
		dynsec__command_reply(j_responses, context, "listGroups", "Internal error", correlation_data);
		return MOSQ_ERR_NOMEM;
	}
	cJSON_AddItemToObject(tree, "data", j_data);

	cJSON_AddIntToObject(j_data, "totalCount", (int)HASH_CNT(hh, local_groups));

	j_groups = cJSON_CreateArray();
	if(j_groups == NULL){
		cJSON_Delete(tree);
		dynsec__command_reply(j_responses, context, "listGroups", "Internal error", correlation_data);
		return MOSQ_ERR_NOMEM;
	}
	cJSON_AddItemToObject(j_data, "groups", j_groups);

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
	if(correlation_data){
		jtmp = cJSON_CreateString(correlation_data);
		if(jtmp == NULL){
			cJSON_Delete(tree);
			dynsec__command_reply(j_responses, context, "listGroups", "Internal error", correlation_data);
			return 1;
		}
		cJSON_AddItemToObject(tree, "correlationData", jtmp);
	}

	cJSON_AddItemToArray(j_responses, tree);

	return MOSQ_ERR_SUCCESS;
}


int dynsec_groups__process_get(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
	char *groupname;
	cJSON *tree, *j_group, *jtmp, *j_data;
	struct dynsec__group *group;

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

	jtmp = cJSON_CreateString("getGroup");
	if(jtmp == NULL){
		cJSON_Delete(tree);
		dynsec__command_reply(j_responses, context, "getGroup", "Internal error", correlation_data);
		return MOSQ_ERR_NOMEM;
	}
	cJSON_AddItemToObject(tree, "command", jtmp);

	j_data = cJSON_CreateObject();
	if(j_data == NULL){
		cJSON_Delete(tree);
		dynsec__command_reply(j_responses, context, "getGroup", "Internal error", correlation_data);
		return MOSQ_ERR_NOMEM;
	}
	cJSON_AddItemToObject(tree, "data", j_data);

	group = dynsec_groups__find(groupname);
	if(group){
		j_group = add_group_to_json(group);
		if(j_group == NULL){
			cJSON_Delete(tree);
			dynsec__command_reply(j_responses, context, "getGroup", "Internal error", correlation_data);
			return MOSQ_ERR_NOMEM;
		}
		cJSON_AddItemToObject(j_data, "group", j_group);
	}
	if(correlation_data){
		jtmp = cJSON_CreateString(correlation_data);
		if(jtmp == NULL){
			dynsec__command_reply(j_responses, context, "getGroup", "Internal error", correlation_data);
			cJSON_Delete(tree);
			return 1;
		}
		cJSON_AddItemToObject(tree, "correlationData", jtmp);
	}

	cJSON_AddItemToArray(j_responses, tree);

	return MOSQ_ERR_SUCCESS;
}


int dynsec_groups__process_remove_role(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
	char *groupname, *rolename;
	struct dynsec__group *group;
	struct dynsec__role *role;

	if(json_get_string(command, "groupname", &groupname, false) != MOSQ_ERR_SUCCESS){
		dynsec__command_reply(j_responses, context, "removeGroupRole", "Invalid/missing groupname", correlation_data);
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(groupname, (int)strlen(groupname)) != MOSQ_ERR_SUCCESS){
		dynsec__command_reply(j_responses, context, "removeGroupRole", "Group name not valid UTF-8", correlation_data);
		return MOSQ_ERR_INVAL;
	}

	if(json_get_string(command, "roleName", &rolename, false) != MOSQ_ERR_SUCCESS){
		dynsec__command_reply(j_responses, context, "removeGroupRole", "Invalid/missing roleName", correlation_data);
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

	dynsec_rolelists__group_remove_role(group, role);
	dynsec__config_save();
	dynsec__command_reply(j_responses, context, "removeGroupRole", NULL, correlation_data);

	/* Enforce any changes */
	if(group == dynsec_anonymous_group){
		mosquitto_kick_client_by_username(NULL, false);
	}
	dynsec_clientlist__kick_all(group->clientlist);
	return MOSQ_ERR_SUCCESS;
}


int dynsec_groups__process_modify(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
	char *groupname;
	char *text_name, *text_description;
	struct dynsec__group *group;
	struct dynsec__rolelist *rolelist = NULL;
	char *str;
	int rc;
	int priority;
	cJSON *j_client, *j_clients, *jtmp;

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
		dynsec__command_reply(j_responses, context, "modifyGroup", "Group does not exist", correlation_data);
		return MOSQ_ERR_INVAL;
	}

	if(json_get_string(command, "textname", &text_name, true) == MOSQ_ERR_SUCCESS){
		str = mosquitto_strdup(text_name);
		if(str == NULL){
			dynsec__command_reply(j_responses, context, "modifyGroup", "Internal error", correlation_data);
			return MOSQ_ERR_NOMEM;
		}
		mosquitto_free(group->text_name);
		group->text_name = str;
	}

	if(json_get_string(command, "textdescription", &text_description, true) == MOSQ_ERR_SUCCESS){
		str = mosquitto_strdup(text_description);
		if(str == NULL){
			dynsec__command_reply(j_responses, context, "modifyGroup", "Internal error", correlation_data);
			return MOSQ_ERR_NOMEM;
		}
		mosquitto_free(group->text_description);
		group->text_description = str;
	}

	rc = dynsec_rolelists__load_from_json(command, &rolelist);
	if(rc == MOSQ_ERR_SUCCESS){
		dynsec_rolelists__free_all(&group->rolelist);
		group->rolelist = rolelist;
	}else if(rc == MOSQ_ERR_NOT_FOUND){
		dynsec__command_reply(j_responses, context, "modifyGroup", "Role not found", correlation_data);
		dynsec_rolelists__free_all(&rolelist);
		return MOSQ_ERR_INVAL;
	}else if(rc == ERR_LIST_NOT_FOUND){
		/* There was no list in the JSON, so no modification */
	}else{
		dynsec__command_reply(j_responses, context, "modifyGroup", "Internal error", correlation_data);
		dynsec_rolelists__free_all(&rolelist);
		return MOSQ_ERR_INVAL;
	}

	j_clients = cJSON_GetObjectItem(command, "clients");
	if(j_clients && cJSON_IsArray(j_clients)){
		dynsec__remove_all_clients_from_group(group);

		cJSON_ArrayForEach(j_client, j_clients){
			if(cJSON_IsObject(j_client)){
				jtmp = cJSON_GetObjectItem(j_client, "username");
				if(jtmp && cJSON_IsString(jtmp)){
					json_get_int(j_client, "priority", &priority, true, -1);
					dynsec_groups__add_client(jtmp->valuestring, groupname, priority, false);
				}
			}
		}
	}

	dynsec__config_save();

	dynsec__command_reply(j_responses, context, "modifyGroup", NULL, correlation_data);

	/* Enforce any changes */
	if(group == dynsec_anonymous_group){
		mosquitto_kick_client_by_username(NULL, false);
	}
	dynsec_clientlist__kick_all(group->clientlist);
	return MOSQ_ERR_SUCCESS;
}


int dynsec_groups__process_set_anonymous_group(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
	char *groupname;
	struct dynsec__group *group = NULL;

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

	return MOSQ_ERR_SUCCESS;
}

int dynsec_groups__process_get_anonymous_group(cJSON *j_responses, struct mosquitto *context, cJSON *command, char *correlation_data)
{
	cJSON *tree, *jtmp, *j_data, *j_group;

	tree = cJSON_CreateObject();
	if(tree == NULL){
		dynsec__command_reply(j_responses, context, "getAnonymousGroup", "Internal error", correlation_data);
		return MOSQ_ERR_NOMEM;
	}

	jtmp = cJSON_CreateString("getAnonymousGroup");
	if(jtmp == NULL){
		cJSON_Delete(tree);
		dynsec__command_reply(j_responses, context, "getAnonymousGroup", "Internal error", correlation_data);
		return MOSQ_ERR_NOMEM;
	}
	cJSON_AddItemToObject(tree, "command", jtmp);

	j_data = cJSON_CreateObject();
	if(j_data == NULL){
		cJSON_Delete(tree);
		dynsec__command_reply(j_responses, context, "getAnonymousGroup", "Internal error", correlation_data);
		return MOSQ_ERR_NOMEM;
	}
	cJSON_AddItemToObject(tree, "data", j_data);

	j_group = cJSON_CreateObject();
	if(j_group == NULL){
		cJSON_Delete(tree);
		dynsec__command_reply(j_responses, context, "getAnonymousGroup", "Internal error", correlation_data);
		return MOSQ_ERR_NOMEM;
	}
	cJSON_AddItemToObject(j_data, "group", j_group);

	if(cJSON_AddStringToObject(j_group, "groupname", dynsec_anonymous_group->groupname) == NULL
			){

		cJSON_Delete(tree);
		dynsec__command_reply(j_responses, context, "getAnonymousGroup", "Internal error", correlation_data);
		return MOSQ_ERR_NOMEM;
	}

	cJSON_AddItemToArray(j_responses, tree);

	return MOSQ_ERR_SUCCESS;
}
