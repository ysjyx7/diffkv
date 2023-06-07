#include "base_db_listener.h"
#include "rocksdb/listener.h"
#include <vector>

namespace rocksdb {
namespace titandb {

BaseDbListener::BaseDbListener(TitanDBImpl* db) : db_impl_(db) {}

BaseDbListener::~BaseDbListener() {}

void BaseDbListener::OnFlushCompleted(DB* /*db*/,
                                      const FlushJobInfo& flush_job_info) {

  /*
  std::cerr << "OnFlushCompleted [ trigger_write_slowdown: " 
  << flush_job_info.triggered_writes_slowdown 
  << " trigger_write_stop: " << flush_job_info.triggered_writes_stop 
  << " ]" << std::endl; 
  */
  db_impl_->OnFlushCompleted(flush_job_info);
}

void BaseDbListener::OnMemTableSealed(const MemTableInfo& /*info*/) {
  db_impl_->OnMemTableSealed();
}

void BaseDbListener::OnStallConditionsChanged(const WriteStallInfo & info) {
  /*
  auto print_val = [](const WriteStallCondition &wc){ 
    auto val = std::vector<std::string>{"KDelayed", "kNormal", "kStopped"};
     if (wc ==  rocksdb::WriteStallCondition::kDelayed) return val[0];
     if (wc ==  rocksdb::WriteStallCondition::kNormal) return val[1];
     if (wc ==  rocksdb::WriteStallCondition::kStopped) return val[2];
     return val[0];
  };

  std::cerr << "OnStallConditionsChanged [ " << "cur: " 
  << print_val(info.condition.cur) << " -> prev: " 
  << print_val(info.condition.prev) << " ]" 
  << std::endl;
  */
}
void BaseDbListener::OnCompactionCompleted(
    DB* /* db */, const CompactionJobInfo& compaction_job_info) {
  /*
  std::cerr << "OnCompactionCompleted [" <<  ": input level " 
  << compaction_job_info.base_input_level << " -> output level: "
  << compaction_job_info.output_level << " size: "
  << compaction_job_info.output_files.size() << " ]" << std::endl;
  */
  db_impl_->OnCompactionCompleted(compaction_job_info);
}

}  // namespace titandb
}  // namespace rocksdb
