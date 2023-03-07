#include "db_impl.h"

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>

#include "logging/log_buffer.h"
#include "port/port.h"
#include "util/autovector.h"

#include "atomic"
#include "base_db_listener.h"
#include "blob_file_builder.h"
#include "blob_file_iterator.h"
#include "blob_file_size_collector.h"
#include "blob_gc.h"
#include "db_iter.h"
#include "iostream"
#include "table_factory.h"
#include "titan_build_version.h"

extern std::atomic<uint64_t> bytes_written;
extern std::atomic<uint64_t> gc_update_lsm;
extern std::atomic<uint64_t> gc_read_lsm;
extern std::atomic<uint64_t> gc_write_value;
extern std::atomic<uint64_t> gc_total;
extern std::atomic<uint64_t> gc_sample;
extern std::atomic<uint64_t> blob_merge_time;
extern std::atomic<uint64_t> blob_read_time;
extern std::atomic<uint64_t> blob_add_time;
extern std::atomic<uint64_t> blob_finish_time;
extern std::atomic<uint64_t> foreground_blob_add_time;
extern std::atomic<uint64_t> foreground_blob_finish_time;
extern std::atomic<uint64_t> compute_gc_score;
extern std::atomic<uint64_t> gc_write_blob;
extern std::atomic<uint64_t> waitflush;
extern std::atomic<uint64_t> skipMergeforStall;

std::atomic<uint64_t> range_merge_file{0};
std::atomic<uint64_t> gc_mark_file{0};

namespace rocksdb {
namespace titandb {

// ThreadPool* TitanDBIterator::pool_ = new ThreadPool(32);

class TitanDBImpl::FileManager : public BlobFileManager {
 public:
  FileManager(TitanDBImpl* db) : db_(db) {}

  Status NewFile(std::unique_ptr<BlobFileHandle>* handle) override {
    return NewFile(handle, db_->env_options_);
  }

  Status NewFile(std::unique_ptr<BlobFileHandle>* handle,
                 const EnvOptions& options) override {
    auto number = db_->blob_file_set_->NewFileNumber();
    auto name = BlobFileName(db_->dirname_, number);

    Status s;
    std::unique_ptr<WritableFileWriter> file;
    {
      std::unique_ptr<WritableFile> f;
      s = db_->env_->NewWritableFile(name, &f, options);
      if (!s.ok()) return s;
      file.reset(new WritableFileWriter(std::move(f), name, options));
    }

    handle->reset(new FileHandle(number, name, std::move(file)));
    {
      MutexLock l(&db_->mutex_);
      db_->pending_outputs_.insert(number);
    }
    return s;
  }

  Status BatchFinishFiles(
      uint32_t cf_id,
      const std::vector<std::pair<std::shared_ptr<BlobFileMeta>,
                                  std::unique_ptr<BlobFileHandle>>>& files)
      override {
    Status s;
    VersionEdit edit;
    edit.SetColumnFamilyID(cf_id);
    for (auto& file : files) {
      // RecordTick(db_->stats_.get(), BLOB_DB_BLOB_FILE_SYNCED);
      {
        StopWatch sync_sw(db_->env_, db_->stats_.get(),
                          BLOB_DB_BLOB_FILE_SYNC_MICROS);
        s = file.second->GetFile()->Sync(false);
      }
      if (s.ok()) {
        s = file.second->GetFile()->Close();
      }
      if (!s.ok()) return s;

      ROCKS_LOG_INFO(db_->db_options_.info_log,
                     "Titan adding blob file [%" PRIu64 "] range [%s, %s]",
                     file.first->file_number(),
                     file.first->smallest_key().c_str(),
                     file.first->largest_key().c_str());
      edit.AddBlobFile(file.first);
    }

    {
      MutexLock l(&db_->mutex_);
      s = db_->blob_file_set_->LogAndApply(edit);
      if (!s.ok()) {
        db_->SetBGError(s);
      }
      for (const auto& file : files)
        db_->pending_outputs_.erase(file.second->GetNumber());
    }
    return s;
  }

  Status BatchDeleteFiles(
      const std::vector<std::unique_ptr<BlobFileHandle>>& handles) override {
    Status s;
    uint64_t file_size = 0;
    for (auto& handle : handles) {
      s = db_->env_->DeleteFile(handle->GetName());
      file_size += handle->GetFile()->GetFileSize();
    }
    {
      MutexLock l(&db_->mutex_);
      for (const auto& handle : handles)
        db_->pending_outputs_.erase(handle->GetNumber());
    }
    return s;
  }

 private:
  class FileHandle : public BlobFileHandle {
   public:
    FileHandle(uint64_t number, const std::string& name,
               std::unique_ptr<WritableFileWriter> file)
        : number_(number), name_(name), file_(std::move(file)) {}

    uint64_t GetNumber() const override { return number_; }

    const std::string& GetName() const override { return name_; }

    WritableFileWriter* GetFile() const override { return file_.get(); }

   private:
    uint64_t number_;
    std::string name_;
    std::unique_ptr<WritableFileWriter> file_;
  };

  TitanDBImpl* db_;
};

TitanDBImpl::TitanDBImpl(const TitanDBOptions& options,
                         const std::string& dbname)
    : bg_cv_(&mutex_),
      dbname_(dbname),
      env_(options.env),
      env_options_(options),
      db_options_(options),
      size_cv_(&size_mutex_) {
  if (db_options_.dirname.empty()) {
    db_options_.dirname = dbname_ + "/titandb";
  }
  dirname_ = db_options_.dirname;
  if (db_options_.statistics != nullptr) {
    stats_.reset(new TitanStats(db_options_.statistics.get()));
  }
  blob_manager_.reset(new FileManager(this));
}

TitanDBImpl::~TitanDBImpl() {
  DumpStats();
  Close();
  std::cerr<<"done"<<std::endl;
}

void TitanDBImpl::StartBackgroundTasks() {
  if (thread_purge_obsolete_ == nullptr &&
      db_options_.purge_obsolete_files_period_sec > 0) {
    thread_purge_obsolete_.reset(new rocksdb::RepeatableThread(
        [this]() { TitanDBImpl::PurgeObsoleteFiles(); }, "titanbg", env_,
        db_options_.purge_obsolete_files_period_sec * 1000 * 1000));
  }
  if (thread_dump_stats_ == nullptr &&
      db_options_.titan_stats_dump_period_sec > 0) {
    thread_dump_stats_.reset(new rocksdb::RepeatableThread(
        [this]() { TitanDBImpl::DumpStats(); }, "titanst", env_,
        db_options_.titan_stats_dump_period_sec * 1000 * 1000));
  }
}

Status TitanDBImpl::ValidateOptions(
    const TitanDBOptions& options,
    const std::vector<TitanCFDescriptor>& column_families) const {
  return Status::OK();
}

Status TitanDBImpl::Open(const std::vector<TitanCFDescriptor>& descs,
                         std::vector<ColumnFamilyHandle*>* handles) {
  Status s = ValidateOptions(db_options_, descs);
  if (!s.ok()) {
    return s;
  }
  // Sets up directories for base DB and Titan.
  s = env_->CreateDirIfMissing(dbname_);
  if (!s.ok()) return s;
  if (!db_options_.info_log) {
    s = CreateLoggerFromOptions(dbname_, db_options_, &db_options_.info_log);
    if (!s.ok()) return s;
  }
  s = env_->CreateDirIfMissing(dirname_);
  if (!s.ok()) return s;
  s = env_->LockFile(LockFileName(dirname_), &lock_);
  if (!s.ok()) return s;

  // Descriptors for initial DB open to get CF ids.
  std::vector<ColumnFamilyDescriptor> init_descs;
  // Descriptors for actually open DB.
  std::vector<ColumnFamilyDescriptor> base_descs;
  for (auto& desc : descs) {
    init_descs.emplace_back(desc.name, desc.options);
    base_descs.emplace_back(desc.name, desc.options);
  }
  std::map<uint32_t, TitanCFOptions> column_families;

  // Opens the base DB first to collect the column families information
  //
  // Disable compaction at this point because we haven't add table properties
  // collector. A compaction can generate a SST file without blob size table
  // property. A later compaction after Titan DB open can cause crash because
  // OnCompactionCompleted use table property to discover blob files generated
  // by the compaction, and get confused by missing property.
  //
  // We also avoid flush here because we haven't replaced the table factory
  // yet, but rocksdb may still flush if memtable is full. This is fine though,
  // since values in memtable are raw values.
  for (auto& desc : init_descs) {
    desc.options.disable_auto_compactions = true;
  }
  db_options_.avoid_flush_during_recovery = true;
  // Add EventListener to collect statistics for GC
  db_options_.listeners.emplace_back(std::make_shared<BaseDbListener>(this));
  // Note that info log is initialized after `CreateLoggerFromOptions`,
  // so new `BlobFileSet` here but not in constructor is to get a proper info
  // log.
  blob_file_set_.reset(new BlobFileSet(db_options_, stats_.get()));

  s = DB::Open(db_options_, dbname_, init_descs, handles, &db_);
  if (s.ok()) {
    for (size_t i = 0; i < descs.size(); i++) {
      auto handle = (*handles)[i];
      uint32_t cf_id = handle->GetID();
      std::string cf_name = handle->GetName();
      column_families.emplace(cf_id, descs[i].options);
      db_->DestroyColumnFamilyHandle(handle);
      // Replaces the provided table factory with TitanTableFactory.
      // While we need to preserve original table_factory for GetOptions.
      auto& base_table_factory = base_descs[i].options.table_factory;
      assert(base_table_factory != nullptr);
      auto titan_table_factory = std::make_shared<TitanTableFactory>(
          db_options_, descs[i].options, blob_manager_, &mutex_,
          blob_file_set_.get(), stats_.get());
      cf_info_.emplace(cf_id,
                       TitanColumnFamilyInfo(
                           {cf_name, ImmutableTitanCFOptions(descs[i].options),
                            MutableTitanCFOptions(descs[i].options),
                            base_table_factory, titan_table_factory}));
      // builders_.emplace(cf_id,
      // ForegroundBuilder(cf_id,
      // blob_manager_,blob_file_set_->GetBlobStorage(cf_id) ,db_options_,
      // descs[i].options));
      base_descs[i].options.table_factory = titan_table_factory;
      // Add TableProperties for collecting statistics GC
      base_descs[i].options.table_properties_collector_factories.emplace_back(
          std::make_shared<BlobFileSizeCollectorFactory>());
    }
    handles->clear();
    s = db_->Close();
    delete db_;
    db_ = nullptr;
  }
  if (!s.ok()) return s;

  if (stats_.get()) {
    stats_->Initialize(column_families);
  }

  s = blob_file_set_->Open(column_families);
  if (db_options_.sep_before_flush) {
    for (auto cf : column_families) {
      builders_.emplace(
          cf.first, ForegroundBuilder(cf.first, blob_manager_,
                                      blob_file_set_->GetBlobStorage(cf.first),
                                      db_options_, cf.second, stats_.get()));
      builders_[cf.first].Init();
    }
  }
  if (!s.ok()) return s;

  // Initialize GC thread pool.
  if (!db_options_.disable_background_gc && db_options_.max_background_gc > 0) {
    env_->IncBackgroundThreadsIfNeeded(db_options_.max_background_gc,
                                       Env::Priority::USER);
  }

  s = DB::Open(db_options_, dbname_, base_descs, handles, &db_);
  if (s.ok()) {
    db_impl_ = reinterpret_cast<DBImpl*>(db_->GetRootDB());
    ROCKS_LOG_INFO(db_options_.info_log, "Titan DB open.");
    ROCKS_LOG_HEADER(db_options_.info_log, "Titan git sha: %s",
                     titan_build_git_sha);
    db_options_.Dump(db_options_.info_log.get());
    for (auto& desc : descs) {
      ROCKS_LOG_HEADER(db_options_.info_log,
                       "Column family [%s], options:", desc.name.c_str());
      desc.options.Dump(db_options_.info_log.get());
    }
  } else {
    ROCKS_LOG_ERROR(db_options_.info_log, "Titan DB open failed: %s",
                    s.ToString().c_str());
  }
  return s;
}

Status TitanDBImpl::Close() {
  Status s;
  std::cerr<<"close impl\n";
  PurgeObsoleteFiles();
  CloseImpl();
  if (db_) {
    std::cerr<<"close db_\n";
    s = db_->Close();
    std::cerr<<"close db_ done\n";
    delete db_;
    db_ = nullptr;
    db_impl_ = nullptr;
  }
  for (auto& builder : builders_) {
    std::cerr<<"finish"<<std::endl;
    builder.second.Finish();
  }
  if (lock_) {
    std::cerr<<"unlock file\n";
    env_->UnlockFile(lock_);
    lock_ = nullptr;
  }
  std::cerr<<"close done\n";
  return s;
}

Status TitanDBImpl::CloseImpl() {
  {
    std::cerr<<"lock in close impl"<<std::endl;
    MutexLock l(&mutex_);
    MutexLock l1(&size_mutex_);
    std::cerr<<"got lock"<<std::endl;
    // Although `shuting_down_` is atomic bool object, we should set it under
    // the protection of mutex_, otherwise, there maybe something wrong with it,
    // like:
    // 1, A thread: shuting_down_.load = false
    // 2, B thread: shuting_down_.store(true)
    // 3, B thread: unschedule all bg work
    // 4, A thread: schedule bg work
    shuting_down_.store(true, std::memory_order_release);
    block_for_size_.store(false);
    size_cv_.SignalAll();
  }

  int gc_unscheduled = env_->UnSchedule(this, Env::Priority::USER);
  {
    MutexLock l(&mutex_);
    bg_gc_scheduled_ -= gc_unscheduled;
    while (bg_gc_scheduled_ > 0) {
      std::cerr<<"wait gc\n";
      bg_cv_.Wait();
      std::cerr<<"wait gc end\n";
    }
  }

  if (thread_purge_obsolete_ != nullptr) {
    thread_purge_obsolete_->cancel();
    mutex_.Lock();
    thread_purge_obsolete_.reset();
    mutex_.Unlock();
  }

  return Status::OK();
}

Status TitanDBImpl::CreateColumnFamilies(
    const std::vector<TitanCFDescriptor>& descs,
    std::vector<ColumnFamilyHandle*>* handles) {
  std::vector<ColumnFamilyDescriptor> base_descs;
  std::vector<std::shared_ptr<TableFactory>> base_table_factory;
  std::vector<std::shared_ptr<TitanTableFactory>> titan_table_factory;
  for (auto& desc : descs) {
    ColumnFamilyOptions options = desc.options;
    // Replaces the provided table factory with TitanTableFactory.
    base_table_factory.emplace_back(options.table_factory);
    titan_table_factory.emplace_back(std::make_shared<TitanTableFactory>(
        db_options_, desc.options, blob_manager_, &mutex_, blob_file_set_.get(),
        stats_.get()));
    options.table_factory = titan_table_factory.back();
    options.table_properties_collector_factories.emplace_back(
        std::make_shared<BlobFileSizeCollectorFactory>());
    base_descs.emplace_back(desc.name, options);
  }

  Status s = db_impl_->CreateColumnFamilies(base_descs, handles);
  assert(handles->size() == descs.size());

  if (s.ok()) {
    std::map<uint32_t, TitanCFOptions> column_families;
    {
      MutexLock l(&mutex_);
      for (size_t i = 0; i < descs.size(); i++) {
        ColumnFamilyHandle* handle = (*handles)[i];
        uint32_t cf_id = handle->GetID();
        column_families.emplace(cf_id, descs[i].options);
        cf_info_.emplace(
            cf_id,
            TitanColumnFamilyInfo(
                {handle->GetName(), ImmutableTitanCFOptions(descs[i].options),
                 MutableTitanCFOptions(descs[i].options), base_table_factory[i],
                 titan_table_factory[i]}));
      }
      blob_file_set_->AddColumnFamilies(column_families);
    }
  }
  if (s.ok()) {
    for (auto& desc : descs) {
      ROCKS_LOG_INFO(db_options_.info_log, "Created column family [%s].",
                     desc.name.c_str());
      desc.options.Dump(db_options_.info_log.get());
    }
  } else {
    std::string column_families_str;
    for (auto& desc : descs) {
      column_families_str += "[" + desc.name + "]";
    }
    ROCKS_LOG_ERROR(db_options_.info_log,
                    "Failed to create column families %s: %s",
                    column_families_str.c_str(), s.ToString().c_str());
  }
  return s;
}

Status TitanDBImpl::DropColumnFamilies(
    const std::vector<ColumnFamilyHandle*>& handles) {
  TEST_SYNC_POINT("TitanDBImpl::DropColumnFamilies:Begin");
  std::vector<uint32_t> column_families;
  std::string column_families_str;
  for (auto& handle : handles) {
    column_families.emplace_back(handle->GetID());
    column_families_str += "[" + handle->GetName() + "]";
  }
  {
    MutexLock l(&mutex_);
    drop_cf_requests_++;

    // Has to wait till no GC job is running before proceed, otherwise GC jobs
    // can fail and set background error.
    // TODO(yiwu): only wait for GC jobs of CFs being dropped.
    while (bg_gc_running_ > 0) {
      bg_cv_.Wait();
    }
  }
  TEST_SYNC_POINT_CALLBACK("TitanDBImpl::DropColumnFamilies:BeforeBaseDBDropCF",
                           nullptr);
  Status s = db_impl_->DropColumnFamilies(handles);
  if (s.ok()) {
    MutexLock l(&mutex_);
    SequenceNumber obsolete_sequence = db_impl_->GetLatestSequenceNumber();
    s = blob_file_set_->DropColumnFamilies(column_families, obsolete_sequence);
    drop_cf_requests_--;
    if (drop_cf_requests_ == 0) {
      bg_cv_.SignalAll();
    }
  }
  if (s.ok()) {
    ROCKS_LOG_INFO(db_options_.info_log, "Dropped column families: %s",
                   column_families_str.c_str());
  } else {
    ROCKS_LOG_ERROR(db_options_.info_log,
                    "Failed to drop column families %s: %s",
                    column_families_str.c_str(), s.ToString().c_str());
  }
  return s;
}

Status TitanDBImpl::DestroyColumnFamilyHandle(
    ColumnFamilyHandle* column_family) {
  if (column_family == nullptr) {
    return Status::InvalidArgument("Column family handle is nullptr.");
  }
  auto cf_id = column_family->GetID();
  std::string cf_name = column_family->GetName();
  Status s = db_impl_->DestroyColumnFamilyHandle(column_family);

  if (s.ok()) {
    MutexLock l(&mutex_);
    // it just changes some marks and doesn't delete blob files physically.
    Status destroy_status = blob_file_set_->MaybeDestroyColumnFamily(cf_id);
    // BlobFileSet will return NotFound status if the cf is not destroyed.
    if (destroy_status.ok()) {
      assert(cf_info_.count(cf_id) > 0);
      cf_info_.erase(cf_id);
    }
  }
  if (s.ok()) {
    ROCKS_LOG_INFO(db_options_.info_log, "Destroyed column family handle [%s].",
                   cf_name.c_str());
  } else {
    ROCKS_LOG_ERROR(db_options_.info_log,
                    "Failed to destroy column family handle [%s]: %s",
                    cf_name.c_str(), s.ToString().c_str());
  }
  return s;
}

Status TitanDBImpl::CompactFiles(
    const CompactionOptions& compact_options, ColumnFamilyHandle* column_family,
    const std::vector<std::string>& input_file_names, const int output_level,
    const int output_path_id, std::vector<std::string>* const output_file_names,
    CompactionJobInfo* compaction_job_info) {
  if (HasBGError()) return GetBGError();
  std::unique_ptr<CompactionJobInfo> compaction_job_info_ptr;
  if (compaction_job_info == nullptr) {
    compaction_job_info_ptr.reset(new CompactionJobInfo());
    compaction_job_info = compaction_job_info_ptr.get();
  }
  auto s = db_impl_->CompactFiles(
      compact_options, column_family, input_file_names, output_level,
      output_path_id, output_file_names, compaction_job_info);
  if (s.ok()) {
    OnCompactionCompleted(*compaction_job_info);
  }

  return s;
}

int TitanDBImpl::Scan(const ReadOptions& options, const std::string& start_key,
                      int len, std::vector<std::string>& keys,
                      std::vector<std::string>& vals) {
  if ((int)keys.size() < len) keys.resize(len);
  if ((int)vals.size() < len) vals.resize(len);
  auto iter = this->NewIterator(options);
  int ret = len;
  static_cast<TitanDBIterator*>(iter)->Scan(start_key, ret, keys, vals);
  delete iter;
  return ret;
}

Status TitanDBImpl::Put(const rocksdb::WriteOptions& options,
                        rocksdb::ColumnFamilyHandle* column_family,
                        const rocksdb::Slice& key,
                        const rocksdb::Slice& value) {
  if (HasBGError()) return GetBGError();
  // std::cerr<<"block write size is "<<db_options_.block_write_size<<".\n";
  while (db_options_.block_write_size>0 && block_for_size_.load()){
    std::cerr<<"blocked by size_cv\n";
      {
        uint32_t cf_id = column_family->GetID();
        auto bs = blob_file_set_->GetBlobStorage(cf_id).lock();
        bs->ComputeGCScore();
        AddToGCQueue(cf_id);
        MaybeScheduleGC();
      }
    MutexLock l(&size_mutex_);
    if(block_for_size_.load()){
    size_cv_.Wait();
    std::cerr<<"wait done\n";
    }
  }
  // if (db_options_.sep_before_flush && value.size() >
  // cf_info_[column_family->GetID()].immutable_cf_options.mid_blob_size) {
  if (db_options_.sep_before_flush) {
    auto wb = WriteBatch();
    if (builders_[column_family->GetID()].Add(key, value, &wb).ok()) {
      return db_->Write(options, &wb);
    }
    return db_->Put(options, column_family, key, value);
  } else {
    return db_->Put(options, column_family, key, value);
  }
}

Status TitanDBImpl::Write(const rocksdb::WriteOptions& options,
                          rocksdb::WriteBatch* updates) {
  return HasBGError() ? GetBGError() : db_->Write(options, updates);
}

Status TitanDBImpl::Delete(const rocksdb::WriteOptions& options,
                           rocksdb::ColumnFamilyHandle* column_family,
                           const rocksdb::Slice& key) {
  return HasBGError() ? GetBGError() : db_->Delete(options, column_family, key);
}

Status TitanDBImpl::IngestExternalFile(
    rocksdb::ColumnFamilyHandle* column_family,
    const std::vector<std::string>& external_files,
    const rocksdb::IngestExternalFileOptions& options) {
  return HasBGError()
             ? GetBGError()
             : db_->IngestExternalFile(column_family, external_files, options);
}

Status TitanDBImpl::CompactRange(const rocksdb::CompactRangeOptions& options,
                                 rocksdb::ColumnFamilyHandle* column_family,
                                 const rocksdb::Slice* begin,
                                 const rocksdb::Slice* end) {
  return HasBGError() ? GetBGError()
                      : db_->CompactRange(options, column_family, begin, end);
}

Status TitanDBImpl::Flush(const rocksdb::FlushOptions& options,
                          rocksdb::ColumnFamilyHandle* column_family) {
  return HasBGError() ? GetBGError() : db_->Flush(options, column_family);
}

Status TitanDBImpl::Get(const ReadOptions& options, ColumnFamilyHandle* handle,
                        const Slice& key, PinnableSlice* value) {
  if (options.snapshot) {
    return GetImpl(options, handle, key, value);
  }
  ReadOptions ro(options);
  ManagedSnapshot snapshot(this);
  ro.snapshot = snapshot.snapshot();
  return GetImpl(ro, handle, key, value);
}

Status TitanDBImpl::GetImpl(const ReadOptions& options,
                            ColumnFamilyHandle* handle, const Slice& key,
                            PinnableSlice* value) {
  Status s;
  bool is_blob_index = false;
  s = db_impl_->GetImpl(options, handle, key, value, nullptr /*value_found*/,
                        nullptr /*read_callback*/, &is_blob_index);
  if (!s.ok() || !is_blob_index) return s;

  StopWatch get_sw(env_, stats_.get(), BLOB_DB_GET_MICROS);
  // RecordTick(stats_.get(), BLOB_DB_NUM_GET);

  BlobIndex index;
  s = index.DecodeFrom(value);
  assert(s.ok());
  if (!s.ok()) return s;

  BlobRecord record;
  PinnableSlice buffer;

  mutex_.Lock();
  auto storage = blob_file_set_->GetBlobStorage(handle->GetID()).lock();
  mutex_.Unlock();

  if (storage) {
    StopWatch read_sw(env_, stats_.get(), BLOB_DB_BLOB_FILE_READ_MICROS);
    s = storage->Get(options, index, &record, &buffer);
    // RecordTick(stats_.get(), BLOB_DB_NUM_KEYS_READ);
    // RecordTick(stats_.get(), BLOB_DB_BLOB_FILE_BYTES_READ,
    //  index.blob_handle.size);
  } else {
    ROCKS_LOG_ERROR(db_options_.info_log,
                    "Column family id:%" PRIu32 " not Found.", handle->GetID());
    return Status::NotFound(
        "Column family id: " + std::to_string(handle->GetID()) + " not Found.");
  }
  if (s.IsCorruption()) {
    ROCKS_LOG_ERROR(db_options_.info_log,
                    "Key:%s Snapshot:%" PRIu64 " GetBlobFile err:%s\n",
                    key.ToString(true).c_str(),
                    options.snapshot->GetSequenceNumber(),
                    s.ToString().c_str());
  }
  if (s.ok()) {
    value->Reset();
    value->PinSelf(record.value);
  }
  return s;
}

std::vector<Status> TitanDBImpl::MultiGet(
    const ReadOptions& options, const std::vector<ColumnFamilyHandle*>& handles,
    const std::vector<Slice>& keys, std::vector<std::string>* values) {
  auto options_copy = options;
  options_copy.total_order_seek = true;
  if (options_copy.snapshot) {
    return MultiGetImpl(options_copy, handles, keys, values);
  }
  ReadOptions ro(options_copy);
  ManagedSnapshot snapshot(this);
  ro.snapshot = snapshot.snapshot();
  return MultiGetImpl(ro, handles, keys, values);
}

std::vector<Status> TitanDBImpl::MultiGetImpl(
    const ReadOptions& options, const std::vector<ColumnFamilyHandle*>& handles,
    const std::vector<Slice>& keys, std::vector<std::string>* values) {
  std::vector<Status> res;
  res.resize(keys.size());
  values->resize(keys.size());
  for (size_t i = 0; i < keys.size(); i++) {
    auto value = &(*values)[i];
    PinnableSlice pinnable_value(value);
    res[i] = GetImpl(options, handles[i], keys[i], &pinnable_value);
    if (res[i].ok() && pinnable_value.IsPinned()) {
      value->assign(pinnable_value.data(), pinnable_value.size());
    }
  }
  return res;
}

Iterator* TitanDBImpl::NewIterator(const TitanReadOptions& options,
                                   ColumnFamilyHandle* handle) {
  TitanReadOptions options_copy = options;
  options_copy.total_order_seek = true;
  std::shared_ptr<ManagedSnapshot> snapshot;
  if (options_copy.snapshot) {
    return NewIteratorImpl(options_copy, handle, snapshot);
  }
  TitanReadOptions ro(options_copy);
  snapshot.reset(new ManagedSnapshot(this));
  ro.snapshot = snapshot->snapshot();
  return NewIteratorImpl(ro, handle, snapshot);
}

Iterator* TitanDBImpl::NewIteratorImpl(
    const TitanReadOptions& options, ColumnFamilyHandle* handle,
    std::shared_ptr<ManagedSnapshot> snapshot) {
  auto cfd = reinterpret_cast<ColumnFamilyHandleImpl*>(handle)->cfd();

  mutex_.Lock();
  auto storage = blob_file_set_->GetBlobStorage(handle->GetID()).lock();
  mutex_.Unlock();

  if (!storage) {
    ROCKS_LOG_ERROR(db_options_.info_log,
                    "Column family id:%" PRIu32 " not Found.", handle->GetID());
    return nullptr;
  }

  std::unique_ptr<ArenaWrappedDBIter> iter(db_impl_->NewIteratorImpl(
      options, cfd, options.snapshot->GetSequenceNumber(),
      nullptr /*read_callback*/, true /*allow_blob*/, true /*allow_refresh*/));
  return new TitanDBIterator(options, storage.get(), snapshot, std::move(iter),
                             env_, stats_.get(), db_options_.info_log.get());
}

Status TitanDBImpl::NewIterators(
    const TitanReadOptions& options,
    const std::vector<ColumnFamilyHandle*>& handles,
    std::vector<Iterator*>* iterators) {
  TitanReadOptions ro(options);
  ro.total_order_seek = true;
  std::shared_ptr<ManagedSnapshot> snapshot;
  if (!ro.snapshot) {
    snapshot.reset(new ManagedSnapshot(this));
    ro.snapshot = snapshot->snapshot();
  }
  iterators->clear();
  iterators->reserve(handles.size());
  for (auto& handle : handles) {
    iterators->emplace_back(NewIteratorImpl(ro, handle, snapshot));
  }
  return Status::OK();
}

const Snapshot* TitanDBImpl::GetSnapshot() { return db_->GetSnapshot(); }

void TitanDBImpl::ReleaseSnapshot(const Snapshot* snapshot) {
  // TODO:
  // We can record here whether the oldest snapshot is released.
  // If not, we can just skip the next round of purging obsolete files.
  db_->ReleaseSnapshot(snapshot);
}

Status TitanDBImpl::DeleteFilesInRanges(ColumnFamilyHandle* column_family,
                                        const RangePtr* ranges, size_t n,
                                        bool include_end) {
  TablePropertiesCollection props;
  auto cfh = reinterpret_cast<ColumnFamilyHandleImpl*>(column_family);
  auto cfd = cfh->cfd();
  Version* version = nullptr;

  // Increment the ref count
  {
    InstrumentedMutexLock l(db_impl_->mutex());
    version = cfd->current();
    version->Ref();
  }

  auto* vstorage = version->storage_info();
  for (size_t i = 0; i < n; i++) {
    auto begin = ranges[i].start, end = ranges[i].limit;
    // Get all the files within range except L0, cause `DeleteFilesInRanges`
    // would not delete the files in L0.
    for (int level = 1; level < vstorage->num_non_empty_levels(); level++) {
      if (vstorage->LevelFiles(i).empty() ||
          !vstorage->OverlapInLevel(i, begin, end)) {
        continue;
      }
      std::vector<FileMetaData*> level_files;
      InternalKey begin_storage, end_storage, *begin_key, *end_key;
      if (begin == nullptr) {
        begin_key = nullptr;
      } else {
        begin_storage.SetMinPossibleForUserKey(*begin);
        begin_key = &begin_storage;
      }
      if (end == nullptr) {
        end_key = nullptr;
      } else {
        end_storage.SetMaxPossibleForUserKey(*end);
        end_key = &end_storage;
      }

      std::vector<FileMetaData*> files;
      vstorage->GetCleanInputsWithinInterval(level, begin_key, end_key, &files,
                                             -1 /* hint_index */,
                                             nullptr /* file_index */);
      for (const auto& file_meta : files) {
        if (file_meta->being_compacted) {
          continue;
        }
        if (!include_end && end != nullptr &&
            cfd->user_comparator()->Compare(file_meta->largest.user_key(),
                                            *end) == 0) {
          continue;
        }
        auto fname =
            TableFileName(cfd->ioptions()->cf_paths, file_meta->fd.GetNumber(),
                          file_meta->fd.GetPathId());
        if (props.count(fname) == 0) {
          std::shared_ptr<const TableProperties> table_properties;
          Status s =
              version->GetTableProperties(&table_properties, file_meta, &fname);
          if (s.ok() && table_properties) {
            props.insert({fname, table_properties});
          } else {
            return s;
          }
        }
      }
    }
  }

  // Decrement the ref count
  {
    InstrumentedMutexLock l(db_impl_->mutex());
    version->Unref();
  }

  auto cf_id = column_family->GetID();
  std::map<uint64_t, uint64_t> blob_files_discardable_size;
  for (auto& collection : props) {
    auto& prop = collection.second;
    auto ucp_iter = prop->user_collected_properties.find(
        BlobFileSizeCollector::kPropertiesName);
    // this sst file doesn't contain any blob index
    if (ucp_iter == prop->user_collected_properties.end()) {
      continue;
    }
    std::map<uint64_t, uint64_t> sst_blob_files_size;
    std::string str = ucp_iter->second;
    Slice slice{str};
    if (!BlobFileSizeCollector::Decode(&slice, &sst_blob_files_size)) {
      // TODO: Should treat it as background error and make DB read-only.
      ROCKS_LOG_ERROR(db_options_.info_log,
                      "failed to decode table property, "
                      "deleted file: %s, property size: %" ROCKSDB_PRIszt ".",
                      collection.first.c_str(), str.size());
      assert(false);
      continue;
    }

    for (auto& it : sst_blob_files_size) {
      blob_files_discardable_size[it.first] += it.second;
    }
  }

  // Here could be a running compaction install a new version after obtain
  // current and before we call DeleteFilesInRange for the base DB. In this case
  // the properties we get could be inaccurate.
  // TODO: we can use the OnTableFileDeleted callback after adding table
  // property field to TableFileDeletionInfo.
  Status s =
      db_impl_->DeleteFilesInRanges(column_family, ranges, n, include_end);
  if (!s.ok()) return s;

  MutexLock l(&mutex_);
  SequenceNumber obsolete_sequence = db_impl_->GetLatestSequenceNumber();
  s = blob_file_set_->DeleteBlobFilesInRanges(cf_id, ranges, n, include_end,
                                              obsolete_sequence);
  if (!s.ok()) return s;

  auto bs = blob_file_set_->GetBlobStorage(cf_id).lock();
  if (!bs) {
    // TODO: Should treat it as background error and make DB read-only.
    ROCKS_LOG_ERROR(db_options_.info_log,
                    "Column family id:%" PRIu32 " not Found.", cf_id);
    return Status::NotFound("Column family id: " + std::to_string(cf_id) +
                            " not Found.");
  }

  uint64_t delta = 0;
  VersionEdit edit;
  auto cf_options = bs->cf_options();
  for (const auto& bfs : blob_files_discardable_size) {
    auto file = bs->FindFile(bfs.first).lock();
    if (!file) {
      // file has been gc out
      continue;
    }
    if (!file->is_obsolete()) {
      delta += bfs.second;
    }
    auto before = file->GetDiscardableRatioLevel();
    file->AddDiscardableSize(static_cast<uint64_t>(bfs.second));
    auto after = file->GetDiscardableRatioLevel();
    if (before != after) {
      AddStats(stats_.get(), cf_id, after, 1);
      SubStats(stats_.get(), cf_id, before, 1);
    }
    if (cf_options.level_merge) {
      if (file->NoLiveData()) {
        edit.DeleteBlobFile(file->file_number(),
                            db_impl_->GetLatestSequenceNumber());
      } else if (file->GetDiscardableRatio() >
                 ((int)file->file_level() == cf_options.num_levels - 1
                      ? cf_options.blob_file_discardable_ratio
                      : cf_options.high_level_blob_discardable_ratio)) {
        edit.UpdateBlobFile(bfs.first, bfs.second);
        file->FileStateTransit(BlobFileMeta::FileEvent::kNeedGC);
      } else if (bfs.second > 0) {
        edit.UpdateBlobFile(bfs.first, bfs.second);
      }
    }
  }
  SubStats(stats_.get(), cf_id, TitanInternalStats::LIVE_BLOB_SIZE, delta);
  if (cf_options.level_merge) {
    blob_file_set_->LogAndApply(edit);
  } else {
    bs->ComputeGCScore();

    AddToGCQueue(cf_id);
    MaybeScheduleGC();
  }

  return s;
}

void TitanDBImpl::MarkFileIfNeedMerge(
    const std::vector<std::shared_ptr<BlobFileMeta>>& files,
    int max_sorted_runs) {
  // std::cerr<<"if need merge?"<<files.size()<<"。"<<std::endl;
  mutex_.AssertHeld();
  if (files.empty()) return;

  // store and sort both ends of blob files to count sorted runs
  std::vector<std::pair<BlobFileMeta*, bool /* is smallest end? */>> blob_ends;
  for (const auto& file : files) {
    blob_ends.emplace_back(std::make_pair(file.get(), true));
    blob_ends.emplace_back(std::make_pair(file.get(), false));
  }
  auto blob_ends_cmp = [](const std::pair<BlobFileMeta*, bool>& end1,
                          const std::pair<BlobFileMeta*, bool>& end2) {
    const std::string& key1 =
        end1.second ? end1.first->smallest_key() : end1.first->largest_key();
    const std::string& key2 =
        end2.second ? end2.first->smallest_key() : end2.first->largest_key();
    int cmp = key1.compare(key2);
    // when the key being the same, order largest_key before smallest_key
    if(cmp==0) {
      return end1.first == end2.first? end1.second : (!end1.second && end2.second);
    }
    return cmp < 0;
  };
  std::sort(blob_ends.begin(), blob_ends.end(), blob_ends_cmp);

  int cur_add = 0;
  int cur_remove = 0;
  int size = blob_ends.size();
  int marked = 0;
  std::unordered_map<BlobFileMeta*, int> tmp;
  for (int i = 0; i < size; i++) {
    if (blob_ends[i].second) {
      ++cur_add;
      tmp[blob_ends[i].first] = cur_remove;
    } else {
      ++cur_remove;
      auto record = tmp.find(blob_ends[i].first);
      if(record == tmp.end()){
        std::cout<<"maybe bug in mark range merge\n";
        continue;
      }
      if (cur_add - record->second > max_sorted_runs &&
          record->first->file_state() != BlobFileMeta::FileState::kObsolete &&
          record->first->file_state() != BlobFileMeta::FileState::kToMerge &&
          record->first->file_state() != BlobFileMeta::FileState::kToGC) {
        if(marked==0 && record->first->file_level()==0)
        std::cerr<<"mark file level"<<record->first->file_level()<<std::endl;
        marked++;
        record->first->FileStateTransit(BlobFileMeta::FileEvent::kNeedMerge);
      }
    }
  }
  // std::cerr<<"mark "<<marked<<"files to merge"<<std::endl;
  range_merge_file.fetch_add(marked);
}

Options TitanDBImpl::GetOptions(ColumnFamilyHandle* column_family) const {
  assert(column_family != nullptr);
  Options options = db_->GetOptions(column_family);
  uint32_t cf_id = column_family->GetID();

  MutexLock l(&mutex_);
  if (cf_info_.count(cf_id) > 0) {
    options.table_factory = cf_info_.at(cf_id).base_table_factory;
  } else {
    ROCKS_LOG_ERROR(
        db_options_.info_log,
        "Failed to get original table factory for column family %s.",
        column_family->GetName().c_str());
    options.table_factory.reset();
  }
  return options;
}

Status TitanDBImpl::SetOptions(
    ColumnFamilyHandle* column_family,
    const std::unordered_map<std::string, std::string>& new_options) {
  Status s;
  auto opts = new_options;
  auto p = opts.find("blob_run_mode");
  bool set_blob_run_mode = (p != opts.end());
  TitanBlobRunMode mode = TitanBlobRunMode::kNormal;
  if (set_blob_run_mode) {
    const std::string& blob_run_mode_string = p->second;
    auto pm = blob_run_mode_string_map.find(blob_run_mode_string);
    if (pm == blob_run_mode_string_map.end()) {
      return Status::InvalidArgument("No blob_run_mode defined for " +
                                     blob_run_mode_string);
    } else {
      mode = pm->second;
      ROCKS_LOG_INFO(db_options_.info_log, "[%s] Set blob_run_mode: %s",
                     column_family->GetName().c_str(),
                     blob_run_mode_string.c_str());
    }
    opts.erase(p);
  }
  if (opts.size() > 0) {
    s = db_->SetOptions(column_family, opts);
    if (!s.ok()) {
      return s;
    }
  }
  // Make sure base db's SetOptions success before setting blob_run_mode.
  if (set_blob_run_mode) {
    uint32_t cf_id = column_family->GetID();
    {
      MutexLock l(&mutex_);
      assert(cf_info_.count(cf_id) > 0);
      TitanColumnFamilyInfo& cf_info = cf_info_[cf_id];
      cf_info.titan_table_factory->SetBlobRunMode(mode);
      cf_info.mutable_cf_options.blob_run_mode = mode;
    }
  }
  return Status::OK();
}

TitanOptions TitanDBImpl::GetTitanOptions(
    ColumnFamilyHandle* column_family) const {
  assert(column_family != nullptr);
  Options base_options = GetOptions(column_family);
  TitanOptions titan_options;
  *static_cast<TitanDBOptions*>(&titan_options) = db_options_;
  *static_cast<DBOptions*>(&titan_options) =
      static_cast<DBOptions>(base_options);
  uint32_t cf_id = column_family->GetID();
  {
    MutexLock l(&mutex_);
    assert(cf_info_.count(cf_id) > 0);
    const TitanColumnFamilyInfo& cf_info = cf_info_.at(cf_id);
    *static_cast<TitanCFOptions*>(&titan_options) = TitanCFOptions(
        static_cast<ColumnFamilyOptions>(base_options),
        cf_info.immutable_cf_options, cf_info.mutable_cf_options);
  }
  return titan_options;
}

TitanDBOptions TitanDBImpl::GetTitanDBOptions() const {
  // Titan db_options_ is not mutable after DB open.
  TitanDBOptions result = db_options_;
  *static_cast<DBOptions*>(&result) = db_impl_->GetDBOptions();
  return result;
}

bool TitanDBImpl::GetProperty(ColumnFamilyHandle* column_family,
                              const Slice& property, std::string* value) {
  std::cout << "## write size ##\n";
  std::cout << "blob builder written bytes: " << bytes_written / 1000000.0
            << std::endl;

  std::cout << "\n## unsorted blob file gc ##\n";
  std::cout << "gc update lsm time: " << gc_update_lsm / 1000000.0 << std::endl;
  std::cout << "gc read lsm time: " << gc_read_lsm / 1000000.0 << std::endl;
  std::cout << "gc sample time: " << gc_sample / 1000000.0 << std::endl;
  std::cout << "gc write blob time: " << gc_write_blob / 1000000.0 << std::endl;
  std::cout << "total gc time: " << gc_total / 1000000.0 << std::endl;
  std::cout << "compute gc score time: " << compute_gc_score / 1000000.0
            << std::endl;

  std::cout << "\n## sorted blob file merge ##\n";
  std::cout << "blob merge time: " << blob_merge_time / 1000000.0 << std::endl;
  std::cout << "blob read time: " << blob_read_time / 1000000.0 << std::endl;
  std::cout << "blob add time: " << blob_add_time / 1000000.0 << std::endl;
  std::cout << "blob finish time; " << blob_finish_time / 1000000.0
            << std::endl;

  std::cout << "\n ## marked files ##\n";
  std::cout << "range merge marked file: " << range_merge_file << std::endl;
  std::cout << "gc marked file: " << gc_mark_file << std::endl;

  std::cout << "\n## unsorted blob file build ##\n";
  std::cout << "builder wait flush: " << waitflush / 1000000.0 << std::endl;
  std::cout << "blob add time; "
            << foreground_blob_add_time / 1000000.0 << std::endl;
  std::cout << "blob finish time: "
            << foreground_blob_finish_time / 1000000.0 << std::endl;
  std::cout << "skip merge for stall: "<<skipMergeforStall<<std::endl;

  std::cout << "\n## blob file states in each level ##\n";
  blob_file_set_->PrintFileStates();
  assert(column_family != nullptr);
  bool s = false;
  /*
  if (stats_.get() != nullptr) {
    auto stats = stats_->internal_stats(column_family->GetID());
    if (stats != nullptr) {
      s = stats->GetStringProperty(property, value);
    }
  }
  */
  if (s) {
    return s;
  } else {
    return db_impl_->GetProperty(column_family, property, value);
  }
}

bool TitanDBImpl::GetIntProperty(ColumnFamilyHandle* column_family,
                                 const Slice& property, uint64_t* value) {
  assert(column_family != nullptr);
  bool s = false;
  if (stats_.get() != nullptr) {
    auto stats = stats_->internal_stats(column_family->GetID());
    if (stats != nullptr) {
      s = stats->GetIntProperty(property, value);
    }
  }
  if (s) {
    return s;
  } else {
    return db_impl_->GetIntProperty(column_family, property, value);
  }
}

void TitanDBImpl::OnMemTableSealed() {
  if (db_options_.sep_before_flush) {
    for (auto& builder : builders_) {
      builder.second.Flush();
    }
  }
}

void TitanDBImpl::OnFlushCompleted(const FlushJobInfo& flush_job_info) {
  const auto& tps = flush_job_info.table_properties;
  // std::cerr <<"file name"<<  flush_job_info.file_path<< "data size: "<< tps.data_size <<"index size: " << tps.index_size <<std::endl;
  auto ucp_iter = tps.user_collected_properties.find(
      BlobFileSizeCollector::kPropertiesName);
  // sst file doesn't contain any blob index
  if (ucp_iter == tps.user_collected_properties.end()) {
    return;
  }
  std::map<uint64_t, uint64_t> blob_files_size;
  Slice src{ucp_iter->second};
  if (!BlobFileSizeCollector::Decode(&src, &blob_files_size)) {
    // TODO: Should treat it as background error and make DB read-only.
    ROCKS_LOG_ERROR(db_options_.info_log,
                    "OnFlushCompleted[%d]: failed to decode table property, "
                    "property size: %" ROCKSDB_PRIszt ".",
                    flush_job_info.job_id, ucp_iter->second.size());
    assert(false);
  }
  assert(!blob_files_size.empty());

  {
    MutexLock l(&mutex_);
    auto blob_storage =
        blob_file_set_->GetBlobStorage(flush_job_info.cf_id).lock();
    if (!blob_storage) {
      // TODO: Should treat it as background error and make DB read-only.
      ROCKS_LOG_ERROR(db_options_.info_log,
                      "OnFlushCompleted[%d]: Column family id: %" PRIu32
                      " Not Found.",
                      flush_job_info.job_id, flush_job_info.cf_id);
      assert(false);
      return;
    }
    uint64_t delta = 0;
    for (const auto& f : blob_files_size) {
      auto file = blob_storage->FindFile(f.first).lock();
      // This file maybe output of a gc job, and it's been GCed out.
      if (!file) {
        continue;
      }
      ROCKS_LOG_INFO(db_options_.info_log,
                     "OnFlushCompleted[%d]: output blob file %" PRIu64 ".",
                     flush_job_info.job_id, file->file_number());
      uint64_t discardable = 0;

      if (file->discardable_size() == 0) {
        discardable = file->file_size() - f.second -
                                 kBlobHeaderSize - kBlobFooterSize;
      } else {
        discardable = -f.second;
      }
      delta += discardable;
      SubStats(stats_.get(), flush_job_info.cf_id, TitanInternalStats::LIVE_BLOB_SIZE, delta);

      file->AddDiscardableSize(discardable);
      if (file->file_type() == kSorted) assert(file->discardable_size() == 0);
      file->FileStateTransit(BlobFileMeta::FileEvent::kFlushCompleted);
    }
  }
  TEST_SYNC_POINT("TitanDBImpl::OnFlushCompleted:Finished");
}

void TitanDBImpl::OnCompactionCompleted(
    const CompactionJobInfo& compaction_job_info) {
  if (!compaction_job_info.status.ok()) {
    // TODO: Clean up blob file generated by the failed compaction.
    return;
  }
  int mark = 0;
  std::map<uint64_t, int64_t> blob_files_size_diff;
  std::set<uint64_t> outputs;
  std::set<uint64_t> inputs;
  auto calc_bfs = [&](const std::vector<std::string>& files, int coefficient,
                      bool output) {
    for (const auto& file : files) {
      auto tp_iter = compaction_job_info.table_properties.find(file);
      if (tp_iter == compaction_job_info.table_properties.end()) {
        if (output) {
          ROCKS_LOG_WARN(
              db_options_.info_log,
              "OnCompactionCompleted[%d]: No table properties for file %s.",
              compaction_job_info.job_id, file.c_str());
        }
        continue;
      }
      auto ucp_iter = tp_iter->second->user_collected_properties.find(
          BlobFileSizeCollector::kPropertiesName);
      // this sst file doesn't contain any blob index
      if (ucp_iter == tp_iter->second->user_collected_properties.end()) {
        continue;
      }
      std::map<uint64_t, uint64_t> input_blob_files_size;
      std::string s = ucp_iter->second;
      Slice slice{s};
      if (!BlobFileSizeCollector::Decode(&slice, &input_blob_files_size)) {
        // TODO: Should treat it as background error and make DB read-only.
        ROCKS_LOG_ERROR(
            db_options_.info_log,
            "OnCompactionCompleted[%d]: failed to decode table property, "
            "compaction file: %s, property size: %" ROCKSDB_PRIszt ".",
            compaction_job_info.job_id, file.c_str(), s.size());
        assert(false);
        continue;
      }
      for (const auto& input_bfs : input_blob_files_size) {
        if (output) {
          if (inputs.find(input_bfs.first) == inputs.end()) {
            outputs.insert(input_bfs.first);
          }
        } else {
          inputs.insert(input_bfs.first);
        }
        auto bfs_iter = blob_files_size_diff.find(input_bfs.first);
        if (bfs_iter == blob_files_size_diff.end()) {
          blob_files_size_diff[input_bfs.first] =
              coefficient * input_bfs.second;
        } else {
          bfs_iter->second += coefficient * input_bfs.second;
        }
      }
    }
  };

  calc_bfs(compaction_job_info.input_files, -1, false);
  calc_bfs(compaction_job_info.output_files, 1, true);

  {
    MutexLock l(&mutex_);
    auto bs = blob_file_set_->GetBlobStorage(compaction_job_info.cf_id).lock();
    if (!bs) {
      // TODO: Should treat it as background error and make DB read-only.
      ROCKS_LOG_ERROR(db_options_.info_log,
                      "OnCompactionCompleted[%d] Column family id:%" PRIu32
                      " not Found.",
                      compaction_job_info.job_id, compaction_job_info.cf_id);
      return;
    }
    for (const auto& file_number : outputs) {
      auto file = bs->FindFile(file_number).lock();
      if (!file) {
        // TODO: Should treat it as background error and make DB read-only.
        ROCKS_LOG_ERROR(
            db_options_.info_log,
            "OnCompactionCompleted[%d]: Failed to get file %" PRIu64,
            compaction_job_info.job_id, file_number);
        assert(false);
        return;
      }
      ROCKS_LOG_INFO(
          db_options_.info_log,
          "OnCompactionCompleted[%d]: compaction output blob file %" PRIu64 ".",
          compaction_job_info.job_id, file->file_number());
      file->FileStateTransit(BlobFileMeta::FileEvent::kCompactionCompleted);
    }

    uint64_t delta = 0;
    VersionEdit edit;
    auto cf_options = bs->cf_options();
    std::vector<std::shared_ptr<BlobFileMeta>> files;
    bool count_sorted_run =
        cf_options.level_merge && cf_options.range_merge &&
        cf_options.num_levels - 2 <= compaction_job_info.output_level;
        /*
    if(count_sorted_run){
    std::cerr<<"num levels "<<cf_options.num_levels<<" output level "<<compaction_job_info.output_level<<" input level "<<compaction_job_info.base_input_level<<std::endl;
    if(cf_options.num_levels - 1 == compaction_job_info.output_level) std::cerr<<compaction_job_info.output_level<<" is "<<cf_options.num_levels - 1<<std::endl;
    else std::cerr<<"no"<<std::endl;
    }*/
    for (const auto& bfs : blob_files_size_diff) {
      // std::cerr<<"total "<<blob_files_size_diff.size()<<"vtables in output level"<<compaction_job_info.output_level<<std::endl;
      // blob file size < 0 means discardable size > 0
      if (bfs.second >= 0) {
        if (count_sorted_run) {
          auto file = bs->FindFile(bfs.first).lock();
          if (file != nullptr && file->file_type() == kSorted && (int)file->file_level()>=cf_options.num_levels-2) {
            files.emplace_back(std::move(file));
          }
        }
        continue;
      }
      auto file = bs->FindFile(bfs.first).lock();
      if (!file) {
        // file has been gc out
        continue;
      }
      if (!file->is_obsolete()) {
        delta += -bfs.second;
      }
      auto before = file->GetDiscardableRatioLevel();
      file->AddDiscardableSize(static_cast<uint64_t>(-bfs.second));
      // std::cerr<<"after add "<<file->GetDiscardableRatio()<<std::endl;
      auto after = file->GetDiscardableRatioLevel();
      if (before != after) {
        AddStats(stats_.get(), compaction_job_info.cf_id, after, 1);
        SubStats(stats_.get(), compaction_job_info.cf_id, before, 1);
      }
      if (cf_options.level_merge) {
        // After level merge, most entries of merged blob files are written to
        // new blob files. Delete blob files which have no live data.
        // Mark last two level blob files to merge in next compaction if
        // discardable size reached GC threshold
        if (file->NoLiveData() && file->file_type() == kSorted) {
          edit.DeleteBlobFile(file->file_number(),
                              db_impl_->GetLatestSequenceNumber());
          continue;
        } else if (file->file_type() == kSorted &&
                   (/*file->GetDiscardableRatio() >
                        cf_options.high_level_blob_discardable_ratio ||
                    (static_cast<int>(file->file_level()) >=
                         cf_options.num_levels - 2 &&*/
                     file->GetDiscardableRatio() >
                   // file->GetOOPSLADiscardableRatio()>1
                         cf_options.blob_file_discardable_ratio
                         )) {
          if(file->file_state() != BlobFileMeta::FileState::kToGC)
            mark++;
          file->FileStateTransit(BlobFileMeta::FileEvent::kNeedGC);
        }
        if (count_sorted_run && file->file_type() == kSorted && (int)file->file_level()>=cf_options.num_levels-2) {
          files.emplace_back(std::move(file));
        }
      }
    }
    SubStats(stats_.get(), compaction_job_info.cf_id,
             TitanInternalStats::LIVE_BLOB_SIZE, delta);
    // If level merge is enabled, blob files will be deleted by live
    // data based GC, so we don't need to trigger regular GC anymore
    if (cf_options.level_merge) {
      blob_file_set_->LogAndApply(edit);
      MarkFileIfNeedMerge(files, cf_options.max_sorted_runs);
    }

    // calculate total invalid ratio of blob files
    uint64_t live_size = 0;
    uint64_t total_size = 0;
    GetIntProperty("rocksdb.titandb.live-blob-size",&live_size);
    GetIntProperty("rocksdb.titandb.live-blob-file-size",&total_size);
    // bool bg_gc = total_size>live_size&& (double)(total_size-live_size)/total_size > cf_options.blob_file_discardable_ratio; // trigger wisckey gc?
    bool wisc_gc = !cf_options.level_merge && total_size > (uint64_t) (100+100*cf_options.blob_file_discardable_ratio)<<30;
    bool bg_gc = cf_options.level_merge&&db_options_.sep_before_flush;
    if(db_options_.block_write_size>0){
    if(total_size>db_options_.block_write_size) {
      block_for_size_.store(true);
    } else if(block_for_size_.load()){
      block_for_size_.store(false);
      size_cv_.SignalAll();
    }
    }

    if (wisc_gc || bg_gc) {
      if (bs->ComputeGCScore() >
          (1<<30) / cf_options.blob_file_target_size || block_for_size_.load()) {
        AddToGCQueue(compaction_job_info.cf_id);
        MaybeScheduleGC();
      }
    }
  }
  gc_mark_file += mark;
}

Status TitanDBImpl::SetBGError(const Status& s) {
  if (s.ok()) return s;
  mutex_.AssertHeld();
  Status bg_err = s;
  if (!db_options_.listeners.empty()) {
    // TODO(@jiayu) : check if mutex_ is freeable for future use case
    mutex_.Unlock();
    for (auto& listener : db_options_.listeners) {
      listener->OnBackgroundError(BackgroundErrorReason::kCompaction, &bg_err);
    }
    mutex_.Lock();
  }
  if (!bg_err.ok()) {
    bg_error_ = bg_err;
    has_bg_error_.store(true);
  }
  return bg_err;
}

void TitanDBImpl::DumpStats() {
  if (stats_ == nullptr) {
    return;
  }
  LogBuffer log_buffer(InfoLogLevel::HEADER_LEVEL, db_options_.info_log.get());
  {
    MutexLock l(&mutex_);
    for (auto& cf : cf_info_) {
      TitanInternalStats* internal_stats = stats_->internal_stats(cf.first);
      if (internal_stats == nullptr) {
        ROCKS_LOG_WARN(db_options_.info_log,
                       "Column family [%s] missing internal stats.",
                       cf.second.name.c_str());
        continue;
      }
      LogToBuffer(&log_buffer, "Titan internal stats for column family [%s]:",
                  cf.second.name.c_str());
      internal_stats->DumpAndResetInternalOpStats(&log_buffer);
    }
  }
  log_buffer.FlushBufferToLog();
}

}  // namespace titandb
}  // namespace rocksdb
