/*
 * DDMetrics.actor.cpp
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

#include "fdbclient/NativeAPI.actor.h"
#include "fdbserver/TesterInterface.actor.h"
#include "fdbserver/Status.h"
#include "fdbserver/QuietDatabase.h"
#include "fdbserver/ServerDBInfo.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "flow/actorcompiler.h"  // This must be the last #include.

struct DDMetricsWorkload : TestWorkload {
	double startDelay, ddDone;

	DDMetricsWorkload(WorkloadContext const& wcx)
		: TestWorkload(wcx), ddDone( 0.0 )
	{
		startDelay = getOption( options, LiteralStringRef("beginPoll"), 10.0 );
	}

	virtual std::string description() { return "Data Distribution Metrics"; }

	ACTOR Future<int> getHighPriorityRelocationsInFlight( Database cx, DDMetricsWorkload *self ) {
		WorkerInterface masterWorker = wait(getMasterWorker(cx, self->dbInfo));

		TraceEvent("GetHighPriorityReliocationsInFlight").detail("Stage", "ContactingMaster");
		TraceEventFields md = wait( timeoutError(masterWorker.eventLogRequest.getReply(
			EventLogRequest( LiteralStringRef( "MovingData" ) ) ), 1.0 ) );
		int relocations;
		sscanf(md.getValue("UnhealthyRelocations").c_str(), "%d", &relocations);
		return relocations;
	}

	ACTOR Future<Void> work( Database cx, DDMetricsWorkload *self ) {
		try {
			TraceEvent("DDMetricsWaiting").detail("StartDelay", self->startDelay);
			wait( delay( self->startDelay ) );
			TraceEvent("DDMetricsStarting");
			state double startTime = now();
			loop {
				wait( delay( 2.5 ) );
				int dif = wait( self->getHighPriorityRelocationsInFlight( cx, self ) );
				TraceEvent("DDMetricsCheck").detail("DIF", dif);
				if( dif == 0 ) {
					self->ddDone = now() - startTime;
					return Void();
				}
			}
		} catch( Error& e ) {
			TraceEvent("DDMetricsError").error(e);
		}
		return Void();
	}

	virtual Future<Void> start( Database const& cx ) {
		return clientId == 0 ? work( cx, this ) : Void();
	}

	virtual Future<bool> check( Database const& cx ) {
		return true;
	}

	virtual void getMetrics( vector<PerfMetric>& m ) {
		m.push_back( PerfMetric( "DDDuration", ddDone, false ) );
	}

};

WorkloadFactory<DDMetricsWorkload> DDMetricsWorkloadFactory("DDMetrics");
