
#ifndef PROFILER_SERVICE_H
#define PROFILER_SERVICE_H
    
#include <mysql/components/service.h>


BEGIN_SERVICE_DEFINITION(profiler_var)
DECLARE_BOOL_METHOD(get, (const char *szName, char *szOutValue, size_t *inoutSize));
END_SERVICE_DEFINITION(profiler_var)

BEGIN_SERVICE_DEFINITION(profiler_pfs)
DECLARE_BOOL_METHOD(add, (const char* profiler_type, const char* profiler_allocator, 
                                const char* profiler_action, const char* profiler_filename,
                                const char* profiler_extra));
END_SERVICE_DEFINITION(profiler_pfs)

#endif /* PROFILER_SERVICE_H */
