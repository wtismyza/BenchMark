/*
 * Schemas.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FDBCLIENT_SCHEMAS_H
#define FDBCLIENT_SCHEMAS_H

#pragma once

#include "flow/flow.h"
#include "fdbclient/FDBTypes.h"

struct JSONSchemas {
	static const KeyRef statusSchema;
	static const KeyRef clusterConfigurationSchema;
	static const KeyRef latencyBandConfigurationSchema;
};

#endif /* FDBCLIENT_SCHEMAS_H */
