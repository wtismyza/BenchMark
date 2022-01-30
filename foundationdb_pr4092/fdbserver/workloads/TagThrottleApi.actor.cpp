/*
 * TagThrottle.actor.cpp
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

#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/TagThrottle.h"
#include "fdbserver/TesterInterface.actor.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "fdbrpc/simulator.h"
#include "flow/actorcompiler.h" // This must be the last #include.

struct TagThrottleApiWorkload : TestWorkload {
	double testDuration;

	TagThrottleApiWorkload(WorkloadContext const& wcx) : TestWorkload(wcx) {
		testDuration = getOption( options, LiteralStringRef("testDuration"), 10.0 );
	}

	virtual std::string description() { return "TagThrottleApi"; }

	virtual Future<Void> setup(Database const& cx) { 
		DatabaseContext::debugUseTags = true;
		return Void();
	}

	virtual Future<Void> start(Database const& cx) {
		if (this->clientId != 0) return Void();
		return timeout(runThrottleApi(this, cx), testDuration, Void());
	}

	virtual Future<bool> check(Database const& cx) { 
		return true; 
	}

	virtual void getMetrics(vector<PerfMetric>& m) {}

	static Optional<TagThrottleType> randomTagThrottleType() {
		Optional<TagThrottleType> throttleType;
		switch(deterministicRandom()->randomInt(0, 3)) {
			case 0:
				throttleType = TagThrottleType::AUTO;
				break;
			case 1:
				throttleType = TagThrottleType::MANUAL;
				break;
			default:	
				break;
		}

		return throttleType;
	}

	ACTOR Future<Void> throttleTag(Database cx, std::map<std::pair<TransactionTag, TransactionPriority>, TagThrottleInfo> *manuallyThrottledTags) {
		state TransactionTag tag = TransactionTagRef(deterministicRandom()->randomChoice(DatabaseContext::debugTransactionTagChoices));
		state TransactionPriority priority = deterministicRandom()->randomChoice(allTransactionPriorities);
		state double rate = deterministicRandom()->random01() * 20;
		state double duration = 1 + deterministicRandom()->random01() * 19;

		TagSet tagSet;
		tagSet.addTag(tag);

		try {
			wait(ThrottleApi::throttleTags(cx, tagSet, rate, duration, TagThrottleType::MANUAL, priority));
		}
		catch(Error &e) {
			state Error err = e;
			if(e.code() == error_code_too_many_tag_throttles) {
				ASSERT(manuallyThrottledTags->size() >= SERVER_KNOBS->MAX_MANUAL_THROTTLED_TRANSACTION_TAGS);
				return Void();
			}

			throw err;
		}

		manuallyThrottledTags->insert_or_assign(std::make_pair(tag, priority), TagThrottleInfo(tag, TagThrottleType::MANUAL, priority, rate, now() + duration, duration));

		return Void();
	}

	ACTOR Future<Void> unthrottleTag(Database cx, std::map<std::pair<TransactionTag, TransactionPriority>, TagThrottleInfo> *manuallyThrottledTags) {
		state TransactionTag tag = TransactionTagRef(deterministicRandom()->randomChoice(DatabaseContext::debugTransactionTagChoices));
		TagSet tagSet;
		tagSet.addTag(tag);

		state Optional<TagThrottleType> throttleType = TagThrottleApiWorkload::randomTagThrottleType();
		Optional<TransactionPriority> priority = deterministicRandom()->coinflip() ? Optional<TransactionPriority>() : deterministicRandom()->randomChoice(allTransactionPriorities);

		state bool erased = false;
		state double maxExpiration = 0;
		if(!throttleType.present() || throttleType.get() == TagThrottleType::MANUAL) {
			for(auto p : allTransactionPriorities) {
				if(!priority.present() || priority.get() == p) {
					auto itr = manuallyThrottledTags->find(std::make_pair(tag, p));
					if(itr != manuallyThrottledTags->end()) {
						maxExpiration = std::max(maxExpiration, itr->second.expirationTime);
						erased = true;
						manuallyThrottledTags->erase(itr);
					}
				}
			}
		}

		bool removed = wait(ThrottleApi::unthrottleTags(cx, tagSet, throttleType, priority));
		if(removed) {
			ASSERT(erased || !throttleType.present() || throttleType.get() == TagThrottleType::AUTO);
		}
		else {
			ASSERT(maxExpiration < now());
		}

		return Void();
	}

	ACTOR Future<Void> getTags(Database cx, std::map<std::pair<TransactionTag, TransactionPriority>, TagThrottleInfo> const* manuallyThrottledTags) { 
		std::vector<TagThrottleInfo> tags = wait(ThrottleApi::getThrottledTags(cx, CLIENT_KNOBS->TOO_MANY));

		int manualThrottledTags = 0;
		int activeAutoThrottledTags = 0;
		for(auto &tag : tags) {
			if(tag.throttleType == TagThrottleType::MANUAL) {
				ASSERT(manuallyThrottledTags->find(std::make_pair(tag.tag, tag.priority)) != manuallyThrottledTags->end());
				++manualThrottledTags;
			}
			else if(tag.expirationTime > now()) {
				++activeAutoThrottledTags;
			}
		}

		ASSERT(manualThrottledTags <= SERVER_KNOBS->MAX_MANUAL_THROTTLED_TRANSACTION_TAGS);
		ASSERT(activeAutoThrottledTags <= SERVER_KNOBS->MAX_AUTO_THROTTLED_TRANSACTION_TAGS);

		int minManualThrottledTags = 0;
		int maxManualThrottledTags = 0;
		for(auto &tag : *manuallyThrottledTags) {
			if(tag.second.expirationTime > now()) {
				++minManualThrottledTags;
			}
			++maxManualThrottledTags;
		}

		ASSERT(manualThrottledTags >= minManualThrottledTags && manualThrottledTags <= maxManualThrottledTags);
		return Void();
	}

	ACTOR Future<Void> unthrottleTagGroup(Database cx, std::map<std::pair<TransactionTag, TransactionPriority>, TagThrottleInfo> *manuallyThrottledTags) {
		state Optional<TagThrottleType> throttleType = TagThrottleApiWorkload::randomTagThrottleType();
		state Optional<TransactionPriority> priority = deterministicRandom()->coinflip() ? Optional<TransactionPriority>() : deterministicRandom()->randomChoice(allTransactionPriorities);

		bool unthrottled = wait(ThrottleApi::unthrottleAll(cx, throttleType, priority));
		if(!throttleType.present() || throttleType.get() == TagThrottleType::MANUAL) {
			bool unthrottleExpected = false;
			bool empty = manuallyThrottledTags->empty();
			for(auto itr = manuallyThrottledTags->begin(); itr != manuallyThrottledTags->end();) {
				if(!priority.present() || priority.get() == itr->first.second) {
					if(itr->second.expirationTime > now()) {
						unthrottleExpected = true;
					}

					itr = manuallyThrottledTags->erase(itr);
				}
				else {
					++itr;
				}
			}

			if(throttleType.present()) {
				ASSERT((unthrottled && !empty) || (!unthrottled && !unthrottleExpected));
			}
			else {
				ASSERT(unthrottled || !unthrottleExpected);
			}
		}

		return Void();
	}

	ACTOR Future<Void> enableAutoThrottling(Database cx) {
		if(deterministicRandom()->coinflip()) {
			wait(ThrottleApi::enableAuto(cx, true));
			if(deterministicRandom()->coinflip()) {
				bool unthrottled = wait(ThrottleApi::unthrottleAll(cx, TagThrottleType::AUTO, Optional<TransactionPriority>()));
			}
		}
		else {
			wait(ThrottleApi::enableAuto(cx, false));
		}

		return Void();
	}

	ACTOR Future<Void> runThrottleApi(TagThrottleApiWorkload *self, Database cx) {
		state std::map<std::pair<TransactionTag, TransactionPriority>, TagThrottleInfo> manuallyThrottledTags;
		loop {
			double delayTime = deterministicRandom()->random01() * 5;
			wait(delay(delayTime));

			state int action = deterministicRandom()->randomInt(0, 5);

			if(action == 0) {
				wait(self->throttleTag(cx, &manuallyThrottledTags));
			}
			else if(action == 1) {
				wait(self->unthrottleTag(cx, &manuallyThrottledTags));
			}
			else if(action == 2) {
				wait(self->getTags(cx, &manuallyThrottledTags));
			}
			else if(action == 3) {
				wait(self->unthrottleTagGroup(cx, &manuallyThrottledTags));
			}
			else if(action == 4) { 
				wait(self->enableAutoThrottling(cx));
			}
		}
	}
};

WorkloadFactory<TagThrottleApiWorkload> TagThrottleApiWorkloadFactory("TagThrottleApi");
