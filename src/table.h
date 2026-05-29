#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <cstring> // For memcpy

#include "page.h"
#include "page_manager.h"
#include "definitions.h" // Include common definitions
// #include "b_plus_tree.h" // Removed to break circular dependency

namespace toydb {

// Forward declaration of BPlusTree
template <typename KeyType, typename ValueType>
class BPlusTree;

enum class ColumnType {
    INT,
    VARCHAR,
    DOUBLE
};

struct Column {
    std::string name;
    ColumnType type;
    size_t length; // For VARCHAR

    Column() : name(""), type(ColumnType::INT), length(0) {} // Default constructor
    Column(std::string name, ColumnType type, size_t length = 0)
        : name(std::move(name)), type(type), length(length) {}
};

class Table {
public:
    // Declare constructor, implement in .cpp
    Table(std::string name, std::vector<Column> columns, PageManager& page_manager, uint32_t root_page_id = INVALID_PAGE_ID);
    // Declare destructor, implement in .cpp
    ~Table();

    const std::string& GetName() const { return name_; }
    const std::vector<Column>& GetColumns() const { return columns_; }
    uint32_t GetRootPageId() const { return root_page_id_; }
    // Declare SetRootPageId, implement in .cpp
    void SetRootPageId(uint32_t root_page_id);

    // Serialization/Deserialization methods for Table metadata
    void Serialize(Page* page, size_t offset) const;
    void Deserialize(const Page* page, size_t offset, PageManager& page_manager);

    // Helpers for transaction rollback (logical undo on primary-key index)
    void UndoInsert(int key); // undo an INSERT => delete the key
    void UndoDelete(int key, const std::vector<std::string>& old_row); // undo a DELETE => reinsert old row
    void UndoUpdate(int key, const std::vector<std::string>& old_row); // undo an UPDATE => restore old row

    // 主键索引：使用 B+ 树将 int 主键映射到版本链（std::vector<VersionedRow>）
    // 每个 key 对应一个版本链，支持多版本并发控制
    std::unique_ptr<toydb::BPlusTree<int, std::vector<VersionedRow>>> bptree_;

private:
    std::string name_;
    std::vector<Column> columns_;
    uint32_t root_page_id_; // Store the root page ID here
};

// Helper to print std::vector<std::string>
// inline std::string FormatRow(const std::vector<std::string>& row) {
//     std::string formatted_row = "[";
//     for (size_t i = 0; i < row.size(); ++i) {
//         formatted_row += "'" + row[i] + "'";
//         if (i < row.size() - 1) {
//             formatted_row += ", ";
//         }
//     }
//     formatted_row += "]";
//     return formatted_row;
// }

} // namespace toydb
