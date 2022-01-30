.. _command-line-interface:

######################
Command Line Interface
######################

.. include:: guide-common.rst.inc

FoundationDB comes with a command line interface tool called ``fdbcli``. This document describes basic usage of ``fdbcli`` and the commands it supports. The use of ``fdbcli`` while :doc:`configuring <configuration>` and :doc:`administering <administration>` FoundationDB clusters is described in more detail in the documents on those topics and will be referenced as appropriate.

.. _cli-invocation:

Invocation at the Command Line
==============================

You can invoke ``fdbcli`` at the command line simply by typing it. For example::

    user@host$ fdbcli
    Using cluster file `/etc/foundationdb/fdb.cluster'.

    The database is available.

    Welcome to the fdbcli. For help, type `help'.
    fdb>

This will result in ``fdbcli`` connecting to the :ref:`default cluster file <default-cluster-file>` (``/etc/foundationdb/fdb.cluster`` for Linux.) You can also specify a cluster file as an argument to ``fdbcli`` using the ``-C`` option. For further information, see :ref:`specifying-a-cluster-file`.

Commands within ``fdbcli``
==========================

The following commands can be issued from within ``fdbcli`` at the internal ``fdb>`` prompt:

advanceversion
--------------

Forces the cluster to recover at the specified version. If the specified version is larger than the current version of the cluster, the cluster version is advanced to the specified version via a forced recovery.

begin
-----

The ``begin`` command begins a new transaction. By default, ``fdbcli`` operates in autocommit mode. All operations are performed in their own transaction and are automatically committed. By explicitly beginning a transaction, successive operations are all performed as part of a single transaction.

To commit the transaction, use the ``commit`` command. To discard the transaction, use the ``reset`` command.

clear
-----

The ``clear`` command clears a key from the database. Its syntax is ``clear <KEY>``. This command succeeds even if the specified key is not present but may fail due to conflicts.

Note that :ref:`characters can be escaped <cli-escaping>` when specifying keys (or values) in ``fdbcli``.

clearrange
----------

The ``clearrange`` command clears a range of keys from the database. Its syntax is ``clearrange <BEGINKEY> <ENDKEY>``. All keys between ``<BEGINKEY>`` (inclusive) and ``<ENDKEY>`` (exclusive) are cleared from the database. This command succeeds even if the specified range is empty but may fail due to conflicts.

Note that :ref:`characters can be escaped <cli-escaping>` when specifying keys (or values) in ``fdbcli``.

commit
------

The ``commit`` command commits the current transaction. Any sets or clears executed after the start of the current transaction will be committed to the database. On success, the committed version number is displayed. If commit fails, the error is displayed and the transaction must be retried.

configure
---------

The ``configure`` command changes the database configuration. Its syntax is ``configure [new] [single|double|triple|three_data_hall|three_datacenter] [ssd|memory] [proxies=<N>] [resolvers=<N>] [logs=<N>]``.

The ``new`` option, if present, initializes a new database with the given configuration rather than changing the configuration of an existing one. When ``new`` is used, both a redundancy mode and a storage engine must be specified.

redundancy mode
^^^^^^^^^^^^^^^

Redundancy modes define storage requirements, required cluster size, and resilience to failure. The available redundancy modes are:

* ``single``
* ``double``
* ``triple``
* ``three_data_hall``
* ``three_datacenter``

For descriptions of redundancy modes, see :ref:`configuration-choosing-redundancy-mode`.

storage engine
^^^^^^^^^^^^^^^

The storage engine is responsible for durably storing data. FoundationDB has two storage engines:

* ``ssd``
* ``memory``

For descriptions of storage engines, see :ref:`configuration-storage-engine`.

process types
^^^^^^^^^^^^^

A FoundationDB cluster employs server processes of different types. It automatically allocates these processes in default numbers appropriate for small-to-medium sized clusters.

For large clusters, you can manually set the allocated number of processes of a given type. Valid process types are:

* ``proxies``
* ``resolvers``
* ``logs``

Set the process using ``configure [proxies|resolvers|logs]=<N>``, where ``<N>`` is an integer greater than 0, or -1 to reset the value to its default.

For recommendations on appropriate values for process types in large clusters, see :ref:`guidelines-process-class-config`.

consistencycheck
----------------

The ``consistencycheck`` command enables or disables consistency checking. Its syntax is ``consistencycheck [on|off]``. Calling it with ``on`` enables consistency checking, and ``off`` disables it. Calling it with no arguments displays whether consistency checking is currently enabled.

You must be running an ``fdbserver`` process with the ``consistencycheck`` role to perform consistency checking.

coordinators
------------

The ``coordinators`` command is used to change cluster coordinators or description. Its syntax is ``coordinators auto|<ADDRESS...> [description=<DESC>]``.

Addresses may be specified as a list of IP:port pairs (such as ``coordinators 10.0.0.1:4000 10.0.0.2:4000 10.0.0.3:4000``). If addresses are specified, the coordinators will be set to them. An ``fdbserver`` process must be running on each of the specified addresses.

If ``auto`` is specified, coordinator addresses will be chosen automatically to support the configured redundancy level. Processes with class coordinator will be prioritized. (If the current set of coordinators are healthy and already support the configured redundancy level, nothing will be changed.)

For more information on setting coordinators, see :ref:`configuration-changing-coordination-servers`.

If ``description=<DESC>`` is specified, the description field in the cluster file is changed to ``<DESC>``, which must match ``[A-Za-z0-9_]+``.

For more information on setting the cluster description, see :ref:`configuration-setting-cluster-description`.

exclude
-------

The ``exclude`` command excludes servers from the database or marks them as failed. Its syntax is ``exclude [failed] <ADDRESS...>``. If no addresses are specified, the command provides the set of excluded and failed servers.

For each IP address or IP:port pair in ``<ADDRESS...>``, the command adds the address to the set of excluded servers. It then waits until all database state has been safely moved off the specified servers.

If the ``failed`` keyword is specified, the address is marked as failed and added to the set of failed servers. It will not wait for the database state to move off the specified servers.

For more information on excluding servers, see :ref:`removing-machines-from-a-cluster`.

exit
----

The ``exit`` command exits ``fdbcli``.

fileconfigure
-------------

The ``fileconfigure`` command is alternative to the ``configure`` command which changes the configuration of the database based on a json document. The command loads a JSON document from the provided file, and change the database configuration to match the contents of the JSON document.

The format should be the same as the value of the ``configuration`` entry in status JSON without ``excluded_servers`` or ``coordinators_count``. Its syntax is ``fileconfigure [new] <FILENAME>``.

"The ``new`` option, if present, initializes a new database with the given configuration rather than changing the configuration of an existing one.

force_recovery_with_data_loss
-----------------------------

The ``force_recovery_with_data_loss`` command will recover a multi-region database to the specified datacenter. Its syntax is ``force_recovery_with_data_loss <DCID>``. It will likely result in the loss of the most recently committed mutations and is intended to be used if the primary datacenter has been lost. 

This command will change the :ref:`region configuration <configuration-configuring-regions>` to have a positive priority for the chosen ``DCID`` and a negative priority for all other ``DCIDs``. It will also set ``usable_regions`` to 1. If the database has already recovered, this command does nothing.

get
---

The ``get`` command fetches the value of a given key. Its syntax is ``get <KEY>``. It displays the value of ``<KEY>`` if ``<KEY>`` is present in the database and ``not found`` otherwise.

Note that :ref:`characters can be escaped <cli-escaping>` when specifying keys (or values) in ``fdbcli``.

getrange
--------

The ``getrange`` command fetches key-value pairs in a range. Its syntax is ``getrange <BEGINKEY> [ENDKEY] [LIMIT]``. It displays up to ``<LIMIT>`` keys and values for keys between ``<BEGINKEY>`` (inclusive) and ``<ENDKEY>`` (exclusive). If ``<ENDKEY>`` is omitted, then the range will include all keys starting with ``<BEGINKEY>``. ``<LIMIT>`` defaults to 25 if omitted.

Note that :ref:`characters can be escaped <cli-escaping>` when specifying keys (or values) in ``fdbcli``.

getrangekeys
------------

The ``getrangekeys`` command fetches keys in a range. Its syntax is ``getrangekeys <BEGINKEY> [ENDKEY] [LIMIT]``. It displays up to ``<LIMIT>`` keys for keys between ``<BEGINKEY>`` (inclusive) and ``<ENDKEY>`` (exclusive). If ``<ENDKEY>`` is omitted, then the range will include all keys starting with ``<BEGINKEY>``. ``<LIMIT>`` defaults to 25 if omitted.

Note that :ref:`characters can be escaped <cli-escaping>` when specifying keys (or values) in ``fdbcli``.

getversion
----------

The ``getversion`` command fetches the current read version of the cluster or currently running transaction.

help
----

The ``help`` command provides information on specific commands. Its syntax is ``help <TOPIC>``, where ``<TOPIC>`` is any of the commands in this section, ``escaping``, or ``options``. The latter two topics are described below:

.. _cli-escaping:

help escaping
^^^^^^^^^^^^^

``help escaping`` provides the following information on escaping keys and values within ``fdbcli``:

When parsing commands, ``fdbcli`` considers a space to delimit individual tokens. To include a space in a single value, you may either enclose the token in quotation marks ``"``, prefix the space with a backslash ``\``, or encode the space as a hex character.

To include a literal quotation mark in a token, precede it with a backslash ``\"``.

To express a binary value, encode each byte as a two-digit hex value, preceded by ``\x`` (e.g. ``\x20`` for a space character, or ``\x0a\x00\x00\x00`` for a 32-bit, little-endian representation of the integer 10).

All keys and values are displayed by ``fdbcli`` with non-printable characters and spaces encoded as two-digit hex bytes.

.. _cli-options:

help options
^^^^^^^^^^^^

The following options are available for use with the ``option`` command:

``ACCESS_SYSTEM_KEYS`` - Allows this transaction to read and modify system keys (those that start with the byte ``0xFF``).

``CAUSAL_READ_RISKY`` - In the event of a fault or partition, the read version returned may not the last committed version potentially causing you to read outdated data.

``CAUSAL_WRITE_RISKY`` - The transaction, if not self-conflicting, may be committed a second time after commit succeeds, in the event of a fault.

``INITIALIZE_NEW_DATABASE`` - This is a write-only transaction which sets the initial configuration.

``NEXT_WRITE_NO_WRITE_CONFLICT_RANGE`` - The next write performed on this transaction will not generate a write conflict range. As a result, other transactions which read the key(s) being modified by the next write will not conflict with this transaction. Care needs to be taken when using this option on a transaction that is shared between multiple threads. When setting this option, write conflict ranges will be disabled on the next write operation, regardless of what thread it is on.

``PRIORITY_BATCH`` - Specifies that this transaction should be treated as low priority and that default priority transactions should be processed first. Useful for doing batch work simultaneously with latency-sensitive work.

``PRIORITY_SYSTEM_IMMEDIATE`` - Specifies that this transaction should be treated as highest priority and that lower priority transactions should block behind this one. Use is discouraged outside of low-level tools.

``READ_AHEAD_DISABLE`` - Disables read-ahead caching for range reads. Under normal operation, a transaction will read extra rows from the database into cache if range reads are used to page through a series of data one row at a time (i.e. if a range read with a one row limit is followed by another one row range read starting immediately after the result of the first).

``READ_YOUR_WRITES_DISABLE`` - Reads performed by a transaction will not see any prior mutations that occurred in that transaction, instead seeing the value which was in the database at the transaction's read version. This option may provide a small performance benefit for the client, but also disables a number of client-side optimizations which are beneficial for transactions which tend to read and write the same keys within a single transaction.

``RETRY_LIMIT`` - Set a maximum number of retries after which additional calls to ``onError`` will throw the most recently seen error code. Valid parameter values are ``[-1, INT_MAX]``. If set to -1, will disable the retry limit. Like all transaction options, the retry limit must be reset after a call to ``onError``. This behavior allows the user to make the retry limit dynamic.

``TIMEOUT`` - Set a timeout in milliseconds which, when elapsed, will cause the transaction automatically to be cancelled. Valid parameter values are ``[0, INT_MAX]``. If set to 0, will disable all timeouts. All pending and any future uses of the transaction will throw an exception. The transaction can be used again after it is reset. Like all transaction options, a timeout must be reset after a call to ``onError``. This behavior allows the user to make the timeouts dynamic.

include
-------

The ``include`` command permits previously excluded or failed servers to rejoin the database. Its syntax is ``include [failed] all|<ADDRESS...>``.

The ``failed`` keyword is required if the servers were previously marked as failed rather than excluded.

If ``all`` is specified, the excluded servers list is cleared. This will not clear the failed servers list.

If ``failed all`` or ``all failed`` is specified, the failed servers list is cleared. This will not clear the excluded servers list.

For each IP address or IP:port pair in ``<ADDRESS...>``, the command removes any matching exclusions from the excluded servers list. (A specified IP will match all ``IP:*`` exclusion entries).

For information on adding machines to a cluster, see :ref:`adding-machines-to-a-cluster`.

kill
----

The ``kill`` command attempts to kill one or more processes in the cluster.

``kill``

With no arguments, ``kill`` populates the list of processes that can be killed. This must be run prior to running any other ``kill`` commands.

``kill list``

Displays all known processes. This is only useful when the database is unresponsive.

``kill <ADDRESS...>``

Attempts to kill all specified processes. Each address should include the IP and port of the process being targeted.

``kill all``

Attempts to kill all known processes in the cluster.

lock
----

The ``lock`` command locks the database with a randomly generated lockUID.

maintenance
-----------

The ``maintenance`` command marks a particular zone ID (i.e. fault domain) as being under maintenance. Its syntax is ``maintenance [on|off] [ZONEID] [SECONDS]``. 

A zone that is under maintenance will not have data moved away from it even if processes in that zone fail. In particular, this means the cluster will not attempt to heal the replication factor as a result of failures in the maintenance zone. This is useful when the amount of time that the processes in a fault domain are expected to be absent is reasonably short and you don't want to move data to and from the affected processes. 

Running this command with no arguments will display the state of any current maintenance.

Running ``maintenance on <ZONEID> <SECONDS>`` will turn maintenance on for the specified zone. A duration must be specified for the length of maintenance mode.

Running ``maintenance off`` will turn off maintenance mode.

option
------

The ``option`` command enables or disables an option. Its syntax is ``option <STATE> <OPTION> [ARG]``. Descriptions of :ref:`the available options <cli-options>` can be obtained within ``fdbcli`` by typing ``help options``.

If ``<STATE>`` is ``on``, then ``<OPTION>`` will be enabled with optional parameter ``<ARG>``, if required. If ``<STATE>`` is ``off``, then ``<OPTION>`` will be disabled.

If there is no active transaction, then the option will be applied to all operations as well as all subsequently created transactions (using ``begin``).

If there is an active transaction (one created with ``begin``), then enabled options apply only to that transaction. Options cannot be disabled on an active transaction.

Calling the ``option`` command with no parameters prints a list of all enabled options.

profile
-------

The ``profile`` command is used to control various profiling actions.

client
^^^^^^

``profile client <get|set>``

Reads or sets parameters of client transaction sampling. Use ``get`` to list the current parameters, and ``set <RATE|default> <SIZE|default>`` to set them. ``RATE`` is the fraction of transactions to be sampled, and ``SIZE`` is the amount (in bytes) of sampled data to store in the database.

list
^^^^

``profile list``

Lists the processes that can be profiled using the ``flow`` and ``heap`` subcommands.

flow
^^^^

``profile flow run <DURATION> <FILENAME> <PROCESS...>``

Enables flow profiling on the specifed processes for ``DURATION`` seconds. Profiling output will be stored at the specified filename relative to the fdbserver process's trace log directory. To profile all processes, use ``all`` for the ``PROCESS`` parameter.

heap
^^^^

``profile heap <PROCESS>``

Enables heap profiling for the specified process.

reset
-----

The ``reset`` command resets the current transaction. Any sets or clears executed after the start of the active transaction will be discarded.

rollback
--------

The ``rollback`` command rolls back the current transaction. The active transaction will be discarded, including any sets or clears executed since the transaction was started.

set
---

The ``set`` command sets a value for a given key. Its syntax is ``set <KEY> <VALUE>``. If ``<KEY>`` is not already present in the database, it will be created.

Note that :ref:`characters can be escaped <cli-escaping>` when specifying keys (or values) in ``fdbcli``.

setclass
--------

The ``setclass`` command can be used to change the :ref:`process class <guidelines-process-class-config>` for a given process. Its syntax is ``setclass [<ADDRESS> <CLASS>]``. If no arguments are specified, then the process classes of all processes are listed. Setting the class to ``default`` to revert to the process class specified on the command line.

The available process classes are ``unset``, ``storage``, ``transaction``, ``resolution``, ``proxy``, ``master``, ``test``, ``unset``, ``stateless``, ``log``, ``router``, ``cluster_controller``, ``fast_restore``, ``data_distributor``, ``coordinator``, ``ratekeeper``, ``storage_cache``, ``backup``, and ``default``.

sleep
-----

The ``sleep`` command inserts a delay before running the next command. Its syntax is ``sleep <SECONDS>``. This command can be useful when ``fdbcli`` is run with the ``--exec`` flag to control the timing of commands.

.. _cli-status:

status
------

The ``status`` command reports the status of the FoundationDB cluster to which ``fdbcli`` is connected. Its syntax is ``status [minimal|details|json]``.


If the cluster is down, this command will print a diagnostic which may be useful
in figuring out what is wrong. If the cluster is running, this command will
print cluster statistics.

status minimal
^^^^^^^^^^^^^^

``status minimal`` will provide only an indication of whether the database is available.

status details
^^^^^^^^^^^^^^

``status details`` will provide load information for individual workers.

For a detailed description of ``status`` output, see :ref:`administration-monitoring-cluster-status`.

status json
^^^^^^^^^^^

``status json`` will provide the cluster status in its JSON format. For a detailed description of this format, see :doc:`mr-status`.

.. _cli-throttle:

throttle
--------

The throttle command is used to inspect and modify the list of throttled transaction tags in the cluster. For more information, see :doc:`transaction-tagging`. The throttle command has the following subcommands:

on
^^

``throttle on tag <TAG> [RATE] [DURATION] [PRIORITY]``

Enables throttling for the specified transaction tag.

``TAG`` - the tag being throttled. This argument is required.

``RATE`` - the number of transactions that may be started per second. Defaults to 0.

``DURATION`` - the duration that the throttle should remain in effect, which must include a time suffix (``s`` - seconds, ``m`` - minutes, ``h`` - hours, ``d`` - days). Defaults to ``1h``.

``PRIORITY`` - the maximum priority that the throttle will apply to. Choices are ``default``, ``batch``, and ``immediate``. Defaults to ``default``.

off
^^^

``throttle off [all|auto|manual] [tag <TAG>] [PRIORITY]``

Disables throttling for all transaction tag throttles that match the specified filtering criteria. At least one filter must be specified, and filters can be given in any order.

``all`` - affects all throttles (auto and manual).

``auto`` - only throttles automatically created by the cluster will be affected.

``manual`` - only throttles created manually will be affected (this is the default).

``tag`` - only the specified tag will be affected.

``PRIORITY`` - the priority of the throttle to disable. Choices are ``default``, ``batch``, and ``immediate``. Defaults to ``default``.

For example, to disable all throttles, run::

> throttle off all

To disable all manually created batch priority throttles, run::

> throttle off batch

To disable auto throttles at batch priority on the tag ``foo``, run::

> throttle off auto tag foo batch

enable
^^^^^^

``throttle enable auto``

Enables cluster auto-throttling for busy transaction tags.

disable
^^^^^^^

``throttle disable auto``

Disables cluster auto-throttling for busy transaction tags. This does not disable any currently active throttles. To do so, run the following command after disabling auto-throttling::

> throttle off auto

list
^^^^

``throttle list [LIMIT]``

Prints a list of currently active transaction tag throttles.

``LIMIT`` - The number of throttles to print. Defaults to 100.

unlock
------

The ``unlock`` command unlocks the database with the specified lock UID. Because this is a potentially dangerous operation, users must copy a passphrase before the unlock command is executed.

writemode
---------

Controls whether or not ``fdbcli`` can perform sets and clears.

``writemode off``

Disables writing from ``fdbcli`` (the default). In this mode, attempting to set or clear keys will result in an error.

``writemode on``

Enables writing from ``fdbcli``.
