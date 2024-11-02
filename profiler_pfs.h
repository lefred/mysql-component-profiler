#include <mysql/components/services/pfs_plugin_table_service.h>
#include "profiler_mutex.h"

extern REQUIRES_SERVICE_PLACEHOLDER(pfs_plugin_table_v1);
extern REQUIRES_SERVICE_PLACEHOLDER_AS(pfs_plugin_column_string_v2, pfs_string);
extern REQUIRES_SERVICE_PLACEHOLDER_AS(pfs_plugin_column_timestamp_v2, pfs_timestamp);


/* Maximum number of rows in the table */
#define PROFILER_MAX_ROWS  100

struct Profiler_record {
  time_t profiler_timestamp;
  std::string profiler_type;
  std::string profiler_filename;
  std::string profiler_allocator;
  std::string profiler_action;
};

class Profiler_POS {
 private:
  unsigned int m_index = 0;

 public:
  ~Profiler_POS() = default;
  Profiler_POS() { m_index = 0; }

  void reset() { m_index = 0; }

  unsigned int get_index() { return m_index; }

  void set_at(Profiler_POS *pos) { m_index = pos->m_index; }

  void set_after(Profiler_POS *pos) { m_index = pos->m_index + 1; }
};

struct Profiler_Table_Handle {
  /* Current position instance */
  Profiler_POS m_pos;
  /* Next position instance */
  Profiler_POS m_next_pos;

  /* Current row for the table */
  Profiler_record current_row;

  /* Index indicator */
  unsigned int index_num;
};

extern void addProfiler_element(time_t profiler_timestamp,
                      std::string profiler_filename,
                      std::string profiler_type,
                      std::string profiler_allocator,
                      std::string profiler_action
                      );

extern void updateProfiler_element(std::string profiler_filename,
                      std::string profiler_type,
                      std::string profiler_allocator,
                      std::string profiler_action
                      );

int profiler_prepare_insert_row();

void init_profiler_share(PFS_engine_table_share_proxy *share);
void init_profiler_data();
void cleanup_profiler_data();

extern PFS_engine_table_share_proxy profiler_st_share;

extern PFS_engine_table_share_proxy *share_list[];
extern unsigned int share_list_count;
