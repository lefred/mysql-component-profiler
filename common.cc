#include "profiler.h"

bool have_required_privilege(void *opaque_thd)
{
  // get the security context of the thread
  Security_context_handle ctx = nullptr;
  if (mysql_service_mysql_thd_security_context->get(opaque_thd, &ctx) || !ctx)
  {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                    "problem trying to get security context");
    return false;
  }

  if (mysql_service_global_grants_check->has_global_grant(
          ctx, PRIVILEGE_NAME, strlen(PRIVILEGE_NAME)))
    return true;

  return false;
}

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

bool get_profiler_variable(const char* variable_name, std::string* output) {

    char variable_value[1024];
    char *p_variable_value;
    size_t value_length = sizeof(variable_value) - 1;

    p_variable_value = &variable_value[0];
    
    #if MYSQL_VERSION_ID >= 90000
    if(mysql_service_mysql_system_variable_reader->get(
              nullptr, "GLOBAL", "profiler", variable_name,
              (void **)&p_variable_value, &value_length)) {
            LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "Failed to read the profiler variable");
            return 1;
    }
    #else
    if (mysql_service_component_sys_variable_register->get_variable(
          "profiler", variable_name, (void **)&p_variable_value, &value_length)) {
            LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "Failed to read the profiler variable");
            return 1;
    }
    #endif
 
    *output = variable_value;
    return 0;
}

bool get_mysqld(std::string* output) {
    if (output == nullptr) {
	    return 1;
    }

    char pid_file[1024];
    char *p_pid_file; 
    size_t value_length = sizeof(pid_file) - 1;
 
    p_pid_file = &pid_file[0];

    #if MYSQL_VERSION_ID >= 90000
    if(mysql_service_mysql_system_variable_reader->get(
              nullptr, "GLOBAL", "mysql_server", "pid_file", 
              (void **)&p_pid_file, &value_length)) {
            LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "Failed to read pid_file system variable");
            return 1;
    }
    #else
    if (mysql_service_component_sys_variable_register->get_variable(
          "mysql_server", "pid_file", (void **)&p_pid_file, &value_length)) {
            LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "Failed to read pid_file system variable");
            return 1;
    }
    #endif

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
