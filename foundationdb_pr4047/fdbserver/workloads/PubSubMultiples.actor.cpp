/*
 * PubSubMultiples.actor.cpp
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
#include "fdbserver/pubsub.h"
#include "fdbserver/TesterInterface.actor.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "flow/actorcompiler.h"  // This must be the last #include.

struct PubSubMultiplesWorkload : TestWorkload {
	double testDuration, messagesPerSecond;
	int actorCount, inboxesPerActor;

	vector<Future<Void>> inboxWatchers;
	PerfIntCounter messages;

	PubSubMultiplesWorkload(WorkloadContext const& wcx)
		: TestWorkload(wcx),
		messages("Messages")
	{
		testDuration = getOption( options, LiteralStringRef("testDuration"), 10.0 );
		messagesPerSecond = getOption( options, LiteralStringRef("messagesPerSecond"), 500.0 ) / clientCount;
		actorCount = getOption( options, LiteralStringRef("actorsPerClient"), 20 );
		inboxesPerActor = getOption( options, LiteralStringRef("inboxesPerActor"), 20 );
	}

	virtual std::string description() { return "PubSubMultiplesWorkload"; }
	virtual Future<Void> setup( Database const& cx ) { 
		return createNodes( this, cx );
	}
	virtual Future<Void> start( Database const& cx ) {
		Future<Void> _ = startTests( this, cx );
		return delay(testDuration);
	}
	virtual Future<bool> check( Database const& cx ) {
		return true;
	}

	virtual void getMetrics( vector<PerfMetric>& m ) {
		m.push_back( messages.getMetric() );
	}

	Key keyForFeed( int i ) { return StringRef( format( "/PSM/feeds/%d", i ) ); }
	Key keyForInbox( int i ) { return StringRef( format( "/PSM/inbox/%d", i ) ); }
	Value valueForUInt( uint64_t i ) { return StringRef( format( "%llx", i ) ); }

	ACTOR Future<Void> createNodeSwath( PubSubMultiplesWorkload *self, int actor, Database cx ) {
		state PubSub ps(cx);
		state vector<uint64_t> feeds;
		state vector<uint64_t> inboxes;
		state int idx;
		for(idx = 0; idx < self->inboxesPerActor; idx++) {
			uint64_t feedIdx = wait( ps.createFeed( StringRef() ) );
			feeds.push_back( feedIdx );
			uint64_t inboxIdx = wait( ps.createInbox( StringRef() ) );
			inboxes.push_back( inboxIdx );
		}
		state Transaction tr(cx);
		loop {
			try {
				for(int idx = 0; idx < self->inboxesPerActor; idx++) {
					int offset = ( self->clientId * self->clientCount * self->actorCount * self->inboxesPerActor ) 
								+ ( actor * self->actorCount * self->inboxesPerActor ) + idx;
					tr.set( self->keyForFeed( offset ), self->valueForUInt( feeds[idx] ) );
					tr.set( self->keyForInbox( offset ), self->valueForUInt( inboxes[idx] ) );
				}
				wait( tr.commit() );
				break;
			} catch(Error& e) {
				wait( tr.onError(e) );
			}
		}
		return Void();
	}

	ACTOR Future<Void> createNodes( PubSubMultiplesWorkload *self, Database cx ) {
		state PubSub ps(cx);
		vector<Future<Void>> actors;
		for(int i=0; i<self->actorCount; i++)
			actors.push_back( self->createNodeSwath( self, i, cx->clone() ) );
		wait( waitForAll( actors ) );
		TraceEvent("PSMNodesCreated").detail("ClientIdx", self->clientId);
		return Void();
	}

	/*ACTOR*/ Future<Void> createSubscriptions( PubSubMultiplesWorkload *self, int actor, Database cx ) {
		// create the "multiples" subscriptions for each owned inbox
		return Void();
	}

	/*ACTOR*/ Future<Void> messageSender( PubSubMultiplesWorkload *self, Database cx ) {
		// use a possion loop and post messages to feeds
		return Void();
	}

	ACTOR Future<Void> startTests( PubSubMultiplesWorkload *self, Database cx ) {
		vector<Future<Void>> subscribers;
		for(int i=0; i<self->actorCount; i++)
			subscribers.push_back( self->createSubscriptions( self, i, cx ) );
		wait( waitForAll( subscribers ) );

		state Future<Void> sender = self->messageSender( self, cx );
		return Void();
	}
};

WorkloadFactory<PubSubMultiplesWorkload> PubSubMultiplesWorkloadFactory("PubSubMultiples");
