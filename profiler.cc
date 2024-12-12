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

#include "profiler.h"
#include "profiler_pfs.h"
#include "profiler_service.h"

REQUIRES_SERVICE_PLACEHOLDER(log_builtins);
REQUIRES_SERVICE_PLACEHOLDER(log_builtins_string);
REQUIRES_SERVICE_PLACEHOLDER(mysql_thd_security_context);
REQUIRES_SERVICE_PLACEHOLDER(mysql_current_thread_reader);
REQUIRES_SERVICE_PLACEHOLDER(mysql_security_context_options);
REQUIRES_SERVICE_PLACEHOLDER(global_grants_check);
REQUIRES_SERVICE_PLACEHOLDER(mysql_runtime_error);
REQUIRES_SERVICE_PLACEHOLDER(udf_registration);
REQUIRES_SERVICE_PLACEHOLDER(mysql_udf_metadata);
REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_register);
REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_unregister);
#if MYSQL_VERSION_ID >= 90000
REQUIRES_SERVICE_PLACEHOLDER(mysql_system_variable_reader);
#endif
REQUIRES_MYSQL_MUTEX_SERVICE_PLACEHOLDER;

SERVICE_TYPE(log_builtins) * log_bi;
SERVICE_TYPE(log_builtins_string) * log_bs;

PSI_mutex_key key_mutex_profiler_data = 0;
PSI_mutex_info profiler_data_mutex[] = {
  {&key_mutex_profiler_data, "profiler_data", PSI_FLAG_SINGLETON, PSI_VOLATILITY_PERMANENT,
     "Profiler data, permanent mutex, singleton."}
}; 

static const char *DEFAULT_MEMPROF_DUMP_PATH = "/tmp/mysql.memprof";
static const char *DEFAULT_PPROF_PATH = "/usr/bin/pprof";

// Buffer for the value of the memprof.dump_path global variable
static char *memprof_dump_path_value;
// Buffer for the value of the memprof.pprof_path global variable
static char *pprof_path_value;

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

bool remove_files_with_prefix(const std::string& directory, const std::string& prefix) {
    namespace fs = std::filesystem;

    try {
        for (const auto& entry : fs::directory_iterator(directory)) {
            if (entry.is_regular_file() && entry.path().filename().string().find(prefix) == 0) {
                fs::remove(entry.path());
            }
        }
    } catch (const fs::filesystem_error& e) {
        mysql_error_service_emit_printf(mysql_service_mysql_runtime_error,
                                    ER_UDF_ERROR, 0, "profiler",
                                    "Error while deleting files.");
        return false; 
    }
    return true;
}

static int memprof_dump_path_check(MYSQL_THD thd,
                                       SYS_VAR *self MY_ATTRIBUTE((unused)),
                                       void *save,
                                       struct st_mysql_value *value) {
  // check if the user has the right privilege to change it
  if (!have_required_privilege(thd)) {
    Security_context_handle ctx = nullptr;
    mysql_service_mysql_thd_security_context->get(thd, &ctx);
    // get the user and host to display in error log
    // has the current user as not access to the pfs table
    MYSQL_LEX_CSTRING user;
    mysql_service_mysql_security_context_options->get(ctx, "priv_user",
                                                        &user);
    MYSQL_LEX_CSTRING host;
    mysql_service_mysql_security_context_options->get(ctx, "priv_host",
                                                        &host);
    char buf[1024];
    sprintf(buf, "user (%s@%s) has no access to "
      "set memprof.dump_path variable "
      "(privilege %s required).", user.str, host.str, PRIVILEGE_NAME);

    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, buf);
    my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), PRIVILEGE_NAME);
    return (ER_SPECIFIC_ACCESS_DENIED_ERROR);
  }

  int value_len = 0;

  // Check if we can write in the folder 
  if (!parentDirectoryExists(value->val_str(value, nullptr, &value_len))) {
    mysql_error_service_emit_printf(mysql_service_mysql_runtime_error,
                                    ER_UDF_ERROR, 0, "memprof",
                                    "we don't have access to write in that folder.");
    return true;
  }

  // Save the string value
  *static_cast<const char **>(save) =
      value->val_str(value, nullptr, &value_len);

  return (0);

}

static void memprof_dump_path_update(MYSQL_THD, SYS_VAR *, void *var_ptr,
                          const void *save) {
  *(const char **)var_ptr =
      *(static_cast<const char **>(const_cast<void *>(save)));
}

static int pprof_path_check(MYSQL_THD thd,
                                       SYS_VAR *self MY_ATTRIBUTE((unused)),
                                       void *save,
                                       struct st_mysql_value *value) {
  // check if the user has the right privilege to change it
  if (!have_required_privilege(thd)) {
    Security_context_handle ctx = nullptr;
    mysql_service_mysql_thd_security_context->get(thd, &ctx);
    // get the user and host to display in error log
    // has the current user as not access to the pfs table
    MYSQL_LEX_CSTRING user;
    mysql_service_mysql_security_context_options->get(ctx, "priv_user",
                                                        &user);
    MYSQL_LEX_CSTRING host;
    mysql_service_mysql_security_context_options->get(ctx, "priv_host",
                                                        &host);
    char buf[1024];
    sprintf(buf, "user (%s@%s) has no access to "
      "set profiler.pprof_path variable "
      "(privilege %s required).", user.str, host.str, PRIVILEGE_NAME);

    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, buf);
    my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), PRIVILEGE_NAME);
    return (ER_SPECIFIC_ACCESS_DENIED_ERROR);
  }

  int value_len = 0;

  // Check if we have pprof and that it can be executed
  if (!isExecutable(value->val_str(value, nullptr, &value_len)) || 
		  !canExecute(value->val_str(value, nullptr, &value_len))) {
    mysql_error_service_emit_printf(mysql_service_mysql_runtime_error,
                                    ER_UDF_ERROR, 0, "profiler",
                                    "pprof is not available or cannot be executed.");
    return true;
  }

  // Save the string value
  *static_cast<const char **>(save) =
      value->val_str(value, nullptr, &value_len);

  return (0);
}

static void pprof_path_update(MYSQL_THD, SYS_VAR *, void *var_ptr,
                          const void *save) {
  *(const char **)var_ptr =
      *(static_cast<const char **>(const_cast<void *>(save)));
}

namespace udf_impl {

const char *udf_init = "udf_init", *my_udf = "my_udf",
           *my_udf_clear = "my_clear", *my_udf_add = "my_udf_add";

static bool profiler_cleanup_udf_init(UDF_INIT *initid, UDF_ARGS *, char *) {
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

static void profiler_cleanup_udf_deinit(__attribute__((unused))
                                       UDF_INIT *initid) {
  assert(initid->ptr == udf_init || initid->ptr == my_udf);
}

const char *profiler_cleanup_udf(UDF_INIT *, UDF_ARGS *, char *outp,
                                unsigned long *length, char *is_null,
                                char *error) {
  
  *error = 0;
  *is_null = 0;
  std::filesystem::path p(memprof_dump_path_value);
  if(remove_files_with_prefix(p.parent_path().string(), p.filename().string())) {
    strcpy(outp, "Profiling data matching has been cleaned up.");
    snprintf(outp, 500, "Profiling data matching %s prefix has been cleaned up.", memprof_dump_path_value);
  } else {
    strcpy(outp, "Error while cleaning up profiling data.");
  }

  *length = strlen(outp);

  return const_cast<char *>(outp);
}

} /* namespace udf_impl */

static mysql_service_status_t profiler_service_init() {
  mysql_service_status_t result = 0;

  log_bi = mysql_service_log_builtins;
  log_bs = mysql_service_log_builtins_string;

  STR_CHECK_ARG(str) memprof_dump_path_arg;
  STR_CHECK_ARG(str1) pprof_path_arg;

  memprof_dump_path_arg.def_val = const_cast<char*>(DEFAULT_MEMPROF_DUMP_PATH);
  memprof_dump_path_value = nullptr;
  pprof_path_arg.def_val = const_cast<char*>(DEFAULT_PPROF_PATH);
  pprof_path_value = nullptr;

  //Todo check is thre is a value already if not set the default

  LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, "initializing…");

  list = new udf_list();

  if (list->add_scalar("PROFILER_CLEANUP", Item_result::STRING_RESULT,
                       (Udf_func_any)udf_impl::profiler_cleanup_udf,
                       udf_impl::profiler_cleanup_udf_init,
                       udf_impl::profiler_cleanup_udf_deinit)) {
    delete list;
    return 1; /* failure: one of the UDF registrations failed */
  }
  LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
                    "new UDF 'profiler_cleanup()' has been registered successfully.");


  // Registration of the global system variable
  if (mysql_service_component_sys_variable_register->register_variable(
          "profiler", "dump_path",
          PLUGIN_VAR_STR | PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
          "Path defining where to dump profiling data", 
	  memprof_dump_path_check, memprof_dump_path_update, 
	  (void *)&memprof_dump_path_arg, (void *)&memprof_dump_path_value)) {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                    "could not register new variable 'profiler.dump_path'.");
    result = 1;
  } else {
    LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
                    "new variable 'profiler.dump_path' has been registered successfully.");
  }

  if (mysql_service_component_sys_variable_register->register_variable(
          "profiler", "pprof_binary",
          PLUGIN_VAR_STR | PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
          "Path of the pprof binary file",
          pprof_path_check, pprof_path_update,
          (void *)&pprof_path_arg, (void *)&pprof_path_value)) {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                    "could not register new variable 'profiler.pprof_binary'.");
    result = 1;
  } else {
    LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
                    "new variable 'profiler.pprof_binary' has been registered successfully.");
  }

  mysql_mutex_init(key_mutex_profiler_data, &LOCK_profiler_data, nullptr);
  init_profiler_share(&profiler_st_share);
  init_profiler_data();
  share_list[0] = &profiler_st_share;
  if (mysql_service_pfs_plugin_table_v1->add_tables(&share_list[0], 
                                                 share_list_count)) {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                    "PFS table has NOT been registered successfully!");
    mysql_mutex_destroy(&LOCK_profiler_data);
    return 1;
  } else{
    LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
                    "PFS table has been registered successfully.");
  }
  
  return result;
}

static mysql_service_status_t profiler_service_deinit() {
  mysql_service_status_t result = 0;

  cleanup_profiler_data();

  if (list->unregister()) return 1; /* failure: some UDFs still in use */

  delete list;

  if (mysql_service_component_sys_variable_unregister->unregister_variable(
              "profiler", "dump_path")) {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
              "could not unregister variable 'profiler.dump_path'.");
    return 1;
  } else {
    LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
              "variable 'profiler.dump_path' is now unregistered successfully.");
  }
  if (mysql_service_component_sys_variable_unregister->unregister_variable(
              "profiler", "pprof_binary")) {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
              "could not unregister variable 'profiler.pprof_binary'.");
    return 1;
  } else {
    LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
              "variable 'profiler.pprof_binary' is now unregistered successfully.");
  }

  if (mysql_service_pfs_plugin_table_v1->delete_tables(&share_list[0],
                                                    share_list_count)) {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                    "Error while trying to remove PFS table");
    return 1;
  } else{
    LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
                    "PFS table has been removed successfully.");
  }

  memprof_dump_path_value = nullptr;
  pprof_path_value = nullptr;

  LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, "uninstalled.");

  mysql_mutex_destroy(&LOCK_profiler_data);


  return result;
}

DEFINE_BOOL_METHOD(get, (const char *szName, char *szOutValue, size_t *inoutSize)) {

   std::string s_profiler_variable;	
   if (get_profiler_variable(szName, &s_profiler_variable)) {
       return true;
   }

   // Check if the output buffer is large enough
   if (*inoutSize < s_profiler_variable.length() + 1) {
       // Update the required size
       *inoutSize = s_profiler_variable.length() + 1;
       return false; // Indicate failure due to insufficient buffer size
   }

   // Copy the string to the output buffer
   strncpy(szOutValue, s_profiler_variable.c_str(), *inoutSize);
   *inoutSize = s_profiler_variable.length(); // Update the size

   return false;
}

DEFINE_BOOL_METHOD(add, (const char* profiler_type, const char* profiler_allocator, 
                                const char* profiler_action, const char* profiler_filename,
                                const char* profiler_extra)) {

  addProfiler_element(time(nullptr), profiler_filename, profiler_type,
                          profiler_allocator, profiler_action, profiler_extra);
  return false;
}

BEGIN_SERVICE_IMPLEMENTATION(profiler, profiler_var)
get, END_SERVICE_IMPLEMENTATION();

BEGIN_SERVICE_IMPLEMENTATION(profiler, profiler_pfs)
add, END_SERVICE_IMPLEMENTATION();


BEGIN_COMPONENT_PROVIDES(profiler_service)
  PROVIDES_SERVICE(profiler, profiler_var),
  PROVIDES_SERVICE(profiler, profiler_pfs),
END_COMPONENT_PROVIDES();

BEGIN_COMPONENT_REQUIRES(profiler_service)
    REQUIRES_SERVICE(log_builtins), 
    REQUIRES_SERVICE(log_builtins_string),
    REQUIRES_SERVICE(mysql_thd_security_context),
    REQUIRES_SERVICE(mysql_security_context_options),
    REQUIRES_SERVICE(component_sys_variable_register),
    REQUIRES_SERVICE(component_sys_variable_unregister),
    REQUIRES_SERVICE(global_grants_check),
    REQUIRES_SERVICE(mysql_current_thread_reader),
    REQUIRES_SERVICE(mysql_runtime_error), 
    REQUIRES_SERVICE(pfs_plugin_table_v1),
    REQUIRES_SERVICE(mysql_udf_metadata), 
    REQUIRES_SERVICE(udf_registration),
    REQUIRES_SERVICE_AS(pfs_plugin_column_string_v2, pfs_string),
    REQUIRES_SERVICE_AS(pfs_plugin_column_timestamp_v2, pfs_timestamp),
#if MYSQL_VERSION_ID >= 90000
    REQUIRES_SERVICE(mysql_system_variable_reader),
#endif
    REQUIRES_MYSQL_MUTEX_SERVICE,
END_COMPONENT_REQUIRES();

/* A list of metadata to describe the Component. */
BEGIN_COMPONENT_METADATA(profiler_service)
METADATA("mysql.author", "Oracle Corporation / lefred"),
    METADATA("mysql.license", "GPL"), METADATA("mysql.dev", "lefred"),
    END_COMPONENT_METADATA();

/* Declaration of the Component. */
DECLARE_COMPONENT(profiler_service, "mysql:profiler_service")
profiler_service_init,
    profiler_service_deinit END_DECLARE_COMPONENT();

/* Defines list of Components contained in this library. Note that for now
  we assume that library will have exactly one Component. */
DECLARE_LIBRARY_COMPONENTS &COMPONENT_REF(profiler_service)
    END_DECLARE_LIBRARY_COMPONENTS
