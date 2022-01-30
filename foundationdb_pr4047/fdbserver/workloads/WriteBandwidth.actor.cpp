/*
 * WriteBandwidth.actor.cpp
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

#include <boost/lexical_cast.hpp>

#include "fdbrpc/ContinuousSample.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbserver/TesterInterface.actor.h"
#include "fdbserver/WorkerInterface.actor.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "fdbserver/workloads/BulkSetup.actor.h"
#include "flow/actorcompiler.h"  // This must be the last #include.

struct WriteBandwidthWorkload : KVWorkload {
	int keysPerTransaction;
	double testDuration, warmingDelay, loadTime, maxInsertRate;
	std::string valueString;

	std::vector<Future<Void>> clients;
	PerfIntCounter transactions, retries;
	ContinuousSample<double> commitLatencies, GRVLatencies;

	WriteBandwidthWorkload(WorkloadContext const& wcx)
		: KVWorkload(wcx),
		commitLatencies( 2000 ), GRVLatencies( 2000 ),
		loadTime( 0.0 ), transactions("Transactions"), retries("Retries")
	{
		testDuration = getOption( options, LiteralStringRef("testDuration"), 10.0 );
		keysPerTransaction = getOption( options, LiteralStringRef("keysPerTransaction"), 100 );
		valueString = std::string( maxValueBytes, '.' );

		warmingDelay = getOption( options, LiteralStringRef("warmingDelay"), 0.0 );
		maxInsertRate = getOption( options, LiteralStringRef("maxInsertRate"), 1e12 );
	}
		
	virtual std::string description() { return "WriteBandwidth"; }
	virtual Future<Void> setup( Database const& cx ) { return _setup( cx, this ); }
	virtual Future<Void> start( Database const& cx ) { return _start( cx, this ); }

	virtual Future<bool> check( Database const& cx ) { return true; }

	virtual void getMetrics( std::vector<PerfMetric>& m ) {
		double duration = testDuration;
		int writes = transactions.getValue() * keysPerTransaction;
		m.emplace_back("Measured Duration", duration, true);
		m.emplace_back("Transactions/sec", transactions.getValue() / duration, false);
		m.emplace_back("Operations/sec", writes / duration, false);
		m.push_back(transactions.getMetric());
		m.push_back(retries.getMetric());
		m.emplace_back("Mean load time (seconds)", loadTime, true);
		m.emplace_back("Write rows", writes, false);

		m.emplace_back("Mean GRV Latency (ms)", 1000 * GRVLatencies.mean(), true);
		m.emplace_back("Median GRV Latency (ms, averaged)", 1000 * GRVLatencies.median(), true);
		m.emplace_back("90% GRV Latency (ms, averaged)", 1000 * GRVLatencies.percentile( 0.90 ), true);
		m.emplace_back("98% GRV Latency (ms, averaged)", 1000 * GRVLatencies.percentile( 0.98 ), true);

		m.emplace_back("Mean Commit Latency (ms)", 1000 * commitLatencies.mean(), true);
		m.emplace_back("Median Commit Latency (ms, averaged)", 1000 * commitLatencies.median(), true);
		m.emplace_back("90% Commit Latency (ms, averaged)", 1000 * commitLatencies.percentile( 0.90 ), true);
		m.emplace_back("98% Commit Latency (ms, averaged)", 1000 * commitLatencies.percentile( 0.98 ), true);

		m.emplace_back("Write rows/sec", writes / duration, false);
		m.emplace_back("Bytes written/sec", (writes * (keyBytes + (minValueBytes+maxValueBytes)*0.5)) / duration, false);
	}

	Value randomValue() { return StringRef( (uint8_t*)valueString.c_str(), deterministicRandom()->randomInt(minValueBytes, maxValueBytes+1) );	}

	Standalone<KeyValueRef> operator()( uint64_t n ) {
		return KeyValueRef( keyForIndex( n, false ), randomValue() );
	}

	ACTOR Future<Void> _setup( Database cx, WriteBandwidthWorkload *self ) {
		state Promise<double> loadTime;
		state Promise<std::vector<std::pair<uint64_t, double> > > ratesAtKeyCounts;

		wait( bulkSetup( cx, self, self->nodeCount, loadTime, true, self->warmingDelay, self->maxInsertRate ) );
		self->loadTime = loadTime.getFuture().get();
		return Void();
	}

	ACTOR Future<Void> _start( Database cx, WriteBandwidthWorkload *self ) {
		for( int i = 0; i < self->actorCount; i++ ) {
			self->clients.push_back( self->writeClient( cx, self ) );
		}

		wait( timeout( waitForAll( self->clients ), self->testDuration, Void() ) );
		self->clients.clear();
		return Void();
	}

	ACTOR Future<Void> writeClient( Database cx, WriteBandwidthWorkload *self ) {
		loop {
			state Transaction tr( cx );
			state uint64_t startIdx = deterministicRandom()->random01() * (self->nodeCount - self->keysPerTransaction);
			loop {
				try {
					state double start = now();
					wait(success(tr.getReadVersion()));
					self->GRVLatencies.addSample( now() - start );

					// Predefine a single large write conflict range over the whole key space
					tr.addWriteConflictRange( KeyRangeRef( 
							self->keyForIndex( startIdx, false ), 
							keyAfter( self->keyForIndex( startIdx + self->keysPerTransaction - 1, false ) ) ) );

					for( int i = 0; i < self->keysPerTransaction; i++ )
						tr.set( self->keyForIndex( startIdx + i, false ), self->randomValue(), false );

					start = now();
					wait( tr.commit() );
					self->commitLatencies.addSample( now() - start );
					break;
				} catch( Error& e ) {
					wait( tr.onError( e ) );
					++self->retries;
				}
			}
			++self->transactions;
		}
	}
};

WorkloadFactory<WriteBandwidthWorkload> WriteBandwidthWorkloadFactory("WriteBandwidth");
