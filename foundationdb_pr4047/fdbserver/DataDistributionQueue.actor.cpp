/*
 * DataDistributionQueue.actor.cpp
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

#include <numeric>
#include <limits>

#include "flow/ActorCollection.h"
#include "flow/Util.h"
#include "fdbrpc/sim_validation.h"
#include "fdbclient/SystemData.h"
#include "fdbserver/DataDistribution.actor.h"
#include "fdbclient/DatabaseContext.h"
#include "fdbserver/MoveKeys.actor.h"
#include "fdbserver/Knobs.h"
#include "fdbrpc/simulator.h"
#include "flow/actorcompiler.h"  // This must be the last #include.

#define WORK_FULL_UTILIZATION 10000   // This is not a knob; it is a fixed point scaling factor!

struct RelocateData {
	KeyRange keys;
	int priority;
	int boundaryPriority;
	int healthPriority;

	double startTime;
	UID randomId;
	int workFactor;
	std::vector<UID> src;
	std::vector<UID> completeSources;
	bool wantsNewServers;
	TraceInterval interval;

	RelocateData() : startTime(-1), priority(-1), boundaryPriority(-1), healthPriority(-1), workFactor(0), wantsNewServers(false), interval("QueuedRelocation") {}
	explicit RelocateData( RelocateShard const& rs ) : keys(rs.keys), priority(rs.priority), boundaryPriority(isBoundaryPriority(rs.priority) ? rs.priority : -1), healthPriority(isHealthPriority(rs.priority) ? rs.priority : -1), startTime(now()), randomId(deterministicRandom()->randomUniqueID()), workFactor(0),
		wantsNewServers(
			rs.priority == SERVER_KNOBS->PRIORITY_REBALANCE_OVERUTILIZED_TEAM ||
			rs.priority == SERVER_KNOBS->PRIORITY_REBALANCE_UNDERUTILIZED_TEAM ||
			rs.priority == SERVER_KNOBS->PRIORITY_SPLIT_SHARD ||
			rs.priority == SERVER_KNOBS->PRIORITY_TEAM_REDUNDANT), interval("QueuedRelocation") {}

	static bool isHealthPriority(int priority) {
		return  priority == SERVER_KNOBS->PRIORITY_POPULATE_REGION ||
				priority == SERVER_KNOBS->PRIORITY_TEAM_UNHEALTHY || 
				priority == SERVER_KNOBS->PRIORITY_TEAM_2_LEFT ||
				priority == SERVER_KNOBS->PRIORITY_TEAM_1_LEFT ||
				priority == SERVER_KNOBS->PRIORITY_TEAM_0_LEFT ||
				priority == SERVER_KNOBS->PRIORITY_TEAM_REDUNDANT ||
				priority == SERVER_KNOBS->PRIORITY_TEAM_HEALTHY ||
				priority == SERVER_KNOBS->PRIORITY_TEAM_CONTAINS_UNDESIRED_SERVER;
	}

	static bool isBoundaryPriority(int priority) {
		return  priority == SERVER_KNOBS->PRIORITY_SPLIT_SHARD || 
				priority == SERVER_KNOBS->PRIORITY_MERGE_SHARD;
	}

	bool operator> (const RelocateData& rhs) const {
		return priority != rhs.priority ? priority > rhs.priority : ( startTime != rhs.startTime ? startTime < rhs.startTime : randomId > rhs.randomId );
	}

	bool operator== (const RelocateData& rhs) const {
		return priority == rhs.priority && boundaryPriority == rhs.boundaryPriority && healthPriority == rhs.healthPriority && keys == rhs.keys && startTime == rhs.startTime && workFactor == rhs.workFactor && src == rhs.src && completeSources == rhs.completeSources && wantsNewServers == rhs.wantsNewServers && randomId == rhs.randomId;
	}
};

class ParallelTCInfo : public ReferenceCounted<ParallelTCInfo>, public IDataDistributionTeam {
public:
	vector<Reference<IDataDistributionTeam>> teams;
	vector<UID> tempServerIDs;

	ParallelTCInfo() {}

	void addTeam(Reference<IDataDistributionTeam> team) {
		teams.push_back(team);
	}

	void clear() {
		teams.clear();
	}

	int64_t sum(std::function<int64_t(Reference<IDataDistributionTeam>)> func) {
		int64_t result = 0;
		for (auto it = teams.begin(); it != teams.end(); it++) {
			result += func(*it);
		}
		return result;
	}

	template<class T>
	vector<T> collect(std::function < vector<T>(Reference<IDataDistributionTeam>)> func) {
		vector<T> result;

		for (auto it = teams.begin(); it != teams.end(); it++) {
			vector<T> newItems = func(*it);
			result.insert(result.end(), newItems.begin(), newItems.end());
		}
		return result;
	}

	bool any(std::function<bool(Reference<IDataDistributionTeam>)> func) {
		for (auto it = teams.begin(); it != teams.end(); it++) {
			if (func(*it)) {
				return true;
			}
		}
		return false;
	}

	bool all(std::function<bool(Reference<IDataDistributionTeam>)> func) {
		return !any([func](Reference<IDataDistributionTeam> team) {
			return !func(team);
		});
	}

	virtual vector<StorageServerInterface> getLastKnownServerInterfaces() {
		return collect<StorageServerInterface>([](Reference<IDataDistributionTeam> team) {
			return team->getLastKnownServerInterfaces();
		});
	}

	virtual int size() {
		int totalSize = 0;
		for (auto it = teams.begin(); it != teams.end(); it++) {
			totalSize += (*it)->size();
		}
		return totalSize;
	}

	virtual vector<UID> const& getServerIDs() {
		tempServerIDs.clear();
		for (auto it = teams.begin(); it != teams.end(); it++) {
			vector<UID> const& childIDs = (*it)->getServerIDs();
			tempServerIDs.insert(tempServerIDs.end(), childIDs.begin(), childIDs.end());
		}
		return tempServerIDs;
	}

	virtual void addDataInFlightToTeam(int64_t delta) {
		for (auto it = teams.begin(); it != teams.end(); it++) {
			(*it)->addDataInFlightToTeam(delta);
		}
	}

	virtual int64_t getDataInFlightToTeam() {
		return sum([](Reference<IDataDistributionTeam> team) {
			return team->getDataInFlightToTeam();
		});
	}

	virtual int64_t getLoadBytes(bool includeInFlight = true, double inflightPenalty = 1.0 ) {
		return sum([includeInFlight, inflightPenalty](Reference<IDataDistributionTeam> team) {
			return team->getLoadBytes(includeInFlight, inflightPenalty);
		});
	}

	virtual int64_t getMinAvailableSpace(bool includeInFlight = true) {
		int64_t result = std::numeric_limits<int64_t>::max();
		for (auto it = teams.begin(); it != teams.end(); it++) {
			result = std::min(result, (*it)->getMinAvailableSpace(includeInFlight));
		}
		return result;
	}

	virtual double getMinAvailableSpaceRatio(bool includeInFlight = true) {
		double result = std::numeric_limits<double>::max();
		for (auto it = teams.begin(); it != teams.end(); it++) {
			result = std::min(result, (*it)->getMinAvailableSpaceRatio(includeInFlight));
		}
		return result;
	}

	virtual bool hasHealthyAvailableSpace(double minRatio) {
		return all([minRatio](Reference<IDataDistributionTeam> team) {
			return team->hasHealthyAvailableSpace(minRatio);
		});
	}

	virtual Future<Void> updateStorageMetrics() {
		vector<Future<Void>> futures;

		for (auto it = teams.begin(); it != teams.end(); it++) {
			futures.push_back((*it)->updateStorageMetrics());
		}
		return waitForAll(futures);
	}

	virtual bool isOptimal() {
		return all([](Reference<IDataDistributionTeam> team) {
			return team->isOptimal();
		});
	}

	virtual bool isWrongConfiguration() {
		return any([](Reference<IDataDistributionTeam> team) {
			return team->isWrongConfiguration();
		});
	}
	virtual void setWrongConfiguration(bool wrongConfiguration) {
		for (auto it = teams.begin(); it != teams.end(); it++) {
			(*it)->setWrongConfiguration(wrongConfiguration);
		}
	}

	virtual bool isHealthy() {
		return all([](Reference<IDataDistributionTeam> team) {
			return team->isHealthy();
		});
	}

	virtual void setHealthy(bool h) {
		for (auto it = teams.begin(); it != teams.end(); it++) {
			(*it)->setHealthy(h);
		}
	}
	virtual int getPriority() {
		int priority = 0;
		for (auto it = teams.begin(); it != teams.end(); it++) {
			priority = std::max(priority, (*it)->getPriority());
		}
		return priority;
	}

	virtual void setPriority(int p) {
		for (auto it = teams.begin(); it != teams.end(); it++) {
			(*it)->setPriority(p);
		}
	}
	virtual void addref() { ReferenceCounted<ParallelTCInfo>::addref(); }
	virtual void delref() { ReferenceCounted<ParallelTCInfo>::delref(); }

	virtual void addServers(const std::vector<UID>& servers) {
		ASSERT(!teams.empty());
		teams[0]->addServers(servers);
	}

	std::string getTeamID() override {
		std::string id;
		for (int i = 0; i < teams.size(); i++) {
			auto const& team = teams[i];
			id += (i == teams.size() - 1) ? team->getTeamID() : format("%s, ", team->getTeamID().c_str());
		}
		return id;
	}
};

struct Busyness {
	vector<int> ledger;

	Busyness() : ledger( 10, 0 ) {}

	bool canLaunch( int prio, int work ) {
		ASSERT( prio > 0 && prio < 1000 );
		return ledger[ prio / 100 ] <= WORK_FULL_UTILIZATION - work;  // allow for rounding errors in double division
	}
	void addWork( int prio, int work ) {
		ASSERT( prio > 0 && prio < 1000 );
		for( int i = 0; i <= (prio / 100); i++ )
			ledger[i] += work;
	}
	void removeWork( int prio, int work ) {
		addWork( prio, -work );
	}
	std::string toString() {
		std::string result;
		for(int i = 1; i < ledger.size();) {
			int j = i+1;
			while(j < ledger.size() && ledger[i] == ledger[j])
				j++;
			if(i != 1)
				result += ", ";
			result += i+1 == j ? format("%03d", i*100) : format("%03d/%03d", i*100, (j-1)*100);
			result += format("=%1.02f", (float)ledger[i] / WORK_FULL_UTILIZATION);
			i = j;
		}
		return result;
	}
};

// find the "workFactor" for this, were it launched now
int getWorkFactor( RelocateData const& relocation, int singleRegionTeamSize ) {
	if( relocation.healthPriority == SERVER_KNOBS->PRIORITY_TEAM_1_LEFT || relocation.healthPriority == SERVER_KNOBS->PRIORITY_TEAM_0_LEFT )
		return WORK_FULL_UTILIZATION / SERVER_KNOBS->RELOCATION_PARALLELISM_PER_SOURCE_SERVER;
	else if( relocation.healthPriority == SERVER_KNOBS->PRIORITY_TEAM_2_LEFT )
		return WORK_FULL_UTILIZATION / 2 / SERVER_KNOBS->RELOCATION_PARALLELISM_PER_SOURCE_SERVER;
	else // for now we assume that any message at a lower priority can best be assumed to have a full team left for work
		return WORK_FULL_UTILIZATION / singleRegionTeamSize / SERVER_KNOBS->RELOCATION_PARALLELISM_PER_SOURCE_SERVER;
}

// Data movement's resource control: Do not overload source servers used for the RelocateData
// return true if servers are not too busy to launch the relocation
bool canLaunch( RelocateData & relocation, int teamSize, int singleRegionTeamSize, std::map<UID, Busyness> & busymap,
		std::vector<RelocateData> cancellableRelocations ) {
	// assert this has not already been launched
	ASSERT( relocation.workFactor == 0 );
	ASSERT( relocation.src.size() != 0 );
	ASSERT( teamSize >= singleRegionTeamSize );

	// find the "workFactor" for this, were it launched now
	int workFactor = getWorkFactor( relocation, singleRegionTeamSize );
	int neededServers = std::min<int>( relocation.src.size(), teamSize - singleRegionTeamSize + 1 );
	if(SERVER_KNOBS->USE_OLD_NEEDED_SERVERS) {
		neededServers = std::max( 1, (int)relocation.src.size() - teamSize + 1 );
	}
	// see if each of the SS can launch this task
	for( int i = 0; i < relocation.src.size(); i++ ) {
		// For each source server for this relocation, copy and modify its busyness to reflect work that WOULD be cancelled
		auto busyCopy = busymap[ relocation.src[i] ];
		for( int j = 0; j < cancellableRelocations.size(); j++ ) {
			auto& servers = cancellableRelocations[j].src;
			if( std::count( servers.begin(), servers.end(), relocation.src[i] ) )
				busyCopy.removeWork( cancellableRelocations[j].priority, cancellableRelocations[j].workFactor );
		}
		// Use this modified busyness to check if this relocation could be launched
		if( busyCopy.canLaunch( relocation.priority, workFactor ) ) {
			--neededServers;
			if( neededServers == 0 )
				return true;
		}
	}
	return false;
}

// update busyness for each server
void launch( RelocateData & relocation, std::map<UID, Busyness> & busymap, int singleRegionTeamSize ) {
	// if we are here this means that we can launch and should adjust all the work the servers can do
	relocation.workFactor = getWorkFactor( relocation, singleRegionTeamSize );
	for( int i = 0; i < relocation.src.size(); i++ )
		busymap[ relocation.src[i] ].addWork( relocation.priority, relocation.workFactor );
}

void complete( RelocateData const& relocation, std::map<UID, Busyness> & busymap ) {
	ASSERT( relocation.workFactor > 0 );
	for( int i = 0; i < relocation.src.size(); i++ )
		busymap[ relocation.src[i] ].removeWork( relocation.priority, relocation.workFactor );
}

Future<Void> dataDistributionRelocator( struct DDQueueData* const& self, RelocateData const& rd );

struct DDQueueData {
	UID distributorId;
	MoveKeysLock lock;
	Database cx;

	std::vector<TeamCollectionInterface> teamCollections;
	Reference<ShardsAffectedByTeamFailure> shardsAffectedByTeamFailure;
	PromiseStream<Promise<int64_t>> getAverageShardBytes;

	FlowLock startMoveKeysParallelismLock;
	FlowLock finishMoveKeysParallelismLock;
	Reference<FlowLock> fetchSourceLock;

	int activeRelocations;
	int queuedRelocations;
	int64_t bytesWritten;
	int teamSize;
	int singleRegionTeamSize;

	std::map<UID, Busyness> busymap;

	KeyRangeMap< RelocateData > queueMap;
	std::set<RelocateData, std::greater<RelocateData>> fetchingSourcesQueue;
	std::set<RelocateData, std::greater<RelocateData>> fetchKeysComplete;
	KeyRangeActorMap getSourceActors;
	std::map<UID, std::set<RelocateData, std::greater<RelocateData>>> queue; //Key UID is serverID, value is the serverID's set of RelocateData to relocate

	KeyRangeMap< RelocateData > inFlight;
	KeyRangeActorMap inFlightActors; //Key: RelocatData, Value: Actor to move the data

	Promise<Void> error;
	PromiseStream<RelocateData> dataTransferComplete;
	PromiseStream<RelocateData> relocationComplete;
	PromiseStream<RelocateData> fetchSourceServersComplete;

	PromiseStream<RelocateShard> output;
	FutureStream<RelocateShard> input;
	PromiseStream<GetMetricsRequest> getShardMetrics;

	double* lastLimited;
	double lastInterval;
	int suppressIntervals;

	Reference<AsyncVar<bool>> rawProcessingUnhealthy; //many operations will remove relocations before adding a new one, so delay a small time before settling on a new number.

	std::map<int, int> priority_relocations;
	int unhealthyRelocations;
	void startRelocation(int priority, int healthPriority) {
		// Although PRIORITY_TEAM_REDUNDANT has lower priority than split and merge shard movement,
		// we must count it into unhealthyRelocations; because team removers relies on unhealthyRelocations to
		// ensure a team remover will not start before the previous one finishes removing a team and move away data
		// NOTE: split and merge shard have higher priority. If they have to wait for unhealthyRelocations = 0,
		// deadlock may happen: split/merge shard waits for unhealthyRelocations, while blocks team_redundant.
		if (healthPriority == SERVER_KNOBS->PRIORITY_POPULATE_REGION || healthPriority == SERVER_KNOBS->PRIORITY_TEAM_UNHEALTHY || healthPriority == SERVER_KNOBS->PRIORITY_TEAM_2_LEFT ||
		 healthPriority == SERVER_KNOBS->PRIORITY_TEAM_1_LEFT || healthPriority == SERVER_KNOBS->PRIORITY_TEAM_0_LEFT || healthPriority == SERVER_KNOBS->PRIORITY_TEAM_REDUNDANT) { 
			unhealthyRelocations++;
			rawProcessingUnhealthy->set(true);
		}
		priority_relocations[priority]++;
	}
	void finishRelocation(int priority, int healthPriority) {
		if (healthPriority == SERVER_KNOBS->PRIORITY_POPULATE_REGION || healthPriority == SERVER_KNOBS->PRIORITY_TEAM_UNHEALTHY || healthPriority == SERVER_KNOBS->PRIORITY_TEAM_2_LEFT ||
		 healthPriority == SERVER_KNOBS->PRIORITY_TEAM_1_LEFT || healthPriority == SERVER_KNOBS->PRIORITY_TEAM_0_LEFT || healthPriority == SERVER_KNOBS->PRIORITY_TEAM_REDUNDANT) {
			unhealthyRelocations--;
			ASSERT(unhealthyRelocations >= 0);
			if(unhealthyRelocations == 0) {
				rawProcessingUnhealthy->set(false);
			}
		} 
		priority_relocations[priority]--;
	}

	DDQueueData( UID mid, MoveKeysLock lock, Database cx, std::vector<TeamCollectionInterface> teamCollections,
		Reference<ShardsAffectedByTeamFailure> sABTF, PromiseStream<Promise<int64_t>> getAverageShardBytes,
		int teamSize, int singleRegionTeamSize, PromiseStream<RelocateShard> output, FutureStream<RelocateShard> input, PromiseStream<GetMetricsRequest> getShardMetrics, double* lastLimited ) :
			activeRelocations( 0 ), queuedRelocations( 0 ), bytesWritten ( 0 ), teamCollections( teamCollections ),
			shardsAffectedByTeamFailure( sABTF ), getAverageShardBytes( getAverageShardBytes ), distributorId( mid ), lock( lock ),
			cx( cx ), teamSize( teamSize ), singleRegionTeamSize( singleRegionTeamSize ), output( output ), input( input ), getShardMetrics( getShardMetrics ), startMoveKeysParallelismLock( SERVER_KNOBS->DD_MOVE_KEYS_PARALLELISM ),
			finishMoveKeysParallelismLock( SERVER_KNOBS->DD_MOVE_KEYS_PARALLELISM ), fetchSourceLock( new FlowLock(SERVER_KNOBS->DD_FETCH_SOURCE_PARALLELISM) ), lastLimited(lastLimited),
			suppressIntervals(0), lastInterval(0), unhealthyRelocations(0), rawProcessingUnhealthy( new AsyncVar<bool>(false) ) {}

	void validate() {
		if( EXPENSIVE_VALIDATION ) {
			for( auto it = fetchingSourcesQueue.begin(); it != fetchingSourcesQueue.end(); ++it ) {
				// relocates in the fetching queue do not have src servers yet.
				if( it->src.size() )
					TraceEvent(SevError, "DDQueueValidateError1").detail("Problem", "relocates in the fetching queue do not have src servers yet");

				// relocates in the fetching queue do not have a work factor yet.
				if( it->workFactor != 0.0 )
					TraceEvent(SevError, "DDQueueValidateError2").detail("Problem", "relocates in the fetching queue do not have a work factor yet");

				// relocates in the fetching queue are in the queueMap.
				auto range = queueMap.rangeContaining( it->keys.begin );
				if( range.value() != *it || range.range() != it->keys )
					TraceEvent(SevError, "DDQueueValidateError3").detail("Problem", "relocates in the fetching queue are in the queueMap");
			}

			/*
			for( auto it = queue.begin(); it != queue.end(); ++it ) {
				for( auto rdit = it->second.begin(); rdit != it->second.end(); ++rdit ) {
					// relocates in the queue are in the queueMap exactly.
					auto range = queueMap.rangeContaining( rdit->keys.begin );
					if( range.value() != *rdit || range.range() != rdit->keys )
						TraceEvent(SevError, "DDQueueValidateError4").detail("Problem", "relocates in the queue are in the queueMap exactly")
						.detail("RangeBegin", range.range().begin)
						.detail("RangeEnd", range.range().end)
						.detail("RelocateBegin2", range.value().keys.begin)
						.detail("RelocateEnd2", range.value().keys.end)
						.detail("RelocateStart", range.value().startTime)
						.detail("MapStart", rdit->startTime)
						.detail("RelocateWork", range.value().workFactor)
						.detail("MapWork", rdit->workFactor)
						.detail("RelocateSrc", range.value().src.size())
						.detail("MapSrc", rdit->src.size())
						.detail("RelocatePrio", range.value().priority)
						.detail("MapPrio", rdit->priority);

					// relocates in the queue have src servers
					if( !rdit->src.size() )
						TraceEvent(SevError, "DDQueueValidateError5").detail("Problem", "relocates in the queue have src servers");

					// relocates in the queue do not have a work factor yet.
					if( rdit->workFactor != 0.0 )
						TraceEvent(SevError, "DDQueueValidateError6").detail("Problem", "relocates in the queue do not have a work factor yet");

					bool contains = false;
					for( int i = 0; i < rdit->src.size(); i++ ) {
						if( rdit->src[i] == it->first ) {
							contains = true;
							break;
						}
					}
					if( !contains )
						TraceEvent(SevError, "DDQueueValidateError7").detail("Problem", "queued relocate data does not include ss under which its filed");
				}
			}*/

			auto inFlightRanges = inFlight.ranges();
			for( auto it = inFlightRanges.begin(); it != inFlightRanges.end(); ++it ) {
				for( int i = 0; i < it->value().src.size(); i++ ) {
					// each server in the inFlight map is in the busymap
					if( !busymap.count( it->value().src[i] ) )
						TraceEvent(SevError, "DDQueueValidateError8").detail("Problem", "each server in the inFlight map is in the busymap");

					// relocate data that is inFlight is not also in the queue
					if( queue[it->value().src[i]].count( it->value() ) )
						TraceEvent(SevError, "DDQueueValidateError9").detail("Problem", "relocate data that is inFlight is not also in the queue");
				}

				// in flight relocates have source servers
				if( it->value().startTime != -1 && !it->value().src.size() )
					TraceEvent(SevError, "DDQueueValidateError10").detail("Problem", "in flight relocates have source servers");

				if( inFlightActors.liveActorAt( it->range().begin ) ) {
					// the key range in the inFlight map matches the key range in the RelocateData message
					if( it->value().keys != it->range() )
						TraceEvent(SevError, "DDQueueValidateError11").detail("Problem", "the key range in the inFlight map matches the key range in the RelocateData message");
				}
			}

			for( auto it = busymap.begin(); it != busymap.end(); ++it ) {
				for( int i = 0; i < it->second.ledger.size() - 1; i++ ) {
					if( it->second.ledger[i] < it->second.ledger[i+1] )
						TraceEvent(SevError, "DDQueueValidateError12").detail("Problem", "ascending ledger problem")
						.detail("LedgerLevel", i).detail("LedgerValueA", it->second.ledger[i]).detail("LedgerValueB", it->second.ledger[i+1]);
					if( it->second.ledger[i] < 0.0 )
						TraceEvent(SevError, "DDQueueValidateError13").detail("Problem", "negative ascending problem")
						.detail("LedgerLevel", i).detail("LedgerValue", it->second.ledger[i]);
				}
			}

			std::set<RelocateData, std::greater<RelocateData>> queuedRelocationsMatch;
			for(auto it = queue.begin(); it != queue.end(); ++it)
				queuedRelocationsMatch.insert( it->second.begin(), it->second.end() );
			ASSERT( queuedRelocations == queuedRelocationsMatch.size() + fetchingSourcesQueue.size() );

			int testActive = 0;
			for(auto it = priority_relocations.begin(); it != priority_relocations.end(); ++it )
				testActive += it->second;
			ASSERT( activeRelocations + queuedRelocations == testActive );
		}
	}

	ACTOR Future<Void> getSourceServersForRange( Database cx, RelocateData input, PromiseStream<RelocateData> output, Reference<FlowLock> fetchLock ) {
		state std::set<UID> servers;
		state Transaction tr(cx);

		// FIXME: is the merge case needed
		if( input.priority == SERVER_KNOBS->PRIORITY_MERGE_SHARD ) {
			wait( delay( 0.5, decrementPriority(decrementPriority(TaskPriority::DataDistribution )) ) );
		} else {
			wait( delay( 0.0001, TaskPriority::DataDistributionLaunch ) );
		}

		wait( fetchLock->take( TaskPriority::DataDistributionLaunch ) );
		state FlowLock::Releaser releaser( *fetchLock );

		loop {
			servers.clear();
			tr.setOption( FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE );
			try {
				Standalone<RangeResultRef> keyServersEntries  = wait(
					tr.getRange( lastLessOrEqual( keyServersKey( input.keys.begin ) ),
						firstGreaterOrEqual( keyServersKey( input.keys.end ) ), SERVER_KNOBS->DD_QUEUE_MAX_KEY_SERVERS ) );

				if(keyServersEntries.size() < SERVER_KNOBS->DD_QUEUE_MAX_KEY_SERVERS) {
					for( int shard = 0; shard < keyServersEntries.size(); shard++ ) {
						vector<UID> src, dest;
						decodeKeyServersValue( keyServersEntries[shard].value, src, dest );
						ASSERT( src.size() );
						for( int i = 0; i < src.size(); i++ ) {
							servers.insert( src[i] );
						}
						if(shard == 0) {
							input.completeSources = src;
						} else {
							for(int i = 0; i < input.completeSources.size(); i++) {
								if(std::find(src.begin(), src.end(), input.completeSources[i]) == src.end()) {
									swapAndPop(&input.completeSources, i--);
								}
							}
						}
					}

					ASSERT(servers.size() > 0);
				}

				// If the size of keyServerEntries is large, then just assume we are using all storage servers
				// Why the size can be large?
				// When a shard is inflight and DD crashes, some destination servers may have already got the data.
				// The new DD will treat the destination servers as source servers. So the size can be large.
				else {
					Standalone<RangeResultRef> serverList = wait( tr.getRange( serverListKeys, CLIENT_KNOBS->TOO_MANY ) );
					ASSERT( !serverList.more && serverList.size() < CLIENT_KNOBS->TOO_MANY );

					for(auto s = serverList.begin(); s != serverList.end(); ++s)
						servers.insert(decodeServerListValue( s->value ).id());

					ASSERT(servers.size() > 0);
				}

				break;
			} catch( Error& e ) {
				wait( tr.onError(e) );
			}
		}

		input.src = std::vector<UID>( servers.begin(), servers.end() );
		output.send( input );
		return Void();
	}

	//This function cannot handle relocation requests which split a shard into three pieces
	void queueRelocation( RelocateShard rs, std::set<UID> &serversToLaunchFrom ) {
		//TraceEvent("QueueRelocationBegin").detail("Begin", rd.keys.begin).detail("End", rd.keys.end);

		// remove all items from both queues that are fully contained in the new relocation (i.e. will be overwritten)
		RelocateData rd(rs);
		bool hasHealthPriority = RelocateData::isHealthPriority( rd.priority );
		bool hasBoundaryPriority = RelocateData::isBoundaryPriority( rd.priority );
		
		auto ranges = queueMap.intersectingRanges( rd.keys );
		for(auto r = ranges.begin(); r != ranges.end(); ++r ) {
			RelocateData& rrs = r->value();

			auto fetchingSourcesItr = fetchingSourcesQueue.find(rrs);
			bool foundActiveFetching = fetchingSourcesItr != fetchingSourcesQueue.end();
			std::set<RelocateData, std::greater<RelocateData>>* firstQueue;
			std::set<RelocateData, std::greater<RelocateData>>::iterator firstRelocationItr;
			bool foundActiveRelocation = false;

			if( !foundActiveFetching && rrs.src.size() ) {
				firstQueue = &queue[rrs.src[0]];
				firstRelocationItr = firstQueue->find( rrs );
				foundActiveRelocation = firstRelocationItr != firstQueue->end();
			}

			// If there is a queued job that wants data relocation which we are about to cancel/modify,
			//  make sure that we keep the relocation intent for the job that we queue up
			if( foundActiveFetching || foundActiveRelocation ) {
				rd.wantsNewServers |= rrs.wantsNewServers;
				rd.startTime = std::min( rd.startTime, rrs.startTime );
				if(!hasHealthPriority) {
					rd.healthPriority = std::max(rd.healthPriority, rrs.healthPriority);
				}
				if(!hasBoundaryPriority) {
					rd.boundaryPriority = std::max(rd.boundaryPriority, rrs.boundaryPriority);
				}
				rd.priority = std::max(rd.priority, std::max(rd.boundaryPriority, rd.healthPriority));
			}

			if( rd.keys.contains( rrs.keys ) ) {
				if(foundActiveFetching)
					fetchingSourcesQueue.erase( fetchingSourcesItr );
				else if(foundActiveRelocation) {
					firstQueue->erase( firstRelocationItr );
					for( int i = 1; i < rrs.src.size(); i++ )
						queue[rrs.src[i]].erase( rrs );
				}
			}

			if( foundActiveFetching || foundActiveRelocation ) {
				serversToLaunchFrom.insert( rrs.src.begin(), rrs.src.end() );
				/*TraceEvent(rrs.interval.end(), mi.id()).detail("Result","Cancelled")
					.detail("WasFetching", foundActiveFetching).detail("Contained", rd.keys.contains( rrs.keys ));*/
				queuedRelocations--;
				finishRelocation(rrs.priority, rrs.healthPriority);
			}
		}

		// determine the final state of the relocations map
		auto affectedQueuedItems = queueMap.getAffectedRangesAfterInsertion( rd.keys, rd );

		// put the new request into the global map of requests (modifies the ranges already present)
		queueMap.insert( rd.keys, rd );

		// cancel all the getSourceServers actors that intersect the new range that we will be getting
		getSourceActors.cancel( KeyRangeRef( affectedQueuedItems.front().begin, affectedQueuedItems.back().end ) );

		// update fetchingSourcesQueue and the per-server queue based on truncated ranges after insertion, (re-)launch getSourceServers
		auto queueMapItr = queueMap.rangeContaining(affectedQueuedItems[0].begin);
		for(int r = 0; r < affectedQueuedItems.size(); ++r, ++queueMapItr) {
			//ASSERT(queueMapItr->value() == queueMap.rangeContaining(affectedQueuedItems[r].begin)->value());
			RelocateData& rrs = queueMapItr->value();

			if( rrs.src.size() == 0 && ( rrs.keys == rd.keys || fetchingSourcesQueue.erase(rrs) > 0 ) ) {
				rrs.keys = affectedQueuedItems[r];

				rrs.interval = TraceInterval("QueuedRelocation");
				/*TraceEvent(rrs.interval.begin(), distributorId);
				  .detail("KeyBegin", rrs.keys.begin).detail("KeyEnd", rrs.keys.end)
					.detail("Priority", rrs.priority).detail("WantsNewServers", rrs.wantsNewServers);*/
				queuedRelocations++;
				startRelocation(rrs.priority, rrs.healthPriority);

				fetchingSourcesQueue.insert( rrs );
				getSourceActors.insert( rrs.keys, getSourceServersForRange( cx, rrs, fetchSourceServersComplete, fetchSourceLock ) );
			} else {
				RelocateData newData( rrs );
				newData.keys = affectedQueuedItems[r];
				ASSERT( rrs.src.size() || rrs.startTime == -1 );

				bool foundActiveRelocation = false;
				for( int i = 0; i < rrs.src.size(); i++ ) {
					auto& serverQueue = queue[rrs.src[i]];

					if( serverQueue.erase(rrs) > 0 ) {
						if( !foundActiveRelocation ) {
							newData.interval = TraceInterval("QueuedRelocation");
							/*TraceEvent(newData.interval.begin(), distributorId);
							  .detail("KeyBegin", newData.keys.begin).detail("KeyEnd", newData.keys.end)
								.detail("Priority", newData.priority).detail("WantsNewServers", newData.wantsNewServers);*/
							queuedRelocations++;
							startRelocation(newData.priority, newData.healthPriority);
							foundActiveRelocation = true;
						}

						serverQueue.insert( newData );
					}
					else
						break;
				}

				// We update the keys of a relocation even if it is "dead" since it helps validate()
				rrs.keys = affectedQueuedItems[r];
				rrs.interval = newData.interval;
			}
		}

		/*TraceEvent("ReceivedRelocateShard", distributorId)
		  .detail("KeyBegin", rd.keys.begin)
		  .detail("KeyEnd", rd.keys.end)
			.detail("Priority", rd.priority)
			.detail("AffectedRanges", affectedQueuedItems.size()); */
	}

	void completeSourceFetch( const RelocateData& results ) {
		ASSERT( fetchingSourcesQueue.count( results ) );

		//logRelocation( results, "GotSourceServers" );

		fetchingSourcesQueue.erase( results );
		queueMap.insert( results.keys, results );
		for( int i = 0; i < results.src.size(); i++ ) {
			queue[results.src[i]].insert( results );
		}
	}

	void logRelocation( const RelocateData& rd, const char *title ) {
		std::string busyString;
		for(int i = 0; i < rd.src.size() && i < teamSize * 2; i++)
			busyString += describe(rd.src[i]) + " - (" + busymap[ rd.src[i] ].toString() + "); ";

		TraceEvent(title, distributorId)
			.detail("KeyBegin", rd.keys.begin)
			.detail("KeyEnd", rd.keys.end)
			.detail("Priority", rd.priority)
			.detail("WorkFactor", rd.workFactor)
			.detail("SourceServerCount", rd.src.size())
			.detail("SourceServers", describe(rd.src, teamSize * 2))
			.detail("SourceBusyness", busyString);
	}

	void launchQueuedWork( KeyRange keys ) {
		//combine all queued work in the key range and check to see if there is anything to launch
		std::set<RelocateData, std::greater<RelocateData>> combined;
		auto f = queueMap.intersectingRanges( keys );
		for(auto it = f.begin(); it != f.end(); ++it) {
			if( it->value().src.size() && queue[it->value().src[0]].count( it->value() ) )
				combined.insert( it->value() );
		}
		launchQueuedWork( combined );
	}

	void launchQueuedWork( std::set<UID> serversToLaunchFrom ) {
		//combine all work from the source servers to see if there is anything new to launch
		std::set<RelocateData, std::greater<RelocateData>> combined;
		for( auto id : serversToLaunchFrom ) {
			auto& queuedWork = queue[id];
			auto it = queuedWork.begin();
			for( int j = 0; j < teamSize && it != queuedWork.end(); j++) {
				combined.insert( *it );
				++it;
			}
		}
		launchQueuedWork( combined );
	}

	void launchQueuedWork( RelocateData launchData ) {
		//check a single RelocateData to see if it can be launched
		std::set<RelocateData, std::greater<RelocateData>> combined;
		combined.insert( launchData );
		launchQueuedWork( combined );
	}

	void launchQueuedWork( std::set<RelocateData, std::greater<RelocateData>> combined ) {
		int startedHere = 0;
		double startTime = now();
		// kick off relocators from items in the queue as need be
		std::set<RelocateData, std::greater<RelocateData>>::iterator it = combined.begin();
		for(; it != combined.end(); it++ ) {
			RelocateData rd( *it );

			bool overlappingInFlight = false;
			auto intersectingInFlight = inFlight.intersectingRanges( rd.keys );
			for(auto it = intersectingInFlight.begin(); it != intersectingInFlight.end(); ++it) {
				if (fetchKeysComplete.count(it->value()) && inFlightActors.liveActorAt(it->range().begin) &&
				    !rd.keys.contains(it->range()) && it->value().priority >= rd.priority &&
				    rd.healthPriority < SERVER_KNOBS->PRIORITY_TEAM_UNHEALTHY) {
					/*TraceEvent("OverlappingInFlight", distributorId)
						.detail("KeyBegin", it->value().keys.begin)
						.detail("KeyEnd", it->value().keys.end)
						.detail("Priority", it->value().priority);*/
					overlappingInFlight = true;
					break;
				}
			}

			if( overlappingInFlight ) {
				//logRelocation( rd, "SkippingOverlappingInFlight" );
				continue;
			}

			// Because the busyness of a server is decreased when a superseding relocation is issued, we
			//  need to consider what the busyness of a server WOULD be if
			auto containedRanges = inFlight.containedRanges( rd.keys );
			std::vector<RelocateData> cancellableRelocations;
			for(auto it = containedRanges.begin(); it != containedRanges.end(); ++it) {
				if( inFlightActors.liveActorAt( it->range().begin ) ) {
					cancellableRelocations.push_back( it->value() );
				}
			}

			// Data movement avoids overloading source servers in moving data.
			// SOMEDAY: the list of source servers may be outdated since they were fetched when the work was put in the queue
			// FIXME: we need spare capacity even when we're just going to be cancelling work via TEAM_HEALTHY
			if( !canLaunch( rd, teamSize, singleRegionTeamSize, busymap, cancellableRelocations ) ) {
				//logRelocation( rd, "SkippingQueuedRelocation" );
				continue;
			}

			// From now on, the source servers for the RelocateData rd have enough resource to move the data away,
			// because they do not have too much inflight data movement.

			//logRelocation( rd, "LaunchingRelocation" );

			//TraceEvent(rd.interval.end(), distributorId).detail("Result","Success");
			queuedRelocations--;
			finishRelocation(rd.priority, rd.healthPriority);

			// now we are launching: remove this entry from the queue of all the src servers
			for( int i = 0; i < rd.src.size(); i++ ) {
				ASSERT( queue[rd.src[i]].erase(rd) );
			}

			// If there is a job in flight that wants data relocation which we are about to cancel/modify,
			//     make sure that we keep the relocation intent for the job that we launch
			auto f = inFlight.intersectingRanges( rd.keys );
			for(auto it = f.begin(); it != f.end(); ++it) {
				if( inFlightActors.liveActorAt( it->range().begin ) ) {
					rd.wantsNewServers |= it->value().wantsNewServers;
				}
			}
			startedHere++;

			// update both inFlightActors and inFlight key range maps, cancelling deleted RelocateShards
			vector<KeyRange> ranges;
			inFlightActors.getRangesAffectedByInsertion( rd.keys, ranges );
			inFlightActors.cancel( KeyRangeRef( ranges.front().begin, ranges.back().end ) );
			inFlight.insert( rd.keys, rd );
			for(int r=0; r<ranges.size(); r++) {
				RelocateData& rrs = inFlight.rangeContaining(ranges[r].begin)->value();
				rrs.keys = ranges[r];

				launch( rrs, busymap, singleRegionTeamSize );
				activeRelocations++;
				startRelocation(rrs.priority, rrs.healthPriority);
				inFlightActors.insert( rrs.keys, dataDistributionRelocator( this, rrs ) );
			}

			//logRelocation( rd, "LaunchedRelocation" );
		}
		if( now() - startTime > .001 && deterministicRandom()->random01()<0.001 )
			TraceEvent(SevWarnAlways, "LaunchingQueueSlowx1000").detail("Elapsed", now() - startTime );

		/*if( startedHere > 0 ) {
			TraceEvent("StartedDDRelocators", distributorId)
				.detail("QueueSize", queuedRelocations)
				.detail("StartedHere", startedHere)
				.detail("ActiveRelocations", activeRelocations);
		} */

		validate();
	}
};

extern bool noUnseed;

// This actor relocates the specified keys to a good place.
// The inFlightActor key range map stores the actor for each RelocateData
ACTOR Future<Void> dataDistributionRelocator( DDQueueData *self, RelocateData rd )
{
	state Promise<Void> errorOut( self->error );
	state TraceInterval relocateShardInterval("RelocateShard");
	state PromiseStream<RelocateData> dataTransferComplete( self->dataTransferComplete );
	state PromiseStream<RelocateData> relocationComplete( self->relocationComplete );
	state bool signalledTransferComplete = false;
	state UID distributorId = self->distributorId;
	state ParallelTCInfo healthyDestinations;

	state bool anyHealthy = false;
	state bool allHealthy = true;
	state bool anyWithSource = false;
	state std::vector<std::pair<Reference<IDataDistributionTeam>,bool>> bestTeams;
	state double startTime = now();
	state std::vector<UID> destIds;

	try {
		if(now() - self->lastInterval < 1.0) {
			relocateShardInterval.severity = SevDebug;
			self->suppressIntervals++;
		}

		TraceEvent(relocateShardInterval.begin(), distributorId)
			.detail("KeyBegin", rd.keys.begin).detail("KeyEnd", rd.keys.end)
			.detail("Priority", rd.priority).detail("RelocationID", relocateShardInterval.pairID).detail("SuppressedEventCount", self->suppressIntervals);

		if(relocateShardInterval.severity != SevDebug) {
			self->lastInterval = now();
			self->suppressIntervals = 0;
		}

		state StorageMetrics metrics = wait( brokenPromiseToNever( self->getShardMetrics.getReply( GetMetricsRequest( rd.keys ) ) ) );

		ASSERT( rd.src.size() );
		loop {
			state int stuckCount = 0;
			// state int bestTeamStuckThreshold = 50;
			loop {
				state int tciIndex = 0;
				state bool foundTeams = true;
				anyHealthy = false;
				allHealthy = true;
				anyWithSource = false;
				bestTeams.clear();
				while( tciIndex < self->teamCollections.size() ) {
					double inflightPenalty = SERVER_KNOBS->INFLIGHT_PENALTY_HEALTHY;
					if(rd.healthPriority == SERVER_KNOBS->PRIORITY_TEAM_UNHEALTHY || rd.healthPriority == SERVER_KNOBS->PRIORITY_TEAM_2_LEFT) inflightPenalty = SERVER_KNOBS->INFLIGHT_PENALTY_UNHEALTHY;
					if(rd.healthPriority == SERVER_KNOBS->PRIORITY_POPULATE_REGION || rd.healthPriority == SERVER_KNOBS->PRIORITY_TEAM_1_LEFT || rd.healthPriority == SERVER_KNOBS->PRIORITY_TEAM_0_LEFT) inflightPenalty = SERVER_KNOBS->INFLIGHT_PENALTY_ONE_LEFT;

					auto req = GetTeamRequest(rd.wantsNewServers, rd.priority == SERVER_KNOBS->PRIORITY_REBALANCE_UNDERUTILIZED_TEAM, true, false, inflightPenalty);
					req.src = rd.src;
					req.completeSources = rd.completeSources;
					std::pair<Optional<Reference<IDataDistributionTeam>>,bool> bestTeam = wait(brokenPromiseToNever(self->teamCollections[tciIndex].getTeam.getReply(req)));
					// If a DC has no healthy team, we stop checking the other DCs until
					// the unhealthy DC is healthy again or is excluded.
					if(!bestTeam.first.present()) {
						foundTeams = false;
						break;
					}
					if(!bestTeam.first.get()->isHealthy()) {
						allHealthy = false;
					} else {
						anyHealthy = true;
					}

					if(bestTeam.second) {
						anyWithSource = true;
					}
					
					bestTeams.push_back(std::make_pair(bestTeam.first.get(), bestTeam.second));
					tciIndex++;
				}
				if (foundTeams && anyHealthy) {
					break;
				}

				TEST(true); //did not find a healthy destination team on the first attempt
				stuckCount++;
				TraceEvent(stuckCount > 50 ? SevWarnAlways : SevWarn, "BestTeamStuck", distributorId)
				    .suppressFor(1.0)
				    .detail("Count", stuckCount)
				    .detail("TeamCollectionId", tciIndex)
				    .detail("NumOfTeamCollections", self->teamCollections.size());
				wait( delay( SERVER_KNOBS->BEST_TEAM_STUCK_DELAY, TaskPriority::DataDistributionLaunch ) );
			}

			destIds.clear();
			state std::vector<UID> healthyIds;
			state std::vector<UID> extraIds;
			state std::vector<ShardsAffectedByTeamFailure::Team> destinationTeams;

			for(int i = 0; i < bestTeams.size(); i++) {
				auto& serverIds = bestTeams[i].first->getServerIDs();
				destinationTeams.push_back(ShardsAffectedByTeamFailure::Team(serverIds, i == 0));

				if (allHealthy && anyWithSource && !bestTeams[i].second) {
					// When all teams in bestTeams[i] do not hold the shard
					// We randomly choose a server in bestTeams[i] as the shard's destination and
					// move the shard to the randomly chosen server (in the remote DC), which will later
					// propogate its data to the servers in the same team. This saves data movement bandwidth across DC
					int idx = deterministicRandom()->randomInt(0, serverIds.size());
					destIds.push_back(serverIds[idx]);
					healthyIds.push_back(serverIds[idx]);
					for(int j = 0; j < serverIds.size(); j++) {
						if(j != idx) {
							extraIds.push_back(serverIds[j]);
						}
					}
					healthyDestinations.addTeam(bestTeams[i].first);
				} else {
					destIds.insert(destIds.end(), serverIds.begin(), serverIds.end());
					if(bestTeams[i].first->isHealthy()) {
						healthyIds.insert(healthyIds.end(), serverIds.begin(), serverIds.end());
						healthyDestinations.addTeam(bestTeams[i].first);
					}
				}
			}

			// Sanity check
			state int totalIds = 0;
			for (auto& destTeam : destinationTeams) {
				totalIds += destTeam.servers.size();
			}
			if (totalIds != self->teamSize) {
				TraceEvent(SevWarn, "IncorrectDestTeamSize")
				    .suppressFor(1.0)
				    .detail("ExpectedTeamSize", self->teamSize)
				    .detail("DestTeamSize", totalIds);
			}

			self->shardsAffectedByTeamFailure->moveShard(rd.keys, destinationTeams);

			//FIXME: do not add data in flight to servers that were already in the src.
			healthyDestinations.addDataInFlightToTeam(+metrics.bytes);

			if (SERVER_KNOBS->DD_ENABLE_VERBOSE_TRACING) {
				// StorageMetrics is the rd shard's metrics, e.g., bytes and write bandwidth
				TraceEvent(SevInfo, "RelocateShardDecision", distributorId)
				    .detail("PairId", relocateShardInterval.pairID)
				    .detail("Priority", rd.priority)
				    .detail("KeyBegin", rd.keys.begin)
				    .detail("KeyEnd", rd.keys.end)
				    .detail("StorageMetrics", metrics.toString())
				    .detail("SourceServers", describe(rd.src))
				    .detail("DestinationTeam", describe(destIds))
				    .detail("ExtraIds", describe(extraIds));
			} else {
				TraceEvent(relocateShardInterval.severity, "RelocateShardHasDestination", distributorId)
				    .detail("PairId", relocateShardInterval.pairID)
				    .detail("KeyBegin", rd.keys.begin)
				    .detail("KeyEnd", rd.keys.end)
				    .detail("SourceServers", describe(rd.src))
				    .detail("DestinationTeam", describe(destIds))
				    .detail("ExtraIds", describe(extraIds));
			}

			state Error error = success();
			state Promise<Void> dataMovementComplete;
			// Move keys from source to destination by chaning the serverKeyList and keyServerList system keys
			state Future<Void> doMoveKeys = moveKeys(self->cx, rd.keys, destIds, healthyIds, self->lock, dataMovementComplete, &self->startMoveKeysParallelismLock, &self->finishMoveKeysParallelismLock, self->teamCollections.size() > 1, relocateShardInterval.pairID );
			state Future<Void> pollHealth = signalledTransferComplete ? Never() : delay( SERVER_KNOBS->HEALTH_POLL_TIME, TaskPriority::DataDistributionLaunch );
			try {
				loop {
					choose {
						when( wait( doMoveKeys ) ) {
							if(extraIds.size()) {
								destIds.insert(destIds.end(), extraIds.begin(), extraIds.end());
								healthyIds.insert(healthyIds.end(), extraIds.begin(), extraIds.end());
								extraIds.clear();
								ASSERT(totalIds == destIds.size()); // Sanity check the destIDs before we move keys
								doMoveKeys = moveKeys(self->cx, rd.keys, destIds, healthyIds, self->lock, Promise<Void>(), &self->startMoveKeysParallelismLock, &self->finishMoveKeysParallelismLock, self->teamCollections.size() > 1, relocateShardInterval.pairID );
							} else {
								self->fetchKeysComplete.insert( rd );
								break;
							}
						}
						when( wait( pollHealth ) ) {
							if (!healthyDestinations.isHealthy()) {
								if (!signalledTransferComplete) {
									signalledTransferComplete = true;
									self->dataTransferComplete.send(rd);
								}
							}
							pollHealth = signalledTransferComplete ? Never() : delay( SERVER_KNOBS->HEALTH_POLL_TIME, TaskPriority::DataDistributionLaunch );
						}
						when( wait( signalledTransferComplete ? Never() : dataMovementComplete.getFuture() ) ) {
							self->fetchKeysComplete.insert( rd );
							if( !signalledTransferComplete ) {
								signalledTransferComplete = true;
								self->dataTransferComplete.send( rd );
							}
						}
					}
				}
			} catch( Error& e ) {
				error = e;
			}

			//TraceEvent("RelocateShardFinished", distributorId).detail("RelocateId", relocateShardInterval.pairID);

			if( error.code() != error_code_move_to_removed_server ) {
				if( !error.code() ) {
					try {
						wait( healthyDestinations.updateStorageMetrics() ); //prevent a gap between the polling for an increase in storage metrics and decrementing data in flight
					} catch( Error& e ) {
						error = e;
					}
				}

				healthyDestinations.addDataInFlightToTeam( -metrics.bytes );

				// onFinished.send( rs );
				if( !error.code() ) {
					TraceEvent(relocateShardInterval.end(), distributorId).detail("Duration", now() - startTime).detail("Result","Success");
					if(now() - startTime > 600) {
						TraceEvent(SevWarnAlways, "RelocateShardTooLong").detail("Duration", now() - startTime).detail("Dest", describe(destIds)).detail("Src", describe(rd.src));
					}
					if(rd.keys.begin == keyServersPrefix) {
						TraceEvent("MovedKeyServerKeys").detail("Dest", describe(destIds)).trackLatest("MovedKeyServers");
					}

					if( !signalledTransferComplete ) {
						signalledTransferComplete = true;
						dataTransferComplete.send( rd );
					}

					self->bytesWritten += metrics.bytes;
					self->shardsAffectedByTeamFailure->finishMove(rd.keys);
					relocationComplete.send( rd );
					return Void();
				} else {
					throw error;
				}
			} else {
				TEST(true);  // move to removed server
				healthyDestinations.addDataInFlightToTeam( -metrics.bytes );
				wait( delay( SERVER_KNOBS->RETRY_RELOCATESHARD_DELAY, TaskPriority::DataDistributionLaunch ) );
			}
		}
	} catch (Error& e) {
		TraceEvent(relocateShardInterval.end(), distributorId).error(e, true).detail("Duration", now() - startTime);
		if(now() - startTime > 600) {
			TraceEvent(SevWarnAlways, "RelocateShardTooLong").error(e, true).detail("Duration", now() - startTime).detail("Dest", describe(destIds)).detail("Src", describe(rd.src));
		}
		if( !signalledTransferComplete )
			dataTransferComplete.send( rd );

		relocationComplete.send( rd );

		if( e.code() != error_code_actor_cancelled ) {
			if (errorOut.canBeSet()) {
				errorOut.sendError(e);
			}
		}
		throw;
	}
}

// Move a random shard of sourceTeam's to destTeam if sourceTeam has much more data than destTeam
ACTOR Future<bool> rebalanceTeams( DDQueueData* self, int priority, Reference<IDataDistributionTeam> sourceTeam, 
                                   Reference<IDataDistributionTeam> destTeam, bool primary, TraceEvent *traceEvent ) {
	if(g_network->isSimulated() && g_simulator.speedUpSimulation) {
		traceEvent->detail("CancelingDueToSimulationSpeedup", true);
		return false;
	}

	Promise<int64_t> req;
	self->getAverageShardBytes.send( req );

	state int64_t averageShardBytes = wait(req.getFuture());
	state std::vector<KeyRange> shards = self->shardsAffectedByTeamFailure->getShardsFor( ShardsAffectedByTeamFailure::Team( sourceTeam->getServerIDs(), primary ) );

	traceEvent->detail("AverageShardBytes", averageShardBytes)
		.detail("ShardsInSource", shards.size());

	if( !shards.size() )
		return false;

	state KeyRange moveShard;
	state StorageMetrics metrics;
	state int retries = 0;
	while(retries < SERVER_KNOBS->REBALANCE_MAX_RETRIES) {
		state KeyRange testShard = deterministicRandom()->randomChoice( shards );
		StorageMetrics testMetrics = wait( brokenPromiseToNever( self->getShardMetrics.getReply(GetMetricsRequest(testShard)) ) );
		if(testMetrics.bytes > metrics.bytes) {
			moveShard = testShard;
			metrics = testMetrics;
			if(metrics.bytes > averageShardBytes) {
				break;
			}
		}
		retries++;
	}

	int64_t sourceBytes = sourceTeam->getLoadBytes(false);
	int64_t destBytes = destTeam->getLoadBytes();

	bool sourceAndDestTooSimilar = sourceBytes - destBytes <= 3 * std::max<int64_t>(SERVER_KNOBS->MIN_SHARD_BYTES, metrics.bytes);
	traceEvent->detail("SourceBytes", sourceBytes)
		.detail("DestBytes", destBytes)
		.detail("ShardBytes", metrics.bytes)
		.detail("SourceAndDestTooSimilar", sourceAndDestTooSimilar);

	if( sourceAndDestTooSimilar || metrics.bytes == 0 ) {
		return false;
	}

	//verify the shard is still in sabtf
	shards = self->shardsAffectedByTeamFailure->getShardsFor( ShardsAffectedByTeamFailure::Team( sourceTeam->getServerIDs(), primary ) );
	for( int i = 0; i < shards.size(); i++ ) {
		if( moveShard == shards[i] ) {
			traceEvent->detail("ShardStillPresent", true);
			self->output.send( RelocateShard( moveShard, priority ) );
			return true;
		}
	}

	traceEvent->detail("ShardStillPresent", false);
	return false;
}

ACTOR Future<Void> BgDDMountainChopper( DDQueueData* self, int teamCollectionIndex ) {
	state double rebalancePollingInterval = SERVER_KNOBS->BG_REBALANCE_POLLING_INTERVAL;
	state int resetCount = SERVER_KNOBS->DD_REBALANCE_RESET_AMOUNT;
	state Transaction tr(self->cx);
	state double lastRead = 0;
	state bool skipCurrentLoop = false;
	loop {
		state bool moved = false;
		state TraceEvent traceEvent("BgDDMountainChopper", self->distributorId);
		traceEvent.suppressFor(5.0)
			.detail("PollingInterval", rebalancePollingInterval);

		if(*self->lastLimited > 0) {
			traceEvent.detail("SecondsSinceLastLimited", now() - *self->lastLimited);
		}

		try {
			state Future<Void> delayF = delay(rebalancePollingInterval, TaskPriority::DataDistributionLaunch);
			if ((now() - lastRead) > SERVER_KNOBS->BG_REBALANCE_SWITCH_CHECK_INTERVAL) {
				tr.setOption(FDBTransactionOptions::LOCK_AWARE);
				Optional<Value> val = wait(tr.get(rebalanceDDIgnoreKey));
				lastRead = now();
				if (skipCurrentLoop && !val.present()) {
					// reset loop interval
					rebalancePollingInterval = SERVER_KNOBS->BG_REBALANCE_POLLING_INTERVAL;
				}
				skipCurrentLoop = val.present();
			}

			traceEvent.detail("Enabled", !skipCurrentLoop);

			wait(delayF);
			if (skipCurrentLoop) {
				// set loop interval to avoid busy wait here.
				rebalancePollingInterval =
				    std::max(rebalancePollingInterval, SERVER_KNOBS->BG_REBALANCE_SWITCH_CHECK_INTERVAL);
				continue;
			}

			traceEvent.detail("QueuedRelocations", self->priority_relocations[SERVER_KNOBS->PRIORITY_REBALANCE_OVERUTILIZED_TEAM]);
			if (self->priority_relocations[SERVER_KNOBS->PRIORITY_REBALANCE_OVERUTILIZED_TEAM] <
			    SERVER_KNOBS->DD_REBALANCE_PARALLELISM) {
				state std::pair<Optional<Reference<IDataDistributionTeam>>,bool> randomTeam = wait(brokenPromiseToNever(
				    self->teamCollections[teamCollectionIndex].getTeam.getReply(GetTeamRequest(true, false, true, false))));
				
				traceEvent.detail("DestTeam", printable(randomTeam.first.map<std::string>([](const Reference<IDataDistributionTeam>& team){
					return team->getDesc();
				})));

				if (randomTeam.first.present()) {
					state std::pair<Optional<Reference<IDataDistributionTeam>>,bool> loadedTeam =
						wait(brokenPromiseToNever(self->teamCollections[teamCollectionIndex].getTeam.getReply(
							GetTeamRequest(true, true, false, true))));

					traceEvent.detail("SourceTeam", printable(loadedTeam.first.map<std::string>([](const Reference<IDataDistributionTeam>& team){
						return team->getDesc();
					})));

					if (loadedTeam.first.present()) {
						bool _moved =
							wait(rebalanceTeams(self, SERVER_KNOBS->PRIORITY_REBALANCE_OVERUTILIZED_TEAM, loadedTeam.first.get(),
												randomTeam.first.get(), teamCollectionIndex == 0, &traceEvent));
						moved = _moved;	
						if (moved) {
							resetCount = 0;
						} else {
							resetCount++;
						}
					}
				}
			}

			if (now() - (*self->lastLimited) < SERVER_KNOBS->BG_DD_SATURATION_DELAY) {
				rebalancePollingInterval = std::min(SERVER_KNOBS->BG_DD_MAX_WAIT,
				                                    rebalancePollingInterval * SERVER_KNOBS->BG_DD_INCREASE_RATE);
			} else {
				rebalancePollingInterval = std::max(SERVER_KNOBS->BG_DD_MIN_WAIT,
				                                    rebalancePollingInterval / SERVER_KNOBS->BG_DD_DECREASE_RATE);
			}

			if (resetCount >= SERVER_KNOBS->DD_REBALANCE_RESET_AMOUNT &&
			    rebalancePollingInterval < SERVER_KNOBS->BG_REBALANCE_POLLING_INTERVAL) {
				rebalancePollingInterval = SERVER_KNOBS->BG_REBALANCE_POLLING_INTERVAL;
				resetCount = SERVER_KNOBS->DD_REBALANCE_RESET_AMOUNT;
			}

			traceEvent.detail("ResetCount", resetCount);
			tr.reset();
		} catch (Error& e) {
			traceEvent.error(e, true); // Log actor_cancelled because it's not legal to suppress an event that's initialized
			wait(tr.onError(e));
		}

		traceEvent.detail("Moved", moved);
		traceEvent.log();
	}
}

ACTOR Future<Void> BgDDValleyFiller( DDQueueData* self, int teamCollectionIndex) {
	state double rebalancePollingInterval = SERVER_KNOBS->BG_REBALANCE_POLLING_INTERVAL;
	state int resetCount = SERVER_KNOBS->DD_REBALANCE_RESET_AMOUNT;
	state Transaction tr(self->cx);
	state double lastRead = 0;
	state bool skipCurrentLoop = false;
	loop {
		state bool moved = false;
		state TraceEvent traceEvent("BgDDValleyFiller", self->distributorId);
		traceEvent.suppressFor(5.0)
			.detail("PollingInterval", rebalancePollingInterval);

		if(*self->lastLimited > 0) {	
			traceEvent.detail("SecondsSinceLastLimited", now() - *self->lastLimited);
		}

		try {
			state Future<Void> delayF = delay(rebalancePollingInterval, TaskPriority::DataDistributionLaunch);
			if ((now() - lastRead) > SERVER_KNOBS->BG_REBALANCE_SWITCH_CHECK_INTERVAL) {
				tr.setOption(FDBTransactionOptions::LOCK_AWARE);
				Optional<Value> val = wait(tr.get(rebalanceDDIgnoreKey));
				lastRead = now();
				if (skipCurrentLoop && !val.present()) {
					// reset loop interval
					rebalancePollingInterval = SERVER_KNOBS->BG_REBALANCE_POLLING_INTERVAL;
				}
				skipCurrentLoop = val.present();
			}

			traceEvent.detail("Enabled", !skipCurrentLoop);

			wait(delayF);
			if (skipCurrentLoop) {
				// set loop interval to avoid busy wait here.
				rebalancePollingInterval =
				    std::max(rebalancePollingInterval, SERVER_KNOBS->BG_REBALANCE_SWITCH_CHECK_INTERVAL);
				continue;
			}

			traceEvent.detail("QueuedRelocations", self->priority_relocations[SERVER_KNOBS->PRIORITY_REBALANCE_UNDERUTILIZED_TEAM]);
			if (self->priority_relocations[SERVER_KNOBS->PRIORITY_REBALANCE_UNDERUTILIZED_TEAM] <
			    SERVER_KNOBS->DD_REBALANCE_PARALLELISM) {
				state std::pair<Optional<Reference<IDataDistributionTeam>>,bool> randomTeam = wait(brokenPromiseToNever(
				    self->teamCollections[teamCollectionIndex].getTeam.getReply(GetTeamRequest(true, false, false, true))));

				traceEvent.detail("SourceTeam", printable(randomTeam.first.map<std::string>([](const Reference<IDataDistributionTeam>& team){
					return team->getDesc();
				})));

				if (randomTeam.first.present()) {
					state std::pair<Optional<Reference<IDataDistributionTeam>>,bool> unloadedTeam = wait(brokenPromiseToNever(
					    self->teamCollections[teamCollectionIndex].getTeam.getReply(GetTeamRequest(true, true, true, false))));

					traceEvent.detail("DestTeam", printable(unloadedTeam.first.map<std::string>([](const Reference<IDataDistributionTeam>& team){
						return team->getDesc();
					})));

					if (unloadedTeam.first.present()) {
						bool _moved =
							wait(rebalanceTeams(self, SERVER_KNOBS->PRIORITY_REBALANCE_UNDERUTILIZED_TEAM, randomTeam.first.get(),
												unloadedTeam.first.get(), teamCollectionIndex == 0, &traceEvent));
						moved = _moved;
						if (moved) {
							resetCount = 0;
						} else {
							resetCount++;
						}
					}
				}
			}

			if (now() - (*self->lastLimited) < SERVER_KNOBS->BG_DD_SATURATION_DELAY) {
				rebalancePollingInterval = std::min(SERVER_KNOBS->BG_DD_MAX_WAIT,
				                                    rebalancePollingInterval * SERVER_KNOBS->BG_DD_INCREASE_RATE);
			} else {
				rebalancePollingInterval = std::max(SERVER_KNOBS->BG_DD_MIN_WAIT,
				                                    rebalancePollingInterval / SERVER_KNOBS->BG_DD_DECREASE_RATE);
			}

			if (resetCount >= SERVER_KNOBS->DD_REBALANCE_RESET_AMOUNT &&
			    rebalancePollingInterval < SERVER_KNOBS->BG_REBALANCE_POLLING_INTERVAL) {
				rebalancePollingInterval = SERVER_KNOBS->BG_REBALANCE_POLLING_INTERVAL;
				resetCount = SERVER_KNOBS->DD_REBALANCE_RESET_AMOUNT;
			}

			traceEvent.detail("ResetCount", resetCount);
			tr.reset();
		} catch (Error& e) {
			traceEvent.error(e, true); // Log actor_cancelled because it's not legal to suppress an event that's initialized
			wait(tr.onError(e));
		}

		traceEvent.detail("Moved", moved);
		traceEvent.log();
	}
}

ACTOR Future<Void> dataDistributionQueue(
	Database cx,
	PromiseStream<RelocateShard> output,
	FutureStream<RelocateShard> input,
	PromiseStream<GetMetricsRequest> getShardMetrics,
	Reference<AsyncVar<bool>> processingUnhealthy,
	std::vector<TeamCollectionInterface> teamCollections,
	Reference<ShardsAffectedByTeamFailure> shardsAffectedByTeamFailure,
	MoveKeysLock lock,
	PromiseStream<Promise<int64_t>> getAverageShardBytes,
	UID distributorId,
	int teamSize,
	int singleRegionTeamSize,
	double* lastLimited)
{
	state DDQueueData self( distributorId, lock, cx, teamCollections, shardsAffectedByTeamFailure, getAverageShardBytes, teamSize, singleRegionTeamSize, output, input, getShardMetrics, lastLimited );
	state std::set<UID> serversToLaunchFrom;
	state KeyRange keysToLaunchFrom;
	state RelocateData launchData;
	state Future<Void> recordMetrics = delay(SERVER_KNOBS->DD_QUEUE_LOGGING_INTERVAL);

	state vector<Future<Void>> balancingFutures;

	state ActorCollectionNoErrors actors;
	state PromiseStream<KeyRange> rangesComplete;
	state Future<Void> launchQueuedWorkTimeout = Never();

	for (int i = 0; i < teamCollections.size(); i++) {
		balancingFutures.push_back(BgDDMountainChopper(&self, i));
		balancingFutures.push_back(BgDDValleyFiller(&self, i));
	}
	balancingFutures.push_back(delayedAsyncVar(self.rawProcessingUnhealthy, processingUnhealthy, 0));

	try {
		loop {
			self.validate();

			// For the given servers that caused us to go around the loop, find the next item(s) that can be launched.
			if( launchData.startTime != -1 ) {
				// Launch dataDistributionRelocator actor to relocate the launchData
				self.launchQueuedWork( launchData );
				launchData = RelocateData();
			}
			else if( !keysToLaunchFrom.empty() ) {
				self.launchQueuedWork( keysToLaunchFrom );
				keysToLaunchFrom = KeyRangeRef();
			}

			ASSERT( launchData.startTime == -1 && keysToLaunchFrom.empty() );

			choose {
				when ( RelocateShard rs = waitNext( self.input ) ) {
					bool wasEmpty = serversToLaunchFrom.empty();
					self.queueRelocation( rs, serversToLaunchFrom );
					if(wasEmpty && !serversToLaunchFrom.empty())
						launchQueuedWorkTimeout = delay(0, TaskPriority::DataDistributionLaunch);
				}
				when ( wait(launchQueuedWorkTimeout) ) {
					self.launchQueuedWork( serversToLaunchFrom );
					serversToLaunchFrom = std::set<UID>();
					launchQueuedWorkTimeout = Never();
				}
				when ( RelocateData results = waitNext( self.fetchSourceServersComplete.getFuture() ) ) {
					// This when is triggered by queueRelocation() which is triggered by sending self.input
					self.completeSourceFetch( results );
					launchData = results;
				}
				when ( RelocateData done = waitNext( self.dataTransferComplete.getFuture() ) ) {
					complete( done, self.busymap );
					if(serversToLaunchFrom.empty() && !done.src.empty())
						launchQueuedWorkTimeout = delay(0, TaskPriority::DataDistributionLaunch);
					serversToLaunchFrom.insert(done.src.begin(), done.src.end());
				}
				when ( RelocateData done = waitNext( self.relocationComplete.getFuture() ) ) {
					self.activeRelocations--;
					self.finishRelocation(done.priority, done.healthPriority);
					self.fetchKeysComplete.erase( done );
					//self.logRelocation( done, "ShardRelocatorDone" );
					actors.add( tag( delay(0, TaskPriority::DataDistributionLaunch), done.keys, rangesComplete ) );
					if( g_network->isSimulated() && debug_isCheckRelocationDuration() && now() - done.startTime > 60 ) {
						TraceEvent(SevWarnAlways, "RelocationDurationTooLong").detail("Duration", now() - done.startTime);
						debug_setCheckRelocationDuration(false);
					}
				}
				when ( KeyRange done = waitNext( rangesComplete.getFuture() ) ) {
					keysToLaunchFrom = done;
				}
				when ( wait( recordMetrics ) ) {
					Promise<int64_t> req;
					getAverageShardBytes.send( req );

					recordMetrics = delay(SERVER_KNOBS->DD_QUEUE_LOGGING_INTERVAL, TaskPriority::FlushTrace);

					int highestPriorityRelocation = 0;
					for( auto it = self.priority_relocations.begin(); it != self.priority_relocations.end(); ++it ) {
						if (it->second) {
							highestPriorityRelocation = std::max(highestPriorityRelocation, it->first);
						}
					}

					TraceEvent("MovingData", distributorId)
						.detail( "InFlight", self.activeRelocations )
						.detail( "InQueue", self.queuedRelocations )
						.detail( "AverageShardSize", req.getFuture().isReady() ? req.getFuture().get() : -1 )
						.detail( "UnhealthyRelocations", self.unhealthyRelocations )
						.detail( "HighestPriority", highestPriorityRelocation )
						.detail( "BytesWritten", self.bytesWritten )
						.detail( "PriorityRecoverMove", self.priority_relocations[SERVER_KNOBS->PRIORITY_RECOVER_MOVE] )
						.detail( "PriorityRebalanceUnderutilizedTeam", self.priority_relocations[SERVER_KNOBS->PRIORITY_REBALANCE_UNDERUTILIZED_TEAM] )
						.detail( "PriorityRebalanceOverutilizedTeam", self.priority_relocations[SERVER_KNOBS->PRIORITY_REBALANCE_OVERUTILIZED_TEAM] )
						.detail( "PriorityTeamHealthy", self.priority_relocations[SERVER_KNOBS->PRIORITY_TEAM_HEALTHY] )
						.detail( "PriorityTeamContainsUndesiredServer", self.priority_relocations[SERVER_KNOBS->PRIORITY_TEAM_CONTAINS_UNDESIRED_SERVER] )
						.detail( "PriorityTeamRedundant", self.priority_relocations[SERVER_KNOBS->PRIORITY_TEAM_REDUNDANT] )
						.detail( "PriorityMergeShard", self.priority_relocations[SERVER_KNOBS->PRIORITY_MERGE_SHARD] )
						.detail( "PriorityPopulateRegion", self.priority_relocations[SERVER_KNOBS->PRIORITY_POPULATE_REGION] )
						.detail( "PriorityTeamUnhealthy", self.priority_relocations[SERVER_KNOBS->PRIORITY_TEAM_UNHEALTHY] )
						.detail( "PriorityTeam2Left", self.priority_relocations[SERVER_KNOBS->PRIORITY_TEAM_2_LEFT] )
						.detail( "PriorityTeam1Left", self.priority_relocations[SERVER_KNOBS->PRIORITY_TEAM_1_LEFT] )
						.detail( "PriorityTeam0Left", self.priority_relocations[SERVER_KNOBS->PRIORITY_TEAM_0_LEFT] )
						.detail( "PrioritySplitShard", self.priority_relocations[SERVER_KNOBS->PRIORITY_SPLIT_SHARD] )
						.trackLatest( "MovingData" );
				}
				when ( wait( self.error.getFuture() ) ) {}  // Propagate errors from dataDistributionRelocator
				when ( wait(waitForAll( balancingFutures ) )) {}
			}
		}
	} catch (Error& e) {
		if (e.code() != error_code_broken_promise && // FIXME: Get rid of these broken_promise errors every time we are killed by the master dying
			e.code() != error_code_movekeys_conflict)
			TraceEvent(SevError, "DataDistributionQueueError", distributorId).error(e);
		throw e;
	}
}
