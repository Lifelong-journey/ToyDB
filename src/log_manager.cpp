#include "log_manager.h"

#include <iostream>
#include "table.h"

namespace toydb {

LogManager::LogManager(const std::string& log_file) : log_file_(log_file) {
    log_stream_.open(log_file_, std::ios::out | std::ios::app);
    if (!log_stream_.is_open()) {
        throw std::runtime_error("Failed to open log file: " + log_file_);
    }
}

LogManager::~LogManager() {
    if (log_stream_.is_open()) {
        log_stream_.flush();
        log_stream_.close();
    }
}

int LogManager::BeginTransaction() {
    std::lock_guard<std::mutex> guard(mutex_);
    int tx_id = next_tx_id_++;
    active_transactions_.emplace(tx_id, TransactionContext{});
    AppendLogLine("BEGIN " + std::to_string(tx_id));
    std::cout << "Transaction " << tx_id << " started." << std::endl;
    return tx_id;
}

bool LogManager::CommitTransaction(int tx_id, PageManager& page_manager) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = active_transactions_.find(tx_id);
    if (it == active_transactions_.end()) {
        std::cerr << "LogManager: COMMIT unknown TX " << tx_id << std::endl;
        return false;
    }
    // 先刷脏页，再记录 COMMIT
    page_manager.FlushAllPages();
    AppendLogLine("COMMIT " + std::to_string(tx_id));
    std::cout << "Transaction " << tx_id << " committed." << std::endl;
    active_transactions_.erase(it);
    return true;
}

bool LogManager::RollbackTransaction(int tx_id, Catalog& catalog, PageManager& page_manager) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = active_transactions_.find(tx_id);
    if (it == active_transactions_.end()) {
        std::cerr << "LogManager: ROLLBACK unknown TX " << tx_id << std::endl;
        return false;
    }

    // 按相反顺序回放 undo
    for (auto rec_it = it->second.records.rbegin(); rec_it != it->second.records.rend(); ++rec_it) {
        ApplyUndo(*rec_it, catalog, page_manager);
    }

    AppendLogLine("ROLLBACK " + std::to_string(tx_id));
    std::cout << "Transaction " << tx_id << " rolled back." << std::endl;
    active_transactions_.erase(it);
    return true;
}

bool LogManager::IsTransactionActive(int tx_id) const {
    std::lock_guard<std::mutex> guard(mutex_);
    return active_transactions_.count(tx_id) > 0;
}

void LogManager::LogInsert(int tx_id, const std::string& table_name, int key, const std::vector<std::string>& new_row) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto* ctx = GetContextUnlocked(tx_id);
    if (ctx == nullptr) {
        return;
    }
    LogRecord rec;
    rec.tx_id = tx_id;
    rec.type = LogRecordType::INSERT;
    rec.table_name = table_name;
    rec.key = key;
    rec.new_row = new_row;
    ctx->records.push_back(rec);
    AppendLogLine("TX " + std::to_string(rec.tx_id) + " INSERT " + table_name + " key=" + std::to_string(key));
}

void LogManager::LogDelete(int tx_id, const std::string& table_name, int key, const std::vector<std::string>& old_row) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto* ctx = GetContextUnlocked(tx_id);
    if (ctx == nullptr) {
        return;
    }
    LogRecord rec;
    rec.tx_id = tx_id;
    rec.type = LogRecordType::DELETE;
    rec.table_name = table_name;
    rec.key = key;
    rec.old_row = old_row;
    ctx->records.push_back(rec);
    AppendLogLine("TX " + std::to_string(rec.tx_id) + " DELETE " + table_name + " key=" + std::to_string(key));
}

void LogManager::LogUpdate(int tx_id, const std::string& table_name, int key,
                           const std::vector<std::string>& old_row,
                           const std::vector<std::string>& new_row) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto* ctx = GetContextUnlocked(tx_id);
    if (ctx == nullptr) {
        return;
    }
    LogRecord rec;
    rec.tx_id = tx_id;
    rec.type = LogRecordType::UPDATE;
    rec.table_name = table_name;
    rec.key = key;
    rec.old_row = old_row;
    rec.new_row = new_row;
    ctx->records.push_back(rec);
    AppendLogLine("TX " + std::to_string(rec.tx_id) + " UPDATE " + table_name + " key=" + std::to_string(key));
}

void LogManager::AppendLogLine(const std::string& line) {
    if (!log_stream_.is_open()) return;
    log_stream_ << line << std::endl;
    log_stream_.flush();
}

LogManager::TransactionContext* LogManager::GetContextUnlocked(int tx_id) {
    auto it = active_transactions_.find(tx_id);
    if (it == active_transactions_.end()) {
        std::cerr << "LogManager: TX " << tx_id << " is not active, ignoring log record." << std::endl;
        return nullptr;
    }
    return &it->second;
}

void LogManager::ApplyUndo(const LogRecord& rec, Catalog& catalog, PageManager& page_manager) {
    Table* table = catalog.GetTable(rec.table_name);
    if (!table) {
        std::cerr << "LogManager: undo failed, table not found: " << rec.table_name << std::endl;
        return;
    }
    
    switch (rec.type) {
        case LogRecordType::INSERT: {
            // 撤销 INSERT => 删除该 key
            table->UndoInsert(rec.key);
            break;
        }
        case LogRecordType::DELETE: {
            // 撤销 DELETE => 重新插入旧行
            table->UndoDelete(rec.key, rec.old_row);
            break;
        }
        case LogRecordType::UPDATE: {
            // 撤销 UPDATE => 恢复旧行
            table->UndoUpdate(rec.key, rec.old_row);
            break;
        }
        default:
            break;
    }
    // 刷盘，保证撤销对页面可见
    page_manager.FlushAllPages();
}

} // namespace toydb


