/*
 * Trace.h
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

#ifndef FLOW_TRACE_H
#define FLOW_TRACE_H
#pragma once

#include <atomic>
#include <stdarg.h>
#include <stdint.h>
#include <string>
#include <map>
#include <set>
#include <type_traits>
#include "flow/IRandom.h"
#include "flow/Error.h"

#define TRACE_DEFAULT_ROLL_SIZE (10 << 20)
#define TRACE_DEFAULT_MAX_LOGS_SIZE (10 * TRACE_DEFAULT_ROLL_SIZE)

inline int fastrand() {
	static int g_seed = 0;
	g_seed = 214013*g_seed + 2531011;
	return (g_seed>>16)&0x7fff;
}

//inline static bool TRACE_SAMPLE() { return fastrand()<16; }
inline static bool TRACE_SAMPLE() { return false; }

extern thread_local int g_allocation_tracing_disabled;

enum Severity {
	SevVerbose = 0,
	SevSample = 1,
	SevDebug = 5,
	SevInfo = 10,
	SevWarn = 20,
	SevWarnAlways = 30,
	SevError = 40,
	SevMaxUsed = SevError,
	SevMax = 1000000
};

class TraceEventFields {
public:
	constexpr static FileIdentifier file_identifier = 11262274;
	typedef std::pair<std::string, std::string> Field;
	typedef std::vector<Field> FieldContainer;
	typedef FieldContainer::const_iterator FieldIterator;

	TraceEventFields();

	size_t size() const;
	size_t sizeBytes() const;
	FieldIterator begin() const;
	FieldIterator end() const;
	bool isAnnotated() const;
	void setAnnotated();

	void addField(const std::string& key, const std::string& value);
	void addField(std::string&& key, std::string&& value);

	const Field &operator[] (int index) const;
	bool tryGetValue(std::string key, std::string &outValue) const;
	std::string getValue(std::string key) const;
	int getInt(std::string key, bool permissive=false) const;
	int64_t getInt64(std::string key, bool permissive=false) const;
	double getDouble(std::string key, bool permissive=false) const;

	Field &mutate(int index);

	std::string toString() const;
	void validateFormat() const;
	template<class Archiver>
	void serialize(Archiver& ar) {
		static_assert(is_fb_function<Archiver>, "Streaming serializer has to use load/save");
		serializer(ar, fields);
	}

private:
	FieldContainer fields;
	size_t bytes;
	bool annotated;
};

template <class Archive>
inline void load( Archive& ar, TraceEventFields& value ) {
	uint32_t count;
	ar >> count;

	std::string k;
	std::string v;
	for(uint32_t i = 0; i < count; ++i) {
		ar >> k >> v;
		value.addField(k, v);
	}
}
template <class Archive>
inline void save( Archive& ar, const TraceEventFields& value ) {
	ar << (uint32_t)value.size();

	for(auto itr : value) {
		ar << itr.first << itr.second;
	}
}

class TraceBatch {
public:
	void addEvent( const char *name, uint64_t id, const char *location );
	void addAttach( const char *name, uint64_t id, uint64_t to );
	void addBuggify( int activated, int line, std::string file );
	void dump();

private:
	struct EventInfo {
		TraceEventFields fields;
		EventInfo(double time, const char *name, uint64_t id, const char *location); 
	};

	struct AttachInfo {
		TraceEventFields fields;
		AttachInfo(double time, const char *name, uint64_t id, uint64_t to);
	};

	struct BuggifyInfo {
		TraceEventFields fields;
		BuggifyInfo(double time, int activated, int line, std::string file);
	};

	std::vector<EventInfo> eventBatch;
	std::vector<AttachInfo> attachBatch;
	std::vector<BuggifyInfo> buggifyBatch;
	static bool dumpImmediately();
};

struct DynamicEventMetric;

template<class IntType>
char base16Char(IntType c) {
	switch ((c % 16 + 16) % 16) {
		case 0:
			return '0';
		case 1:
			return '1';
		case 2:
			return '2';
		case 3:
			return '3';
		case 4:
			return '4';
		case 5:
			return '5';
		case 6:
			return '6';
		case 7:
			return '7';
		case 8:
			return '8';
		case 9:
			return '9';
		case 10:
			return 'a';
		case 11:
			return 'b';
		case 12:
			return 'c';
		case 13:
			return 'd';
		case 14:
			return 'e';
		case 15:
			return 'f';
		default:
			UNSTOPPABLE_ASSERT(false);
	}
}

// forward declare format from flow.h as we
// can't include flow.h here
std::string format(const char* form, ...);

template<class T>
struct Traceable : std::false_type {};

#define FORMAT_TRACEABLE(type, fmt)					\
	template<>										\
	struct Traceable<type> : std::true_type {		\
		static std::string toString(type value) {	\
			return format(fmt, value);				\
		}											\
	}

FORMAT_TRACEABLE(bool, "%d");
FORMAT_TRACEABLE(signed char, "%d");
FORMAT_TRACEABLE(unsigned char, "%d");
FORMAT_TRACEABLE(short, "%d");
FORMAT_TRACEABLE(unsigned short, "%d");
FORMAT_TRACEABLE(int, "%d");
FORMAT_TRACEABLE(unsigned, "%u");
FORMAT_TRACEABLE(long int, "%ld");
FORMAT_TRACEABLE(unsigned long int, "%lu");
FORMAT_TRACEABLE(long long int, "%lld");
FORMAT_TRACEABLE(unsigned long long int, "%llu");
FORMAT_TRACEABLE(double, "%g");
FORMAT_TRACEABLE(void*, "%p");
FORMAT_TRACEABLE(volatile long, "%ld");
FORMAT_TRACEABLE(volatile unsigned long, "%lu");
FORMAT_TRACEABLE(volatile long long, "%lld");
FORMAT_TRACEABLE(volatile unsigned long long, "%llu");
FORMAT_TRACEABLE(volatile double, "%g");


template<>
struct Traceable<UID> : std::true_type {
	static std::string toString(const UID& value) {
		return format("%016llx", value.first());
	}
};

template<class Str>
struct TraceableString {
	static auto begin(const Str& value) -> decltype(value.begin()) {
		return value.begin();
	}

	static bool atEnd(const Str& value, decltype(value.begin()) iter) {
		return iter == value.end();
	}

	static std::string toString(const Str& value) {
		return value.toString();
	}
};

template<>
struct TraceableString<std::string> {
	static auto begin(const std::string& value) -> decltype(value.begin()) {
		return value.begin();
	}

	static bool atEnd(const std::string& value, decltype(value.begin()) iter) {
		return iter == value.end();
	}

	template<class S>
	static std::string toString(S&& value) {
		return std::forward<S>(value);
	}
};

template<>
struct TraceableString<const char*> {
	static const char* begin(const char* value) {
		return value;
	}

	static bool atEnd(const char* value, const char* iter) {
		return *iter == '\0';
	}

	static std::string toString(const char* value) {
		return std::string(value);
	}
};

std::string traceableStringToString(const char* value, size_t S);

template<size_t S>
struct TraceableString<char[S]> {
	static_assert(S > 0, "Only string literals are supported.");
	static const char* begin(const char* value) {
		return value;
	}

	static bool atEnd(const char* value, const char* iter) {
		return iter - value == S - 1; // Exclude trailing \0 byte
	}

	static std::string toString(const char* value) { return traceableStringToString(value, S); }
};

template<>
struct TraceableString<char*> {
	static const char* begin(char* value) {
		return value;
	}

	static bool atEnd(char* value, const char* iter) {
		return *iter == '\0';
	}

	static std::string toString(char* value) {
		return std::string(value);
	}
};

template<class T>
struct TraceableStringImpl : std::true_type {
	static constexpr bool isPrintable(char c) { return 32 <= c && c <= 126; }

	template<class Str>
	static std::string toString(Str&& value) {
		// if all characters are printable ascii, we simply return the string
		int nonPrintables = 0;
		int numBackslashes = 0;
		int size = 0;
		for (auto iter = TraceableString<T>::begin(value); !TraceableString<T>::atEnd(value, iter); ++iter) {
			++size;
			if (!isPrintable(char(*iter))) {
				++nonPrintables;
			} else if (*iter == '\\') {
				++numBackslashes;
			}
		}
		if (nonPrintables == 0 && numBackslashes == 0) {
			return TraceableString<T>::toString(std::forward<Str>(value));
		}
		std::string result;
		result.reserve(size - nonPrintables + (nonPrintables * 4) + numBackslashes);
		for (auto iter = TraceableString<T>::begin(value); !TraceableString<T>::atEnd(value, iter); ++iter) {
			if (isPrintable(*iter)) {
				result.push_back(*iter);
			} else if (*iter == '\\') {
				result.push_back('\\');
				result.push_back('\\');
			} else {
				const uint8_t byte = *iter;
				result.push_back('\\');
				result.push_back('x');
				result.push_back(base16Char(byte / 16));
				result.push_back(base16Char(byte));
			}
		}
		return result;
	}
};

template<>
struct Traceable<const char*> : TraceableStringImpl<const char*> {};
template<>
struct Traceable<char*> : TraceableStringImpl<char*> {};
template<size_t S>
struct Traceable<char[S]> : TraceableStringImpl<char[S]> {};
template<>
struct Traceable<std::string> : TraceableStringImpl<std::string> {};

template<class T>
struct SpecialTraceMetricType
	: std::conditional<std::is_integral<T>::value || std::is_enum<T>::value,
					   std::true_type, std::false_type>::type {
	static int64_t getValue(T v) {
		return v;
	}
};

#define TRACE_METRIC_TYPE(from, to)							\
	template<>												\
	struct SpecialTraceMetricType<from> : std::true_type {	\
		static to getValue(from v) {						\
			return v;										\
		}													\
	}

TRACE_METRIC_TYPE(double, double);

struct TraceEvent {
	TraceEvent();
	TraceEvent( const char* type, UID id = UID() );   // Assumes SevInfo severity
	TraceEvent( Severity, const char* type, UID id = UID() );
	TraceEvent( struct TraceInterval&, UID id = UID() );
	TraceEvent( Severity severity, struct TraceInterval& interval, UID id = UID() );

	TraceEvent( TraceEvent &&ev );
	TraceEvent& operator=( TraceEvent &&ev );

	static void setNetworkThread();
	static bool isNetworkThread();

	static double getCurrentTime();

	//Must be called directly after constructing the trace event
	TraceEvent& error(const class Error& e, bool includeCancelled=false) {
		if (enabled) {
			return errorImpl(e, includeCancelled);
		}
		return *this;
	}

	template<class T>
	typename std::enable_if<Traceable<T>::value, TraceEvent&>::type
	detail( std::string&& key, const T& value ) {
		if (enabled && init()) {
			auto s = Traceable<T>::toString(value);
			addMetric(key.c_str(), value, s);
			return detailImpl(std::move(key), std::move(s), false);
		}
		return *this;
	}

	template<class T>
	typename std::enable_if<Traceable<T>::value, TraceEvent&>::type
	detail( const char* key, const T& value ) {
		if (enabled && init()) {
			auto s = Traceable<T>::toString(value);
			addMetric(key, value, s);
			return detailImpl(std::string(key), std::move(s), false);
		}
		return *this;
	}
	template<class T>
	typename std::enable_if<std::is_enum<T>::value, TraceEvent&>::type
	detail(const char* key, T value) {
		if (enabled && init()) {
			setField(key, int64_t(value));
			return detailImpl(std::string(key), format("%d", value), false);
		}
		return *this;
	}
	TraceEvent& detailf( std::string key, const char* valueFormat, ... );
private:
	template<class T>
	typename std::enable_if<SpecialTraceMetricType<T>::value, void>::type
	addMetric(const char* key, const T& value, const std::string&) {
		setField(key, SpecialTraceMetricType<T>::getValue(value));
	}

	template<class T>
	typename std::enable_if<!SpecialTraceMetricType<T>::value, void>::type
	addMetric(const char* key, const T&, const std::string& value) {
		setField(key, value);
	}

	void setField(const char* key, int64_t value);
	void setField(const char* key, double value);
	void setField(const char* key, const std::string& value);

	TraceEvent& errorImpl(const class Error& e, bool includeCancelled=false);
	// Private version of detailf that does NOT write to the eventMetric.  This is to be used by other detail methods
	// which can write field metrics of a more appropriate type than string but use detailf() to add to the TraceEvent.
	TraceEvent& detailfNoMetric( std::string&& key, const char* valueFormat, ... );
	TraceEvent& detailImpl( std::string&& key, std::string&& value, bool writeEventMetricField=true );
public:
	TraceEvent& backtrace(const std::string& prefix = "");
	TraceEvent& trackLatest(const std::string& trackingKey );
	TraceEvent& sample( double sampleRate, bool logSampleRate=true );

	// Sets the maximum length a field can be before it gets truncated. A value of 0 uses the default, a negative value
	// disables truncation. This should be called before the field whose length you want to change, and it can be
	// changed multiple times in a single event.
	TraceEvent& setMaxFieldLength(int maxFieldLength);

	// Sets the maximum event length before the event gets suppressed and a warning is logged. A value of 0 uses the default,
	// a negative value disables length suppression. This should be called before adding details.
	TraceEvent& setMaxEventLength(int maxEventLength);

	//Cannot call other functions which could disable the trace event afterwords
	TraceEvent& suppressFor( double duration, bool logSuppressedEventCount=true );

	TraceEvent& GetLastError();

	bool isEnabled() const {
		return enabled;
	}

	void log();

	~TraceEvent();  // Actually logs the event

	// Return the number of invocations of TraceEvent() at the specified logging level.
	static unsigned long CountEventsLoggedAt(Severity);

	DynamicEventMetric *tmpEventMetric;  // This just just a place to store fields

private:
	bool initialized;
	bool enabled;
	bool logged;
	std::string trackingKey;
	TraceEventFields fields;
	Severity severity;
	const char *type;
	UID id;
	Error err;

	int maxFieldLength;
	int maxEventLength;
	int timeIndex;

	void setSizeLimits();

	static unsigned long eventCounts[5];
	static thread_local bool networkThread;

	bool init();
	bool init( struct TraceInterval& );
};

struct ITraceLogWriter {
	virtual void open() = 0;
	virtual void roll() = 0;
	virtual void close() = 0;
	virtual void write(const std::string&) = 0;
	virtual void sync() = 0;

	virtual void addref() = 0;
	virtual void delref() = 0;
};

struct ITraceLogFormatter {
	virtual const char* getExtension() = 0;
	virtual const char* getHeader() = 0; // Called when starting a new file
	virtual const char* getFooter() = 0; // Called when ending a file
	virtual std::string formatEvent(const TraceEventFields&) = 0; // Called for each event

	virtual void addref() = 0;
	virtual void delref() = 0;
};

struct ITraceLogIssuesReporter {
	virtual void addIssue(std::string issue) = 0;
	virtual void resolveIssue(std::string issue) = 0;

	virtual void retrieveIssues(std::set<std::string>& out) = 0;

	virtual void addref() = 0;
	virtual void delref() = 0;
};

struct TraceInterval {
	TraceInterval( const char* type ) : count(-1), type(type), severity(SevInfo) {}

	TraceInterval& begin();
	TraceInterval& end() { return *this; }

	const char* type;
	UID pairID;
	int count;
	Severity severity;
};

struct LatestEventCache {
public:
	void set( std::string tag, const TraceEventFields& fields );
	TraceEventFields get( std::string tag );
	std::vector<TraceEventFields> getAll();
	std::vector<TraceEventFields> getAllUnsafe();

	void clear( std::string prefix );
	void clear();

	// Latest error tracking only tracks errors when called from the main thread. Other errors are silently ignored.
	void setLatestError( const TraceEventFields& contents );
	TraceEventFields getLatestError();
private:
	std::map<struct NetworkAddress, std::map<std::string, TraceEventFields>> latest;
	std::map<struct NetworkAddress, TraceEventFields> latestErrors;
};

extern LatestEventCache latestEventCache;

struct EventCacheHolder : public ReferenceCounted<EventCacheHolder> {
	std::string trackingKey;

	EventCacheHolder(const std::string& trackingKey) : trackingKey(trackingKey) {}

	~EventCacheHolder() {
		latestEventCache.clear(trackingKey);
	}
};

// Evil but potentially useful for verbose messages:
#if CENABLED(0, NOT_IN_CLEAN)
#define TRACE( t, m ) if (TraceEvent::isEnabled(t)) TraceEvent(t,m)
#endif

struct NetworkAddress;
void openTraceFile(const NetworkAddress& na, uint64_t rollsize, uint64_t maxLogsSize, std::string directory = ".",
                   std::string baseOfBase = "trace", std::string logGroup = "default", std::string identifier = "");
void initTraceEventMetrics();
void closeTraceFile();
bool traceFileIsOpen();
void flushTraceFileVoid();

// Changes the format of trace files. Returns false if the format is unrecognized. No longer safe to call after a call
// to openTraceFile.
bool selectTraceFormatter(std::string format);
// Returns true iff format is recognized.
bool validateTraceFormat(std::string format);

// Select the clock source for trace files. Returns false if the format is unrecognized. No longer safe to call after a call
// to openTraceFile.
bool selectTraceClockSource(std::string source);
// Returns true iff source is recognized.
bool validateTraceClockSource(std::string source);

void addTraceRole(std::string role);
void removeTraceRole(std::string role);
void retriveTraceLogIssues(std::set<std::string>& out);
void setTraceLogGroup(const std::string& role);
template <class T>
struct Future;
struct Void;
Future<Void> pingTraceLogWriterThread();

enum trace_clock_t { TRACE_CLOCK_NOW, TRACE_CLOCK_REALTIME };
extern std::atomic<trace_clock_t> g_trace_clock;
extern TraceBatch g_traceBatch;

#define DUMPTOKEN(name)                                                                                                \
	TraceEvent("DumpToken", recruited.id()).detail("Name", #name).detail("Token", name.getEndpoint().token)

#endif
