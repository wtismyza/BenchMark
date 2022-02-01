export
PLATFORM := $(shell uname)
ARCH := $(shell uname -m)
TOPDIR := $(shell pwd)

# Allow custom libc++ hack for Ubuntu
ifeq ("$(wildcard /etc/centos-release)", "")
  LIBSTDCPP_HACK ?= 1
endif

ifeq ($(ARCH),x86_64)
  ARCH := x64
else
  $(error Not prepared to compile on $(ARCH))
endif

MONO := $(shell which mono 2>/dev/null)
ifeq ($(MONO),)
  MONO := /usr/bin/mono
endif

MCS := $(shell which mcs 2>/dev/null)
ifeq ($(MCS),)
  MCS := $(shell which dmcs 2>/dev/null)
endif
ifeq ($(MCS),)
  MCS := /usr/bin/mcs
endif

CFLAGS := -Werror -Wno-error=format -fPIC -DNO_INTELLISENSE -fvisibility=hidden -DNDEBUG=1 -Wreturn-type -fno-omit-frame-pointer
ifeq ($(RELEASE),true)
	CFLAGS += -DFDB_CLEAN_BUILD
endif
ifeq ($(NIGHTLY),true)
	CFLAGS += -DFDB_CLEAN_BUILD
endif

BOOST_BASENAME ?= boost_1_67_0
ifeq ($(PLATFORM),Linux)
  PLATFORM := linux

  CC ?= gcc
  CXX ?= g++

  ifneq '' '$(findstring clang++,$(CXX))'
    CXXFLAGS += -Wno-undefined-var-template -Wno-unknown-warning-option -Wno-unused-command-line-argument
  endif

  CXXFLAGS += -std=c++17

  BOOST_BASEDIR ?= /opt
  TLS_LIBDIR ?= /usr/local/lib64
  DLEXT := so
  java_DLEXT := so
  TARGET_LIBC_VERSION ?= 2.11
else ifeq ($(PLATFORM),Darwin)
  PLATFORM := osx

  CC := /usr/bin/clang
  CXX := /usr/bin/clang

  CFLAGS += -mmacosx-version-min=10.14 -stdlib=libc++
  CXXFLAGS += -mmacosx-version-min=10.14 -std=c++17 -stdlib=libc++ -msse4.2 -Wno-undefined-var-template -Wno-unknown-warning-option

  .LIBPATTERNS := lib%.dylib lib%.a

  BOOST_BASEDIR ?= ${HOME}
  TLS_LIBDIR ?= /usr/local/lib64
  DLEXT := dylib
  java_DLEXT := jnilib
else
  $(error Not prepared to compile on platform $(PLATFORM))
endif
BOOSTDIR ?= ${BOOST_BASEDIR}/${BOOST_BASENAME}

CCACHE := $(shell which ccache 2>/dev/null)
ifneq ($(CCACHE),)
  CCACHE_CC := $(CCACHE) $(CC)
  CCACHE_CXX := $(CCACHE) $(CXX)
else
  CCACHE_CC := $(CC)
  CCACHE_CXX := $(CXX)
endif

# Default variables don't get pushed into the environment, but scripts in build/
# rely on the existence of CC in the environment.
ifeq ($(origin CC), default)
  CC := $(CC)
endif

ACTORCOMPILER := bin/actorcompiler.exe

# UNSTRIPPED := 1

# Normal optimization level
CFLAGS += -O2

# Or turn off optimization entirely
# CFLAGS += -O0

# Debugging symbols are a good thing (and harmless, since we keep them
# in external debug files)
CFLAGS += -g

# valgrind-compatibile builds are enabled by uncommenting lines in valgind.mk

# Define the TLS compilation and link variables
ifdef TLS_DISABLED
CFLAGS += -DTLS_DISABLED
FDB_TLS_LIB :=
TLS_LIBS :=
else
FDB_TLS_LIB :=
TLS_LIBS += $(addprefix $(TLS_LIBDIR)/,libssl.a libcrypto.a)
endif

CXXFLAGS += -Wno-deprecated -DBOOST_ERROR_CODE_HEADER_ONLY -DBOOST_SYSTEM_NO_DEPRECATED
LDFLAGS :=
LIBS :=
STATIC_LIBS :=

# Add library search paths (that aren't -Llib) to the VPATH
VPATH += $(addprefix :,$(filter-out lib,$(patsubst -L%,%,$(filter -L%,$(LDFLAGS)))))

CS_PROJECTS := flow/actorcompiler flow/coveragetool fdbclient/vexillographer
CPP_PROJECTS := flow fdbrpc fdbclient fdbbackup fdbserver fdbcli bindings/c bindings/java fdbmonitor bindings/flow/tester bindings/flow
OTHER_PROJECTS := bindings/python bindings/ruby bindings/go

CS_MK_GENERATED := $(CS_PROJECTS:=/generated.mk)
CPP_MK_GENERATED := $(CPP_PROJECTS:=/generated.mk)

MK_GENERATED := $(CS_MK_GENERATED) $(CPP_MK_GENERATED)

# build/valgrind.mk needs to be included before any _MK_GENERATED (which in turn includes local.mk)
MK_INCLUDE := build/scver.mk build/valgrind.mk $(CS_MK_GENERATED) $(CPP_MK_GENERATED) $(OTHER_PROJECTS:=/include.mk) build/packages.mk

ALL_MAKEFILES := Makefile $(MK_INCLUDE) $(patsubst %/generated.mk,%/local.mk,$(MK_GENERATED))

TARGETS =

.PHONY: clean all Makefiles

default: fdbserver fdbbackup fdbcli fdb_c fdb_python fdb_python_sdist

all: $(CS_PROJECTS) $(CPP_PROJECTS) $(OTHER_PROJECTS)

# These are always defined and ready to use. Any target that uses them and needs them up to date
#  should depend on versions.target
VERSION := $(shell cat versions.target | grep '<Version>' | sed -e 's,^[^>]*>,,' -e 's,<.*,,')
PACKAGE_NAME := $(shell cat versions.target | grep '<PackageName>' | sed -e 's,^[^>]*>,,' -e 's,<.*,,')

versions.h: Makefile versions.target
	@rm -f $@
ifeq ($(RELEASE),true)
	@echo "#define FDB_VT_VERSION \"$(VERSION)\"" >> $@
else
	@echo "#define FDB_VT_VERSION \"$(VERSION)-PRERELEASE\"" >> $@
endif
	@echo "#define FDB_VT_PACKAGE_NAME \"$(PACKAGE_NAME)\"" >> $@

bindings: fdb_c fdb_python fdb_ruby fdb_java fdb_flow fdb_flow_tester fdb_go fdb_go_tester fdb_c_tests

Makefiles: $(MK_GENERATED)

$(CS_MK_GENERATED): build/csprojtom4.py build/csproj.mk Makefile
	@echo "Creating       $@"
	@python build/csprojtom4.py $(@D)/*.csproj | m4 -DGENDIR="$(@D)" -DGENNAME=`basename $(@D)/*.csproj .csproj` - build/csproj.mk > $(@D)/generated.mk

$(CPP_MK_GENERATED): build/vcxprojtom4.py build/vcxproj.mk Makefile
	@echo "Creating       $@"
	@python build/vcxprojtom4.py $(@D)/*.vcxproj | m4 -DGENDIR="$(@D)" -DGENNAME=`basename $(@D)/*.vcxproj .vcxproj` - build/vcxproj.mk > $(@D)/generated.mk

DEPSDIR := .deps
OBJDIR := .objs
CMDDIR := .cmds

COMPILE_COMMANDS_JSONS := $(addprefix $(CMDDIR)/,$(addsuffix /compile_commands.json,${CPP_PROJECTS}))
compile_commands.json: build/concatinate_jsons.py ${COMPILE_COMMANDS_JSONS}
	@build/concatinate_jsons.py ${COMPILE_COMMANDS_JSONS}

include $(MK_INCLUDE)

clean: $(CLEAN_TARGETS) docpreview_clean
	@echo "Cleaning       toplevel"
	@rm -rf $(OBJDIR)
	@rm -rf $(DEPSDIR)
	@rm -rf lib/
	@rm -rf bin/coverage.*.xml
	@rm -rf $(CMDDIR) compile_commands.json
	@find . -name "*.g.cpp" -exec rm -f {} \; -or -name "*.g.h" -exec rm -f {} \;

targets:
	@echo "Available targets:"
	@for i in $(sort $(TARGETS)); do echo "  $$i" ; done
	@echo "Append _clean to clean specific target."

lib/libstdc++.a: $(shell $(CC) -print-file-name=libstdc++_pic.a)
	@echo "Frobnicating   $@"
	@mkdir -p lib
	@rm -rf .libstdc++
	@mkdir .libstdc++
	@(cd .libstdc++ && ar x $<)
	@for i in .libstdc++/*.o ; do \
		nm $$i | grep -q \@ || continue ; \
		nm $$i | awk '$$3 ~ /@@/ { COPY = $$3; sub(/@@.*/, "", COPY); print $$3, COPY; }' > .libstdc++/replacements ; \
		objcopy --redefine-syms=.libstdc++/replacements $$i $$i.new && mv $$i.new $$i ; \
		rm .libstdc++/replacements ; \
		nm $$i | awk '$$3 ~ /@/ { print $$3; }' > .libstdc++/deletes ; \
		objcopy --strip-symbols=.libstdc++/deletes $$i $$i.new && mv $$i.new $$i ; \
		rm .libstdc++/deletes ; \
	done
	@ar rcs $@ .libstdc++/*.o
	@rm -r .libstdc++


docpreview: javadoc
	@echo "Generating     docpreview"
	@TARGETS= $(MAKE) -C documentation docpreview

docpreview_clean:
	@echo "Cleaning       docpreview"
	@CLEAN_TARGETS= $(MAKE) -C documentation -s --no-print-directory docpreview_clean

packages/foundationdb-docs-$(VERSION).tar.gz: FORCE javadoc
	@echo "Packaging      documentation"
	@TARGETS= $(MAKE) -C documentation docpackage
	@mkdir -p packages
	@rm -f packages/foundationdb-docs-$(VERSION).tar.gz
	@cp documentation/sphinx/.dist/foundationdb-docs-$(VERSION).tar.gz packages/foundationdb-docs-$(VERSION).tar.gz

docpackage: packages/foundationdb-docs-$(VERSION).tar.gz

FORCE:

.SECONDEXPANSION:

bin/coverage.%.xml: bin/coveragetool.exe $$(%_ALL_SOURCES)
	@echo "Creating       $@"
	@$(MONO) bin/coveragetool.exe $@ $(filter-out $<,$^) >/dev/null

$(CPP_MK_GENERATED): $$(@D)/*.vcxproj

$(CS_MK_GENERATED): $$(@D)/*.csproj
