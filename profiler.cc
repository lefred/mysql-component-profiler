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
#include "profiler_service.h"


REQUIRES_SERVICE_PLACEHOLDER(log_builtins);
REQUIRES_SERVICE_PLACEHOLDER(log_builtins_string);
REQUIRES_SERVICE_PLACEHOLDER(mysql_thd_security_context);
REQUIRES_SERVICE_PLACEHOLDER(mysql_current_thread_reader);
REQUIRES_SERVICE_PLACEHOLDER(mysql_security_context_options);
REQUIRES_SERVICE_PLACEHOLDER(global_grants_check);
REQUIRES_SERVICE_PLACEHOLDER(mysql_runtime_error);
REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_register);
REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_unregister);
REQUIRES_SERVICE_PLACEHOLDER(mysql_system_variable_reader);


SERVICE_TYPE(log_builtins) * log_bi;
SERVICE_TYPE(log_builtins_string) * log_bs;


static const char *DEFAULT_MEMPROF_DUMP_PATH = "/tmp/mysql.memprof";
static const char *DEFAULT_PPROF_PATH = "/usr/bin/pprof";

// Buffer for the value of the memprof.dump_path global variable
static char *memprof_dump_path_value;
// Buffer for the value of the memprof.pprof_path global variable
static char *pprof_path_value;

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
  
  return result;
}

static mysql_service_status_t profiler_service_deinit() {
  mysql_service_status_t result = 0;

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

  memprof_dump_path_value = nullptr;
  pprof_path_value = nullptr;

  LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, "uninstalled.");

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

BEGIN_SERVICE_IMPLEMENTATION(profiler, profiler_var)
get, END_SERVICE_IMPLEMENTATION();


BEGIN_COMPONENT_PROVIDES(profiler_service)
  PROVIDES_SERVICE(profiler, profiler_var),
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
    REQUIRES_SERVICE(mysql_system_variable_reader),
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
