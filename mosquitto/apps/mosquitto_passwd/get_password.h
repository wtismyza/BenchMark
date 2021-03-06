#ifndef GET_PASSWORD_H
#define GET_PASSWORD_H
/*
Copyright (c) 2012-2020 Roger Light <roger@atchoo.org>

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

#include <stdbool.h>

void get_password__reset_term(void);
int get_password(const char *prompt, const char *verify_prompt, bool quiet, char *password, size_t len);

#endif
