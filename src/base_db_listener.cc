#include "base_db_listener.h"

namespace rocksdb {
namespace titandb {
bool IsWriteStall;

BaseDbListener::BaseDbListener(TitanDBImpl* db) : db_impl_(db) {}

BaseDbListener::~BaseDbListener() {}

void BaseDbListener::OnFlushCompleted(DB* /*db*/,
                                      const FlushJobInfo& flush_job_info) {
  db_impl_->OnFlushCompleted(flush_job_info);
}

void BaseDbListener::OnMemTableSealed(const MemTableInfo& /*info*/) {
  db_impl_->OnMemTableSealed();
}

void BaseDbListener::OnCompactionCompleted(
    DB* /* db */, const CompactionJobInfo& compaction_job_info) {
  db_impl_->OnCompactionCompleted(compaction_job_info);
}

void BaseDbListener::NotifyWriteStall(){
  IsWriteStall=true;
}

void BaseDbListener::NotifyWriteStallComplete(){
  IsWriteStall=false;
}

}  // namespace titandb
}  // namespace rocksdb
