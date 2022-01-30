#include "fdbserver/workloads/workloads.actor.h"
#include "fdbserver/ServerDBInfo.h"
#include "fdbclient/ManagementAPI.actor.h"
#include "fdbclient/RunTransaction.actor.h"
#include "flow/actorcompiler.h" // has to be last include


static const Key CLIENT_LATENCY_INFO_PREFIX = LiteralStringRef("client_latency/");
static const Key CLIENT_LATENCY_INFO_CTR_PREFIX = LiteralStringRef("client_latency_counter/");

/*
FF               - 2 bytes \xff\x02
SSSSSSSSSS       - 10 bytes Version Stamp
RRRRRRRRRRRRRRRR - 16 bytes Transaction id
NNNN             - 4 Bytes Chunk number (Big Endian)
TTTT             - 4 Bytes Total number of chunks (Big Endian)
XXXX             - Variable length user provided transaction identifier
*/
StringRef sampleTrInfoKey = LiteralStringRef("\xff\x02/fdbClientInfo/client_latency/SSSSSSSSSS/RRRRRRRRRRRRRRRR/NNNNTTTT/XXXX/");
static const auto chunkNumStartIndex = sampleTrInfoKey.toString().find('N');
static const auto numChunksStartIndex = sampleTrInfoKey.toString().find('T');
static const int chunkFormatSize = 4;
static const auto trIdStartIndex = sampleTrInfoKey.toString().find('R');
static const int trIdFormatSize = 16;


namespace ClientLogEventsParser {

	void parseEventGetVersion(BinaryReader &reader) {
		FdbClientLogEvents::EventGetVersion gv;
		reader >> gv;
		ASSERT(gv.latency < 10000);
	}

	void parseEventGetVersion_V2(BinaryReader &reader) {
		FdbClientLogEvents::EventGetVersion_V2 gv;
		reader >> gv;
		ASSERT(gv.latency < 10000);
		ASSERT(gv.priorityType >= 0 && gv.priorityType < FdbClientLogEvents::PRIORITY_END);
	}

	void parseEventGet(BinaryReader &reader) {
		FdbClientLogEvents::EventGet g;
		reader >> g;
		ASSERT(g.latency < 10000 && g.valueSize < CLIENT_KNOBS->VALUE_SIZE_LIMIT && g.key.size() < CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT);
	}

	void parseEventGetRange(BinaryReader &reader) {
		FdbClientLogEvents::EventGetRange gr;
		reader >> gr;
		ASSERT(gr.latency < 10000 && gr.rangeSize < 1000000000 && gr.startKey.size() < CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT && gr.endKey.size() < CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT);
	}

	void parseEventCommit(BinaryReader &reader) {
		FdbClientLogEvents::EventCommit c;
		reader >> c;
		ASSERT(c.latency < 10000 && c.commitBytes < CLIENT_KNOBS->TRANSACTION_SIZE_LIMIT && c.numMutations < 1000000);
	}

	void parseEventErrorGet(BinaryReader &reader) {
		FdbClientLogEvents::EventGetError ge;
		reader >> ge;
		ASSERT(ge.errCode < 10000 && ge.key.size() < CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT);
	}

	void parseEventErrorGetRange(BinaryReader &reader) {
		FdbClientLogEvents::EventGetRangeError gre;
		reader >> gre;
		ASSERT(gre.errCode < 10000 && gre.startKey.size() < CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT && gre.endKey.size() < CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT);
	}

	void parseEventErrorCommit(BinaryReader &reader) {
			FdbClientLogEvents::EventCommitError ce;
			reader >> ce;
			ASSERT(ce.errCode < 10000);
	}

	struct ParserBase {
		std::function<void (BinaryReader &)> parseGetVersion = parseEventGetVersion;
		std::function<void (BinaryReader &)> parseGet = parseEventGet;
		std::function<void (BinaryReader &)> parseGetRange = parseEventGetRange;
		std::function<void (BinaryReader &)> parseCommit = parseEventCommit;
		std::function<void (BinaryReader &)> parseErrorGet = parseEventErrorGet;
		std::function<void (BinaryReader &)> parseErrorGetRange = parseEventErrorGetRange;
		std::function<void (BinaryReader &)> parseErrorCommit = parseEventErrorCommit;
		virtual ~ParserBase() = 0;
	};
	ParserBase::~ParserBase() {}

	struct Parser_V1 : ParserBase {
		virtual ~Parser_V1() override {}
	};
	struct Parser_V2 : ParserBase {
		Parser_V2() { parseGetVersion = parseEventGetVersion_V2; }
		virtual ~Parser_V2() override {}
	};

	struct ParserFactory {
		static std::unique_ptr<ParserBase> getParser(ProtocolVersion version) {
			if(version.version() >= (uint64_t) 0x0FDB00B062000001LL) {
				return std::unique_ptr<ParserBase>(new Parser_V2());
			} else {
				return std::unique_ptr<ParserBase>(new Parser_V1());
			}
		}
	};
};

// Checks TransactionInfo format
bool checkTxInfoEntryFormat(BinaryReader &reader) {
	// Check protocol version
	ProtocolVersion protocolVersion;
	reader >> protocolVersion;
	reader.setProtocolVersion(protocolVersion);
	std::unique_ptr<ClientLogEventsParser::ParserBase> parser = ClientLogEventsParser::ParserFactory::getParser(protocolVersion);

	while (!reader.empty()) {
		// Get EventType and timestamp
		FdbClientLogEvents::EventType event;
		reader >> event;
		double timeStamp;
		reader >> timeStamp;
		switch (event)
		{
		case FdbClientLogEvents::GET_VERSION_LATENCY:
			parser->parseGetVersion(reader);
			break;
		case FdbClientLogEvents::GET_LATENCY:
			parser->parseGet(reader);
			break;
		case FdbClientLogEvents::GET_RANGE_LATENCY:
			parser->parseGetRange(reader);
			break;
		case FdbClientLogEvents::COMMIT_LATENCY:
			parser->parseCommit(reader);
			break;
		case FdbClientLogEvents::ERROR_GET:
			parser->parseErrorGet(reader);
			break;
		case FdbClientLogEvents::ERROR_GET_RANGE:
			parser->parseErrorGetRange(reader);
			break;
		case FdbClientLogEvents::ERROR_COMMIT:
			parser->parseErrorCommit(reader);
			break;
		default:
			TraceEvent(SevError, "ClientTransactionProfilingUnknownEvent").detail("EventType", event);
			return false;
		}
	}

	return true;
}

struct ClientTransactionProfileCorrectnessWorkload : TestWorkload {
	double samplingProbability;
	int64_t trInfoSizeLimit;

	ClientTransactionProfileCorrectnessWorkload(WorkloadContext const& wcx)
		: TestWorkload(wcx)
	{
		samplingProbability = getOption(options, LiteralStringRef("samplingProbability"), deterministicRandom()->random01() / 10); //rand range 0 - 0.1
		trInfoSizeLimit = getOption(options, LiteralStringRef("trInfoSizeLimit"), deterministicRandom()->randomInt(100 * 1024, 10 * 1024 * 1024)); // 100 KB - 10 MB
		TraceEvent(SevInfo, "ClientTransactionProfilingSetup").detail("ClientId", clientId).detail("SamplingProbability", samplingProbability).detail("TrInfoSizeLimit", trInfoSizeLimit);
	}

	virtual std::string description() { return "ClientTransactionProfileCorrectness"; }

	virtual Future<Void> setup(Database const& cx) {
		if (clientId == 0) {
			const_cast<ClientKnobs *>(CLIENT_KNOBS)->CSI_STATUS_DELAY = 2.0; // 2 seconds
			return changeProfilingParameters(cx, trInfoSizeLimit, samplingProbability);
		}
		return Void();
	}

	virtual Future<Void> start(Database const& cx) {
		return Void();
	}

	int getNumChunks(KeyRef key) {
		return bigEndian32(BinaryReader::fromStringRef<int>(key.substr(numChunksStartIndex, chunkFormatSize), Unversioned()));
	}

	int getChunkNum(KeyRef key) {
		return bigEndian32(BinaryReader::fromStringRef<int>(key.substr(chunkNumStartIndex, chunkFormatSize), Unversioned()));
	}

	std::string getTrId(KeyRef key) {
		return key.substr(trIdStartIndex, trIdFormatSize).toString();
	}

	bool checkTxInfoEntriesFormat(const Standalone<RangeResultRef> &txInfoEntries) {
		std::string val;
		std::map<std::string, std::vector<ValueRef>> trInfoChunks;
		for (auto kv : txInfoEntries) {
			int numChunks = getNumChunks(kv.key);
			int chunkNum = getChunkNum(kv.key);
			std::string trId = getTrId(kv.key);

			if (numChunks == 1) {
				ASSERT(chunkNum == 1);
				BinaryReader reader(kv.value, Unversioned());
					if (!checkTxInfoEntryFormat(reader))
						return false;
			}
			else {
				if (chunkNum == 1) { // First chunk
					// Remove any entry if already present. There are scenarios (eg., commit_unknown_result) where a transaction info
					// may be logged multiple times
					trInfoChunks.erase(trId);
					trInfoChunks.insert(std::pair < std::string, std::vector<ValueRef> >(trId, {kv.value}));
				}
				else {
					if (trInfoChunks.find(trId) == trInfoChunks.end()) {
						// Some of the earlier chunks for this trId should have been deleted.
						// Discard this chunk as it is of not much use
						TraceEvent(SevInfo, "ClientTransactionProfilingSomeChunksMissing").detail("TrId", trId);
					}
					else {
						// Check if it is the expected chunk. Otherwise discard the whole transaction entry.
						// There are scenarios (eg., when deletion is happening) where some chunks get missed.
						if (chunkNum != trInfoChunks.find(trId)->second.size() + 1) {
							TraceEvent(SevInfo, "ClientTransactionProfilingChunksMissing").detail("TrId", trId);
							trInfoChunks.erase(trId);
						}
						else {
							trInfoChunks.find(trId)->second.push_back(kv.value);
						}
						
					}
				}
				if (chunkNum == numChunks && trInfoChunks.find(trId) != trInfoChunks.end()) {
					auto iter = trInfoChunks.find(trId);
					BinaryWriter bw(Unversioned());
					for (auto val : iter->second)
						bw.serializeBytes(val.begin(), val.size());
					BinaryReader reader(bw.getData(), bw.getLength(), Unversioned());
					if (!checkTxInfoEntryFormat(reader))
						return false;
					trInfoChunks.erase(iter);
				}
			}
		}
		return true;
	}

	ACTOR Future<Void> changeProfilingParameters(Database cx, int64_t sizeLimit, double sampleProbability) {

		wait(runRYWTransaction(cx, [=](Reference<ReadYourWritesTransaction> tr) -> Future<Void>
						{
							tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
							tr->setOption(FDBTransactionOptions::LOCK_AWARE);
							tr->set(fdbClientInfoTxnSampleRate, BinaryWriter::toValue(sampleProbability, Unversioned()));
							tr->set(fdbClientInfoTxnSizeLimit, BinaryWriter::toValue(sizeLimit, Unversioned()));
							return Void();
						}
					 ));
		return Void();
	}

	ACTOR Future<bool> _check(Database cx, ClientTransactionProfileCorrectnessWorkload* self) {
		wait(self->changeProfilingParameters(cx, self->trInfoSizeLimit, 0));  // Disable sampling
		// FIXME: Better way to ensure that all client profile data has been flushed to the database
		wait(delay(CLIENT_KNOBS->CSI_STATUS_DELAY));

		state Key clientLatencyAtomicCtr = CLIENT_LATENCY_INFO_CTR_PREFIX.withPrefix(fdbClientInfoPrefixRange.begin);
		state int64_t counter;
		state Standalone<RangeResultRef> txInfoEntries;
		Optional<Value> ctrValue = wait(runRYWTransaction(cx, [=](Reference<ReadYourWritesTransaction> tr) -> Future<Optional<Value>> 
																											{ 
																												tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
																												tr->setOption(FDBTransactionOptions::LOCK_AWARE);
																												return tr->get(clientLatencyAtomicCtr); 
																											}
																											));
		counter = ctrValue.present() ? BinaryReader::fromStringRef<int64_t>(ctrValue.get(), Unversioned()) : 0;
		state Key clientLatencyName = CLIENT_LATENCY_INFO_PREFIX.withPrefix(fdbClientInfoPrefixRange.begin);

		state KeySelector begin = firstGreaterOrEqual(CLIENT_LATENCY_INFO_PREFIX.withPrefix(fdbClientInfoPrefixRange.begin));
		state KeySelector end = firstGreaterOrEqual(strinc(begin.getKey()));
		state int keysLimit = 10;
		state Transaction tr(cx);
		loop {
			try {
				tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
				tr.setOption(FDBTransactionOptions::LOCK_AWARE);
				state Standalone<RangeResultRef> kvRange = wait(tr.getRange(begin, end, keysLimit));
				if (kvRange.empty())
					break;
				txInfoEntries.arena().dependsOn(kvRange.arena());
				txInfoEntries.append(txInfoEntries.arena(), kvRange.begin(), kvRange.size());
				begin = firstGreaterThan(kvRange.back().key);
				tr.reset();
			}
			catch (Error& e) {
				if (e.code() == error_code_transaction_too_old)
					keysLimit = std::max(1, keysLimit / 2);
				wait(tr.onError(e));
			}
		}

		// Check if the counter value matches the size of contents
		int64_t contentsSize = 0;
		for (auto &kv : txInfoEntries) {
			contentsSize += kv.key.size() + kv.value.size();
		}
		// FIXME: Find a way to check that contentsSize is not greater than a certain limit.
		//if (counter != contentsSize) {
		//	TraceEvent(SevError, "ClientTransactionProfilingIncorrectCtrVal").detail("Counter", counter).detail("ContentsSize", contentsSize);
		//	return false;
		//}
		TraceEvent(SevInfo, "ClientTransactionProfilingCtrval").detail("Counter", counter);
		TraceEvent(SevInfo, "ClientTransactionProfilingContentsSize").detail("ContentsSize", contentsSize);

		// Check if the data format is as expected
		return self->checkTxInfoEntriesFormat(txInfoEntries);
	}

	virtual Future<bool> check(Database const& cx) {
		if (clientId != 0)
			return true;
		return _check(cx, this);
	}

	virtual void getMetrics(vector<PerfMetric>& m) {
	}

};

WorkloadFactory<ClientTransactionProfileCorrectnessWorkload> ClientTransactionProfileCorrectnessWorkloadFactory("ClientTransactionProfileCorrectness");
