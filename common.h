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

#include <mysql/components/component_implementation.h>
#include <mysql/components/services/log_builtins.h> /* LogComponentErr */
#include <mysql/components/services/component_sys_var_service.h>
#include <mysql/components/services/mysql_current_thread_reader.h>
#include <mysql/components/services/component_sys_var_service.h>
#include <mysql/components/services/mysql_system_variable.h>
#include <mysql/components/services/component_status_var_service.h>
#include <mysql/components/services/security_context.h>
#include <mysql/components/services/mysql_runtime_error_service.h>
#include <mysql/components/services/udf_metadata.h>
#include <mysql/components/services/udf_registration.h>
#include <mysqld_error.h> /* Errors */

#include <list>
#include <sstream>
#include <string>
#include <array>

#include "sql/sql_udf.h"

#include <iostream>
#include <filesystem>
#include <fstream>
#include <unistd.h> // For access()

#include "mysql_version.h" /* MYSQL_VERSION_ID */

extern REQUIRES_SERVICE_PLACEHOLDER(log_builtins);
extern REQUIRES_SERVICE_PLACEHOLDER(log_builtins_string);
extern REQUIRES_SERVICE_PLACEHOLDER(udf_registration);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_udf_metadata);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_thd_security_context);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_current_thread_reader);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_security_context_options);
extern REQUIRES_SERVICE_PLACEHOLDER(global_grants_check);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_runtime_error);
extern REQUIRES_SERVICE_PLACEHOLDER(status_variable_registration);
extern REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_register);
extern REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_unregister);
#if MYSQL_VERSION_ID >= 90000
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_system_variable_reader);
#endif

extern SERVICE_TYPE(log_builtins) * log_bi;
extern SERVICE_TYPE(log_builtins_string) * log_bs;

#define PRIVILEGE_NAME "SENSITIVE_VARIABLES_OBSERVER"

extern bool have_required_privilege(void *opaque_thd);
extern bool isExecutable(const std::string& path);
extern bool canExecute(const std::string& path);
extern bool fileExists(const std::string& path);
extern bool canWriteToPath(const std::string& path);
extern bool parentDirectoryExists(const std::string& filePath);
extern std::string exec_pprof(const char* cmd);
extern bool get_profiler_variable(const char* variable_name, std::string* output);
extern bool get_mysqld(std::string* output); 
extern std::string limit_lines(const std::string& input, size_t max_lines);
