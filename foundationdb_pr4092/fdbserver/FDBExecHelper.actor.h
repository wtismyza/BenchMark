#pragma once
#if defined(NO_INTELLISENSE) && !defined(FDBSERVER_EXEC_HELPER_ACTOR_G_H)
	#define FDBSERVER_EXEC_HELPER_ACTOR_G_H
	#include "fdbserver/FDBExecHelper.actor.g.h"
#elif !defined(FDBSERVER_EXEC_HELPER_ACTOR_H)
	#define FDBSERVER_EXEC_HELPER_ACTOR_H

#include <string>
#include <vector>
#include <map>
#include "flow/Arena.h"
#include "flow/flow.h"
#include "flow/actorcompiler.h"
#include "fdbclient/FDBTypes.h"

// execute/snapshot command takes two arguments: <param1> <param2>
// param1 - represents the command type/name
// param2 - takes a binary path followed by a set of arguments in the following
// format <binary-path>:<key1=val1>,<key2=val2>...
// this class will abstract the format and give functions to get various pieces
// of information
class ExecCmdValueString {
public: // ctor & dtor
	ExecCmdValueString() {}
	explicit ExecCmdValueString(StringRef cmdValueString);

public: // interfaces
	StringRef getBinaryPath();
	VectorRef<StringRef> getBinaryArgs();
	void setCmdValueString(StringRef cmdValueString);
	StringRef getCmdValueString(void);

public: // helper functions
	void dbgPrint();

private: // functions
	void parseCmdValue();

private: // data
	Standalone<StringRef> cmdValueString;
	Standalone<VectorRef<StringRef>> binaryArgs;
	StringRef binaryPath;
};

// FIXME: move this function to a common location
// spawns a process pointed by `binPath` and the arguments provided at `paramList`,
// if the process spawned takes more than `maxWaitTime` then it will be killed
// if isSync is set to true then the process will be synchronously executed
// if async and in simulator then delay spawning the process to max of maxSimDelayTime
ACTOR Future<int> spawnProcess(std::string binPath, std::vector<std::string> paramList, double maxWaitTime, bool isSync, double maxSimDelayTime);

// helper to run all the work related to running the exec command
ACTOR Future<int> execHelper(ExecCmdValueString* execArg, UID snapUID, std::string folder, std::string role);

// set the data version for the specified storage server UID
void setDataVersion(UID uid, Version version);
// set the data durable version for the specified storage server UID
void setDataDurableVersion(UID uid, Version version);
// print the version info all the storages servers on this node
void printStorageVersionInfo();

#include "flow/unactorcompiler.h"
#endif
