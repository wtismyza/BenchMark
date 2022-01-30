/*
 * Throughput.actor.cpp
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

#include "fdbrpc/ContinuousSample.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbserver/TesterInterface.actor.h"
#include "fdbserver/WorkerInterface.actor.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "flow/ActorCollection.h"
#include "fdbrpc/Smoother.h"
#include "flow/actorcompiler.h"  // This must be the last #include.

struct ITransactor : ReferenceCounted<ITransactor> {
	struct Stats {
		int64_t reads, writes, retries, transactions;
		double totalLatency, grvLatency, rowReadLatency, commitLatency;
		Stats() : reads(0), writes(0), retries(0), transactions(0), totalLatency(0), grvLatency(0), rowReadLatency(0), commitLatency(0) {}
		void operator += (Stats const& s) { 
			reads += s.reads; writes += s.writes; retries += s.retries; transactions += s.transactions; 
			totalLatency += s.totalLatency; grvLatency += s.grvLatency; rowReadLatency += s.rowReadLatency; commitLatency += s.commitLatency;
		}
	};

	virtual Future<Void> doTransaction(Database const&, Stats* stats) = 0;
	virtual ~ITransactor() {}
};

struct RWTransactor : ITransactor {
	int reads, writes;
	int minValueBytes, maxValueBytes;
	std::string valueString;
	int keyCount, keyBytes;
	
	RWTransactor( int reads, int writes, int keyCount, int keyBytes, int minValueBytes, int maxValueBytes )
		: reads(reads), writes(writes), keyCount(keyCount), keyBytes(keyBytes),
		  minValueBytes(minValueBytes), maxValueBytes(maxValueBytes)
	{
		ASSERT(minValueBytes <= maxValueBytes);
		valueString = std::string( maxValueBytes, '.' );
	}

	Key randomKey() {
		Key result = makeString( keyBytes );
		uint8_t* data = mutateString( result );
		memset(data, '.', keyBytes);

		double d = double(deterministicRandom()->randomInt(0, keyCount)) / keyCount;
		emplaceIndex( data, 0, *(int64_t*)&d );

		return result;
	}

	Value randomValue() { return StringRef( (const uint8_t*)valueString.c_str(), deterministicRandom()->randomInt(minValueBytes, maxValueBytes+1) ); };

	virtual Future<Void> doTransaction(Database const& db, Stats* stats) {
		return rwTransaction(db, Reference<RWTransactor>::addRef(this), stats );
	}

	ACTOR static Future<Optional<Value>> getLatency( Future<Optional<Value>> f, double* t ) {
		Optional<Value> v = wait(f);
		*t += now();
		return v;
	}

	ACTOR static Future<Void> rwTransaction( Database db, Reference<RWTransactor> self, Stats* stats ) {
		state vector<Key> keys;
		state vector<Value> values;
		state Transaction tr(db);

		for(int op = 0; op < self->reads || op < self->writes; op++ )
			keys.push_back( self->randomKey() );
		for(int op = 0; op < self->writes; op++ )
			values.push_back( self->randomValue() );

		loop {
			try {
				state double t_start = now();
				wait(success( tr.getReadVersion() ));
				state double t_rv = now();
				state double rrLatency = -t_rv * self->reads;

				state vector<Future<Optional<Value>>> reads;
				for(int i=0; i<self->reads; i++)
					reads.push_back( getLatency( tr.get( keys[i] ), &rrLatency ) );
				wait( waitForAll(reads) );
				for(int i=0; i<self->writes; i++)
					tr.set( keys[i], values[i] );
				state double t_beforeCommit = now();
				wait( tr.commit() );

				stats->transactions++;
				stats->reads += self->reads;
				stats->writes += self->writes;
				stats->grvLatency += t_rv - t_start;
				stats->commitLatency += now() - t_beforeCommit;
				stats->rowReadLatency += rrLatency / self->reads;
				break;
			} catch (Error& e) {
				wait( tr.onError(e) );
				stats->retries++;
			}
		}

		return Void();
	}
};

struct ABTransactor : ITransactor {
	Reference<ITransactor> a, b;
	double alpha;  // 0.0 = all a, 1.0 = all b
	
	ABTransactor( double alpha, Reference<ITransactor> a, Reference<ITransactor> b )
		: alpha(alpha), a(a), b(b)
	{
	}

	virtual Future<Void> doTransaction(Database const& db, Stats* stats) {
		return deterministicRandom()->random01() >= alpha ? a->doTransaction(db,stats) : b->doTransaction(db,stats);
	}
};

struct SweepTransactor : ITransactor {
	// Runs a linearly-changing workload that changes from A-type to B-type over 
	//    the specified duration--the timer starts at the first transaction.
	Reference<ITransactor> a, b;
	double startTime;
	double startDelay;
	double duration;

	SweepTransactor( double duration, double startDelay, Reference<ITransactor> a, Reference<ITransactor> b )
		: a(a), b(b), duration(duration), startTime(-1), startDelay(startDelay)
	{
	}

	virtual Future<Void> doTransaction(Database const& db, Stats* stats) {
		if (startTime==-1) startTime = now()+startDelay;
		
		double alpha;
		double n = now();
		if (n < startTime) alpha = 0;
		else if (n > startTime+duration) alpha = 1;
		else alpha = (n-startTime) / duration;

		return deterministicRandom()->random01() >= alpha ? a->doTransaction(db,stats) : b->doTransaction(db,stats);
	}
};

struct IMeasurer : ReferenceCounted<IMeasurer> {
	// This could be an ITransactor, but then it needs an actor to wait for the transaction to actually finish
	virtual Future<Void> start() { return Void(); }
	virtual void addTransaction(ITransactor::Stats* stats, double now) = 0;
	virtual void getMetrics( vector<PerfMetric>& m ) = 0;
	IMeasurer& operator=(IMeasurer const&) {return *this;} // allow copy operator for non-reference counted instances of subclasses

	virtual ~IMeasurer() {}
};

struct MeasureSinglePeriod : IMeasurer {
	double delay, duration;
	double startT;
	
	ContinuousSample<double> totalLatency, grvLatency, rowReadLatency, commitLatency;
	ITransactor::Stats stats;  // totalled over the period

	MeasureSinglePeriod( double delay, double duration ) : delay(delay), duration(duration), totalLatency(2000), grvLatency(2000), rowReadLatency(2000), commitLatency(2000) {}

	virtual Future<Void> start() { startT = now(); return Void(); }
	virtual void addTransaction(ITransactor::Stats* st, double now) {
		if (!(now >= startT+delay && now < startT+delay+duration)) return;

		totalLatency.addSample( st->totalLatency );
		grvLatency.addSample( st->grvLatency );
		rowReadLatency.addSample( st->rowReadLatency );

		if(st->commitLatency > 0) {
			commitLatency.addSample( st->commitLatency );
		}

		stats += *st;
	}
	virtual void getMetrics( vector<PerfMetric>& m ) {
		double measureDuration = duration;
		m.push_back( PerfMetric( "Transactions/sec", stats.transactions / measureDuration, false ) );
		m.push_back( PerfMetric( "Retries/sec", stats.retries / measureDuration, false ) );
		m.push_back( PerfMetric( "Operations/sec", (stats.reads + stats.writes) / measureDuration, false ) );
		m.push_back( PerfMetric( "Read rows/sec", stats.reads / measureDuration, false ) );
		m.push_back( PerfMetric( "Write rows/sec", stats.writes / measureDuration, false ) );

		m.push_back( PerfMetric( "Mean Latency (ms)", 1000 * totalLatency.mean(), true ) );
		m.push_back( PerfMetric( "Median Latency (ms, averaged)", 1000 * totalLatency.median(), true ) );
		m.push_back( PerfMetric( "90% Latency (ms, averaged)", 1000 * totalLatency.percentile( 0.90 ), true ) );
		m.push_back( PerfMetric( "98% Latency (ms, averaged)", 1000 * totalLatency.percentile( 0.98 ), true ) );

		m.push_back( PerfMetric( "Mean Row Read Latency (ms)", 1000 * rowReadLatency.mean(), true ) );
		m.push_back( PerfMetric( "Median Row Read Latency (ms, averaged)", 1000 * rowReadLatency.median(), true ) );
		m.push_back( PerfMetric( "Mean GRV Latency (ms)", 1000 * grvLatency.mean(), true ) );
		m.push_back( PerfMetric( "Median GRV Latency (ms, averaged)", 1000 * grvLatency.median(), true ) );
		m.push_back( PerfMetric( "Mean Commit Latency (ms)", 1000 * commitLatency.mean(), true ) );
		m.push_back( PerfMetric( "Median Commit Latency (ms, averaged)", 1000 * commitLatency.median(), true ) );
	}
};

struct MeasurePeriodically : IMeasurer {
	double period;
	std::set<std::string> includeMetrics;
	MeasureSinglePeriod msp, msp0;
	vector<PerfMetric> accumulatedMetrics;

	MeasurePeriodically( double period, std::set<std::string> includeMetrics ) : period(period), includeMetrics(includeMetrics), msp(0,period), msp0(0,period) {}

	virtual Future<Void> start() { 
		msp.start();
		return periodicActor(this);
	}
	virtual void addTransaction(ITransactor::Stats* st, double now) {
		msp.addTransaction(st, now);
	}
	virtual void getMetrics( vector<PerfMetric>& m ) {
		m.insert(m.end(), accumulatedMetrics.begin(), accumulatedMetrics.end());
	}
	void nextPeriod(double t) {
		// output stats
		std::string prefix = format("T=%04.0fs:", t);
		vector<PerfMetric> m;
		msp.getMetrics(m);
		for(auto i=m.begin(); i!=m.end(); ++i)
			if (includeMetrics.count(i->name())) {
				accumulatedMetrics.push_back( i->withPrefix(prefix) );
			}

		// reset stats
		msp = msp0;
		msp.start();
	}

	ACTOR static Future<Void> periodicActor( MeasurePeriodically* self ) {
		state double startT = now();
		state double elapsed = 0;
		loop {
			elapsed += self->period;
			wait( delayUntil(startT + elapsed) );
			self->nextPeriod(elapsed);
		}
	}
};

struct MeasureMulti : IMeasurer {
	vector<Reference<IMeasurer>> ms;
	virtual Future<Void> start() {
		vector<Future<Void>> s;
		for(auto m=ms.begin(); m!=ms.end(); ++m)
			s.push_back( (*m)->start() );
		return waitForAll(s);
	}
	virtual void addTransaction(ITransactor::Stats* stats, double now) {
		for(auto m=ms.begin(); m!=ms.end(); ++m)
			(*m)->addTransaction(stats, now);
	}
	virtual void getMetrics( vector<PerfMetric>& metrics ) {
		for(auto m=ms.begin(); m!=ms.end(); ++m)
			(*m)->getMetrics(metrics);
	}
};

struct ThroughputWorkload : TestWorkload {
	double targetLatency, testDuration, Pgain, Igain;
	Reference<ITransactor> op;
	Reference<IMeasurer> measurer;

	int activeActors;
	double totalLatencyIntegral, totalTransactionsIntegral, startT;

	ThroughputWorkload(WorkloadContext const& wcx)
		: TestWorkload(wcx), activeActors(0), totalLatencyIntegral(0), totalTransactionsIntegral(0)
	{
		Reference<MeasureMulti> multi( new MeasureMulti );
		measurer = multi;

		targetLatency = getOption( options, LiteralStringRef("targetLatency"), 0.05 );

		int keyCount = getOption( options, LiteralStringRef("nodeCount"), (uint64_t)100000 );
		int keyBytes = std::max( getOption( options, LiteralStringRef("keyBytes"), 16 ), 16 );
		int maxValueBytes = getOption( options, LiteralStringRef("valueBytes"), 100 );
		int minValueBytes = getOption( options, LiteralStringRef("minValueBytes"), maxValueBytes);
		double sweepDuration = getOption( options, LiteralStringRef("sweepDuration"), 0);
		double sweepDelay = getOption(options, LiteralStringRef("sweepDelay"), 0);

		auto AType = Reference<ITransactor>( new RWTransactor( 
						getOption( options, LiteralStringRef("readsPerTransactionA"), 10 ),
						getOption( options, LiteralStringRef("writesPerTransactionA"), 0 ),
						keyCount, keyBytes, minValueBytes, maxValueBytes ) );
		auto BType = Reference<ITransactor>( new RWTransactor( 
						getOption( options, LiteralStringRef("readsPerTransactionB"), 5 ),
						getOption( options, LiteralStringRef("writesPerTransactionB"), 5 ),
						keyCount, keyBytes, minValueBytes, maxValueBytes ) );

		if (sweepDuration > 0){
			op = Reference<ITransactor>( new SweepTransactor( sweepDuration, sweepDelay, AType, BType ) );
		} else {
			op = Reference<ITransactor>( new ABTransactor( getOption( options, LiteralStringRef("alpha"), 0.1 ), AType, BType) );
		}

		double measureDelay = getOption( options, LiteralStringRef("measureDelay"), 50.0 );
		double measureDuration = getOption( options, LiteralStringRef("measureDuration"), 10.0 );
		multi->ms.push_back( Reference<IMeasurer>( new MeasureSinglePeriod( measureDelay, measureDuration ) ) );

		double measurePeriod = getOption( options, LiteralStringRef("measurePeriod"), 0.0 );
		vector<std::string> periodicMetrics = getOption( options, LiteralStringRef("measurePeriodicMetrics"), vector<std::string>() );
		if (measurePeriod) {
			ASSERT( periodicMetrics.size() != 0 );
			multi->ms.push_back( Reference<IMeasurer>( new MeasurePeriodically( measurePeriod, std::set<std::string>(periodicMetrics.begin(),periodicMetrics.end()) ) ) );
		}

		Pgain = getOption( options, LiteralStringRef("ProportionalGain"), 0.1 );
		Igain = getOption( options, LiteralStringRef("IntegralGain"), 0.005 );

		testDuration = measureDelay + measureDuration;
		//testDuration = getOption( options, LiteralStringRef("testDuration"), measureDelay + measureDuration );
	}

	virtual std::string description() { return "Throughput"; }

	virtual Future<Void> setup( Database const& cx ) {
		return Void();  // No setup for now - use a separate workload to do setup
	}

	virtual Future<Void> start( Database const& cx ) {
		startT = now();
		PromiseStream<Future<Void>> add;
		Future<Void> ac = actorCollection( add.getFuture(), &activeActors );
		Future<Void> r = timeout( measurer->start() && ac, testDuration, Void() );
		ASSERT( !ac.isReady() ); // ... because else the following line would create an unbreakable reference cycle
		add.send( throughputActor( cx, this, add ) );
		return r;
	}

	virtual Future<bool> check( Database const& cx ) { return true; }

	ACTOR static Future<Void> throughputActor( Database db, ThroughputWorkload* self, PromiseStream<Future<Void>> add ) {
		state double before = now();
		state ITransactor::Stats stats;
		wait( self->op->doTransaction(db, &stats) );
		state double after = now();

		wait( delay( 0.0 ) );
		stats.totalLatency = after-before;
		self->measurer->addTransaction( &stats, after );
		
		self->totalLatencyIntegral += after-before;
		self->totalTransactionsIntegral += 1;

		double error = after - before - self->targetLatency;
		// Ideally ierror would be integral [avg. transaction latency - targetLatency] dt.  
		// Actually we calculate integral[ transaction latency - targetLatency ] dtransaction and change units.
		double ierror = (self->totalLatencyIntegral - self->totalTransactionsIntegral * self->targetLatency) / 
			self->totalTransactionsIntegral * (after-self->startT);

		double desiredSuccessors = 1 - (error*self->Pgain + ierror*self->Igain) / self->targetLatency;

		//if (deterministicRandom()->random01() < .001) TraceEvent("ThroughputControl").detail("Error", error).detail("IError", ierror).detail("DesiredSuccessors", desiredSuccessors).detail("ActiveActors", self->activeActors);

		desiredSuccessors = std::min( desiredSuccessors, 2.0 );

		// SOMEDAY: How can we prevent the number of actors on different clients from diverging?

		int successors = deterministicRandom()->random01() + desiredSuccessors;
		if (successors<1 && self->activeActors <= 1) successors = 1;
		if (successors>1 && self->activeActors >= 200000) successors = 1;
		for(int s=0; s<successors; s++)
			add.send( throughputActor(db, self, add) );
		return Void();
	}

	virtual void getMetrics( vector<PerfMetric>& m ) {
		measurer->getMetrics(m);
	}
};
WorkloadFactory<ThroughputWorkload> ThroughputWorkloadFactory("Throughput");
