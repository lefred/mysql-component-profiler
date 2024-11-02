
#include <mysql/components/services/mysql_mutex.h>

extern REQUIRES_MYSQL_MUTEX_SERVICE_PLACEHOLDER;

static mysql_mutex_t LOCK_profiler_data;

extern PSI_mutex_key key_mutex_profiler_data;
extern PSI_mutex_info profiler_data_mutex[];
