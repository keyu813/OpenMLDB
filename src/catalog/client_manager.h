/*
 * client_manager.h
 * Copyright (C) 4paradigm.com 2020
 * Author denglong
 * Date 2020-09-14
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_CATALOG_CLIENT_MANAGER_H_
#define SRC_CATALOG_CLIENT_MANAGER_H_

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/random.h"
#include "base/spinlock.h"
#include "client/tablet_client.h"
#include "storage/schema.h"
#include "vm/catalog.h"
#include "vm/mem_catalog.h"

namespace rtidb {
namespace catalog {

using TablePartitions = ::google::protobuf::RepeatedPtrField<::rtidb::nameserver::TablePartition>;

class TabletRowHandler : public ::fesql::vm::RowHandler {
 public:
    TabletRowHandler(const std::string& db, rtidb::RpcCallback<rtidb::api::QueryResponse>* callback);
    ~TabletRowHandler();
    explicit TabletRowHandler(::fesql::base::Status status);
    const ::fesql::vm::Schema* GetSchema() override { return nullptr; }
    const std::string& GetName() override { return name_; }
    const std::string& GetDatabase() override { return db_; }

    ::fesql::base::Status GetStatus() override { return status_; }
    const ::fesql::codec::Row& GetValue() override;

 private:
    std::string db_;
    std::string name_;
    ::fesql::base::Status status_;
    ::fesql::codec::Row row_;
    rtidb::RpcCallback<rtidb::api::QueryResponse>* callback_;
};
class AsyncTableHandler : public ::fesql::vm::MemTableHandler {
 public:
    AsyncTableHandler(rtidb::RpcCallback<rtidb::api::SQLBatchRequestQueryResponse>* callback);
    ~AsyncTableHandler() {}
    std::unique_ptr<fesql::vm::RowIterator> GetIterator();
    fesql::vm::RowIterator* GetRawIterator();
    std::unique_ptr<fesql::vm::WindowIterator> GetWindowIterator(
        const std::string& idx_name) {
        return std::unique_ptr<fesql::vm::WindowIterator>();
    }
    const std::string GetHandlerTypeName() override {
        return "AsyncTableHandler";
    }
    virtual fesql::base::Status GetStatus() { return status_; }
 private:
    void SyncRpcResponse();
    fesql::base::Status status_;
    rtidb::RpcCallback<rtidb::api::SQLBatchRequestQueryResponse>* callback_;
};
class AsyncTablesHandler : public ::fesql::vm::MemTableHandler {
 public:
    AsyncTablesHandler();
    ~AsyncTablesHandler() {}
    void AddAsyncRpcHandler(std::shared_ptr<TableHandler> handler,
                            std::vector<size_t>& pos_info) {
        handlers_.push_back(handler);
        posinfos_.push_back(pos_info);
        rows_cnt_ += pos_info.size();
    }
    std::unique_ptr<fesql::vm::RowIterator> GetIterator();
    fesql::vm::RowIterator* GetRawIterator();
    std::unique_ptr<fesql::vm::WindowIterator> GetWindowIterator(
        const std::string& idx_name) {
        return std::unique_ptr<fesql::vm::WindowIterator>();
    }
    const std::string GetHandlerTypeName() override {
        return "AsyncTableHandler";
    }
    virtual fesql::base::Status GetStatus() { return status_; }
 private:
    bool SyncAllTableHandlers();
    fesql::base::Status status_;
    size_t rows_cnt_;
    std::vector<std::vector<size_t >> posinfos_;
    std::vector<std::shared_ptr<TableHandler>> handlers_;
};

class TabletAccessor : public ::fesql::vm::Tablet {
 public:
    explicit TabletAccessor(const std::string& name) : name_(name), tablet_client_() {}

    TabletAccessor(const std::string& name, const std::shared_ptr<::rtidb::client::TabletClient>& client)
        : name_(name), tablet_client_(client) {}

    std::shared_ptr<::rtidb::client::TabletClient> GetClient() {
        return std::atomic_load_explicit(&tablet_client_, std::memory_order_relaxed);
    }

    bool UpdateClient(const std::string& endpoint) {
        auto client = std::make_shared<::rtidb::client::TabletClient>(name_, endpoint);
        if (client->Init() != 0) {
            return false;
        }
        std::atomic_store_explicit(&tablet_client_, client, std::memory_order_relaxed);
        return true;
    }

    bool UpdateClient(const std::shared_ptr<::rtidb::client::TabletClient>& client) {
        std::atomic_store_explicit(&tablet_client_, client, std::memory_order_relaxed);
        return true;
    }

    std::shared_ptr<::fesql::vm::RowHandler> SubQuery(uint32_t task_id, const std::string& db, const std::string& sql,
                                                      const ::fesql::codec::Row& row,
                                                      const bool is_procedure,
                                                      const bool is_debug) override;

    std::shared_ptr<::fesql::vm::TableHandler> SubQuery(uint32_t task_id, const std::string& db, const std::string& sql,
                                                      const std::vector<::fesql::codec::Row>& row,
                                                      const bool is_procedure,
                                                      const bool is_debug) override;
    const std::string& GetName() const { return name_; }
 private:
    std::string name_;
    std::shared_ptr<::rtidb::client::TabletClient> tablet_client_;
};
class TabletsAccessor : public ::fesql::vm::Tablet {
 public:
    TabletsAccessor(): rows_cnt_(0) {}
    ~TabletsAccessor() {}
    void AddTabletAccessor(std::shared_ptr<TabletAccessor> accessor) {
        auto iter = name_idx_map_.find(accessor->GetName());
        if (iter == name_idx_map_.cend()) {
            accessors_.push_back(accessor);
            name_idx_map_.insert(std::make_pair(accessor->GetName(), accessors_.size()-1));
            posinfos_.push_back(std::vector<size_t>({rows_cnt_}));
            assign_accessor_idxs_.push_back(accessors_.size()-1);
        } else {
            posinfos_[iter->second].push_back(rows_cnt_);
            assign_accessor_idxs_.push_back(iter->second);
        }
        rows_cnt_++;
    }
    std::shared_ptr<fesql::vm::RowHandler> SubQuery(uint32_t task_id, const std::string& db, const std::string& sql,
                                                    const fesql::codec::Row& row, const bool is_procedure,
                                                    const bool is_debug) override;
    std::shared_ptr<fesql::vm::TableHandler> SubQuery(uint32_t task_id, const std::string& db, const std::string& sql,
                                                      const std::vector<fesql::codec::Row>& rows,
                                                      const bool is_procedure, const bool is_debug);

 private:
    size_t rows_cnt_;
    std::vector<std::shared_ptr<Tablet>> accessors_;
    std::vector<size_t > assign_accessor_idxs_;
    std::vector<std::vector<size_t>> posinfos_;
    std::map<std::string, size_t> name_idx_map_;
};
class PartitionClientManager {
 public:
    PartitionClientManager(uint32_t pid, const std::shared_ptr<TabletAccessor>& leader,
                           const std::vector<std::shared_ptr<TabletAccessor>>& followers);

    inline std::shared_ptr<TabletAccessor> GetLeader() const { return leader_; }

    std::shared_ptr<TabletAccessor> GetFollower();

 private:
    uint32_t pid_;
    std::shared_ptr<TabletAccessor> leader_;
    std::vector<std::shared_ptr<TabletAccessor>> followers_;
    ::rtidb::base::Random rand_;
};

class ClientManager;

class TableClientManager {
 public:
    TableClientManager(const TablePartitions& partitions, const ClientManager& client_manager);

    TableClientManager(const ::rtidb::storage::TableSt& table_st, const ClientManager& client_manager);

    void Show() const {
        DLOG(INFO) << "show client manager ";
        for (size_t id = 0; id < partition_managers_.size(); id++) {
            auto pmg = std::atomic_load_explicit(&partition_managers_[id], std::memory_order_relaxed);
            if (pmg) {
                if (pmg->GetLeader()) {
                    DLOG(INFO) << "partition managers (pid, leader) " << id << ", " << pmg->GetLeader()->GetName();
                } else {
                    DLOG(INFO) << "partition managers (pid, leader) " << id << ", null leader";
                }
            } else {
                DLOG(INFO) << "partition managers (pid, leader) " << id << ", null mamanger";
            }
        }
    }
    std::shared_ptr<PartitionClientManager> GetPartitionClientManager(uint32_t pid) const {
        if (pid < partition_managers_.size()) {
            return std::atomic_load_explicit(&partition_managers_[pid], std::memory_order_relaxed);
        }
        return std::shared_ptr<PartitionClientManager>();
    }

    bool UpdatePartitionClientManager(const ::rtidb::storage::PartitionSt& partition,
                                      const ClientManager& client_manager);

    std::shared_ptr<TabletAccessor> GetTablet(uint32_t pid) const {
        auto partition_manager = GetPartitionClientManager(pid);
        if (partition_manager) {
            return partition_manager->GetLeader();
        }
        return std::shared_ptr<TabletAccessor>();
    }
    std::shared_ptr<TabletsAccessor> GetTablet(std::vector<uint32_t> pids) const {
        std::shared_ptr<TabletsAccessor> tablets_accessor = std::shared_ptr<TabletsAccessor>(new TabletsAccessor());
        for(size_t idx = 0; idx <  pids.size(); idx++) {
            auto partition_manager = GetPartitionClientManager(pids[idx]);
            if (partition_manager) {
                tablets_accessor->AddTabletAccessor(partition_manager->GetLeader());
            } else {
                LOG(WARNING) << "fail to get tablet: pid " << pids[idx] << " not exist";
                return std::shared_ptr<TabletsAccessor>();
            }
        }
        return tablets_accessor;
    }

 private:
    std::vector<std::shared_ptr<PartitionClientManager>> partition_managers_;
};

class ClientManager {
 public:
    ClientManager() : real_endpoint_map_(), clients_(), mu_(), rand_(0xdeadbeef) {}
    std::shared_ptr<TabletAccessor> GetTablet(const std::string& name) const;
    std::shared_ptr<TabletAccessor> GetTablet() const;

    bool UpdateClient(const std::map<std::string, std::string>& real_ep_map);

    bool UpdateClient(const std::map<std::string, std::shared_ptr<::rtidb::client::TabletClient>>& tablet_clients);

 private:
    std::unordered_map<std::string, std::string> real_endpoint_map_;
    std::unordered_map<std::string, std::shared_ptr<TabletAccessor>> clients_;
    mutable ::rtidb::base::SpinMutex mu_;
    mutable ::rtidb::base::Random rand_;
};

}  // namespace catalog
}  // namespace rtidb
#endif  // SRC_CATALOG_CLIENT_MANAGER_H_
