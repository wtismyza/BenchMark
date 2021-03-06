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

#include "mosquitto_ctrl.h"

void ctrl_help(void)
{
	printf("\nExample module\n");
	printf("==============\n");
	printf("    mosquitto_ctrl example help\n");
}

int ctrl_main(int argc, char *argv[], struct mosq_ctrl *ctrl)
{
	if(!strcasecmp(argv[0], "help")){
		ctrl_help();
		return -1;
	}else{
		return MOSQ_ERR_INVAL;
	}
}
