/*
 * DatabaseContext.h
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

#ifndef DatabaseContext_h
#define DatabaseContext_h
#pragma once

#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/KeyRangeMap.h"
#include "fdbclient/MasterProxyInterface.h"
#include "fdbclient/SpecialKeySpace.actor.h"
#include "fdbrpc/QueueModel.h"
#include "fdbrpc/MultiInterface.h"
#include "flow/TDMetric.actor.h"
#include "fdbclient/EventTypes.actor.h"
#include "fdbrpc/ContinuousSample.h"
#include "fdbrpc/Smoother.h"

class StorageServerInfo : public ReferencedInterface<StorageServerInterface> {
public:
	static Reference<StorageServerInfo> getInterface( DatabaseContext *cx, StorageServerInterface const& interf, LocalityData const& locality );
	void notifyContextDestroyed();

	virtual ~StorageServerInfo();
private:
	DatabaseContext *cx;
	StorageServerInfo( DatabaseContext *cx, StorageServerInterface const& interf, LocalityData const& locality ) : cx(cx), ReferencedInterface<StorageServerInterface>(interf, locality) {}
};

typedef MultiInterface<ReferencedInterface<StorageServerInterface>> LocationInfo;
typedef ModelInterface<MasterProxyInterface> ProxyInfo;

class ClientTagThrottleData : NonCopyable {
private:
	double tpsRate;
	double expiration;
	double lastCheck;
	bool rateSet = false;

	Smoother smoothRate;
	Smoother smoothReleased;

public:
	ClientTagThrottleData(ClientTagThrottleLimits const& limits)
	  : tpsRate(limits.tpsRate), expiration(limits.expiration), lastCheck(now()), smoothRate(CLIENT_KNOBS->TAG_THROTTLE_SMOOTHING_WINDOW), 
	    smoothReleased(CLIENT_KNOBS->TAG_THROTTLE_SMOOTHING_WINDOW) 
	{
		ASSERT(tpsRate >= 0);
		smoothRate.reset(tpsRate);
	}

	void update(ClientTagThrottleLimits const& limits) {
		ASSERT(limits.tpsRate >= 0);
		this->tpsRate = limits.tpsRate;

		if(!rateSet || expired()) {
			rateSet = true;
			smoothRate.reset(limits.tpsRate);
		}
		else {
			smoothRate.setTotal(limits.tpsRate);
		}

		expiration = limits.expiration;
	}

	void addReleased(int released) {
		smoothReleased.addDelta(released);
	}

	bool expired() {
		return expiration <= now();
	}

	void updateChecked() {
		lastCheck = now();
	}

	bool canRecheck() {
		return lastCheck < now() - CLIENT_KNOBS->TAG_THROTTLE_RECHECK_INTERVAL;
	}

	double throttleDuration() {
		if(expiration <= now()) {
			return 0.0;
		}

		double capacity = (smoothRate.smoothTotal() - smoothReleased.smoothRate()) * CLIENT_KNOBS->TAG_THROTTLE_SMOOTHING_WINDOW;
		if(capacity >= 1) {
			return 0.0;
		}

		if(tpsRate == 0) {
			return std::max(0.0, expiration - now());
		}

		return std::min(expiration - now(), capacity / tpsRate);
	}
};

class DatabaseContext : public ReferenceCounted<DatabaseContext>, public FastAllocated<DatabaseContext>, NonCopyable {
public:
	static DatabaseContext* allocateOnForeignThread() {
		return (DatabaseContext*)DatabaseContext::operator new(sizeof(DatabaseContext));
	}

	// For internal (fdbserver) use only
	static Database create(Reference<AsyncVar<ClientDBInfo>> clientInfo, Future<Void> clientInfoMonitor,
	                       LocalityData clientLocality, bool enableLocalityLoadBalance,
	                       TaskPriority taskID = TaskPriority::DefaultEndpoint, bool lockAware = false,
	                       int apiVersion = Database::API_VERSION_LATEST, bool switchable = false);

	~DatabaseContext();

	Database clone() const { return Database(new DatabaseContext( connectionFile, clientInfo, clientInfoMonitor, taskID, clientLocality, enableLocalityLoadBalance, lockAware, internal, apiVersion, switchable )); }

	std::pair<KeyRange,Reference<LocationInfo>> getCachedLocation( const KeyRef&, bool isBackward = false );
	bool getCachedLocations( const KeyRangeRef&, vector<std::pair<KeyRange,Reference<LocationInfo>>>&, int limit, bool reverse );
	Reference<LocationInfo> setCachedLocation( const KeyRangeRef&, const vector<struct StorageServerInterface>& );
	void invalidateCache( const KeyRef&, bool isBackward = false );
	void invalidateCache( const KeyRangeRef& );

	bool sampleReadTags();

	Reference<ProxyInfo> getMasterProxies(bool useProvisionalProxies, bool useGrvProxies = false);
	Future<Reference<ProxyInfo>> getMasterProxiesFuture(bool useProvisionalProxies, bool useGrvProxies = false);
	Future<Void> onMasterProxiesChanged();
	Future<HealthMetrics> getHealthMetrics(bool detailed);

	// Update the watch counter for the database
	void addWatch();
	void removeWatch();
	
	void setOption( FDBDatabaseOptions::Option option, Optional<StringRef> value );

	Error deferredError;
	bool lockAware;

	bool isError() {
		return deferredError.code() != invalid_error_code;	
	}

	void checkDeferredError() {
		if(isError()) {
			throw deferredError;
		}
	}

	int apiVersionAtLeast(int minVersion) { return apiVersion < 0 || apiVersion >= minVersion; }

	Future<Void> onConnected(); // Returns after a majority of coordination servers are available and have reported a leader. The cluster file therefore is valid, but the database might be unavailable.
	Reference<ClusterConnectionFile> getConnectionFile();

	// Switch the database to use the new connection file, and recreate all pending watches for committed transactions.
	//
	// Meant to be used as part of a 'hot standby' solution to switch to the standby. A correct switch will involve
	// advancing the version on the new cluster sufficiently far that any transaction begun with a read version from the
	// old cluster will fail to commit. Assuming the above version-advancing is done properly, a call to
	// switchConnectionFile guarantees that any read with a version from the old cluster will not be attempted on the
	// new cluster.
	Future<Void> switchConnectionFile(Reference<ClusterConnectionFile> standby);
	Future<Void> connectionFileChanged();
	bool switchable = false;

//private: 
	explicit DatabaseContext( Reference<AsyncVar<Reference<ClusterConnectionFile>>> connectionFile, Reference<AsyncVar<ClientDBInfo>> clientDBInfo,
		Future<Void> clientInfoMonitor, TaskPriority taskID, LocalityData const& clientLocality, 
		bool enableLocalityLoadBalance, bool lockAware, bool internal = true, int apiVersion = Database::API_VERSION_LATEST, bool switchable = false );

	explicit DatabaseContext( const Error &err );

	void expireThrottles();

	// Key DB-specific information
	Reference<AsyncVar<Reference<ClusterConnectionFile>>> connectionFile;
	AsyncTrigger masterProxiesChangeTrigger;
	Future<Void> monitorMasterProxiesInfoChange;
	Reference<ProxyInfo> masterProxies;
	Reference<ProxyInfo> grvProxies;
	bool provisional;
	UID masterProxiesLastChange;
	LocalityData clientLocality;
	QueueModel queueModel;
	bool enableLocalityLoadBalance;

	struct VersionRequest {
		Promise<GetReadVersionReply> reply;
		TagSet tags;
		Optional<UID> debugID;

		VersionRequest(TagSet tags = TagSet(), Optional<UID> debugID = Optional<UID>()) : tags(tags), debugID(debugID) {}
	};

	// Transaction start request batching
	struct VersionBatcher {
		PromiseStream<VersionRequest> stream;
		Future<Void> actor;
	};
	std::map<uint32_t, VersionBatcher> versionBatcher;

	AsyncTrigger connectionFileChangedTrigger;

	// Disallow any reads at a read version lower than minAcceptableReadVersion.  This way the client does not have to
	// trust that the read version (possibly set manually by the application) is actually from the correct cluster.
	// Updated everytime we get a GRV response
	Version minAcceptableReadVersion = std::numeric_limits<Version>::max();
	void validateVersion(Version);

	// Client status updater
	struct ClientStatusUpdater {
		std::vector< std::pair<std::string, BinaryWriter> > inStatusQ;
		std::vector< std::pair<std::string, BinaryWriter> > outStatusQ;
		Future<Void> actor;
	};
	ClientStatusUpdater clientStatusUpdater;

	// Cache of location information
	int locationCacheSize;
	CoalescedKeyRangeMap< Reference<LocationInfo> > locationCache;

	std::map< UID, StorageServerInfo* > server_interf;

	UID dbId;
	bool internal; // Only contexts created through the C client and fdbcli are non-internal

	PrioritizedTransactionTagMap<ClientTagThrottleData> throttledTags;

	CounterCollection cc;

	Counter transactionReadVersions;
	Counter transactionReadVersionsThrottled;
	Counter transactionReadVersionsCompleted;
	Counter transactionReadVersionBatches;
	Counter transactionBatchReadVersions;
	Counter transactionDefaultReadVersions;
	Counter transactionImmediateReadVersions;
	Counter transactionBatchReadVersionsCompleted;
	Counter transactionDefaultReadVersionsCompleted;
	Counter transactionImmediateReadVersionsCompleted;
	Counter transactionLogicalReads;
	Counter transactionPhysicalReads;
	Counter transactionPhysicalReadsCompleted;
	Counter transactionGetKeyRequests;
	Counter transactionGetValueRequests;
	Counter transactionGetRangeRequests;
	Counter transactionWatchRequests;
	Counter transactionGetAddressesForKeyRequests;
	Counter transactionBytesRead;
	Counter transactionKeysRead;
	Counter transactionMetadataVersionReads;
	Counter transactionCommittedMutations;
	Counter transactionCommittedMutationBytes;
	Counter transactionSetMutations;
	Counter transactionClearMutations;
	Counter transactionAtomicMutations;
	Counter transactionsCommitStarted;
	Counter transactionsCommitCompleted;
	Counter transactionKeyServerLocationRequests;
	Counter transactionKeyServerLocationRequestsCompleted;
	Counter transactionsTooOld;
	Counter transactionsFutureVersions;
	Counter transactionsNotCommitted;
	Counter transactionsMaybeCommitted;
	Counter transactionsResourceConstrained;
	Counter transactionsProcessBehind;
	Counter transactionsThrottled;

	ContinuousSample<double> latencies, readLatencies, commitLatencies, GRVLatencies, mutationsPerCommit, bytesPerCommit;

	int outstandingWatches;
	int maxOutstandingWatches;

	int snapshotRywEnabled;

	Future<Void> logger;
	Future<Void> throttleExpirer;

	TaskPriority taskID;

	Int64MetricHandle getValueSubmitted;
	EventMetricHandle<GetValueComplete> getValueCompleted;

	Reference<AsyncVar<ClientDBInfo>> clientInfo;
	Future<Void> clientInfoMonitor;
	Future<Void> connected;

	Reference<AsyncVar<Optional<ClusterInterface>>> statusClusterInterface;
	Future<Void> statusLeaderMon;
	double lastStatusFetch;

	int apiVersion;

	int mvCacheInsertLocation;
	std::vector<std::pair<Version, Optional<Value>>> metadataVersionCache;

	HealthMetrics healthMetrics;
	double healthMetricsLastUpdated;
	double detailedHealthMetricsLastUpdated;

	UniqueOrderedOptionList<FDBTransactionOptions> transactionDefaults;

	std::vector<std::unique_ptr<SpecialKeyRangeBaseImpl>> specialKeySpaceModules;
	std::unique_ptr<SpecialKeySpace> specialKeySpace;
	void registerSpecialKeySpaceModule(SpecialKeySpace::MODULE module, std::unique_ptr<SpecialKeyRangeBaseImpl> impl);

	static bool debugUseTags;
	static const std::vector<std::string> debugTransactionTagChoices; 
};

#endif
