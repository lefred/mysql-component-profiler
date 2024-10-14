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

#define LOG_COMPONENT_TAG "profiler"

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

#include "sql/field.h"
#include "sql/sql_udf.h"

#include <gperftools/heap-profiler.h>
#include <gperftools/profiler.h>

#include <iostream>
#include <filesystem>
#include <fstream>
#include <unistd.h> // For access()

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
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_system_variable_reader);


extern SERVICE_TYPE(log_builtins) * log_bi;
extern SERVICE_TYPE(log_builtins_string) * log_bs;

#define PRIVILEGE_NAME "SENSITIVE_VARIABLES_OBSERVER"

bool isExecutable(const std::string& path) {
    // Check if the file has executable permissions for others
    auto perms = std::filesystem::status(path).permissions();
    return (perms & std::filesystem::perms::others_exec) != std::filesystem::perms::none;
}

bool canExecute(const std::string& path) {
    // Use access() with X_OK to check if the file is executable by the current process
    return access(path.c_str(), X_OK) == 0;
}


bool fileExists(const std::string& path) {
    return std::filesystem::exists(path);
}

bool canWriteToPath(const std::string& path) {
    // Try to open a file in write mode to check if the path is writable
    std::ofstream file(path, std::ios::app); // Use std::ios::app to append without truncating
    return file.good();
}

bool parentDirectoryExists(const std::string& filePath) {
    // Get the parent directory of the given file path
    std::filesystem::path parentDir = std::filesystem::path(filePath).parent_path();

    // Check if the parent directory exists
    return std::filesystem::exists(parentDir);
}


std::string exec_pprof(const char* cmd) {
    std::array<char, 128> buffer;
    std::string result;

    std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen(cmd, "r"), pclose);

    if (!pipe) {
        result = "can't run pprof";
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }

    return result;
}

bool get_mysqld(std::string* output) {
    if (output == nullptr) {
	    return 1;
    }

    char pid_file[1024];
    char *p_pid_file; 
    size_t value_length = sizeof(pid_file) - 1;
 
    p_pid_file = &pid_file[0];

    if(mysql_service_mysql_system_variable_reader->get(
	      nullptr, "GLOBAL", "mysql_server", "pid_file", 
              (void **)&p_pid_file, &value_length)) {
	    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "Failed to read pid_file system variable");
	    return 1;
    }

    std::ifstream f(pid_file);
    if (!f.is_open()) {
	    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "Failed to open the pid_file");
	    return 1;
    }

    std::string mysql_pid;
    if (!std::getline(f, mysql_pid)) {
	    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "Failed to read the content of the pid_file");
	    return 1;
    }
    f.close();

    std::ifstream cmdlineFile("/proc/" + mysql_pid + "/cmdline", std::ios::in | std::ios::binary);
    if (!cmdlineFile) {
	LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "Failed to open cmdline proc file");
        return 1;
    }
    std::string mysql_binary;
    std::getline(cmdlineFile, mysql_binary, '\0');
    cmdlineFile.close();

    *output = mysql_binary;
    return 0; 
}
