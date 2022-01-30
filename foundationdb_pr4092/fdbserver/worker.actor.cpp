/*
 * worker.actor.cpp
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

#include <tuple>
#include <boost/lexical_cast.hpp>

#include "fdbserver/Knobs.h"
#include "flow/ActorCollection.h"
#include "flow/SystemMonitor.h"
#include "flow/TDMetric.actor.h"
#include "fdbrpc/simulator.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/MetricLogger.h"
#include "fdbserver/BackupInterface.h"
#include "fdbserver/WorkerInterface.actor.h"
#include "fdbserver/IKeyValueStore.h"
#include "fdbserver/WaitFailure.h"
#include "fdbserver/TesterInterface.actor.h"  // for poisson()
#include "fdbserver/IDiskQueue.h"
#include "fdbclient/DatabaseContext.h"
#include "fdbserver/DataDistributorInterface.h"
#include "fdbserver/ServerDBInfo.h"
#include "fdbserver/FDBExecHelper.actor.h"
#include "fdbserver/CoordinationInterface.h"
#include "fdbclient/MonitorLeader.h"
#include "fdbclient/ClientWorkerInterface.h"
#include "flow/Profiler.h"
#include "flow/ThreadHelper.actor.h"
#include "flow/Trace.h"

#ifdef __linux__
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#if defined(__linux__) || defined(__FreeBSD__)
#ifdef USE_GPERFTOOLS
#include "gperftools/profiler.h"
#include "gperftools/heap-profiler.h"
#endif
#include <unistd.h>
#include <thread>
#include <execinfo.h>
#endif
#include "flow/actorcompiler.h"  // This must be the last #include.

#if CENABLED(0, NOT_IN_CLEAN)
extern IKeyValueStore* keyValueStoreCompressTestData(IKeyValueStore* store);
# define KV_STORE(filename,uid) keyValueStoreCompressTestData(keyValueStoreSQLite(filename,uid))
#elif CENABLED(0, NOT_IN_CLEAN)
# define KV_STORE(filename,uid) keyValueStoreSQLite(filename,uid)
#else
# define KV_STORE(filename,uid) keyValueStoreMemory(filename,uid)
#endif

ACTOR Future<std::vector<Endpoint>> tryDBInfoBroadcast(RequestStream<UpdateServerDBInfoRequest> stream, UpdateServerDBInfoRequest req) {
	ErrorOr<std::vector<Endpoint>> rep = wait( stream.getReplyUnlessFailedFor(req, SERVER_KNOBS->DBINFO_FAILED_DELAY, 0) );
	if(rep.present()) {
		return rep.get();
	}
	req.broadcastInfo.push_back(stream.getEndpoint());
	return req.broadcastInfo;
}

ACTOR Future<std::vector<Endpoint>> broadcastDBInfoRequest(UpdateServerDBInfoRequest req, int sendAmount, Optional<Endpoint> sender, bool sendReply) {
	state std::vector<Future<std::vector<Endpoint>>> replies;
	state ReplyPromise<std::vector<Endpoint>> reply = req.reply;
	resetReply( req );
	int currentStream = 0;
	std::vector<Endpoint> broadcastEndpoints = req.broadcastInfo;
	for(int i = 0; i < sendAmount && currentStream < broadcastEndpoints.size(); i++) {
		std::vector<Endpoint> endpoints;
		RequestStream<UpdateServerDBInfoRequest> cur(broadcastEndpoints[currentStream++]);
		while(currentStream < broadcastEndpoints.size()*(i+1)/sendAmount) {
			endpoints.push_back(broadcastEndpoints[currentStream++]);
		}
		req.broadcastInfo = endpoints;
		replies.push_back( tryDBInfoBroadcast( cur, req ) );
		resetReply( req );
	}
	wait( waitForAll(replies) );
	std::vector<Endpoint> notUpdated;
	if(sender.present()) {
		notUpdated.push_back(sender.get());
	}
	for(auto& it : replies) {
		notUpdated.insert(notUpdated.end(), it.get().begin(), it.get().end());
	}
	if(sendReply) {
		reply.send(notUpdated);
	}
	return notUpdated;
}

ACTOR static Future<Void> extractClientInfo( Reference<AsyncVar<ServerDBInfo>> db, Reference<AsyncVar<ClientDBInfo>> info ) {
	state std::vector<UID> lastProxyUIDs;
	state std::vector<MasterProxyInterface> lastProxies;
	loop {
		ClientDBInfo ni = db->get().client;
		shrinkProxyList(ni, lastProxyUIDs, lastProxies);
		info->set( ni );
		wait( db->onChange() );
	}
}

Database openDBOnServer( Reference<AsyncVar<ServerDBInfo>> const& db, TaskPriority taskID, bool enableLocalityLoadBalance, bool lockAware ) {
	Reference<AsyncVar<ClientDBInfo>> info( new AsyncVar<ClientDBInfo> );
	return DatabaseContext::create( info, extractClientInfo(db, info), enableLocalityLoadBalance ? db->get().myLocality : LocalityData(), enableLocalityLoadBalance, taskID, lockAware );
}

struct ErrorInfo {
	Error error;
	const Role &role;
	UID id;
	ErrorInfo( Error e, const Role &role, UID id ) : error(e), role(role), id(id) {}
	template <class Ar> void serialize(Ar&) { ASSERT(false); }
};

Error checkIOTimeout(Error const &e) {
	// Convert all_errors to io_timeout if global timeout bool was set
	bool timeoutOccurred = (bool)g_network->global(INetwork::enASIOTimedOut);
	// In simulation, have to check global timed out flag for both this process and the machine process on which IO is done
	if(g_network->isSimulated() && !timeoutOccurred)
		timeoutOccurred = g_pSimulator->getCurrentProcess()->machine->machineProcess->global(INetwork::enASIOTimedOut);

	if(timeoutOccurred) {
		TEST(true); // Timeout occurred
		Error timeout = io_timeout();
		// Preserve injectedness of error
		if(e.isInjectedFault())
			timeout = timeout.asInjectedFault();
		return timeout;
	}
	return e;
}

ACTOR Future<Void> forwardError( PromiseStream<ErrorInfo> errors,
	Role role, UID id,
	Future<Void> process )
{
	try {
		wait(process);
		errors.send( ErrorInfo(success(), role, id) );
		return Void();
	} catch (Error& e) {
		errors.send( ErrorInfo(e, role, id) );
		return Void();
	}
}

ACTOR Future<Void> handleIOErrors( Future<Void> actor, IClosable* store, UID id, Future<Void> onClosed = Void() ) {
	state Future<ErrorOr<Void>> storeError = actor.isReady() ? Never() : errorOr( store->getError() );
	choose {
		when (state ErrorOr<Void> e = wait( errorOr(actor) )) {
			if (e.isError() && e.getError().code() == error_code_please_reboot) {
				// no need to wait.
			} else {
				wait(onClosed);
			}
			if(e.isError() && e.getError().code() == error_code_broken_promise && !storeError.isReady()) {
				wait(delay(0.00001 + FLOW_KNOBS->MAX_BUGGIFIED_DELAY));
			}
			if(storeError.isReady()) throw storeError.get().getError();
			if (e.isError()) throw e.getError(); else return e.get();
		}
		when (ErrorOr<Void> e = wait( storeError )) {
			TraceEvent("WorkerTerminatingByIOError", id).error(e.getError(), true);
			actor.cancel();
			// file_not_found can occur due to attempting to open a partially deleted DiskQueue, which should not be reported SevError.
			if (e.getError().code() == error_code_file_not_found) {
				TEST(true); // Worker terminated with file_not_found error
				return Void();
			}
			throw e.getError();
		}
	}
}

ACTOR Future<Void> workerHandleErrors(FutureStream<ErrorInfo> errors) {
	loop choose {
		when( ErrorInfo _err = waitNext(errors) ) {
			ErrorInfo err = _err;
			bool ok =
				err.error.code() == error_code_success ||
				err.error.code() == error_code_please_reboot ||
				err.error.code() == error_code_actor_cancelled ||
				err.error.code() == error_code_coordinators_changed ||  // The worker server was cancelled
				err.error.code() == error_code_shutdown_in_progress;

			if (!ok) {
				err.error = checkIOTimeout(err.error);  // Possibly convert error to io_timeout
			}

			endRole(err.role, err.id, "Error", ok, err.error);


			if (err.error.code() == error_code_please_reboot || err.error.code() == error_code_io_timeout || (err.role == Role::SHARED_TRANSACTION_LOG && err.error.code() == error_code_io_error )) throw err.error;
		}
	}
}

// Improve simulation code coverage by sometimes deferring the destruction of workerInterface (and therefore "endpoint not found" responses to clients
//		for an extra second, so that clients are more likely to see broken_promise errors
ACTOR template<class T> Future<Void> zombie(T workerInterface, Future<Void> worker) {
	try {
		wait(worker);
		if (BUGGIFY)
			wait(delay(1.0));
		return Void();
	} catch (Error& e) {
		throw;
	}
}

ACTOR Future<Void> loadedPonger( FutureStream<LoadedPingRequest> pings ) {
	state Standalone<StringRef> payloadBack(std::string( 20480, '.' ));

	loop {
		LoadedPingRequest pong = waitNext( pings );
		LoadedReply rep;
		rep.payload = (pong.loadReply ? payloadBack : LiteralStringRef(""));
		rep.id = pong.id;
		pong.reply.send( rep );
	}
}

StringRef fileStoragePrefix = LiteralStringRef("storage-");
StringRef fileLogDataPrefix = LiteralStringRef("log-");
StringRef fileVersionedLogDataPrefix = LiteralStringRef("log2-");
StringRef fileLogQueuePrefix = LiteralStringRef("logqueue-");
StringRef tlogQueueExtension = LiteralStringRef("fdq");

enum class FilesystemCheck {
	FILES_ONLY,
	DIRECTORIES_ONLY,
	FILES_AND_DIRECTORIES,
};

struct KeyValueStoreSuffix {
	KeyValueStoreType type;
	std::string suffix;
	FilesystemCheck check;
};

KeyValueStoreSuffix bTreeV1Suffix = { KeyValueStoreType::SSD_BTREE_V1, ".fdb", FilesystemCheck::FILES_ONLY };
KeyValueStoreSuffix bTreeV2Suffix = { KeyValueStoreType::SSD_BTREE_V2, ".sqlite", FilesystemCheck::FILES_ONLY };
KeyValueStoreSuffix memorySuffix = { KeyValueStoreType::MEMORY, "-0.fdq", FilesystemCheck::FILES_ONLY };
KeyValueStoreSuffix memoryRTSuffix = { KeyValueStoreType::MEMORY_RADIXTREE, "-0.fdr", FilesystemCheck::FILES_ONLY };
KeyValueStoreSuffix redwoodSuffix = { KeyValueStoreType::SSD_REDWOOD_V1, ".redwood", FilesystemCheck::FILES_ONLY };
KeyValueStoreSuffix rocksdbSuffix = { KeyValueStoreType::SSD_ROCKSDB_V1, ".rocksdb",
	                                  FilesystemCheck::DIRECTORIES_ONLY };

std::string validationFilename = "_validate";

std::string filenameFromSample( KeyValueStoreType storeType, std::string folder, std::string sample_filename ) {
	if( storeType == KeyValueStoreType::SSD_BTREE_V1 )
		return joinPath( folder, sample_filename );
	else if ( storeType == KeyValueStoreType::SSD_BTREE_V2 )
		return joinPath(folder, sample_filename);
	else if( storeType == KeyValueStoreType::MEMORY || storeType == KeyValueStoreType::MEMORY_RADIXTREE )
		return joinPath( folder, sample_filename.substr(0, sample_filename.size() - 5) );
	else if ( storeType == KeyValueStoreType::SSD_REDWOOD_V1 )
		return joinPath(folder, sample_filename);
	else if (storeType == KeyValueStoreType::SSD_ROCKSDB_V1)
		return joinPath(folder, sample_filename);
	UNREACHABLE();
}

std::string filenameFromId( KeyValueStoreType storeType, std::string folder, std::string prefix, UID id ) {
	if(storeType == KeyValueStoreType::SSD_BTREE_V1)
		return joinPath( folder, prefix + id.toString() + ".fdb" );
	else if (storeType == KeyValueStoreType::SSD_BTREE_V2)
		return joinPath(folder, prefix + id.toString() + ".sqlite");
	else if(storeType == KeyValueStoreType::MEMORY || storeType == KeyValueStoreType::MEMORY_RADIXTREE)
		return joinPath( folder, prefix + id.toString() + "-" );
	else if (storeType == KeyValueStoreType::SSD_REDWOOD_V1)
		return joinPath(folder, prefix + id.toString() + ".redwood");
	else if (storeType == KeyValueStoreType::SSD_ROCKSDB_V1)
		return joinPath(folder, prefix + id.toString() + ".rocksdb");

	UNREACHABLE();
}


struct TLogOptions {
	TLogOptions() = default;
	TLogOptions( TLogVersion v, TLogSpillType s ) : version(v), spillType(s) {}

	TLogVersion version = TLogVersion::DEFAULT;
	TLogSpillType spillType = TLogSpillType::UNSET;

	static ErrorOr<TLogOptions> FromStringRef( StringRef s ) {
		TLogOptions options;
		for (StringRef key = s.eat("_"), value = s.eat("_");
		     s.size() != 0 || key.size();
		     key = s.eat("_"), value = s.eat("_")) {
			if (key.size() != 0 && value.size() == 0) return default_error_or();

			if (key == LiteralStringRef("V")) {
				ErrorOr<TLogVersion> tLogVersion = TLogVersion::FromStringRef(value);
				if (tLogVersion.isError()) return tLogVersion.getError();
				options.version = tLogVersion.get();
			} else if (key == LiteralStringRef("LS")) {
				ErrorOr<TLogSpillType> tLogSpillType = TLogSpillType::FromStringRef(value);
				if (tLogSpillType.isError()) return tLogSpillType.getError();
				options.spillType = tLogSpillType.get();
			} else {
				return default_error_or();
			}
		}
		return options;
	}

	bool operator == ( const TLogOptions& o ) {
		return version == o.version &&
			(spillType == o.spillType || version >= TLogVersion::V5);
	}

	std::string toPrefix() const {
		std::string toReturn = "";
		switch (version) {
		case TLogVersion::UNSET:
			ASSERT(false);
		case TLogVersion::V2:
			return "";
		case TLogVersion::V3:
		case TLogVersion::V4:
			toReturn =
				"V_" + boost::lexical_cast<std::string>(version) +
				"_LS_" + boost::lexical_cast<std::string>(spillType);
			break;
		case TLogVersion::V5:
			toReturn = "V_" + boost::lexical_cast<std::string>(version);
			break;
		}
		ASSERT_WE_THINK( FromStringRef( toReturn ).get() == *this );
		return toReturn + "-";
	}
};

TLogFn tLogFnForOptions( TLogOptions options ) {
	switch (options.version) {
		case TLogVersion::V2:
			if (options.spillType == TLogSpillType::REFERENCE)
				ASSERT(false);
			return oldTLog_6_0::tLog;
		case TLogVersion::V3:
		case TLogVersion::V4:
			if (options.spillType == TLogSpillType::VALUE)
				return oldTLog_6_0::tLog;
			else
				return oldTLog_6_2::tLog;
		case TLogVersion::V5:
			return tLog;
		default:
			ASSERT(false);
	}
	return tLog;
}

struct DiskStore {
	enum COMPONENT { TLogData, Storage, UNSET };

	UID storeID = UID();
	std::string filename = ""; // For KVStoreMemory just the base filename to be passed to IDiskQueue
	COMPONENT storedComponent = UNSET;
	KeyValueStoreType storeType = KeyValueStoreType::END;
	TLogOptions tLogOptions;
};

std::vector<DiskStore> getDiskStores(std::string folder, std::string suffix, KeyValueStoreType type,
                                     FilesystemCheck check) {
	std::vector< DiskStore > result;
	vector<std::string> files;

	if (check == FilesystemCheck::FILES_ONLY || check == FilesystemCheck::FILES_AND_DIRECTORIES) {
		files = platform::listFiles(folder, suffix);
	}
	if (check == FilesystemCheck::DIRECTORIES_ONLY || check == FilesystemCheck::FILES_AND_DIRECTORIES) {
		for (const auto& directory : platform::listDirectories(folder)) {
			if (StringRef(directory).endsWith(suffix)) {
				files.push_back(directory);
			}
		}
	}

	for( int idx = 0; idx < files.size(); idx++ ) {
		DiskStore store;
		store.storeType = type;

		StringRef filename = StringRef( files[idx] );
		Standalone<StringRef> prefix;
		if( filename.startsWith( fileStoragePrefix ) ) {
			store.storedComponent = DiskStore::Storage;
			prefix = fileStoragePrefix;
		}
		else if( filename.startsWith( fileVersionedLogDataPrefix ) ) {
			store.storedComponent = DiskStore::TLogData;
			// Use the option string that's in the file rather than tLogOptions.toPrefix(),
			// because they might be different if a new option was introduced in this version.
			StringRef optionsString = filename.removePrefix(fileVersionedLogDataPrefix).eat("-");
			TraceEvent("DiskStoreVersioned").detail("Filename", filename);
			ErrorOr<TLogOptions> tLogOptions = TLogOptions::FromStringRef(optionsString);
			if (tLogOptions.isError()) {
				TraceEvent(SevWarn, "DiskStoreMalformedFilename").detail("Filename", filename);
				continue;
			}
			TraceEvent("DiskStoreVersionedSuccess").detail("Filename", filename);
			store.tLogOptions = tLogOptions.get();
			prefix = filename.substr(0, fileVersionedLogDataPrefix.size() + optionsString.size() + 1);
		}
		else if( filename.startsWith( fileLogDataPrefix ) ) {
			TraceEvent("DiskStoreUnversioned").detail("Filename", filename);
			store.storedComponent = DiskStore::TLogData;
			store.tLogOptions.version = TLogVersion::V2;
			store.tLogOptions.spillType = TLogSpillType::VALUE;
			prefix = fileLogDataPrefix;
		}
		else
			continue;

		store.storeID = UID::fromString( files[idx].substr( prefix.size(), 32 ) );
		store.filename = filenameFromSample( type, folder, files[idx] );
		result.push_back( store );
	}
	return result;
}

std::vector< DiskStore > getDiskStores( std::string folder ) {
	auto result = getDiskStores(folder, bTreeV1Suffix.suffix, bTreeV1Suffix.type, bTreeV1Suffix.check);
	auto result1 = getDiskStores(folder, bTreeV2Suffix.suffix, bTreeV2Suffix.type, bTreeV2Suffix.check);
	result.insert( result.end(), result1.begin(), result1.end() );
	auto result2 = getDiskStores(folder, memorySuffix.suffix, memorySuffix.type, memorySuffix.check);
	result.insert( result.end(), result2.begin(), result2.end() );
	auto result3 = getDiskStores(folder, redwoodSuffix.suffix, redwoodSuffix.type, redwoodSuffix.check);
	result.insert( result.end(), result3.begin(), result3.end() );
	auto result4 = getDiskStores(folder, memoryRTSuffix.suffix, memoryRTSuffix.type, memoryRTSuffix.check);
	result.insert( result.end(), result4.begin(), result4.end() );
	auto result5 = getDiskStores(folder, rocksdbSuffix.suffix, rocksdbSuffix.type, rocksdbSuffix.check);
	result.insert( result.end(), result5.begin(), result5.end() );
	return result;
}

// Register the worker interf to cluster controller (cc) and
// re-register the worker when key roles interface, e.g., cc, dd, ratekeeper, change.
ACTOR Future<Void> registrationClient(
		Reference<AsyncVar<Optional<ClusterControllerFullInterface>>> ccInterface,
		WorkerInterface interf,
		Reference<AsyncVar<ClusterControllerPriorityInfo>> asyncPriorityInfo,
		ProcessClass initialClass,
		Reference<AsyncVar<Optional<DataDistributorInterface>>> ddInterf,
		Reference<AsyncVar<Optional<RatekeeperInterface>>> rkInterf,
		Reference<AsyncVar<bool>> degraded,
		PromiseStream< ErrorInfo > errors,
		LocalityData locality,
		Reference<AsyncVar<ServerDBInfo>> dbInfo,
		Reference<ClusterConnectionFile> connFile,
		Reference<AsyncVar<std::set<std::string>>> issues) {
	// Keeps the cluster controller (as it may be re-elected) informed that this worker exists
	// The cluster controller uses waitFailureClient to find out if we die, and returns from registrationReply (requiring us to re-register)
	// The registration request piggybacks optional distributor interface if it exists.
	state Generation requestGeneration = 0;
	state ProcessClass processClass = initialClass;
	state Reference<AsyncVar<Optional<std::pair<uint16_t,StorageServerInterface>>>> scInterf( new AsyncVar<Optional<std::pair<uint16_t,StorageServerInterface>>>() );
	state Future<Void> cacheProcessFuture;
	state Future<Void> cacheErrorsFuture;
	state Optional<double> incorrectTime;
	loop {
		RegisterWorkerRequest request(interf, initialClass, processClass, asyncPriorityInfo->get(), requestGeneration++, ddInterf->get(), rkInterf->get(), scInterf->get(), degraded->get());
		for (auto const& i : issues->get()) {
			request.issues.push_back_deep(request.issues.arena(), i);
		}
		ClusterConnectionString fileConnectionString;
		if (connFile && !connFile->fileContentsUpToDate(fileConnectionString)) {
			request.issues.push_back_deep(request.issues.arena(), LiteralStringRef("incorrect_cluster_file_contents"));
			std::string connectionString = connFile->getConnectionString().toString();
			if(!incorrectTime.present()) {
				incorrectTime = now();
			}
			if(connFile->canGetFilename()) {
				// Don't log a SevWarnAlways initially to account for transient issues (e.g. someone else changing the file right before us)
				TraceEvent(now() - incorrectTime.get() > 300 ? SevWarnAlways : SevWarn, "IncorrectClusterFileContents")
					.detail("Filename", connFile->getFilename())
					.detail("ConnectionStringFromFile", fileConnectionString.toString())
					.detail("CurrentConnectionString", connectionString);
			}
		}
		else {
			incorrectTime = Optional<double>();
		}

		auto peers = FlowTransport::transport().getIncompatiblePeers();
		for(auto it = peers->begin(); it != peers->end();) {
			if( now() - it->second.second > FLOW_KNOBS->INCOMPATIBLE_PEER_DELAY_BEFORE_LOGGING ) {
				request.incompatiblePeers.push_back(it->first);
				it = peers->erase(it);
			} else {
				it++;
			}
		}

		Future<RegisterWorkerReply> registrationReply = ccInterface->get().present() ? brokenPromiseToNever( ccInterface->get().get().registerWorker.getReply(request) ) : Never();
		choose {
			when ( RegisterWorkerReply reply = wait( registrationReply )) {
				processClass = reply.processClass;
				asyncPriorityInfo->set( reply.priorityInfo );

				if(!reply.storageCache.present()) {
					cacheProcessFuture.cancel();
					scInterf->set(Optional<std::pair<uint16_t,StorageServerInterface>>());
				} else if (!scInterf->get().present() || scInterf->get().get().first != reply.storageCache.get()) {
					StorageServerInterface recruited;
					recruited.locality = locality;
					recruited.initEndpoints();

					std::map<std::string, std::string> details;
					startRole( Role::STORAGE_CACHE, recruited.id(), interf.id(), details );

					//DUMPTOKEN(recruited.getVersion);
					DUMPTOKEN(recruited.getValue);
					DUMPTOKEN(recruited.getKey);
					DUMPTOKEN(recruited.getKeyValues);
					DUMPTOKEN(recruited.getShardState);
					DUMPTOKEN(recruited.waitMetrics);
					DUMPTOKEN(recruited.splitMetrics);
					DUMPTOKEN(recruited.getReadHotRanges);
					DUMPTOKEN(recruited.getStorageMetrics);
					DUMPTOKEN(recruited.waitFailure);
					DUMPTOKEN(recruited.getQueuingMetrics);
					DUMPTOKEN(recruited.getKeyValueStoreType);
					DUMPTOKEN(recruited.watchValue);

					cacheProcessFuture = storageCache( recruited, reply.storageCache.get(), dbInfo );
					cacheErrorsFuture = forwardError(errors, Role::STORAGE_CACHE, recruited.id(), setWhenDoneOrError(cacheProcessFuture, scInterf, Optional<std::pair<uint16_t,StorageServerInterface>>()));
					scInterf->set(std::make_pair(reply.storageCache.get(), recruited));
				}
			}
			when ( wait( ccInterface->onChange() )) {}
			when ( wait( ddInterf->onChange() ) ) {}
			when ( wait( rkInterf->onChange() ) ) {}
			when ( wait( scInterf->onChange() ) ) {}
			when ( wait( degraded->onChange() ) ) {}
			when ( wait( FlowTransport::transport().onIncompatibleChanged() ) ) {}
			when ( wait( issues->onChange() ) ) {}
		}
	}
}

#if (defined(__linux__) || defined(__FreeBSD__)) && defined(USE_GPERFTOOLS)
//A set of threads that should be profiled
std::set<std::thread::id> profiledThreads;

//Returns whether or not a given thread should be profiled
int filter_in_thread(void *arg) {
	return profiledThreads.count(std::this_thread::get_id()) > 0 ? 1 : 0;
}
#endif

//Enables the calling thread to be profiled
void registerThreadForProfiling() {
#if (defined(__linux__) || defined(__FreeBSD__)) && defined(USE_GPERFTOOLS)
	//Not sure if this is actually needed, but a call to backtrace was advised here:
	//http://groups.google.com/group/google-perftools/browse_thread/thread/0dfd74532e038eb8/2686d9f24ac4365f?pli=1
	profiledThreads.insert(std::this_thread::get_id());
	const int num_levels = 100;
	void *pc[num_levels];
	backtrace(pc, num_levels);
#endif
}

//Starts or stops the CPU profiler
void updateCpuProfiler(ProfilerRequest req) {
	switch (req.type) {
	case ProfilerRequest::Type::GPROF:
#if (defined(__linux__) || defined(__FreeBSD__)) && defined(USE_GPERFTOOLS) && !defined(VALGRIND)
		switch (req.action) {
		case ProfilerRequest::Action::ENABLE: {
			const char *path = (const char*)req.outputFile.begin();
			ProfilerOptions *options = new ProfilerOptions();
			options->filter_in_thread = &filter_in_thread;
			options->filter_in_thread_arg = NULL;
			ProfilerStartWithOptions(path, options);
			break;
		}
		case ProfilerRequest::Action::DISABLE:
			ProfilerStop();
			break;
		case ProfilerRequest::Action::RUN:
			ASSERT(false);  // User should have called runProfiler.
			break;
		}
#endif
		break;
	case ProfilerRequest::Type::FLOW:
		switch (req.action) {
		case ProfilerRequest::Action::ENABLE:
			startProfiling(g_network, {}, req.outputFile);
			break;
		case ProfilerRequest::Action::DISABLE:
			stopProfiling();
			break;
		case ProfilerRequest::Action::RUN:
			ASSERT(false);  // User should have called runProfiler.
			break;
		}
		break;
	default:
		ASSERT(false);
		break;
	}
}

ACTOR Future<Void> runCpuProfiler(ProfilerRequest req) {
	if (req.action == ProfilerRequest::Action::RUN) {
		req.action = ProfilerRequest::Action::ENABLE;
		updateCpuProfiler(req);
		wait(delay(req.duration));
		req.action = ProfilerRequest::Action::DISABLE;
		updateCpuProfiler(req);
		return Void();
	} else {
		updateCpuProfiler(req);
		return Void();
	}
}

void runHeapProfiler(const char* msg) {
#if defined(__linux__) && defined(USE_GPERFTOOLS) && !defined(VALGRIND)
	if (IsHeapProfilerRunning()) {
		HeapProfilerDump(msg);
	} else {
		TraceEvent("ProfilerError").detail("Message", "HeapProfiler not running");
	}
#else
	TraceEvent("ProfilerError").detail("Message", "HeapProfiler Unsupported");
#endif
}

ACTOR Future<Void> runProfiler(ProfilerRequest req) {
	if (req.type == ProfilerRequest::Type::GPROF_HEAP) {
		runHeapProfiler("User triggered heap dump");
	} else {
		wait( runCpuProfiler(req) );
	}

	return Void();
}

bool checkHighMemory(int64_t threshold, bool* error) {
#if defined(__linux__) && defined(USE_GPERFTOOLS) && !defined(VALGRIND)
	*error = false;
	uint64_t page_size = sysconf(_SC_PAGESIZE);
	int fd = open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		TraceEvent("OpenStatmFileFailure");
		*error = true;
		return false;
	}

	const int buf_sz = 256;
	char stat_buf[buf_sz];
	ssize_t stat_nread = read(fd, stat_buf, buf_sz);
	if (stat_nread < 0) {
		TraceEvent("ReadStatmFileFailure");
		*error = true;
		return false;
	}

	uint64_t vmsize, rss;
	sscanf(stat_buf, "%lu %lu", &vmsize, &rss);
	rss *= page_size;
	if (rss >= threshold) {
		return true;
	}
#else
	TraceEvent("CheckHighMemoryUnsupported");
	*error = true;
#endif
	return false;
}

// Runs heap profiler when RSS memory usage is high.
ACTOR Future<Void> monitorHighMemory(int64_t threshold) {
	if (threshold <= 0) return Void();

	loop {
		bool err = false;
		bool highmem = checkHighMemory(threshold, &err);
		if (err) break;

		if (highmem) runHeapProfiler("Highmem heap dump");
		wait( delay(SERVER_KNOBS->HEAP_PROFILER_INTERVAL) );
	}
	return Void();
}

ACTOR Future<Void> storageServerRollbackRebooter( Future<Void> prevStorageServer, KeyValueStoreType storeType, std::string filename, UID id, LocalityData locality, Reference<AsyncVar<ServerDBInfo>> db, std::string folder, ActorCollection* filesClosed, int64_t memoryLimit, IKeyValueStore* store ) {
	loop {
		ErrorOr<Void> e = wait( errorOr( prevStorageServer) );
		if (!e.isError()) return Void();
		else if (e.getError().code() != error_code_please_reboot) throw e.getError();

		TraceEvent("StorageServerRequestedReboot", id);

		StorageServerInterface recruited;
		recruited.uniqueID = id;
		recruited.locality = locality;
		recruited.initEndpoints();

		DUMPTOKEN(recruited.getValue);
		DUMPTOKEN(recruited.getKey);
		DUMPTOKEN(recruited.getKeyValues);
		DUMPTOKEN(recruited.getShardState);
		DUMPTOKEN(recruited.waitMetrics);
		DUMPTOKEN(recruited.splitMetrics);
		DUMPTOKEN(recruited.getReadHotRanges);
		DUMPTOKEN(recruited.getStorageMetrics);
		DUMPTOKEN(recruited.waitFailure);
		DUMPTOKEN(recruited.getQueuingMetrics);
		DUMPTOKEN(recruited.getKeyValueStoreType);
		DUMPTOKEN(recruited.watchValue);

		prevStorageServer = storageServer( store, recruited, db, folder, Promise<Void>(), Reference<ClusterConnectionFile> (nullptr) );
		prevStorageServer = handleIOErrors(prevStorageServer, store, id, store->onClosed());
	}
}

// FIXME:  This will not work correctly in simulation as all workers would share the same roles map
std::set<std::pair<std::string, std::string>> g_roles;

Standalone<StringRef> roleString(std::set<std::pair<std::string, std::string>> roles, bool with_ids) {
	std:: string result;
	for(auto &r : roles) {
		if(!result.empty())
			result.append(",");
		result.append(r.first);
		if(with_ids) {
			result.append(":");
			result.append(r.second);
		}
	}
	return StringRef(result);
}

void startRole(const Role &role, UID roleId, UID workerId, const std::map<std::string, std::string> &details, const std::string &origination) {
	if(role.includeInTraceRoles) {
		addTraceRole(role.abbreviation);
	}

	TraceEvent ev("Role", roleId);
	ev.detail("As", role.roleName)
		.detail("Transition", "Begin")
		.detail("Origination", origination)
		.detail("OnWorker", workerId);
	for(auto it = details.begin(); it != details.end(); it++)
		ev.detail(it->first.c_str(), it->second);

	ev.trackLatest( roleId.shortString() + ".Role"  );

	// Update roles map, log Roles metrics
	g_roles.insert({role.roleName, roleId.shortString()});
	StringMetricHandle(LiteralStringRef("Roles")) = roleString(g_roles, false);
	StringMetricHandle(LiteralStringRef("RolesWithIDs")) = roleString(g_roles, true);
	if (g_network->isSimulated()) g_simulator.addRole(g_network->getLocalAddress(), role.roleName);
}

void endRole(const Role &role, UID id, std::string reason, bool ok, Error e) {
	{
		TraceEvent ev("Role", id);
		if(e.code() != invalid_error_code)
			ev.error(e, true);
		ev.detail("Transition", "End")
			.detail("As", role.roleName)
			.detail("Reason", reason);

		ev.trackLatest( id.shortString() + ".Role" );
	}

	if(!ok) {
		std::string type = role.roleName + "Failed";

		TraceEvent err(SevError, type.c_str(), id);
		if(e.code() != invalid_error_code) {
			err.error(e, true);
		}
		err.detail("Reason", reason);
	}

	latestEventCache.clear( id.shortString() );

	// Update roles map, log Roles metrics
	g_roles.erase({role.roleName, id.shortString()});
	StringMetricHandle(LiteralStringRef("Roles")) = roleString(g_roles, false);
	StringMetricHandle(LiteralStringRef("RolesWithIDs")) = roleString(g_roles, true);
	if (g_network->isSimulated()) g_simulator.removeRole(g_network->getLocalAddress(), role.roleName);

	if(role.includeInTraceRoles) {
		removeTraceRole(role.abbreviation);
	}
}

ACTOR Future<Void>
traceRole(Role role, UID roleId)
{
	loop {
		wait(delay(SERVER_KNOBS->WORKER_LOGGING_INTERVAL));
		TraceEvent("Role", roleId)
			.detail("Transition", "Refresh")
			.detail("As", role.roleName);
	}
}

ACTOR Future<Void> workerSnapCreate(WorkerSnapRequest snapReq, StringRef snapFolder) {
	state ExecCmdValueString snapArg(snapReq.snapPayload);
	try {
		int err = wait(execHelper(&snapArg, snapReq.snapUID, snapFolder.toString(), snapReq.role.toString()));
		std::string uidStr = snapReq.snapUID.toString();
		TraceEvent("ExecTraceWorker")
			.detail("Uid", uidStr)
			.detail("Status", err)
			.detail("Role", snapReq.role)
			.detail("Value", snapFolder)
			.detail("ExecPayload", snapReq.snapPayload);
		if (err != 0) {
			throw operation_failed();
		}
		if (snapReq.role.toString() == "storage") {
			printStorageVersionInfo();
		}
		snapReq.reply.send(Void());
	} catch (Error& e) {
		TraceEvent("ExecHelperError").error(e, true /*includeCancelled*/);
		if (e.code() != error_code_operation_cancelled) {
			snapReq.reply.sendError(e);
		} else {
			throw e;
		}
	}
	return Void();
}

// TODO: `issues` is right now only updated by `monitorTraceLogIssues` and thus is being `set` on every update.
// It could be changed to `insert` and `trigger` later if we want to use it as a generic way for the caller of this
// function to report issues to cluster controller.
ACTOR Future<Void> monitorTraceLogIssues(Reference<AsyncVar<std::set<std::string>>> issues) {
	state bool pingTimeout = false;
	loop {
		wait(delay(SERVER_KNOBS->TRACE_LOG_FLUSH_FAILURE_CHECK_INTERVAL_SECONDS));
		Future<Void> pingAck = pingTraceLogWriterThread();
		try {
			wait(timeoutError(pingAck, SERVER_KNOBS->TRACE_LOG_PING_TIMEOUT_SECONDS));
		} catch (Error& e) {
			if (e.code() == error_code_timed_out) {
				pingTimeout = true;
			} else {
				throw;
			}
		}
		std::set<std::string> _issues;
		retriveTraceLogIssues(_issues);
		if (pingTimeout) {
			// Ping trace log writer thread timeout.
			_issues.insert("trace_log_writer_thread_unresponsive");
			pingTimeout = false;
		}
		issues->set(_issues);
	}
}

class SharedLogsKey {
	TLogVersion logVersion;
	TLogSpillType spillType;
	KeyValueStoreType storeType;

public:
	SharedLogsKey( const TLogOptions& options, KeyValueStoreType kvst )
			: logVersion(options.version), spillType(options.spillType), storeType(kvst) {
		if (logVersion >= TLogVersion::V5)
			spillType = TLogSpillType::UNSET;
	}

	bool operator<(const SharedLogsKey& other) const {
		return std::tie(logVersion, spillType, storeType) <
			std::tie(other.logVersion, other.spillType, other.storeType);
	}
};

struct SharedLogsValue {
	Future<Void> actor = Void();
	UID uid = UID();
	PromiseStream<InitializeTLogRequest> requests;

	SharedLogsValue() = default;
	SharedLogsValue( Future<Void> actor, UID uid, PromiseStream<InitializeTLogRequest> requests )
		: actor(actor), uid(uid), requests(requests) {
	}
};

ACTOR Future<Void> workerServer(
		Reference<ClusterConnectionFile> connFile,
		Reference<AsyncVar<Optional<ClusterControllerFullInterface>>> ccInterface,
		LocalityData locality,
		Reference<AsyncVar<ClusterControllerPriorityInfo>> asyncPriorityInfo,
		ProcessClass initialClass, std::string folder, int64_t memoryLimit,
		std::string metricsConnFile, std::string metricsPrefix,
		Promise<Void> recoveredDiskFiles, int64_t memoryProfileThreshold,
		std::string _coordFolder, std::string whitelistBinPaths,
		Reference<AsyncVar<ServerDBInfo>> dbInfo) {
	state PromiseStream< ErrorInfo > errors;
	state Reference<AsyncVar<Optional<DataDistributorInterface>>> ddInterf( new AsyncVar<Optional<DataDistributorInterface>>() );
	state Reference<AsyncVar<Optional<RatekeeperInterface>>> rkInterf( new AsyncVar<Optional<RatekeeperInterface>>() );
	state Future<Void> handleErrors = workerHandleErrors( errors.getFuture() );  // Needs to be stopped last
	state ActorCollection errorForwarders(false);
	state Future<Void> loggingTrigger = Void();
	state double loggingDelay = SERVER_KNOBS->WORKER_LOGGING_INTERVAL;
	state ActorCollection filesClosed(true);
	state Promise<Void> stopping;
	state WorkerCache<InitializeStorageReply> storageCache;
	state Future<Void> metricsLogger;
	state Reference<AsyncVar<bool>> degraded = FlowTransport::transport().getDegraded();
	// tLogFnForOptions() can return a function that doesn't correspond with the FDB version that the
	// TLogVersion represents.  This can be done if the newer TLog doesn't support a requested option.
	// As (store type, spill type) can map to the same TLogFn across multiple TLogVersions, we need to
	// decide if we should collapse them into the same SharedTLog instance as well.  The answer
	// here is no, so that when running with log_version==3, all files should say V=3.
	state std::map<SharedLogsKey, SharedLogsValue> sharedLogs;
	state Reference<AsyncVar<UID>> activeSharedTLog(new AsyncVar<UID>());
	state WorkerCache<InitializeBackupReply> backupWorkerCache;

	state std::string coordFolder = abspath(_coordFolder);

	state WorkerInterface interf( locality );
	interf.initEndpoints();

	state Reference<AsyncVar<std::set<std::string>>> issues(new AsyncVar<std::set<std::string>>());

	folder = abspath(folder);

	if(metricsPrefix.size() > 0) {
		if( metricsConnFile.size() > 0) {
			try {
				state Database db = Database::createDatabase(metricsConnFile, Database::API_VERSION_LATEST, true, locality);
				metricsLogger = runMetrics( db, KeyRef(metricsPrefix) );
			} catch(Error &e) {
				TraceEvent(SevWarnAlways, "TDMetricsBadClusterFile").error(e).detail("ConnFile", metricsConnFile);
			}
		} else {
			bool lockAware = metricsPrefix.size() && metricsPrefix[0] == '\xff';
			metricsLogger = runMetrics( openDBOnServer( dbInfo, TaskPriority::DefaultEndpoint, true, lockAware ), KeyRef(metricsPrefix) );
		}
	}

	errorForwarders.add( resetAfter(degraded, SERVER_KNOBS->DEGRADED_RESET_INTERVAL, false, SERVER_KNOBS->DEGRADED_WARNING_LIMIT, SERVER_KNOBS->DEGRADED_WARNING_RESET_DELAY, "DegradedReset"));
	errorForwarders.add( loadedPonger( interf.debugPing.getFuture() ) );
	errorForwarders.add( waitFailureServer( interf.waitFailure.getFuture() ) );
	errorForwarders.add( monitorTraceLogIssues(issues) );
	errorForwarders.add( testerServerCore( interf.testerInterface, connFile, dbInfo, locality ) );
	errorForwarders.add(monitorHighMemory(memoryProfileThreshold));

	filesClosed.add(stopping.getFuture());

	initializeSystemMonitorMachineState(SystemMonitorMachineState(folder, locality.zoneId(), locality.machineId(), g_network->getLocalAddress().ip));

	{
		auto recruited = interf;  //ghetto! don't we all love a good #define
		DUMPTOKEN(recruited.clientInterface.reboot);
		DUMPTOKEN(recruited.clientInterface.profiler);
		DUMPTOKEN(recruited.tLog);
		DUMPTOKEN(recruited.master);
		DUMPTOKEN(recruited.masterProxy);
		DUMPTOKEN(recruited.resolver);
		DUMPTOKEN(recruited.storage);
		DUMPTOKEN(recruited.debugPing);
		DUMPTOKEN(recruited.coordinationPing);
		DUMPTOKEN(recruited.waitFailure);
		DUMPTOKEN(recruited.setMetricsRate);
		DUMPTOKEN(recruited.eventLogRequest);
		DUMPTOKEN(recruited.traceBatchDumpRequest);
		DUMPTOKEN(recruited.updateServerDBInfo);
	}

	state std::vector<Future<Void>> recoveries;

	try {
		std::vector<DiskStore> stores = getDiskStores( folder );
		bool validateDataFiles = deleteFile(joinPath(folder, validationFilename));
		for( int f = 0; f < stores.size(); f++ ) {
			DiskStore s = stores[f];
			// FIXME: Error handling
			if( s.storedComponent == DiskStore::Storage ) {
				IKeyValueStore* kv = openKVStore(s.storeType, s.filename, s.storeID, memoryLimit, false, validateDataFiles);
				Future<Void> kvClosed = kv->onClosed();
				filesClosed.add( kvClosed );

				StorageServerInterface recruited;
				recruited.uniqueID = s.storeID;
				recruited.locality = locality;
				recruited.initEndpoints();

				std::map<std::string, std::string> details;
				details["StorageEngine"] = s.storeType.toString();
				startRole( Role::STORAGE_SERVER, recruited.id(), interf.id(), details, "Restored" );

				DUMPTOKEN(recruited.getValue);
				DUMPTOKEN(recruited.getKey);
				DUMPTOKEN(recruited.getKeyValues);
				DUMPTOKEN(recruited.getShardState);
				DUMPTOKEN(recruited.waitMetrics);
				DUMPTOKEN(recruited.splitMetrics);
				DUMPTOKEN(recruited.getReadHotRanges);
				DUMPTOKEN(recruited.getStorageMetrics);
				DUMPTOKEN(recruited.waitFailure);
				DUMPTOKEN(recruited.getQueuingMetrics);
				DUMPTOKEN(recruited.getKeyValueStoreType);
				DUMPTOKEN(recruited.watchValue);

				Promise<Void> recovery;
				Future<Void> f = storageServer( kv, recruited, dbInfo, folder, recovery, connFile);
				recoveries.push_back(recovery.getFuture());
				f = handleIOErrors( f, kv, s.storeID, kvClosed );
				f = storageServerRollbackRebooter( f, s.storeType, s.filename, recruited.id(), recruited.locality, dbInfo, folder, &filesClosed, memoryLimit, kv);
				errorForwarders.add( forwardError( errors, Role::STORAGE_SERVER, recruited.id(), f ) );
			} else if( s.storedComponent == DiskStore::TLogData ) {
				std::string logQueueBasename;
				const std::string filename = basename(s.filename);
				if (StringRef(filename).startsWith(fileLogDataPrefix)) {
					logQueueBasename = fileLogQueuePrefix.toString();
				} else {
					StringRef optionsString = StringRef(filename).removePrefix(fileVersionedLogDataPrefix).eat("-");
					logQueueBasename = fileLogQueuePrefix.toString() + optionsString.toString() + "-";
				}
				ASSERT_WE_THINK( abspath(parentDirectory(s.filename)) == folder );
				IKeyValueStore* kv = openKVStore( s.storeType, s.filename, s.storeID, memoryLimit, validateDataFiles );
				const DiskQueueVersion dqv = s.tLogOptions.version >= TLogVersion::V3 ? DiskQueueVersion::V1 : DiskQueueVersion::V0;
				const int64_t diskQueueWarnSize = s.tLogOptions.spillType == TLogSpillType::VALUE ? 10*SERVER_KNOBS->TARGET_BYTES_PER_TLOG : -1;
				IDiskQueue* queue = openDiskQueue(
					joinPath( folder, logQueueBasename + s.storeID.toString() + "-"), tlogQueueExtension.toString(), s.storeID, dqv, diskQueueWarnSize);
				filesClosed.add( kv->onClosed() );
				filesClosed.add( queue->onClosed() );

				std::map<std::string, std::string> details;
				details["StorageEngine"] = s.storeType.toString();
				startRole( Role::SHARED_TRANSACTION_LOG, s.storeID, interf.id(), details, "Restored" );

				Promise<Void> oldLog;
				Promise<Void> recovery;
				TLogFn tLogFn = tLogFnForOptions(s.tLogOptions);
				auto& logData = sharedLogs[SharedLogsKey(s.tLogOptions, s.storeType)];
				// FIXME: Shouldn't if logData.first isValid && !isReady, shouldn't we
				// be sending a fake InitializeTLogRequest rather than calling tLog() ?
				Future<Void> tl = tLogFn( kv, queue, dbInfo, locality, !logData.actor.isValid() || logData.actor.isReady() ? logData.requests : PromiseStream<InitializeTLogRequest>(), s.storeID, interf.id(), true, oldLog, recovery, folder, degraded, activeSharedTLog );
				recoveries.push_back(recovery.getFuture());
				activeSharedTLog->set(s.storeID);

				tl = handleIOErrors( tl, kv, s.storeID );
				tl = handleIOErrors( tl, queue, s.storeID );
				if(!logData.actor.isValid() || logData.actor.isReady()) {
					logData.actor = oldLog.getFuture() || tl;
					logData.uid = s.storeID;
				}
				errorForwarders.add( forwardError( errors, Role::SHARED_TRANSACTION_LOG, s.storeID, tl ) );
			}
		}

		std::map<std::string, std::string> details;
		details["Locality"] = locality.toString();
		details["DataFolder"] = folder;
		details["StoresPresent"] = format("%d", stores.size());
		startRole( Role::WORKER, interf.id(), interf.id(), details );
		errorForwarders.add(traceRole(Role::WORKER, interf.id()));

		wait(waitForAll(recoveries));
		recoveredDiskFiles.send(Void());

		errorForwarders.add( registrationClient( ccInterface, interf, asyncPriorityInfo, initialClass, ddInterf, rkInterf, degraded, errors, locality, dbInfo, connFile, issues) );

		TraceEvent("RecoveriesComplete", interf.id());

		loop choose {
			when( UpdateServerDBInfoRequest req = waitNext( interf.updateServerDBInfo.getFuture() ) ) {
				ServerDBInfo localInfo = BinaryReader::fromStringRef<ServerDBInfo>(req.serializedDbInfo, AssumeVersion(currentProtocolVersion));
				localInfo.myLocality = locality;

				if(localInfo.infoGeneration < dbInfo->get().infoGeneration && localInfo.clusterInterface == dbInfo->get().clusterInterface) {
					std::vector<Endpoint> rep = req.broadcastInfo;
					rep.push_back(interf.updateServerDBInfo.getEndpoint());
					req.reply.send(rep);
				} else {
					Optional<Endpoint> notUpdated;
					if(!ccInterface->get().present() || localInfo.clusterInterface != ccInterface->get().get()) {
						notUpdated = interf.updateServerDBInfo.getEndpoint();
					}
					else if(localInfo.infoGeneration > dbInfo->get().infoGeneration || dbInfo->get().clusterInterface != ccInterface->get().get()) {

						TraceEvent("GotServerDBInfoChange").detail("ChangeID", localInfo.id).detail("MasterID", localInfo.master.id())
						.detail("RatekeeperID", localInfo.ratekeeper.present() ? localInfo.ratekeeper.get().id() : UID())
						.detail("DataDistributorID", localInfo.distributor.present() ? localInfo.distributor.get().id() : UID());
						dbInfo->set(localInfo);
					}
					errorForwarders.add(success(broadcastDBInfoRequest(req, SERVER_KNOBS->DBINFO_SEND_AMOUNT, notUpdated, true)));
				}
			}
			when( RebootRequest req = waitNext( interf.clientInterface.reboot.getFuture() ) ) {
				state RebootRequest rebootReq = req;
				// If suspendDuration is INT_MAX, the trace will not be logged if it was inside the next block
				// Also a useful trace to have even if suspendDuration is 0
				TraceEvent("RebootRequestSuspendingProcess").detail("Duration", req.waitForDuration);
				if(req.waitForDuration) {
					flushTraceFileVoid();
					setProfilingEnabled(0);
					g_network->stop();
					threadSleep(req.waitForDuration);
				}
				if(rebootReq.checkData) {
					Reference<IAsyncFile> checkFile = wait( IAsyncFileSystem::filesystem()->open( joinPath(folder, validationFilename), IAsyncFile::OPEN_CREATE | IAsyncFile::OPEN_READWRITE, 0600 ) );
					wait( checkFile->sync() );
				}

				if(g_network->isSimulated()) {
					TraceEvent("SimulatedReboot").detail("Deletion", rebootReq.deleteData );
					if( rebootReq.deleteData ) {
						throw please_reboot_delete();
					}
					throw please_reboot();
				}
				else {
					TraceEvent("ProcessReboot");
					ASSERT(!rebootReq.deleteData);
					flushAndExit(0);
				}
			}
			when( ProfilerRequest req = waitNext(interf.clientInterface.profiler.getFuture()) ) {
				state ProfilerRequest profilerReq = req;
				// There really isn't a great "filepath sanitizer" or "filepath escape" function available,
				// thus we instead enforce a different requirement.  One can only write to a file that's
				// beneath the working directory, and we remove the ability to do any symlink or ../..
				// tricks by resolving all paths through `abspath` first.
				try {
					std::string realLogDir = abspath(SERVER_KNOBS->LOG_DIRECTORY);
					std::string realOutPath = abspath(realLogDir + "/" + profilerReq.outputFile.toString());
					if (realLogDir.size() < realOutPath.size() &&
					    strncmp(realLogDir.c_str(), realOutPath.c_str(), realLogDir.size()) == 0) {
						profilerReq.outputFile = realOutPath;
						uncancellable(runProfiler(profilerReq));
						profilerReq.reply.send(Void());
					} else {
						profilerReq.reply.sendError(client_invalid_operation());
					}
				} catch (Error& e) {
					profilerReq.reply.sendError(e);
				}
			}
			when( RecruitMasterRequest req = waitNext(interf.master.getFuture()) ) {
				MasterInterface recruited;
				recruited.locality = locality;
				recruited.initEndpoints();

				startRole( Role::MASTER, recruited.id(), interf.id() );

				DUMPTOKEN( recruited.waitFailure );
				DUMPTOKEN( recruited.tlogRejoin );
				DUMPTOKEN( recruited.changeCoordinators );
				DUMPTOKEN( recruited.getCommitVersion );

				//printf("Recruited as masterServer\n");
				Future<Void> masterProcess = masterServer( recruited, dbInfo, ccInterface, ServerCoordinators( connFile ), req.lifetime, req.forceRecovery );
				errorForwarders.add( zombie(recruited, forwardError( errors, Role::MASTER, recruited.id(), masterProcess )) );
				req.reply.send(recruited);
			}
			when ( InitializeDataDistributorRequest req = waitNext(interf.dataDistributor.getFuture()) ) {
				DataDistributorInterface recruited(locality);
				recruited.initEndpoints();

				if ( ddInterf->get().present() ) {
					recruited = ddInterf->get().get();
					TEST(true);  // Recruited while already a data distributor.
				} else {
					startRole( Role::DATA_DISTRIBUTOR, recruited.id(), interf.id() );
					DUMPTOKEN( recruited.waitFailure );

					Future<Void> dataDistributorProcess = dataDistributor( recruited, dbInfo );
					errorForwarders.add( forwardError( errors, Role::DATA_DISTRIBUTOR, recruited.id(), setWhenDoneOrError( dataDistributorProcess, ddInterf, Optional<DataDistributorInterface>() ) ) );
					ddInterf->set(Optional<DataDistributorInterface>(recruited));
				}
				TraceEvent("DataDistributorReceived", req.reqId).detail("DataDistributorId", recruited.id());
				req.reply.send(recruited);
			}
			when ( InitializeRatekeeperRequest req = waitNext(interf.ratekeeper.getFuture()) ) {
				RatekeeperInterface recruited(locality, req.reqId);
				recruited.initEndpoints();

				if (rkInterf->get().present()) {
					recruited = rkInterf->get().get();
					TEST(true);  // Recruited while already a ratekeeper.
				} else {
					startRole(Role::RATEKEEPER, recruited.id(), interf.id());
					DUMPTOKEN( recruited.waitFailure );
					DUMPTOKEN( recruited.getRateInfo );
					DUMPTOKEN( recruited.haltRatekeeper );

					Future<Void> ratekeeperProcess = ratekeeper(recruited, dbInfo);
					errorForwarders.add(
					    forwardError(errors, Role::RATEKEEPER, recruited.id(),
					                 setWhenDoneOrError(ratekeeperProcess, rkInterf, Optional<RatekeeperInterface>())));
					rkInterf->set(Optional<RatekeeperInterface>(recruited));
				}
				TraceEvent("Ratekeeper_InitRequest", req.reqId).detail("RatekeeperId", recruited.id());
				req.reply.send(recruited);
			}
			when (InitializeBackupRequest req = waitNext(interf.backup.getFuture())) {
				if (!backupWorkerCache.exists(req.reqId)) {
					BackupInterface recruited(locality);
					recruited.initEndpoints();

					startRole(Role::BACKUP, recruited.id(), interf.id());
					DUMPTOKEN(recruited.waitFailure);

					ReplyPromise<InitializeBackupReply> backupReady = req.reply;
					backupWorkerCache.set(req.reqId, backupReady.getFuture());
					Future<Void> backupProcess = backupWorker(recruited, req, dbInfo);
					backupProcess = storageCache.removeOnReady(req.reqId, backupProcess);
					errorForwarders.add(forwardError(errors, Role::BACKUP, recruited.id(), backupProcess));
					TraceEvent("BackupInitRequest", req.reqId).detail("BackupId", recruited.id());
					InitializeBackupReply reply(recruited, req.backupEpoch);
					backupReady.send(reply);
				} else {
					forwardPromise(req.reply, backupWorkerCache.get(req.reqId));
				}
			}
			when( InitializeTLogRequest req = waitNext(interf.tLog.getFuture()) ) {
				// For now, there's a one-to-one mapping of spill type to TLogVersion.
				// With future work, a particular version of the TLog can support multiple
				// different spilling strategies, at which point SpillType will need to be
				// plumbed down into tLogFn.
				if (req.logVersion < TLogVersion::MIN_RECRUITABLE) {
					TraceEvent(SevError, "InitializeTLogInvalidLogVersion")
						.detail("Version", req.logVersion)
						.detail("MinRecruitable", TLogVersion::MIN_RECRUITABLE);
					req.reply.sendError(internal_error());
				}
				TLogOptions tLogOptions(req.logVersion, req.spillType);
				TLogFn tLogFn = tLogFnForOptions(tLogOptions);
				auto& logData = sharedLogs[SharedLogsKey(tLogOptions, req.storeType)];
				logData.requests.send(req);
				if(!logData.actor.isValid() || logData.actor.isReady()) {
					UID logId = deterministicRandom()->randomUniqueID();
					std::map<std::string, std::string> details;
					details["ForMaster"] = req.recruitmentID.shortString();
					details["StorageEngine"] = req.storeType.toString();

					//FIXME: start role for every tlog instance, rather that just for the shared actor, also use a different role type for the shared actor
					startRole( Role::SHARED_TRANSACTION_LOG, logId, interf.id(), details );

					const StringRef prefix = req.logVersion > TLogVersion::V2 ? fileVersionedLogDataPrefix : fileLogDataPrefix;
					std::string filename = filenameFromId( req.storeType, folder, prefix.toString() + tLogOptions.toPrefix(), logId );
					IKeyValueStore* data = openKVStore( req.storeType, filename, logId, memoryLimit );
					const DiskQueueVersion dqv = tLogOptions.version >= TLogVersion::V3 ? DiskQueueVersion::V1 : DiskQueueVersion::V0;
					IDiskQueue* queue = openDiskQueue( joinPath( folder, fileLogQueuePrefix.toString() + tLogOptions.toPrefix() + logId.toString() + "-" ), tlogQueueExtension.toString(), logId, dqv );
					filesClosed.add( data->onClosed() );
					filesClosed.add( queue->onClosed() );

					Future<Void> tLogCore = tLogFn( data, queue, dbInfo, locality, logData.requests, logId, interf.id(), false, Promise<Void>(), Promise<Void>(), folder, degraded, activeSharedTLog );
					tLogCore = handleIOErrors( tLogCore, data, logId );
					tLogCore = handleIOErrors( tLogCore, queue, logId );
					errorForwarders.add( forwardError( errors, Role::SHARED_TRANSACTION_LOG, logId, tLogCore ) );
					logData.actor = tLogCore;
					logData.uid = logId;
				}
				activeSharedTLog->set(logData.uid);
			}
			when( InitializeStorageRequest req = waitNext(interf.storage.getFuture()) ) {
				if( !storageCache.exists( req.reqId ) ) {
					StorageServerInterface recruited(req.interfaceId);
					recruited.locality = locality;
					recruited.initEndpoints();

					std::map<std::string, std::string> details;
					details["StorageEngine"] = req.storeType.toString();
					startRole( Role::STORAGE_SERVER, recruited.id(), interf.id(), details );

					DUMPTOKEN(recruited.getValue);
					DUMPTOKEN(recruited.getKey);
					DUMPTOKEN(recruited.getKeyValues);
					DUMPTOKEN(recruited.getShardState);
					DUMPTOKEN(recruited.waitMetrics);
					DUMPTOKEN(recruited.splitMetrics);
					DUMPTOKEN(recruited.getReadHotRanges);
					DUMPTOKEN(recruited.getStorageMetrics);
					DUMPTOKEN(recruited.waitFailure);
					DUMPTOKEN(recruited.getQueuingMetrics);
					DUMPTOKEN(recruited.getKeyValueStoreType);
					DUMPTOKEN(recruited.watchValue);
					//printf("Recruited as storageServer\n");

					std::string filename = filenameFromId( req.storeType, folder, fileStoragePrefix.toString(), recruited.id() );
					IKeyValueStore* data = openKVStore( req.storeType, filename, recruited.id(), memoryLimit );
					Future<Void> kvClosed = data->onClosed();
					filesClosed.add( kvClosed );
					ReplyPromise<InitializeStorageReply> storageReady = req.reply;
					storageCache.set( req.reqId, storageReady.getFuture() );
					Future<Void> s = storageServer( data, recruited, req.seedTag, storageReady, dbInfo, folder );
					s = handleIOErrors(s, data, recruited.id(), kvClosed);
					s = storageCache.removeOnReady( req.reqId, s );
					s = storageServerRollbackRebooter( s, req.storeType, filename, recruited.id(), recruited.locality, dbInfo, folder, &filesClosed, memoryLimit, data );
					errorForwarders.add( forwardError( errors, Role::STORAGE_SERVER, recruited.id(), s ) );
				} else
					forwardPromise( req.reply, storageCache.get( req.reqId ) );
			}
			when( InitializeMasterProxyRequest req = waitNext(interf.masterProxy.getFuture()) ) {
				MasterProxyInterface recruited;
				recruited.processId = locality.processId();
				recruited.provisional = false;
				recruited.initEndpoints();

				std::map<std::string, std::string> details;
				details["ForMaster"] = req.master.id().shortString();
				startRole( Role::MASTER_PROXY, recruited.id(), interf.id(), details );

				DUMPTOKEN(recruited.commit);
				DUMPTOKEN(recruited.getConsistentReadVersion);
				DUMPTOKEN(recruited.getKeyServersLocations);
				DUMPTOKEN(recruited.getStorageServerRejoinInfo);
				DUMPTOKEN(recruited.waitFailure);
				DUMPTOKEN(recruited.getRawCommittedVersion);
				DUMPTOKEN(recruited.txnState);

				//printf("Recruited as masterProxyServer\n");
				errorForwarders.add( zombie(recruited, forwardError( errors, Role::MASTER_PROXY, recruited.id(),
						masterProxyServer( recruited, req, dbInfo, whitelistBinPaths ) ) ) );
				req.reply.send(recruited);
			}
			when( InitializeResolverRequest req = waitNext(interf.resolver.getFuture()) ) {
				ResolverInterface recruited;
				recruited.locality = locality;
				recruited.initEndpoints();

				std::map<std::string, std::string> details;
				startRole( Role::RESOLVER, recruited.id(), interf.id(), details );

				DUMPTOKEN(recruited.resolve);
				DUMPTOKEN(recruited.metrics);
				DUMPTOKEN(recruited.split);
				DUMPTOKEN(recruited.waitFailure);

				errorForwarders.add( zombie(recruited, forwardError( errors, Role::RESOLVER, recruited.id(),
						resolver( recruited, req, dbInfo ) ) ) );
				req.reply.send(recruited);
			}
			when( InitializeLogRouterRequest req = waitNext(interf.logRouter.getFuture()) ) {
				TLogInterface recruited(locality);
				recruited.initEndpoints();

				std::map<std::string, std::string> details;
				startRole( Role::LOG_ROUTER, recruited.id(), interf.id(), details );

				DUMPTOKEN( recruited.peekMessages );
				DUMPTOKEN( recruited.popMessages );
				DUMPTOKEN( recruited.commit );
				DUMPTOKEN( recruited.lock );
				DUMPTOKEN( recruited.getQueuingMetrics );
				DUMPTOKEN( recruited.confirmRunning );
				DUMPTOKEN( recruited.waitFailure );
				DUMPTOKEN( recruited.recoveryFinished );
				DUMPTOKEN( recruited.disablePopRequest );
				DUMPTOKEN( recruited.enablePopRequest );
				DUMPTOKEN( recruited.snapRequest );

				errorForwarders.add( zombie(recruited, forwardError( errors, Role::LOG_ROUTER, recruited.id(),
						logRouter( recruited, req, dbInfo ) ) ) );
				req.reply.send(recruited);
			}
			when( CoordinationPingMessage m = waitNext( interf.coordinationPing.getFuture() ) ) {
				TraceEvent("CoordinationPing", interf.id()).detail("CCID", m.clusterControllerId).detail("TimeStep", m.timeStep);
			}
			when( SetMetricsLogRateRequest req = waitNext(interf.setMetricsRate.getFuture()) ) {
				TraceEvent("LoggingRateChange", interf.id()).detail("OldDelay", loggingDelay).detail("NewLogPS", req.metricsLogsPerSecond);
				if( req.metricsLogsPerSecond != 0 ) {
					loggingDelay = 1.0 / req.metricsLogsPerSecond;
					loggingTrigger = Void();
				}
			}
			when( EventLogRequest req = waitNext(interf.eventLogRequest.getFuture()) ) {
				TraceEventFields e;
				if( req.getLastError )
					e = latestEventCache.getLatestError();
				else
					e = latestEventCache.get( req.eventName.toString() );
				req.reply.send(e);
			}
			when( TraceBatchDumpRequest req = waitNext(interf.traceBatchDumpRequest.getFuture()) ) {
				g_traceBatch.dump();
				req.reply.send(Void());
			}
			when( DiskStoreRequest req = waitNext(interf.diskStoreRequest.getFuture()) ) {
				Standalone<VectorRef<UID>> ids;
				for(DiskStore d : getDiskStores(folder)) {
					bool included = true;
					if(!req.includePartialStores) {
						if(d.storeType == KeyValueStoreType::SSD_BTREE_V1) {
							included = fileExists(d.filename + ".fdb-wal");
						}
						else if (d.storeType == KeyValueStoreType::SSD_BTREE_V2) {
							included = fileExists(d.filename + ".sqlite-wal");
						}
						else if (d.storeType == KeyValueStoreType::SSD_REDWOOD_V1) {
							included = fileExists(d.filename + "0.pagerlog") && fileExists(d.filename + "1.pagerlog");
						}
						else if (d.storeType == KeyValueStoreType::SSD_ROCKSDB_V1) {
							included = fileExists(joinPath(d.filename, "CURRENT")) && fileExists(joinPath(d.filename, "IDENTITY"));
						} else if (d.storeType == KeyValueStoreType::MEMORY) {
							included = fileExists(d.filename + "1.fdq");
						} else {
							ASSERT(d.storeType == KeyValueStoreType::MEMORY_RADIXTREE);
							included = fileExists(d.filename + "1.fdr");
						}
						if(d.storedComponent == DiskStore::COMPONENT::TLogData && included) {
							included = false;
							// The previous code assumed that d.filename is a filename.  But that is not true.
							// d.filename is a path. Removing a prefix and adding a new one just makes a broken
							// directory name.  So fileExists would always return false.
							// Weirdly, this doesn't break anything, as tested by taking a clean check of FDB,
							// setting included to false always, and then running correctness.  So I'm just
							// improving the situation by actually marking it as broken.
							// FIXME: this whole thing
							/*
							std::string logDataBasename;
							StringRef filename = d.filename;
							if (filename.startsWith(fileLogDataPrefix)) {
								logDataBasename = fileLogQueuePrefix.toString() + d.filename.substr(fileLogDataPrefix.size());
							} else {
								StringRef optionsString = filename.removePrefix(fileVersionedLogDataPrefix).eat("-");
								logDataBasename = fileLogQueuePrefix.toString() + optionsString.toString() + "-";
							}
							TraceEvent("DiskStoreRequest").detail("FilenameBasename", logDataBasename);
							if (fileExists(logDataBasename + "0.fdq") && fileExists(logDataBasename + "1.fdq")) {
								included = true;
							}
							*/
						}
					}
					if(included) {
						ids.push_back(ids.arena(), d.storeID);
					}
				}
				req.reply.send(ids);
			}
			when( wait( loggingTrigger ) ) {
				systemMonitor();
				loggingTrigger = delay( loggingDelay, TaskPriority::FlushTrace );
			}
			when(state WorkerSnapRequest snapReq = waitNext(interf.workerSnapReq.getFuture())) {
				Standalone<StringRef> snapFolder = StringRef(folder);
				if (snapReq.role.toString() == "coord") {
					snapFolder = coordFolder;
				}
				errorForwarders.add(workerSnapCreate(snapReq, snapFolder));
			}
			when( wait( errorForwarders.getResult() ) ) {}
			when( wait( handleErrors ) ) {}
		}
	} catch (Error& err) {
		// Make sure actors are cancelled before "recovery" promises are destructed.
		for (auto f : recoveries) f.cancel();
		state Error e = err;
		bool ok = e.code() == error_code_please_reboot || e.code() == error_code_actor_cancelled || e.code() == error_code_please_reboot_delete;

		endRole(Role::WORKER, interf.id(), "WorkerError", ok, e);
		errorForwarders.clear(false);
		sharedLogs.clear();

		if (e.code() != error_code_actor_cancelled) { // We get cancelled e.g. when an entire simulation times out, but in that case we won't be restarted and don't need to wait for shutdown
			stopping.send(Void());
			wait( filesClosed.getResult() ); // Wait for complete shutdown of KV stores
			wait(delay(0.0)); //Unwind the callstack to make sure that IAsyncFile references are all gone
			TraceEvent(SevInfo, "WorkerShutdownComplete", interf.id());
		}

		throw e;
	}
}

ACTOR Future<Void> extractClusterInterface( Reference<AsyncVar<Optional<ClusterControllerFullInterface>>> a, Reference<AsyncVar<Optional<ClusterInterface>>> b ) {
	loop {
		if(a->get().present())
			b->set(a->get().get().clientInterface);
		else
			b->set(Optional<ClusterInterface>());
		wait(a->onChange());
	}
}

static std::set<int> const& normalWorkerErrors() {
	static std::set<int> s;
	if (s.empty()) {
		s.insert( error_code_please_reboot );
		s.insert( error_code_please_reboot_delete );
	}
	return s;
}

ACTOR Future<Void> fileNotFoundToNever(Future<Void> f) {
	try {
		wait(f);
		return Void();
	} catch (Error& e) {
		if (e.code() == error_code_file_not_found) {
			TraceEvent(SevWarn, "ClusterCoordinatorFailed").error(e);
			return Never();
		}
		throw;
	}
}

ACTOR Future<Void> printTimeout() {
	wait( delay(5) );
	if( !g_network->isSimulated() ) {
		fprintf(stderr, "Warning: FDBD has not joined the cluster after 5 seconds.\n");
		fprintf(stderr, "  Check configuration and availability using the 'status' command with the fdbcli\n");
	}
	return Void();
}

ACTOR Future<Void> printOnFirstConnected( Reference<AsyncVar<Optional<ClusterInterface>>> ci ) {
	state Future<Void> timeoutFuture = printTimeout();
	loop {
		choose {
			when (wait( ci->get().present() ? IFailureMonitor::failureMonitor().onStateEqual( ci->get().get().openDatabase.getEndpoint(), FailureStatus(false) ) : Never() )) {
				printf("FDBD joined cluster.\n");
				TraceEvent("FDBDConnected");
				return Void();
			}
			when( wait(ci->onChange())) {}
		}
	}
}

ClusterControllerPriorityInfo getCCPriorityInfo(std::string filePath, ProcessClass processClass) {
	if (!fileExists(filePath))
		return ClusterControllerPriorityInfo(ProcessClass(processClass.classType(), ProcessClass::CommandLineSource).machineClassFitness(ProcessClass::ClusterController), false, ClusterControllerPriorityInfo::FitnessUnknown);
	std::string contents(readFileBytes(filePath, 1000));
	BinaryReader br(StringRef(contents), IncludeVersion());
	ClusterControllerPriorityInfo priorityInfo(ProcessClass::UnsetFit, false, ClusterControllerPriorityInfo::FitnessUnknown);
	br >> priorityInfo;
	if (!br.empty()) {
		if (g_network->isSimulated()) {
			ASSERT(false);
		}
		else {
			TraceEvent(SevWarnAlways, "FitnessFileCorrupted").detail("filePath", filePath);
			return ClusterControllerPriorityInfo(ProcessClass(processClass.classType(), ProcessClass::CommandLineSource).machineClassFitness(ProcessClass::ClusterController), false, ClusterControllerPriorityInfo::FitnessUnknown);
		}
	}
	return priorityInfo;
}

ACTOR Future<Void> monitorAndWriteCCPriorityInfo(std::string filePath, Reference<AsyncVar<ClusterControllerPriorityInfo>> asyncPriorityInfo) {
	loop {
		wait(asyncPriorityInfo->onChange());
		std::string contents(BinaryWriter::toValue(asyncPriorityInfo->get(), IncludeVersion(ProtocolVersion::withClusterControllerPriorityInfo())).toString());
		atomicReplace(filePath, contents, false);
	}
}

ACTOR Future<UID> createAndLockProcessIdFile(std::string folder) {
	state UID processIDUid;
	platform::createDirectory(folder);

	loop {
		try {
			state std::string lockFilePath = joinPath(folder, "processId");
			state ErrorOr<Reference<IAsyncFile>> lockFile = wait(errorOr(IAsyncFileSystem::filesystem(g_network)->open(lockFilePath, IAsyncFile::OPEN_READWRITE | IAsyncFile::OPEN_LOCK, 0600)));

			if (lockFile.isError() && lockFile.getError().code() == error_code_file_not_found && !fileExists(lockFilePath)) {
				Reference<IAsyncFile> _lockFile = wait(IAsyncFileSystem::filesystem()->open(lockFilePath, IAsyncFile::OPEN_ATOMIC_WRITE_AND_CREATE | IAsyncFile::OPEN_CREATE | IAsyncFile::OPEN_LOCK | IAsyncFile::OPEN_READWRITE, 0600));
				lockFile = _lockFile;
				processIDUid = deterministicRandom()->randomUniqueID();
				BinaryWriter wr(IncludeVersion(ProtocolVersion::withProcessIDFile()));
				wr << processIDUid;
				wait(lockFile.get()->write(wr.getData(), wr.getLength(), 0));
				wait(lockFile.get()->sync());
			}
			else {
				if (lockFile.isError()) throw lockFile.getError(); // If we've failed to open the file, throw an exception

				int64_t fileSize = wait(lockFile.get()->size());
				state Key fileData = makeString(fileSize);
				wait(success(lockFile.get()->read(mutateString(fileData), fileSize, 0)));
				try {
					processIDUid = BinaryReader::fromStringRef<UID>(fileData, IncludeVersion());
					return processIDUid;
				} catch (Error& e) {
					if(!g_network->isSimulated()) {
						throw;
					}
					lockFile = ErrorOr<Reference<IAsyncFile>>();
					wait(IAsyncFileSystem::filesystem()->deleteFile(lockFilePath, true));
				}
			}
		}
		catch (Error& e) {
			if (e.code() == error_code_actor_cancelled) {
				throw;
			}
			if (!e.isInjectedFault()) {
				fprintf(stderr, "ERROR: error creating or opening process id file `%s'.\n", joinPath(folder, "processId").c_str());
			}
			TraceEvent(SevError, "OpenProcessIdError").error(e);
			throw;
		}
	}
}

ACTOR Future<MonitorLeaderInfo> monitorLeaderRemotelyOneGeneration( Reference<ClusterConnectionFile> connFile, Reference<AsyncVar<Value>> result, MonitorLeaderInfo info ) {
	state ClusterConnectionString ccf = info.intermediateConnFile->getConnectionString();
	state vector<NetworkAddress> addrs = ccf.coordinators();
	state ElectionResultRequest request;
	state int index = 0;
	state int successIndex = 0;
	request.key = ccf.clusterKey();
	request.coordinators = ccf.coordinators();

	deterministicRandom()->randomShuffle(addrs);

	loop {
		LeaderElectionRegInterface interf( addrs[index] );
		request.reply = ReplyPromise<Optional<LeaderInfo>>();

		ErrorOr<Optional<LeaderInfo>> leader = wait( interf.electionResult.tryGetReply( request ) );
		if (leader.present()) {
			if(leader.get().present()) {
				if( leader.get().get().forward ) {
					info.intermediateConnFile = Reference<ClusterConnectionFile>(new ClusterConnectionFile(connFile->getFilename(), ClusterConnectionString(leader.get().get().serializedInfo.toString())));
					return info;
				}
				if(connFile != info.intermediateConnFile) {
					if(!info.hasConnected) {
						TraceEvent(SevWarnAlways, "IncorrectClusterFileContentsAtConnection").detail("Filename", connFile->getFilename())
							.detail("ConnectionStringFromFile", connFile->getConnectionString().toString())
							.detail("CurrentConnectionString", info.intermediateConnFile->getConnectionString().toString());
					}
					connFile->setConnectionString(info.intermediateConnFile->getConnectionString());
					info.intermediateConnFile = connFile;
				}

				info.hasConnected = true;
				connFile->notifyConnected();
				request.knownLeader = leader.get().get().changeID;

				ClusterControllerPriorityInfo info = leader.get().get().getPriorityInfo();
				if( leader.get().get().serializedInfo.size() && !info.isExcluded &&
				    (info.dcFitness == ClusterControllerPriorityInfo::FitnessPrimary ||
				     info.dcFitness == ClusterControllerPriorityInfo::FitnessPreferred ||
				     info.dcFitness == ClusterControllerPriorityInfo::FitnessUnknown)) {
					result->set(leader.get().get().serializedInfo);
				} else {
					result->set(Value());
				}
			}
			successIndex = index;
		} else {
			index = (index+1) % addrs.size();
			if (index == successIndex) {
				wait( delay( CLIENT_KNOBS->COORDINATOR_RECONNECTION_DELAY ) );
			}
		}
	}
}

ACTOR Future<Void> monitorLeaderRemotelyInternal( Reference<ClusterConnectionFile> connFile, Reference<AsyncVar<Value>> outSerializedLeaderInfo ) {
	state MonitorLeaderInfo info(connFile);
	loop {
		MonitorLeaderInfo _info = wait( monitorLeaderRemotelyOneGeneration( connFile, outSerializedLeaderInfo, info ) );
		info = _info;
	}
}

template <class LeaderInterface>
Future<Void> monitorLeaderRemotely(Reference<ClusterConnectionFile> const& connFile,
						   Reference<AsyncVar<Optional<LeaderInterface>>> const& outKnownLeader) {
	LeaderDeserializer<LeaderInterface> deserializer;
	Reference<AsyncVar<Value>> serializedInfo( new AsyncVar<Value> );
	Future<Void> m = monitorLeaderRemotelyInternal( connFile, serializedInfo );
	return m || deserializer( serializedInfo, outKnownLeader );
}

ACTOR Future<Void> monitorLeaderRemotelyWithDelayedCandidacy( Reference<ClusterConnectionFile> connFile, Reference<AsyncVar<Optional<ClusterControllerFullInterface>>> currentCC, Reference<AsyncVar<ClusterControllerPriorityInfo>> asyncPriorityInfo, Future<Void> recoveredDiskFiles, LocalityData locality, Reference<AsyncVar<ServerDBInfo>> dbInfo ) {
	state Future<Void> monitor = monitorLeaderRemotely( connFile, currentCC );
	state Future<Void> timeout;

	wait(recoveredDiskFiles);

	loop {
		if(currentCC->get().present() && dbInfo->get().clusterInterface == currentCC->get().get() && IFailureMonitor::failureMonitor().getState( currentCC->get().get().registerWorker.getEndpoint() ).isAvailable()) {
			timeout = Future<Void>();
		} else if(!timeout.isValid()) {
			timeout = delay( SERVER_KNOBS->MIN_DELAY_CC_WORST_FIT_CANDIDACY_SECONDS + (deterministicRandom()->random01()*(SERVER_KNOBS->MAX_DELAY_CC_WORST_FIT_CANDIDACY_SECONDS-SERVER_KNOBS->MIN_DELAY_CC_WORST_FIT_CANDIDACY_SECONDS)) );
		}
		choose {
			when( wait(currentCC->onChange()) ) {}
			when( wait(dbInfo->onChange()) ) {}
			when( wait(currentCC->get().present() ? IFailureMonitor::failureMonitor().onStateChanged( currentCC->get().get().registerWorker.getEndpoint() ) : Never() ) ) {}
			when( wait(timeout.isValid() ? timeout : Never()) ) {
				monitor.cancel();
				wait( clusterController( connFile, currentCC , asyncPriorityInfo, recoveredDiskFiles, locality ) );
				return Void();
			}
		}
	}
}

ACTOR Future<Void> fdbd(
	Reference<ClusterConnectionFile> connFile,
	LocalityData localities,
	ProcessClass processClass,
	std::string dataFolder,
	std::string coordFolder,
	int64_t memoryLimit,
	std::string metricsConnFile,
	std::string metricsPrefix,
	int64_t memoryProfileThreshold,
	std::string whitelistBinPaths)
{
	state vector<Future<Void>> actors;
	state Promise<Void> recoveredDiskFiles;

	try {
		ServerCoordinators coordinators( connFile );
		if (g_network->isSimulated()) {
			whitelistBinPaths = ",, random_path,  /bin/snap_create.sh,,";
		}
		TraceEvent("StartingFDBD").detail("ZoneID", localities.zoneId()).detail("MachineId", localities.machineId()).detail("DiskPath", dataFolder).detail("CoordPath", coordFolder).detail("WhiteListBinPath", whitelistBinPaths);

		// SOMEDAY: start the services on the machine in a staggered fashion in simulation?
		// Endpoints should be registered first before any process trying to connect to it.
		// So coordinationServer actor should be the first one executed before any other.
		if (coordFolder.size()) {
			// SOMEDAY: remove the fileNotFound wrapper and make DiskQueue construction safe from errors setting up
			// their files
			actors.push_back(fileNotFoundToNever(coordinationServer(coordFolder)));
		}

		state UID processIDUid = wait(createAndLockProcessIdFile(dataFolder));
		localities.set(LocalityData::keyProcessId, processIDUid.toString());
		// Only one process can execute on a dataFolder from this point onwards

		std::string fitnessFilePath = joinPath(dataFolder, "fitness");
		Reference<AsyncVar<Optional<ClusterControllerFullInterface>>> cc(new AsyncVar<Optional<ClusterControllerFullInterface>>);
		Reference<AsyncVar<Optional<ClusterInterface>>> ci(new AsyncVar<Optional<ClusterInterface>>);
		Reference<AsyncVar<ClusterControllerPriorityInfo>> asyncPriorityInfo(new AsyncVar<ClusterControllerPriorityInfo>(getCCPriorityInfo(fitnessFilePath, processClass)));
		Reference<AsyncVar<ServerDBInfo>> dbInfo( new AsyncVar<ServerDBInfo>(ServerDBInfo()) );

		actors.push_back(reportErrors(monitorAndWriteCCPriorityInfo(fitnessFilePath, asyncPriorityInfo), "MonitorAndWriteCCPriorityInfo"));
		if (processClass.machineClassFitness(ProcessClass::ClusterController) == ProcessClass::NeverAssign) {
			actors.push_back( reportErrors( monitorLeader( connFile, cc ), "ClusterController" ) );
		} else if (processClass.machineClassFitness(ProcessClass::ClusterController) == ProcessClass::WorstFit && SERVER_KNOBS->MAX_DELAY_CC_WORST_FIT_CANDIDACY_SECONDS > 0) {
			actors.push_back( reportErrors( monitorLeaderRemotelyWithDelayedCandidacy( connFile, cc, asyncPriorityInfo, recoveredDiskFiles.getFuture(), localities, dbInfo ), "ClusterController" ) );
		} else {
			actors.push_back( reportErrors( clusterController( connFile, cc , asyncPriorityInfo, recoveredDiskFiles.getFuture(), localities ), "ClusterController") );
		}
		actors.push_back( reportErrors(extractClusterInterface( cc, ci ), "ExtractClusterInterface") );
		actors.push_back( reportErrorsExcept(workerServer(connFile, cc, localities, asyncPriorityInfo, processClass, dataFolder, memoryLimit, metricsConnFile, metricsPrefix, recoveredDiskFiles, memoryProfileThreshold, coordFolder, whitelistBinPaths, dbInfo), "WorkerServer", UID(), &normalWorkerErrors()) );
		state Future<Void> firstConnect = reportErrors( printOnFirstConnected(ci), "ClusterFirstConnectedError" );

		wait( quorum(actors,1) );
		ASSERT(false);  // None of these actors should terminate normally
		throw internal_error();
	} catch (Error& e) {
		// Make sure actors are cancelled before recoveredDiskFiles is destructed.
		// Otherwise, these actors may get a broken promise error.
		for (auto f : actors) f.cancel();
		Error err = checkIOTimeout(e);
		throw err;
	}
}

const Role Role::WORKER("Worker", "WK", false);
const Role Role::STORAGE_SERVER("StorageServer", "SS");
const Role Role::TRANSACTION_LOG("TLog", "TL");
const Role Role::SHARED_TRANSACTION_LOG("SharedTLog", "SL", false);
const Role Role::MASTER_PROXY("MasterProxyServer", "MP");
const Role Role::MASTER("MasterServer", "MS");
const Role Role::RESOLVER("Resolver", "RV");
const Role Role::CLUSTER_CONTROLLER("ClusterController", "CC");
const Role Role::TESTER("Tester", "TS");
const Role Role::LOG_ROUTER("LogRouter", "LR");
const Role Role::DATA_DISTRIBUTOR("DataDistributor", "DD");
const Role Role::RATEKEEPER("Ratekeeper", "RK");
const Role Role::STORAGE_CACHE("StorageCache", "SC");
const Role Role::COORDINATOR("Coordinator", "CD");
const Role Role::BACKUP("Backup", "BK");
