//
// tablet_impl.cc
// Copyright (C) 2017 4paradigm.com
// Author wangtaize 
// Date 2017-04-01
//

#include "tablet/tablet_impl.h"
#include "tablet/file_sender.h"

#include "config.h"
#include <vector>
#include <stdlib.h>
#include <stdio.h>
#include <gflags/gflags.h>
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#ifdef TCMALLOC_ENABLE 
#include "gperftools/malloc_extension.h"
#endif
#include "base/status.h"
#include "base/codec.h"
#include "base/strings.h"
#include "base/file_util.h"
#include "base/hash.h"
#include "storage/segment.h"
#include "storage/binlog.h"
#include "logging.h"
#include "timer.h"
#include <google/protobuf/text_format.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <thread>

using ::baidu::common::INFO;
using ::baidu::common::WARNING;
using ::baidu::common::DEBUG;
using ::rtidb::storage::Table;
using ::rtidb::storage::DataBlock;

DECLARE_int32(gc_interval);
DECLARE_int32(disk_gc_interval);
DECLARE_int32(gc_pool_size);
DECLARE_int32(statdb_ttl);
DECLARE_uint32(scan_max_bytes_size);
DECLARE_uint32(scan_reserve_size);
DECLARE_double(mem_release_rate);
DECLARE_string(db_root_path);
DECLARE_string(ssd_root_path);
DECLARE_string(hdd_root_path);
DECLARE_bool(binlog_notify_on_put);
DECLARE_int32(task_pool_size);
DECLARE_int32(io_pool_size);
DECLARE_int32(make_snapshot_time);
DECLARE_int32(make_disktable_snapshot_interval);
DECLARE_int32(make_snapshot_check_interval);
DECLARE_uint32(make_snapshot_offline_interval);
DECLARE_bool(recycle_bin_enabled);
DECLARE_uint32(recycle_ttl);
DECLARE_string(recycle_bin_root_path);
DECLARE_string(recycle_ssd_bin_root_path);
DECLARE_string(recycle_hdd_bin_root_path);
DECLARE_int32(make_snapshot_threshold_offset);
DECLARE_uint32(get_table_diskused_interval);

// cluster config
DECLARE_string(endpoint);
DECLARE_string(zk_cluster);
DECLARE_string(zk_root_path);
DECLARE_int32(zk_session_timeout);
DECLARE_int32(zk_keep_alive_check_interval);

DECLARE_int32(binlog_sync_to_disk_interval);
DECLARE_int32(binlog_delete_interval);
DECLARE_uint32(absolute_ttl_max);
DECLARE_uint32(latest_ttl_max);
DECLARE_uint32(max_traverse_cnt);

namespace rtidb {
namespace tablet {

const static std::string SERVER_CONCURRENCY_KEY = "server";
const static uint32_t SEED = 0xe17a1465;

TabletImpl::TabletImpl():tables_(),mu_(), gc_pool_(FLAGS_gc_pool_size),
    replicators_(), snapshots_(), zk_client_(NULL),
    keep_alive_pool_(1), task_pool_(FLAGS_task_pool_size),
    io_pool_(FLAGS_io_pool_size), snapshot_pool_(1), server_(NULL),
    mode_root_paths_(), mode_recycle_root_paths_(){
    follower_.store(false);
}

TabletImpl::~TabletImpl() {
    task_pool_.Stop(true);
    keep_alive_pool_.Stop(true);
    gc_pool_.Stop(true);
    io_pool_.Stop(true);
    snapshot_pool_.Stop(true);
}

bool TabletImpl::Init() {
    std::lock_guard<std::mutex> lock(mu_);
    ::rtidb::base::SplitString(FLAGS_db_root_path, ",", mode_root_paths_[::rtidb::common::kMemory]);
    ::rtidb::base::SplitString(FLAGS_ssd_root_path, ",", mode_root_paths_[::rtidb::common::kSSD]);
    ::rtidb::base::SplitString(FLAGS_hdd_root_path, ",", mode_root_paths_[::rtidb::common::kHDD]);

    ::rtidb::base::SplitString(FLAGS_recycle_bin_root_path, ",", mode_recycle_root_paths_[::rtidb::common::kMemory]);
    ::rtidb::base::SplitString(FLAGS_recycle_ssd_bin_root_path, ",", mode_recycle_root_paths_[::rtidb::common::kSSD]);
    ::rtidb::base::SplitString(FLAGS_recycle_hdd_bin_root_path, ",", mode_recycle_root_paths_[::rtidb::common::kHDD]);

    if (!FLAGS_zk_cluster.empty()) {
        zk_client_ = new ZkClient(FLAGS_zk_cluster, FLAGS_zk_session_timeout,
                FLAGS_endpoint, FLAGS_zk_root_path);
        bool ok = zk_client_->Init();
        if (!ok) {
            PDLOG(WARNING, "fail to init zookeeper with cluster %s", FLAGS_zk_cluster.c_str());
            return false;
        }
    }else {
        PDLOG(INFO, "zk cluster disabled");
    }

    if (FLAGS_make_snapshot_time < 0 || FLAGS_make_snapshot_time > 23) {
        PDLOG(WARNING, "make_snapshot_time[%d] is illegal.", FLAGS_make_snapshot_time);
        return false;
    }

    if (FLAGS_make_disktable_snapshot_interval <= 0) {
        PDLOG(WARNING, "make_disktable_snapshot_interval[%d] is illegal.", FLAGS_make_disktable_snapshot_interval);
        return false;
    }

    if (!CreateMultiDir(mode_root_paths_[::rtidb::common::kMemory])) {
        PDLOG(WARNING, "fail to create db root path %s", FLAGS_db_root_path.c_str());
        return false;
    }

    if (!CreateMultiDir(mode_root_paths_[::rtidb::common::kSSD])) {
        PDLOG(WARNING, "fail to create ssd root path %s", FLAGS_ssd_root_path.c_str());
        return false;
    }

    if (!CreateMultiDir(mode_root_paths_[::rtidb::common::kHDD])) {
        PDLOG(WARNING, "fail to create hdd root path %s", FLAGS_hdd_root_path.c_str());
        return false;
    }

    if (!CreateMultiDir(mode_recycle_root_paths_[::rtidb::common::kMemory])) {
        PDLOG(WARNING, "fail to create recycle bin root path %s", FLAGS_recycle_bin_root_path.c_str());
        return false;
    }

    if (!CreateMultiDir(mode_recycle_root_paths_[::rtidb::common::kSSD])) {
        PDLOG(WARNING, "fail to create recycle ssd bin root path %s", FLAGS_recycle_ssd_bin_root_path.c_str());
        return false;
    }

    if (!CreateMultiDir(mode_recycle_root_paths_[::rtidb::common::kHDD])) {
        PDLOG(WARNING, "fail to create recycle bin root path %s", FLAGS_recycle_hdd_bin_root_path.c_str());
        return false;
    }

    snapshot_pool_.DelayTask(FLAGS_make_snapshot_check_interval, boost::bind(&TabletImpl::SchedMakeSnapshot, this));
    snapshot_pool_.DelayTask(FLAGS_make_disktable_snapshot_interval * 60 * 1000, boost::bind(&TabletImpl::SchedMakeDiskTableSnapshot, this));
    task_pool_.AddTask(boost::bind(&TabletImpl::GetDiskused, this));
    if (FLAGS_recycle_ttl != 0) {
        task_pool_.DelayTask(FLAGS_recycle_ttl*60*1000, boost::bind(&TabletImpl::SchedDelRecycle, this));
    }
#ifdef TCMALLOC_ENABLE
    MallocExtension* tcmalloc = MallocExtension::instance();
    tcmalloc->SetMemoryReleaseRate(FLAGS_mem_release_rate);
#endif 
    return true;
}

void TabletImpl::UpdateTTL(RpcController* ctrl,
        const ::rtidb::api::UpdateTTLRequest* request,
        ::rtidb::api::UpdateTTLResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<Table> table = GetTable(request->tid(), request->pid());

    if (!table) {
        PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(),
                request->pid());
        response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
        response->set_msg("table is not exist");
        return;
    }

    uint64_t abs_ttl = 0;
    uint64_t lat_ttl = 0;
    ::rtidb::api::TTLType ttl_type = ::rtidb::api::TTLType::kAbsoluteTime;
    if (request->has_ttl_desc()) {
        ttl_type = request->ttl_desc().ttl_type();
        abs_ttl = request->ttl_desc().abs_ttl();
        lat_ttl = request->ttl_desc().lat_ttl();
    } else if (request->has_value()){
        ttl_type = request->type();
        if (ttl_type == ::rtidb::api::TTLType::kAbsoluteTime) {
            abs_ttl = request->value();
            lat_ttl = 0;
        } else {
            abs_ttl = 0;
            lat_ttl = request->value();
        }
    }
    if (ttl_type != table->GetTTLType()) {
        response->set_code(::rtidb::base::ReturnCode::kTtlTypeMismatch);
        response->set_msg("ttl type mismatch");
        PDLOG(WARNING, "ttl type mismatch. tid %u, pid %u", request->tid(), request->pid());
        return;
    }
    if (abs_ttl > FLAGS_absolute_ttl_max || lat_ttl > FLAGS_latest_ttl_max) {
        response->set_code(::rtidb::base::ReturnCode::kTtlIsGreaterThanConfValue);
        response->set_msg("ttl is greater than conf value. max abs_ttl is " + std::to_string(FLAGS_absolute_ttl_max) + ", max lat_ttl is " + std::to_string(FLAGS_latest_ttl_max));
        PDLOG(WARNING, "ttl is greater than conf value. abs_ttl[%lu] lat_ttl[%lu] ttl_type[%s] max abs_ttl[%u] max lat_ttl[%u]", 
                        abs_ttl, abs_ttl, ::rtidb::api::TTLType_Name(ttl_type).c_str(), 
                        FLAGS_absolute_ttl_max, FLAGS_latest_ttl_max);
        return;
    }
    if (request->has_ts_name() && request->ts_name().size() > 0) {
        auto iter = table->GetTSMapping().find(request->ts_name());
        if (iter == table->GetTSMapping().end()) {
            PDLOG(WARNING, "ts name %s not found in table tid %u, pid %u", request->ts_name().c_str(),
                  request->tid(), request->pid());
            response->set_code(::rtidb::base::ReturnCode::kTsNameNotFound);
            response->set_msg("ts name not found");
            return;
        }
        table->SetTTL(iter->second, abs_ttl, lat_ttl);
        PDLOG(INFO, "update table #tid %d #pid %d ttl to abs_ttl %lu lat_ttl %lu, ts_name %s",
                request->tid(), request->pid(), abs_ttl, lat_ttl, request->ts_name().c_str());
    } else if (!table->GetTSMapping().size()){
        table->SetTTL(abs_ttl, lat_ttl);
        PDLOG(INFO, "update table #tid %d #pid %d ttl to abs_ttl %lu lat_ttl %lu", request->tid(), request->pid(), abs_ttl, lat_ttl);
    } else {
        PDLOG(WARNING, "set ttl without ts name,  table tid %u, pid %u", request->tid(), request->pid());
        response->set_code(::rtidb::base::ReturnCode::kTsNameNotFound);
        response->set_msg("set ttl need to specify ts column");
        return;
    }
    response->set_code(::rtidb::base::ReturnCode::kOk);
    response->set_msg("ok");
}

bool TabletImpl::RegisterZK() {
    if (!FLAGS_zk_cluster.empty()) {
        if (!zk_client_->Register(true)) {
            PDLOG(WARNING, "fail to register tablet with endpoint %s", FLAGS_endpoint.c_str());
            return false;
        }
        PDLOG(INFO, "tablet with endpoint %s register to zk cluster %s ok", FLAGS_endpoint.c_str(), FLAGS_zk_cluster.c_str());
        keep_alive_pool_.DelayTask(FLAGS_zk_keep_alive_check_interval, boost::bind(&TabletImpl::CheckZkClient, this));
    }
    return true;
}

bool TabletImpl::CheckGetDone(::rtidb::api::GetType type, uint64_t ts,  uint64_t target_ts) {
    switch (type) {
        case rtidb::api::GetType::kSubKeyEq:
            if (ts == target_ts) {
                return true;
            }
            break;
        case rtidb::api::GetType::kSubKeyLe:
            if (ts <= target_ts) {
                return true;
            }
            break;
        case rtidb::api::GetType::kSubKeyLt:
            if (ts < target_ts) {
                return true;
            }
            break;
        case rtidb::api::GetType::kSubKeyGe:
            if (ts >= target_ts) {
                return true;
            }
            break;
        case rtidb::api::GetType::kSubKeyGt:
            if (ts > target_ts) {
                return true;
            }
    }
    return false;
}

int32_t TabletImpl::GetIndex(uint64_t expire_time, uint64_t expire_cnt,
                                ::rtidb::api::TTLType ttl_type,
                                 ::rtidb::storage::TableIterator* it,
                                 const ::rtidb::api::GetRequest* request,
                                 std::string* value,
                                 uint64_t* ts) {
    uint64_t st = request->ts();
    const rtidb::api::GetType& st_type = request->type();
    uint64_t et = request->et();
    const rtidb::api::GetType& et_type = request->et_type();
    if (it == NULL || value == NULL || ts == NULL) {
        PDLOG(WARNING, "invalid args");
        return -1;
    }
    if (st_type == ::rtidb::api::kSubKeyEq
            && et_type == ::rtidb::api::kSubKeyEq
            && st != et) return -1;

    ::rtidb::api::GetType real_et_type = et_type;
    if (ttl_type == ::rtidb::api::TTLType::kAbsoluteTime || ttl_type == ::rtidb::api::TTLType::kAbsOrLat) {
        et = std::max(et, expire_time);
    }
    if (et < expire_time && et_type == ::rtidb::api::GetType::kSubKeyGt) {
        real_et_type = ::rtidb::api::GetType::kSubKeyGe; 
    }

    if (st_type != ::rtidb::api::GetType::kSubKeyEq &&
        st_type != ::rtidb::api::GetType::kSubKeyLe &&
        st_type != ::rtidb::api::GetType::kSubKeyLt &&
        st_type != ::rtidb::api::GetType::kSubKeyGt &&
        st_type != ::rtidb::api::GetType::kSubKeyGe) {
        PDLOG(WARNING, "invalid st type %s", ::rtidb::api::GetType_Name(st_type).c_str());
        return -2;
    }
    uint32_t cnt = 0;
    if (st > 0) {
        if (st < et) {
            PDLOG(WARNING, "invalid args for st %lu less than et %lu or expire time %lu", st, et, expire_time);
            return -1;
        }
        switch(ttl_type) {
            case ::rtidb::api::TTLType::kAbsoluteTime: {
                if (!Seek(it, st, st_type)) {
                    return 1;
                }
                break;
            }
            case ::rtidb::api::TTLType::kAbsAndLat: {
                if (st < expire_time) {
                    if (!SeekWithCount(it, st, st_type, expire_cnt, cnt)) { return 1; }
                } else {
                    if (!Seek(it, st, st_type)) { return 1;}
                }
                break;
            }
            default: {
                if (!SeekWithCount(it, st, st_type, expire_cnt, cnt)) {
                    return 1; 
                }
                break;
            }
        }
    } else {
        it->SeekToFirst();
    }
    if (it->Valid()) {
        bool jump_out = false;
        if (st_type == ::rtidb::api::GetType::kSubKeyGe ||
            st_type == ::rtidb::api::GetType::kSubKeyGt) {
            ::rtidb::base::Slice it_value = it->GetValue();
            value->assign(it_value.data(), it_value.size());
            *ts = it->GetKey();
            return 0;
        }
        switch(real_et_type) {
            case ::rtidb::api::GetType::kSubKeyEq:
                if (it->GetKey() != et) {
                    jump_out = true;
                }
                break;

            case ::rtidb::api::GetType::kSubKeyGt:
                if (it->GetKey() <= et) {
                    jump_out = true;
                }
                break;

            case ::rtidb::api::GetType::kSubKeyGe:
                if (it->GetKey() < et) {
                    jump_out = true;
                }
                break;

            default:
                PDLOG(WARNING, "invalid et type %s", ::rtidb::api::GetType_Name(et_type).c_str());
                return -2;
        }
        if (jump_out) {
            return 1;
        }
        ::rtidb::base::Slice it_value = it->GetValue();
        value->assign(it_value.data(), it_value.size());
        *ts = it->GetKey();
        return 0;
    }
    // not found
    return 1;
}


void TabletImpl::Get(RpcController* controller,
             const ::rtidb::api::GetRequest* request,
             ::rtidb::api::GetResponse* response,
             Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<Table> table = GetTable(request->tid(), request->pid());
    std::shared_ptr<RelationalTable> r_table;
    if (!table) {
        std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
        r_table = GetRelationalTableUnLock(request->tid(), request->pid());
        if (!r_table) {
            PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(), request->pid());
            response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
            response->set_msg("table is not exist");
            return;
        }
    }
    if (table) {
        if (table->GetTableStat() == ::rtidb::storage::kLoading) {
            PDLOG(WARNING, "table is loading. tid %u, pid %u", 
                    request->tid(), request->pid());
            response->set_code(::rtidb::base::ReturnCode::kTableIsLoading);
            response->set_msg("table is loading");
            return;
        }

        uint32_t index = 0;
        int ts_index = -1;
        if (request->has_idx_name() && request->idx_name().size() > 0) {
            std::shared_ptr<IndexDef> index_def = table->GetIndex(request->idx_name());
            if (!index_def || !index_def->IsReady()) {
                PDLOG(WARNING, "idx name %s not found in table tid %u, pid %u", request->idx_name().c_str(),
                        request->tid(), request->pid());
                response->set_code(::rtidb::base::ReturnCode::kIdxNameNotFound);
                response->set_msg("idx name not found");
                return;
            }
            index = index_def->GetId();
        }
        if (request->has_ts_name() && request->ts_name().size() > 0) {
            auto iter = table->GetTSMapping().find(request->ts_name());
            if (iter == table->GetTSMapping().end()) {
                PDLOG(WARNING, "ts name %s not found in table tid %u, pid %u", request->ts_name().c_str(), request->tid(), request->pid());
                response->set_code(::rtidb::base::ReturnCode::kTsNameNotFound);
                response->set_msg("ts name not found");
                return;
            }
            ts_index = iter->second;
        }

        ::rtidb::storage::Ticket ticket;
        ::rtidb::storage::TableIterator* it = NULL;
        if (ts_index >= 0) {
            it = table->NewIterator(index, ts_index, request->key(), ticket);
        } else {
            it = table->NewIterator(index, request->key(), ticket);
        }

        if (it == NULL) {
            response->set_code(::rtidb::base::ReturnCode::kTsNameNotFound);
            response->set_msg("ts name not found");
            return;
        }

        ::rtidb::storage::TTLDesc ttl = ts_index < 0 ? table->GetTTL(index) : table->GetTTL(index, ts_index);
        std::string* value = response->mutable_value(); 
        uint64_t ts = 0;
        int32_t code = 0;
        code = GetIndex(table->GetExpireTime(ttl.abs_ttl*60*1000), ttl.lat_ttl,
                table->GetTTLType(), it, request, value, &ts);
        delete it;
        response->set_ts(ts);
        response->set_code(code);
        switch(code) {
            case 1:
                response->set_code(::rtidb::base::ReturnCode::kKeyNotFound);
                response->set_msg("key not found");
                return;
            case 0:
                return;
            case -1:
                response->set_msg("invalid args");
                response->set_code(::rtidb::base::ReturnCode::kInvalidParameter);
                return;
            case -2:
                response->set_code(::rtidb::base::ReturnCode::kInvalidParameter);
                response->set_msg("st/et sub key type is invalid");
                return;
            default:
                return;
        }
    } else {
        std::string * value = response->mutable_value(); 
        bool ok = false;
        uint32_t index = 0;
        /**
        if (request->has_idx_name() && request->idx_name().size() > 0) {
            std::map<std::string, uint32_t>::iterator iit = r_table->GetMapping().find(request->idx_name());
            if (iit == r_table->GetMapping().end()) {
                PDLOG(WARNING, "idx name %s not found in table tid %u, pid %u", request->idx_name().c_str(),
                        request->tid(), request->pid());
                response->set_code(::rtidb::base::ReturnCode::kIdxNameNotFound);
                response->set_msg("idx name not found");
                return;
            }
            index = iit->second;
        }
        */
        rtidb::base::Slice slice;
        ok = r_table->Get(index, request->key(), slice);
        if (!ok) {
            response->set_code(::rtidb::base::ReturnCode::kKeyNotFound);
            response->set_msg("key not found");
            return;
        }
        value->assign(slice.data(), slice.size());
        response->set_code(::rtidb::base::ReturnCode::kOk);
        response->set_msg("ok");
    }
}

void TabletImpl::Update(RpcController* controller,
        const ::rtidb::api::UpdateRequest* request,
        ::rtidb::api::GeneralResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    if (follower_.load(std::memory_order_relaxed)) {
        response->set_code(::rtidb::base::ReturnCode::kIsFollowerCluster);
        response->set_msg("is follower cluster");
        return;
    }
    std::shared_ptr<RelationalTable> r_table;
    {
        std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
        r_table = GetRelationalTableUnLock(request->tid(), request->pid());
        if (!r_table) {
            PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(), request->pid());
            response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
            response->set_msg("table is not exist");
            return;
        }
    }
    bool ok = r_table->Update(request->condition_columns(), request->value_columns());
    if (!ok) {
        response->set_code(::rtidb::base::ReturnCode::kUpdateFailed);
        response->set_msg("update failed");
        PDLOG(WARNING, "update failed. tid %u, pid %u", request->tid(), request->pid());
        return;
    }
    response->set_code(::rtidb::base::ReturnCode::kOk);
    response->set_msg("ok");
}

void TabletImpl::Put(RpcController* controller,
        const ::rtidb::api::PutRequest* request,
        ::rtidb::api::PutResponse* response,
        Closure* done) {
    if (follower_.load(std::memory_order_relaxed)) {
        response->set_code(::rtidb::base::ReturnCode::kIsFollowerCluster);
        response->set_msg("is follower cluster");
        done->Run();
        return;
    }
    std::shared_ptr<Table> table = GetTable(request->tid(), request->pid());
    std::shared_ptr<RelationalTable> r_table;
    if (!table) {
        std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
        r_table = GetRelationalTableUnLock(request->tid(), request->pid());
        if (!r_table) {
            PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(), request->pid());
            response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
            response->set_msg("table is not exist");
            done->Run();
            return;
        }
    }
    if (table) {
        if (request->time() == 0 && request->ts_dimensions_size() == 0) {
            response->set_code(::rtidb::base::ReturnCode::kTsMustBeGreaterThanZero);
            response->set_msg("ts must be greater than zero");
            done->Run();
            return;
        }

        if (!table->IsLeader()) {
            response->set_code(::rtidb::base::ReturnCode::kTableIsFollower);
            response->set_msg("table is follower");
            done->Run();
            return;
        }
        if (table->GetTableStat() == ::rtidb::storage::kLoading) {
            PDLOG(WARNING, "table is loading. tid %u, pid %u", 
                    request->tid(), request->pid());
            response->set_code(::rtidb::base::ReturnCode::kTableIsLoading);
            response->set_msg("table is loading");
            done->Run();
            return;
        }
        bool ok = false;
        if (request->dimensions_size() > 0) {
            int32_t ret_code = CheckDimessionPut(request, table->GetIdxCnt());
            if (ret_code != 0) {
                response->set_code(::rtidb::base::ReturnCode::kInvalidDimensionParameter);
                response->set_msg("invalid dimension parameter");
                done->Run();
                return;
            }
            if (request->ts_dimensions_size() > 0) {
                ok = table->Put(request->dimensions(), request->ts_dimensions(), request->value());
            } else {
                ok = table->Put(request->time(), request->value(), request->dimensions());
            }
        } else {
            ok = table->Put(request->pk(), 
                    request->time(), 
                    request->value().c_str(),
                    request->value().size());
        }
        if (!ok) {
            response->set_code(::rtidb::base::ReturnCode::kPutFailed);
            response->set_msg("put failed");
            done->Run();
            return;
        }
        response->set_code(::rtidb::base::ReturnCode::kOk);
        std::shared_ptr<LogReplicator> replicator;
        do {
            replicator = GetReplicator(request->tid(), request->pid());
            if (!replicator) {
                PDLOG(WARNING, "fail to find table tid %u pid %u leader's log replicator", request->tid(),
                        request->pid());
                break;
            }
            ::rtidb::api::LogEntry entry;
            entry.set_pk(request->pk());
            entry.set_ts(request->time());
            entry.set_value(request->value());
            entry.set_term(replicator->GetLeaderTerm());
            if (request->dimensions_size() > 0) {
                entry.mutable_dimensions()->CopyFrom(request->dimensions());
            }
            if (request->ts_dimensions_size() > 0) {
                entry.mutable_ts_dimensions()->CopyFrom(request->ts_dimensions());
            }
            replicator->AppendEntry(entry);
        } while(false);
        done->Run();
        if (replicator) {
            if (FLAGS_binlog_notify_on_put) {
                replicator->Notify(); 
            }
        }
    } else {
        bool ok = r_table->Put(request->value());
        if (!ok) {
            response->set_code(::rtidb::base::ReturnCode::kPutFailed);
            response->set_msg("put failed");
            done->Run();
            return;
        }
        done->Run();
        response->set_code(::rtidb::base::ReturnCode::kOk);
    }
}

int TabletImpl::CheckTableMeta(const rtidb::api::TableMeta* table_meta, std::string& msg) {
    msg.clear();
    if (table_meta->name().size() <= 0) {
        msg = "table name is empty";
        return -1;
    }
    if (table_meta->tid() <= 0) {
        msg = "tid is zero";
        return -1;
    }
    ::rtidb::api::TTLType type = ::rtidb::api::TTLType::kAbsoluteTime;
    if (table_meta->has_ttl_desc()) {
        type = table_meta->ttl_desc().ttl_type();
        if ((table_meta->ttl_desc().abs_ttl() > FLAGS_absolute_ttl_max) ||
                (table_meta->ttl_desc().lat_ttl() > FLAGS_latest_ttl_max)) {
            msg = "ttl is greater than conf value. max abs_ttl is " + std::to_string(FLAGS_absolute_ttl_max) + ", max lat_ttl is " + std::to_string(FLAGS_latest_ttl_max);
            return -1;
        }
    } else if (table_meta->has_ttl()){
        uint64_t ttl = table_meta->ttl();
        type = table_meta->ttl_type();
        if ((type == ::rtidb::api::TTLType::kAbsoluteTime && ttl > FLAGS_absolute_ttl_max) ||
                (type == ::rtidb::api::kLatestTime && ttl > FLAGS_latest_ttl_max)) {
            uint32_t max_ttl = type == ::rtidb::api::TTLType::kAbsoluteTime ? FLAGS_absolute_ttl_max : FLAGS_latest_ttl_max;
            msg = "ttl is greater than conf value. max ttl is " + std::to_string(max_ttl);
            return -1;
        }
    }

    std::map<std::string, std::string> column_map;
    std::set<std::string> ts_set;
    if (table_meta->column_desc_size() > 0) {
        for (const auto& column_desc : table_meta->column_desc()) {
            if (column_map.find(column_desc.name()) != column_map.end()) {
                msg = "has repeated column name " + column_desc.name();
                return -1;
            }
            if (column_desc.is_ts_col()) {
                if (column_desc.add_ts_idx()) {
                    msg = "can not set add_ts_idx and is_ts_col together. column name " + column_desc.name();
                    return -1;
                }
                if (column_desc.type() != "int64" && column_desc.type() != "uint64" && 
                        column_desc.type() != "timestamp") {
                    msg = "ttl column type must be int64, uint64, timestamp";
                    return -1;
                }
                if (column_desc.has_abs_ttl() || column_desc.has_lat_ttl()) {
                    if ((column_desc.abs_ttl() > FLAGS_absolute_ttl_max) ||
                            (column_desc.lat_ttl() > FLAGS_latest_ttl_max)) {
                        msg = "ttl is greater than conf value. max abs_ttl is " + std::to_string(FLAGS_absolute_ttl_max) + ", max lat_ttl is " + std::to_string(FLAGS_latest_ttl_max);
                        return -1;
                    }
                } else if (column_desc.has_ttl()) {
                    uint64_t ttl = column_desc.ttl();
                    if ((type == ::rtidb::api::TTLType::kAbsoluteTime && ttl > FLAGS_absolute_ttl_max) ||
                            (type == ::rtidb::api::kLatestTime && ttl > FLAGS_latest_ttl_max)) {
                        uint32_t max_ttl = type == ::rtidb::api::TTLType::kAbsoluteTime ? FLAGS_absolute_ttl_max : FLAGS_latest_ttl_max;
                        msg = "ttl is greater than conf value. max ttl is " + std::to_string(max_ttl);
                        return -1;
                    }
                }
                ts_set.insert(column_desc.name());
            }
            if (column_desc.add_ts_idx() && ((column_desc.type() == "float") || (column_desc.type() == "double"))) {
                msg = "float or double column can not be index";
                return -1;
            }
            column_map.insert(std::make_pair(column_desc.name(), column_desc.type()));
        }
    }
    std::set<std::string> index_set;
    if (table_meta->column_key_size() > 0) {
        for (const auto& column_key : table_meta->column_key()) {
            if (index_set.find(column_key.index_name()) != index_set.end()) {
                msg = "has repeated index name " + column_key.index_name();
                return -1;
            }
            index_set.insert(column_key.index_name());
            bool has_col = false;
            for (const auto& column_name : column_key.col_name()) {
                has_col = true;
                auto iter = column_map.find(column_name);
                if (iter == column_map.end()) {
                    msg = "not found column name " + column_name;
                    return -1;
                }
                if ((iter->second == "float") || (iter->second == "double")) {
                    msg = "float or double column can not be index" + column_name;
                    return -1;
                }
                if (ts_set.find(column_name) != ts_set.end()) {
                    msg = "column name in column key can not set ts col. column name " + column_name;
                    return -1;
                }
            }
            if (!has_col) {
                auto iter = column_map.find(column_key.index_name());
                if (iter == column_map.end()) {
                    msg = "index must member of columns when column key col name is empty";
                    return -1;
                } else {
                    if ((iter->second == "float") || (iter->second == "double")) {
                        msg = "indxe name column type can not float or column";
                        return -1;
                    }
                }
            }
            std::set<std::string> ts_name_set;
            for (const auto& ts_name : column_key.ts_name()) {
                if (ts_set.find(ts_name) == ts_set.end()) {
                    msg = "not found ts_name " + ts_name;
                    return -1;
                }
                if (ts_name_set.find(ts_name) != ts_name_set.end()) {
                    msg = "has repeated ts_name " + ts_name;
                    return -1;
                }
                ts_name_set.insert(ts_name);
            }
            if (ts_set.size() > 1 && column_key.ts_name_size() == 0) {
                msg = "ts column num more than one, must set ts name";
                return -1;
            }
        }
    } else if (ts_set.size() > 1) {
        msg = "column_key should be set when has two or more ts columns";
        return -1;
    }
    return 0;
}

int32_t TabletImpl::ScanIndex(uint64_t expire_time, uint64_t expire_cnt,
        ::rtidb::api::TTLType ttl_type,
        ::rtidb::storage::TableIterator* it,
        const ::rtidb::api::ScanRequest* request,
        std::string* pairs,
        uint32_t* count) {
    uint32_t limit = request->limit();
    uint32_t atleast = request->atleast();
    uint64_t st = request->st();
    const rtidb::api::GetType& st_type = request->st_type();
    uint64_t et = request->et();
    const rtidb::api::GetType& et_type = request->et_type();
    bool remove_duplicated_record = request->has_enable_remove_duplicated_record() 
        && request->enable_remove_duplicated_record();
    if (it == NULL || pairs == NULL || count == NULL || (atleast > limit && limit != 0)) {
        PDLOG(WARNING, "invalid args");
        return -1;
    }
    rtidb::api::GetType real_st_type = st_type;
    rtidb::api::GetType real_et_type = et_type;
    if (et < expire_time && et_type == ::rtidb::api::GetType::kSubKeyGt) {
        real_et_type = ::rtidb::api::GetType::kSubKeyGe;
    }
    uint64_t real_et = 0;
    if (ttl_type == ::rtidb::api::TTLType::kAbsoluteTime || ttl_type == ::rtidb::api::TTLType::kAbsOrLat) {
        real_et = std::max(et, expire_time);
    } else {
        real_et = et;
    }
    if (st_type == ::rtidb::api::GetType::kSubKeyEq) {
        real_st_type = ::rtidb::api::GetType::kSubKeyLe;
    }
    if (st_type != ::rtidb::api::GetType::kSubKeyEq &&
            st_type != ::rtidb::api::GetType::kSubKeyLe &&
            st_type != ::rtidb::api::GetType::kSubKeyLt) {
        PDLOG(WARNING, "invalid st type %s", ::rtidb::api::GetType_Name(st_type).c_str());
        return -2;
    }
    uint32_t cnt = 0;
    if (st > 0) {
        if (st < expire_time || st < et) {
            PDLOG(WARNING, "invalid args for st %lu less than et %lu or expire time %lu", st, et, expire_time);
            return -1;
        }
        switch (ttl_type) {
            case ::rtidb::api::TTLType::kAbsoluteTime: 
                Seek(it, st, real_st_type);
                break;
            default: 
                SeekWithCount(it, st, real_st_type, expire_cnt, cnt);
                break;
        }
    } else {
        it->SeekToFirst();
    }

    uint64_t last_time = 0;
    std::vector<std::pair<uint64_t, ::rtidb::base::Slice>> tmp;
    tmp.reserve(FLAGS_scan_reserve_size);
    uint32_t total_block_size = 0;
    while(it->Valid()) {
        if (limit > 0 && tmp.size() >= limit) {
            break;
        }
        if (ttl_type == ::rtidb::api::TTLType::kAbsoluteTime) {
            if (expire_time != 0 && it->GetKey() <= expire_time) {
                break;
            }
            if (remove_duplicated_record && tmp.size() > 0 && 
                    last_time == it->GetKey()) {
                it->Next();
                continue;
            }
            last_time = it->GetKey();
        } else if (ttl_type == ::rtidb::api::TTLType::kLatestTime) {
            if (expire_cnt != 0 && cnt >= expire_cnt) {
                break;
            }
        } else if (ttl_type == ::rtidb::api::TTLType::kAbsAndLat) {
            if ((expire_cnt != 0 && cnt >= expire_cnt) && (expire_time != 0 && it->GetKey() <= expire_time)) {
                break;
            }
        } else {
            if ((expire_cnt != 0 && cnt >= expire_cnt) || (expire_time != 0 && it->GetKey() <= expire_time)) {
                break;
            }
        }
        ++cnt;

        if (atleast <= 0 || tmp.size() >= atleast) {
            bool jump_out = false;
            switch(real_et_type) {
                case ::rtidb::api::GetType::kSubKeyEq:
                    if (it->GetKey() != real_et) {
                        jump_out = true;
                    }
                    break;
                case ::rtidb::api::GetType::kSubKeyGt:
                    if (it->GetKey() <= real_et) {
                        jump_out = true;
                    }
                    break;
                case ::rtidb::api::GetType::kSubKeyGe:
                    if (it->GetKey() < real_et) {
                        jump_out = true;
                    }
                    break;
                default:
                    PDLOG(WARNING, "invalid et type %s", ::rtidb::api::GetType_Name(et_type).c_str());
                    return -2;
            }
            if (jump_out) break;
        }
        ::rtidb::base::Slice it_value = it->GetValue();
        tmp.push_back(std::make_pair(it->GetKey(), it_value));
        total_block_size += it_value.size();
        it->Next();
        if (total_block_size > FLAGS_scan_max_bytes_size) {
            PDLOG(WARNING, "reach the max byte size");
            return -3;
        }
    }
    int32_t ok = ::rtidb::base::EncodeRows(tmp, total_block_size, pairs);
    if (ok == -1) {
        PDLOG(WARNING, "fail to encode rows");
        return -4;
    }
    *count = tmp.size();
    return 0;
}

int32_t TabletImpl::CountIndex(uint64_t expire_time, uint64_t expire_cnt,
        ::rtidb::api::TTLType ttl_type,
        ::rtidb::storage::TableIterator* it,
        const ::rtidb::api::CountRequest* request,
        uint32_t* count) {
    uint64_t st = request->st();
    const rtidb::api::GetType& st_type = request->st_type();
    uint64_t et = request->et();
    const rtidb::api::GetType& et_type = request->et_type();
    bool remove_duplicated_record = request->has_enable_remove_duplicated_record()
        && request->enable_remove_duplicated_record();
    if (it == NULL || count == NULL) {
        PDLOG(WARNING, "invalid args");
        return -1;
    }
    rtidb::api::GetType real_st_type = st_type;
    rtidb::api::GetType real_et_type = et_type;
    if (et < expire_time && et_type == ::rtidb::api::GetType::kSubKeyGt) {
        real_et_type = ::rtidb::api::GetType::kSubKeyGe;
    }
    if (ttl_type == ::rtidb::api::TTLType::kAbsoluteTime || ttl_type == ::rtidb::api::TTLType::kAbsOrLat) {
        et = std::max(et, expire_time);
    }
    if (st_type == ::rtidb::api::GetType::kSubKeyEq) {
        real_st_type = ::rtidb::api::GetType::kSubKeyLe;
    }
    if (st_type != ::rtidb::api::GetType::kSubKeyEq &&
            st_type != ::rtidb::api::GetType::kSubKeyLe &&
            st_type != ::rtidb::api::GetType::kSubKeyLt) {
        PDLOG(WARNING, "invalid st type %s", ::rtidb::api::GetType_Name(st_type).c_str());
        return -2;
    }
    uint32_t cnt = 0;
    if (st > 0) {
        if (st < et) {
            PDLOG(WARNING, "invalid args for st %lu less than et %lu or expire time %lu", st, et, expire_time);
            return -1;
        }
        switch (ttl_type) {
            case ::rtidb::api::TTLType::kAbsoluteTime: 
                Seek(it, st, real_st_type);
                break;
            default: 
                SeekWithCount(it, st, real_st_type, expire_cnt, cnt);
                break;
        }
    } else {
        it->SeekToFirst();
    }

    uint64_t last_key = 0;
    uint32_t internal_cnt = 0;

    while(it->Valid()) {
        if (remove_duplicated_record 
                && internal_cnt > 0
                && last_key == it->GetKey()) {
            cnt++;
            it->Next();
            continue;
        }
        if (ttl_type == ::rtidb::api::TTLType::kAbsoluteTime) {
            if (expire_time != 0 && it->GetKey() <= expire_time) {
                break;
            }
        } else if (ttl_type == ::rtidb::api::TTLType::kLatestTime) {
            if (expire_cnt != 0 && cnt >= expire_cnt) {
                break;
            }
        } else if (ttl_type == ::rtidb::api::TTLType::kAbsAndLat) {
            if ((expire_cnt != 0 && cnt >= expire_cnt) && (expire_time != 0 && it->GetKey() <= expire_time)) {
                break;
            }
        } else {
            if ((expire_cnt != 0 && cnt >= expire_cnt) || (expire_time != 0 && it->GetKey() <= expire_time)) {
                break;
            }
        }
        ++cnt;
        bool jump_out = false;
        last_key = it->GetKey();
        switch(real_et_type) {
            case ::rtidb::api::GetType::kSubKeyEq:
                if (it->GetKey() != et) {
                    jump_out = true;
                }
                break;
            case ::rtidb::api::GetType::kSubKeyGt:
                if (it->GetKey() <= et) {
                    jump_out = true;
                }
                break;
            case ::rtidb::api::GetType::kSubKeyGe:
                if (it->GetKey() < et) {
                    jump_out = true;
                }
                break;
            default:
                PDLOG(WARNING, "invalid et type %s", ::rtidb::api::GetType_Name(et_type).c_str());
                return -2;
        }
        if (jump_out) break;
        last_key = it->GetKey();
        internal_cnt++;
        it->Next();
    }
    *count = internal_cnt;
    return 0;
}

void TabletImpl::Scan(RpcController* controller,
        const ::rtidb::api::ScanRequest* request,
        ::rtidb::api::ScanResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    if (request->st() < request->et()) {
        response->set_code(::rtidb::base::ReturnCode::kStLessThanEt);
        response->set_msg("starttime less than endtime");
        return;
    }
    std::shared_ptr<Table> table = GetTable(request->tid(), request->pid());
    if (!table) {
        PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(), request->pid());
        response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
        response->set_msg("table is not exist");
        return;
    }
    if (table->GetTableStat() == ::rtidb::storage::kLoading) {
        PDLOG(WARNING, "table is loading. tid %u, pid %u", 
                request->tid(), request->pid());
        response->set_code(::rtidb::base::ReturnCode::kTableIsLoading);
        response->set_msg("table is loading");
        return;
    }
    uint32_t index = 0;
    int ts_index = -1;
    if (request->has_idx_name() && request->idx_name().size() > 0) {
        std::shared_ptr<IndexDef> index_def = table->GetIndex(request->idx_name());
        if (!index_def || !index_def->IsReady()) {
            PDLOG(WARNING, "idx name %s not found in table tid %u, pid %u", request->idx_name().c_str(),
                    request->tid(), request->pid());
            response->set_code(::rtidb::base::ReturnCode::kIdxNameNotFound);
            response->set_msg("idx name not found");
            return;
        }
        index = index_def->GetId();
    }
    if (request->has_ts_name() && request->ts_name().size() > 0) {
        auto iter = table->GetTSMapping().find(request->ts_name());
        if (iter == table->GetTSMapping().end()) {
            PDLOG(WARNING, "ts name %s not found in table tid %u, pid %u", request->ts_name().c_str(),
                    request->tid(), request->pid());
            response->set_code(::rtidb::base::ReturnCode::kTsNameNotFound);
            response->set_msg("ts name not found");
            return;
        }
        ts_index = iter->second;
    }    

    // Use seek to process scan request
    // the first seek to find the total size to copy
    ::rtidb::storage::Ticket ticket;
    ::rtidb::storage::TableIterator* it = NULL;
    if (ts_index >= 0) {
        it = table->NewIterator(index, ts_index, request->pk(), ticket);
    } else {
        it = table->NewIterator(index, request->pk(), ticket);
    }
    if (it == NULL) {
        response->set_code(::rtidb::base::ReturnCode::kKeyNotFound);
        response->set_msg("key not found");
        return;
    }
    ::rtidb::storage::TTLDesc ttl = ts_index < 0 ? table->GetTTL(index) : table->GetTTL(index, ts_index);
    std::string* pairs = response->mutable_pairs(); 
    uint32_t count = 0;
    int32_t code = 0;
    uint64_t expire_time = table->GetExpireTime(ttl.abs_ttl*60*1000);
    uint64_t expire_cnt = ttl.lat_ttl;
    code = ScanIndex(expire_time, expire_cnt, table->GetTTLType(),
            it, request, pairs, &count);
    delete it;
    response->set_code(code);
    response->set_count(count);
    switch(code) {
        case 0:
            return;
        case -1:
            response->set_msg("invalid args");
            response->set_code(::rtidb::base::ReturnCode::kInvalidParameter);
            return;
        case -2:
            response->set_msg("st/et sub key type is invalid");
            response->set_code(::rtidb::base::ReturnCode::kInvalidParameter);
            return;
        case -3:
            response->set_code(::rtidb::base::ReturnCode::kReacheTheScanMaxBytesSize);
            response->set_msg("reach the max scan byte size");
            return;
        case -4:
            response->set_msg("fail to encode data rows");
            response->set_code(::rtidb::base::ReturnCode::kFailToUpdateTtlFromTablet);
            return;
        default:
            return;
    }
}

void TabletImpl::Count(RpcController* controller,
        const ::rtidb::api::CountRequest* request,
        ::rtidb::api::CountResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<Table> table = GetTable(request->tid(), request->pid());
    if (!table) {
        PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(), request->pid());
        response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
        response->set_msg("table is not exist");
        return;
    }
    if (table->GetTableStat() == ::rtidb::storage::kLoading) {
        PDLOG(WARNING, "table is loading. tid %u, pid %u", 
                request->tid(), request->pid());
        response->set_code(::rtidb::base::ReturnCode::kTableIsLoading);
        response->set_msg("table is loading");
        return;
    }
    uint32_t index = 0;
    int ts_index = -1;
    if (request->has_idx_name() && request->idx_name().size() > 0) {
        std::shared_ptr<IndexDef> index_def = table->GetIndex(request->idx_name());
        if (!index_def || !index_def->IsReady()) {
            PDLOG(WARNING, "idx name %s not found in table tid %u, pid %u", request->idx_name().c_str(),
                    request->tid(), request->pid());
            response->set_code(::rtidb::base::ReturnCode::kIdxNameNotFound);
            response->set_msg("idx name not found");
            return;
        }
        index = index_def->GetId();
    }
    if (request->has_ts_name() && request->ts_name().size() > 0) {
        auto iter = table->GetTSMapping().find(request->ts_name());
        if (iter == table->GetTSMapping().end()) {
            PDLOG(WARNING, "ts name %s not found in table tid %u, pid %u", request->ts_name().c_str(),
                    request->tid(), request->pid());
            response->set_code(::rtidb::base::ReturnCode::kTsNameNotFound);
            response->set_msg("ts name not found");
            return;
        }
        ts_index = iter->second;
        if (!table->CheckTsValid(index, ts_index)) {
            response->set_code(::rtidb::base::ReturnCode::kTsNameNotFound);
            response->set_msg("ts name not found");
            return;
        }
    }    
    if (!request->filter_expired_data() && table->GetStorageMode() == ::rtidb::common::StorageMode::kMemory) {
        MemTable* mem_table = dynamic_cast<MemTable*>(table.get());
        if (mem_table != NULL) {
            uint64_t count = 0;
            if (ts_index >= 0) {
                if (mem_table->GetCount(index, ts_index, request->key(), count) < 0) {
                    count = 0;
                }
            } else {
                if (mem_table->GetCount(index, request->key(), count) < 0) {
                    count = 0;
                }
            }
            response->set_code(::rtidb::base::ReturnCode::kOk);
            response->set_msg("ok");
            response->set_count(count);
            return;
        }
    }
    ::rtidb::storage::Ticket ticket;
    ::rtidb::storage::TableIterator* it = NULL;
    if (ts_index >= 0) {
        it = table->NewIterator(index, ts_index, request->key(), ticket);
    } else {
        it = table->NewIterator(index, request->key(), ticket);
    }
    if (it == NULL) {
        response->set_code(::rtidb::base::ReturnCode::kTsNameNotFound);
        response->set_msg("ts name not found");
        return;
    }
    ::rtidb::storage::TTLDesc ttl = ts_index < 0 ? table->GetTTL(index) : table->GetTTL(index, ts_index);
    uint32_t count = 0;
    int32_t code = 0;
    code = CountIndex(table->GetExpireTime(ttl.abs_ttl*60*1000),
            ttl.lat_ttl, table->GetTTLType(), it,
            request, &count);
    delete it;
    response->set_code(code);
    response->set_count(count);
    switch(code) {
        case 0:
            return;
        case -1:
            response->set_msg("invalid args");
            response->set_code(::rtidb::base::ReturnCode::kInvalidParameter);
            return;
        case -2:
            response->set_msg("st/et sub key type is invalid");
            response->set_code(::rtidb::base::ReturnCode::kInvalidParameter);
            return;
        case -3:
            response->set_code(::rtidb::base::ReturnCode::kReacheTheScanMaxBytesSize);
            response->set_msg("reach the max scan byte size");
            return;
        case -4:
            response->set_msg("fail to encode data rows");
            response->set_code(::rtidb::base::ReturnCode::kFailToUpdateTtlFromTablet);
            return;
        default:
            return;
    }
}

void TabletImpl::Traverse(RpcController* controller,
        const ::rtidb::api::TraverseRequest* request,
        ::rtidb::api::TraverseResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<Table> table = GetTable(request->tid(), request->pid());
    std::shared_ptr<RelationalTable> r_table;
    if (!table) {
        std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
        r_table = GetRelationalTableUnLock(request->tid(), request->pid());
        if (!r_table) {
            PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(), request->pid());
            response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
            response->set_msg("table is not exist");
            return;
        }
    }
    if (table) {
        if (table->GetTableStat() == ::rtidb::storage::kLoading) {
            PDLOG(WARNING, "table is loading. tid %u, pid %u", 
                    request->tid(), request->pid());
            response->set_code(::rtidb::base::ReturnCode::kTableIsLoading);
            response->set_msg("table is loading");
            return;
        }
        uint32_t index = 0;
        int ts_index = -1;
        if (request->has_idx_name() && request->idx_name().size() > 0) {
            std::shared_ptr<IndexDef> index_def = table->GetIndex(request->idx_name());
            if (!index_def || !index_def->IsReady()) {
                PDLOG(WARNING, "idx name %s not found in table tid %u, pid %u", request->idx_name().c_str(),
                        request->tid(), request->pid());
                response->set_code(::rtidb::base::ReturnCode::kIdxNameNotFound);
                response->set_msg("idx name not found");
                return;
            }
            index = index_def->GetId();
        }
        if (request->has_ts_name() && request->ts_name().size() > 0) {
            auto iter = table->GetTSMapping().find(request->ts_name());
            if (iter == table->GetTSMapping().end()) {
                PDLOG(WARNING, "ts name %s not found in table tid %u, pid %u", request->ts_name().c_str(),
                        request->tid(), request->pid());
                response->set_code(::rtidb::base::ReturnCode::kTsNameNotFound);
                response->set_msg("ts name not found");
                return;
            }
            ts_index = iter->second;
        }
        ::rtidb::storage::TableIterator* it = NULL;
        if (ts_index >= 0) {
            it = table->NewTraverseIterator(index, ts_index);
        } else {
            it = table->NewTraverseIterator(index);
        }
        if (it == NULL) {
            response->set_code(::rtidb::base::ReturnCode::kTsNameNotFound);
            response->set_msg("ts name not found, when create iterator");
            return;
        }

        uint64_t last_time = 0;
        std::string last_pk;
        if (request->has_pk() && request->pk().size() > 0) {
            PDLOG(DEBUG, "tid %u, pid %u seek pk %s ts %lu", 
                    request->tid(), request->pid(), request->pk().c_str(), request->ts());
            it->Seek(request->pk(), request->ts());
            last_pk = request->pk();
            last_time = request->ts();
        } else {
            PDLOG(DEBUG, "tid %u, pid %u seek to first", request->tid(), request->pid());
            it->SeekToFirst();
        }
        std::map<std::string, std::vector<std::pair<uint64_t, rtidb::base::Slice>>> value_map;
        uint32_t total_block_size = 0;
        bool remove_duplicated_record = false;
        if (request->has_enable_remove_duplicated_record()) {
            remove_duplicated_record = request->enable_remove_duplicated_record();
        }
        uint32_t scount = 0;
        for (;it->Valid(); it->Next()) {
            if (request->limit() > 0 && scount > request->limit() - 1) {
                PDLOG(DEBUG, "reache the limit %u ", request->limit());
                break;
            }
            PDLOG(DEBUG, "traverse pk %s ts %lu", it->GetPK().c_str(), it->GetKey());
            // skip duplicate record
            if (remove_duplicated_record && last_time == it->GetKey() && last_pk == it->GetPK()) {
                PDLOG(DEBUG, "filter duplicate record for key %s with ts %lu", last_pk.c_str(), last_time);
                continue;
            }
            last_pk = it->GetPK();
            last_time = it->GetKey();
            if (value_map.find(last_pk) == value_map.end()) {
                value_map.insert(std::make_pair(last_pk, std::vector<std::pair<uint64_t, rtidb::base::Slice>>()));
                value_map[last_pk].reserve(request->limit());
            }
            rtidb::base::Slice value = it->GetValue();
            value_map[last_pk].push_back(std::make_pair(it->GetKey(), value));
            total_block_size += last_pk.length() + value.size();
            scount ++;
            if (it->GetCount() >= FLAGS_max_traverse_cnt) {
                PDLOG(DEBUG, "traverse cnt %lu max %lu, key %s ts %lu", 
                        it->GetCount(), FLAGS_max_traverse_cnt, last_pk.c_str(), last_time);
                break;
            }
        }
        bool is_finish = false;
        if (it->GetCount() >= FLAGS_max_traverse_cnt) {
            PDLOG(DEBUG, "traverse cnt %lu is great than max %lu, key %s ts %lu", 
                    it->GetCount(), FLAGS_max_traverse_cnt, last_pk.c_str(), last_time);
            last_pk = it->GetPK();
            last_time = it->GetKey();
            if (last_pk.empty()) {
                is_finish = true;
            }
        } else if (scount < request->limit()) {
            is_finish = true;
        }
        delete it;
        uint32_t total_size = scount * (8+4+4) + total_block_size;
        std::string* pairs = response->mutable_pairs();
        if (scount <= 0) {
            pairs->resize(0);
        } else {
            pairs->resize(total_size);
        }
        char* rbuffer = reinterpret_cast<char*>(& ((*pairs)[0]));
        uint32_t offset = 0;
        for (const auto& kv : value_map) {
            for (const auto& pair : kv.second) {
                PDLOG(DEBUG, "encode pk %s ts %lu size %u", kv.first.c_str(), pair.first, pair.second.size());
                ::rtidb::base::EncodeFull(kv.first, pair.first, pair.second.data(), pair.second.size(), rbuffer, offset);
                offset += (4 + 4 + 8 + kv.first.length() + pair.second.size());
            }
        }
        PDLOG(DEBUG, "traverse count %d. last_pk %s last_time %lu", scount, last_pk.c_str(), last_time);
        response->set_code(::rtidb::base::ReturnCode::kOk);
        response->set_count(scount);
        response->set_pk(last_pk);
        response->set_ts(last_time);
        response->set_is_finish(is_finish);
    } else {
        uint32_t index = 0;
        rtidb::storage::RelationalTableTraverseIterator* it =
            r_table->NewTraverse(index);
        if (it == NULL) {
            response->set_code(::rtidb::base::ReturnCode::kIdxNameNotFound);
            response->set_msg("idx name not found");
        }
        if (request->has_pk()) {
            it->Seek(request->pk());
            it->Next();
        } else {
            it->SeekToFirst();
        }
        uint32_t scount = 0;
        std::vector<rtidb::base::Slice> value_vec;
        uint32_t total_block_size = 0;
        for (; it->Valid(); it->Next()) {
            if (request->limit() > 0 && scount > request->limit() - 1) {
                PDLOG(DEBUG, "reache the limit %u", request->limit());
                break;
            }
            rtidb::base::Slice value = it->GetValue();
            total_block_size += value.size();
            value_vec.push_back(value);
            scount++;
            if (it->GetCount() >= FLAGS_max_traverse_cnt) {
                PDLOG(DEBUG, "traverse cnt %lu max %lu",
                      it->GetCount(), FLAGS_max_traverse_cnt);
                break;
            }
        }

        bool is_finish = false;
        if (!it->Valid()) {
            is_finish = true;
        }
        delete it;
        uint32_t total_size = scount * 4 + total_block_size;
        std::string* pairs = response->mutable_pairs();
        if (scount <= 0) {
            pairs->resize(0);
        } else {
            pairs->resize(total_size);
        }
        char* rbuffer = reinterpret_cast<char*>(&((*pairs)[0]));
        uint32_t offset = 0;
        for (const auto& value : value_vec) {
            rtidb::base::Encode(value.data(), value.size(), rbuffer, offset);
            offset += (4 + value.size());
        }
        PDLOG(DEBUG, "tid %u pid %u, traverse count %d.", request->tid(), request->pid(), scount);
        response->set_code(0);
        response->set_count(scount);
        response->set_is_finish(is_finish);
    }
}

void TabletImpl::Delete(RpcController* controller,
        const ::rtidb::api::DeleteRequest* request,
        ::rtidb::api::GeneralResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    if (follower_.load(std::memory_order_relaxed)) {
        response->set_code(::rtidb::base::ReturnCode::kIsFollowerCluster);
        response->set_msg("is follower cluster");
        return;
    }
    std::shared_ptr<Table> table = GetTable(request->tid(), request->pid());
    std::shared_ptr<RelationalTable> r_table;
    if (!table) {
        std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
        r_table = GetRelationalTableUnLock(request->tid(), request->pid());
        if (!r_table) {
            PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(), request->pid());
            response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
            response->set_msg("table is not exist");
            return;
        }
    }
    if (table) {
        if (!table->IsLeader()) {
            PDLOG(DEBUG, "table is follower. tid %u, pid %u", request->tid(),
                    request->pid());
            response->set_code(::rtidb::base::ReturnCode::kTableIsFollower);
            response->set_msg("table is follower");
            return;
        }
        if (table->GetTableStat() == ::rtidb::storage::kLoading) {
            PDLOG(WARNING, "table is loading. tid %u, pid %u", request->tid(), request->pid());
            response->set_code(::rtidb::base::ReturnCode::kTableIsLoading);
            response->set_msg("table is loading");
            return;
        }
        uint32_t idx = 0;
        if (request->has_idx_name() && request->idx_name().size() > 0) {
            std::shared_ptr<IndexDef> index_def = table->GetIndex(request->idx_name());
            if (!index_def || !index_def->IsReady()) {
                PDLOG(WARNING, "idx name %s not found in table tid %u, pid %u", request->idx_name().c_str(),
                        request->tid(), request->pid());
                response->set_code(::rtidb::base::ReturnCode::kIdxNameNotFound);
                response->set_msg("idx name not found");
                return;
            }
            idx = index_def->GetId();
        }
        if (table->Delete(request->key(), idx)) {
            response->set_code(::rtidb::base::ReturnCode::kOk);
            response->set_msg("ok");
            PDLOG(DEBUG, "delete ok. tid %u, pid %u, key %s", request->tid(), request->pid(), request->key().c_str());
        } else {
            response->set_code(::rtidb::base::ReturnCode::kDeleteFailed);
            response->set_msg("delete failed");
            return;
        }
        std::shared_ptr<LogReplicator> replicator;
        do {
            replicator = GetReplicator(request->tid(), request->pid());
            if (!replicator) {
                PDLOG(WARNING, "fail to find table tid %u pid %u leader's log replicator", request->tid(),
                        request->pid());
                break;
            }
            ::rtidb::api::LogEntry entry;
            entry.set_term(replicator->GetLeaderTerm());
            entry.set_method_type(::rtidb::api::MethodType::kDelete);
            ::rtidb::api::Dimension* dimension = entry.add_dimensions();
            dimension->set_key(request->key());
            dimension->set_idx(idx);
            replicator->AppendEntry(entry);
        } while(false);
        if (replicator && FLAGS_binlog_notify_on_put) {
            replicator->Notify();
        }
        return;
    } else {
        uint32_t idx = 0;
        /**
        if (request->has_idx_name() && request->idx_name().size() > 0) {
            std::map<std::string, uint32_t>::iterator iit = table->GetMapping().find(request->idx_name());
            if (iit == table->GetMapping().end()) {
                PDLOG(WARNING, "idx name %s not found in table tid %u, pid %u", request->idx_name().c_str(),
                        request->tid(), request->pid());
                response->set_code(::rtidb::base::ReturnCode::kIdxNameNotFound);
                response->set_msg("idx name not found");
                return;
            }
            idx = iit->second;
        }
        */
        if (r_table->Delete(request->key(), idx)) {
            response->set_code(::rtidb::base::ReturnCode::kOk);
            response->set_msg("ok");
            PDLOG(DEBUG, "delete ok. tid %u, pid %u, key %s", request->tid(), request->pid(), request->key().c_str());
        } else {
            response->set_code(::rtidb::base::ReturnCode::kDeleteFailed);
            response->set_msg("delete failed");
            return;
        }
    }
}

void TabletImpl::BatchQuery(RpcController* controller,
                const rtidb::api::BatchQueryRequest* request,
                rtidb::api::BatchQueryResponse* response,
                Closure*done) {
    brpc::ClosureGuard done_guard(done);
    if (request->query_key_size() < 1) {
        response->set_code(::rtidb::base::ReturnCode::kOk);
        return;
    }
    uint32_t tid = request->tid();
    uint32_t pid = request->pid();
    std::shared_ptr<RelationalTable> r_table;
    {
        std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
        r_table = GetRelationalTableUnLock(tid, pid);
    }
    if (!r_table) {
        PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(), request->pid());
        response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
        response->set_msg("table is not exist");
        return;
    }
    uint32_t index = 0;
    rtidb::storage::RelationalTableTraverseIterator* it =
            r_table->NewTraverse(index);
    if (it == NULL) {
        response->set_code(::rtidb::base::ReturnCode::kIdxNameNotFound);
        response->set_msg("idx name not found");
        return;
    }
    std::vector<rtidb::base::Slice> value_vec;
    uint32_t total_block_size = 0;

    uint32_t scount = 0;
    uint32_t not_found_count = 0;
    for (auto& key : request->query_key()) {
        it->Seek(key);
        scount++;
        if (!it->Valid()) {
            not_found_count++;
            continue;
        }
        rtidb::base::Slice value = it->GetValue();

        total_block_size += value.size();
        value_vec.push_back(value);
        if (scount >= FLAGS_max_traverse_cnt) {
            PDLOG(DEBUG, "batchquery cnt %lu max %lu",
                  scount, FLAGS_max_traverse_cnt);
            break;
        }
    }

    delete it;
    if (total_block_size == 0) {
        PDLOG(DEBUG, "tid %u pid %u, batchQuery not key found.", request->tid(), request->pid());
        response->set_code(rtidb::base::ReturnCode::kOk);
        response->set_is_finish(true);
    }
    bool is_finish = false;
    if (static_cast<uint64_t>(scount) == static_cast<uint64_t>(request->query_key_size())) {
        is_finish = true;
    }
    uint32_t total_size = (scount - not_found_count) * 4 + total_block_size;
    std::string* pairs = response->mutable_pairs();
    if (scount <= 0) {
        pairs->resize(0);
    } else {
        pairs->resize(total_size);
    }
    char* rbuffer = reinterpret_cast<char*>(&((*pairs)[0]));
    uint32_t offset = 0;
    for (const auto& value : value_vec) {
        rtidb::base::Encode(value.data(), value.size(), rbuffer, offset);
        offset += (4 + value.size());
    }
    PDLOG(DEBUG, "tid %u pid %u, batchQuery count %d.", request->tid(), request->pid(), scount);
    response->set_code(rtidb::base::ReturnCode::kOk);
    response->set_is_finish(is_finish);
    response->set_count(scount);
}

void TabletImpl::ChangeRole(RpcController* controller,
        const ::rtidb::api::ChangeRoleRequest* request,
        ::rtidb::api::ChangeRoleResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    uint32_t tid = request->tid();
    uint32_t pid = request->pid();
    std::shared_ptr<Table> table = GetTable(tid, pid);
    if (!table) {
        response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
        response->set_msg("table is not exist");
        return;
    }
    if (table->GetTableStat() != ::rtidb::storage::kNormal) {
        PDLOG(WARNING, "table state[%u] can not change role. tid[%u] pid[%u]", 
                table->GetTableStat(), tid, pid);
        response->set_code(::rtidb::base::ReturnCode::kTableStatusIsNotKnormal);
        response->set_msg("table status is not kNormal");
        return;
    }
    std::shared_ptr<LogReplicator> replicator = GetReplicator(tid, pid);
    if (!replicator) {
        response->set_code(::rtidb::base::ReturnCode::kReplicatorIsNotExist);
        response->set_msg("replicator is not exist");
        return;
    }
    bool is_leader = false;
    if (request->mode() == ::rtidb::api::TableMode::kTableLeader) {
        is_leader = true;
    }
    std::vector<std::string> vec;
    for (int idx = 0; idx < request->replicas_size(); idx++) {
        vec.push_back(request->replicas(idx).c_str());
    }
    if (is_leader) {
        {
            std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
            if (table->IsLeader()) {
                PDLOG(WARNING, "table is leader. tid[%u] pid[%u]", tid, pid);
                response->set_code(::rtidb::base::ReturnCode::kTableIsLeader);
                response->set_msg("table is leader");
                return ;
            }
            PDLOG(INFO, "change to leader. tid[%u] pid[%u] term[%lu]", tid, pid, request->term());
            table->SetLeader(true);
            replicator->SetRole(ReplicatorRole::kLeaderNode);
            if (!FLAGS_zk_cluster.empty()) {
                replicator->SetLeaderTerm(request->term());
            }
        }
        if (replicator->AddReplicateNode(vec) < 0) {
            PDLOG(WARNING,"add replicator failed. tid[%u] pid[%u]", tid, pid);
        }
        for (auto& e : request->endpoint_tid()) {
            std::vector<std::string> endpoints{e.endpoint()};
            replicator->AddReplicateNode(endpoints, e.tid());
        }
    } else {
        std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
        if (!table->IsLeader()) {
            PDLOG(WARNING, "table is follower. tid[%u] pid[%u]", tid, pid);
            response->set_code(::rtidb::base::ReturnCode::kOk);
            response->set_msg("table is follower");
            return;
        }
        replicator->DelAllReplicateNode();
        replicator->SetRole(ReplicatorRole::kFollowerNode);
        table->SetLeader(false);
        PDLOG(INFO, "change to follower. tid[%u] pid[%u]", tid, pid);
    }
    response->set_code(::rtidb::base::ReturnCode::kOk);
    response->set_msg("ok");
}

void TabletImpl::AddReplica(RpcController* controller, 
        const ::rtidb::api::ReplicaRequest* request,
        ::rtidb::api::AddReplicaResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);        
    std::shared_ptr<::rtidb::api::TaskInfo> task_ptr;
    if (request->has_task_info() && request->task_info().IsInitialized()) {
        if (AddOPMultiTask(request->task_info(), ::rtidb::api::TaskType::kAddReplica, task_ptr) < 0) {
            response->set_code(-1);
            response->set_msg("add task failed");
            return;
        }
    }
    std::shared_ptr<Table> table = GetTable(request->tid(), request->pid());
    do {
        if (!table) {
            PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(),
                    request->pid());
            response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
            response->set_msg("table is not exist");
            break;
        }
        if (!table->IsLeader()) {
            PDLOG(WARNING, "table is follower. tid %u, pid %u", request->tid(), request->pid());
            response->set_code(::rtidb::base::ReturnCode::kTableIsFollower);
            response->set_msg("table is follower");
            break;
        }
        std::shared_ptr<LogReplicator> replicator = GetReplicator(request->tid(), request->pid());
        if (!replicator) {
            response->set_code(::rtidb::base::ReturnCode::kReplicatorIsNotExist);
            response->set_msg("replicator is not exist");
            PDLOG(WARNING,"replicator is not exist. tid %u, pid %u", request->tid(), request->pid());
            break;
        }
        std::vector<std::string> vec;
        vec.push_back(request->endpoint());
        int ret = -1;
        if (request->has_remote_tid()) {
            ret = replicator->AddReplicateNode(vec, request->remote_tid());
        } else {
            ret = replicator->AddReplicateNode(vec);
        }
        if (ret == 0) {
            response->set_code(::rtidb::base::ReturnCode::kOk);
            response->set_msg("ok");
        } else if (ret < 0) {
            response->set_code(::rtidb::base::ReturnCode::kFailToAddReplicaEndpoint);
            PDLOG(WARNING, "fail to add replica endpoint. tid %u pid %u", request->tid(), request->pid());
            response->set_msg("fail to add replica endpoint");
            break;
        } else {
            response->set_code(::rtidb::base::ReturnCode::kReplicaEndpointAlreadyExists);
            response->set_msg("replica endpoint already exists");
            PDLOG(WARNING, "replica endpoint already exists. tid %u pid %u", request->tid(), request->pid());
        }
        if (task_ptr) {
            std::lock_guard<std::mutex> lock(mu_);
            task_ptr->set_status(::rtidb::api::TaskStatus::kDone);
        }
        return;
    } while(0);
    if (task_ptr) {
        std::lock_guard<std::mutex> lock(mu_);
        task_ptr->set_status(::rtidb::api::TaskStatus::kFailed);
    }
}

void TabletImpl::DelReplica(RpcController* controller, 
        const ::rtidb::api::ReplicaRequest* request,
        ::rtidb::api::GeneralResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);        
    std::shared_ptr<::rtidb::api::TaskInfo> task_ptr;
    if (request->has_task_info() && request->task_info().IsInitialized()) {
        if (AddOPTask(request->task_info(), ::rtidb::api::TaskType::kDelReplica, task_ptr) < 0) {
            response->set_code(-1);
            response->set_msg("add task failed");
            return;
        }
    }
    std::shared_ptr<Table> table = GetTable(request->tid(), request->pid());
    do {
        if (!table) {
            PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(),
                    request->pid());
            response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
            response->set_msg("table is not exist");
            break;
        }
        if (!table->IsLeader()) {
            PDLOG(WARNING, "table is follower. tid %u, pid %u", request->tid(), request->pid());
            response->set_code(::rtidb::base::ReturnCode::kTableIsFollower);
            response->set_msg("table is follower");
            break;
        }
        std::shared_ptr<LogReplicator> replicator = GetReplicator(request->tid(), request->pid());
        if (!replicator) {
            response->set_code(::rtidb::base::ReturnCode::kReplicatorIsNotExist);
            response->set_msg("replicator is not exist");
            PDLOG(WARNING,"replicator is not exist. tid %u, pid %u", request->tid(), request->pid());
            break;
        }
        int ret = replicator->DelReplicateNode(request->endpoint());
        if (ret == 0) {
            response->set_code(::rtidb::base::ReturnCode::kOk);
            response->set_msg("ok");
        } else if (ret < 0) {
            response->set_code(::rtidb::base::ReturnCode::kReplicatorRoleIsNotLeader);
            PDLOG(WARNING, "replicator role is not leader. table %u pid %u", request->tid(), request->pid());
            response->set_msg("replicator role is not leader");
            break;
        } else {
            response->set_code(::rtidb::base::ReturnCode::kOk);
            PDLOG(WARNING, "fail to del endpoint for table %u pid %u. replica does not exist", 
                    request->tid(), request->pid());
            response->set_msg("replica does not exist");
        }
        if (task_ptr) {
            std::lock_guard<std::mutex> lock(mu_);
            task_ptr->set_status(::rtidb::api::TaskStatus::kDone);
        }
        return;
    } while (0);
    if (task_ptr) {
        std::lock_guard<std::mutex> lock(mu_);
        task_ptr->set_status(::rtidb::api::TaskStatus::kFailed);
    }
}

void TabletImpl::AppendEntries(RpcController* controller,
        const ::rtidb::api::AppendEntriesRequest* request,
        ::rtidb::api::AppendEntriesResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<Table> table = GetTable(request->tid(), request->pid());
    if (!table) {
        PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(),
                request->pid());
        response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
        response->set_msg("table is not exist");
        return;
    }
    if (!follower_.load(std::memory_order_relaxed) && table->IsLeader()) {
        PDLOG(WARNING, "table is leader. tid %u, pid %u", request->tid(), request->pid());
        response->set_code(::rtidb::base::ReturnCode::kTableIsLeader);
        response->set_msg("table is leader");
        return;
    }
    if (table->GetTableStat() == ::rtidb::storage::kLoading) {
        response->set_code(::rtidb::base::ReturnCode::kTableIsLoading);
        response->set_msg("table is loading");
        PDLOG(WARNING, "table is loading. tid %u, pid %u", request->tid(), request->pid());
        return;
    }    
    std::shared_ptr<LogReplicator> replicator = GetReplicator(request->tid(), request->pid());
    if (!replicator) {
        response->set_code(::rtidb::base::ReturnCode::kReplicatorIsNotExist);
        response->set_msg("replicator is not exist");
        return;
    }
    bool ok = replicator->AppendEntries(request, response);
    if (!ok) {
        response->set_code(::rtidb::base::ReturnCode::kFailToAppendEntriesToReplicator);
        response->set_msg("fail to append entries to replicator");
    } else {
        response->set_code(::rtidb::base::ReturnCode::kOk);
        response->set_msg("ok");
    }
}

void TabletImpl::GetTableSchema(RpcController* controller,
        const ::rtidb::api::GetTableSchemaRequest* request,
        ::rtidb::api::GetTableSchemaResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);        
    std::shared_ptr<Table> table = GetTable(request->tid(), request->pid());
    if (!table) {
        response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
        response->set_msg("table is not exist");
        PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(),
                request->pid());
        return;
    } else {
        response->set_schema(table->GetSchema());
    }
    response->set_code(::rtidb::base::ReturnCode::kOk);
    response->set_msg("ok");
    response->set_schema(table->GetSchema());
    response->mutable_table_meta()->CopyFrom(table->GetTableMeta());
}

void TabletImpl::UpdateTableMetaForAddField(RpcController* controller,
        const ::rtidb::api::UpdateTableMetaForAddFieldRequest* request,
        ::rtidb::api::GeneralResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    uint32_t tid = request->tid();
    std::map<uint32_t, std::shared_ptr<Table>> table_map;
    {
        std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
        auto it = tables_.find(tid);
        if (it == tables_.end()) {
            response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
            response->set_msg("table doesn`t exist");
            PDLOG(WARNING, "table tid %u doesn`t exist.", tid);
            return;
        }
        table_map = it->second;
    }
    for (auto pit = table_map.begin(); pit != table_map.end(); ++pit) {
        uint32_t pid = pit->first;
        std::shared_ptr<Table> table = pit->second;
        //judge if field exists
        bool repeated = false;
        std::string col_name = request->column_desc().name();
        for (const auto& column : table->GetTableMeta().column_desc()) {
            if (column.name() == col_name) {
                PDLOG(WARNING, "field name[%s] repeated in tablet!", col_name.c_str());
                repeated = true;
                break;
            } 
        }
        if (!repeated) {
            for (const auto& column : table->GetTableMeta().added_column_desc()) {
                if (column.name() == col_name) {
                    PDLOG(WARNING, "field name[%s] repeated in tablet!", col_name.c_str());
                    repeated = true;
                    break;
                } 
            }
        }
        if (repeated) {
            continue;
        }
        ::rtidb::api::TableMeta table_meta;
        table_meta.CopyFrom(table->GetTableMeta());
        ::rtidb::common::ColumnDesc* column_desc = table_meta.add_added_column_desc();
        column_desc->CopyFrom(request->column_desc());
        table_meta.set_schema(request->schema());
        table->SetTableMeta(table_meta);
        table->SetSchema(request->schema());
        //update TableMeta.txt
        std::string db_root_path;
        ::rtidb::common::StorageMode mode = table_meta.storage_mode();
        bool ok = ChooseDBRootPath(tid, pid, mode, db_root_path);
        if (!ok) {
            response->set_code(::rtidb::base::ReturnCode::kFailToGetDbRootPath);
            response->set_msg("fail to get db root path");
            PDLOG(WARNING, "fail to get table db root path for tid %u, pid %u", tid, pid);
            return;
        }
        std::string db_path = db_root_path + "/" + std::to_string(tid) + 
            "_" + std::to_string(pid);
        if (!::rtidb::base::IsExists(db_path)) {
            PDLOG(WARNING, "table db path doesn`t exist. tid %u, pid %u", tid, pid);
            response->set_code(::rtidb::base::ReturnCode::kTableDbPathIsNotExist);
            response->set_msg("table db path is not exist");
            return;
        }
        UpdateTableMeta(db_path, &table_meta, true);
        if (WriteTableMeta(db_path, &table_meta) < 0) {
            PDLOG(WARNING, "write table_meta failed. tid[%u] pid[%u]", tid, pid);
            response->set_code(::rtidb::base::ReturnCode::kWriteDataFailed);
            response->set_msg("write data failed");
            return;
        }
    }
    response->set_code(::rtidb::base::ReturnCode::kOk);
    response->set_msg("ok");
}

void TabletImpl::GetTableStatus(RpcController* controller,
        const ::rtidb::api::GetTableStatusRequest* request,
        ::rtidb::api::GetTableStatusResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
    for (auto it = tables_.begin(); it != tables_.end(); ++it) {
        if (request->has_tid() && request->tid() != it->first) {
            continue;
        }
        for (auto pit = it->second.begin(); pit != it->second.end(); ++pit) {
            if (request->has_pid() && request->pid() != pit->first) {
                continue;
            }
            std::shared_ptr<Table> table = pit->second;
            ::rtidb::api::TableStatus* status = response->add_all_table_status();
            status->set_mode(::rtidb::api::TableMode::kTableFollower);
            if (table->IsLeader()) {
                status->set_mode(::rtidb::api::TableMode::kTableLeader);
            }
            status->set_tid(table->GetId());
            status->set_pid(table->GetPid());
            status->set_compress_type(table->GetCompressType());
            status->set_storage_mode(table->GetStorageMode());
            status->set_name(table->GetName());
            ::rtidb::api::TTLDesc* ttl_desc = status->mutable_ttl_desc();
            ::rtidb::storage::TTLDesc ttl = table->GetTTL();
            ttl_desc->set_abs_ttl(ttl.abs_ttl);
            ttl_desc->set_lat_ttl(ttl.lat_ttl);
            ttl_desc->set_ttl_type(table->GetTTLType());
            status->set_ttl_type(table->GetTTLType());
            status->set_diskused(table->GetDiskused());
            if (status->ttl_type() == ::rtidb::api::TTLType::kLatestTime) {
                status->set_ttl(table->GetTTL().lat_ttl);
            } else {
                status->set_ttl(table->GetTTL().abs_ttl);
            }
            if (::rtidb::api::TableState_IsValid(table->GetTableStat())) {
                status->set_state(::rtidb::api::TableState(table->GetTableStat()));
            }
            std::shared_ptr<LogReplicator> replicator = GetReplicatorUnLock(table->GetId(), table->GetPid());
            if (replicator) {
                status->set_offset(replicator->GetOffset());
            }
            status->set_record_cnt(table->GetRecordCnt());
            if (table->GetStorageMode() == ::rtidb::common::StorageMode::kMemory) {
                if (MemTable* mem_table = dynamic_cast<MemTable*>(table.get())) {
                    status->set_time_offset(mem_table->GetTimeOffset());
                    status->set_is_expire(mem_table->GetExpireStatus());
                    status->set_record_byte_size(mem_table->GetRecordByteSize());
                    status->set_record_idx_byte_size(mem_table->GetRecordIdxByteSize());
                    status->set_record_pk_cnt(mem_table->GetRecordPkCnt());
                    status->set_skiplist_height(mem_table->GetKeyEntryHeight());
                    uint64_t record_idx_cnt = 0;
                    auto indexs = table->GetAllIndex();
                    for (const auto& index_def : indexs) {
                        ::rtidb::api::TsIdxStatus* ts_idx_status = status->add_ts_idx_status();
                        ts_idx_status->set_idx_name(index_def->GetName());
                        uint64_t* stats = NULL;
                        uint32_t size = 0;
                        bool ok = mem_table->GetRecordIdxCnt(index_def->GetId(), &stats, &size);
                        if (ok) {
                            for (uint32_t i = 0; i < size; i++) {
                                ts_idx_status->add_seg_cnts(stats[i]); 
                                record_idx_cnt += stats[i];
                            }
                        }
                        delete stats;
                    }
                    status->set_idx_cnt(record_idx_cnt);
                }
            }
            if (request->has_need_schema() && request->need_schema()) {
                status->set_schema(table->GetSchema());
            }
        }
    }
    response->set_code(::rtidb::base::ReturnCode::kOk);
}

void TabletImpl::SetExpire(RpcController* controller,
        const ::rtidb::api::SetExpireRequest* request,
        ::rtidb::api::GeneralResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);        
    std::shared_ptr<Table> table = GetTable(request->tid(), request->pid());
    if (!table) {
        PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(), request->pid());
        response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
        response->set_msg("table is not exist");
        return;
    }
    if (table->GetStorageMode() == ::rtidb::common::StorageMode::kMemory) {
        MemTable* mem_table = dynamic_cast<MemTable*>(table.get());
        if (mem_table != NULL) {
            mem_table->SetExpire(request->is_expire());
            PDLOG(INFO, "set table expire[%d]. tid[%u] pid[%u]", request->is_expire(), request->tid(), request->pid());
        }
    }
    response->set_code(::rtidb::base::ReturnCode::kOk);
    response->set_msg("ok");
}

void TabletImpl::SetTTLClock(RpcController* controller,
        const ::rtidb::api::SetTTLClockRequest* request,
        ::rtidb::api::GeneralResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);        
    std::shared_ptr<Table> table = GetTable(request->tid(), request->pid());
    if (!table) {
        PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(), request->pid());
        response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
        response->set_msg("table is not exist");
        return;
    }
    if (table->GetStorageMode() == ::rtidb::common::StorageMode::kMemory) {
        MemTable* mem_table = dynamic_cast<MemTable*>(table.get());
        if (mem_table != NULL) {
            int64_t cur_time = ::baidu::common::timer::get_micros() / 1000000;
            int64_t offset = (int64_t)request->timestamp() - cur_time;
            mem_table->SetTimeOffset(offset);
            PDLOG(INFO, "set table virtual timestamp[%lu] cur timestamp[%lu] offset[%ld]. tid[%u] pid[%u]", 
                    request->timestamp(), cur_time, offset, request->tid(), request->pid());
        }
    }
    response->set_code(::rtidb::base::ReturnCode::kOk);
    response->set_msg("ok");
}

void TabletImpl::MakeSnapshotInternal(uint32_t tid, uint32_t pid, uint64_t end_offset, std::shared_ptr<::rtidb::api::TaskInfo> task) {
    std::shared_ptr<Table> table;
    std::shared_ptr<Snapshot> snapshot;
    std::shared_ptr<LogReplicator> replicator;
    bool has_error = true;
    do {
        std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
        table = GetTableUnLock(tid, pid);
        if (!table) {
            PDLOG(WARNING, "table is not exist. tid[%u] pid[%u]", tid, pid);
            break;
        }
        if (table->GetTableStat() != ::rtidb::storage::kNormal) {
            PDLOG(WARNING, "table state is %d, cannot make snapshot. %u, pid %u", 
                    table->GetTableStat(), tid, pid);
            break;
        }    
        snapshot = GetSnapshotUnLock(tid, pid);
        if (!snapshot) {
            PDLOG(WARNING, "snapshot is not exist. tid[%u] pid[%u]", tid, pid);
            break;
        }
        replicator = GetReplicatorUnLock(tid, pid);
        if (!replicator) {
            PDLOG(WARNING, "replicator is not exist. tid[%u] pid[%u]", tid, pid);
            break;
        }
        has_error = false;
    } while (0);
    if (has_error) {
        if (task) {
            std::lock_guard<std::mutex> lock(mu_);
            task->set_status(::rtidb::api::kFailed);
        }    
        return;
    }
    {
        std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
        table->SetTableStat(::rtidb::storage::kMakingSnapshot);
    }
    uint64_t cur_offset = replicator->GetOffset();
    uint64_t snapshot_offset = snapshot->GetOffset();
    int ret = 0;
    if (cur_offset < snapshot_offset + FLAGS_make_snapshot_threshold_offset && end_offset == 0) {
        PDLOG(INFO, "offset can't reach the threshold. tid[%u] pid[%u] cur_offset[%lu], snapshot_offset[%lu] end_offset[%lu]",
                tid, pid, cur_offset, snapshot_offset, end_offset);
    } else {
        if (table->GetStorageMode() != ::rtidb::common::StorageMode::kMemory) {
            ::rtidb::storage::DiskTableSnapshot* disk_snapshot = 
                dynamic_cast<::rtidb::storage::DiskTableSnapshot*>(snapshot.get());
            if (disk_snapshot != NULL) {
                disk_snapshot->SetTerm(replicator->GetLeaderTerm());
            }    
        }
        uint64_t offset = 0;
        ret = snapshot->MakeSnapshot(table, offset, end_offset);
        if (ret == 0) {
            std::shared_ptr<LogReplicator> replicator = GetReplicator(tid, pid);
            if (replicator) {
                replicator->SetSnapshotLogPartIndex(offset);
            }
        }
    }
    {
        std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
        table->SetTableStat(::rtidb::storage::kNormal);
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (task) {
            if (ret == 0) {
                task->set_status(::rtidb::api::kDone);
                if (table->GetStorageMode() == common::StorageMode::kMemory) {
                    auto right_now = std::chrono::system_clock::now().time_since_epoch();
                    int64_t ts = std::chrono::duration_cast<std::chrono::seconds>(right_now).count();
                    table->SetMakeSnapshotTime(ts);
                }
            } else {
                task->set_status(::rtidb::api::kFailed);
            }    
        }
    }
}

void TabletImpl::MakeSnapshot(RpcController* controller,
        const ::rtidb::api::GeneralRequest* request,
        ::rtidb::api::GeneralResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<::rtidb::api::TaskInfo> task_ptr;
    if (request->has_task_info() && request->task_info().IsInitialized()) {
        if (AddOPTask(request->task_info(), ::rtidb::api::TaskType::kMakeSnapshot, task_ptr) < 0) {
            response->set_code(-1);
            response->set_msg("add task failed");
            return;
        }
    }    
    uint32_t tid = request->tid();        
    uint32_t pid = request->pid();
    uint64_t offset = 0;
    if (request->has_offset() && request->offset() > 0) {
        offset = request->offset();
    }
    do {
        {
            std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
            std::shared_ptr<Snapshot> snapshot = GetSnapshotUnLock(tid, pid);
            if (!snapshot) {
                response->set_code(::rtidb::base::ReturnCode::kSnapshotIsNotExist);
                response->set_msg("snapshot is not exist");
                PDLOG(WARNING, "snapshot is not exist. tid[%u] pid[%u]", tid, pid);
                break;
            }
            std::shared_ptr<Table> table = GetTableUnLock(request->tid(), request->pid());
            if (!table) {
                PDLOG(WARNING, "table is not exist. tid %u, pid %u", tid, pid);
                response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
                response->set_msg("table is not exist");
                break;
            }
            if (table->GetTableStat() != ::rtidb::storage::kNormal) {
                response->set_code(::rtidb::base::ReturnCode::kTableStatusIsNotKnormal);
                response->set_msg("table status is not kNormal");
                PDLOG(WARNING, "table state is %d, cannot make snapshot. %u, pid %u", 
                        table->GetTableStat(), tid, pid);
                break;
            }
        }
        std::lock_guard<std::mutex> lock(mu_);
        if (task_ptr) {
            task_ptr->set_status(::rtidb::api::TaskStatus::kDoing);
        }    
        snapshot_pool_.AddTask(boost::bind(&TabletImpl::MakeSnapshotInternal, this, tid, pid, offset, task_ptr));
        response->set_code(::rtidb::base::ReturnCode::kOk);
        response->set_msg("ok");
        return;
    } while (0);
    if (task_ptr) {       
        std::lock_guard<std::mutex> lock(mu_);
        task_ptr->set_status(::rtidb::api::TaskStatus::kFailed);
    }
}

void TabletImpl::SchedMakeSnapshot() {
    int now_hour = ::rtidb::base::GetNowHour();
    if (now_hour != FLAGS_make_snapshot_time) {
        snapshot_pool_.DelayTask(FLAGS_make_snapshot_check_interval, boost::bind(&TabletImpl::SchedMakeSnapshot, this));
        return;
    }
    std::vector<std::pair<uint32_t, uint32_t> > table_set;
    {
        std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
        auto right_now = std::chrono::system_clock::now().time_since_epoch();
        int64_t ts = std::chrono::duration_cast<std::chrono::seconds>(right_now).count();
        for (auto iter = tables_.begin(); iter != tables_.end(); ++iter) {
            for (auto inner = iter->second.begin(); inner != iter->second.end(); ++ inner) {
                if (iter->first == 0 && inner->first == 0) {
                    continue;
                }
                if (inner->second->GetStorageMode() == ::rtidb::common::StorageMode::kMemory) {
                    if (ts - inner->second->GetMakeSnapshotTime() <= FLAGS_make_snapshot_offline_interval && !FLAGS_zk_cluster.empty()) {
                        continue;
                    }
                    table_set.push_back(std::make_pair(iter->first, inner->first));
                }
            }
        }
    }
    for (auto iter = table_set.begin(); iter != table_set.end(); ++iter) {
        PDLOG(INFO, "start make snapshot tid[%u] pid[%u]", iter->first, iter->second);
        MakeSnapshotInternal(iter->first, iter->second, 0, std::shared_ptr<::rtidb::api::TaskInfo>());
    }
    // delay task one hour later avoid execute  more than one time
    snapshot_pool_.DelayTask(FLAGS_make_snapshot_check_interval + 60 * 60 * 1000, boost::bind(&TabletImpl::SchedMakeSnapshot, this));
}

void TabletImpl::SchedMakeDiskTableSnapshot() {
    std::vector<std::pair<uint32_t, uint32_t> > table_set;
    {
        std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
        for (auto iter = tables_.begin(); iter != tables_.end(); ++iter) {
            for (auto inner = iter->second.begin(); inner != iter->second.end(); ++ inner) {
                if (iter->first == 0 && inner->first == 0) {
                    continue;
                }
                if (inner->second->GetStorageMode() != ::rtidb::common::StorageMode::kMemory) {
                    table_set.push_back(std::make_pair(iter->first, inner->first));
                }
            }
        }
    }
    for (auto iter = table_set.begin(); iter != table_set.end(); ++iter) {
        PDLOG(INFO, "start make snapshot tid[%u] pid[%u]", iter->first, iter->second);
        MakeSnapshotInternal(iter->first, iter->second, 0, std::shared_ptr<::rtidb::api::TaskInfo>());
    }
    // delay task one hour later avoid execute  more than one time
    snapshot_pool_.DelayTask(FLAGS_make_disktable_snapshot_interval * 60 * 1000, boost::bind(&TabletImpl::SchedMakeDiskTableSnapshot, this));
}

void TabletImpl::SendData(RpcController* controller,
        const ::rtidb::api::SendDataRequest* request,
        ::rtidb::api::GeneralResponse* response,
        Closure* done) {

    brpc::ClosureGuard done_guard(done);
    brpc::Controller *cntl = static_cast<brpc::Controller*>(controller);
    uint32_t tid = request->tid(); 
    uint32_t pid = request->pid(); 
    ::rtidb::common::StorageMode mode = ::rtidb::common::kMemory;
    if (request->has_storage_mode()) {
        mode = request->storage_mode();
    }
    std::string db_root_path;
    bool ok = ChooseDBRootPath(tid, pid, mode, db_root_path);
    if (!ok) {
        response->set_code(::rtidb::base::ReturnCode::kFailToGetDbRootPath);
        response->set_msg("fail to get db root path");
        PDLOG(WARNING, "fail to get table db root path for tid %u, pid %u", tid, pid);
        return;
    }
    std::string combine_key = std::to_string(tid) + "_" + std::to_string(pid) + "_" + request->file_name();
    std::shared_ptr<FileReceiver> receiver;
    std::string path = db_root_path + "/" + std::to_string(tid) + "_" + std::to_string(pid) + "/";
    if (request->file_name() != "table_meta.txt") {
        path.append("snapshot/");
    }
    std::string dir_name;
    if (request->has_dir_name() && request->dir_name().size() > 0) {
        dir_name = request->dir_name();
        path.append(request->dir_name() + "/");
    }
    std::shared_ptr<Table> table;
    if (request->block_id() == 0) {
        table = GetTable(tid, pid);
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto iter = file_receiver_map_.find(combine_key);
        if (request->block_id() == 0) {
            if (table) {
                PDLOG(WARNING, "table already exists. tid %u, pid %u", tid, pid);
                response->set_code(::rtidb::base::ReturnCode::kTableAlreadyExists);
                response->set_msg("table already exists");
                return;
            }
            if (iter == file_receiver_map_.end()) {
                file_receiver_map_.insert(std::make_pair(combine_key, 
                            std::make_shared<FileReceiver>(request->file_name(), dir_name, path)));
                iter = file_receiver_map_.find(combine_key);
            }
            if (!iter->second->Init()) {
                PDLOG(WARNING, "file receiver init failed. tid %u, pid %u, file_name %s", tid, pid, request->file_name().c_str());
                response->set_code(::rtidb::base::ReturnCode::kFileReceiverInitFailed);
                response->set_msg("file receiver init failed");
                file_receiver_map_.erase(iter);
                return;
            }
            PDLOG(INFO, "file receiver init ok. tid %u, pid %u, file_name %s", tid, pid, request->file_name().c_str());
            response->set_code(::rtidb::base::ReturnCode::kOk);
            response->set_msg("ok");
        } else if (iter == file_receiver_map_.end()){
            PDLOG(WARNING, "cannot find receiver. tid %u, pid %u, file_name %s", tid, pid, request->file_name().c_str());
            response->set_code(::rtidb::base::ReturnCode::kCannotFindReceiver);
            response->set_msg("cannot find receiver");
            return;
        }
        receiver = iter->second;
    }
    if (!receiver) {
        PDLOG(WARNING, "cannot find receiver. tid %u, pid %u, file_name %s", tid, pid, request->file_name().c_str());
        response->set_code(::rtidb::base::ReturnCode::kCannotFindReceiver);
        response->set_msg("cannot find receiver");
        return;
    }
    if (receiver->GetBlockId() == request->block_id()) {
        response->set_msg("ok");
        response->set_code(::rtidb::base::ReturnCode::kOk);
        return;
    }
    if (request->block_id() != receiver->GetBlockId() + 1) {
        response->set_msg("block_id mismatch");
        PDLOG(WARNING, "block_id mismatch. tid %u, pid %u, file_name %s, request block_id %lu cur block_id %lu", 
                tid, pid, request->file_name().c_str(), request->block_id(), receiver->GetBlockId());
        response->set_code(::rtidb::base::ReturnCode::kBlockIdMismatch);
        return;
    }
    std::string data = cntl->request_attachment().to_string();
    if (data.length() != request->block_size()) {
        PDLOG(WARNING, "receive data error. tid %u, pid %u, file_name %s, expected length %u real length %u", 
                tid, pid, request->file_name().c_str(), request->block_size(), data.length());
        response->set_code(::rtidb::base::ReturnCode::kReceiveDataError);
        response->set_msg("receive data error");
        return;
    }
    if (receiver->WriteData(data, request->block_id()) < 0) {
        PDLOG(WARNING, "receiver write data failed. tid %u, pid %u, file_name %s", tid, pid, request->file_name().c_str());
        response->set_code(::rtidb::base::ReturnCode::kWriteDataFailed);
        response->set_msg("write data failed");
        return;
    }
    if (request->eof()) {
        receiver->SaveFile();
        std::lock_guard<std::mutex> lock(mu_);
        file_receiver_map_.erase(combine_key);
    }
    response->set_msg("ok");
    response->set_code(::rtidb::base::ReturnCode::kOk);
}

void TabletImpl::SendSnapshot(RpcController* controller,
        const ::rtidb::api::SendSnapshotRequest* request,
        ::rtidb::api::GeneralResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<::rtidb::api::TaskInfo> task_ptr;
    if (request->has_task_info() && request->task_info().IsInitialized()) {
        if (AddOPTask(request->task_info(), ::rtidb::api::TaskType::kSendSnapshot, task_ptr) < 0) {
            response->set_code(-1);
            response->set_msg("add task failed");
            return;
        }
    }
    uint32_t tid = request->tid();
    uint32_t pid = request->pid();
    std::string sync_snapshot_key = request->endpoint() + "_" + 
        std::to_string(tid) + "_" + std::to_string(pid);
    do {
        {
            std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
            std::shared_ptr<Table> table = GetTableUnLock(tid, pid);
            if (!table) {
                PDLOG(WARNING, "table is not exist. tid %u, pid %u", tid, pid);
                response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
                response->set_msg("table is not exist");
                break;
            }
            if (!table->IsLeader()) {
                PDLOG(WARNING, "table is follower. tid %u, pid %u", tid, pid);
                response->set_code(::rtidb::base::ReturnCode::kTableIsFollower);
                response->set_msg("table is follower");
                break;
            }
            if (table->GetTableStat() != ::rtidb::storage::kSnapshotPaused) {
                PDLOG(WARNING, "table status is not kSnapshotPaused. tid %u, pid %u", tid, pid);
                response->set_code(::rtidb::base::ReturnCode::kTableStatusIsNotKsnapshotpaused);
                response->set_msg("table status is not kSnapshotPaused");
                break;
            }
        }
        std::lock_guard<std::mutex> lock(mu_);
        if (sync_snapshot_set_.find(sync_snapshot_key) != sync_snapshot_set_.end()) {
            PDLOG(WARNING, "snapshot is sending. tid %u pid %u endpoint %s", 
                    tid, pid, request->endpoint().c_str());
            response->set_code(::rtidb::base::ReturnCode::kSnapshotIsSending);
            response->set_msg("snapshot is sending");
            break;
        }
        if (task_ptr) {
            task_ptr->set_status(::rtidb::api::TaskStatus::kDoing);
        }    
        sync_snapshot_set_.insert(sync_snapshot_key);
        task_pool_.AddTask(boost::bind(&TabletImpl::SendSnapshotInternal, this, 
                    request->endpoint(), tid, pid, request->remote_tid(), task_ptr));
        response->set_code(::rtidb::base::ReturnCode::kOk);
        response->set_msg("ok");
        return;
    } while(0);
    if (task_ptr) {
        std::lock_guard<std::mutex> lock(mu_);
        task_ptr->set_status(::rtidb::api::TaskStatus::kFailed);
    }
}

void TabletImpl::SendSnapshotInternal(const std::string& endpoint, uint32_t tid, uint32_t pid, 
        uint32_t remote_tid, std::shared_ptr<::rtidb::api::TaskInfo> task) {
    bool has_error = true;
    do {
        std::shared_ptr<Table> table = GetTable(tid, pid);
        if (!table) {
            PDLOG(WARNING, "table is not exist. tid %u, pid %u", tid, pid);
            break;
        }
        std::string db_root_path;
        bool ok = ChooseDBRootPath(tid, pid, table->GetStorageMode(), db_root_path);
        if (!ok) {
            PDLOG(WARNING, "fail to get db root path for table tid %u, pid %u", tid, pid);
            break;
        }
        FileSender sender(remote_tid, pid, table->GetStorageMode(), endpoint);
        if (!sender.Init()) {
            PDLOG(WARNING, "Init FileSender failed. tid[%u] pid[%u] endpoint[%s]", tid, pid, endpoint.c_str());
            break;
        }
        // send table_meta file
        std::string full_path = db_root_path + "/" + std::to_string(tid) + "_" + std::to_string(pid) + "/";
        std::string file_name = "table_meta.txt";
        if (sender.SendFile(file_name, full_path + file_name) < 0) {
            PDLOG(WARNING, "send table_meta.txt failed. tid[%u] pid[%u]", tid, pid);
            break;
        }
        full_path.append("snapshot/");
        std::string manifest_file = full_path + "MANIFEST";
        std::string snapshot_file;
        {
            int fd = open(manifest_file.c_str(), O_RDONLY);
            if (fd < 0) {
                PDLOG(WARNING, "[%s] is not exist", manifest_file.c_str());
                has_error = false;
                break;
            }
            google::protobuf::io::FileInputStream fileInput(fd);
            fileInput.SetCloseOnDelete(true);
            ::rtidb::api::Manifest manifest;
            if (!google::protobuf::TextFormat::Parse(&fileInput, &manifest)) {
                PDLOG(WARNING, "parse manifest failed. tid[%u] pid[%u]", tid, pid);
                break;
            }
            snapshot_file = manifest.name();
        }
        if (table->GetStorageMode() == ::rtidb::common::StorageMode::kMemory) {
            // send snapshot file
            if (sender.SendFile(snapshot_file, full_path + snapshot_file) < 0) {
                PDLOG(WARNING, "send snapshot failed. tid[%u] pid[%u]", tid, pid);
                break;
            }
        } else {
            if (sender.SendDir(snapshot_file, full_path + snapshot_file) < 0) {
                PDLOG(WARNING, "send snapshot failed. tid[%u] pid[%u]", tid, pid);
                break;
            }
        }
        // send manifest file
        file_name = "MANIFEST";
        if (sender.SendFile(file_name, full_path + file_name) < 0) {
            PDLOG(WARNING, "send MANIFEST failed. tid[%u] pid[%u]", tid, pid);
            break;
        }
        has_error = false;
        PDLOG(INFO, "send snapshot success. endpoint %s tid %u pid %u", endpoint.c_str(), tid, pid);
    } while(0);
    std::lock_guard<std::mutex> lock(mu_);
    if (task) {
        if (has_error) {
            task->set_status(::rtidb::api::kFailed);
        } else {
            task->set_status(::rtidb::api::kDone);
        }
    }
    std::string sync_snapshot_key = endpoint + "_" + 
        std::to_string(tid) + "_" + std::to_string(pid);
    sync_snapshot_set_.erase(sync_snapshot_key);
}

void TabletImpl::PauseSnapshot(RpcController* controller,
        const ::rtidb::api::GeneralRequest* request,
        ::rtidb::api::GeneralResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);        
    std::shared_ptr<::rtidb::api::TaskInfo> task_ptr;
    if (request->has_task_info() && request->task_info().IsInitialized()) {
        if (AddOPTask(request->task_info(), ::rtidb::api::TaskType::kPauseSnapshot, task_ptr) < 0) {
            response->set_code(-1);
            response->set_msg("add task failed");
            return;
        }
    }    
    do {
        {
            std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
            std::shared_ptr<Table> table = GetTableUnLock(request->tid(), request->pid());
            if (!table) {
                PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(), request->pid());
                response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
                response->set_msg("table is not exist");
                break;
            }
            if (table->GetTableStat() == ::rtidb::storage::kSnapshotPaused) {
                PDLOG(INFO, "table status is kSnapshotPaused, need not pause. tid[%u] pid[%u]", 
                        request->tid(), request->pid());
            } else if (table->GetTableStat() != ::rtidb::storage::kNormal) {
                PDLOG(WARNING, "table status is [%u], cann't pause. tid[%u] pid[%u]", 
                        table->GetTableStat(), request->tid(), request->pid());
                response->set_code(::rtidb::base::ReturnCode::kTableStatusIsNotKnormal);
                response->set_msg("table status is not kNormal");
                break;
            } else {
                table->SetTableStat(::rtidb::storage::kSnapshotPaused);
                PDLOG(INFO, "table status has set[%u]. tid[%u] pid[%u]", 
                        table->GetTableStat(), request->tid(), request->pid());
            }
        }
        if (task_ptr) {
            std::lock_guard<std::mutex> lock(mu_);
            task_ptr->set_status(::rtidb::api::TaskStatus::kDone);
        }
        response->set_code(::rtidb::base::ReturnCode::kOk);
        response->set_msg("ok");
        return;
    } while(0);
    if (task_ptr) {
        std::lock_guard<std::mutex> lock(mu_);
        task_ptr->set_status(::rtidb::api::TaskStatus::kFailed);
    }
}

void TabletImpl::RecoverSnapshot(RpcController* controller,
        const ::rtidb::api::GeneralRequest* request,
        ::rtidb::api::GeneralResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);        
    std::shared_ptr<::rtidb::api::TaskInfo> task_ptr;
    if (request->has_task_info() && request->task_info().IsInitialized()) {
        if (AddOPTask(request->task_info(), ::rtidb::api::TaskType::kRecoverSnapshot, task_ptr) < 0) {
            response->set_code(-1);
            response->set_msg("add task failed");
            return;
        }
    }
    do {
        {
            std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
            std::shared_ptr<Table> table = GetTableUnLock(request->tid(), request->pid());
            if (!table) {
                PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(), request->pid());
                response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
                response->set_msg("table is not exist");
                break;
            }
            if (table->GetTableStat() == rtidb::storage::kNormal) {
                PDLOG(INFO, "table status is already kNormal, need not recover. tid[%u] pid[%u]", 
                        request->tid(), request->pid());

            } else if (table->GetTableStat() != ::rtidb::storage::kSnapshotPaused) {
                PDLOG(WARNING, "table status is [%u], cann't recover. tid[%u] pid[%u]", 
                        table->GetTableStat(), request->tid(), request->pid());
                response->set_code(::rtidb::base::ReturnCode::kTableStatusIsNotKsnapshotpaused);
                response->set_msg("table status is not kSnapshotPaused");
                break;
            } else {
                table->SetTableStat(::rtidb::storage::kNormal);
                PDLOG(INFO, "table status has set[%u]. tid[%u] pid[%u]", 
                        table->GetTableStat(), request->tid(), request->pid());
            }
        }
        std::lock_guard<std::mutex> lock(mu_);
        if (task_ptr) {       
            task_ptr->set_status(::rtidb::api::TaskStatus::kDone);
        }
        response->set_code(::rtidb::base::ReturnCode::kOk);
        response->set_msg("ok");
        return;
    } while(0);
    if (task_ptr) {       
        std::lock_guard<std::mutex> lock(mu_);
        task_ptr->set_status(::rtidb::api::TaskStatus::kFailed);
    }
}

void TabletImpl::LoadTable(RpcController* controller,
        const ::rtidb::api::LoadTableRequest* request,
        ::rtidb::api::GeneralResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<::rtidb::api::TaskInfo> task_ptr;
    if (request->has_task_info() && request->task_info().IsInitialized()) {
        if (AddOPTask(request->task_info(), ::rtidb::api::TaskType::kLoadTable, task_ptr) < 0) {
            response->set_code(-1);
            response->set_msg("add task failed");
            return;
        }
    }
    do {
        ::rtidb::api::TableMeta table_meta;
        table_meta.CopyFrom(request->table_meta());
        std::string msg;
        if (CheckTableMeta(&table_meta, msg) != 0) {
            response->set_code(::rtidb::base::ReturnCode::kTableMetaIsIllegal);
            response->set_msg(msg);
            break;
        }
        uint32_t tid = table_meta.tid();
        uint32_t pid = table_meta.pid();
        std::string root_path;
        bool ok = ChooseDBRootPath(tid, pid, table_meta.storage_mode(), root_path);
        if (!ok) {
            response->set_code(::rtidb::base::ReturnCode::kFailToGetDbRootPath);
            response->set_msg("fail to get table db root path");
            PDLOG(WARNING, "table db path is not found. tid %u, pid %u", tid, pid);
            break;
        }

        std::string db_path = root_path + "/" + std::to_string(tid) + 
            "_" + std::to_string(pid);
        if (!::rtidb::base::IsExists(db_path)) {
            PDLOG(WARNING, "table db path is not exist. tid %u, pid %u, path %s", tid, pid, db_path.c_str());
            response->set_code(::rtidb::base::ReturnCode::kTableDbPathIsNotExist);
            response->set_msg("table db path is not exist");
            break;
        }

        std::shared_ptr<Table> table = GetTable(tid, pid);
        if (table) {
            PDLOG(WARNING, "table with tid[%u] and pid[%u] exists", tid, pid);
            response->set_code(::rtidb::base::ReturnCode::kTableAlreadyExists);
            response->set_msg("table already exists");
            break;
        }

        UpdateTableMeta(db_path, &table_meta);
        if (WriteTableMeta(db_path, &table_meta) < 0) {
            PDLOG(WARNING, "write table_meta failed. tid[%lu] pid[%lu]", tid, pid);
            response->set_code(::rtidb::base::ReturnCode::kWriteDataFailed);
            response->set_msg("write data failed");
            break;
        }
        if (table_meta.storage_mode() == rtidb::common::kMemory) {
            std::string msg;
            if (CreateTableInternal(&table_meta, msg) < 0) {
                response->set_code(::rtidb::base::ReturnCode::kCreateTableFailed);
                response->set_msg(msg.c_str());
                break;
            }
            uint64_t ttl = table_meta.ttl();
            std::string name = table_meta.name();
            uint32_t seg_cnt = 8;
            if (table_meta.seg_cnt() > 0) {
                seg_cnt = table_meta.seg_cnt();
            }
            PDLOG(INFO, "start to recover table with id %u pid %u name %s seg_cnt %d idx_cnt %u schema_size %u ttl %llu", tid, 
                    pid, name.c_str(), seg_cnt, table_meta.dimensions_size(), table_meta.schema().size(), ttl);
            task_pool_.AddTask(boost::bind(&TabletImpl::LoadTableInternal, this, tid, pid, task_ptr));
        } else {
            task_pool_.AddTask(boost::bind(&TabletImpl::LoadDiskTableInternal, this, tid, pid, table_meta, task_ptr));
            PDLOG(INFO, "load table tid[%u] pid[%u] storage mode[%s]", 
                    tid, pid, ::rtidb::common::StorageMode_Name(table_meta.storage_mode()).c_str());
        }
        response->set_code(::rtidb::base::ReturnCode::kOk);
        response->set_msg("ok");
        return;
    } while(0);
    if (task_ptr) {
        std::lock_guard<std::mutex> lock(mu_);
        task_ptr->set_status(::rtidb::api::TaskStatus::kFailed);
    }
}

int TabletImpl::LoadDiskTableInternal(uint32_t tid, uint32_t pid, 
        const ::rtidb::api::TableMeta& table_meta, std::shared_ptr<::rtidb::api::TaskInfo> task_ptr) {
    do {
        std::string db_root_path;
        bool ok = ChooseDBRootPath(tid, pid, table_meta.storage_mode(), db_root_path);
        if (!ok) {
            PDLOG(WARNING, "fail to find db root path for table tid %u pid %u", tid, pid);
            break;
        }
        std::string table_path = db_root_path + 
            "/" + std::to_string(tid) + "_" + std::to_string(pid);
        std::string snapshot_path = table_path + "/snapshot/";
        ::rtidb::api::Manifest manifest;
        uint64_t snapshot_offset = 0;
        std::string data_path = table_path + "/data";
        if (::rtidb::base::IsExists(data_path)) {
            if (!::rtidb::base::RemoveDir(data_path)) {
                PDLOG(WARNING, "remove dir failed. tid %u pid %u path %s", tid, pid, data_path.c_str());
                break;
            }
        }
        bool need_load = false;
        std::string manifest_file = snapshot_path + "MANIFEST";
        if (Snapshot::GetLocalManifest(manifest_file, manifest) == 0) {
            std::string snapshot_dir = snapshot_path + manifest.name();
            PDLOG(INFO, "rename dir %s to %s. tid %u pid %u", snapshot_dir.c_str(), data_path.c_str(), tid, pid);
            if (!::rtidb::base::Rename(snapshot_dir, data_path)) {
                PDLOG(WARNING, "rename dir failed. tid %u pid %u path %s", tid, pid, snapshot_dir.c_str());
                break; 
            }
            if (unlink(manifest_file.c_str()) < 0) {
                PDLOG(WARNING, "remove manifest failed. tid %u pid %u path %s", tid, pid, manifest_file.c_str());
                break;
            }
            snapshot_offset = manifest.offset();
            need_load = true;
        }
        std::string msg;
        if (CreateDiskTableInternal(&table_meta, need_load, msg) < 0) {
            PDLOG(WARNING, "create table failed. tid %u pid %u msg %s", tid, pid, msg.c_str());
            break;
        }
        // load snapshot data
        std::shared_ptr<Table> table = GetTable(tid, pid);
        if (!table) {
            PDLOG(WARNING, "table with tid %u and pid %u does not exist", tid, pid);
            break; 
        }
        DiskTable* disk_table = dynamic_cast<DiskTable*>(table.get());
        if (disk_table == NULL) {
            break;
        }
        std::shared_ptr<Snapshot> snapshot = GetSnapshot(tid, pid);
        if (!snapshot) {
            PDLOG(WARNING, "snapshot with tid %u and pid %u does not exist", tid, pid);
            break; 
        }
        std::shared_ptr<LogReplicator> replicator = GetReplicator(tid, pid);
        if (!replicator) {
            PDLOG(WARNING, "replicator with tid %u and pid %u does not exist", tid, pid);
            break;
        }
        {
            std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
            table->SetTableStat(::rtidb::storage::kLoading);
        }
        uint64_t latest_offset = 0;
        std::string binlog_path = table_path + "/binlog/";
        ::rtidb::storage::Binlog binlog(replicator->GetLogPart(), binlog_path);
        if (binlog.RecoverFromBinlog(table, snapshot_offset, latest_offset)) {
            table->SetTableStat(::rtidb::storage::kNormal);
            replicator->SetOffset(latest_offset);
            replicator->SetSnapshotLogPartIndex(snapshot->GetOffset());
            replicator->StartSyncing();
            disk_table->SetOffset(latest_offset);
            table->SchedGc();
            gc_pool_.DelayTask(FLAGS_gc_interval * 60 * 1000, boost::bind(&TabletImpl::GcTable, this, tid, pid, false));
            io_pool_.DelayTask(FLAGS_binlog_sync_to_disk_interval, boost::bind(&TabletImpl::SchedSyncDisk, this, tid, pid));
            task_pool_.DelayTask(FLAGS_binlog_delete_interval, boost::bind(&TabletImpl::SchedDelBinlog, this, tid, pid));
            PDLOG(INFO, "load table success. tid %u pid %u", tid, pid);
            MakeSnapshotInternal(tid, pid, 0, std::shared_ptr<::rtidb::api::TaskInfo>());
            if (task_ptr) {
                std::lock_guard<std::mutex> lock(mu_);
                task_ptr->set_status(::rtidb::api::TaskStatus::kDone);
                return 0;
            }
        } else {
            DeleteTableInternal(tid, pid, std::shared_ptr<::rtidb::api::TaskInfo>());
        }
    } while (0);
    if (task_ptr) {
        std::lock_guard<std::mutex> lock(mu_);
        task_ptr->set_status(::rtidb::api::TaskStatus::kFailed);
    }
    return -1;
}

int TabletImpl::LoadTableInternal(uint32_t tid, uint32_t pid, std::shared_ptr<::rtidb::api::TaskInfo> task_ptr) {
    do {
        // load snapshot data
        std::shared_ptr<Table> table = GetTable(tid, pid);        
        if (!table) {
            PDLOG(WARNING, "table with tid %u and pid %u does not exist", tid, pid);
            break; 
        }
        std::shared_ptr<Snapshot> snapshot = GetSnapshot(tid, pid);
        if (!snapshot) {
            PDLOG(WARNING, "snapshot with tid %u and pid %u does not exist", tid, pid);
            break; 
        }
        std::shared_ptr<LogReplicator> replicator = GetReplicator(tid, pid);
        if (!replicator) {
            PDLOG(WARNING, "replicator with tid %u and pid %u does not exist", tid, pid);
            break;
        }
        {
            std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
            table->SetTableStat(::rtidb::storage::kLoading);
        }
        uint64_t latest_offset = 0;
        uint64_t snapshot_offset = 0;
        std::string db_root_path;
        bool ok = ChooseDBRootPath(tid, pid, table->GetStorageMode(), db_root_path);
        if (!ok) {
            PDLOG(WARNING, "fail to find db root path for table tid %u pid %u", tid, pid);
            break;
        }
        std::string binlog_path = db_root_path + 
            "/" + std::to_string(tid) + "_" + std::to_string(pid) + "/binlog/";
        ::rtidb::storage::Binlog binlog(replicator->GetLogPart(), binlog_path);
        if (snapshot->Recover(table, snapshot_offset) && binlog.RecoverFromBinlog(table, snapshot_offset, latest_offset)) {
            table->SetTableStat(::rtidb::storage::kNormal);
            replicator->SetOffset(latest_offset);
            replicator->SetSnapshotLogPartIndex(snapshot->GetOffset());
            replicator->StartSyncing();
            table->SchedGc();
            gc_pool_.DelayTask(FLAGS_gc_interval * 60 * 1000, boost::bind(&TabletImpl::GcTable, this, tid, pid, false));
            io_pool_.DelayTask(FLAGS_binlog_sync_to_disk_interval, boost::bind(&TabletImpl::SchedSyncDisk, this, tid, pid));
            task_pool_.DelayTask(FLAGS_binlog_delete_interval, boost::bind(&TabletImpl::SchedDelBinlog, this, tid, pid));
            PDLOG(INFO, "load table success. tid %u pid %u", tid, pid);
            if (task_ptr) {
                std::lock_guard<std::mutex> lock(mu_);
                task_ptr->set_status(::rtidb::api::TaskStatus::kDone);
                return 0;
            }
        } else {
            DeleteTableInternal(tid, pid, std::shared_ptr<::rtidb::api::TaskInfo>());
        }
    } while (0);    
    if (task_ptr) {
        std::lock_guard<std::mutex> lock(mu_);
        task_ptr->set_status(::rtidb::api::TaskStatus::kFailed);
    }
    return -1;
}

int32_t TabletImpl::DeleteTableInternal(uint32_t tid, uint32_t pid, std::shared_ptr<::rtidb::api::TaskInfo> task_ptr) {
    std::string root_path;
    std::string recycle_bin_root_path;
    int32_t code = -1;
    do {
        std::shared_ptr<Table> table = GetTable(tid, pid);
        if (!table) {
            PDLOG(WARNING, "table is not exist. tid %u pid %u", tid, pid);
            break;
        }
        bool ok = ChooseDBRootPath(tid, pid, table->GetStorageMode(), root_path);
        if (!ok) {
            PDLOG(WARNING, "fail to get db root path. tid %u pid %u", tid, pid);
            break;
        }
        ok = ChooseRecycleBinRootPath(tid, pid, table->GetStorageMode(), recycle_bin_root_path);
        if (!ok) {
            PDLOG(WARNING, "fail to get recycle bin root path. tid %u pid %u", tid, pid);
            break;
        }
        std::shared_ptr<LogReplicator> replicator = GetReplicator(tid, pid);
        {
            std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
            tables_[tid].erase(pid);
            replicators_[tid].erase(pid);
            snapshots_[tid].erase(pid);
            if (tables_[tid].empty()) {
                tables_.erase(tid);
            }
            if (replicators_[tid].empty()) {
                replicators_.erase(tid);
            }
            if (snapshots_[tid].empty()) {
                snapshots_.erase(tid);
            }
        }

        if (replicator) {
            replicator->DelAllReplicateNode();
            PDLOG(INFO, "drop replicator for tid %u, pid %u", tid, pid);
        }
        code = 0;
    } while (0);
    if (code < 0) {
        if (task_ptr) {
            std::lock_guard<std::mutex> lock(mu_);
            task_ptr->set_status(::rtidb::api::TaskStatus::kFailed);
        }
        return code;
    }

    std::string source_path = root_path + "/" + std::to_string(tid) + "_" + std::to_string(pid);
    if (!::rtidb::base::IsExists(source_path)) {
        if (task_ptr) {
            std::lock_guard<std::mutex> lock(mu_);
            task_ptr->set_status(::rtidb::api::TaskStatus::kDone);
        } 
        PDLOG(INFO, "drop table ok. tid[%u] pid[%u]", tid, pid);
        return 0;
    }

    if(FLAGS_recycle_bin_enabled) {
        std::string recycle_path = recycle_bin_root_path + "/" + std::to_string(tid) + 
            "_" + std::to_string(pid) + "_" + ::rtidb::base::GetNowTime();
        ::rtidb::base::Rename(source_path, recycle_path);
    } else {
        ::rtidb::base::RemoveDirRecursive(source_path);
    }

    if (task_ptr) {
        std::lock_guard<std::mutex> lock(mu_);
        task_ptr->set_status(::rtidb::api::TaskStatus::kDone);
    }
    PDLOG(INFO, "drop table ok. tid[%u] pid[%u]", tid, pid);
    return 0;
}

int32_t TabletImpl::DeleteRelationalTableInternal(uint32_t tid, uint32_t pid, std::shared_ptr<::rtidb::api::TaskInfo> task_ptr) {
    std::string root_path;
    std::string recycle_bin_root_path;
    int32_t code = -1;
    do {
        std::shared_ptr<RelationalTable> table;
        {
            std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
            table = GetRelationalTableUnLock(tid, pid);
        }
        if (!table) {
            PDLOG(WARNING, "table is not exist. tid %u pid %u", tid, pid);
            break;
        }
        bool ok = ChooseDBRootPath(tid, pid, table->GetStorageMode(), root_path);
        if (!ok) {
            PDLOG(WARNING, "fail to get db root path. tid %u pid %u", tid, pid);
            break;
        }
        ok = ChooseRecycleBinRootPath(tid, pid, table->GetStorageMode(), recycle_bin_root_path);
        if (!ok) {
            PDLOG(WARNING, "fail to get recycle bin root path. tid %u pid %u", tid, pid);
            break;
        }
        {
            std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
            tables_[tid].erase(pid);
            if (tables_[tid].empty()) {
                tables_.erase(tid);
            }
        }
        code = 0;
    } while (0);
    if (code < 0) {
        if (task_ptr) {
            std::lock_guard<std::mutex> lock(mu_);
            task_ptr->set_status(::rtidb::api::TaskStatus::kFailed);
        }
        return code;
    }

    std::string source_path = root_path + "/" + std::to_string(tid) + "_" + std::to_string(pid);
    if (!::rtidb::base::IsExists(source_path)) {
        if (task_ptr) {
            std::lock_guard<std::mutex> lock(mu_);
            task_ptr->set_status(::rtidb::api::TaskStatus::kDone);
        } 
        PDLOG(INFO, "drop table ok. tid[%u] pid[%u]", tid, pid);
        return 0;
    }

    if(FLAGS_recycle_bin_enabled) {
        std::string recycle_path = recycle_bin_root_path + "/" + std::to_string(tid) + 
            "_" + std::to_string(pid) + "_" + ::rtidb::base::GetNowTime();
        ::rtidb::base::Rename(source_path, recycle_path);
    } else {
        ::rtidb::base::RemoveDirRecursive(source_path);
    }

    if (task_ptr) {
        std::lock_guard<std::mutex> lock(mu_);
        task_ptr->set_status(::rtidb::api::TaskStatus::kDone);
    }
    PDLOG(INFO, "drop table ok. tid[%u] pid[%u]", tid, pid);
    return 0;
}

void TabletImpl::CreateTable(RpcController* controller,
        const ::rtidb::api::CreateTableRequest* request,
        ::rtidb::api::CreateTableResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    const ::rtidb::api::TableMeta* table_meta = &request->table_meta();
    std::string msg;
    uint32_t tid = table_meta->tid();
    uint32_t pid = table_meta->pid();
    if (!table_meta->has_table_type() || table_meta->table_type() == ::rtidb::type::kTimeSeries) {
        if (CheckTableMeta(table_meta, msg) != 0) {
            response->set_code(::rtidb::base::ReturnCode::kTableMetaIsIllegal);
            response->set_msg(msg);
            PDLOG(WARNING, "check table_meta failed. tid[%u] pid[%u], err_msg[%s]", tid, pid, msg.c_str());
            return;
        }
        std::shared_ptr<Table> table = GetTable(tid, pid);
        std::shared_ptr<Snapshot> snapshot = GetSnapshot(tid, pid);
        if (table || snapshot) {
            if (table) {
                PDLOG(WARNING, "table with tid[%u] and pid[%u] exists", tid, pid);
            }
            if (snapshot) {
                PDLOG(WARNING, "snapshot with tid[%u] and pid[%u] exists", tid, pid);
            }
            response->set_code(::rtidb::base::ReturnCode::kTableAlreadyExists);
            response->set_msg("table already exists");
            return;
        }
    }
    std::string name = table_meta->name();
    PDLOG(INFO, "start creating table tid[%u] pid[%u] with mode %s", 
            tid, pid, ::rtidb::api::TableMode_Name(request->table_meta().mode()).c_str());
    std::string db_root_path;
    bool ok = ChooseDBRootPath(tid, pid, table_meta->storage_mode(), db_root_path);
    if (!ok) {
        PDLOG(WARNING, "fail to find db root path tid[%u] pid[%u]", tid, pid);
        response->set_code(::rtidb::base::ReturnCode::kFailToGetDbRootPath);
        response->set_msg("fail to find db root path");
        return;
    }
    std::string table_db_path = db_root_path + "/" + std::to_string(tid) +
        "_" + std::to_string(pid);

    if (WriteTableMeta(table_db_path, table_meta) < 0) {
        PDLOG(WARNING, "write table_meta failed. tid[%u] pid[%u]", tid, pid);
        response->set_code(::rtidb::base::ReturnCode::kWriteDataFailed);
        response->set_msg("write data failed");
        return;
    }
    if (table_meta->has_table_type() && table_meta->table_type() == rtidb::type::kRelational) {
        std::string msg;
        if (CreateRelationalTableInternal(table_meta, msg) < 0) {
            response->set_code(::rtidb::base::ReturnCode::kCreateTableFailed);
            response->set_msg(msg.c_str());
            return;
        }
    } else if (table_meta->storage_mode() != rtidb::common::kMemory) {
        std::string msg;
        if (CreateDiskTableInternal(table_meta, false, msg) < 0) {
            response->set_code(::rtidb::base::ReturnCode::kCreateTableFailed);
            response->set_msg(msg.c_str());
            return;
        }
    } else {
        std::string msg;
        if (CreateTableInternal(table_meta, msg) < 0) {
            response->set_code(::rtidb::base::ReturnCode::kCreateTableFailed);
            response->set_msg(msg.c_str());
            return;
        }
    }
    if (!table_meta->has_table_type() || table_meta->table_type() == ::rtidb::type::kTimeSeries) {
        std::shared_ptr<Table> table = GetTable(tid, pid);
        if (!table) {
            response->set_code(::rtidb::base::ReturnCode::kCreateTableFailed);
            response->set_msg("table is not exist");
            PDLOG(WARNING, "table with tid %u and pid %u does not exist", tid, pid);
            return; 
        }
        std::shared_ptr<LogReplicator> replicator = GetReplicator(tid, pid);
        if (!replicator) {
            response->set_code(::rtidb::base::ReturnCode::kCreateTableFailed);
            response->set_msg("replicator is not exist");
            PDLOG(WARNING, "replicator with tid %u and pid %u does not exist", tid, pid);
            return;
        }
        response->set_code(::rtidb::base::ReturnCode::kOk);
        response->set_msg("ok");
        table->SetTableStat(::rtidb::storage::kNormal);
        replicator->StartSyncing();
        io_pool_.DelayTask(FLAGS_binlog_sync_to_disk_interval, boost::bind(&TabletImpl::SchedSyncDisk, this, tid, pid));
        task_pool_.DelayTask(FLAGS_binlog_delete_interval, boost::bind(&TabletImpl::SchedDelBinlog, this, tid, pid));
        PDLOG(INFO, "create table with id %u pid %u name %s abs_ttl %llu lat_ttl %llu type %s", 
                tid, pid, name.c_str(), table_meta->ttl_desc().abs_ttl(), table_meta->ttl_desc().lat_ttl(),
                ::rtidb::api::TTLType_Name(table_meta->ttl_desc().ttl_type()).c_str());
        gc_pool_.DelayTask(FLAGS_gc_interval * 60 * 1000, boost::bind(&TabletImpl::GcTable, this, tid, pid, false));
    } else {
        std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
        std::shared_ptr<RelationalTable> table = GetRelationalTableUnLock(tid, pid);
        if (!table) {
            response->set_code(::rtidb::base::ReturnCode::kCreateTableFailed);
            response->set_msg("table is not exist");
            PDLOG(WARNING, "table with tid %u and pid %u does not exist", tid, pid);
            return; 
        }
        table->SetTableStat(::rtidb::storage::kNormal);
    }
}

void TabletImpl::ExecuteGc(RpcController* controller,
        const ::rtidb::api::ExecuteGcRequest* request,
        ::rtidb::api::GeneralResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    uint32_t tid = request->tid();
    uint32_t pid = request->pid();
    std::shared_ptr<Table> table = GetTable(tid, pid);
    if (!table) {
        PDLOG(DEBUG, "table is not exist. tid %u pid %u", tid, pid);
        response->set_code(-1);
        response->set_msg("table not found");
        return;
    }
    gc_pool_.AddTask(boost::bind(&TabletImpl::GcTable, this, tid, pid, true));
    response->set_code(::rtidb::base::ReturnCode::kOk);
    response->set_msg("ok");
    PDLOG(INFO, "ExecuteGc. tid %u pid %u", tid, pid);
}

void TabletImpl::GetTableFollower(RpcController* controller,
        const ::rtidb::api::GetTableFollowerRequest* request,
        ::rtidb::api::GetTableFollowerResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    uint32_t tid = request->tid();
    uint32_t pid = request->pid();
    std::shared_ptr<Table> table = GetTable(tid, pid);
    if (!table) {
        PDLOG(DEBUG, "table is not exist. tid %u pid %u", tid, pid);
        response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
        response->set_msg("table is not exist");
        return;
    }
    if (!table->IsLeader()) {
        PDLOG(DEBUG, "table is follower. tid %u, pid %u", tid, pid);
        response->set_msg("table is follower");
        response->set_code(::rtidb::base::ReturnCode::kTableIsFollower);
        return;
    }
    std::shared_ptr<LogReplicator> replicator = GetReplicator(tid, pid);
    if (!replicator) {
        PDLOG(DEBUG, "replicator is not exist. tid %u pid %u", tid, pid);
        response->set_msg("replicator is not exist");
        response->set_code(::rtidb::base::ReturnCode::kReplicatorIsNotExist);
        return;
    }
    response->set_offset(replicator->GetOffset());
    std::map<std::string, uint64_t> info_map;
    replicator->GetReplicateInfo(info_map);
    if (info_map.empty()) {
        response->set_msg("has no follower");
        response->set_code(::rtidb::base::ReturnCode::kNoFollower);
    }
    for (const auto& kv : info_map) {
        ::rtidb::api::FollowerInfo* follower_info = response->add_follower_info();
        follower_info->set_endpoint(kv.first);
        follower_info->set_offset(kv.second);
    }
    response->set_msg("ok");
    response->set_code(::rtidb::base::ReturnCode::kOk);
}

int32_t TabletImpl::GetSnapshotOffset(uint32_t tid, uint32_t pid, rtidb::common::StorageMode sm, std::string& msg, uint64_t& term, uint64_t& offset) {
    std::string db_root_path;
    bool ok = ChooseDBRootPath(tid, pid, sm, db_root_path);
    if (!ok) {
        msg = "fail to get db root path";
        PDLOG(WARNING, "fail to get table db root path");
        return 138;
    }
    std::string db_path = db_root_path + "/" + std::to_string(tid) + "_" + std::to_string(pid);
    std::string manifest_file =  db_path + "/snapshot/MANIFEST";
    int fd = open(manifest_file.c_str(), O_RDONLY);
    if (fd < 0) {
        PDLOG(WARNING, "[%s] is not exist", manifest_file.c_str());
        return 0;
    }
    google::protobuf::io::FileInputStream fileInput(fd);
    fileInput.SetCloseOnDelete(true);
    ::rtidb::api::Manifest manifest;
    if (!google::protobuf::TextFormat::Parse(&fileInput, &manifest)) {
        PDLOG(WARNING, "parse manifest failed");
        return 0;
    }
    std::string snapshot_file = db_path + "/snapshot/" + manifest.name();
    if (!::rtidb::base::IsExists(snapshot_file)) {
        PDLOG(WARNING, "snapshot file[%s] is not exist", snapshot_file.c_str());
        return 0;
    }
    offset = manifest.offset();
    term = manifest.term();
    return 0;

}
void TabletImpl::GetAllSnapshotOffset(RpcController* controller,
        const ::rtidb::api::EmptyRequest* request,
        ::rtidb::api::TableSnapshotOffsetResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::map<uint32_t, rtidb::common::StorageMode>  table_sm;
    std::map<uint32_t, std::vector<uint32_t>> tid_pid;
    {
        std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
        for (auto table_iter = tables_.begin(); table_iter != tables_.end(); table_iter++) {
            if (table_iter->second.empty()) {
                continue;
            }
            uint32_t tid = table_iter->first;
            std::vector<uint32_t> pids;
            auto part_iter = table_iter->second.begin();
            rtidb::common::StorageMode sm = part_iter ->second->GetStorageMode();
            for (;part_iter != table_iter->second.end(); part_iter++) {
                pids.push_back(part_iter->first);
            }
            table_sm.insert(std::make_pair(tid, sm));
            tid_pid.insert(std::make_pair(tid, pids));
        }
    }
    std::string msg;
    for (auto iter = tid_pid.begin(); iter != tid_pid.end(); iter++) {
        uint32_t tid = iter->first;
        auto table = response->add_tables();
        table->set_tid(tid);
        for (auto pid : iter->second) {
            uint64_t term = 0 , offset = 0;
            rtidb::common::StorageMode sm = table_sm.find(tid)->second;
            int32_t code = GetSnapshotOffset(tid, pid, sm, msg, term, offset);
            if (code != 0) {
                continue;
            }
            auto partition = table->add_parts();
            partition->set_offset(offset);
            partition->set_pid(pid);
        }
    }
    response->set_code(::rtidb::base::ReturnCode::kOk);
}

void TabletImpl::GetTermPair(RpcController* controller,
        const ::rtidb::api::GetTermPairRequest* request,
        ::rtidb::api::GetTermPairResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    if (FLAGS_zk_cluster.empty()) {
        response->set_code(-1);
        response->set_msg("tablet is not run in cluster mode");
        PDLOG(WARNING, "tablet is not run in cluster mode");
        return;
    }
    uint32_t tid = request->tid();
    uint32_t pid = request->pid();
    std::shared_ptr<Table> table = GetTable(tid, pid);
    ::rtidb::common::StorageMode mode = ::rtidb::common::kMemory;
    if (request->has_storage_mode()) {
        mode = request->storage_mode();
    }
    if (!table) {
        response->set_code(::rtidb::base::ReturnCode::kOk);
        response->set_has_table(false);
        response->set_msg("table is not exist");
        std::string msg;
        uint64_t term = 0, offset = 0;
        int32_t code = GetSnapshotOffset(tid, pid, mode, msg, term, offset);
        response->set_code(code);
        if (code == 0) {
            response->set_term(term);
            response->set_offset(offset);
        } else {
            response->set_msg(msg);
        }
        return;
    }
    std::shared_ptr<LogReplicator> replicator = GetReplicator(tid, pid);
    if (!replicator) {
        response->set_code(::rtidb::base::ReturnCode::kReplicatorIsNotExist);
        response->set_msg("replicator is not exist");
        return;
    }
    response->set_code(::rtidb::base::ReturnCode::kOk);
    response->set_msg("ok");
    response->set_has_table(true);
    if (table->IsLeader()) {
        response->set_is_leader(true);
    } else {
        response->set_is_leader(false);
    }
    response->set_term(replicator->GetLeaderTerm());
    response->set_offset(replicator->GetOffset());
}

void TabletImpl::DeleteBinlog(RpcController* controller,
        const ::rtidb::api::GeneralRequest* request,
        ::rtidb::api::GeneralResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    uint32_t tid = request->tid();
    uint32_t pid = request->pid();
    ::rtidb::common::StorageMode mode = ::rtidb::common::kMemory;
    if (request->has_storage_mode()) {
        mode = request->storage_mode();
    }
    std::string db_root_path;
    bool ok = ChooseDBRootPath(tid, pid, mode, db_root_path);
    if (!ok) {
        response->set_code(::rtidb::base::ReturnCode::kFailToGetDbRootPath);
        response->set_msg("fail to get db root path");
        PDLOG(WARNING, "fail to get table db root path");
        return;
    }
    std::string db_path = db_root_path + "/" + std::to_string(tid) + "_" + std::to_string(pid);
    std::string binlog_path = db_path + "/binlog";
    if (::rtidb::base::IsExists(binlog_path)) {
        //TODO add clean the recycle bin logic
        if(FLAGS_recycle_bin_enabled) {
            std::string recycle_bin_root_path;
            ok = ChooseRecycleBinRootPath(tid, pid, mode, recycle_bin_root_path);
            if (!ok) {
                response->set_code(::rtidb::base::ReturnCode::kFailToGetRecycleRootPath);
                response->set_msg("fail to get recycle root path");
                PDLOG(WARNING, "fail to get table recycle root path");
                return;
            }
            std::string recycle_path = recycle_bin_root_path + "/" + std::to_string(tid) + 
                "_" + std::to_string(pid) + "_binlog_" + ::rtidb::base::GetNowTime();
            ::rtidb::base::Rename(binlog_path, recycle_path);
            PDLOG(INFO, "binlog has moved form %s to %s. tid %u pid %u", 
                    binlog_path.c_str(), recycle_path.c_str(), tid, pid);
        } else {
            ::rtidb::base::RemoveDirRecursive(binlog_path);
            PDLOG(INFO, "binlog %s has removed. tid %u pid %u", 
                    binlog_path.c_str(), tid, pid);
        }
    }
    response->set_code(::rtidb::base::ReturnCode::kOk);
    response->set_msg("ok");
}

void TabletImpl::CheckFile(RpcController* controller,
        const ::rtidb::api::CheckFileRequest* request,
        ::rtidb::api::GeneralResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    uint32_t tid = request->tid();
    uint32_t pid = request->pid();
    std::string db_root_path;
    ::rtidb::common::StorageMode mode = ::rtidb::common::kMemory;
    if (request->has_storage_mode()) {
        mode = request->storage_mode();
    }
    bool ok = ChooseDBRootPath(tid, pid, mode, db_root_path);
    if (!ok) {
        response->set_code(::rtidb::base::ReturnCode::kFailToGetDbRootPath);
        response->set_msg("fail to get db root path");
        PDLOG(WARNING, "fail to get table db root path");
        return;
    }
    std::string file_name = request->file();
    std::string full_path = db_root_path + "/" + std::to_string(tid) + "_" + std::to_string(pid) + "/";
    if (file_name != "table_meta.txt") {
        full_path += "snapshot/";
    }
    if (request->has_dir_name() && request->dir_name().size() > 0) {
        full_path.append(request->dir_name() + "/");
    }
    full_path += file_name;
    uint64_t size = 0;
    if (::rtidb::base::GetSize(full_path, size) < 0) {
        response->set_code(-1);
        response->set_msg("get size failed");
        PDLOG(WARNING, "get size failed. file[%s]", full_path.c_str());
        return;
    }
    if (size != request->size()) {
        response->set_code(-1);
        response->set_msg("check size failed");
        PDLOG(WARNING, "check size failed. file[%s] cur_size[%lu] expect_size[%lu]", 
                full_path.c_str(), size, request->size());
        return;
    }
    response->set_code(::rtidb::base::ReturnCode::kOk);
    response->set_msg("ok");
}

void TabletImpl::GetManifest(RpcController* controller,
        const ::rtidb::api::GetManifestRequest* request,
        ::rtidb::api::GetManifestResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::string db_root_path;
    ::rtidb::common::StorageMode mode = ::rtidb::common::kMemory;
    if (request->has_storage_mode()) {
        mode = request->storage_mode();
    }
    bool ok = ChooseDBRootPath(request->tid(), request->pid(), 
            mode, db_root_path);
    if (!ok) {
        response->set_code(::rtidb::base::ReturnCode::kFailToGetDbRootPath);
        response->set_msg("fail to get db root path");
        PDLOG(WARNING, "fail to get table db root path");
        return;
    }
    std::string db_path = db_root_path + "/" + std::to_string(request->tid()) + "_" + 
        std::to_string(request->pid());
    std::string manifest_file =  db_path + "/snapshot/MANIFEST";
    ::rtidb::api::Manifest manifest;
    int fd = open(manifest_file.c_str(), O_RDONLY);
    if (fd >= 0) {
        google::protobuf::io::FileInputStream fileInput(fd);
        fileInput.SetCloseOnDelete(true);
        if (!google::protobuf::TextFormat::Parse(&fileInput, &manifest)) {
            PDLOG(WARNING, "parse manifest failed");
            response->set_code(-1);
            response->set_msg("parse manifest failed");
            return;
        }
    } else {
        PDLOG(INFO, "[%s] is not exist", manifest_file.c_str());
        manifest.set_offset(0);
    }
    response->set_code(::rtidb::base::ReturnCode::kOk);
    response->set_msg("ok");
    ::rtidb::api::Manifest* manifest_r = response->mutable_manifest();
    manifest_r->CopyFrom(manifest);
}

int TabletImpl::WriteTableMeta(const std::string& path, const ::rtidb::api::TableMeta* table_meta) {
    if (!::rtidb::base::MkdirRecur(path)) {
        PDLOG(WARNING, "fail to create path %s", path.c_str());
        return -1;
    }
    std::string full_path = path + "/table_meta.txt";
    std::string table_meta_info;
    google::protobuf::TextFormat::PrintToString(*table_meta, &table_meta_info);
    FILE* fd_write = fopen(full_path.c_str(), "w");
    if (fd_write == NULL) {
        PDLOG(WARNING, "fail to open file %s. err[%d: %s]", full_path.c_str(), errno, strerror(errno));
        return -1;
    }
    if (fputs(table_meta_info.c_str(), fd_write) == EOF) {
        PDLOG(WARNING, "write error. path[%s], err[%d: %s]", full_path.c_str(), errno, strerror(errno));
        fclose(fd_write);
        return -1;
    }
    fclose(fd_write);
    return 0;
}

int TabletImpl::UpdateTableMeta(const std::string& path, ::rtidb::api::TableMeta* table_meta, bool for_add_column) {
    std::string full_path = path + "/table_meta.txt";
    int fd = open(full_path.c_str(), O_RDONLY);
    ::rtidb::api::TableMeta old_meta;
    if (fd < 0) {
        PDLOG(WARNING, "[%s] is not exist", "table_meta.txt");
        return 1;
    } else {
        google::protobuf::io::FileInputStream fileInput(fd);
        fileInput.SetCloseOnDelete(true);
        if (!google::protobuf::TextFormat::Parse(&fileInput, &old_meta)) {
            PDLOG(WARNING, "parse table_meta failed");
            return -1;
        }
    }
    // use replicas in LoadRequest
    if (!for_add_column) {
        old_meta.clear_replicas();
        old_meta.MergeFrom(*table_meta);
        table_meta->CopyFrom(old_meta);
    }
    std::string new_name = full_path + "." + ::rtidb::base::GetNowTime();
    rename(full_path.c_str(), new_name.c_str());
    return 0;
}

int TabletImpl::UpdateTableMeta(const std::string& path, ::rtidb::api::TableMeta* table_meta) {
    return UpdateTableMeta(path, table_meta, false);
}


int TabletImpl::CreateTableInternal(const ::rtidb::api::TableMeta* table_meta, std::string& msg) {
    std::vector<std::string> endpoints;
    for (int32_t i = 0; i < table_meta->replicas_size(); i++) {
        endpoints.push_back(table_meta->replicas(i));
    }
    uint32_t tid = table_meta->tid();
    uint32_t pid = table_meta->pid();
    std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
    std::shared_ptr<Table> table = GetTableUnLock(tid, pid);
    if (table) {
        PDLOG(WARNING, "table with tid[%u] and pid[%u] exists", tid, pid);
        return -1;
    }
    Table* table_ptr = new MemTable(*table_meta);
    table.reset(table_ptr);
    if (!table->Init()) {
        PDLOG(WARNING, "fail to init table. tid %u, pid %u", table_meta->tid(), table_meta->pid());
        msg.assign("fail to init table");
        return -1;
    }
    std::string db_root_path;
    bool ok = ChooseDBRootPath(tid, pid, table->GetStorageMode(), db_root_path);
    if (!ok) {
        PDLOG(WARNING, "fail to get table db root path");
        msg.assign("fail to get table db root path");
        return -1;
    }
    std::string table_db_path = db_root_path + "/" + std::to_string(table_meta->tid()) +
        "_" + std::to_string(table_meta->pid());
    std::shared_ptr<LogReplicator> replicator;
    if (table->IsLeader()) {
        replicator = std::make_shared<LogReplicator>(table_db_path, 
                endpoints,
                ReplicatorRole::kLeaderNode, 
                table, &follower_);
    } else {
        replicator = std::make_shared<LogReplicator>(table_db_path, 
                std::vector<std::string>(), 
                ReplicatorRole::kFollowerNode,
                table, &follower_);
    }
    if (!replicator) {
        PDLOG(WARNING, "fail to create replicator for table tid %u, pid %u", table_meta->tid(), table_meta->pid());
        msg.assign("fail create replicator for table");
        return -1;
    }
    ok = replicator->Init();
    if (!ok) {
        PDLOG(WARNING, "fail to init replicator for table tid %u, pid %u", table_meta->tid(), table_meta->pid());
        // clean memory
        msg.assign("fail init replicator for table");
        return -1;
    }
    if (!FLAGS_zk_cluster.empty() && table_meta->mode() == ::rtidb::api::TableMode::kTableLeader) {
        replicator->SetLeaderTerm(table_meta->term());
    }
    ::rtidb::storage::Snapshot* snapshot_ptr = 
        new ::rtidb::storage::MemTableSnapshot(table_meta->tid(), table_meta->pid(), replicator->GetLogPart(),
                db_root_path);

    if (!snapshot_ptr->Init()){
        PDLOG(WARNING, "fail to init snapshot for tid %u, pid %u", table_meta->tid(), table_meta->pid());
        msg.assign("fail to init snapshot");
        return -1;
    }
    std::shared_ptr<Snapshot> snapshot(snapshot_ptr);
    tables_[table_meta->tid()].insert(std::make_pair(table_meta->pid(), table));
    snapshots_[table_meta->tid()].insert(std::make_pair(table_meta->pid(), snapshot));
    replicators_[table_meta->tid()].insert(std::make_pair(table_meta->pid(), replicator));
    return 0;
}

int TabletImpl::CreateDiskTableInternal(const ::rtidb::api::TableMeta* table_meta, bool is_load, std::string& msg) {
    std::vector<std::string> endpoints;
    ::rtidb::api::TTLType ttl_type = table_meta->ttl_type();
    if (table_meta->has_ttl_desc()) {
        ttl_type = table_meta->ttl_desc().ttl_type();
    }
    if (ttl_type == ::rtidb::api::kAbsAndLat || ttl_type == ::rtidb::api::kAbsOrLat) {
        PDLOG(WARNING, "disktable doesn't support abs&&lat, abs||lat in this version");
        msg.assign("disktable doesn't support abs&&lat, abs||lat in this version");
        return -1;
    }
    for (int32_t i = 0; i < table_meta->replicas_size(); i++) {
        endpoints.push_back(table_meta->replicas(i));
    }
    uint32_t tid = table_meta->tid();
    uint32_t pid = table_meta->pid();
    std::string db_root_path;
    bool ok = ChooseDBRootPath(table_meta->tid(), table_meta->pid(), table_meta->storage_mode(), db_root_path);
    if (!ok) {
        PDLOG(WARNING, "fail to get table db root path");
        msg.assign("fail to get table db root path");
        return -1;
    }
    DiskTable* table_ptr = new DiskTable(*table_meta, db_root_path);
    if (is_load) {
        if (!table_ptr->LoadTable()) {
            return -1;
        }
        PDLOG(INFO, "load disk table. tid %u pid %u", tid, pid);
    } else {
        if (!table_ptr->Init()) {
            return -1;
        }
        PDLOG(INFO, "create disk table. tid %u pid %u", tid, pid);
    }
    std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
    std::shared_ptr<Table> table = GetTableUnLock(tid, pid);
    if (table) {
        PDLOG(WARNING, "table with tid[%u] and pid[%u] exists", tid, pid);
        return -1;
    }
    table.reset((Table*)table_ptr);
    tables_[table_meta->tid()].insert(std::make_pair(table_meta->pid(), table));
    ::rtidb::storage::Snapshot* snapshot_ptr = 
        new ::rtidb::storage::DiskTableSnapshot(table_meta->tid(), table_meta->pid(), table_meta->storage_mode(),
                db_root_path);
    if (!snapshot_ptr->Init()) {
        PDLOG(WARNING, "fail to init snapshot for tid %u, pid %u", table_meta->tid(), table_meta->pid());
        msg.assign("fail to init snapshot");
        return -1;
    }
    std::string table_db_path =db_root_path + "/" + std::to_string(table_meta->tid()) + "_" + std::to_string(table_meta->pid());
    std::shared_ptr<LogReplicator> replicator;
    if (table->IsLeader()) {
        replicator = std::make_shared<LogReplicator>(table_db_path, 
                endpoints,
                ReplicatorRole::kLeaderNode, 
                table, &follower_);
    } else {
        replicator = std::make_shared<LogReplicator>(table_db_path, 
                std::vector<std::string>(), 
                ReplicatorRole::kFollowerNode,
                table, &follower_);
    }
    if (!replicator) {
        PDLOG(WARNING, "fail to create replicator for table tid %u, pid %u", table_meta->tid(), table_meta->pid());
        msg.assign("fail create replicator for table");
        return -1;
    }
    ok = replicator->Init();
    if (!ok) {
        PDLOG(WARNING, "fail to init replicator for table tid %u, pid %u", table_meta->tid(), table_meta->pid());
        // clean memory
        msg.assign("fail init replicator for table");
        return -1;
    }
    if (!FLAGS_zk_cluster.empty() && table_meta->mode() == ::rtidb::api::TableMode::kTableLeader) {
        replicator->SetLeaderTerm(table_meta->term());
    }
    std::shared_ptr<Snapshot> snapshot(snapshot_ptr);
    tables_[table_meta->tid()].insert(std::make_pair(table_meta->pid(), table));
    snapshots_[table_meta->tid()].insert(std::make_pair(table_meta->pid(), snapshot));
    replicators_[table_meta->tid()].insert(std::make_pair(table_meta->pid(), replicator));
    return 0;
}

int TabletImpl::CreateRelationalTableInternal(const ::rtidb::api::TableMeta* table_meta, std::string& msg) {
    uint32_t tid = table_meta->tid();
    uint32_t pid = table_meta->pid();
    std::string db_root_path;
    bool ok = ChooseDBRootPath(table_meta->tid(), table_meta->pid(), table_meta->storage_mode(), db_root_path);
    if (!ok) {
        PDLOG(WARNING, "fail to get table db root path");
        msg.assign("fail to get table db root path");
        return -1;
    }
    RelationalTable* table_ptr = new RelationalTable(*table_meta, db_root_path);
    if (!table_ptr->Init()) {
        return -1;
    }
    PDLOG(INFO, "create relation table. tid %u pid %u", tid, pid);
    std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
    std::shared_ptr<RelationalTable> table = GetRelationalTableUnLock(tid, pid);
    if (table) {
        PDLOG(WARNING, "table with tid[%u] and pid[%u] exists", tid, pid);
        return -1;
    }
    table.reset(table_ptr);
    relational_tables_[table_meta->tid()].insert(std::make_pair(table_meta->pid(), table));
    return 0;
}

void TabletImpl::DropTable(RpcController* controller,
        const ::rtidb::api::DropTableRequest* request,
        ::rtidb::api::DropTableResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);        
    std::shared_ptr<::rtidb::api::TaskInfo> task_ptr;
    if (request->has_task_info() && request->task_info().IsInitialized()) {
        if (AddOPTask(request->task_info(), ::rtidb::api::TaskType::kDropTable, task_ptr) < 0) {
            response->set_code(-1);
            response->set_msg("add task failed");
            return;
        }
    }
    uint32_t tid = request->tid();
    uint32_t pid = request->pid();
    PDLOG(INFO, "drop table. tid[%u] pid[%u]", tid, pid);
    do {
        if (!request->has_table_type() || request->table_type() == ::rtidb::type::kTimeSeries) {
            std::shared_ptr<Table> table = GetTable(tid, pid);
            if (!table) {
                response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
                response->set_msg("table is not exist");
                break;
            } else {
                if (table->GetTableStat() == ::rtidb::storage::kMakingSnapshot) {
                    PDLOG(WARNING, "making snapshot task is running now. tid[%u] pid[%u]", tid, pid);
                    response->set_code(::rtidb::base::ReturnCode::kTableStatusIsKmakingsnapshot);
                    response->set_msg("table status is kMakingSnapshot");
                    break;
                }
            }
            task_pool_.AddTask(boost::bind(&TabletImpl::DeleteTableInternal, this, tid, pid, task_ptr));
        } else {
            std::shared_ptr<RelationalTable> table;
            {
                std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
                table = GetRelationalTableUnLock(request->tid(), request->pid());
                if (!table) {
                    PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(), request->pid());
                    response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
                    response->set_msg("table is not exist");
                    break;
                }
            }
            task_pool_.AddTask(boost::bind(&TabletImpl::DeleteRelationalTableInternal, this, tid, pid, task_ptr));
        }
        response->set_code(::rtidb::base::ReturnCode::kOk);
        response->set_msg("ok");
        return;
    } while (0);
    if (task_ptr) {       
        std::lock_guard<std::mutex> lock(mu_);
        task_ptr->set_status(::rtidb::api::TaskStatus::kFailed);
    }
}

void TabletImpl::GetTaskStatus(RpcController* controller,
        const ::rtidb::api::TaskStatusRequest* request,
        ::rtidb::api::TaskStatusResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& kv : task_map_) {
        for (const auto& task_info : kv.second) {
            ::rtidb::api::TaskInfo* task = response->add_task();
            task->CopyFrom(*task_info);
        }
    }
    response->set_code(::rtidb::base::ReturnCode::kOk);
    response->set_msg("ok");
}

void TabletImpl::DeleteOPTask(RpcController* controller,
        const ::rtidb::api::DeleteTaskRequest* request,
        ::rtidb::api::GeneralResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::lock_guard<std::mutex> lock(mu_);
    for (int idx = 0; idx < request->op_id_size(); idx++) {
        auto iter = task_map_.find(request->op_id(idx));
        if (iter == task_map_.end()) {
            continue;
        }
        if (!iter->second.empty()) {
            PDLOG(INFO, "delete op task. op_id[%lu] op_type[%s] task_num[%u]", 
                        request->op_id(idx),
                        ::rtidb::api::OPType_Name(iter->second.front()->op_type()).c_str(),
                        iter->second.size());
            iter->second.clear();
        }
        task_map_.erase(iter);
    }
    response->set_code(::rtidb::base::ReturnCode::kOk);
    response->set_msg("ok");
}

void TabletImpl::ConnectZK(RpcController* controller,
            const ::rtidb::api::ConnectZKRequest* request,
            ::rtidb::api::GeneralResponse* response,
            Closure* done) {
    brpc::ClosureGuard done_guard(done);
    if (zk_client_->Reconnect() && zk_client_->Register()) {
        response->set_code(::rtidb::base::ReturnCode::kOk);
        response->set_msg("ok");
        PDLOG(INFO, "connect zk ok"); 
        return;
    }
    response->set_code(-1);
    response->set_msg("connect failed");
}

void TabletImpl::DisConnectZK(RpcController* controller,
            const ::rtidb::api::DisConnectZKRequest* request,
            ::rtidb::api::GeneralResponse* response,
            Closure* done) {
    brpc::ClosureGuard done_guard(done);
    zk_client_->CloseZK();
    response->set_code(::rtidb::base::ReturnCode::kOk);
    response->set_msg("ok");
    PDLOG(INFO, "disconnect zk ok"); 
    return;
}

void TabletImpl::SetConcurrency(RpcController* ctrl,
        const ::rtidb::api::SetConcurrencyRequest* request,
        ::rtidb::api::SetConcurrencyResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    if (server_ == NULL) {
        response->set_code(-1);
        response->set_msg("server is NULL");
        return;
    }

    if (request->max_concurrency() < 0) {
        response->set_code(::rtidb::base::ReturnCode::kInvalidConcurrency);
        response->set_msg("invalid concurrency " + request->max_concurrency());
        return;
    }

    if (SERVER_CONCURRENCY_KEY.compare(request->key()) == 0) {
        PDLOG(INFO, "update server max concurrency to %d", request->max_concurrency());
        server_->ResetMaxConcurrency(request->max_concurrency());
    }else {
        PDLOG(INFO, "update server api %s max concurrency to %d", request->key().c_str(), request->max_concurrency());
        server_->MaxConcurrencyOf(this, request->key()) = request->max_concurrency();
    }
    response->set_code(::rtidb::base::ReturnCode::kOk);
    response->set_msg("ok");
}

int TabletImpl::AddOPTask(const ::rtidb::api::TaskInfo& task_info, ::rtidb::api::TaskType task_type,
            std::shared_ptr<::rtidb::api::TaskInfo>& task_ptr) {
    std::lock_guard<std::mutex> lock(mu_);
    if (FindTask(task_info.op_id(), task_info.task_type())) {
        PDLOG(WARNING, "task is running. op_id[%lu] op_type[%s] task_type[%s]", 
                        task_info.op_id(),
                        ::rtidb::api::OPType_Name(task_info.op_type()).c_str(),
                        ::rtidb::api::TaskType_Name(task_info.task_type()).c_str());
        return -1;
    }
    task_ptr.reset(task_info.New());
    task_ptr->CopyFrom(task_info);
    task_ptr->set_status(::rtidb::api::TaskStatus::kDoing);
    auto iter = task_map_.find(task_info.op_id());
    if (iter == task_map_.end()) {
        task_map_.insert(std::make_pair(task_info.op_id(), 
                std::list<std::shared_ptr<::rtidb::api::TaskInfo>>()));
    }
    task_map_[task_info.op_id()].push_back(task_ptr);
    if (task_info.task_type() != task_type) {
        PDLOG(WARNING, "task type is not match. type is[%s]", 
                        ::rtidb::api::TaskType_Name(task_info.task_type()).c_str());
        task_ptr->set_status(::rtidb::api::TaskStatus::kFailed);
        return -1;
    }
    return 0;
}

std::shared_ptr<::rtidb::api::TaskInfo> TabletImpl::FindTask(
        uint64_t op_id, ::rtidb::api::TaskType task_type) {
    auto iter = task_map_.find(op_id);
    if (iter == task_map_.end()) {
        return std::shared_ptr<::rtidb::api::TaskInfo>();
    }
    for (auto& task : iter->second) {
        if (task->op_id() == op_id && task->task_type() == task_type) {
            return task;
        }
    }
    return std::shared_ptr<::rtidb::api::TaskInfo>();
}

int TabletImpl::AddOPMultiTask(const ::rtidb::api::TaskInfo& task_info, 
        ::rtidb::api::TaskType task_type,
        std::shared_ptr<::rtidb::api::TaskInfo>& task_ptr) {
    std::lock_guard<std::mutex> lock(mu_);
    if (FindMultiTask(task_info)) {
        PDLOG(WARNING, "task is running. op_id[%lu] op_type[%s] task_type[%s]", 
                        task_info.op_id(),
                        ::rtidb::api::OPType_Name(task_info.op_type()).c_str(),
                        ::rtidb::api::TaskType_Name(task_info.task_type()).c_str());
        return -1;
    }
    task_ptr.reset(task_info.New());
    task_ptr->CopyFrom(task_info);
    task_ptr->set_status(::rtidb::api::TaskStatus::kDoing);
    auto iter = task_map_.find(task_info.op_id());
    if (iter == task_map_.end()) {
        task_map_.insert(std::make_pair(task_info.op_id(), 
                std::list<std::shared_ptr<::rtidb::api::TaskInfo>>()));
    }
    task_map_[task_info.op_id()].push_back(task_ptr);
    if (task_info.task_type() != task_type) {
        PDLOG(WARNING, "task type is not match. type is[%s]", 
                        ::rtidb::api::TaskType_Name(task_info.task_type()).c_str());
        task_ptr->set_status(::rtidb::api::TaskStatus::kFailed);
        return -1;
    }
    return 0;
}

std::shared_ptr<::rtidb::api::TaskInfo> TabletImpl::FindMultiTask(const ::rtidb::api::TaskInfo& task_info) {
    auto iter = task_map_.find(task_info.op_id());
    if (iter == task_map_.end()) {
        return std::shared_ptr<::rtidb::api::TaskInfo>();
    }
    for (auto& task : iter->second) {
        if (task->op_id() == task_info.op_id() && 
                task->task_type() == task_info.task_type() &&
                task->task_id() == task_info.task_id()) {
            return task;
        }
    }
    return std::shared_ptr<::rtidb::api::TaskInfo>();
}

void TabletImpl::GcTable(uint32_t tid, uint32_t pid, bool execute_once) {
    std::shared_ptr<Table> table = GetTable(tid, pid);
    if (table) {
        int32_t gc_interval = FLAGS_gc_interval;
        if (table->GetStorageMode() != ::rtidb::common::StorageMode::kMemory) {
            gc_interval = FLAGS_disk_gc_interval;
        }
        table->SchedGc();
        if (!execute_once) {
            gc_pool_.DelayTask(gc_interval * 60 * 1000, boost::bind(&TabletImpl::GcTable, this, tid, pid, false));
        }
        return;
    }
}

std::shared_ptr<Snapshot> TabletImpl::GetSnapshot(uint32_t tid, uint32_t pid) {
    std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
    return GetSnapshotUnLock(tid, pid);
}

std::shared_ptr<Snapshot> TabletImpl::GetSnapshotUnLock(uint32_t tid, uint32_t pid) {
    Snapshots::iterator it = snapshots_.find(tid);
    if (it != snapshots_.end()) {
        auto tit = it->second.find(pid);
        if (tit != it->second.end()) {
            return tit->second;
        }
    }
    return std::shared_ptr<Snapshot>();
}

std::shared_ptr<LogReplicator> TabletImpl::GetReplicatorUnLock(uint32_t tid, uint32_t pid) {
    Replicators::iterator it = replicators_.find(tid);
    if (it != replicators_.end()) {
        auto tit = it->second.find(pid);
        if (tit != it->second.end()) {
            return tit->second;
        }
    }
    return std::shared_ptr<LogReplicator>();
}

std::shared_ptr<LogReplicator> TabletImpl::GetReplicator(uint32_t tid, uint32_t pid) {
    std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
    return GetReplicatorUnLock(tid, pid);
}

std::shared_ptr<Table> TabletImpl::GetTable(uint32_t tid, uint32_t pid) {
    std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
    return GetTableUnLock(tid, pid);
}

std::shared_ptr<Table> TabletImpl::GetTableUnLock(uint32_t tid, uint32_t pid) {
    Tables::iterator it = tables_.find(tid);
    if (it != tables_.end()) {
        auto tit = it->second.find(pid);
        if (tit != it->second.end()) {
            return tit->second;
        }
    }
    return std::shared_ptr<Table>();
}

std::shared_ptr<RelationalTable> TabletImpl::GetRelationalTableUnLock(uint32_t tid, uint32_t pid) {
    RelationalTables::iterator it = relational_tables_.find(tid);
    if (it != relational_tables_.end()) {
        auto tit = it->second.find(pid);
        if (tit != it->second.end()) {
            return tit->second;
        }
    }
    return std::shared_ptr<RelationalTable>();
}

void TabletImpl::ShowMemPool(RpcController* controller,
            const ::rtidb::api::HttpRequest* request,
            ::rtidb::api::HttpResponse* response,
            Closure* done) {
    brpc::ClosureGuard done_guard(done);
#ifdef TCMALLOC_ENABLE
    brpc::Controller* cntl = static_cast<brpc::Controller*>(controller);
    MallocExtension* tcmalloc = MallocExtension::instance();
    std::string stat;
    stat.resize(1024);
    char* buffer = reinterpret_cast<char*>(& (stat[0]));
    tcmalloc->GetStats(buffer, 1024);
    cntl->response_attachment().append("<html><head><title>Mem Stat</title></head><body><pre>");
    cntl->response_attachment().append(stat);
    cntl->response_attachment().append("</pre></body></html>");
#endif
}

void TabletImpl::CheckZkClient() {
    if (!zk_client_->IsConnected()) {
        PDLOG(WARNING, "reconnect zk"); 
        if (zk_client_->Reconnect() && zk_client_->Register()) {
            PDLOG(INFO, "reconnect zk ok"); 
        }
    } else if (!zk_client_->IsRegisted()) {
        PDLOG(WARNING, "registe zk"); 
        if (zk_client_->Register()) {
            PDLOG(INFO, "registe zk ok"); 
        }
    }
    keep_alive_pool_.DelayTask(FLAGS_zk_keep_alive_check_interval, boost::bind(&TabletImpl::CheckZkClient, this));
}

int TabletImpl::CheckDimessionPut(const ::rtidb::api::PutRequest* request,
                                      uint32_t idx_cnt) {
    for (int32_t i = 0; i < request->dimensions_size(); i++) {
        if (idx_cnt <= request->dimensions(i).idx()) {
            PDLOG(WARNING, "invalid put request dimensions, request idx %u is greater than table idx cnt %u", 
                    request->dimensions(i).idx(), idx_cnt);
            return -1;
        }
        if (request->dimensions(i).key().length() <= 0) {
            PDLOG(WARNING, "invalid put request dimension key is empty with idx %u", request->dimensions(i).idx());
            return 1;
        }
    }
    return 0;
}

void TabletImpl::SchedSyncDisk(uint32_t tid, uint32_t pid) {
    std::shared_ptr<LogReplicator> replicator = GetReplicator(tid, pid);
    if (replicator) {
        replicator->SyncToDisk();
        io_pool_.DelayTask(FLAGS_binlog_sync_to_disk_interval, boost::bind(&TabletImpl::SchedSyncDisk, this, tid, pid));
    }
}

void TabletImpl::SchedDelBinlog(uint32_t tid, uint32_t pid) {
    std::shared_ptr<LogReplicator> replicator = GetReplicator(tid, pid);
    if (replicator) {
        replicator->DeleteBinlog();
        task_pool_.DelayTask(FLAGS_binlog_delete_interval, boost::bind(&TabletImpl::SchedDelBinlog, this, tid, pid));
    }
}

bool TabletImpl::ChooseDBRootPath(uint32_t tid, uint32_t pid,
                                  const ::rtidb::common::StorageMode& mode,
                                  std::string& path) {

    std::vector<std::string>& paths = mode_root_paths_[mode];
    if (paths.size() < 1) {
        return false;
    }

    if (paths.size() == 1) {
        path.assign(paths[0]);
        return path.size();
    }

    std::string key = std::to_string(tid) + std::to_string(pid);
    uint32_t index = ::rtidb::base::hash(key.c_str(), key.size(), SEED) % paths.size();
    path.assign(paths[index]);
    return path.size();
}


bool TabletImpl::ChooseRecycleBinRootPath(uint32_t tid, uint32_t pid,
                                  const ::rtidb::common::StorageMode& mode,
                                  std::string& path) {
    std::vector<std::string>& paths = mode_recycle_root_paths_[mode];
    if (paths.size() < 1) return false;

    if (paths.size() == 1) {
        path.assign(paths[0]);
        return true;
    }
    std::string key = std::to_string(tid) + std::to_string(pid);
    uint32_t index = ::rtidb::base::hash(key.c_str(), key.size(), SEED) % paths.size();
    path.assign(paths[index]);
    return true;
}

void TabletImpl::DelRecycle(const std::string &path) {
    std::vector<std::string> file_vec;
    ::rtidb::base::GetChildFileName(path, file_vec);
    for(auto file_path : file_vec) {
        std::string file_name = ::rtidb::base::ParseFileNameFromPath(file_path);
        std::vector<std::string> parts;
        int64_t recycle_time;
        int64_t now_time = ::baidu::common::timer::get_micros() / 1000000;
        ::rtidb::base::SplitString(file_name, "_", parts);
        if(parts.size() == 3) {
            recycle_time = ::rtidb::base::ParseTimeToSecond(parts[2], "%Y%m%d%H%M%S");
        } else {
            recycle_time = ::rtidb::base::ParseTimeToSecond(parts[3], "%Y%m%d%H%M%S");
        }
        if (FLAGS_recycle_ttl != 0 && (now_time - recycle_time) > FLAGS_recycle_ttl * 60) {
            PDLOG(INFO, "delete recycle dir %s", file_path.c_str());
            ::rtidb::base::RemoveDirRecursive(file_path);
        }
    }
}

void TabletImpl::SchedDelRecycle() {
    for (auto kv : mode_recycle_root_paths_) {
        for(auto path : kv.second) {
            DelRecycle(path);
        }
    }
    task_pool_.DelayTask(FLAGS_recycle_ttl*60*1000, boost::bind(&TabletImpl::SchedDelRecycle, this));
}

bool TabletImpl::CreateMultiDir(const std::vector<std::string>& dirs) {

    std::vector<std::string>::const_iterator it = dirs.begin();
    for (; it != dirs.end(); ++it) {
        std::string path = *it;
        bool ok = ::rtidb::base::MkdirRecur(path);
        if (!ok) {
            PDLOG(WARNING, "fail to create dir %s", path.c_str());
            return false;
        }
    }
    return true;
}

bool TabletImpl::ChooseTableRootPath(uint32_t tid, uint32_t pid,
        const ::rtidb::common::StorageMode& mode,
        std::string& path) {
    std::string root_path;
    bool ok = ChooseDBRootPath(tid, pid, mode, root_path);
    if (!ok) {
        PDLOG(WARNING, "table db path doesn't found. tid %u, pid %u", tid, pid);
        return false;
    }
    path = root_path + "/" + std::to_string(tid) + 
                        "_" + std::to_string(pid);
    if (!::rtidb::base::IsExists(path)) {
        PDLOG(WARNING, "table db path doesn`t exist. tid %u, pid %u", tid, pid);
        return false;
    }
    return true;
}

bool TabletImpl::GetTableRootSize(uint32_t tid, uint32_t pid, const
        ::rtidb::common::StorageMode& mode, uint64_t& size) {
    std::string table_path;
    if (!ChooseTableRootPath(tid, pid, mode, table_path)) {
        return false;
    }
    if (!::rtidb::base::GetDirSizeRecur(table_path, size)) {
        PDLOG(WARNING, "get table root size failed. tid %u, pid %u", tid, pid);
        return false;
    }
    return true;
}

void TabletImpl::GetDiskused() {
    std::vector<std::shared_ptr<Table>> tables;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = tables_.begin(); it != tables_.end(); ++it) {
            for (auto pit = it->second.begin(); pit != it->second.end(); ++pit) {
                tables.push_back(pit->second);
            }
        }
    }
    for (const auto &table : tables) {
        uint64_t size = 0;
        if (!GetTableRootSize(table->GetId(), table->GetPid(), table->GetStorageMode(), size)) {
            PDLOG(WARNING, "get table root size failed. tid[%u] pid[%u]", table->GetId(), table->GetPid());
        } else {
            table->SetDiskused(size);
        }
    }
    task_pool_.DelayTask(FLAGS_get_table_diskused_interval, boost::bind(&TabletImpl::GetDiskused, this));
}

bool TabletImpl::SeekWithCount(::rtidb::storage::TableIterator* it, const uint64_t time,
        const ::rtidb::api::GetType& type, uint32_t max_cnt, uint32_t& cnt) {
    if (it == NULL) {
        return false;
    }
    it->SeekToFirst();
    while(it->Valid() && (cnt < max_cnt || max_cnt == 0)) {
        switch(type) {
            case ::rtidb::api::GetType::kSubKeyEq:
                if (it->GetKey() <= time) {
                    return it->GetKey() == time;
                }
                break;
            case ::rtidb::api::GetType::kSubKeyLe:
                if (it->GetKey() <= time) {
                    return true;
                }
                break;
            case ::rtidb::api::GetType::kSubKeyLt:
                if (it->GetKey() < time) {
                    return true;
                }
                break;
            case ::rtidb::api::GetType::kSubKeyGe:
                return it->GetKey() >= time;
            case ::rtidb::api::GetType::kSubKeyGt:
                return it->GetKey() > time;
            default:
                return false;
        }
        it->Next();
        ++cnt;
    }
    return false;
}

bool TabletImpl::Seek(::rtidb::storage::TableIterator* it, const uint64_t time,
        const ::rtidb::api::GetType& type) {
    if (it == NULL) {
        return false;
    }
    switch(type) {
        case ::rtidb::api::GetType::kSubKeyEq:
            it->Seek(time);
            return it->Valid() && it->GetKey() == time;
        case ::rtidb::api::GetType::kSubKeyLe:
            it->Seek(time);
            return it->Valid();
        case ::rtidb::api::GetType::kSubKeyLt:
            it->Seek(time - 1);
            return it->Valid();
        case ::rtidb::api::GetType::kSubKeyGe:
            it->SeekToFirst();
            return it->Valid() && it->GetKey() >= time;
        case ::rtidb::api::GetType::kSubKeyGt:
            it->SeekToFirst();
            return it->Valid() && it->GetKey() > time;
        default:
            return false;
    }
    return false;
}

void TabletImpl::SetMode(RpcController* controller,
        const ::rtidb::api::SetModeRequest* request,
        ::rtidb::api::GeneralResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    follower_.store(request->follower(), std::memory_order_relaxed);
    std::string mode = request->follower() == true ? "follower" : "normal";
    PDLOG(INFO, "set tablet mode %s", mode.c_str());
    response->set_code(::rtidb::base::ReturnCode::kOk);
}

void TabletImpl::DeleteIndex(RpcController* controller,
        const ::rtidb::api::DeleteIndexRequest* request,
        ::rtidb::api::GeneralResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::map<uint32_t, std::shared_ptr<Table>> tables;
    {
        std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
        auto iter = tables_.find(request->tid());
        if (iter == tables_.end()) {
            response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
            response->set_msg("table is not exist");
            return;
        }
        tables = iter->second;
        if (tables.begin()->second->GetStorageMode() != ::rtidb::common::kMemory) {
            response->set_code(::rtidb::base::ReturnCode::kOperatorNotSupport);
            response->set_msg("only support mem_table");
            return;
        }
        for (const auto& kv: tables) {
            std::string root_path;
            MemTable* mem_table = dynamic_cast<MemTable*>(kv.second.get());
            if (!mem_table->DeleteIndex(request->idx_name())) {
                response->set_code(::rtidb::base::ReturnCode::kIndexDeleteFailed);
                response->set_msg("delete index fail!");
                PDLOG(WARNING, "delete index %s failed. tid %u pid %u", 
                        request->idx_name().c_str(), request->tid(), kv.first);
                return;
            }
            bool ok = ChooseDBRootPath(request->tid(), kv.second.get()->GetPid(), kv.second.get()->GetStorageMode(), root_path);
            if (!ok) {
                response->set_code(::rtidb::base::ReturnCode::kFailToGetDbRootPath);
                response->set_msg("fail to get table db root path");
                PDLOG(WARNING, "table db path is not found. tid %u, pid %u", request->tid(), kv.second.get()->GetPid());
                break;
            }
            std::string db_path = root_path + "/" + std::to_string(request->tid()) + 
                "_" + std::to_string(kv.second.get()->GetPid());
            WriteTableMeta(db_path, &kv.second.get()->GetTableMeta());
            PDLOG(INFO, "delete index %s success. tid %u pid %u", 
                            request->idx_name().c_str(), request->tid(), kv.first);
        }
    }
    PDLOG(INFO, "delete index %s success. tid %u", request->idx_name().c_str(), request->tid());
    response->set_code(0);
    response->set_msg("ok");
}

void TabletImpl::DumpIndexData(RpcController* controller,
        const ::rtidb::api::DumpIndexDataRequest* request,
        ::rtidb::api::GeneralResponse* response,
        Closure* done) {
    std::shared_ptr<Table> table;
    std::shared_ptr<Snapshot> snapshot;
    std::shared_ptr<LogReplicator> replicator;
    std::string db_root_path;
    {
        std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
        table = GetTableUnLock(request->tid(), request->pid());
        if (!table) {
            PDLOG(WARNING, "table is not exist. tid[%u] pid[%u]", request->tid(), request->pid());
            response->set_code(::rtidb::base::ReturnCode::kTableIsNotExist);
            response->set_msg("table is not exist");
            return;
        }
        if (table->GetStorageMode() != ::rtidb::common::kMemory) {
            response->set_code(::rtidb::base::ReturnCode::kOperatorNotSupport);
            response->set_msg("only support mem_table");
            return;
        }
        if (table->GetTableStat() != ::rtidb::storage::kNormal) {
            PDLOG(WARNING, "table state is %d, cannot dump index data. %u, pid %u", 
                    table->GetTableStat(), request->tid(), request->pid());
            response->set_code(::rtidb::base::ReturnCode::kTableStatusIsNotKnormal);
            response->set_msg("table status is not kNormal");
            return;
        }
        snapshot = GetSnapshotUnLock(request->tid(), request->pid());
        if (!snapshot) {
            PDLOG(WARNING, "snapshot is not exist. tid[%u] pid[%u]", request->tid(), request->pid());
            response->set_code(::rtidb::base::ReturnCode::kSnapshotIsNotExist);
            response->set_msg("table snapshot is not exist");
            return;
        }
        replicator = GetReplicatorUnLock(request->tid(), request->pid());
        if (!replicator) {
            PDLOG(WARNING, "fail to find table tid %u pid %u leader's log replicator", 
                request->tid(),request->pid());
            response->set_code(::rtidb::base::ReturnCode::kReplicatorIsNotExist);
            response->set_msg("replicator is not exist");
            return;
        }
        bool ok = ChooseDBRootPath(request->tid(), request->pid(), table->GetStorageMode(), db_root_path);
        if (!ok) {
            PDLOG(WARNING, "fail to find db root path for table tid %u pid %u", request->tid(), request->pid());
            response->set_code(::rtidb::base::ReturnCode::kFailToGetDbRootPath);
            response->set_msg("fail to get db root path");
            return;
        }
    }
    std::string index_path = db_root_path + "/" + std::to_string(request->tid()) + "_" + std::to_string(request->pid()) + "/index/";
    if (!::rtidb::base::MkdirRecur(index_path)) {
        PDLOG(WARNING, "fail to create path %s", index_path.c_str());
        response->set_code(::rtidb::base::ReturnCode::kFailToCreateFile);
        response->set_msg("fail to create path");
        return;
    }
    std::string binlog_path = db_root_path + "/" + std::to_string(request->tid()) + "_" + std::to_string(request->pid()) + "/binlog/";
    ::rtidb::storage::Binlog binlog(replicator->GetLogPart(), binlog_path);
    std::vector<::rtidb::log::WriteHandle*> whs;
    for (int i=0;i<request->partition_num();++i) {
        std::string index_file_name = std::to_string(request->pid()) + "_" + std::to_string(i) + "_index.data";
        std::string index_data_path = index_path + index_file_name;
        FILE* fd = fopen(index_data_path.c_str(), "wb+");
        if (fd == NULL) {
            PDLOG(WARNING, "fail to create file %s", index_data_path.c_str());
            response->set_code(::rtidb::base::ReturnCode::kFailToGetDbRootPath);
            response->set_msg("fail to get db root path");
            return;
        }
        ::rtidb::log::WriteHandle* wh = new ::rtidb::log::WriteHandle(index_file_name, fd);
        whs.push_back(wh);
    }
    task_pool_.AddTask(boost::bind(&TabletImpl::LoadTableInternal, this, tid, pid, task_ptr));
    uint64_t offset = 0;
    std::shared_ptr<::rtidb::storage::MemTableSnapshot> memtable_snapshot = std::static_pointer_cast<::rtidb::storage::MemTableSnapshot>(snapshot);
    if (memtable_snapshot->DumpSnapshotIndexData(table, request->column_key(), request->idx(), whs, offset) && binlog.DumpBinlogIndexData(table, request->column_key(), request->idx(), whs, offset)) {
        PDLOG(INFO, "dump index on table tid[%u] pid[%u] succeed", request->tid(), request->pid());
        response->set_code(::rtidb::base::ReturnCode::kOk);
        response->set_msg("ok");
    } else {
        PDLOG(WARNING, "fail to dump index on table tid[%u] pid[%u]", request->tid(), request->pid());
        response->set_code(::rtidb::base::ReturnCode::kDumpIndexDataFailed);
        response->set_msg("dump index data failed");
    }
    for (auto& wh : whs) {
        wh->EndLog();
        delete wh;
        wh = NULL;
    }
}

void TabletImpl::DumpIndexDataInternal() {
    uint64_t offset = 0;
    std::shared_ptr<::rtidb::storage::MemTableSnapshot> memtable_snapshot = std::static_pointer_cast<::rtidb::storage::MemTableSnapshot>(snapshot);
    if (memtable_snapshot->DumpSnapshotIndexData(table, request->column_key(), request->idx(), whs, offset) && binlog.DumpBinlogIndexData(table, request->column_key(), request->idx(), whs, offset)) {
        PDLOG(INFO, "dump index on table tid[%u] pid[%u] succeed", request->tid(), request->pid());
        response->set_code(::rtidb::base::ReturnCode::kOk);
        response->set_msg("ok");
    } else {
        PDLOG(WARNING, "fail to dump index on table tid[%u] pid[%u]", request->tid(), request->pid());
        response->set_code(::rtidb::base::ReturnCode::kDumpIndexDataFailed);
        response->set_msg("dump index data failed");
    }
    for (auto& wh : whs) {
        wh->EndLog();
        delete wh;
        wh = NULL;
    }
}


}
}


