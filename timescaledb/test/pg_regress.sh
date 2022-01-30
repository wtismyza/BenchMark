#!/usr/bin/env bash

# Wrapper around pg_regress and pg_isolation_regress to be able to control the schedule with environment variables
#
# The following control variables are supported:
#
# TESTS     only run tests from this list
# IGNORES   failure of tests in this list will not lead to test failure
# SKIPS     tests from this list are not run

CURRENT_DIR=$(dirname $0)
EXE_DIR=${EXE_DIR:-${CURRENT_DIR}}
PG_REGRESS=${PG_REGRESS:-pg_regress}
PG_REGRESS_DIFF_OPTS=-u
TEST_SCHEDULE=${TEST_SCHEDULE:-}
TEMP_SCHEDULE=${CURRENT_DIR}/temp_schedule
SCHEDULE=
TESTS=${TESTS:-}
IGNORES=${IGNORES:-}
SKIPS=${SKIPS:-}

contains() {
    # a list contains a value foo if the regex ".* foo .*" holds true
    [[ $1 =~ (.*[[:space:]]|^)$2([[:space:]].*|$) ]];
    return $?
}

if [[ -z ${TEST_SCHEDULE} ]];  then
  echo "No test schedule supplied please set TEST_SCHEDULE"
  exit 1;
fi

echo "TESTS ${TESTS}"
echo "IGNORES ${IGNORES}"
echo "SKIPS ${SKIPS}"

if [[ -z ${TESTS} ]] && [[ -z ${SKIPS} ]] && [[ -z ${IGNORES} ]]; then
  # no filter variables set
  # nothing to do here and we can use the cmake generated schedule

  SCHEDULE=${TEST_SCHEDULE}

elif [[ -z ${TESTS} ]] && [[ -z ${SKIPS} ]] && [[ -n ${IGNORES} ]]; then
  # if we only have IGNORES we can use the cmake created schedule
  # and just prepend ignore lines for the tests whose result should be
  # ignored. This will allow us to retain the parallel groupings from
  # the supplied schedule

  echo > ${TEMP_SCHEDULE}

  for t in ${IGNORES}; do
      echo "ignore: ${t}" >> ${TEMP_SCHEDULE}
  done
  cat ${TEST_SCHEDULE} >> ${TEMP_SCHEDULE}

  SCHEDULE=${TEMP_SCHEDULE}

else
  # either TESTS or SKIPS was specified so we need to create a new schedule
  # based on those

  ALL_TESTS=$(grep '^test: ' ${TEST_SCHEDULE} | sed -e 's!^test: !!' |tr '\n' ' ')

  if [[ -z "${TESTS}" ]]; then
    TESTS=${ALL_TESTS}
  fi

  # build new test list in current_tests removing entries in SKIPS and
  # validating against schedule as TESTS might contain tests from
  # multiple suites and not apply to current run
  current_tests=""
  for t in ${TESTS}; do
    if ! contains "${SKIPS}" "${t}"; then
      if contains "${ALL_TESTS}" "${t}"; then
        current_tests="${current_tests} ${t}"
      fi
    fi
  done

  # if none of the tests survived filtering we can exit early
  if [[ -z "${current_tests}" ]]; then
    exit 0
  fi

  current_tests=$(echo "${current_tests}" | tr ' ' '\n' | sort)

  TESTS=${current_tests}

  echo > ${TEMP_SCHEDULE}

  for t in ${IGNORES}; do
      echo "ignore: ${t}" >> ${TEMP_SCHEDULE}
  done

  for t in ${TESTS}; do
      echo "test: ${t}" >> ${TEMP_SCHEDULE}
  done

  SCHEDULE=${TEMP_SCHEDULE}

fi

function cleanup() {
  rm -rf ${EXE_DIR}/sql/dump
  rm -rf ${TEST_TABLESPACE1_PATH}
  rm -rf ${TEST_TABLESPACE2_PATH}
  rm -rf ${TEST_TABLESPACE3_PATH}
  rm -f ${TEMP_SCHEDULE}
  rm -rf ${TEST_OUTPUT_DIR}/.pg_init
}

trap cleanup EXIT

# This mktemp line will work on both OSX and GNU systems
TEST_TABLESPACE1_PATH=${TEST_TABLESPACE1_PATH:-$(mktemp -d 2>/dev/null || mktemp -d -t 'timescaledb_regress')}
TEST_TABLESPACE2_PATH=${TEST_TABLESPACE2_PATH:-$(mktemp -d 2>/dev/null || mktemp -d -t 'timescaledb_regress')}
TEST_TABLESPACE3_PATH=${TEST_TABLESPACE3_PATH:-$(mktemp -d 2>/dev/null || mktemp -d -t 'timescaledb_regress')}
export TEST_TABLESPACE1_PATH TEST_TABLESPACE2_PATH TEST_TABLESPACE3_PATH

rm -rf ${TEST_OUTPUT_DIR}/.pg_init
mkdir -p ${EXE_DIR}/sql/dump

export PG_REGRESS_DIFF_OPTS

PG_REGRESS_OPTS="${PG_REGRESS_OPTS}  --schedule=${SCHEDULE}"
${PG_REGRESS} $@ ${PG_REGRESS_OPTS}
