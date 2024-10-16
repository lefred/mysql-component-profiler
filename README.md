# mysql-component-profiler

This is component collection to extend MySQL providing profiler capabilities using gperftools (https://github.com/gperftools/gperftools).

When using tcmalloc, you have the possibility to profile the memory, the CPU or both. 

The component uses `pprof` to generate text reports or dot output.

## installation & prerequisities

There are 3 components:

- `component_profiler.so`: the main one. It's a dependency of the other ones 
- `component_profiler_memory.so`: to profile memory. It requires `component_profiler.so` to be installed.
- `component_profiler_cpu.so`: to porfile CPU. It requires `component_profiler.so` to be installed.

`component_profiler_cpu.so` and `component_profiler_memory.so` can be installed independently.

To install the components copy the `component_profiler.so` and the memory and/or cpu files into the plugins directory, `/usr/lib64/mysql/plugin/` on Oracle Linux.

```
MySQL > install component 'file://component_profiler';
Query OK, 0 rows affected (0.0017 sec)

MySQL > install component 'file://component_profiler_cpu';
Query OK, 0 rows affected (0.0017 sec)

MySQL > install component 'file://component_profiler_memory';
Query OK, 0 rows affected (0.0017 sec)

MySQL > select * from mysql.component;
+--------------+--------------------+----------------------------------+
| component_id | component_group_id | component_urn                    |
+--------------+--------------------+----------------------------------+
|            1 |                  1 | file://component_profiler        |
|            2 |                  2 | file://component_profiler_cpu    |
|            3 |                  3 | file://component_profiler_memory |
+--------------+--------------------+----------------------------------+
3 rows in set (0.0006 sec)
```

During the installation of the components, the following lines will be added in the error log:

```
2024-10-16T16:24:39.404023Z 8 [Note] [MY-011071] [Server] Component profiler reported: 'initializing…'
2024-10-16T16:24:39.404131Z 8 [Note] [MY-011071] [Server] Component profiler reported: 'new variable 'profiler.dump_path' has been registered successfully.'
2024-10-16T16:24:39.404200Z 8 [Note] [MY-011071] [Server] Component profiler reported: 'new variable 'profiler.pprof_binary' has been registered successfully.'
2024-10-16T16:24:41.536139Z 8 [Note] [MY-011071] [Server] Component profiler_cpu reported: 'initializing…'
2024-10-16T16:24:41.536184Z 8 [Note] [MY-011071] [Server] Component profiler_cpu reported: 'new UDF 'cpuprof_start()' has been registered successfully.'
2024-10-16T16:24:41.536202Z 8 [Note] [MY-011071] [Server] Component profiler_cpu reported: 'new UDF 'cpuprof_stop()' has been registered successfully.'
2024-10-16T16:24:41.536220Z 8 [Note] [MY-011071] [Server] Component profiler_cpu reported: 'new UDF 'cpuprof_report()' has been registered successfully.'
2024-10-16T16:24:41.536271Z 8 [Note] [MY-011071] [Server] Component profiler_cpu reported: 'Status variable(s) registered'
2024-10-16T16:32:15.037022Z 8 [Note] [MY-011071] [Server] Component profiler_memory reported: 'initializing…'
2024-10-16T16:32:15.037071Z 8 [Note] [MY-011071] [Server] Component profiler_memory reported: 'new UDF 'memprof_start()' has been registered successfully.'
2024-10-16T16:32:15.037095Z 8 [Note] [MY-011071] [Server] Component profiler_memory reported: 'new UDF 'memprof_stop()' has been registered successfully.'
2024-10-16T16:32:15.037114Z 8 [Note] [MY-011071] [Server] Component profiler_memory reported: 'new UDF 'memprof_dump()' has been registered successfully.'
2024-10-16T16:32:15.037133Z 8 [Note] [MY-011071] [Server] Component profiler_memory reported: 'new UDF 'memprof_report()' has been registered successfully.'
2024-10-16T16:32:15.037203Z 8 [Note] [MY-011071] [Server] Component profiler_memory reported: 'Status variable(s) registered
```

As we can see several UDFs were created:

```
MySQL > SELECT UDF_NAME FROM performance_schema.user_defined_functions where udf_name like '%PROF_%';
+----------------+
| UDF_NAME       |
+----------------+
| MEMPROF_REPORT |
| MEMPROF_DUMP   |
| CPUPROF_STOP   |
| MEMPROF_STOP   |
| CPUPROF_START  |
| MEMPROF_START  |
| CPUPROF_REPORT |
+----------------+
7 rows in set (0.0007 sec)
```

To use the component, the user needs to have the `SENSITIVE_VARIABLES_OBSERVER` privilege granted.

## global variables

```
show global variables like 'profiler.%';
+-----------------------+-----------------+
| Variable_name         | Value           |
+-----------------------+-----------------+
| profiler.dump_path    | /tmp/dimk/mysql |
| profiler.pprof_binary | /usr/bin/pprof  |
+-----------------------+-----------------+
2 rows in set (0.0011 sec)
```

### profiler.dump_path

This defines where the collected data should be dumped on the server.

### profiled.pprof_binary

The only way to parse the collected data is the use the `pprof` program. This variables defines where is installed the pprof binary executable file.

## status variables

```
MySQL > show status like 'profiler.%';
+------------------------+---------+
| Variable_name          | Value   |
+------------------------+---------+
| profiler.cpu_status    | STOPPED |
| profiler.memory_status | STOPPED |
+------------------------+---------+
2 rows in set (0.0107 sec)
```

These status variables provides the status of the profiling operations.

## CPU profiling

### start

To start the CPU profiling, we use the following command:

```
MySQL > select cpuprof_start();
+-----------------------+
| cpuprof_start()       |
+-----------------------+
| cpu profiling started |
+-----------------------+
1 row in set (0.0022 sec)
```

Now the status variable changed:

```
MySQL > show status like 'profiler.cpu_status';
+---------------------+---------+
| Variable_name       | Value   |
+---------------------+---------+
| profiler.cpu_status | RUNNING |
+---------------------+---------+
1 row in set (0.0018 sec)
```

### stop

To stop the collection, we use the following statement:

```
MySQL > select cpuprof_stop();
+-----------------------+
| cpuprof_stop()        |
+-----------------------+
| cpu profiling stopped |
+-----------------------+
1 row in set (0.0033 sec)
```

This generates a file `.prof` in the directory and file defined by `profiler.dump_path`:

```
$ ls -lh /tmp/dimk/
total 68K
-rw-rw---- 1 fred fred 68K Oct 14 21:08 mysql.prof
```

### report

Now we can generate a report in two format: TEXT (the default) or DOT.

#### text

To generate the report we use the following statement:

```
MySQL > select cpuprof_report()\G
*************************** 1. row ***************************
cpuprof_report(): Total: 95 samples
      39  41.1%  41.1%       39  41.1% __futex_abstimed_wait_common
       6   6.3%  47.4%        6   6.3% __GI___lll_lock_wake
       4   4.2%  51.6%       45  47.4% ___pthread_cond_timedwait
       4   4.2%  55.8%        5   5.3% __clock_gettime_2
       3   3.2%  58.9%        3   3.2% __getrusage
       3   3.2%  62.1%        6   6.3% pfs_end_mutex_wait_v1
       3   3.2%  65.3%        3   3.2% syscall
       2   2.1%  67.4%        3   3.2% OSMutex::enter (inline)
       2   2.1%  69.5%        2   2.1% PFS_single_stat::aggregate_value (inline)
       2   2.1%  71.6%        2   2.1% log_update_concurrency_margin
       2   2.1%  73.7%       47  49.5% os_event::timed_wait
       2   2.1%  75.8%        2   2.1% pfs_start_mutex_wait_v1
       1   1.1%  76.8%        1   1.1% 0x00007ffb339e8af4
       1   1.1%  77.9%        1   1.1% IB_thread::state (inline)
       1   1.1%  78.9%        1   1.1% Log_files_capacity::next_file_earlier_margin (inline)
       1   1.1%  80.0%       11  11.6% PolicyMutex::enter
       1   1.1%  81.1%        3   3.2% PolicyMutex::pfs_begin_lock (inline)
       1   1.1%  82.1%        1   1.1% ___pthread_mutex_lock
       1   1.1%  83.2%        1   1.1% ___pthread_mutex_unlock
       1   1.1%  84.2%        1   1.1% __condvar_dec_grefs
       1   1.1%  85.3%        1   1.1% __memset_evex_unaligned_erms
       1   1.1%  86.3%        1   1.1% __pthread_cleanup_pop
       1   1.1%  87.4%        1   1.1% __strcpy_evex
... 
```

#### dot

We can generate the content of a dot file that can be used to generate an image:

```
MySQL > select cpuprof_report('dot') into outfile 'cpu.dot';
Query OK, 1 row affected (5.6931 sec)
```

```
$ dot -Tpng cpu.dot -o cpu.png
```

![CPU](examples/cpu.png)

## Memory profiling

### start

To start the profiling, we need to use the following statement:

```
MySQL > select memprof_start();
+--------------------------+
| memprof_start()          |
+--------------------------+
| memory profiling started |
+--------------------------+
1 row in set (0.0022 sec) 
```

We can confirm this from the status variable:

```
MySQL > show status like 'profiler.memory_status';
+------------------------+---------+
| Variable_name          | Value   |
+------------------------+---------+
| profiler.memory_status | RUNNING |
+------------------------+---------+
1 row in set (0.0071 sec)
```

### dump

Differently than for the CPU profiling, we have the possibility to dump the collected data for the memory
to disk manually:

```
MySQL > select memprof_dump();
+------------------------------+
| memprof_dump()               |
+------------------------------+
| memory profiling data dumped |
+------------------------------+
1 row in set (0.0036 sec) 
```

We need to dump at the data we want to use for the reporting. Starting and stopping without dumping, won't produce
any file to parse.

We can also provide information about the dump that will be printed to error log: 

```
MySQL > select memprof_dump('after a large select');
+--------------------------------------+
| memprof_dump('after a large select') |
+--------------------------------------+
| memory profiling data dumped         |
+--------------------------------------+
1 row in set (0.0014 sec)
```

When no string is provided, the default `user request` is used.

We can see in error log:

```
Dumping heap profile to /tmp/dimk/mysql.0001.heap (user request)
Dumping heap profile to /tmp/dimk/mysql.0002.heap (after a large select)
```

And on the filesystem:

```
$ ls -lh /tmp/dimk/*heap
-rw-rw---- 1 fred fred 301K Oct 14 21:32 /tmp/dimk/mysql.0001.heap
-rw-rw---- 1 fred fred 302K Oct 14 21:33 /tmp/dimk/mysql.0002.heap
```

### stop

Before being able to generate a report, we need to stop the memory profiling:

```
MySQL > select memprof_stop();
+--------------------------+
| memprof_stop()           |
+--------------------------+
| memory profiling stopped |
+--------------------------+
1 row in set (0.0035 sec)
```

### report

Now we can generate a report for the memory in two format: TEXT (the default) or DOT.

#### text

To generate the report we use the following statement:

```
MySQL > select memprof_report()\G
*************************** 1. row ***************************
memprof_report(): Total: 1.0 MB
     0.7  68.1%  68.1%      0.7  68.1% redirecting_allocator (inline)
     0.3  27.6%  95.7%      0.3  27.6% ut::detail::malloc (inline)
     0.0   1.5%  97.2%      0.0   1.5% __gnu_cxx::__aligned_membuf::_M_addr (inline)
     0.0   1.4%  98.7%      0.0   1.4% std::__cxx11::basic_string::_M_local_data (inline)
     0.0   1.0%  99.7%      0.0   1.5% std::pair::pair (inline)
     0.0   0.1%  99.8%      0.0   0.4% Table_cache::add_used_table (inline)
     0.0   0.1%  99.8%      0.0   0.1% std::__new_allocator::allocate (inline)
     0.0   0.1%  99.9%      0.0   0.1% std::vector::reserve (inline)
     0.0   0.0%  99.9%      0.0   0.0% std::__detail::_Hashtable_alloc::_M_allocate_buckets [clone .isra.0]
     0.0   0.0% 100.0%      0.0   2.9% std::construct_at (inline)
     0.0   0.0% 100.0%      0.0   0.0% void* my_internal_malloc [clone .lto_priv.0] (inline)
     0.0   0.0% 100.0%      0.0   0.0% ngs::Socket_events::callback_timeout (inline)
     0.0   0.0% 100.0%      0.0   0.0% std::__cxx11::basic_string::_M_capacity (inline)
     0.0   0.0% 100.0%      0.0   2.3% Field_blob::clone
     0.0   0.0% 100.0%      0.0   0.2% Field_blob::store_internal
     0.0   0.0% 100.0%      0.0   1.4% Field_enum::clone
     0.0   0.0% 100.0%      0.0   0.7% Field_long::clone
     0.0   0.0% 100.0%      0.0   3.6% Field_longlong::clone
...
1 row in set (5.0192 sec)
```

#### dot

We can generate the ouput in dot format:

```
MySQL > select memprof_report('dot') into outfile 'memory.dot';
Query OK, 1 row affected (4.7576 sec)
```

We can use the file to generate an image:

```
$ dot -Tpng memory.dot -o memory.png
```
![Memory](examples/memory.png)

## errors, warnings, messages

### dependency

When installing `component_profiler_cpu` or `component_profiler_memory` if `component_profiler` is not installed, the following error is displayed: 

```
ERROR: 3534 (HY000): Cannot satisfy dependency for service 'profiler_var'
required by component 'mysql:profiler_cpu_service'.
```

### privilege

The privilege is checked to modify variables and call the UDFs:

```
MySQL > select memprof_report('dot') into outfile 'memory2.dot' ;
ERROR: 1227 (42000): Access denied; you need (at least one of) the 
       SENSITIVE_VARIABLES_OBSERVER privilege(s) for this operation

MySQL > set global profiler.dump_path='/tmp/dimk/mysql2';
ERROR: 1227 (42000): Access denied; you need (at least one of) the
        SENSITIVE_VARIABLES_OBSERVER privilege(s) for this operation
```

### folders and privileges

If the folder doesn't exist or mysql user has no rights, an error will be returned:

```
MySQL > set global profiler.dump_path='/tmp/lefred/mysql';
ERROR: 3200 (HY000): memprof UDF failed; we don't have access to write in that folder.
```

### previous existing data

If there is already data with the same name as defined in `profiler.dump_path`, not profiling can be started:

```
MySQL > select memprof_start();
ERROR: 3200 (HY000): profiler UDF failed; There is already a heap dump, 
       change the 'profiler.dump_path' value first.
```

The previous data must be deleted or another name must be provided.

### report without stopping

Any report must be generated only if the profiling is stopped:

```
MySQL > select memprof_report()\G
ERROR: 3200 (HY000): profiler UDF failed; memory profiler is still running, you need to stop it first.
```

### tcmalloc

MySQL must be started using tcmalloc memory allocator (`LD_PRELOAD=/usr/lib64/libtcmalloc_and_profiler.so`):

```
MySQL > install component 'file://component_profiler';
ERROR: 1126 (HY000): Can't open shared library '/home/fred/workspace/mysql-server/BIN-DEBUG/lib/plugin/component_profiler.so'
(errno: 0 /home/fred/workspace/mysql-server/BIN-DEBUG/lib/plugin/component_profiler.so: undefined symbol: ProfilerStart)
```



