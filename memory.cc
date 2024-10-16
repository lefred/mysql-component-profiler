/* Copyright (c) 2017, 2024, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#define NO_SIGNATURE_CHANGE 0
#define SIGNATURE_CHANGE 1

#include "memory.h"

REQUIRES_SERVICE_PLACEHOLDER(log_builtins);
REQUIRES_SERVICE_PLACEHOLDER(log_builtins_string);
REQUIRES_SERVICE_PLACEHOLDER(mysql_thd_security_context);
REQUIRES_SERVICE_PLACEHOLDER(mysql_current_thread_reader);
REQUIRES_SERVICE_PLACEHOLDER(mysql_security_context_options);
REQUIRES_SERVICE_PLACEHOLDER(global_grants_check);
REQUIRES_SERVICE_PLACEHOLDER(udf_registration);
REQUIRES_SERVICE_PLACEHOLDER(mysql_udf_metadata);
REQUIRES_SERVICE_PLACEHOLDER(mysql_runtime_error);
REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_register);
REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_unregister);
REQUIRES_SERVICE_PLACEHOLDER(status_variable_registration);
REQUIRES_SERVICE_PLACEHOLDER(mysql_system_variable_reader);
REQUIRES_SERVICE_PLACEHOLDER(profiler_var);


SERVICE_TYPE(log_builtins) * log_bi;
SERVICE_TYPE(log_builtins_string) * log_bs;

static char memprof_status[] = "STOPPED";

// Buffer for the value of the profiler.dump_path global variable
std::string memprof_dump_path;

static SHOW_VAR memprof_status_variables[] = {
  {"profiler.memory_status", (char *)&memprof_status, SHOW_CHAR,
    SHOW_SCOPE_GLOBAL},{nullptr, nullptr, SHOW_UNDEF,
    SHOW_SCOPE_UNDEF}  // null terminator required
};

class udf_list {
  typedef std::list<std::string> udf_list_t;

 public:
  ~udf_list() { unregister(); }
  bool add_scalar(const char *func_name, enum Item_result return_type,
                  Udf_func_any func, Udf_func_init init_func = NULL,
                  Udf_func_deinit deinit_func = NULL) {
    if (!mysql_service_udf_registration->udf_register(
            func_name, return_type, func, init_func, deinit_func)) {
      set.push_back(func_name);
      return false;
    }
    return true;
  }

  bool unregister() {
    udf_list_t delete_set;
    /* try to unregister all of the udfs */
    for (auto udf : set) {
      int was_present = 0;
      if (!mysql_service_udf_registration->udf_unregister(udf.c_str(),
                                                          &was_present) ||
          !was_present)
        delete_set.push_back(udf);
    }

    /* remove the unregistered ones from the list */
    for (auto udf : delete_set) set.remove(udf);

    /* success: empty set */
    if (set.empty()) return false;

    /* failure: entries still in the set */
    return true;
  }

 private:
  udf_list_t set;
} *list;


int register_status_variables() {
  if (mysql_service_status_variable_registration->register_variable(
          (SHOW_VAR *)&memprof_status_variables)) {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "Failed to register status variable");
    return 1;
  }
    
  LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, "Status variable(s) registered");
  return 0;
}

int unregister_status_variables() {
  if (mysql_service_status_variable_registration->unregister_variable(
          (SHOW_VAR *)&memprof_status_variables)) {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "Failed to unregister status variable");
    return 1;
  }
  LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, "Status variable(s) unregistered");
  return 0;
}


namespace udf_impl {

void error_msg_size() {
  mysql_error_service_emit_printf(mysql_service_mysql_runtime_error,
                                  ER_UDF_ERROR, 0, "profiler",
                                  "There is an error");
}

const char *udf_init = "udf_init", *my_udf = "my_udf",
           *my_udf_clear = "my_clear", *my_udf_add = "my_udf_add";


// UDF to start the memory profiling

static bool memprof_start_udf_init(UDF_INIT *initid, UDF_ARGS *args, char *) {
  if (args->arg_count > 0) {
    mysql_error_service_emit_printf(mysql_service_mysql_runtime_error,
                                    ER_UDF_ERROR, 0, "profiler",
                                    "this function doesn't require any parameter");
    return true;
  }
  const char* name = "utf8mb4";
  char *value = const_cast<char*>(name);
  initid->ptr = const_cast<char *>(udf_init);
  if (mysql_service_mysql_udf_metadata->result_set(
          initid, "charset",
          const_cast<char *>(value))) {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "failed to set result charset");
    return false;
  }
  return false;
}

static void memprof_start_udf_deinit(__attribute__((unused))
                                       UDF_INIT *initid) {
  assert(initid->ptr == udf_init || initid->ptr == my_udf);
}

const char *memprof_start_udf(UDF_INIT *, UDF_ARGS *, char *outp,
                                unsigned long *length, char *is_null,
                                char *error) {
  *error = 0;
  *is_null = 0;
 
  MYSQL_THD thd;

  mysql_service_mysql_current_thread_reader->get(&thd);
  if (!have_required_privilege(thd))
  {
    mysql_error_service_printf(
        ER_SPECIFIC_ACCESS_DENIED_ERROR, 0,
        PRIVILEGE_NAME);
    *error = 1;
    *is_null = 1;
    return 0;
  }

  char variable_value[1024];
  char *p_variable_value;
  size_t value_length = sizeof(variable_value) - 1;

  p_variable_value = &variable_value[0];
  if (mysql_service_profiler_var->get("dump_path", p_variable_value, &value_length)) {
    mysql_error_service_emit_printf(mysql_service_mysql_runtime_error,
                                    ER_UDF_ERROR, 0, "profiler",
                                    "Impossible to get the value of the global variable profiler.dump_path");
    *error = 1;
    *is_null = 1;
    return 0;
  }
  memprof_dump_path = variable_value;
  std::string filePath = memprof_dump_path + ".0001.heap";
  // Check if there is something already existing
  if (fileExists(filePath)) {
    mysql_error_service_emit_printf(mysql_service_mysql_runtime_error,
                                    ER_UDF_ERROR, 0, "profiler",
                                    "There is already a heap dump, change the 'profiler.dump_path' value first.");
    *error = 1;
    *is_null = 1;
    return 0;
  }

  HeapProfilerStart(memprof_dump_path.c_str());

  strcpy(memprof_status, "RUNNING");

  strcpy(outp, "memory profiling started");
  *length = strlen(outp);

  return const_cast<char *>(outp);
}

// UDF to stop the memory profiling

static bool memprof_stop_udf_init(UDF_INIT *initid, UDF_ARGS *args, char *) {
  if (args->arg_count > 0) {
    mysql_error_service_emit_printf(mysql_service_mysql_runtime_error,
                                    ER_UDF_ERROR, 0, "profiler",
                                    "this function doesn't require any parameter");
    return true;
  }
  const char* name = "utf8mb4";
  char *value = const_cast<char*>(name);
  initid->ptr = const_cast<char *>(udf_init);
  if (mysql_service_mysql_udf_metadata->result_set(
          initid, "charset",
          const_cast<char *>(value))) {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "failed to set result charset");
    return false;
  }
  return false;
}

static void memprof_stop_udf_deinit(__attribute__((unused))
                                       UDF_INIT *initid) {
  assert(initid->ptr == udf_init || initid->ptr == my_udf);
}

const char *memprof_stop_udf(UDF_INIT *, UDF_ARGS *, char *outp,
                                unsigned long *length, char *is_null,
                                char *error) {
  *error = 0;
  *is_null = 0;

  MYSQL_THD thd;

  mysql_service_mysql_current_thread_reader->get(&thd);
  if (!have_required_privilege(thd))
  {
    mysql_error_service_printf(
        ER_SPECIFIC_ACCESS_DENIED_ERROR, 0,
        PRIVILEGE_NAME);
    *error = 1;
    *is_null = 1;
    return 0;
  }

  HeapProfilerStop();

  strcpy(memprof_status, "STOPPED");

  strcpy(outp, "memory profiling stopped");
  *length = strlen(outp);

  return const_cast<char *>(outp);
}


// UDF to dump the memory profiling collected data

static bool memprof_dump_udf_init(UDF_INIT *initid, UDF_ARGS *args, char *) {
  if (args->arg_count > 1) {
    mysql_error_service_emit_printf(mysql_service_mysql_runtime_error,
                                    ER_UDF_ERROR, 0, "profiler",
                                    "this function requires none or 1 parameter");
    return true;
  }
  const char* name = "utf8mb4";
  char *value = const_cast<char*>(name);
  initid->ptr = const_cast<char *>(udf_init);
  if (mysql_service_mysql_udf_metadata->result_set(
          initid, "charset",
          const_cast<char *>(value))) {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "failed to set result charset");
    return false;
  }
  return false;
}

static void memprof_dump_udf_deinit(__attribute__((unused))
                                       UDF_INIT *initid) {
  assert(initid->ptr == udf_init || initid->ptr == my_udf);
}

const char *memprof_dump_udf(UDF_INIT *, UDF_ARGS *args, char *outp,
                                unsigned long *length, char *is_null,
                                char *error) {
  *error = 0;
  *is_null = 0;

  MYSQL_THD thd;

  char buf[1024]="";

  mysql_service_mysql_current_thread_reader->get(&thd);
  if (!have_required_privilege(thd))
  {
    mysql_error_service_printf(
        ER_SPECIFIC_ACCESS_DENIED_ERROR, 0,
        PRIVILEGE_NAME);
    *error = 1;
    *is_null = 1;
    return 0;
  }

  // Todo: we need to check if there is already collected data
  //       with the same path.
  //       If yes we stop

  if (args->arg_count > 0) {
	  strncpy(buf, args->args[0], args->lengths[0]);
  } else {
          strcpy(buf, "user request");
  }


  if (IsHeapProfilerRunning()) {
  	strcpy(memprof_status, "RUNNING");
        HeapProfilerDump(buf);
        strcpy(outp, "memory profiling data dumped");
  } else {
  	strcpy(memprof_status, "STOPPED");
        strcpy(outp, "memory profiling is not started");
  }

  *length = strlen(outp);

  return const_cast<char *>(outp);
}

// UDF to run pprof for memory 

static bool pprof_mem_udf_init(UDF_INIT *initid, UDF_ARGS *args, char *) {
  if (args->arg_count > 1) {
    mysql_error_service_emit_printf(mysql_service_mysql_runtime_error,
                                    ER_UDF_ERROR, 0, "profiler",
                                    "this function requires none or 1 parameter: 'text' or 'dot'");
    return true;
  }

  const char* name = "utf8mb4";
  char *value = const_cast<char*>(name);
  initid->ptr = const_cast<char *>(udf_init);
  if (mysql_service_mysql_udf_metadata->result_set(
          initid, "charset",
          const_cast<char *>(value))) {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "failed to set result charset");
    return false;
  }
  return false;
}

static void pprof_mem_udf_deinit(__attribute__((unused))
                                       UDF_INIT *initid) {
  assert(initid->ptr == udf_init || initid->ptr == my_udf);
}

const char *pprof_mem_udf(UDF_INIT *, UDF_ARGS *args, char *outp,
                                unsigned long *length, char *is_null,
                                char *error) {
  *error = 0;
  *is_null = 0;

  MYSQL_THD thd;

  mysql_service_mysql_current_thread_reader->get(&thd);
  if (!have_required_privilege(thd))
  {
    mysql_error_service_printf(
        ER_SPECIFIC_ACCESS_DENIED_ERROR, 0,
        PRIVILEGE_NAME);
    *error = 1;
    *is_null = 1;
    return 0;
  }

  std::string report_type;
  if (args->arg_count < 1) {
          report_type = "text";
  } else {
          report_type = args->args[0];
          if (strcasecmp(report_type.c_str(), "TEXT") == 0) {
                  report_type = "text";
          } else if (strcasecmp(report_type.c_str(), "DOT") == 0) {
                  report_type = "dot";
          } else {
		mysql_error_service_emit_printf(mysql_service_mysql_runtime_error,
                                    ER_UDF_ERROR, 0, "profiler",
                                    "wrong parameter it must be 'TEXT' or 'DOT'.");
    		*error = 1;
    		*is_null = 1;
    		return 0;
          }
  }


  if (IsHeapProfilerRunning()) {
    mysql_error_service_emit_printf(mysql_service_mysql_runtime_error,
                                    ER_UDF_ERROR, 0, "profiler",
                                    "memory profiler is still running, you need to stop it first.");
    *error = 1;
    *is_null = 1;
    return 0;
  }

  std::string mysqld_binary;
  if (get_mysqld(&mysqld_binary)) {
    mysql_error_service_emit_printf(mysql_service_mysql_runtime_error,
                                    ER_UDF_ERROR, 0, "profiler",
                                    "could not find the mysqld binary.");
    *error = 1;
    *is_null = 1;
    return 0;
  }
  LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, "Turiship1");
  char variable_value[1024];
  char *p_variable_value;
  size_t value_length = sizeof(variable_value) - 1;

  p_variable_value = &variable_value[0];
  LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, "Turiship2");
  if (mysql_service_profiler_var->get("pprof_binary", p_variable_value, &value_length)) {
    mysql_error_service_emit_printf(mysql_service_mysql_runtime_error,
                                    ER_UDF_ERROR, 0, "profiler",
                                    "Impossible to get the value of the global variable profiler.pprof_binary");
    *error = 1;
    *is_null = 1;
    return 0;
  }
  LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, "Turiship3");
  LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, p_variable_value);

  std::string buf; 
  buf = exec_pprof((std::string(p_variable_value) + " --" +  report_type + " "
                 + mysqld_binary + " " + memprof_dump_path + "*.heap").c_str());

  LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, "Turiship4");
  outp = (char *)malloc(buf.length() + 1); 
  if (outp == nullptr) {
      *error = 1;
      *is_null = 1;
      return nullptr;
  }
  strcpy(outp, buf.c_str());
  *length = strlen(outp);

  return const_cast<char *>(outp);
}



} /* namespace udf_impl */

static mysql_service_status_t profiler_memory_service_init() {
  mysql_service_status_t result = 0;

  log_bi = mysql_service_log_builtins;
  log_bs = mysql_service_log_builtins_string;

  //Todo check is thre is a value already if not set the default


  LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, "initializing…");

  list = new udf_list();

  if (list->add_scalar("MEMPROF_START", Item_result::STRING_RESULT,
                       (Udf_func_any)udf_impl::memprof_start_udf,
                       udf_impl::memprof_start_udf_init,
                       udf_impl::memprof_start_udf_deinit)) {
    delete list;
    return 1; /* failure: one of the UDF registrations failed */
  }
  LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
                    "new UDF 'memprof_start()' has been registered successfully.");

  if (list->add_scalar("MEMPROF_STOP", Item_result::STRING_RESULT,
                       (Udf_func_any)udf_impl::memprof_stop_udf,
                       udf_impl::memprof_stop_udf_init,
                       udf_impl::memprof_stop_udf_deinit)) {
    delete list;
    return 1; /* failure: one of the UDF registrations failed */
  }
  LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
                    "new UDF 'memprof_stop()' has been registered successfully.");

  if (list->add_scalar("MEMPROF_DUMP", Item_result::STRING_RESULT,
                       (Udf_func_any)udf_impl::memprof_dump_udf,
                       udf_impl::memprof_dump_udf_init,
                       udf_impl::memprof_dump_udf_deinit)) {
    delete list;
    return 1; /* failure: one of the UDF registrations failed */
  }
  LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
                    "new UDF 'memprof_dump()' has been registered successfully.");

  if (list->add_scalar("MEMPROF_REPORT", Item_result::STRING_RESULT,
                       (Udf_func_any)udf_impl::pprof_mem_udf,
                       udf_impl::pprof_mem_udf_init,
                       udf_impl::pprof_mem_udf_deinit)) {
    delete list;
    return 1; /* failure: one of the UDF registrations failed */
  }
  LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
                    "new UDF 'memprof_report()' has been registered successfully.");

  register_status_variables();

  return result;
}

static mysql_service_status_t profiler_memory_service_deinit() {
  mysql_service_status_t result = 0;

  if (list->unregister()) return 1; /* failure: some UDFs still in use */

  delete list;
  if (mysql_service_component_sys_variable_unregister->unregister_variable(
              "profiler", "dump_path")) {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
              "could not unregister variable 'profiler.dump_path'.");
  } else {
    LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
              "variable 'profiler.dump_path' is now unregistered successfully.");
  }
  if (mysql_service_component_sys_variable_unregister->unregister_variable(
              "profiler", "pprof_binary")) {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
              "could not unregister variable 'profiler.pprof_binary'.");
  } else {
    LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
              "variable 'profiler.pprof_binary' is now unregistered successfully.");
  }

  unregister_status_variables();

  LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, "uninstalled.");

  return result;
}

BEGIN_COMPONENT_PROVIDES(profiler_memory_service)
END_COMPONENT_PROVIDES();

BEGIN_COMPONENT_REQUIRES(profiler_memory_service)
    REQUIRES_SERVICE(log_builtins), 
    REQUIRES_SERVICE(log_builtins_string),
    REQUIRES_SERVICE(mysql_thd_security_context),
    REQUIRES_SERVICE(mysql_security_context_options),
    REQUIRES_SERVICE(component_sys_variable_register),
    REQUIRES_SERVICE(component_sys_variable_unregister),
    REQUIRES_SERVICE(status_variable_registration),
    REQUIRES_SERVICE(global_grants_check),
    REQUIRES_SERVICE(mysql_current_thread_reader),
    REQUIRES_SERVICE(mysql_udf_metadata), 
    REQUIRES_SERVICE(udf_registration),
    REQUIRES_SERVICE(mysql_runtime_error), 
    REQUIRES_SERVICE(mysql_system_variable_reader),
    REQUIRES_SERVICE(profiler_var),
END_COMPONENT_REQUIRES();

/* A list of metadata to describe the Component. */
BEGIN_COMPONENT_METADATA(profiler_memory_service)
METADATA("mysql.author", "Oracle Corporation / lefred"),
    METADATA("mysql.license", "GPL"), METADATA("mysql.dev", "lefred"),
    END_COMPONENT_METADATA();

/* Declaration of the Component. */
DECLARE_COMPONENT(profiler_memory_service, "mysql:profiler_memory_service")
profiler_memory_service_init,
    profiler_memory_service_deinit END_DECLARE_COMPONENT();

/* Defines list of Components contained in this library. Note that for now
  we assume that library will have exactly one Component. */
DECLARE_LIBRARY_COMPONENTS &COMPONENT_REF(profiler_memory_service)
    END_DECLARE_LIBRARY_COMPONENTS
