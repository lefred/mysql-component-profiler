
#ifndef PROFILER_SERVICE_H
#define PROFILER_SERVICE_H
    
#include <mysql/components/service.h>


BEGIN_SERVICE_DEFINITION(profiler_var)
DECLARE_BOOL_METHOD(get, (const char *szName, char *szOutValue, size_t *inoutSize));
END_SERVICE_DEFINITION(profiler_var)

#endif /* PROFILER_SERVICE_H */
