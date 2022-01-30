/*
 * ReadHotDetection.actor.cpp
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
#include "fdbserver/workloads/BulkSetup.actor.h"
#include "fdbclient/ReadYourWrites.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "flow/actorcompiler.h" // This must be the last #include.

struct ReadHotDetectionWorkload : TestWorkload {
	int actorCount, keyCount;

	double testDuration, transactionsPerSecond;
	vector<Future<Void>> clients;
	Future<Void> readHotCheck;
	Key readKey;
	KeyRange wholeRange;
	bool passed;

	ReadHotDetectionWorkload(WorkloadContext const& wcx) : TestWorkload(wcx) {
		testDuration = getOption(options, LiteralStringRef("testDuration"), 120.0);
		transactionsPerSecond = getOption(options, LiteralStringRef("transactionsPerSecond"), 1000.0) / clientCount;
		actorCount = getOption(options, LiteralStringRef("actorsPerClient"), transactionsPerSecond / 5);
		keyCount = getOption(options, LiteralStringRef("keyCount"), 100);
		readKey = StringRef(format("testkey%08x", deterministicRandom()->randomInt(0, keyCount)));
	}

	virtual std::string description() { return "ReadHotDetection"; }

	virtual Future<Void> setup(Database const& cx) { return _setup(cx, this); }

	virtual Future<Void> start(Database const& cx) {
		for (int c = 0; c < actorCount; c++) {
			clients.push_back(timeout(keyReader(cx->clone(), this, actorCount / transactionsPerSecond,
			                                    deterministicRandom()->random01() > 0.4),
			                          testDuration, Void()));
		}
		readHotCheck = clientId == 0 ? _check(cx->clone(), this) : Void();
		return delay(testDuration);
	}

	virtual Future<bool> check(Database const& cx) {
		if (clientId != 0) return true;
		return passed;
	}

	ACTOR Future<Void> _setup(Database cx, ReadHotDetectionWorkload* self) {
		state int g = 0;
		state Standalone<StringRef> largeValue;
		state Standalone<StringRef> smallValue;
		largeValue = self->randomString(largeValue.arena(), 100000);
		smallValue = self->randomString(smallValue.arena(), 100);
		state ReadYourWritesTransaction tr(cx);
		loop {
			try {
				for (int i = 0; i < self->keyCount; i++) {
					Standalone<StringRef> key = StringRef(format("testkey%08x", i));
					if (key == self->readKey) {
						tr.set(key, largeValue);
					} else {
						tr.set(key, deterministicRandom()->random01() > 0.8 ? largeValue : smallValue);
					}
				}
				wait(tr.commit());
				break;
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}
		self->wholeRange = KeyRangeRef(LiteralStringRef(""), LiteralStringRef("\xff"));
		// TraceEvent("RHDLog").detail("Phase", "DoneSetup");
		return Void();
	}

	ACTOR Future<Void> _check(Database cx, ReadHotDetectionWorkload* self) {
		loop {
			state Transaction tr(cx);
			try {
				StorageMetrics sm = wait(tr.getStorageMetrics(self->wholeRange, 100));
				// TraceEvent("RHDCheckPhaseLog")
				//     .detail("KeyRangeSize", sm.bytes)
				//     .detail("KeyRangeReadBandwith", sm.bytesReadPerKSecond);
				Standalone<VectorRef<ReadHotRangeWithMetrics>> keyRanges = wait(tr.getReadHotRanges(self->wholeRange));
				// TraceEvent("RHDCheckPhaseLog")
				//     .detail("KeyRangesSize", keyRanges.size())
				//     .detail("ReadKey", self->readKey.printable().c_str())
				//     .detail("KeyRangesBackBeginKey", keyRanges.back().begin)
				//     .detail("KeyRangesBackEndKey", keyRanges.back().end);
				// Loose check.
				for (const auto& kr : keyRanges) {
					if (kr.keys.contains(self->readKey)) {
						self->passed = true;
					}
				}
				// The key ranges deemed read hot does not contain the readKey, which is impossible here.
				// TraceEvent("RHDCheckPhaseFailed")
				// 	.detail("KeyRangesSize", keyRanges.size())
				// 	.detail("ReadKey", self->readKey.printable().c_str())
				// 	.detail("KeyRangesBackBeginKey", keyRanges.back().begin)
				// 	.detail("KeyRangesBackEndKey", keyRanges.back().end);
				// for(auto kr : keyRanges) {
				// 	TraceEvent("RHDCheckPhaseFailed").detail("KeyRagneBegin", kr.begin).detail("KeyRagneEnd", kr.end);
				// }
				self->passed = false;
			} catch (Error& e) {
				// TraceEvent("RHDCheckPhaseReadGotError").error(e);
				wait(tr.onError(e));
			}
		}
	}

	virtual void getMetrics(vector<PerfMetric>& m) {}

	ACTOR Future<Void> keyReader(Database cx, ReadHotDetectionWorkload* self, double delay, bool useReadKey) {
		state double lastTime = now();
		loop {
			wait(poisson(&lastTime, delay));
			state ReadYourWritesTransaction tr(cx);
			loop {
				try {
					Optional<Value> v = wait(tr.get(
					    useReadKey
					        ? self->readKey
					        : StringRef(format("testkey%08x", deterministicRandom()->randomInt(0, self->keyCount)))));
					break;
				} catch (Error& e) {
					wait(tr.onError(e));
				}
			}
		}
	}
	StringRef randomString(Arena& arena, int len, char firstChar = 'a', char lastChar = 'z') {
		++lastChar;
		StringRef s = makeString(len, arena);
		for (int i = 0; i < len; ++i) {
			*(uint8_t*)(s.begin() + i) = (uint8_t)deterministicRandom()->randomInt(firstChar, lastChar);
		}
		return s;
	}
};

WorkloadFactory<ReadHotDetectionWorkload> ReadHotDetectionWorkloadFactory("ReadHotDetection");
