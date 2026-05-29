#pragma once

#include <string>
#include "nodes/parsenodes.hpp"
#include "catalog.h"
#include "page_manager.h"

namespace toydb {

// Execute CREATE TABLE statement
bool ExecuteCreateTable(duckdb_libpgquery::PGCreateStmt* create_stmt,
                        Catalog& catalog,
                        PageManager& page_manager);

} // namespace toydb


