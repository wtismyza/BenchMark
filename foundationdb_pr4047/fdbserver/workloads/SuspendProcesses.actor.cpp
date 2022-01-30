#include <boost/algorithm/string/predicate.hpp>

#include "fdbserver/workloads/workloads.actor.h"
#include "fdbserver/ServerDBInfo.h"
#include "fdbclient/Status.h"
#include "fdbclient/StatusClient.h"
#include "fdbclient/ManagementAPI.actor.h"
#include "fdbclient/RunTransaction.actor.h"
#include "flow/actorcompiler.h" // has to be last include

struct SuspendProcessesWorkload : TestWorkload {
	std::vector<std::string> prefixSuspendProcesses;
	double suspendTimeDuration;
	double waitTimeDuration;

	SuspendProcessesWorkload(WorkloadContext const& wcx) : TestWorkload(wcx) {
		prefixSuspendProcesses = getOption(options, LiteralStringRef("prefixesSuspendProcesses"), std::vector<std::string>());
		waitTimeDuration = getOption(options, LiteralStringRef("waitTimeDuration"), 0);
		suspendTimeDuration = getOption(options, LiteralStringRef("suspendTimeDuration"), 0);
	}

	virtual std::string description() { return "SuspendProcesses"; }

	virtual Future<Void> setup(Database const& cx) { return Void(); }

	ACTOR Future<Void> _start(Database cx, SuspendProcessesWorkload* self) {
		wait(delay(self->waitTimeDuration));
		state ReadYourWritesTransaction tr(cx);
		loop {
			try {
				tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
				tr.setOption(FDBTransactionOptions::LOCK_AWARE);
				Standalone<RangeResultRef> kvs = wait(tr.getRange(
				    KeyRangeRef(LiteralStringRef("\xff\xff/worker_interfaces"), LiteralStringRef("\xff\xff\xff")), 1));
				std::vector<Standalone<StringRef>> suspendProcessInterfaces;
				for (auto it : kvs) {
					auto ip_port = it.key.endsWith(LiteralStringRef(":tls"))
					                   ? it.key.removeSuffix(LiteralStringRef(":tls"))
					                   : it.key;
					for (auto& killProcess : self->prefixSuspendProcesses) {
						if (boost::starts_with(ip_port.toString().c_str(), killProcess.c_str())) {
							suspendProcessInterfaces.push_back(it.value);
							TraceEvent("SuspendProcessSelectedProcess").detail("IpPort", printable(ip_port));
						}
					}
				}
				for (auto& interf : suspendProcessInterfaces) {
					BinaryReader::fromStringRef<ClientWorkerInterface>(interf, IncludeVersion())
					    .reboot.send(RebootRequest(false, false, self->suspendTimeDuration));
				}
				return Void();
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}
	}

	virtual Future<Void> start(Database const& cx) {
		if (clientId != 0) return Void();
		return _start(cx, this);
	}

	virtual Future<bool> check(Database const& cx) { return true; }

	virtual void getMetrics(vector<PerfMetric>& m) {}
};

WorkloadFactory<SuspendProcessesWorkload> SuspendProcessesWorkloadFactory("SuspendProcesses");
