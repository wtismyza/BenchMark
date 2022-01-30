.. _release-notes:

#############
Release Notes
#############

6.3.9
=====

Features
--------

* Added the ability to set arbitrary tags on transactions. Tags can be specifically throttled using ``fdbcli``, and certain types of tags can be automatically throttled by ratekeeper. `(PR #2942) <https://github.com/apple/foundationdb/pull/2942>`_
* Add an option for transactions to report conflicting keys by calling ``getRange`` with the special key prefix ``\xff\xff/transaction/conflicting_keys/``. `(PR 2257) <https://github.com/apple/foundationdb/pull/2257>`_
* Added the ``exclude failed`` command to ``fdbcli``. This command designates that a process is dead and will never come back, so the transaction logs can forget about mutations sent to that process. `(PR #1955) <https://github.com/apple/foundationdb/pull/1955>`_
* A new fast restore system that can restore a database to a point in time from backup files. It is a Spark-like parallel processing framework that processes backup data asynchronously, in parallel and in pipeline. `(Fast Restore Project) <https://github.com/apple/foundationdb/projects/7>`_
* Added backup workers for pulling mutations from transaction logs and uploading them to blob storage. Switching from the previous backup implementation will double a cluster's maximum write bandwidth. `(PR #1625) <https://github.com/apple/foundationdb/pull/1625>`_ `(PR #2588) <https://github.com/apple/foundationdb/pull/2588>`_ `(PR #2642) <https://github.com/apple/foundationdb/pull/2642>`_ 
* Added a new API in all bindings that can be used to query the estimated byte size of a given range. `(PR #2537) <https://github.com/apple/foundationdb/pull/2537>`_
* Added the ``lock`` and ``unlock`` commands to ``fdbcli`` which lock or unlock a cluster. `(PR #2890) <https://github.com/apple/foundationdb/pull/2890>`_
* Add a framework which helps to add client functions using special keys (keys within ``[\xff\xff, \xff\xff\xff)``). `(PR #2662) <https://github.com/apple/foundationdb/pull/2662>`_
* Added capability of aborting replication to a clone of DR site without affecting replication to the original dr site with ``--dstonly`` option of ``fdbdr abort``. `(PR 3457) <https://github.com/apple/foundationdb/pull/3457>`_

Performance
-----------

* Improved the client's load balancing algorithm so that each proxy processes an equal number of requests. `(PR #2520) <https://github.com/apple/foundationdb/pull/2520>`_
* Significantly reduced the amount of work done on the cluster controller by removing the centralized failure monitoring. `(PR #2518) <https://github.com/apple/foundationdb/pull/2518>`_
* Improved master recovery speeds by more efficiently broadcasting the recovery state between processes.  `(PR #2941) <https://github.com/apple/foundationdb/pull/2941>`_
* Significantly reduced the number of network connections opened to the coordinators. `(PR #3069) <https://github.com/apple/foundationdb/pull/3069>`_
* Improve GRV tail latencies, particularly as the transaction rate gets nearer the ratekeeper limit. `(PR #2735) <https://github.com/apple/foundationdb/pull/2735>`_
* The proxies are now more responsive to changes in workload when unthrottling lower priority transactions. `(PR #2735) <https://github.com/apple/foundationdb/pull/2735>`_
* Removed a lot of unnecessary copying across the codebase. `(PR #2986) <https://github.com/apple/foundationdb/pull/2986>`_ `(PR #2915) <https://github.com/apple/foundationdb/pull/2915>`_ `(PR #3024) <https://github.com/apple/foundationdb/pull/3024>`_ `(PR #2999) <https://github.com/apple/foundationdb/pull/2999>`_
* Optimized the performance of the storage server. `(PR #1988) <https://github.com/apple/foundationdb/pull/1988>`_ `(PR #3103) <https://github.com/apple/foundationdb/pull/3103>`_
* Optimized the performance of the resolver. `(PR #2648) <https://github.com/apple/foundationdb/pull/2648>`_ 
* Replaced most uses of hashlittle2 with crc32 for better performance.  `(PR #2538) <https://github.com/apple/foundationdb/pull/2538>`_
* Significantly reduced the serialized size of conflict ranges and single key clears. `(PR #2513) <https://github.com/apple/foundationdb/pull/2513>`_
* Improved range read performance when the reads overlap recently cleared key ranges. `(PR #2028) <https://github.com/apple/foundationdb/pull/2028>`_
* Reduced the number of comparisons used by various map implementations. `(PR #2882) <https://github.com/apple/foundationdb/pull/2882>`_
* Reduced the serialized size of empty strings. `(PR #3063) <https://github.com/apple/foundationdb/pull/3063>`_
* Reduced the serialized size of various interfaces by 10x. `(PR #3068) <https://github.com/apple/foundationdb/pull/3068>`_
* TLS handshakes can now be done in a background thread pool. `(PR #3403) <https://github.com/apple/foundationdb/pull/3403>`_

Reliability
-----------

* Connections that disconnect frequently are not immediately marked available. `(PR #2932) <https://github.com/apple/foundationdb/pull/2932>`_
* The data distributor will consider storage servers that are continually lagging behind as if they were failed. `(PR #2917) <https://github.com/apple/foundationdb/pull/2917>`_
* Changing the storage engine type of a cluster will no longer cause the cluster to run out of memory. Instead, the cluster will gracefully migrate storage server processes to the new storage engine one by one. `(PR #1985) <https://github.com/apple/foundationdb/pull/1985>`_
* Batch priority transactions which are being throttled by ratekeeper will get a ``batch_transaction_throttled`` error instead of hanging indefinitely.  `(PR #1868) <https://github.com/apple/foundationdb/pull/1868>`_
* Avoid using too much memory on the transaction logs when multiple types of transaction logs exist in the same process. `(PR #2213) <https://github.com/apple/foundationdb/pull/2213>`_

Fixes
-----

* The ``SetVersionstampedKey`` atomic operation no longer conflicts with versions smaller than the current read version of the transaction. `(PR #2557) <https://github.com/apple/foundationdb/pull/2557>`_
* Ratekeeper would measure durability lag a few seconds higher than reality. `(PR #2499) <https://github.com/apple/foundationdb/pull/2499>`_
* In very rare scenarios, the data distributor process could get stuck in an infinite loop. `(PR #2228) <https://github.com/apple/foundationdb/pull/2228>`_
* If the number of configured transaction logs were reduced at the exact same time a change to the system keyspace took place, it was possible for the transaction state store to become corrupted. `(PR #3051) <https://github.com/apple/foundationdb/pull/3051>`_
* Fix multiple data races between threads on the client. `(PR #3026) <https://github.com/apple/foundationdb/pull/3026>`_
* Transaction logs configured to spill by reference had an unintended delay between each spilled batch. `(PR #3153) <https://github.com/apple/foundationdb/pull/3153>`_
* Added guards to honor ``DISABLE_POSIX_KERNEL_AIO``. `(PR #2888) <https://github.com/apple/foundationdb/pull/2888>`_
* Prevent blob upload timeout if request timeout is lower than expected request time. `(PR #3533) <https://github.com/apple/foundationdb/pull/3533>`_
* In very rare scenarios, the data distributor process would crash when being shutdown. `(PR #3530) <https://github.com/apple/foundationdb/pull/3530>`_
* The master would die immediately if it did not have the correct cluster controller interface when recruited. [6.3.4] `(PR #3537) <https://github.com/apple/foundationdb/pull/3537>`_
* Fix an issue where ``fdbcli --exec 'exclude no_wait ...'`` would incorrectly report that processes can safely be removed from the cluster. [6.3.5] `(PR #3566) <https://github.com/apple/foundationdb/pull/3566>`_
* Commit latencies could become large because of inaccurate compute estimates. [6.3.9] `(PR #3845) <https://github.com/apple/foundationdb/pull/3845>`_
* Added a timeout on TLS handshakes to prevent them from hanging indefinitely. [6.3.9] `(PR #3850) <https://github.com/apple/foundationdb/pull/3850>`_

Status
------

* A process's ``memory.available_bytes`` can no longer exceed the memory limit of the process. For purposes of this statistic, processes on the same machine will be allocated memory proportionally based on the size of their memory limits. `(PR #3174) <https://github.com/apple/foundationdb/pull/3174>`_
* Replaced ``cluster.database_locked`` status field with ``cluster.database_lock_state``, which contains two subfields: ``locked`` (boolean) and ``lock_uid`` (which contains the database lock uid if the database is locked). `(PR #2058) <https://github.com/apple/foundationdb/pull/2058>`_
* Removed fields ``worst_version_lag_storage_server`` and ``limiting_version_lag_storage_server`` from the ``cluster.qos`` section. The ``worst_data_lag_storage_server`` and ``limiting_data_lag_storage_server`` objects can be used instead. `(PR #3196) <https://github.com/apple/foundationdb/pull/3196>`_
* If a process is unable to flush trace logs to disk, the problem will now be reported via the output of ``status`` command inside ``fdbcli``. `(PR #2605) <https://github.com/apple/foundationdb/pull/2605>`_ `(PR #2820) <https://github.com/apple/foundationdb/pull/2820>`_
* When a configuration key is changed, it will always be included in ``status json`` output, even the value is reverted back to the default value. [6.3.5] `(PR #3610) <https://github.com/apple/foundationdb/pull/3610>`_

Bindings
--------

* API version updated to 630. See the :ref:`API version upgrade guide <api-version-upgrade-guide-630>` for upgrade details.
* Python: The ``@fdb.transactional`` decorator will now throw an error if the decorated function returns a generator. `(PR #1724) <https://github.com/apple/foundationdb/pull/1724>`_
* Java: Add caching for various JNI objects to improve performance. `(PR #2809) <https://github.com/apple/foundationdb/pull/2809>`_
* Java: Optimize byte array comparisons in ``ByteArrayUtil``. `(PR #2823) <https://github.com/apple/foundationdb/pull/2823>`_
* Java: Add ``FDB.disableShutdownHook`` that can be used to prevent the default shutdown hook from running. Users of this new function should make sure to call ``stopNetwork`` before terminating a client process. `(PR #2635) <https://github.com/apple/foundationdb/pull/2635>`_
* Java: Introduced ``keyAfter`` utility function that can be used to create the immediate next key for a given byte array. `(PR #2458) <https://github.com/apple/foundationdb/pull/2458>`_
* Java:  Combined ``getSummary()`` and ``getResults()`` JNI calls for ``getRange()`` queries. [6.3.5] `(PR #3681) <https://github.com/apple/foundationdb/pull/3681>`_
* Java:  Added support to use ``DirectByteBuffers`` in ``getRange()`` requests for better performance, which can be enabled using ``FDB.enableDirectBufferQueries``. [6.3.5] `(PR #3681) <https://github.com/apple/foundationdb/pull/3681>`_
* Golang: The ``Transact`` function will unwrap errors that have been wrapped using ``xerrors`` to determine if a retryable FoundationDB error is in the error chain. `(PR #3131) <https://github.com/apple/foundationdb/pull/3131>`_
* Golang: Added ``Subspace.PackWithVersionstamp`` that can be used to pack a ``Tuple`` that contains a versionstamp. `(PR #2243) <https://github.com/apple/foundationdb/pull/2243>`_
* Golang: Implement ``Stringer`` interface for ``Tuple``, ``Subspace``, ``UUID``, and ``Versionstamp``. `(PR #3032) <https://github.com/apple/foundationdb/pull/3032>`_
* C: The ``FDBKeyValue`` struct's ``key`` and ``value`` members have changed type from ``void*`` to ``uint8_t*``. `(PR #2622) <https://github.com/apple/foundationdb/pull/2622>`_
* Deprecated ``enable_slow_task_profiling`` network option and replaced it with ``enable_run_loop_profiling``. `(PR #2608) <https://github.com/apple/foundationdb/pull/2608>`_

Other Changes
-------------

* Small key ranges which are being heavily read will be reported in the logs using the trace event ``ReadHotRangeLog``. `(PR #2046) <https://github.com/apple/foundationdb/pull/2046>`_ `(PR #2378) <https://github.com/apple/foundationdb/pull/2378>`_ `(PR #2532) <https://github.com/apple/foundationdb/pull/2532>`_
* Added the read version, commit version, and datacenter locality to the client transaction information.  `(PR #3079) <https://github.com/apple/foundationdb/pull/3079>`_  `(PR #3205) <https://github.com/apple/foundationdb/pull/3205>`_
* Added a network option ``TRACE_FILE_IDENTIFIER`` that can be used to provide a custom identifier string that will be part of the file name for all trace log files created on the client. `(PR #2869) <https://github.com/apple/foundationdb/pull/2869>`_
* It is now possible to use the ``TRACE_LOG_GROUP`` option on a client process after the database has been created. `(PR #2862) <https://github.com/apple/foundationdb/pull/2862>`_
* Added a network option ``TRACE_CLOCK_SOURCE`` that can be used to switch the trace event timestamps to use a realtime clock source. `(PR #2329) <https://github.com/apple/foundationdb/pull/2329>`_
* The ``INCLUDE_PORT_IN_ADDRESS`` transaction option is now on by default. This means ``get_addresses_for_key`` will always return ports in the address strings. `(PR #2639) <https://github.com/apple/foundationdb/pull/2639>`_
* Added the ``getversion`` command to ``fdbcli`` which returns the current read version of the cluster.  `(PR #2882) <https://github.com/apple/foundationdb/pull/2882>`_
* Added the ``advanceversion`` command to ``fdbcli`` which increases the current version of a cluster.  `(PR #2965) <https://github.com/apple/foundationdb/pull/2965>`_
* Improved the slow task profiler to also report backtraces for periods when the run loop is saturated. `(PR #2608) <https://github.com/apple/foundationdb/pull/2608>`_
* Double the number of shard locations that the client will cache locally. `(PR #2198) <https://github.com/apple/foundationdb/pull/2198>`_
* Replaced the ``-add_prefix`` and ``-remove_prefix`` options with ``--add_prefix`` and ``--remove_prefix`` in ``fdbrestore`` `(PR 3206) <https://github.com/apple/foundationdb/pull/3206>`_
* Data distribution metrics can now be read using the special keyspace ``\xff\xff/metrics/data_distribution_stats``. `(PR #2547) <https://github.com/apple/foundationdb/pull/2547>`_
* The ``\xff\xff/worker_interfaces/`` keyspace now begins at a key which includes a trailing ``/`` (previously ``\xff\xff/worker_interfaces``). Range reads to this range now respect the end key passed into the range and include the keyspace prefix in the resulting keys. `(PR #3095) <https://github.com/apple/foundationdb/pull/3095>`_
* Added FreeBSD support. `(PR #2634) <https://github.com/apple/foundationdb/pull/2634>`_
* Updated boost to 1.72.  `(PR #2684) <https://github.com/apple/foundationdb/pull/2684>`_
* Calling ``fdb_run_network`` multiple times in a single run of a client program now returns an error instead of causing undefined behavior. [6.3.1] `(PR #3229) <https://github.com/apple/foundationdb/pull/3229>`_
* Blob backup URL parameter ``request_timeout`` changed to ``request_timeout_min``, with prior name still supported. `(PR #3533) <https://github.com/apple/foundationdb/pull/3533>`_
* Support query command in backup CLI that allows users to query restorable files by key ranges. [6.3.6] `(PR #3703) <https://github.com/apple/foundationdb/pull/3703>`_
* Report missing old tlogs information when in recovery before storage servers are fully recovered. [6.3.6] `(PR #3706) <https://github.com/apple/foundationdb/pull/3706>`_
* Updated OpenSSL to version 1.1.1h. [6.3.7] `(PR #3809) <https://github.com/apple/foundationdb/pull/3809>`_
* Lowered the amount of time a watch will remain registered on a storage server from 900 seconds to 30 seconds. [6.3.8] `(PR #3833) <https://github.com/apple/foundationdb/pull/3833>`_

Fixes from previous versions
----------------------------

* The 6.3.1 patch release includes all fixes from the patch releases 6.2.21 and 6.2.22. :doc:`(6.2 Release Notes) </release-notes/release-notes-620>`
* The 6.3.3 patch release includes all fixes from the patch release 6.2.23. :doc:`(6.2 Release Notes) </release-notes/release-notes-620>`
* The 6.3.5 patch release includes all fixes from the patch releases 6.2.24 and 6.2.25. :doc:`(6.2 Release Notes) </release-notes/release-notes-620>`

Fixes only impacting 6.3.0+
---------------------------

* Clients did not probably balance requests to the proxies. [6.3.3] `(PR #3377) <https://github.com/apple/foundationdb/pull/3377>`_
* Renamed ``MIN_DELAY_STORAGE_CANDIDACY_SECONDS`` knob to ``MIN_DELAY_CC_WORST_FIT_CANDIDACY_SECONDS``. [6.3.2] `(PR #3327) <https://github.com/apple/foundationdb/pull/3327>`_
* Refreshing TLS certificates could cause crashes. [6.3.2] `(PR #3352) <https://github.com/apple/foundationdb/pull/3352>`_
* All storage class processes attempted to connect to the same coordinator. [6.3.2] `(PR #3361) <https://github.com/apple/foundationdb/pull/3361>`_
* Adjusted the proxy load balancing algorithm to be based on the CPU usage of the process instead of the number of requests processed. [6.3.5] `(PR #3653) <https://github.com/apple/foundationdb/pull/3653>`_
* Only return the error code ``batch_transaction_throttled`` for API versions greater than or equal to 630. [6.3.6] `(PR #3799) <https://github.com/apple/foundationdb/pull/3799>`_
* The fault tolerance calculation in status did not take into account region configurations. [6.3.8] `(PR #3836) <https://github.com/apple/foundationdb/pull/3836>`_
* Get read version tail latencies were high because some proxies were serving more read versions than other proxies. [6.3.9] `(PR #3845) <https://github.com/apple/foundationdb/pull/3845>`_

Earlier release notes
---------------------
* :doc:`6.2 (API Version 620) </release-notes/release-notes-620>`
* :doc:`6.1 (API Version 610) </release-notes/release-notes-610>`
* :doc:`6.0 (API Version 600) </release-notes/release-notes-600>`
* :doc:`5.2 (API Version 520) </release-notes/release-notes-520>`
* :doc:`5.1 (API Version 510) </release-notes/release-notes-510>`
* :doc:`5.0 (API Version 500) </release-notes/release-notes-500>`
* :doc:`4.6 (API Version 460) </release-notes/release-notes-460>`
* :doc:`4.5 (API Version 450) </release-notes/release-notes-450>`
* :doc:`4.4 (API Version 440) </release-notes/release-notes-440>`
* :doc:`4.3 (API Version 430) </release-notes/release-notes-430>`
* :doc:`4.2 (API Version 420) </release-notes/release-notes-420>`
* :doc:`4.1 (API Version 410) </release-notes/release-notes-410>`
* :doc:`4.0 (API Version 400) </release-notes/release-notes-400>`
* :doc:`3.0 (API Version 300) </release-notes/release-notes-300>`
* :doc:`2.0 (API Version 200) </release-notes/release-notes-200>`
* :doc:`1.0 (API Version 100) </release-notes/release-notes-100>`
* :doc:`Beta 3 (API Version 23) </release-notes/release-notes-023>`
* :doc:`Beta 2 (API Version 22) </release-notes/release-notes-022>`
* :doc:`Beta 1 (API Version 21) </release-notes/release-notes-021>`
* :doc:`Alpha 6 (API Version 16) </release-notes/release-notes-016>`
* :doc:`Alpha 5 (API Version 14) </release-notes/release-notes-014>`
