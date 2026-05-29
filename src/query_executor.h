#pragma once

#include <string>
#include <optional>
#include "nodes/parsenodes.hpp"
#include "definitions.h"
#include "catalog.h"
#include "page_manager.h"
#include "log_manager.h"
#include "lock_manager.h"
#include "mvcc.h"
#include "query_utils.h"

namespace toydb {

// Execute SELECT query (single table, JOIN, aggregate functions)
bool ExecuteSelect(duckdb_libpgquery::PGSelectStmt* select_stmt,
                   Catalog& catalog,
                   PageManager& page_manager,
                   LogManager& log_manager,
                   LockManager& lock_manager,
                   MVCCManager& mvcc_manager,
                   const std::optional<int>& session_tx_id,
                   const std::optional<TxId>& session_snapshot);

} // namespace toydb


