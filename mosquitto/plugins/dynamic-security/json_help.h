#ifndef JSON_HELP_H
#define JSON_HELP_H
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
#include <stdbool.h>

int json_get_bool(cJSON *json, const char *name, bool *value, bool optional, bool default_value);
int json_get_int(cJSON *json, const char *name, int *value, bool optional, int default_value);
int json_get_string(cJSON *json, const char *name, char **value, bool optional);
double json_get_as_number(const cJSON *json);

cJSON *cJSON_AddIntToObject(cJSON * const object, const char * const name, int number);
cJSON *cJSON_CreateInt(int num);

#endif
