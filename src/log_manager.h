#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <optional>
#include <mutex>
#include <unordered_map>

#include "catalog.h"
#include "page_manager.h"

namespace toydb {

enum class LogRecordType {
    BEGIN,
    COMMIT,
    ROLLBACK,
    INSERT,
    DELETE,
    UPDATE
};

struct LogRecord {
    int tx_id;
    LogRecordType type;
    std::string table_name;
    int key{0};
    std::vector<std::string> old_row;
    std::vector<std::string> new_row;
};

class LogManager {
public:
    explicit LogManager(const std::string& log_file);
    ~LogManager();

    // Transaction control
    int BeginTransaction();
    bool CommitTransaction(int tx_id, PageManager& page_manager);
    bool RollbackTransaction(int tx_id, Catalog& catalog, PageManager& page_manager);
    bool IsTransactionActive(int tx_id) const;

    // Logical logging API (only effective when a transaction is active)
    void LogInsert(int tx_id, const std::string& table_name, int key, const std::vector<std::string>& new_row);
    void LogDelete(int tx_id, const std::string& table_name, int key, const std::vector<std::string>& old_row);
    void LogUpdate(int tx_id, const std::string& table_name, int key,
                   const std::vector<std::string>& old_row,
                   const std::vector<std::string>& new_row);

private:
    struct TransactionContext {
        std::vector<LogRecord> records;
    };

    std::string log_file_;
    std::ofstream log_stream_;
    int next_tx_id_{1};
    mutable std::mutex mutex_;
    std::unordered_map<int, TransactionContext> active_transactions_;

    void AppendLogLine(const std::string& line);
    TransactionContext* GetContextUnlocked(int tx_id);
    void ApplyUndo(const LogRecord& rec, Catalog& catalog, PageManager& page_manager);
};

} // namespace toydb



