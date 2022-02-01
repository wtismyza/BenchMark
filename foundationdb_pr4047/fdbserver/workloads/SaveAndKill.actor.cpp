/*
 * SaveAndKill.actor.cpp
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
#include "fdbrpc/simulator.h"

#undef state
#include "fdbmonitor/SimpleIni.h"
#define state
#undef max
#undef min
#include "flow/actorcompiler.h"  // This must be the last #include.

struct SaveAndKillWorkload : TestWorkload {

	std::string restartInfo;
	double testDuration;
	int isRestoring;

	SaveAndKillWorkload(WorkloadContext const& wcx)
		: TestWorkload(wcx)
	{
		restartInfo = getOption( options, LiteralStringRef("restartInfoLocation"), LiteralStringRef("simfdb/restartInfo.ini") ).toString();
		testDuration = getOption( options, LiteralStringRef("testDuration"), 10.0 );
		isRestoring = getOption( options, LiteralStringRef("isRestoring"),  0 );
	}

	virtual std::string description() { return "SaveAndKillWorkload"; }
	virtual Future<Void> setup( Database const& cx ) {
		g_simulator.disableSwapsToAll();
		return Void();
	}
	virtual Future<Void> start( Database const& cx ) {
		return _start(this);
	}

	ACTOR Future<Void> _start( SaveAndKillWorkload* self) {
		state int i;
		wait(delay(deterministicRandom()->random01()*self->testDuration));

		CSimpleIni ini;
		ini.SetUnicode();
		ini.LoadFile(self->restartInfo.c_str());

		ini.SetValue("RESTORE", "isRestoring", format("%d", self->isRestoring).c_str());
		ini.SetValue("META", "processesPerMachine", format("%d", g_simulator.processesPerMachine).c_str());
		ini.SetValue("META", "listenersPerProcess", format("%d", g_simulator.listenersPerProcess).c_str());
		ini.SetValue("META", "desiredCoordinators", format("%d", g_simulator.desiredCoordinators).c_str());
		ini.SetValue("META", "connectionString",  g_simulator.connectionString.c_str());
		ini.SetValue("META", "testerCount", format("%d", g_simulator.testerCount).c_str());

		std::vector<ISimulator::ProcessInfo*> processes = g_simulator.getAllProcesses();
		std::map<NetworkAddress, ISimulator::ProcessInfo*> rebootingProcesses = g_simulator.currentlyRebootingProcesses;
		std::map<std::string, ISimulator::ProcessInfo*> allProcessesMap = std::map<std::string, ISimulator::ProcessInfo*>();
		for(auto it = rebootingProcesses.begin(); it != rebootingProcesses.end(); it++) {
			if (allProcessesMap.find(it->second->dataFolder) == allProcessesMap.end())
				allProcessesMap[it->second->dataFolder] = it->second;
		}
		for(auto it = processes.begin(); it != processes.end(); it++) {
			if (allProcessesMap.find((*it)->dataFolder) == allProcessesMap.end())
				allProcessesMap[(*it)->dataFolder] = *it;
		}
		ini.SetValue("META", "processCount", format("%d", allProcessesMap.size()-1).c_str());
		std::map<std::string, int> machines;

		int j = 0;
		for(auto processIterator = allProcessesMap.begin(); processIterator != allProcessesMap.end(); processIterator++) {
			ISimulator::ProcessInfo* process = processIterator->second;
			std::string machineId = printable(process->locality.machineId());
			const char* machineIdString = machineId.c_str();
			if (strcmp(process->name, "TestSystem") != 0) {
				if (machines.find(machineId) == machines.end()) {
					machines.insert(std::pair<std::string, int>(machineId, 1));
					ini.SetValue("META", format("%d", j).c_str(), machineIdString);
					ini.SetValue(machineIdString, "dcUID", (process->locality.dcId().present()) ? process->locality.dcId().get().printable().c_str() : "");
					ini.SetValue(machineIdString, "zoneId", (process->locality.zoneId().present()) ? process->locality.zoneId().get().printable().c_str() : "");
					ini.SetValue(machineIdString, "mClass", format("%d", process->startingClass.classType()).c_str());
					ini.SetValue(machineIdString, format("ipAddr%d", process->address.port - 1).c_str(),
					             process->address.ip.toString().c_str());
					ini.SetValue(machineIdString, format("%d", process->address.port-1).c_str(), process->dataFolder);
					ini.SetValue(machineIdString, format("c%d", process->address.port-1).c_str(), process->coordinationFolder);
					j++;
				}
				else {
					ini.SetValue(machineIdString, format("ipAddr%d", process->address.port - 1).c_str(),
					             process->address.ip.toString().c_str());
					int oldValue = machines.find(machineId)->second;
					ini.SetValue(machineIdString, format("%d", process->address.port-1).c_str(), process->dataFolder);
					ini.SetValue(machineIdString, format("c%d", process->address.port-1).c_str(), process->coordinationFolder);
					machines.erase(machines.find(machineId));
					machines.insert(std::pair<std::string, int>(machineId, oldValue+1));
				}
			}
		}
		for(auto entry = machines.begin(); entry != machines.end(); entry++) {
			ini.SetValue((*entry).first.c_str(), "processes", format("%d", (*entry).second).c_str());
		}

	ini.SetValue("META", "machineCount", format("%d", machines.size()).c_str());
		ini.SaveFile(self->restartInfo.c_str());

		for(auto process = allProcessesMap.begin(); process != allProcessesMap.end(); process++) {
			g_simulator.killProcess(process->second, ISimulator::Reboot);
		}

		for (i = 0; i<100; i++) {
			wait(delay(0.0));
		}

		g_simulator.stop();

		return Void();
	}

	virtual Future<bool> check( Database const& cx ) {
		return true;
	}
	virtual void getMetrics( std::vector<PerfMetric>& ) {
	}
};

WorkloadFactory<SaveAndKillWorkload> SaveAndKillWorkloadFactory("SaveAndKill");
