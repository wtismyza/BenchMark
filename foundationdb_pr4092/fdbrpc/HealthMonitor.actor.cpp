/*
 * HealthMonitor.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2020 Apple Inc. and the FoundationDB project authors
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

#include "fdbrpc/FailureMonitor.h"
#include "fdbrpc/FlowTransport.h"
#include "fdbrpc/HealthMonitor.h"

void HealthMonitor::reportPeerClosed(const NetworkAddress& peerAddress) {
	purgeOutdatedHistory();
	peerClosedHistory.push_back(std::make_pair(now(), peerAddress));
	peerClosedNum[peerAddress] += 1;
}

void HealthMonitor::purgeOutdatedHistory() {
	for (auto it : peerClosedHistory) {
		if (it.first < now() - FLOW_KNOBS->HEALTH_MONITOR_CLIENT_REQUEST_INTERVAL_SECS) {
			peerClosedNum[it.second] -= 1;
			ASSERT(peerClosedNum[it.second] >= 0);
			peerClosedHistory.pop_front();
		} else {
			break;
		}
	}
}

bool HealthMonitor::tooManyConnectionsClosed(const NetworkAddress& peerAddress) {
	purgeOutdatedHistory();
	return peerClosedNum[peerAddress] > FLOW_KNOBS->HEALTH_MONITOR_CONNECTION_MAX_CLOSED;
}

int HealthMonitor::closedConnectionsCount(const NetworkAddress& peerAddress) {
	purgeOutdatedHistory();
	return peerClosedNum[peerAddress];
}
