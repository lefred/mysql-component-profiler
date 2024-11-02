#include "profiler.h"
#include "profiler_service.h"
#include "profiler_pfs.h"

#include <array>

REQUIRES_SERVICE_PLACEHOLDER(pfs_plugin_table_v1);
REQUIRES_SERVICE_PLACEHOLDER_AS(pfs_plugin_column_string_v2, pfs_string);
REQUIRES_SERVICE_PLACEHOLDER_AS(pfs_plugin_column_timestamp_v2, pfs_timestamp);


/*
  DATA
*/

static std::array<Profiler_record *, PROFILER_MAX_ROWS> profiler_array;

/* Next available index for new record to be stored in global record array. */
static size_t profiler_next_available_index = 0;

void init_profiler_data() {
  mysql_mutex_lock(&LOCK_profiler_data);
  profiler_next_available_index = 0;
  profiler_array.fill(nullptr);
  mysql_mutex_unlock(&LOCK_profiler_data);
}

void cleanup_profiler_data() {
  mysql_mutex_lock(&LOCK_profiler_data);
  for (Profiler_record *profiler : profiler_array) {
    delete profiler;
  }
  mysql_mutex_unlock(&LOCK_profiler_data);
}

/*
  DATA collection
*/

void addProfiler_element(time_t profiler_timestamp,
                      std::string profiler_filename,
                      std::string profiler_type,
                      std::string profiler_allocator,
                      std::string profiler_action
                      ) {
  size_t index;
  Profiler_record *record;

  mysql_mutex_lock(&LOCK_profiler_data);

  index = profiler_next_available_index++ % PROFILER_MAX_ROWS;
  record = profiler_array[index];

  if (record != nullptr) {
    delete record;
  }

  record = new Profiler_record;
  record->profiler_timestamp = profiler_timestamp;
  record->profiler_filename = profiler_filename;
  record->profiler_type = profiler_type;
  record->profiler_allocator = profiler_allocator;
  record->profiler_action = profiler_action;

  profiler_array[index] = record;

  mysql_mutex_unlock(&LOCK_profiler_data);
}

/*
  DATA access (performance schema table)
*/

/* Collection of table shares to be added to performance schema */
PFS_engine_table_share_proxy *share_list[1] = {nullptr};
unsigned int share_list_count = 1;

/* Global share pointer for a table */
PFS_engine_table_share_proxy profiler_st_share;

int profiler_delete_all_rows(void) {
  cleanup_profiler_data();
  return 0;
}

PSI_table_handle *profiler_open_table(PSI_pos **pos) {
  Profiler_Table_Handle *temp = new Profiler_Table_Handle();
  *pos = (PSI_pos *)(&temp->m_pos);
  return (PSI_table_handle *)temp;
}

void profiler_close_table(PSI_table_handle *handle) {
  Profiler_Table_Handle *temp = (Profiler_Table_Handle *)handle;
  delete temp;
}

static void copy_record_profiler(Profiler_record *dest, const Profiler_record *source) {
  dest->profiler_timestamp = source->profiler_timestamp;
  dest->profiler_filename = source->profiler_filename;
  dest->profiler_type = source->profiler_type;
  dest->profiler_allocator = source->profiler_allocator;
  dest->profiler_action = source->profiler_action;
  return;
}

/* Define implementation of PFS_engine_table_proxy. */
int profiler_rnd_next(PSI_table_handle *handle) {
  Profiler_Table_Handle *h = (Profiler_Table_Handle *)handle;
  h->m_pos.set_at(&h->m_next_pos);
  size_t index = h->m_pos.get_index();

  if (index < profiler_array.size()) {
    Profiler_record *record = profiler_array[index];
    if (record != nullptr) {
      /* Make the current row from records_array buffer */
      copy_record_profiler(&h->current_row, record);
      h->m_next_pos.set_after(&h->m_pos);
      return 0;
    }
  }

  return PFS_HA_ERR_END_OF_FILE;
}

int profiler_rnd_init(PSI_table_handle *, bool) { return 0; }

/* Set position of a cursor on a specific index */
int profiler_rnd_pos(PSI_table_handle *handle) {
  Profiler_Table_Handle *h = (Profiler_Table_Handle *)handle;
  size_t index = h->m_pos.get_index();

  if (index < profiler_array.size()) {
    Profiler_record *record = profiler_array[index];

    if (record != nullptr) {
      /* Make the current row from records_array buffer */
      copy_record_profiler(&h->current_row, record); 
    }
  }

  return 0;
}

/* Reset cursor position */
void profiler_reset_position(PSI_table_handle *handle) {
  Profiler_Table_Handle *h = (Profiler_Table_Handle *)handle;
  h->m_pos.reset();
  h->m_next_pos.reset();
  return;
}

/* Read current row from the current_row and display them in the table */
int profiler_read_column_value(PSI_table_handle *handle, PSI_field *field,
                            unsigned int index) {
  Profiler_Table_Handle *h = (Profiler_Table_Handle *)handle;

  switch (index) {
    case 0: /* LOGGED */
      pfs_timestamp->set2(field, (h->current_row.profiler_timestamp * 1000000));
      break;
    case 1: /* ALLOCATOR */
      pfs_string->set_varchar_utf8mb4(field, h->current_row.profiler_allocator.c_str());
      break;
    case 2: /* TYPE */
      pfs_string->set_varchar_utf8mb4(field,
                                      h->current_row.profiler_type.c_str());
      break;
    case 3: /* ACTION */
      pfs_string->set_varchar_utf8mb4(field,
                                      h->current_row.profiler_action.c_str());
      break;
    case 4: /* FILENAME */
      pfs_string->set_varchar_utf8mb4(field,
                                      h->current_row.profiler_filename.c_str());
      break;
    default: /* We should never reach here */
      assert(0);
      break;
  }
  return 0;
}

unsigned long long profiler_get_row_count(void) { return PROFILER_MAX_ROWS; }

void init_profiler_share(PFS_engine_table_share_proxy *share) {
  /* Instantiate and initialize PFS_engine_table_share_proxy */
  share->m_table_name = "profiler_actions";
  share->m_table_name_length = 16;
  share->m_table_definition =
      "`LOGGED` timestamp, `ALLOCATOR` VARCHAR(10), `TYPE` VARCHAR(8), "
      "`ACTION` VARCHAR(8), `FILENAME` VARCHAR(255)";
  share->m_ref_length = sizeof(Profiler_POS);
  share->m_acl = READONLY;
  share->get_row_count = profiler_get_row_count;
  share->delete_all_rows = nullptr; /* READONLY TABLE */

  /* Initialize PFS_engine_table_proxy */
  share->m_proxy_engine_table = {profiler_rnd_next, profiler_rnd_init, profiler_rnd_pos,
                                 nullptr, nullptr, nullptr,
                                 profiler_read_column_value, profiler_reset_position,
                                 /* READONLY TABLE */
                                 nullptr, /* write_column_value */
                                 nullptr, /* write_row_values */
                                 nullptr, /* update_column_value */
                                 nullptr, /* update_row_values */
                                 nullptr, /* delete_row_values */
                                 profiler_open_table, profiler_close_table};
}
