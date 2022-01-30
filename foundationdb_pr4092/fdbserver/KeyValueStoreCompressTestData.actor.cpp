/*
 * KeyValueStoreCompressTestData.actor.cpp
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

#include "fdbserver/IKeyValueStore.h"
#include "flow/actorcompiler.h" // has to be last include

// KeyValueStoreCompressTestData wraps an existing IKeyValueStore and
// implements the following rudimentary compression scheme:
//   An arbitrarily long value which consists entirely of a single repeated nonzero byte is mapped to
//     a 5-byte value consisting of that byte followed by a little-endian integer giving the number
//     of repetitions.
//   All other values are mapped to a zero byte followed by the value.
// This store is used in testing to let us simulate having much bigger disks than we actually
//   have, in order to test really big databases.

struct KeyValueStoreCompressTestData : IKeyValueStore {
	IKeyValueStore* store;

	KeyValueStoreCompressTestData(IKeyValueStore* store) : store(store) {}

	virtual Future<Void> getError() { return store->getError(); }
	virtual Future<Void> onClosed() { return store->onClosed(); }
	virtual void dispose() { store->dispose(); delete this; }
	virtual void close() { store->close(); delete this; }

	virtual KeyValueStoreType getType() { return store->getType(); }
	virtual StorageBytes getStorageBytes() { return store->getStorageBytes(); }

	virtual void set( KeyValueRef keyValue, const Arena* arena = NULL ) {
		store->set( KeyValueRef( keyValue.key, pack(keyValue.value) ), arena );
	}
	virtual void clear( KeyRangeRef range, const Arena* arena = NULL ) { store->clear( range, arena ); }
	virtual Future<Void> commit(bool sequential = false) { return store->commit(sequential); }

	virtual Future<Optional<Value>> readValue( KeyRef key, Optional<UID> debugID = Optional<UID>() ) {
		return doReadValue(store, key, debugID);
	}
	ACTOR static Future<Optional<Value>> doReadValue( IKeyValueStore* store, Key key, Optional<UID> debugID ) {
		Optional<Value> v = wait( store->readValue(key, debugID) );
		if (!v.present()) return v;
		return unpack(v.get());
	}

	// Note that readValuePrefix doesn't do anything in this implementation of IKeyValueStore, so the "atomic bomb" problem is still
	// present if you are using this storage interface, but this storage interface is not used by customers ever. However, if you want
	// to try to test malicious atomic op workloads with compressed values for some reason, you will need to fix this.
	virtual Future<Optional<Value>> readValuePrefix( KeyRef key, int maxLength, Optional<UID> debugID = Optional<UID>() ) {
		return doReadValuePrefix( store, key, maxLength, debugID );
	}
	ACTOR static Future<Optional<Value>> doReadValuePrefix( IKeyValueStore* store, Key key, int maxLength, Optional<UID> debugID ) {
		Optional<Value> v = wait( doReadValue(store, key, debugID) );
		if (!v.present()) return v;
		if (maxLength < v.get().size()) {
			return v.get().substr(0, maxLength);
		}
		else {
			return v;
		}
	}

	// If rowLimit>=0, reads first rows sorted ascending, otherwise reads last rows sorted descending
	// The total size of the returned value (less the last entry) will be less than byteLimit
	virtual Future<Standalone<RangeResultRef>> readRange( KeyRangeRef keys, int rowLimit = 1<<30, int byteLimit = 1<<30 ) {
		return doReadRange(store, keys, rowLimit, byteLimit);
	}
	ACTOR Future<Standalone<RangeResultRef>> doReadRange( IKeyValueStore* store, KeyRangeRef keys, int rowLimit, int byteLimit ) {
		Standalone<RangeResultRef> _vs = wait( store->readRange(keys, rowLimit, byteLimit) );
		Standalone<RangeResultRef> vs = _vs; // Get rid of implicit const& from wait statement
		Arena& a = vs.arena();
		for(int i=0; i<vs.size(); i++)
			vs[i].value = ValueRef( a, (ValueRef const&)unpack(vs[i].value) );
		return vs;
	}

private:
	// These implement the actual "compression" scheme
	static Value pack( Value val ) {
		if (!val.size()) return val;
		uint8_t c = val[0];

		//If the value starts with a 0-byte, then we don't compress it
		if(c == 0)
			return val.withPrefix(LiteralStringRef("\x00"));

		for(int i=1; i<val.size(); i++) {
			if (val[i] != c) {
				// The value is something other than a single repeated character, so not compressible :-)
				return val.withPrefix(LiteralStringRef("\x00"));
			}
		}

		int n = val.size();
		val = makeString(5);
		uint8_t *p = mutateString(val);
		p[0] = c;
		*(int*)(p+1) = n;
		return val;
	}
	static Value unpack( Value val ) {
		if (!val.size()) return val;
		if (val[0]==0) return val.substr(1);  // Uncompressed value
		ASSERT( val.size() == 5 );
		uint8_t c = val[0];
		int n = *(int*)(val.begin()+1);
		val = makeString(n);
		uint8_t *p = mutateString(val);
		memset(p, c, n);
		return val;
	}

};

IKeyValueStore* keyValueStoreCompressTestData(IKeyValueStore* store) {
	return new KeyValueStoreCompressTestData(store);
}
