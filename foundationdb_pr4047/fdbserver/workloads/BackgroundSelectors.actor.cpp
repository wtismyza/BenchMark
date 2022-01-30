/*
 * BackgroundSelectors.actor.cpp
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
#include "fdbserver/workloads/workloads.actor.h"
#include "flow/actorcompiler.h"  // This must be the last #include.

KeySelector randomizedSelector(const KeyRef &key, bool orEqual, int offset ) {
	if( orEqual && deterministicRandom()->random01() > 0.5 )
		return KeySelectorRef( keyAfter(key), false, offset );
	return KeySelectorRef( key, orEqual, offset );
}

struct BackgroundSelectorWorkload : TestWorkload {
	int actorsPerClient, maxDiff, minDrift, maxDrift, resultLimit;
	double testDuration, transactionsPerSecond;

	vector<Future<Void>> clients;
	PerfIntCounter operations, checks, retries;

	BackgroundSelectorWorkload(WorkloadContext const& wcx)
		: TestWorkload(wcx), operations("Operations"), checks("Checks"), retries("Retries")
	{
		testDuration = getOption( options, LiteralStringRef("testDuration"), 10.0 );
		actorsPerClient = std::max(getOption( options, LiteralStringRef("actorsPerClient"), 1 ), 1 );
		maxDiff = std::max(getOption( options, LiteralStringRef("maxDiff"), 100 ), 2 );
		minDrift = getOption( options, LiteralStringRef("minDiff"), -10 );
		maxDrift = getOption( options, LiteralStringRef("minDiff"), 100 );
		transactionsPerSecond = getOption( options, LiteralStringRef("transactionsPerSecond"), 10.0 ) / (clientCount * actorsPerClient);
		resultLimit = 10*maxDiff;
	}

	virtual std::string description() { return "BackgroundSelector"; }

	virtual Future<Void> setup( Database const& cx ) {
		return Void();
	}

	virtual Future<Void> start( Database const& cx ) {
		return _start( cx, this );
	}

	ACTOR Future<Void> _start( Database cx, BackgroundSelectorWorkload *self ) {
		for(int c=0; c<self->actorsPerClient; c++)
			self->clients.push_back(
				timeout(
				self->backgroundSelectorWorker( cx, self ), self->testDuration, Void()) );
		wait( waitForAll( self->clients ) );
		return Void();
	}

	virtual Future<bool> check( Database const& cx ) {
		bool ok = true;
		for( int i = 0; i < clients.size(); i++ )
			if( clients[i].isError() )
				ok = false;
		clients.clear();
		return ok;
	}

	virtual void getMetrics( vector<PerfMetric>& m ) {
		double duration = testDuration;
		m.push_back( PerfMetric( "Operations/sec", operations.getValue() / duration, false ) );
		m.push_back( operations.getMetric() );
		m.push_back( checks.getMetric() );
		m.push_back( retries.getMetric() );
	}

	ACTOR Future<Void> backgroundSelectorWorker( Database cx, BackgroundSelectorWorkload *self ) {
		state double lastTime = now();
		state bool forward;
		state int direction;
		state Key startKey;
		state Key endKey;
		state int diff;
		state Transaction tr(cx);
		state Standalone<RangeResultRef> rangeResult;
		state Standalone<StringRef> startResult;
		state Standalone<StringRef> endResult;
		state int startDrift;
		state int endDrift;
		state bool restartProcess;

		loop {
			forward = deterministicRandom()->randomInt(0,2) != 0;
			direction = forward ? 1 : -1;
			diff = deterministicRandom()->randomInt(0, self->maxDiff);

			//Setup start and end key
			loop {
				try {
					if( forward ) {
						{
							Standalone<StringRef> res = wait( tr.getKey( KeySelectorRef( allKeys.begin, false, 1 ) ) );
							startKey = res;
						}

						{
							Standalone<StringRef> res = wait( tr.getKey( randomizedSelector( startKey, true, diff) ) );
							endKey = res;
						}
					} else {
						{
							Standalone<StringRef> res = wait( tr.getKey( KeySelectorRef( allKeys.end, false, 0 ) ) );
							endKey = res;
						}

						{
							Standalone<StringRef> res = wait( tr.getKey( randomizedSelector( endKey, true, -1 * diff) ) );
							startKey = res;
						}
					}
					break;
				} catch( Error &e ) {
					wait( tr.onError( e ) );
				}
			}

			loop {
				wait( poisson( &lastTime, 1.0 / self->transactionsPerSecond ) );
				tr.reset();
				startDrift = direction * deterministicRandom()->randomInt( self->minDrift, self->maxDrift );
				endDrift   = direction * deterministicRandom()->randomInt( self->minDrift, self->maxDrift );

				//max sure the end drift does not violate maxDiff
				endDrift = std::max( endDrift, startDrift - self->maxDiff - diff );
				endDrift = std::min( endDrift, startDrift + self->maxDiff - diff );

				diff = diff + endDrift - startDrift;
				if( diff == 0 ) {
					endDrift++;
					diff++;
				}

				loop {
					try {
						if( diff < 0 ) {
							Standalone<RangeResultRef> rangeResult_ = wait( tr.getRange( randomizedSelector(endKey, true, endDrift), randomizedSelector(startKey, true, startDrift + 1), self->resultLimit ) );
							rangeResult = rangeResult_;
							Standalone<StringRef> endResult_ = wait( tr.getKey( randomizedSelector(startKey, true, startDrift) ) );
							endResult = endResult_;
							Standalone<StringRef> startResult_ = wait( tr.getKey( randomizedSelector(endKey, true, endDrift) ) );
							startResult = startResult_;
						}
						else {
							Standalone<RangeResultRef> rangeResult_ = wait( tr.getRange( randomizedSelector(startKey, true, startDrift), randomizedSelector(endKey, true, endDrift + 1), self->resultLimit ) );
							rangeResult = rangeResult_;
							Standalone<StringRef> startResult_ = wait( tr.getKey( randomizedSelector(startKey, true, startDrift) ) );
							startResult = startResult_;
							Standalone<StringRef> endResult_ = wait( tr.getKey( randomizedSelector(endKey, true, endDrift) ) );
							endResult = endResult_;
						}

						restartProcess = false;
						if( rangeResult.size() == 0 ) {
							restartProcess = true;
							break;
						}

						if( rangeResult.size() < self->resultLimit && startResult != allKeys.begin && startResult != allKeys.end ) {
							if( startResult != rangeResult[0].key )
								TraceEvent(SevError, "BackgroundSelectorError").detail("Diff", diff).detail("ResultSize", rangeResult.size()).detail("StartResult", printable(startResult)).detail("RangeResult", printable(rangeResult[0].key));
						} else
							restartProcess = true;

						if( rangeResult.size() < self->resultLimit && endResult != allKeys.begin && endResult != allKeys.end ) {
							if( endResult != rangeResult[rangeResult.size()-1].key )
								TraceEvent(SevError, "BackgroundSelectorError").detail("Diff", diff).detail("ResultSize", rangeResult.size()).detail("EndResult  ", printable(endResult)).detail("RangeResult", printable(rangeResult[rangeResult.size()-1].key));
						} else
							restartProcess = true;

						diff = std::min(rangeResult.size()-1, self->maxDiff);
						startKey = rangeResult[0].key;
						endKey = rangeResult[diff].key;

						break;
					} catch (Error& e) {
						wait( tr.onError(e) );
						++self->retries;
					}
				}
				++self->operations;
				if( restartProcess )
					break;
				++self->checks;
			}
		}
	}
};

WorkloadFactory<BackgroundSelectorWorkload> BackgroundSelectorWorkloadFactory("BackgroundSelector");
