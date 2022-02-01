/*
 * Increment.actor.cpp
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
#include "fdbserver/workloads/workloads.actor.h"
#include "fdbserver/workloads/BulkSetup.actor.h"
#include "flow/actorcompiler.h"  // This must be the last #include.

struct Increment : TestWorkload {
	int actorCount, nodeCount;
	double testDuration, transactionsPerSecond, minExpectedTransactionsPerSecond;

	vector<Future<Void>> clients;
	PerfIntCounter transactions, retries, tooOldRetries, commitFailedRetries;
	PerfDoubleCounter totalLatency;

	Increment(WorkloadContext const& wcx)
		: TestWorkload(wcx),
		transactions("Transactions"), retries("Retries"), totalLatency("Latency"),
		tooOldRetries("Retries.too_old"), commitFailedRetries("Retries.commit_failed")
	{
		testDuration = getOption( options, LiteralStringRef("testDuration"), 10.0 );
		transactionsPerSecond = getOption( options, LiteralStringRef("transactionsPerSecond"), 5000.0 );
		actorCount = getOption( options, LiteralStringRef("actorsPerClient"), transactionsPerSecond / 5 );
		nodeCount = getOption( options, LiteralStringRef("nodeCount"), transactionsPerSecond * clientCount );
		minExpectedTransactionsPerSecond = transactionsPerSecond * getOption( options, LiteralStringRef("expectedRate"), 0.7 );
	}

	virtual std::string description() { return "IncrementWorkload"; }

	virtual Future<Void> setup( Database const& cx ) {
		return Void();
	}
	virtual Future<Void> start( Database const& cx ) {
		for(int c=0; c<actorCount; c++)
			clients.push_back(
				timeout(
					incrementClient( cx->clone(), this, actorCount / transactionsPerSecond ), testDuration, Void()) );
		return delay(testDuration);
	}
	virtual Future<bool> check( Database const& cx ) {
		int errors = 0;
		for(int c=0; c<clients.size(); c++)
			errors += clients[c].isError();
		if (errors)
			TraceEvent(SevError, "TestFailure").detail("Reason", "There were client errors.");
		clients.clear();
		return incrementCheck( cx->clone(), this, !errors );
	}
	virtual void getMetrics( vector<PerfMetric>& m ) {
		m.push_back( transactions.getMetric() );
		m.push_back( retries.getMetric() );
		m.push_back( tooOldRetries.getMetric() );
		m.push_back( commitFailedRetries.getMetric() );
		m.push_back( PerfMetric( "Avg Latency (ms)", 1000 * totalLatency.getValue() / transactions.getValue(), true ) );
		m.push_back( PerfMetric( "Read rows/simsec (approx)", transactions.getValue() * 3 / testDuration, false ) );
		m.push_back( PerfMetric( "Write rows/simsec (approx)", transactions.getValue() * 4 / testDuration, false ) );
	}

	static Key intToTestKey( int i ) {
		return StringRef(format("%016d", i));
	}

	ACTOR Future<Void> incrementClient( Database cx, Increment* self, double delay ) {
		state double lastTime = now();
		try {
			loop {
				wait( poisson( &lastTime, delay ) );

				state double tstart = now();
				state Transaction tr(cx);
				while (true) {
					try {
						tr.atomicOp(intToTestKey(deterministicRandom()->randomInt(0,self->nodeCount/2)), LiteralStringRef("\x01"), MutationRef::AddValue);
						tr.atomicOp(intToTestKey(deterministicRandom()->randomInt(self->nodeCount/2,self->nodeCount)), LiteralStringRef("\x01"), MutationRef::AddValue);
						wait( tr.commit() );
						break;
					} catch (Error& e) {
						if (e.code() == error_code_transaction_too_old) ++self->tooOldRetries;
						else if (e.code() == error_code_not_committed) ++self->commitFailedRetries;
						wait( tr.onError(e) );
					}
					++self->retries;
				}
				++self->transactions;
				self->totalLatency += now() - tstart;
			}
		} catch (Error& e) {
			TraceEvent(SevError, "IncrementClient").error(e);
			throw;
		}
	}
	bool incrementCheckData( const VectorRef<KeyValueRef>& data, Version v, Increment* self ) {
		TEST( self->transactions.getValue() );
		if (self->transactions.getValue() && data.size() == 0) {
			TraceEvent(SevError, "TestFailure").detail("Reason", "No successful increments").detail("Before", nodeCount).detail("After", data.size()).detail("Version", v);
			return false;
		}
		int firstSum = 0;
		int secondSum = 0;
		for(auto it : data) {
			ASSERT( it.value.size() <= sizeof(uint64_t) );
			uint64_t intValue = 0;
			memcpy(&intValue, it.value.begin(), it.value.size());
			if(it.key < intToTestKey(nodeCount/2)) {
				firstSum += intValue;
			}
			else {
				secondSum += intValue;
			}
		}
		if (firstSum != secondSum) {
			TraceEvent(SevError, "TestFailure").detail("Reason", "Bad increments").detail("A", firstSum).detail("B", secondSum);
			return false;
		}
		return true;
	}
	ACTOR Future<bool> incrementCheck( Database cx, Increment* self, bool ok ) {
		if (self->transactions.getMetric().value() < self->testDuration * self->minExpectedTransactionsPerSecond) {
			TraceEvent(SevWarnAlways, "TestFailure").detail("Reason", "Rate below desired rate").detail("Details", format("%.2f", self->transactions.getMetric().value() / (self->transactionsPerSecond * self->testDuration)))
				.detail("TransactionsAchieved", self->transactions.getMetric().value())
				.detail("MinTransactionsExpected", self->testDuration * self->minExpectedTransactionsPerSecond)
				.detail("TransactionGoal", self->transactionsPerSecond * self->testDuration);
			ok = false;
		}
		if (!self->clientId) {
			// One client checks the validity of the cycle
			state Transaction tr(cx);
			state int retryCount = 0;
			loop {
				try {
					state Version v = wait( tr.getReadVersion() );
					Standalone<RangeResultRef> data = wait( tr.getRange( firstGreaterOrEqual(intToTestKey(0)), firstGreaterOrEqual(intToTestKey(self->nodeCount)), self->nodeCount+1) );
					ok = self->incrementCheckData( data, v, self ) && ok;
					break;
				} catch (Error& e) {
					retryCount++;
					TraceEvent(retryCount > 20 ? SevWarnAlways : SevWarn, "IncrementCheckError").error(e);
					wait(tr.onError(e));
				}
			}
		}
		return ok;
	}
};

WorkloadFactory<Increment> IncrementWorkloadFactory("Increment");
