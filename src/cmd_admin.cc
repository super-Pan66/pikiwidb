/*
 * Copyright (c) 2023-present, Qihoo, Inc.  All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "cmd_admin.h"
#include "pikiwidb.h"
#include "db.h"
#include "pstd/env.h"
#include "store.h"

namespace pikiwidb {

CmdConfig::CmdConfig(const std::string& name, int arity) : BaseCmdGroup(name, kCmdFlagsAdmin, kAclCategoryAdmin) {}

bool CmdConfig::HasSubCommand() const { return true; }

CmdConfigGet::CmdConfigGet(const std::string& name, int16_t arity)
    : BaseCmd(name, arity, kCmdFlagsAdmin | kCmdFlagsWrite, kAclCategoryAdmin) {}

bool CmdConfigGet::DoInitial(PClient* client) { return true; }

void CmdConfigGet::DoCmd(PClient* client) {
  std::vector<std::string> results;
  for (int i = 0; i < client->argv_.size() - 2; i++) {
    g_config.Get(client->argv_[i + 2], &results);
  }
  client->AppendStringVector(results);
}

CmdConfigSet::CmdConfigSet(const std::string& name, int16_t arity)
    : BaseCmd(name, arity, kCmdFlagsAdmin, kAclCategoryAdmin) {}

bool CmdConfigSet::DoInitial(PClient* client) { return true; }

void CmdConfigSet::DoCmd(PClient* client) {
  auto s = g_config.Set(client->argv_[2], client->argv_[3]);
  if (!s.ok()) {
    client->SetRes(CmdRes::kInvalidParameter);
  } else {
    client->SetRes(CmdRes::kOK);
  }
}

FlushdbCmd::FlushdbCmd(const std::string& name, int16_t arity)
    : BaseCmd(name, arity, kCmdFlagsExclusive | kCmdFlagsAdmin | kCmdFlagsWrite,
              kAclCategoryWrite | kAclCategoryAdmin) {}

bool FlushdbCmd::DoInitial(PClient* client) { return true; }

void FlushdbCmd::DoCmd(PClient* client) {
  int currentDBIndex = client->GetCurrentDB();
  PSTORE.GetBackend(currentDBIndex).get()->Lock();
  PSTORE.GetBackend(currentDBIndex)->GetStorage().reset();

  std::string db_path = g_config.db_path.ToString() + std::to_string(currentDBIndex);
  std::string path_temp = db_path;
  path_temp.append("_deleting/");
  pstd::RenameFile(db_path, path_temp);

  PSTORE.GetBackend(currentDBIndex)->GetStorage() = std::make_unique<storage::Storage>();
  storage::StorageOptions storage_options;
  storage_options.options = g_config.GetRocksDBOptions();
  auto cap = storage_options.db_instance_num * kColumnNum * storage_options.options.write_buffer_size *
            storage_options.options.max_write_buffer_number;
  storage_options.options.write_buffer_manager = std::make_shared<rocksdb::WriteBufferManager>(cap);

  storage_options.table_options = g_config.GetRocksDBBlockBasedTableOptions();

  storage_options.small_compaction_threshold = g_config.small_compaction_threshold.load();
  storage_options.small_compaction_duration_threshold = g_config.small_compaction_duration_threshold.load();
  storage_options.db_instance_num = g_config.db_instance_num;
  storage_options.db_id = currentDBIndex;

  storage::Status s = PSTORE.GetBackend(currentDBIndex)->GetStorage()->Open(storage_options, db_path.data());
  assert(s.ok());
  pstd::DeleteDir(path_temp);
  PSTORE.GetBackend(currentDBIndex).get()->UnLock();
  client->SetRes(CmdRes::kOK);
}

FlushallCmd::FlushallCmd(const std::string& name, int16_t arity)
    : BaseCmd(name, arity, kCmdFlagsExclusive | kCmdFlagsAdmin | kCmdFlagsWrite,
              kAclCategoryWrite | kAclCategoryAdmin) {}

bool FlushallCmd::DoInitial(PClient* client) { return true; }

void FlushallCmd::DoCmd(PClient* client) {
  for (size_t i = 0; i < g_config.databases; ++i) {
    PSTORE.GetBackend(i).get()->Lock();
    PSTORE.GetBackend(i)->GetStorage().reset();

    std::string db_path = g_config.db_path.ToString() + std::to_string(i);
    std::string path_temp = db_path;
    path_temp.append("_deleting/");
    pstd::RenameFile(db_path, path_temp);

    PSTORE.GetBackend(i)->GetStorage() = std::make_unique<storage::Storage>();
    storage::StorageOptions storage_options;
    storage_options.options = g_config.GetRocksDBOptions();
    auto cap = storage_options.db_instance_num * kColumnNum * storage_options.options.write_buffer_size *
              storage_options.options.max_write_buffer_number;
    storage_options.options.write_buffer_manager = std::make_shared<rocksdb::WriteBufferManager>(cap);

    storage_options.table_options = g_config.GetRocksDBBlockBasedTableOptions();

    storage_options.small_compaction_threshold = g_config.small_compaction_threshold.load();
    storage_options.small_compaction_duration_threshold = g_config.small_compaction_duration_threshold.load();
    storage_options.db_instance_num = g_config.db_instance_num;
    storage_options.db_id = static_cast<int>(i);  

    storage::Status s = PSTORE.GetBackend(i)->GetStorage()->Open(storage_options, db_path.data());
    assert(s.ok());
    pstd::DeleteDir(path_temp);
    PSTORE.GetBackend(i).get()->UnLock();
  }
  client->SetRes(CmdRes::kOK);
}

SelectCmd::SelectCmd(const std::string& name, int16_t arity)
    : BaseCmd(name, arity, kCmdFlagsAdmin | kCmdFlagsReadonly, kAclCategoryAdmin) {}

bool SelectCmd::DoInitial(PClient* client) { return true; }

void SelectCmd::DoCmd(PClient* client) {
  int index = atoi(client->argv_[1].c_str());
  if (index < 0 || index >= g_config.databases) {
    client->SetRes(CmdRes::kInvalidIndex, kCmdNameSelect + " DB index is out of range");
    return;
  }
  client->SetCurrentDB(index);
  client->SetRes(CmdRes::kOK);
}

ShutdownCmd::ShutdownCmd(const std::string& name, int16_t arity)
    : BaseCmd(name, arity, kCmdFlagsAdmin | kCmdFlagsWrite, kAclCategoryAdmin | kAclCategoryWrite) {}

bool ShutdownCmd::DoInitial(PClient* client) {
  // For now, only shutdown need check local
  if (client->PeerIP().find("127.0.0.1") == std::string::npos &&
      client->PeerIP().find(g_config.ip.ToString()) == std::string::npos) {
    client->SetRes(CmdRes::kErrOther, kCmdNameShutdown + " should be localhost");
    return false;
  }
  return true;
}

void ShutdownCmd::DoCmd(PClient* client) {
  PSTORE.GetBackend(client->GetCurrentDB())->UnLockShared();
  g_pikiwidb->Stop();
  PSTORE.GetBackend(client->GetCurrentDB())->LockShared();
  client->SetRes(CmdRes::kNone);
}

PingCmd::PingCmd(const std::string& name, int16_t arity)
    : BaseCmd(name, arity, kCmdFlagsWrite, kAclCategoryWrite | kAclCategoryList) {}

bool PingCmd::DoInitial(PClient* client) { return true; }

void PingCmd::DoCmd(PClient* client) { client->SetRes(CmdRes::kPong, "PONG"); }

}  // namespace pikiwidb
