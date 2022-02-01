/*
 * sim2.actor.cpp
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

#include <cinttypes>

#include "fdbrpc/simulator.h"
#include "flow/IThreadPool.h"
#include "flow/Util.h"
#include "fdbrpc/IAsyncFile.h"
#include "fdbrpc/AsyncFileCached.actor.h"
#include "fdbrpc/AsyncFileNonDurable.actor.h"
#include "flow/Hash3.h"
#include "fdbrpc/TraceFileIO.h"
#include "flow/FaultInjection.h"
#include "flow/network.h"
#include "flow/TLSConfig.actor.h"
#include "fdbrpc/Net2FileSystem.h"
#include "fdbrpc/Replication.h"
#include "fdbrpc/ReplicationUtils.h"
#include "fdbrpc/AsyncFileWriteChecker.h"
#include "flow/actorcompiler.h"  // This must be the last #include.

bool simulator_should_inject_fault( const char* context, const char* file, int line, int error_code ) {
	if (!g_network->isSimulated()) return false;

	auto p = g_simulator.getCurrentProcess();

	if (p->fault_injection_p2 && deterministicRandom()->random01() < p->fault_injection_p2 && !g_simulator.speedUpSimulation) {
		uint32_t h1 = line + (p->fault_injection_r >> 32);

		if (h1 < p->fault_injection_p1*std::numeric_limits<uint32_t>::max()) {
			TEST(true);                                     // A fault was injected
			TEST(error_code == error_code_io_timeout);      // An io timeout was injected
			TEST(error_code == error_code_io_error);        // An io error was injected
			TEST(error_code == error_code_platform_error);  // A platform error was injected.
			TraceEvent(SevWarn, "FaultInjected").detail("Context", context).detail("File", file).detail("Line", line).detail("ErrorCode", error_code);
			if(error_code == error_code_io_timeout) {
				g_network->setGlobal(INetwork::enASIOTimedOut, (flowGlobalType)true);
			}
			return true;
		}
	}

	return false;
}

void ISimulator::displayWorkers() const
{
	std::map<std::string, std::vector<ISimulator::ProcessInfo*>> machineMap;

	// Create a map of machine Id
	for (auto processInfo : getAllProcesses()) {
		std::string dataHall = processInfo->locality.dataHallId().present() ? processInfo->locality.dataHallId().get().printable() : "[unset]";
		std::string machineId = processInfo->locality.machineId().present() ? processInfo->locality.machineId().get().printable() : "[unset]";
		machineMap[format("%-8s  %s", dataHall.c_str(), machineId.c_str())].push_back(processInfo);
	}

	printf("DataHall  MachineId\n");
	printf("                  Address   Name      Class        Excluded Failed Rebooting Cleared Role                                              DataFolder\n");
	for (auto& machineRecord : machineMap) {
		printf("\n%s\n", machineRecord.first.c_str());
		for (auto& processInfo : machineRecord.second) {
			printf("                  %9s %-10s%-13s%-8s %-6s %-9s %-8s %-48s %-40s\n",
			processInfo->address.toString().c_str(), processInfo->name, processInfo->startingClass.toString().c_str(), (processInfo->isExcluded() ? "True" : "False"), (processInfo->failed ? "True" : "False"), (processInfo->rebooting ? "True" : "False"), (processInfo->isCleared() ? "True" : "False"), getRoles(processInfo->address).c_str(), processInfo->dataFolder);
		}
	}

	return;
}

namespace std {
template<>
class hash<Endpoint> {
public:
	size_t operator()(const Endpoint &s) const
	{
		return hashlittle(&s, sizeof(s), 0);
	}
};
}

bool onlyBeforeSimulatorInit() {
	return g_network->isSimulated() && g_simulator.getAllProcesses().empty();
}

const UID TOKEN_ENDPOINT_NOT_FOUND(-1, -1);

int openCount = 0;

struct SimClogging {
	double getSendDelay( NetworkAddress from, NetworkAddress to ) {
		return halfLatency();
		double tnow = now();
		double t = tnow + halfLatency();

		if (!g_simulator.speedUpSimulation && clogSendUntil.count( to.ip ))
			t = std::max( t, clogSendUntil[ to.ip ] );

		return t - tnow;
	}

	double getRecvDelay( NetworkAddress from, NetworkAddress to ) {
		auto pair = std::make_pair( from.ip, to.ip );

		double tnow = now();
		double t = tnow + halfLatency();
		if(!g_simulator.speedUpSimulation)
			t += clogPairLatency[ pair ];

		if (!g_simulator.speedUpSimulation && clogPairUntil.count( pair ))
			t = std::max( t, clogPairUntil[ pair ] );

		if (!g_simulator.speedUpSimulation && clogRecvUntil.count( to.ip ))
			t = std::max( t, clogRecvUntil[ to.ip ] );

		return t - tnow;
	}

	void clogPairFor(const IPAddress& from, const IPAddress& to, double t) {
		auto& u = clogPairUntil[ std::make_pair( from, to ) ];
		u = std::max(u, now() + t);
	}
	void clogSendFor(const IPAddress& from, double t) {
		auto& u = clogSendUntil[from];
		u = std::max(u, now() + t);
	}
	void clogRecvFor(const IPAddress& from, double t) {
		auto& u = clogRecvUntil[from];
		u = std::max(u, now() + t);
	}
	double setPairLatencyIfNotSet(const IPAddress& from, const IPAddress& to, double t) {
		auto i = clogPairLatency.find( std::make_pair(from,to) );
		if (i == clogPairLatency.end())
			i = clogPairLatency.insert( std::make_pair( std::make_pair(from,to), t ) ).first;
		return i->second;
	}

private:
	std::map<IPAddress, double> clogSendUntil, clogRecvUntil;
	std::map<std::pair<IPAddress, IPAddress>, double> clogPairUntil;
	std::map<std::pair<IPAddress, IPAddress>, double> clogPairLatency;
	double halfLatency() {
		double a = deterministicRandom()->random01();
		const double pFast = 0.999;
		if (a <= pFast) {
			a = a / pFast;
			return 0.5 * (FLOW_KNOBS->MIN_NETWORK_LATENCY * (1-a) + FLOW_KNOBS->FAST_NETWORK_LATENCY/pFast * a); // 0.5ms average
		} else {
			a = (a-pFast) / (1-pFast); // uniform 0-1 again
			return 0.5 * (FLOW_KNOBS->MIN_NETWORK_LATENCY * (1-a) + FLOW_KNOBS->SLOW_NETWORK_LATENCY*a); // long tail up to X ms
		}
	}
};

SimClogging g_clogging;

struct Sim2Conn : IConnection, ReferenceCounted<Sim2Conn> {
	Sim2Conn( ISimulator::ProcessInfo* process )
		: process(process), dbgid( deterministicRandom()->randomUniqueID() ), opened(false), closedByCaller(false), stopReceive(Never())
	{
		pipes = sender(this) && receiver(this);
	}

	// connect() is called on a pair of connections immediately after creation; logically it is part of the constructor and no other method may be called previously!
	void connect( Reference<Sim2Conn> peer, NetworkAddress peerEndpoint ) {
		this->peer = peer;
		this->peerProcess = peer->process;
		this->peerId = peer->dbgid;
		this->peerEndpoint = peerEndpoint;

		// Every one-way connection gets a random permanent latency and a random send buffer for the duration of the connection
		auto latency = g_clogging.setPairLatencyIfNotSet( peerProcess->address.ip, process->address.ip, FLOW_KNOBS->MAX_CLOGGING_LATENCY*deterministicRandom()->random01() );
		sendBufSize = std::max<double>( deterministicRandom()->randomInt(0, 5000000), 25e6 * (latency + .002) );
		TraceEvent("Sim2Connection").detail("SendBufSize", sendBufSize).detail("Latency", latency);
	}

	~Sim2Conn() {
		ASSERT_ABORT( !opened || closedByCaller );
	}

	virtual void addref() { ReferenceCounted<Sim2Conn>::addref(); }
	virtual void delref() { ReferenceCounted<Sim2Conn>::delref(); }
	virtual void close() { closedByCaller = true; closeInternal(); }

	virtual Future<Void> acceptHandshake() { return delay(0.01*deterministicRandom()->random01()); }
	virtual Future<Void> connectHandshake() { return delay(0.01*deterministicRandom()->random01()); }

	virtual Future<Void> onWritable() { return whenWritable(this); }
	virtual Future<Void> onReadable() { return whenReadable(this); }

	bool isPeerGone() {
		return !peer || peerProcess->failed;
	}

	void peerClosed() {
		leakedConnectionTracker = trackLeakedConnection(this);
		stopReceive = delay(1.0);
	}

	// Reads as many bytes as possible from the read buffer into [begin,end) and returns the number of bytes read (might be 0)
	// (or may throw an error if the connection dies)
	virtual int read( uint8_t* begin, uint8_t* end ) {
		rollRandomClose();

		int64_t avail = receivedBytes.get() - readBytes.get();  // SOMEDAY: random?
		int toRead = std::min<int64_t>( end-begin, avail );
		ASSERT( toRead >= 0 && toRead <= recvBuf.size() && toRead <= end-begin );
		for(int i=0; i<toRead; i++)
			begin[i] = recvBuf[i];
		recvBuf.erase( recvBuf.begin(), recvBuf.begin() + toRead );
		readBytes.set( readBytes.get() + toRead );
		return toRead;
	}

	// Writes as many bytes as possible from the given SendBuffer chain into the write buffer and returns the number of bytes written (might be 0)
	// (or may throw an error if the connection dies)
	virtual int write( SendBuffer const* buffer, int limit) {
		rollRandomClose();
		ASSERT(limit > 0);

		int toSend = 0;
		if (BUGGIFY) {
			toSend = std::min(limit, buffer->bytes_written - buffer->bytes_sent);
		} else {
			for(auto p = buffer; p; p=p->next) {
				toSend += p->bytes_written - p->bytes_sent;
				if(toSend >= limit) {
					if(toSend > limit)
						toSend = limit;
					break;
				}
			}
		}
		ASSERT(toSend);
		if (BUGGIFY) toSend = std::min(toSend, deterministicRandom()->randomInt(0, 1000));

		if (!peer) return toSend;
		toSend = std::min( toSend, peer->availableSendBufferForPeer() );
		ASSERT( toSend >= 0 );

		int leftToSend = toSend;
		for(auto p = buffer; p && leftToSend>0; p=p->next) {
			int ts = std::min(leftToSend, p->bytes_written - p->bytes_sent);
			peer->recvBuf.insert( peer->recvBuf.end(), p->data + p->bytes_sent, p->data + p->bytes_sent + ts );
			leftToSend -= ts;
		}
		ASSERT( leftToSend == 0 );
		peer->writtenBytes.set( peer->writtenBytes.get() + toSend );
		return toSend;
	}

	// Returns the network address and port of the other end of the connection.  In the case of an incoming connection, this may not
	// be an address we can connect to!
	virtual NetworkAddress getPeerAddress() { return peerEndpoint; }
	virtual UID getDebugID() { return dbgid; }

	bool opened, closedByCaller;

private:
	ISimulator::ProcessInfo* process, *peerProcess;
	UID dbgid, peerId;
	NetworkAddress peerEndpoint;
	std::deque< uint8_t > recvBuf;  // Includes bytes written but not yet received!
	AsyncVar<int64_t> readBytes, // bytes already pulled from recvBuf (location of the beginning of recvBuf)
		              receivedBytes,
					  sentBytes,
					  writtenBytes; // location of the end of recvBuf ( == recvBuf.size() + readBytes.get() )
	Reference<Sim2Conn> peer;
	int sendBufSize;

	Future<Void> leakedConnectionTracker;

	Future<Void> pipes;
	Future<Void> stopReceive;

	int availableSendBufferForPeer() const { return sendBufSize - (writtenBytes.get() - receivedBytes.get()); }  // SOMEDAY: acknowledgedBytes instead of receivedBytes

	void closeInternal() {
		if(peer) {
			peer->peerClosed();
			stopReceive = delay(1.0);
		}
		leakedConnectionTracker.cancel();
		peer.clear();
	}

	ACTOR static Future<Void> sender( Sim2Conn* self ) {
		loop {
			wait( self->writtenBytes.onChange() );  // takes place on peer!
			ASSERT( g_simulator.getCurrentProcess() == self->peerProcess );
			wait( delay( .002 * deterministicRandom()->random01() ) );
			self->sentBytes.set( self->writtenBytes.get() );  // or possibly just some sometimes...
		}
	}
	ACTOR static Future<Void> receiver( Sim2Conn* self ) {
		loop {
			if (self->sentBytes.get() != self->receivedBytes.get())
				wait( g_simulator.onProcess( self->peerProcess ) );
			while ( self->sentBytes.get() == self->receivedBytes.get() )
				wait( self->sentBytes.onChange() );
			ASSERT( g_simulator.getCurrentProcess() == self->peerProcess );
			state int64_t pos = deterministicRandom()->random01() < .5 ? self->sentBytes.get() : deterministicRandom()->randomInt64( self->receivedBytes.get(), self->sentBytes.get()+1 );
			wait( delay( g_clogging.getSendDelay( self->process->address, self->peerProcess->address ) ) );
			wait( g_simulator.onProcess( self->process ) );
			ASSERT( g_simulator.getCurrentProcess() == self->process );
			wait( delay( g_clogging.getRecvDelay( self->process->address, self->peerProcess->address ) ) );
			ASSERT( g_simulator.getCurrentProcess() == self->process );
			if(self->stopReceive.isReady()) {
				wait(Future<Void>(Never()));
			}
			self->receivedBytes.set( pos );
			wait( Future<Void>(Void()) );  // Prior notification can delete self and cancel this actor
			ASSERT( g_simulator.getCurrentProcess() == self->process );
		}
	}
	ACTOR static Future<Void> whenReadable( Sim2Conn* self ) {
		try {
			loop {
				if (self->readBytes.get() != self->receivedBytes.get()) {
					ASSERT( g_simulator.getCurrentProcess() == self->process );
					return Void();
				}
				wait( self->receivedBytes.onChange() );
				self->rollRandomClose();
			}
		} catch (Error& e) {
			ASSERT( g_simulator.getCurrentProcess() == self->process );
			throw;
		}
	}
	ACTOR static Future<Void> whenWritable( Sim2Conn* self ) {
		try {
			loop {
				if (!self->peer) return Void();
				if (self->peer->availableSendBufferForPeer() > 0) {
					ASSERT( g_simulator.getCurrentProcess() == self->process );
					return Void();
				}
				try {
					wait( self->peer->receivedBytes.onChange() );
					ASSERT( g_simulator.getCurrentProcess() == self->peerProcess );
				} catch (Error& e) {
					if (e.code() != error_code_broken_promise) throw;
				}
				wait( g_simulator.onProcess( self->process ) );
			}
		} catch (Error& e) {
			ASSERT( g_simulator.getCurrentProcess() == self->process );
			throw;
		}
	}

	void rollRandomClose() {
		if (now() - g_simulator.lastConnectionFailure > g_simulator.connectionFailuresDisableDuration && deterministicRandom()->random01() < .00001) {
			g_simulator.lastConnectionFailure = now();
			double a = deterministicRandom()->random01(), b = deterministicRandom()->random01();
			TEST(true);  // Simulated connection failure
			TraceEvent("ConnectionFailure", dbgid).detail("MyAddr", process->address).detail("PeerAddr", peerProcess->address).detail("SendClosed", a > .33).detail("RecvClosed", a < .66).detail("Explicit", b < .3);
			if (a < .66 && peer) peer->closeInternal();
			if (a > .33) closeInternal();
			// At the moment, we occasionally notice the connection failed immediately.  In principle, this could happen but only after a delay.
			if (b < .3)
				throw connection_failed();
		}
	}

	ACTOR static Future<Void> trackLeakedConnection( Sim2Conn* self ) {
		wait( g_simulator.onProcess( self->process ) );
		if (self->process->address.isPublic()) {
			wait( delay( FLOW_KNOBS->CONNECTION_MONITOR_IDLE_TIMEOUT * FLOW_KNOBS->CONNECTION_MONITOR_IDLE_TIMEOUT * 1.5 ) );
		} else {
			wait( delay( FLOW_KNOBS->CONNECTION_MONITOR_IDLE_TIMEOUT * 1.5 ) );
		}
		TraceEvent(SevError, "LeakedConnection", self->dbgid)
		    .error(connection_leaked())
		    .detail("MyAddr", self->process->address)
		    .detail("PeerAddr", self->peerEndpoint)
		    .detail("PeerId", self->peerId)
		    .detail("Opened", self->opened);
		return Void();
	}
};

#include <fcntl.h>
#include <sys/stat.h>

int sf_open( const char* filename, int flags, int convFlags, int mode );

#if defined(_WIN32)
#include <io.h>
#define O_CLOEXEC 0

#elif defined(__unixish__)
#define _open ::open
#define _read ::read
#define _write ::write
#define _close ::close
#define _lseeki64 ::lseek
#define _commit ::fsync
#define _chsize ::ftruncate
#define O_BINARY 0

int sf_open( const char* filename, int flags, int convFlags, int mode ) {
	return _open( filename, convFlags, mode );
}

#else
#error How do i open a file on a new platform?
#endif

class SimpleFile : public IAsyncFile, public ReferenceCounted<SimpleFile> {
public:
	static void init() {}

	static bool should_poll() { return false; }

	ACTOR static Future<Reference<IAsyncFile>> open( std::string filename, int flags, int mode,
													Reference<DiskParameters> diskParameters = Reference<DiskParameters>(new DiskParameters(25000, 150000000)), bool delayOnWrite = true ) {
		state ISimulator::ProcessInfo* currentProcess = g_simulator.getCurrentProcess();
		state TaskPriority currentTaskID = g_network->getCurrentTask();

		if(++openCount >= 3000) {
			TraceEvent(SevError, "TooManyFiles");
			ASSERT(false);
		}

		if(openCount == 2000) {
			TraceEvent(SevWarnAlways, "DisableConnectionFailures_TooManyFiles");
			g_simulator.speedUpSimulation = true;
			g_simulator.connectionFailuresDisableDuration = 1e6;
		}

		// Filesystems on average these days seem to start to have limits of around 255 characters for a
		// filename.  We add ".part" below, so we need to stay under 250.
		ASSERT( basename(filename).size() < 250 );

		wait( g_simulator.onMachine( currentProcess ) );
		try {
			wait( delay(FLOW_KNOBS->MIN_OPEN_TIME + deterministicRandom()->random01() * (FLOW_KNOBS->MAX_OPEN_TIME - FLOW_KNOBS->MIN_OPEN_TIME) ) );

			std::string open_filename = filename;
			if (flags & OPEN_ATOMIC_WRITE_AND_CREATE) {
				ASSERT( (flags & OPEN_CREATE) && (flags & OPEN_READWRITE) && !(flags & OPEN_EXCLUSIVE) );
				open_filename = filename + ".part";
			}

			int h = sf_open( open_filename.c_str(), flags, flagConversion(flags), mode );
			if( h == -1 ) {
				bool notFound = errno == ENOENT;
				Error e = notFound ? file_not_found() : io_error();
				TraceEvent(notFound ? SevWarn : SevWarnAlways, "FileOpenError").error(e).GetLastError().detail("File", filename).detail("Flags", flags);
				throw e;
			}

			platform::makeTemporary(open_filename.c_str());
			SimpleFile *simpleFile = new SimpleFile( h, diskParameters, delayOnWrite, filename, open_filename, flags );
			state Reference<IAsyncFile> file = Reference<IAsyncFile>( simpleFile );
			wait( g_simulator.onProcess( currentProcess, currentTaskID ) );
			return file;
		} catch( Error &e ) {
			state Error err = e;
			wait( g_simulator.onProcess( currentProcess, currentTaskID ) );
			throw err;
		}
	}

	virtual void addref() { ReferenceCounted<SimpleFile>::addref(); }
	virtual void delref() { ReferenceCounted<SimpleFile>::delref(); }

	virtual int64_t debugFD() { return (int64_t)h; }

	virtual Future<int> read( void* data, int length, int64_t offset ) {
		return read_impl( this, data, length, offset );
	}

	virtual Future<Void> write( void const* data, int length, int64_t offset ) {
		return write_impl( this, StringRef((const uint8_t*)data, length), offset );
	}

	virtual Future<Void> truncate( int64_t size ) {
		return truncate_impl( this, size );
	}

	virtual Future<Void> sync() {
		return sync_impl( this );
	}

	virtual Future<int64_t> size() {
		return size_impl( this );
	}

	virtual std::string getFilename() {
		return actualFilename;
	}

	~SimpleFile() {
		_close( h );
	}

private:
	int h;

	//Performance parameters of simulated disk
	Reference<DiskParameters> diskParameters;

	std::string filename, actualFilename;
	int flags;
	UID dbgId;

	//If true, then writes/truncates will be preceded by a delay (like other operations).  If false, then they will not
	//This is to support AsyncFileNonDurable, which issues its own delays for writes and truncates
	bool delayOnWrite;

	SimpleFile(int h, Reference<DiskParameters> diskParameters, bool delayOnWrite, const std::string& filename, const std::string& actualFilename, int flags)
		: h(h), diskParameters(diskParameters), delayOnWrite(delayOnWrite), filename(filename), actualFilename(actualFilename), dbgId(deterministicRandom()->randomUniqueID()), flags(flags) {}

	static int flagConversion( int flags ) {
		int outFlags = O_BINARY | O_CLOEXEC;
		if( flags&OPEN_READWRITE ) outFlags |= O_RDWR;
		if( flags&OPEN_CREATE ) outFlags |= O_CREAT;
		if( flags&OPEN_READONLY ) outFlags |= O_RDONLY;
		if( flags&OPEN_EXCLUSIVE ) outFlags |= O_EXCL;
		if( flags&OPEN_ATOMIC_WRITE_AND_CREATE ) outFlags |= O_TRUNC;

		return outFlags;
	}

	ACTOR static Future<int> read_impl( SimpleFile* self, void* data, int length, int64_t offset ) {
		ASSERT( ( self->flags & IAsyncFile::OPEN_NO_AIO ) != 0 ||
		        ( (uintptr_t)data % 4096 == 0 && length % 4096 == 0 && offset % 4096 == 0 ) );  // Required by KAIO.
		state UID opId = deterministicRandom()->randomUniqueID();
		if (randLog)
			fprintf( randLog, "SFR1 %s %s %s %d %" PRId64 "\n", self->dbgId.shortString().c_str(), self->filename.c_str(), opId.shortString().c_str(), length, offset );

		wait( waitUntilDiskReady( self->diskParameters, length ) );

		if( _lseeki64( self->h, offset, SEEK_SET ) == -1 ) {
			TraceEvent(SevWarn, "SimpleFileIOError").detail("Location", 1);
			throw io_error();
		}

		unsigned int read_bytes = 0;
		if( ( read_bytes = _read( self->h, data, (unsigned int) length ) ) == -1 ) {
			TraceEvent(SevWarn, "SimpleFileIOError").detail("Location", 2);
			throw io_error();
		}

		if (randLog) {
			uint32_t a=0, b=0;
			hashlittle2( data, read_bytes, &a, &b );
			fprintf( randLog, "SFR2 %s %s %s %d %d\n", self->dbgId.shortString().c_str(), self->filename.c_str(), opId.shortString().c_str(), read_bytes, a );
		}

		debugFileCheck("SimpleFileRead", self->filename, data, offset, length);

		INJECT_FAULT(io_timeout, "SimpleFile::read");
		INJECT_FAULT(io_error, "SimpleFile::read");

		return read_bytes;
	}

	ACTOR static Future<Void> write_impl( SimpleFile* self, StringRef data, int64_t offset ) {
		state UID opId = deterministicRandom()->randomUniqueID();
		if (randLog) {
			uint32_t a=0, b=0;
			hashlittle2( data.begin(), data.size(), &a, &b );
			fprintf( randLog, "SFW1 %s %s %s %d %d %" PRId64 "\n", self->dbgId.shortString().c_str(), self->filename.c_str(), opId.shortString().c_str(), a, data.size(), offset );
		}

		if(self->delayOnWrite)
			wait( waitUntilDiskReady( self->diskParameters, data.size() ) );

		if( _lseeki64( self->h, offset, SEEK_SET ) == -1 ) {
			TraceEvent(SevWarn, "SimpleFileIOError").detail("Location", 3);
			throw io_error();
		}

		unsigned int write_bytes = 0;
		if ( ( write_bytes = _write( self->h, (void*)data.begin(), data.size() ) ) == -1 ) {
			TraceEvent(SevWarn, "SimpleFileIOError").detail("Location", 4);
			throw io_error();
		}

		if ( write_bytes != data.size() ) {
			TraceEvent(SevWarn, "SimpleFileIOError").detail("Location", 5);
			throw io_error();
		}

		if (randLog) {
			fprintf( randLog, "SFW2 %s %s %s\n", self->dbgId.shortString().c_str(), self->filename.c_str(), opId.shortString().c_str());
		}

		debugFileCheck("SimpleFileWrite", self->filename, (void*)data.begin(), offset, data.size());

		INJECT_FAULT(io_timeout, "SimpleFile::write");
		INJECT_FAULT(io_error, "SimpleFile::write");

		return Void();
	}

	ACTOR static Future<Void> truncate_impl( SimpleFile* self, int64_t size ) {
		state UID opId = deterministicRandom()->randomUniqueID();
		if (randLog)
			fprintf( randLog, "SFT1 %s %s %s %" PRId64 "\n", self->dbgId.shortString().c_str(), self->filename.c_str(), opId.shortString().c_str(), size );

		if (size == 0) {
			// KAIO will return EINVAL, as len==0 is an error.
			throw io_error();
		}

		if(self->delayOnWrite)
			wait( waitUntilDiskReady( self->diskParameters, 0 ) );

		if( _chsize( self->h, (long) size ) == -1 ) {
			TraceEvent(SevWarn, "SimpleFileIOError").detail("Location", 6).detail("Filename", self->filename).detail("Size", size).detail("Fd", self->h).GetLastError();
			throw io_error();
		}

		if (randLog)
			fprintf( randLog, "SFT2 %s %s %s\n", self->dbgId.shortString().c_str(), self->filename.c_str(), opId.shortString().c_str());

		INJECT_FAULT( io_timeout, "SimpleFile::truncate" );
		INJECT_FAULT( io_error, "SimpleFile::truncate" );

		return Void();
	}

	ACTOR static Future<Void> sync_impl( SimpleFile* self ) {
		state UID opId = deterministicRandom()->randomUniqueID();
		if (randLog)
			fprintf( randLog, "SFC1 %s %s %s\n", self->dbgId.shortString().c_str(), self->filename.c_str(), opId.shortString().c_str());

		if(self->delayOnWrite)
			wait( waitUntilDiskReady( self->diskParameters, 0, true ) );

		if (self->flags & OPEN_ATOMIC_WRITE_AND_CREATE) {
			self->flags &= ~OPEN_ATOMIC_WRITE_AND_CREATE;
			auto& machineCache = g_simulator.getCurrentProcess()->machine->openFiles;
			std::string sourceFilename = self->filename + ".part";

			if(machineCache.count(sourceFilename)) {
				TraceEvent("SimpleFileRename").detail("From", sourceFilename).detail("To", self->filename).detail("SourceCount", machineCache.count(sourceFilename)).detail("FileCount", machineCache.count(self->filename));
				renameFile( sourceFilename.c_str(), self->filename.c_str() );

				ASSERT(!machineCache.count(self->filename));
				machineCache[self->filename] = machineCache[sourceFilename];
				machineCache.erase(sourceFilename);
				self->actualFilename = self->filename;
			}
		}

		if (randLog)
			fprintf( randLog, "SFC2 %s %s %s\n", self->dbgId.shortString().c_str(), self->filename.c_str(), opId.shortString().c_str());

		INJECT_FAULT( io_timeout, "SimpleFile::sync" );
		INJECT_FAULT( io_error, "SimpleFile::sync" );

		return Void();
	}

	ACTOR static Future<int64_t> size_impl( SimpleFile* self ) {
		state UID opId = deterministicRandom()->randomUniqueID();
		if (randLog)
			fprintf(randLog, "SFS1 %s %s %s\n", self->dbgId.shortString().c_str(), self->filename.c_str(), opId.shortString().c_str());

		wait( waitUntilDiskReady( self->diskParameters, 0 ) );

		int64_t pos = _lseeki64( self->h, 0L, SEEK_END );
		if( pos == -1 ) {
			TraceEvent(SevWarn, "SimpleFileIOError").detail("Location", 8);
			throw io_error();
		}

		if (randLog)
			fprintf(randLog, "SFS2 %s %s %s %" PRId64 "\n", self->dbgId.shortString().c_str(), self->filename.c_str(), opId.shortString().c_str(), pos);
		INJECT_FAULT( io_error, "SimpleFile::size" );

		return pos;
	}
};

struct SimDiskSpace {
	int64_t totalSpace;
	int64_t baseFreeSpace; //The original free space of the disk + deltas from simulated external modifications
	double lastUpdate;
};

void doReboot( ISimulator::ProcessInfo* const& p, ISimulator::KillType const& kt );

struct Sim2Listener : IListener, ReferenceCounted<Sim2Listener> {
	explicit Sim2Listener( ISimulator::ProcessInfo* process, const NetworkAddress& listenAddr )
		: process(process),
	      address(listenAddr) {}

	void incomingConnection( double seconds, Reference<IConnection> conn ) {  // Called by another process!
		incoming( Reference<Sim2Listener>::addRef( this ), seconds, conn );
	}

	virtual void addref() { ReferenceCounted<Sim2Listener>::addref(); }
	virtual void delref() { ReferenceCounted<Sim2Listener>::delref(); }

	virtual Future<Reference<IConnection>> accept() {
		return popOne( nextConnection.getFuture() );
	}

	virtual NetworkAddress getListenAddress() { return address; }

private:
	ISimulator::ProcessInfo* process;
	PromiseStream< Reference<IConnection> > nextConnection;

	ACTOR static void incoming( Reference<Sim2Listener> self, double seconds, Reference<IConnection> conn ) {
		wait( g_simulator.onProcess(self->process) );
		wait( delay( seconds ) );
		if (((Sim2Conn*)conn.getPtr())->isPeerGone() && deterministicRandom()->random01()<0.5)
			return;
		TraceEvent("Sim2IncomingConn", conn->getDebugID())
			.detail("ListenAddress", self->getListenAddress())
			.detail("PeerAddress", conn->getPeerAddress());
		self->nextConnection.send( conn );
	}
	ACTOR static Future<Reference<IConnection>> popOne( FutureStream< Reference<IConnection> > conns ) {
		Reference<IConnection> c = waitNext( conns );
		((Sim2Conn*)c.getPtr())->opened = true;
		return c;
	}

	NetworkAddress address;
};

#define g_sim2 ((Sim2&)g_simulator)

class Sim2 : public ISimulator, public INetworkConnections {
public:
	// Implement INetwork interface
	// Everything actually network related is delegated to the Sim2Net class; Sim2 is only concerned with simulating machines and time
	virtual double now() { return time; }

	// timer() can be up to 0.1 seconds ahead of now()
	virtual double timer() {
		timerTime += deterministicRandom()->random01()*(time+0.1-timerTime)/2.0;
		return timerTime; 
	}

	virtual Future<class Void> delay( double seconds, TaskPriority taskID ) {
		ASSERT(taskID >= TaskPriority::Min && taskID <= TaskPriority::Max);
		return delay( seconds, taskID, currentProcess );
	}
	Future<class Void> delay( double seconds, TaskPriority taskID, ProcessInfo* machine ) {
		ASSERT( seconds >= -0.0001 );
		seconds = std::max(0.0, seconds);
		Future<Void> f;

		if(!currentProcess->rebooting && machine == currentProcess && !currentProcess->shutdownSignal.isSet() && FLOW_KNOBS->MAX_BUGGIFIED_DELAY > 0 && deterministicRandom()->random01() < 0.25) { //FIXME: why doesnt this work when we are changing machines?
			seconds += FLOW_KNOBS->MAX_BUGGIFIED_DELAY*pow(deterministicRandom()->random01(),1000.0);
		}

		mutex.enter();
		tasks.push( Task( time + seconds, taskID, taskCount++, machine, f ) );
		mutex.leave();

		return f;
	}
	ACTOR static Future<Void> checkShutdown(Sim2 *self, TaskPriority taskID) {
		wait(success(self->getCurrentProcess()->shutdownSignal.getFuture()));
		self->setCurrentTask(taskID);
		return Void();
	}
	virtual Future<class Void> yield( TaskPriority taskID ) {
		if (taskID == TaskPriority::DefaultYield) taskID = currentTaskID;
		if (check_yield(taskID)) {
			// We want to check that yielders can handle actual time elapsing (it sometimes will outside simulation), but
			// don't want to prevent instantaneous shutdown of "rebooted" machines.
			return delay(getCurrentProcess()->rebooting ? 0 : .001,taskID) || checkShutdown(this, taskID);
		}
		setCurrentTask(taskID);
		return Void();
	}
	virtual bool check_yield( TaskPriority taskID ) {
		if (yielded) return true;
		if (--yield_limit <= 0) {
			yield_limit = deterministicRandom()->randomInt(1, 150);  // If yield returns false *too* many times in a row, there could be a stack overflow, since we can't deterministically check stack size as the real network does
			return yielded = true;
		}
		return yielded = BUGGIFY_WITH_PROB(0.01);
	}
	virtual TaskPriority getCurrentTask() {
		return currentTaskID;
	}
	virtual void setCurrentTask(TaskPriority taskID ) {
		currentTaskID = taskID;
	}
	// Sets the taskID/priority of the current task, without yielding
	virtual Future<Reference<IConnection>> connect( NetworkAddress toAddr, std::string host ) {
		ASSERT( host.empty());
		if (!addressMap.count( toAddr )) {
			return waitForProcessAndConnect( toAddr, this );
		}
		auto peerp = getProcessByAddress(toAddr);
		Reference<Sim2Conn> myc( new Sim2Conn( getCurrentProcess() ) );
		Reference<Sim2Conn> peerc( new Sim2Conn( peerp ) );

		myc->connect(peerc, toAddr);
		IPAddress localIp;
		if (getCurrentProcess()->address.ip.isV6()) {
			IPAddress::IPAddressStore store = getCurrentProcess()->address.ip.toV6();
			uint16_t* ipParts = (uint16_t*)store.data();
			ipParts[7] += deterministicRandom()->randomInt(0, 256);
			localIp = IPAddress(store);
		} else {
			localIp = IPAddress(getCurrentProcess()->address.ip.toV4() + deterministicRandom()->randomInt(0, 256));
		}
		peerc->connect(myc, NetworkAddress(localIp, deterministicRandom()->randomInt(40000, 60000), false, toAddr.isTLS()));

		((Sim2Listener*)peerp->getListener(toAddr).getPtr())->incomingConnection( 0.5*deterministicRandom()->random01(), Reference<IConnection>(peerc) );
		return onConnect( ::delay(0.5*deterministicRandom()->random01()), myc );
	}
	virtual Future<std::vector<NetworkAddress>> resolveTCPEndpoint( std::string host, std::string service) {
		throw lookup_failed();
	}
	ACTOR static Future<Reference<IConnection>> onConnect( Future<Void> ready, Reference<Sim2Conn> conn ) {
		wait(ready);
		if (conn->isPeerGone()) {
			conn.clear();
			if(FLOW_KNOBS->SIM_CONNECT_ERROR_MODE == 1 || (FLOW_KNOBS->SIM_CONNECT_ERROR_MODE == 2 && deterministicRandom()->random01() > 0.5)) {
				throw connection_failed();
			}
			wait(Never());
		}
		conn->opened = true;
		return conn;
	}
	virtual Reference<IListener> listen( NetworkAddress localAddr ) {
		Reference<IListener> listener( getCurrentProcess()->getListener(localAddr) );
		ASSERT(listener);
		return listener;
	}
	ACTOR static Future<Reference<IConnection>> waitForProcessAndConnect(
			NetworkAddress toAddr, INetworkConnections *self ) {
		// We have to be able to connect to processes that don't yet exist, so we do some silly polling
		loop {
			wait( ::delay( 0.1 * deterministicRandom()->random01() ) );
			if (g_sim2.addressMap.count(toAddr)) {
				Reference<IConnection> c = wait( self->connect( toAddr ) );
				return c;
			}
		}
	}
	virtual const TLSConfig& getTLSConfig() {
		static TLSConfig emptyConfig;
		return emptyConfig;
	}

	virtual void stop() {
		isStopped = true;
	}
	virtual void addStopCallback( std::function<void()> fn ) {
		stopCallbacks.emplace_back(std::move(fn));
	}
	virtual bool isSimulated() const { return true; }

	struct SimThreadArgs {
		THREAD_FUNC_RETURN (*func) (void*);
		void *arg;

		ISimulator::ProcessInfo *currentProcess;

		SimThreadArgs(THREAD_FUNC_RETURN (*func) (void*), void *arg) : func(func), arg(arg) {
			ASSERT(g_network->isSimulated());
			currentProcess = g_simulator.getCurrentProcess();
		}
	};

	//Starts a new thread, making sure to set any thread local state
	THREAD_FUNC simStartThread(void *arg) {
		SimThreadArgs *simArgs = (SimThreadArgs*)arg;
		ISimulator::currentProcess = simArgs->currentProcess;
		simArgs->func(simArgs->arg);

		delete simArgs;
		THREAD_RETURN;
	}

	virtual THREAD_HANDLE startThread( THREAD_FUNC_RETURN (*func) (void*), void *arg ) {
		SimThreadArgs *simArgs = new SimThreadArgs(func, arg);
		return ::startThread(simStartThread, simArgs);
	}

	virtual void getDiskBytes( std::string const& directory, int64_t& free, int64_t& total) {
		ProcessInfo *proc = getCurrentProcess();
		SimDiskSpace &diskSpace = diskSpaceMap[proc->address.ip];

		int64_t totalFileSize = 0;
		int numFiles = 0;

		//Get the size of all files we've created on the server and subtract them from the free space
		for(auto file = proc->machine->openFiles.begin(); file != proc->machine->openFiles.end(); ++file) {
			if( file->second.isReady() ) {
				totalFileSize += ((AsyncFileNonDurable*)file->second.get().getPtr())->approximateSize;
			}
			numFiles++;
		}

		if(diskSpace.totalSpace == 0) {
			diskSpace.totalSpace = 5e9 + deterministicRandom()->random01() * 100e9; //Total space between 5GB and 105GB
			diskSpace.baseFreeSpace = std::min<int64_t>(diskSpace.totalSpace, std::max(5e9, (deterministicRandom()->random01() * (1 - .075) + .075) * diskSpace.totalSpace) + totalFileSize); //Minimum 5GB or 7.5% total disk space, whichever is higher

			TraceEvent("Sim2DiskSpaceInitialization").detail("TotalSpace", diskSpace.totalSpace).detail("BaseFreeSpace", diskSpace.baseFreeSpace).detail("TotalFileSize", totalFileSize).detail("NumFiles", numFiles);
		}
		else {
			int64_t maxDelta = std::min(5.0, (now() - diskSpace.lastUpdate)) * (BUGGIFY ? 10e6 : 1e6); //External processes modifying the disk
			int64_t delta = -maxDelta + deterministicRandom()->random01() * maxDelta * 2;
			diskSpace.baseFreeSpace = std::min<int64_t>(diskSpace.totalSpace, std::max<int64_t>(diskSpace.baseFreeSpace + delta, totalFileSize));
		}

		diskSpace.lastUpdate = now();

		total = diskSpace.totalSpace;
		free = std::max<int64_t>(0, diskSpace.baseFreeSpace - totalFileSize);

		if(free == 0)
			TraceEvent(SevWarnAlways, "Sim2NoFreeSpace").detail("TotalSpace", diskSpace.totalSpace).detail("BaseFreeSpace", diskSpace.baseFreeSpace).detail("TotalFileSize", totalFileSize).detail("NumFiles", numFiles);
	}
	virtual bool isAddressOnThisHost( NetworkAddress const& addr ) {
		return addr.ip == getCurrentProcess()->address.ip;
	}

	ACTOR static Future<Void> deleteFileImpl( Sim2* self, std::string filename, bool mustBeDurable ) {
		// This is a _rudimentary_ simulation of the untrustworthiness of non-durable deletes and the possibility of
		// rebooting during a durable one.  It isn't perfect: for example, on real filesystems testing
		// for the existence of a non-durably deleted file BEFORE a reboot will show that it apparently doesn't exist.
		if(g_simulator.getCurrentProcess()->machine->openFiles.count(filename)) {
			g_simulator.getCurrentProcess()->machine->openFiles.erase(filename);
			g_simulator.getCurrentProcess()->machine->deletingFiles.insert(filename);
		}
		if ( mustBeDurable || deterministicRandom()->random01() < 0.5 ) {
			state ISimulator::ProcessInfo* currentProcess = g_simulator.getCurrentProcess();
			state TaskPriority currentTaskID = g_network->getCurrentTask();
			wait( g_simulator.onMachine( currentProcess ) );
			try {
				wait( ::delay(0.05 * deterministicRandom()->random01()) );
				if (!currentProcess->rebooting) {
					auto f = IAsyncFileSystem::filesystem(self->net2)->deleteFile(filename, false);
					ASSERT( f.isReady() );
					wait( ::delay(0.05 * deterministicRandom()->random01()) );
					TEST( true );  // Simulated durable delete
				}
				wait( g_simulator.onProcess( currentProcess, currentTaskID ) );
				return Void();
			} catch( Error &e ) {
				state Error err = e;
				wait( g_simulator.onProcess( currentProcess, currentTaskID ) );
				throw err;
			}
		} else {
			TEST( true );  // Simulated non-durable delete
			return Void();
		}
	}

	ACTOR static Future<Void> runLoop(Sim2 *self) {
		state ISimulator::ProcessInfo *callingMachine = self->currentProcess;
		while ( !self->isStopped ) {
			wait( self->net2->yield(TaskPriority::DefaultYield) );

			self->mutex.enter();
			if( self->tasks.size() == 0 ) {
				self->mutex.leave();
				ASSERT(false);
			}
			//if (!randLog/* && now() >= 32.0*/)
			//	randLog = fopen("randLog.txt", "wt");
			Task t = std::move( self->tasks.top() ); // Unfortunately still a copy under gcc where .top() returns const&
			self->currentTaskID = t.taskID;
			self->tasks.pop();
			self->mutex.leave();

			self->execTask(t);
			self->yielded = false;
		}
		self->currentProcess = callingMachine;
		self->net2->stop();
		for ( auto& fn : self->stopCallbacks ) {
			fn();
		}
		return Void();
	}

	ACTOR Future<Void> _run(Sim2 *self) {
		Future<Void> loopFuture = self->runLoop(self);
		self->net2->run();
		wait( loopFuture );
		return Void();
	}

	// Implement ISimulator interface
	virtual void run() {
		_run(this);
	}
	virtual ProcessInfo* newProcess(const char* name, IPAddress ip, uint16_t port, bool sslEnabled, uint16_t listenPerProcess,
	                                LocalityData locality, ProcessClass startingClass, const char* dataFolder,
	                                const char* coordinationFolder) {
		ASSERT( locality.machineId().present() );
		MachineInfo& machine = machines[ locality.machineId().get() ];
		if (!machine.machineId.present())
			machine.machineId = locality.machineId();
		for( int i = 0; i < machine.processes.size(); i++ ) {
			if( machine.processes[i]->locality.machineId() != locality.machineId() ) { // SOMEDAY: compute ip from locality to avoid this check
				TraceEvent("Sim2Mismatch")
				    .detail("IP", format("%s", ip.toString().c_str()))
				    .detail("MachineId", locality.machineId())
				    .detail("NewName", name)
				    .detail("ExistingMachineId", machine.processes[i]->locality.machineId())
				    .detail("ExistingName", machine.processes[i]->name);
				ASSERT( false );
			}
			ASSERT( machine.processes[i]->address.port != port );
		}

		// This is for async operations on non-durable files.
		// These files must live on after process kills for sim purposes.
		if( machine.machineProcess == 0 ) {
			NetworkAddress machineAddress(ip, 0, false, false);
			machine.machineProcess = new ProcessInfo("Machine", locality, startingClass, {machineAddress}, this, "", "");
			machine.machineProcess->machine = &machine;
		}

		NetworkAddressList addresses;
		addresses.address = NetworkAddress(ip, port, true, sslEnabled);
		if(listenPerProcess == 2) {
			addresses.secondaryAddress = NetworkAddress(ip, port+1, true, false);
		}

		ProcessInfo* m = new ProcessInfo(name, locality, startingClass, addresses, this, dataFolder, coordinationFolder);
		for (int processPort = port; processPort < port + listenPerProcess; ++processPort) {
			NetworkAddress address(ip, processPort, true, sslEnabled && processPort == port);
			m->listenerMap[address] = Reference<IListener>( new Sim2Listener(m, address) );
			addressMap[address] = m;
		}
		m->machine = &machine;
		machine.processes.push_back(m);
		currentlyRebootingProcesses.erase(addresses.address);
		m->excluded = g_simulator.isExcluded(addresses.address);
		m->cleared = g_simulator.isCleared(addresses.address);

		m->setGlobal(enTDMetrics, (flowGlobalType) &m->tdmetrics);
		m->setGlobal(enNetworkConnections, (flowGlobalType) m->network);
		m->setGlobal(enASIOTimedOut, (flowGlobalType) false);

		TraceEvent("NewMachine").detail("Name", name).detail("Address", m->address).detail("MachineId", m->locality.machineId()).detail("Excluded", m->excluded).detail("Cleared", m->cleared);

		// FIXME: Sometimes, connections to/from this process will explicitly close

		return m;
	}
	virtual bool isAvailable() const
	{
		std::vector<ProcessInfo*> processesLeft, processesDead;
		for (auto processInfo : getAllProcesses()) {
			if (processInfo->isAvailableClass()) {
				if (processInfo->isExcluded() || processInfo->isCleared() || !processInfo->isAvailable()) {
					processesDead.push_back(processInfo);
				} else {
					processesLeft.push_back(processInfo);
				}
			}
		}
		return canKillProcesses(processesLeft, processesDead, KillInstantly, NULL);
	}

	virtual bool datacenterDead(Optional<Standalone<StringRef>> dcId) const
	{
		if(!dcId.present()) {
			return false;
		}

		LocalityGroup primaryProcessesLeft, primaryProcessesDead;
		std::vector<LocalityData> primaryLocalitiesDead, primaryLocalitiesLeft;

		for (auto processInfo : getAllProcesses()) {
			if (processInfo->isAvailableClass() && processInfo->locality.dcId() == dcId) {
				if (processInfo->isExcluded() || processInfo->isCleared() || !processInfo->isAvailable()) {
					primaryProcessesDead.add(processInfo->locality);
					primaryLocalitiesDead.push_back(processInfo->locality);
				} else {
					primaryProcessesLeft.add(processInfo->locality);
					primaryLocalitiesLeft.push_back(processInfo->locality);
				}
			}
		}

		std::vector<LocalityData> badCombo;
		bool primaryTLogsDead = tLogWriteAntiQuorum ? !validateAllCombinations(badCombo, primaryProcessesDead, tLogPolicy, primaryLocalitiesLeft, tLogWriteAntiQuorum, false) : primaryProcessesDead.validate(tLogPolicy);
		if(usableRegions > 1 && remoteTLogPolicy && !primaryTLogsDead) {
			primaryTLogsDead = primaryProcessesDead.validate(remoteTLogPolicy);
		}

		return primaryTLogsDead || primaryProcessesDead.validate(storagePolicy);
	}

	// The following function will determine if the specified configuration of available and dead processes can allow the cluster to survive
	virtual bool canKillProcesses(std::vector<ProcessInfo*> const& availableProcesses, std::vector<ProcessInfo*> const& deadProcesses, KillType kt, KillType* newKillType) const
	{
		bool canSurvive = true;
		int nQuorum = ((desiredCoordinators+1)/2)*2-1;

		KillType newKt = kt;
		if ((kt == KillInstantly) || (kt == InjectFaults) || (kt == RebootAndDelete) || (kt == RebootProcessAndDelete))
		{
			LocalityGroup primaryProcessesLeft, primaryProcessesDead;
			LocalityGroup primarySatelliteProcessesLeft, primarySatelliteProcessesDead;
			LocalityGroup remoteProcessesLeft, remoteProcessesDead;
			LocalityGroup remoteSatelliteProcessesLeft, remoteSatelliteProcessesDead;

			std::vector<LocalityData> primaryLocalitiesDead, primaryLocalitiesLeft;
			std::vector<LocalityData> primarySatelliteLocalitiesDead, primarySatelliteLocalitiesLeft;
			std::vector<LocalityData> remoteLocalitiesDead, remoteLocalitiesLeft;
			std::vector<LocalityData> remoteSatelliteLocalitiesDead, remoteSatelliteLocalitiesLeft;

			std::vector<LocalityData> badCombo;
			std::set<Optional<Standalone<StringRef>>> uniqueMachines;

			if(!primaryDcId.present()) {
				for (auto processInfo : availableProcesses) {
					primaryProcessesLeft.add(processInfo->locality);
					primaryLocalitiesLeft.push_back(processInfo->locality);
					uniqueMachines.insert(processInfo->locality.zoneId());
				}
				for (auto processInfo : deadProcesses) {
					primaryProcessesDead.add(processInfo->locality);
					primaryLocalitiesDead.push_back(processInfo->locality);
				}
			} else {
				for (auto processInfo : availableProcesses) {
					uniqueMachines.insert(processInfo->locality.zoneId());
					if(processInfo->locality.dcId() == primaryDcId) {
						primaryProcessesLeft.add(processInfo->locality);
						primaryLocalitiesLeft.push_back(processInfo->locality);
					} else if(processInfo->locality.dcId() == remoteDcId) {
						remoteProcessesLeft.add(processInfo->locality);
						remoteLocalitiesLeft.push_back(processInfo->locality);
					} else if(std::find(primarySatelliteDcIds.begin(), primarySatelliteDcIds.end(), processInfo->locality.dcId()) != primarySatelliteDcIds.end()) {
						primarySatelliteProcessesLeft.add(processInfo->locality);
						primarySatelliteLocalitiesLeft.push_back(processInfo->locality);
					} else if(std::find(remoteSatelliteDcIds.begin(), remoteSatelliteDcIds.end(), processInfo->locality.dcId()) != remoteSatelliteDcIds.end()) {
						remoteSatelliteProcessesLeft.add(processInfo->locality);
						remoteSatelliteLocalitiesLeft.push_back(processInfo->locality);
					}
				}
				for (auto processInfo : deadProcesses) {
					if(processInfo->locality.dcId() == primaryDcId) {
						primaryProcessesDead.add(processInfo->locality);
						primaryLocalitiesDead.push_back(processInfo->locality);
					} else if(processInfo->locality.dcId() == remoteDcId) {
						remoteProcessesDead.add(processInfo->locality);
						remoteLocalitiesDead.push_back(processInfo->locality);
					} else if(std::find(primarySatelliteDcIds.begin(), primarySatelliteDcIds.end(), processInfo->locality.dcId()) != primarySatelliteDcIds.end()) {
						primarySatelliteProcessesDead.add(processInfo->locality);
						primarySatelliteLocalitiesDead.push_back(processInfo->locality);
					} else if(std::find(remoteSatelliteDcIds.begin(), remoteSatelliteDcIds.end(), processInfo->locality.dcId()) != remoteSatelliteDcIds.end()) {
						remoteSatelliteProcessesDead.add(processInfo->locality);
						remoteSatelliteLocalitiesDead.push_back(processInfo->locality);
					}
				}
			}

			bool tooManyDead = false;
			bool notEnoughLeft = false;
			bool primaryTLogsDead = tLogWriteAntiQuorum ? !validateAllCombinations(badCombo, primaryProcessesDead, tLogPolicy, primaryLocalitiesLeft, tLogWriteAntiQuorum, false) : primaryProcessesDead.validate(tLogPolicy);
			if(usableRegions > 1 && remoteTLogPolicy && !primaryTLogsDead) {
				primaryTLogsDead = primaryProcessesDead.validate(remoteTLogPolicy);
			}

			if(!primaryDcId.present()) {
				tooManyDead = primaryTLogsDead || primaryProcessesDead.validate(storagePolicy);
				notEnoughLeft = !primaryProcessesLeft.validate(tLogPolicy) || !primaryProcessesLeft.validate(storagePolicy);
			} else {
				bool remoteTLogsDead = tLogWriteAntiQuorum ? !validateAllCombinations(badCombo, remoteProcessesDead, tLogPolicy, remoteLocalitiesLeft, tLogWriteAntiQuorum, false) : remoteProcessesDead.validate(tLogPolicy);
				if(usableRegions > 1 && remoteTLogPolicy && !remoteTLogsDead) {
					remoteTLogsDead = remoteProcessesDead.validate(remoteTLogPolicy);
				}

				if(!hasSatelliteReplication) {
					if(usableRegions > 1) {
						tooManyDead = primaryTLogsDead || remoteTLogsDead || ( primaryProcessesDead.validate(storagePolicy) && remoteProcessesDead.validate(storagePolicy) );
						notEnoughLeft = !primaryProcessesLeft.validate(tLogPolicy) || !primaryProcessesLeft.validate(remoteTLogPolicy) || !primaryProcessesLeft.validate(storagePolicy) || !remoteProcessesLeft.validate(tLogPolicy) || !remoteProcessesLeft.validate(remoteTLogPolicy) || !remoteProcessesLeft.validate(storagePolicy);
					} else {
						tooManyDead = primaryTLogsDead || remoteTLogsDead || primaryProcessesDead.validate(storagePolicy) || remoteProcessesDead.validate(storagePolicy);
						notEnoughLeft = !primaryProcessesLeft.validate(tLogPolicy) || !primaryProcessesLeft.validate(storagePolicy) || !remoteProcessesLeft.validate(tLogPolicy) || !remoteProcessesLeft.validate(storagePolicy);
					}
				} else {
					bool primarySatelliteTLogsDead = satelliteTLogWriteAntiQuorumFallback ? !validateAllCombinations(badCombo, primarySatelliteProcessesDead, satelliteTLogPolicyFallback, primarySatelliteLocalitiesLeft, satelliteTLogWriteAntiQuorumFallback, false) : primarySatelliteProcessesDead.validate(satelliteTLogPolicyFallback);
					bool remoteSatelliteTLogsDead = satelliteTLogWriteAntiQuorumFallback ? !validateAllCombinations(badCombo, remoteSatelliteProcessesDead, satelliteTLogPolicyFallback, remoteSatelliteLocalitiesLeft, satelliteTLogWriteAntiQuorumFallback, false) : remoteSatelliteProcessesDead.validate(satelliteTLogPolicyFallback);

					if(usableRegions > 1) {
						notEnoughLeft = !primaryProcessesLeft.validate(tLogPolicy) || !primaryProcessesLeft.validate(remoteTLogPolicy) || !primaryProcessesLeft.validate(storagePolicy) || !primarySatelliteProcessesLeft.validate(satelliteTLogPolicy) || !remoteProcessesLeft.validate(tLogPolicy) || !remoteProcessesLeft.validate(remoteTLogPolicy) || !remoteProcessesLeft.validate(storagePolicy) || !remoteSatelliteProcessesLeft.validate(satelliteTLogPolicy);
					} else {
						notEnoughLeft = !primaryProcessesLeft.validate(tLogPolicy) || !primaryProcessesLeft.validate(storagePolicy) || !primarySatelliteProcessesLeft.validate(satelliteTLogPolicy) || !remoteProcessesLeft.validate(tLogPolicy) || !remoteProcessesLeft.validate(storagePolicy) || !remoteSatelliteProcessesLeft.validate(satelliteTLogPolicy);
					}

					if(usableRegions > 1 && allowLogSetKills) {
						tooManyDead = ( primaryTLogsDead && primarySatelliteTLogsDead ) || ( remoteTLogsDead && remoteSatelliteTLogsDead ) || ( primaryTLogsDead && remoteTLogsDead ) || ( primaryProcessesDead.validate(storagePolicy) && remoteProcessesDead.validate(storagePolicy) );
					} else {
						tooManyDead = primaryTLogsDead || remoteTLogsDead || primaryProcessesDead.validate(storagePolicy) || remoteProcessesDead.validate(storagePolicy);
					}
				}
			}

			// Reboot if dead machines do fulfill policies
			if (tooManyDead) {
				newKt = Reboot;
				canSurvive = false;
				TraceEvent("KillChanged").detail("KillType", kt).detail("NewKillType", newKt).detail("TLogPolicy", tLogPolicy->info()).detail("Reason", "tLogPolicy validates against dead processes.");
			}
			// Reboot and Delete if remaining machines do NOT fulfill policies
			else if ((kt < RebootAndDelete) && notEnoughLeft) {
				newKt = RebootAndDelete;
				canSurvive = false;
				TraceEvent("KillChanged").detail("KillType", kt).detail("NewKillType", newKt).detail("TLogPolicy", tLogPolicy->info()).detail("Reason", "tLogPolicy does not validates against remaining processes.");
			}
			else if ((kt < RebootAndDelete) && (nQuorum > uniqueMachines.size())) {
				newKt = RebootAndDelete;
				canSurvive = false;
				TraceEvent("KillChanged").detail("KillType", kt).detail("NewKillType", newKt).detail("StoragePolicy", storagePolicy->info()).detail("Quorum", nQuorum).detail("Machines", uniqueMachines.size()).detail("Reason", "Not enough unique machines to perform auto configuration of coordinators.");
			}
			else {
				TraceEvent("CanSurviveKills").detail("KillType", kt).detail("TLogPolicy", tLogPolicy->info()).detail("StoragePolicy", storagePolicy->info()).detail("Quorum", nQuorum).detail("Machines", uniqueMachines.size());
			}
		}
		if (newKillType) *newKillType = newKt;
		return canSurvive;
	}

	virtual void destroyProcess( ISimulator::ProcessInfo *p ) {
		TraceEvent("ProcessDestroyed").detail("Name", p->name).detail("Address", p->address).detail("MachineId", p->locality.machineId());
		currentlyRebootingProcesses.insert(std::pair<NetworkAddress, ProcessInfo*>(p->address, p));
		std::vector<ProcessInfo*>& processes = machines[ p->locality.machineId().get() ].processes;
		if( p != processes.back() ) {
			auto it = std::find( processes.begin(), processes.end(), p );
			std::swap( *it, processes.back() );
		}
		processes.pop_back();
		killProcess_internal( p, KillInstantly );
	}
	void killProcess_internal( ProcessInfo* machine, KillType kt ) {
		TEST( true ); // Simulated machine was killed with any kill type
		TEST( kt == KillInstantly ); // Simulated machine was killed instantly
		TEST( kt == InjectFaults ); // Simulated machine was killed with faults

		if (kt == KillInstantly) {
			TraceEvent(SevWarn, "FailMachine").detail("Name", machine->name).detail("Address", machine->address).detail("ZoneId", machine->locality.zoneId()).detail("Process", machine->toString()).detail("Rebooting", machine->rebooting).detail("Protected", protectedAddresses.count(machine->address)).backtrace();
			// This will remove all the "tracked" messages that came from the machine being killed
			latestEventCache.clear();
			machine->failed = true;
		} else if (kt == InjectFaults) {
			TraceEvent(SevWarn, "FaultMachine").detail("Name", machine->name).detail("Address", machine->address).detail("ZoneId", machine->locality.zoneId()).detail("Process", machine->toString()).detail("Rebooting", machine->rebooting).detail("Protected", protectedAddresses.count(machine->address)).backtrace();
			should_inject_fault = simulator_should_inject_fault;
			machine->fault_injection_r = deterministicRandom()->randomUniqueID().first();
			machine->fault_injection_p1 = 0.1;
			machine->fault_injection_p2 = deterministicRandom()->random01();
		} else {
			ASSERT( false );
		}
		ASSERT(!protectedAddresses.count(machine->address) || machine->rebooting);
	}
	virtual void rebootProcess( ProcessInfo* process, KillType kt ) {
		if( kt == RebootProcessAndDelete && protectedAddresses.count(process->address) ) {
			TraceEvent("RebootChanged").detail("ZoneId", process->locality.describeZone()).detail("KillType", RebootProcess).detail("OrigKillType", kt).detail("Reason", "Protected process");
			kt = RebootProcess;
		}
		doReboot( process, kt );
	}
	virtual void rebootProcess(Optional<Standalone<StringRef>> zoneId, bool allProcesses ) {
		if( allProcesses ) {
			auto processes = getAllProcesses();
			for( int i = 0; i < processes.size(); i++ )
				if( processes[i]->locality.zoneId() == zoneId && !processes[i]->rebooting )
					doReboot( processes[i], RebootProcess );
		} else {
			auto processes = getAllProcesses();
			for( int i = 0; i < processes.size(); i++ ) {
				if( processes[i]->locality.zoneId() != zoneId || processes[i]->rebooting ) {
					swapAndPop(&processes, i--);
				}
			}
			if( processes.size() )
				doReboot( deterministicRandom()->randomChoice( processes ), RebootProcess );
		}
	}
	virtual void killProcess( ProcessInfo* machine, KillType kt ) {
		TraceEvent("AttemptingKillProcess");
		if (kt < RebootAndDelete ) {
			killProcess_internal( machine, kt );
		}
	}
	virtual void killInterface( NetworkAddress address, KillType kt  ) {
		if (kt < RebootAndDelete ) {
			std::vector<ProcessInfo*>& processes = machines[ addressMap[address]->locality.machineId() ].processes;
			for( int i = 0; i < processes.size(); i++ )
				killProcess_internal( processes[i], kt );
		}
	}
	virtual bool killZone(Optional<Standalone<StringRef>> zoneId, KillType kt, bool forceKill, KillType* ktFinal) {
		auto processes = getAllProcesses();
		std::set<Optional<Standalone<StringRef>>> zoneMachines;
		for (auto& process : processes) {
			if(process->locality.zoneId() == zoneId) {
				zoneMachines.insert(process->locality.machineId());
			}
		}
		bool result = false;
		for(auto& machineId : zoneMachines) {
			if(killMachine(machineId, kt, forceKill, ktFinal)) {
				result = true;
			}
		}
		return result;
	}
	virtual bool killMachine(Optional<Standalone<StringRef>> machineId, KillType kt, bool forceKill, KillType* ktFinal) {
		auto ktOrig = kt;

		TEST(true); // Trying to killing a machine
		TEST(kt == KillInstantly); // Trying to kill instantly
		TEST(kt == InjectFaults);  // Trying to kill by injecting faults

		if(speedUpSimulation && !forceKill) {
			TraceEvent(SevWarn, "AbortedKill").detail("MachineId", machineId).detail("Reason", "Unforced kill within speedy simulation.").backtrace();
			if (ktFinal) *ktFinal = None;
			return false;
		}

		int processesOnMachine = 0;

		KillType originalKt = kt;
		// Reboot if any of the processes are protected and count the number of processes not rebooting
		for (auto& process : machines[machineId].processes) {
			if (protectedAddresses.count(process->address))
				kt = Reboot;
			if (!process->rebooting)
				processesOnMachine++;
		}

		// Do nothing, if no processes to kill
		if (processesOnMachine == 0) {
			TraceEvent(SevWarn, "AbortedKill").detail("MachineId", machineId).detail("Reason", "The target had no processes running.").detail("Processes", processesOnMachine).detail("ProcessesPerMachine", processesPerMachine).backtrace();
			if (ktFinal) *ktFinal = None;
			return false;
		}

		// Check if machine can be removed, if requested
		if (!forceKill && ((kt == KillInstantly) || (kt == InjectFaults) || (kt == RebootAndDelete) || (kt == RebootProcessAndDelete)))
		{
			std::vector<ProcessInfo*> processesLeft, processesDead;
			int	protectedWorker = 0, unavailable = 0, excluded = 0, cleared = 0;

			for (auto processInfo : getAllProcesses()) {
				if (processInfo->isAvailableClass()) {
					if (processInfo->isExcluded()) {
						processesDead.push_back(processInfo);
						excluded++;
					}
					else if (processInfo->isCleared()) {
						processesDead.push_back(processInfo);
						cleared++;
					}
					else if (!processInfo->isAvailable()) {
						processesDead.push_back(processInfo);
						unavailable++;
					}
					else if (protectedAddresses.count(processInfo->address)) {
						processesLeft.push_back(processInfo);
						protectedWorker++;
					}
					else if (processInfo->locality.machineId() != machineId) {
						processesLeft.push_back(processInfo);
					} else {
						processesDead.push_back(processInfo);
					}
				}
			}
			if (!canKillProcesses(processesLeft, processesDead, kt, &kt)) {
				TraceEvent("ChangedKillMachine").detail("MachineId", machineId).detail("KillType", kt).detail("OrigKillType", ktOrig).detail("ProcessesLeft", processesLeft.size()).detail("ProcessesDead", processesDead.size()).detail("TotalProcesses", machines.size()).detail("ProcessesPerMachine", processesPerMachine).detail("Protected", protectedWorker).detail("Unavailable", unavailable).detail("Excluded", excluded).detail("Cleared", cleared).detail("ProtectedTotal", protectedAddresses.size()).detail("TLogPolicy", tLogPolicy->info()).detail("StoragePolicy", storagePolicy->info());
			}
			else if ((kt == KillInstantly) || (kt == InjectFaults)) {
				TraceEvent("DeadMachine").detail("MachineId", machineId).detail("KillType", kt).detail("ProcessesLeft", processesLeft.size()).detail("ProcessesDead", processesDead.size()).detail("TotalProcesses", machines.size()).detail("ProcessesPerMachine", processesPerMachine).detail("TLogPolicy", tLogPolicy->info()).detail("StoragePolicy", storagePolicy->info());
				for (auto process : processesLeft) {
					TraceEvent("DeadMachineSurvivors").detail("MachineId", machineId).detail("KillType", kt).detail("ProcessesLeft", processesLeft.size()).detail("ProcessesDead", processesDead.size()).detail("SurvivingProcess", process->toString());
				}
				for (auto process : processesDead) {
					TraceEvent("DeadMachineVictims").detail("MachineId", machineId).detail("KillType", kt).detail("ProcessesLeft", processesLeft.size()).detail("ProcessesDead", processesDead.size()).detail("VictimProcess", process->toString());
				}
			}
			else {
				TraceEvent("ClearMachine").detail("MachineId", machineId).detail("KillType", kt).detail("ProcessesLeft", processesLeft.size()).detail("ProcessesDead", processesDead.size()).detail("TotalProcesses", machines.size()).detail("ProcessesPerMachine", processesPerMachine).detail("TLogPolicy", tLogPolicy->info()).detail("StoragePolicy", storagePolicy->info());
				for (auto process : processesLeft) {
					TraceEvent("ClearMachineSurvivors").detail("MachineId", machineId).detail("KillType", kt).detail("ProcessesLeft", processesLeft.size()).detail("ProcessesDead", processesDead.size()).detail("SurvivingProcess", process->toString());
				}
				for (auto process : processesDead) {
					TraceEvent("ClearMachineVictims").detail("MachineId", machineId).detail("KillType", kt).detail("ProcessesLeft", processesLeft.size()).detail("ProcessesDead", processesDead.size()).detail("VictimProcess", process->toString());
				}
			}
		}

		TEST(originalKt != kt);  // Kill type was changed from requested to reboot.

		// Check if any processes on machine are rebooting
		if( processesOnMachine != processesPerMachine && kt >= RebootAndDelete ) {
			TEST(true); //Attempted reboot, but the target did not have all of its processes running
			TraceEvent(SevWarn, "AbortedKill").detail("KillType", kt).detail("MachineId", machineId).detail("Reason", "Machine processes does not match number of processes per machine").detail("Processes", processesOnMachine).detail("ProcessesPerMachine", processesPerMachine).backtrace();
			if (ktFinal) *ktFinal = None;
			return false;
		}

		// Check if any processes on machine are rebooting
		if ( processesOnMachine != processesPerMachine ) {
			TEST(true); //Attempted reboot, but the target did not have all of its processes running
			TraceEvent(SevWarn, "AbortedKill").detail("KillType", kt).detail("MachineId", machineId).detail("Reason", "Machine processes does not match number of processes per machine").detail("Processes", processesOnMachine).detail("ProcessesPerMachine", processesPerMachine).backtrace();
			if (ktFinal) *ktFinal = None;
			return false;
		}

		TraceEvent("KillMachine").detail("MachineId", machineId).detail("Kt", kt).detail("KtOrig", ktOrig).detail("KillableMachines", processesOnMachine).detail("ProcessPerMachine", processesPerMachine).detail("KillChanged", kt!=ktOrig);
		if ( kt < RebootAndDelete ) {
			if(kt == InjectFaults && machines[machineId].machineProcess != nullptr)
				killProcess_internal( machines[machineId].machineProcess, kt );
			for (auto& process : machines[machineId].processes) {
				TraceEvent("KillMachineProcess").detail("KillType", kt).detail("Process", process->toString()).detail("StartingClass", process->startingClass.toString()).detail("Failed", process->failed).detail("Excluded", process->excluded).detail("Cleared", process->cleared).detail("Rebooting", process->rebooting);
				if (process->startingClass != ProcessClass::TesterClass)
					killProcess_internal( process, kt );
			}
		}
		else if ( kt == Reboot || kt == RebootAndDelete ) {
			for (auto& process : machines[machineId].processes) {
				TraceEvent("KillMachineProcess").detail("KillType", kt).detail("Process", process->toString()).detail("StartingClass", process->startingClass.toString()).detail("Failed", process->failed).detail("Excluded", process->excluded).detail("Cleared", process->cleared).detail("Rebooting", process->rebooting);
				if (process->startingClass != ProcessClass::TesterClass)
					doReboot(process, kt );
			}
		}

		TEST(kt == RebootAndDelete); // Resulted in a reboot and delete
		TEST(kt == Reboot); // Resulted in a reboot
		TEST(kt == KillInstantly); // Resulted in an instant kill
		TEST(kt == InjectFaults);  // Resulted in a kill by injecting faults

		if (ktFinal) *ktFinal = kt;
		return true;
	}

	virtual bool killDataCenter(Optional<Standalone<StringRef>> dcId, KillType kt, bool forceKill, KillType* ktFinal) {
		auto ktOrig = kt;
		auto processes = getAllProcesses();
		std::map<Optional<Standalone<StringRef>>, int> datacenterMachines;
		int	dcProcesses = 0;

		// Switch to a reboot, if anything protected on machine
		for (auto& procRecord : processes) {
			auto processDcId = procRecord->locality.dcId();
			auto processMachineId = procRecord->locality.machineId();
			ASSERT(processMachineId.present());
			if (processDcId.present() && (processDcId == dcId)) {
				if ((kt != Reboot) && (protectedAddresses.count(procRecord->address))) {
					kt = Reboot;
					TraceEvent(SevWarn, "DcKillChanged").detail("DataCenter", dcId).detail("KillType", kt).detail("OrigKillType", ktOrig)
						.detail("Reason", "Datacenter has protected process").detail("ProcessAddress", procRecord->address).detail("Failed", procRecord->failed).detail("Rebooting", procRecord->rebooting).detail("Excluded", procRecord->excluded).detail("Cleared", procRecord->cleared).detail("Process", procRecord->toString());
				}
				datacenterMachines[processMachineId.get()] ++;
				dcProcesses ++;
			}
		}

		// Check if machine can be removed, if requested
		if (!forceKill && ((kt == KillInstantly) || (kt == InjectFaults) || (kt == RebootAndDelete) || (kt == RebootProcessAndDelete)))
		{
			std::vector<ProcessInfo*>	processesLeft, processesDead;
			for (auto processInfo : getAllProcesses()) {
				if (processInfo->isAvailableClass()) {
					if (processInfo->isExcluded() || processInfo->isCleared() || !processInfo->isAvailable()) {
						processesDead.push_back(processInfo);
					} else if (protectedAddresses.count(processInfo->address) || datacenterMachines.find(processInfo->locality.machineId()) == datacenterMachines.end()) {
						processesLeft.push_back(processInfo);
					} else {
						processesDead.push_back(processInfo);
					}
				}
			}

			if (!canKillProcesses(processesLeft, processesDead, kt, &kt)) {
				TraceEvent(SevWarn, "DcKillChanged").detail("DataCenter", dcId).detail("KillType", kt).detail("OrigKillType", ktOrig);
			}
			else {
				TraceEvent("DeadDataCenter").detail("DataCenter", dcId).detail("KillType", kt).detail("DcZones", datacenterMachines.size()).detail("DcProcesses", dcProcesses).detail("ProcessesDead", processesDead.size()).detail("ProcessesLeft", processesLeft.size()).detail("TLogPolicy", tLogPolicy->info()).detail("StoragePolicy", storagePolicy->info());
				for (auto process : processesLeft) {
					TraceEvent("DeadDcSurvivors").detail("MachineId", process->locality.machineId()).detail("KillType", kt).detail("ProcessesLeft", processesLeft.size()).detail("ProcessesDead", processesDead.size()).detail("SurvivingProcess", process->toString());
				}
				for (auto process : processesDead) {
					TraceEvent("DeadDcVictims").detail("MachineId", process->locality.machineId()).detail("KillType", kt).detail("ProcessesLeft", processesLeft.size()).detail("ProcessesDead", processesDead.size()).detail("VictimProcess", process->toString());
				}
			}
		}

		KillType	ktResult, ktMin = kt;
		for (auto& datacenterMachine : datacenterMachines) {
			if(deterministicRandom()->random01() < 0.99) {
				killMachine(datacenterMachine.first, kt, true, &ktResult);
				if (ktResult != kt) {
					TraceEvent(SevWarn, "KillDCFail")
						.detail("Zone", datacenterMachine.first)
						.detail("KillType", kt)
						.detail("KillTypeResult", ktResult)
						.detail("KillTypeOrig", ktOrig);
					ASSERT(ktResult == None);
				}
				ktMin = std::min<KillType>( ktResult, ktMin );
			}
		}

		TraceEvent("KillDataCenter")
			.detail("DcZones", datacenterMachines.size())
			.detail("DcProcesses", dcProcesses)
			.detail("DCID", dcId)
			.detail("KillType", kt)
			.detail("KillTypeOrig", ktOrig)
			.detail("KillTypeMin", ktMin)
			.detail("KilledDC", kt==ktMin);

		TEST(kt != ktMin); // DataCenter kill was rejected by killMachine
		TEST((kt==ktMin) && (kt == RebootAndDelete)); // Resulted in a reboot and delete
		TEST((kt==ktMin) && (kt == Reboot)); // Resulted in a reboot
		TEST((kt==ktMin) && (kt == KillInstantly)); // Resulted in an instant kill
		TEST((kt==ktMin) && (kt == InjectFaults));  // Resulted in a kill by injecting faults
		TEST((kt==ktMin) && (kt != ktOrig)); // Kill request was downgraded
		TEST((kt==ktMin) && (kt == ktOrig)); // Requested kill was done

		if (ktFinal) *ktFinal = ktMin;

		return (kt == ktMin);
	}
	virtual void clogInterface(const IPAddress& ip, double seconds, ClogMode mode = ClogDefault) {
		if (mode == ClogDefault) {
			double a = deterministicRandom()->random01();
			if ( a < 0.3 ) mode = ClogSend;
			else if (a < 0.6 ) mode = ClogReceive;
			else mode = ClogAll;
		}
		TraceEvent("ClogInterface")
		    .detail("IP", ip.toString())
		    .detail("Delay", seconds)
		    .detail("Queue", mode == ClogSend ? "Send" : mode == ClogReceive ? "Receive" : "All");

		if (mode == ClogSend || mode==ClogAll)
			g_clogging.clogSendFor( ip, seconds );
		if (mode == ClogReceive || mode==ClogAll)
			g_clogging.clogRecvFor( ip, seconds );
	}
	virtual void clogPair(const IPAddress& from, const IPAddress& to, double seconds) {
		g_clogging.clogPairFor( from, to, seconds );
	}
	virtual std::vector<ProcessInfo*> getAllProcesses() const {
		std::vector<ProcessInfo*> processes;
		for( auto& c : machines ) {
			processes.insert( processes.end(), c.second.processes.begin(), c.second.processes.end() );
		}
		for( auto& c : currentlyRebootingProcesses ) {
			processes.push_back( c.second );
		}
		return processes;
	}
	virtual ProcessInfo* getProcessByAddress( NetworkAddress const& address ) {
		NetworkAddress normalizedAddress(address.ip, address.port, true, address.isTLS());
		ASSERT( addressMap.count( normalizedAddress ) );
		return addressMap[ normalizedAddress ];
	}

	virtual MachineInfo* getMachineByNetworkAddress(NetworkAddress const& address) {
		return &machines[addressMap[address]->locality.machineId()];
	}

	virtual MachineInfo* getMachineById(Optional<Standalone<StringRef>> const& machineId) {
		return &machines[machineId];
	}

	virtual void destroyMachine(Optional<Standalone<StringRef>> const& machineId ) {
		auto& machine = machines[machineId];
		for( auto process : machine.processes ) {
			ASSERT( process->failed );
		}
		if( machine.machineProcess ) {
			 killProcess_internal( machine.machineProcess, KillInstantly );
		}
		machines.erase(machineId);
	}

	Sim2() : time(0.0), timerTime(0.0), taskCount(0), yielded(false), yield_limit(0), currentTaskID(TaskPriority::Zero) {
		// Not letting currentProcess be NULL eliminates some annoying special cases
		currentProcess = new ProcessInfo("NoMachine", LocalityData(Optional<Standalone<StringRef>>(), StringRef(), StringRef(), StringRef()), ProcessClass(), {NetworkAddress()}, this, "", "");
		g_network = net2 = newNet2(TLSConfig(), false, true);
		g_network->addStopCallback( Net2FileSystem::stop );
		Net2FileSystem::newFileSystem();
		check_yield(TaskPriority::Zero);
	}

	// Implementation
	struct Task {
		TaskPriority taskID;
		double time;
		uint64_t stable;
		ProcessInfo* machine;
		Promise<Void> action;
		Task( double time, TaskPriority taskID, uint64_t stable, ProcessInfo* machine, Promise<Void>&& action ) : time(time), taskID(taskID), stable(stable), machine(machine), action(std::move(action)) {}
		Task( double time, TaskPriority taskID, uint64_t stable, ProcessInfo* machine, Future<Void>& future ) : time(time), taskID(taskID), stable(stable), machine(machine) { future = action.getFuture(); }
		Task(Task&& rhs) BOOST_NOEXCEPT : time(rhs.time), taskID(rhs.taskID), stable(rhs.stable), machine(rhs.machine), action(std::move(rhs.action)) {}
		void operator= ( Task const& rhs ) { taskID = rhs.taskID; time = rhs.time; stable = rhs.stable; machine = rhs.machine; action = rhs.action; }
		Task( Task const& rhs ) : taskID(rhs.taskID), time(rhs.time), stable(rhs.stable), machine(rhs.machine), action(rhs.action) {}
		void operator= (Task&& rhs) BOOST_NOEXCEPT { time = rhs.time; taskID = rhs.taskID; stable = rhs.stable; machine = rhs.machine; action = std::move(rhs.action); }

		bool operator < (Task const& rhs) const {
			// Ordering is reversed for priority_queue
			if (time != rhs.time) return time > rhs.time;
			return stable > rhs.stable;
		}
	};

	void execTask(struct Task& t) {
		if (t.machine->failed) {
			t.action.send(Never());
		}
		else {
			mutex.enter();
			this->time = t.time;
			this->timerTime = std::max(this->timerTime, this->time);
			mutex.leave();

			this->currentProcess = t.machine;
			try {
				//auto before = getCPUTicks();
				t.action.send(Void());
				ASSERT( this->currentProcess == t.machine );
				/*auto elapsed = getCPUTicks() - before;
				currentProcess->cpuTicks += elapsed;
				if (deterministicRandom()->random01() < 0.01){
					TraceEvent("TaskDuration").detail("CpuTicks", currentProcess->cpuTicks);
					currentProcess->cpuTicks = 0;
				}*/
			} catch (Error& e) {
				TraceEvent(SevError, "UnhandledSimulationEventError").error(e, true);
				killProcess(t.machine, KillInstantly);
			}

			//if( this->time > 45.522817 ) {
			//	printf("foo\n");
			//}

			if (randLog)
				fprintf( randLog, "T %f %d %s %" PRId64 "\n", this->time, int(deterministicRandom()->peek() % 10000), t.machine ? t.machine->name : "none", t.stable);
		}
	}

	virtual void onMainThread( Promise<Void>&& signal, TaskPriority taskID ) {
		// This is presumably coming from either a "fake" thread pool thread, i.e. it is actually on this thread
		// or a thread created with g_network->startThread
		ASSERT(getCurrentProcess());

		mutex.enter();
		ASSERT(taskID >= TaskPriority::Min && taskID <= TaskPriority::Max);
		tasks.push( Task( time, taskID, taskCount++, getCurrentProcess(), std::move(signal) ) );
		mutex.leave();
	}
	bool isOnMainThread() const override {
		return net2->isOnMainThread();
	}
	virtual Future<Void> onProcess( ISimulator::ProcessInfo *process, TaskPriority taskID ) {
		return delay( 0, taskID, process );
	}
	virtual Future<Void> onMachine( ISimulator::ProcessInfo *process, TaskPriority taskID ) {
		if( process->machine == 0 )
			return Void();
		return delay( 0, taskID, process->machine->machineProcess );
	}

	//time is guarded by ISimulator::mutex. It is not necessary to guard reads on the main thread because
	//time should only be modified from the main thread.
	double time;
	double timerTime;
	TaskPriority currentTaskID;

	//taskCount is guarded by ISimulator::mutex
	uint64_t taskCount;

	std::map<Optional<Standalone<StringRef>>, MachineInfo > machines;
	std::map<NetworkAddress, ProcessInfo*> addressMap;
	std::map<ProcessInfo*, Promise<Void>> filesDeadMap;

	//tasks is guarded by ISimulator::mutex
	std::priority_queue<Task, std::vector<Task>> tasks;

	std::vector<std::function<void()>> stopCallbacks;

	//Sim2Net network;
	INetwork *net2;

	//Map from machine IP -> machine disk space info
	std::map<IPAddress, SimDiskSpace> diskSpaceMap;

	//Whether or not yield has returned true during the current iteration of the run loop
	bool yielded;
	int yield_limit;  // how many more times yield may return false before next returning true
};

void startNewSimulator() {
	ASSERT( !g_network );
	g_network = g_pSimulator = new Sim2();
	g_simulator.connectionFailuresDisableDuration = deterministicRandom()->random01() < 0.5 ? 0 : 1e6;
}

ACTOR void doReboot( ISimulator::ProcessInfo *p, ISimulator::KillType kt ) {
	TraceEvent("RebootingProcessAttempt").detail("ZoneId", p->locality.zoneId()).detail("KillType", kt).detail("Process", p->toString()).detail("StartingClass", p->startingClass.toString()).detail("Failed", p->failed).detail("Excluded", p->excluded).detail("Cleared", p->cleared).detail("Rebooting", p->rebooting).detail("TaskPriorityDefaultDelay", TaskPriority::DefaultDelay);

	wait( g_sim2.delay( 0, TaskPriority::DefaultDelay, p ) ); // Switch to the machine in question

	try {
		ASSERT( kt == ISimulator::RebootProcess || kt == ISimulator::Reboot || kt == ISimulator::RebootAndDelete || kt == ISimulator::RebootProcessAndDelete );

		TEST( kt == ISimulator::RebootProcess ); // Simulated process rebooted
		TEST( kt == ISimulator::Reboot ); // Simulated machine rebooted
		TEST( kt == ISimulator::RebootAndDelete ); // Simulated machine rebooted with data and coordination state deletion
		TEST( kt == ISimulator::RebootProcessAndDelete ); // Simulated process rebooted with data and coordination state deletion

		if( p->rebooting || !p->isReliable() )
			return;
		TraceEvent("RebootingProcess").detail("KillType", kt).detail("Address", p->address).detail("ZoneId", p->locality.zoneId()).detail("DataHall", p->locality.dataHallId()).detail("Locality", p->locality.toString()).detail("Failed", p->failed).detail("Excluded", p->excluded).detail("Cleared", p->cleared).backtrace();
		p->rebooting = true;
		if ((kt == ISimulator::RebootAndDelete) || (kt == ISimulator::RebootProcessAndDelete)) {
			p->cleared = true;
			g_simulator.clearAddress(p->address);
		}
		p->shutdownSignal.send( kt );
	} catch (Error& e) {
		TraceEvent(SevError, "RebootError").error(e);
		p->shutdownSignal.sendError(e);  // ?
		throw; // goes nowhere!
	}
}

//Simulates delays for performing operations on disk
Future<Void> waitUntilDiskReady( Reference<DiskParameters> diskParameters, int64_t size, bool sync ) {
	if(g_simulator.connectionFailuresDisableDuration > 1e4)
		return delay(0.0001);

	if( diskParameters->nextOperation < now() ) diskParameters->nextOperation = now();
	diskParameters->nextOperation += ( 1.0 / diskParameters->iops ) + ( size / diskParameters->bandwidth );

	double randomLatency;
	if(sync) {
		randomLatency = .005 + deterministicRandom()->random01() * (BUGGIFY ? 1.0 : .010);
	} else
		randomLatency = 10 * deterministicRandom()->random01() / diskParameters->iops;

	return delayUntil( diskParameters->nextOperation + randomLatency );
}

#if defined(_WIN32)

/* Opening with FILE_SHARE_DELETE lets simulation actually work on windows - previously renames were always failing.
   FIXME: Use an actual platform abstraction for this stuff!  Is there any reason we can't use underlying net2 for example? */

#include <Windows.h>

int sf_open( const char* filename, int flags, int convFlags, int mode ) {
	HANDLE wh = CreateFile( filename, GENERIC_READ | ((flags&IAsyncFile::OPEN_READWRITE) ? GENERIC_WRITE : 0),
		FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL,
		(flags&IAsyncFile::OPEN_EXCLUSIVE) ? CREATE_NEW :
			(flags&IAsyncFile::OPEN_CREATE) ? OPEN_ALWAYS :
			OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL );
	int h = -1;
	if (wh != INVALID_HANDLE_VALUE) h = _open_osfhandle( (intptr_t)wh, convFlags );
	else errno = GetLastError() == ERROR_FILE_NOT_FOUND ? ENOENT : EFAULT;
	return h;
}

#endif

// Opens a file for asynchronous I/O
Future< Reference<class IAsyncFile> > Sim2FileSystem::open( std::string filename, int64_t flags, int64_t mode )
{
	ASSERT( (flags & IAsyncFile::OPEN_ATOMIC_WRITE_AND_CREATE) ||
			!(flags & IAsyncFile::OPEN_CREATE) ||
			StringRef(filename).endsWith(LiteralStringRef(".fdb-lock")) );  // We don't use "ordinary" non-atomic file creation right now except for folder locking, and we don't have code to simulate its unsafeness.

	if ( (flags & IAsyncFile::OPEN_EXCLUSIVE) ) ASSERT( flags & IAsyncFile::OPEN_CREATE );

	if (flags & IAsyncFile::OPEN_UNCACHED) {
		auto& machineCache = g_simulator.getCurrentProcess()->machine->openFiles;
		std::string actualFilename = filename;
		if ( machineCache.find(filename) == machineCache.end() ) {
			if(flags & IAsyncFile::OPEN_ATOMIC_WRITE_AND_CREATE) {
				actualFilename = filename + ".part";
				auto partFile = machineCache.find(actualFilename);
				if(partFile != machineCache.end()) {
					Future<Reference<IAsyncFile>> f = AsyncFileDetachable::open(partFile->second);
					if(FLOW_KNOBS->PAGE_WRITE_CHECKSUM_HISTORY > 0)
						f = map(f, [=](Reference<IAsyncFile> r) { return Reference<IAsyncFile>(new AsyncFileWriteChecker(r)); });
					return f;
				}
			}
			//Simulated disk parameters are shared by the AsyncFileNonDurable and the underlying SimpleFile.  This way, they can both keep up with the time to start the next operation
			Reference<DiskParameters> diskParameters(new DiskParameters(FLOW_KNOBS->SIM_DISK_IOPS, FLOW_KNOBS->SIM_DISK_BANDWIDTH));
			machineCache[actualFilename] = AsyncFileNonDurable::open(filename, actualFilename, SimpleFile::open(filename, flags, mode, diskParameters, false), diskParameters);
		}
		Future<Reference<IAsyncFile>> f = AsyncFileDetachable::open( machineCache[actualFilename] );
		if(FLOW_KNOBS->PAGE_WRITE_CHECKSUM_HISTORY > 0)
			f = map(f, [=](Reference<IAsyncFile> r) { return Reference<IAsyncFile>(new AsyncFileWriteChecker(r)); });
		return f;
	}
	else
		return AsyncFileCached::open(filename, flags, mode);
}

// Deletes the given file.  If mustBeDurable, returns only when the file is guaranteed to be deleted even after a power failure.
Future< Void > Sim2FileSystem::deleteFile( std::string filename, bool mustBeDurable )
{
	return Sim2::deleteFileImpl(&g_sim2, filename, mustBeDurable);
}

Future< std::time_t > Sim2FileSystem::lastWriteTime( std::string filename ) {
	// TODO: update this map upon file writes.
	static std::map<std::string, double> fileWrites;
	if (BUGGIFY && deterministicRandom()->random01() < 0.01) {
		fileWrites[filename] = now();
	}
	return fileWrites[filename];
}

void Sim2FileSystem::newFileSystem()
{
	g_network->setGlobal(INetwork::enFileSystem, (flowGlobalType) new Sim2FileSystem());
}
