#
# local.mk
#
# This source file is part of the FoundationDB open source project
#
# Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# -*- mode: makefile; -*-

fdbbackup_CFLAGS := $(fdbclient_CFLAGS)
fdbbackup_LDFLAGS := $(fdbrpc_LDFLAGS)
fdbbackup_LIBS := lib/libfdbclient.a lib/libfdbrpc.a lib/libflow.a $(FDB_TLS_LIB)
fdbbackup_STATIC_LIBS := $(TLS_LIBS)

ifeq ($(PLATFORM),linux)
  fdbbackup_LDFLAGS += -static-libstdc++ -static-libgcc -ldl -lpthread -lrt

  # GPerfTools profiler (uncomment to use)
  # fdbbackup_CFLAGS += -I/opt/gperftools/include -DUSE_GPERFTOOLS=1 -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free
  # fdbbackup_LDFLAGS += -L/opt/gperftools/lib
  # fdbbackup_STATIC_LIBS += -ltcmalloc -lunwind -lprofiler
else ifeq ($(PLATFORM),osx)
  fdbbackup_LDFLAGS += -lc++
endif

fdbbackup_GENERATED_SOURCES += versions.h

#ifeq ($(WORKLOADS),false)
#  fdbbackup_ALL_SOURCES := $(filter-out fdbbackup/workloads/%,$(fdbbackup_ALL_SOURCES))
#  fdbbackup_BUILD_SOURCES := $(filter-out fdbbackup/workloads/%,$(fdbbackup_BUILD_SOURCES))
#endif

bin/fdbbackup: bin/coverage.fdbbackup.xml

bin/fdbbackup.debug: bin/fdbbackup

BACKUP_ALIASES = fdbrestore fdbdr dr_agent backup_agent

$(addprefix bin/, $(BACKUP_ALIASES)): bin/fdbbackup
	@[ -f $@ ] || (echo "SymLinking     $@" && ln -s fdbbackup $@)

$(addprefix bin/, $(addsuffix .debug, $(BACKUP_ALIASES))): bin/fdbbackup.debug
	@[ -f $@ ] || (echo "SymLinking     $@" && ln -s fdbbackup.debug $@)

FORCE:
