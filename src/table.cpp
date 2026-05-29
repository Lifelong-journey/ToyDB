#include "table.h"
#include <algorithm>
#include <iostream>
#include "b_plus_tree.h" // Include for BPlusTree definition

namespace toydb {

// Table constructor implementation
Table::Table(std::string name, std::vector<Column> columns, PageManager& page_manager, uint32_t root_page_id)
    : name_(std::move(name)), columns_(std::move(columns)), root_page_id_(root_page_id),
      bptree_(std::make_unique<toydb::BPlusTree<int, std::vector<VersionedRow>>>(page_manager, root_page_id)) {
    // 注意：这里直接使用参数 root_page_id，而不是 root_page_id_，因为初始化列表中 root_page_id_ 可能还未完全初始化
}

// Table destructor implementation
Table::~Table() = default; // Defined in .cpp where BPlusTree is complete

// SetRootPageId implementation
void Table::SetRootPageId(uint32_t root_page_id) {
    root_page_id_ = root_page_id;
    bptree_->SetRootPageId(root_page_id);
}

void Table::Serialize(Page* page, size_t offset) const {
    char* data = page->data + offset;
    size_t current_offset = 0;

    // Serialize table name
    size_t name_len = name_.length();
    std::memcpy(data + current_offset, &name_len, sizeof(size_t));
    current_offset += sizeof(size_t);
    std::memcpy(data + current_offset, name_.c_str(), name_len);
    current_offset += name_len;

    // Serialize number of columns
    size_t num_columns = columns_.size();
    std::memcpy(data + current_offset, &num_columns, sizeof(size_t));
    current_offset += sizeof(size_t);

    // Serialize each column
    for (const auto& col : columns_) {
        size_t col_name_len = col.name.length();
        std::memcpy(data + current_offset, &col_name_len, sizeof(size_t));
        current_offset += sizeof(size_t);
        std::memcpy(data + current_offset, col.name.c_str(), col_name_len);
        current_offset += col_name_len;

        std::memcpy(data + current_offset, &col.type, sizeof(ColumnType));
        current_offset += sizeof(ColumnType);

        std::memcpy(data + current_offset, &col.length, sizeof(size_t));
        current_offset += sizeof(size_t);
    }

    // Serialize B+ tree root_page_id
    std::memcpy(data + current_offset, &root_page_id_, sizeof(uint32_t));
    current_offset += sizeof(uint32_t);
}

void Table::Deserialize(const Page* page, size_t offset, PageManager& page_manager) {
    const char* data = page->data + offset;
    size_t current_offset = 0;

    // Deserialize table name
    size_t name_len;
    std::memcpy(&name_len, data + current_offset, sizeof(size_t));
    current_offset += sizeof(size_t);
    name_.resize(name_len);
    std::memcpy(&name_[0], data + current_offset, name_len);
    current_offset += name_len;

    // Deserialize number of columns
    size_t num_columns;
    std::memcpy(&num_columns, data + current_offset, sizeof(size_t));
    current_offset += sizeof(size_t);

    // Deserialize each column
    columns_.resize(num_columns);
    for (size_t i = 0; i < num_columns; ++i) {
        size_t col_name_len;
        std::memcpy(&col_name_len, data + current_offset, sizeof(size_t));
        current_offset += sizeof(size_t);
        columns_[i].name.resize(col_name_len);
        std::memcpy(&columns_[i].name[0], data + current_offset, col_name_len);
        current_offset += col_name_len;

        std::memcpy(&columns_[i].type, data + current_offset, sizeof(ColumnType));
        current_offset += sizeof(ColumnType);

        std::memcpy(&columns_[i].length, data + current_offset, sizeof(size_t));
        current_offset += sizeof(size_t);
    }

    // Deserialize B+ tree root_page_id
    std::memcpy(&root_page_id_, data + current_offset, sizeof(uint32_t));
    current_offset += sizeof(uint32_t);

    // Initialize the B+ tree with the deserialized root_page_id
    // The bptree_ unique_ptr is already constructed in Table constructor, just update its root_page_id
    bptree_->SetRootPageId(root_page_id_); 
}

void Table::UndoInsert(int key) {
    // Undo an INSERT by deleting the key if it exists
    if (bptree_) {
        bptree_->Delete(key);
        root_page_id_ = bptree_->GetRootPageId();
    }
}

void Table::UndoDelete(int key, const std::vector<std::string>& old_row) {
    // Undo a DELETE by clearing the delete_tx mark (logical delete rollback)
    // In MVCC, DELETE marks the row with delete_tx, so rollback should clear this mark
    if (bptree_) {
        std::vector<VersionedRow> version_chain;
        if (bptree_->Search(key, version_chain)) {
            // 找到版本链中最后一个被标记删除的版本，清除其 delete_tx
            for (auto it = version_chain.rbegin(); it != version_chain.rend(); ++it) {
                if (it->delete_tx != INVALID_TX_ID) {
                    it->delete_tx = INVALID_TX_ID;
                    it->committed = true; // Restore as committed
                    break;
                }
            }
            bptree_->Delete(key);
            bptree_->Insert(key, version_chain);
        } else {
            // Row was physically deleted (shouldn't happen in MVCC, but handle for compatibility)
            std::vector<VersionedRow> new_chain;
            VersionedRow v;
            v.data = old_row;
            v.create_tx = INVALID_TX_ID;
            v.delete_tx = INVALID_TX_ID;
            v.committed = true;
            new_chain.push_back(v);
            bptree_->Insert(key, new_chain);
        }
        root_page_id_ = bptree_->GetRootPageId();
    }
}

void Table::UndoUpdate(int key, const std::vector<std::string>& old_row) {
    // Undo an UPDATE by removing the latest version from the chain
    if (bptree_) {
        std::vector<VersionedRow> version_chain;
        if (bptree_->Search(key, version_chain)) {
            // 移除版本链中最后一个版本（最新的 UPDATE）
            if (!version_chain.empty()) {
                version_chain.pop_back();
            }
        bptree_->Delete(key);
            if (!version_chain.empty()) {
                bptree_->Insert(key, version_chain);
            }
        } else {
            // 如果版本链不存在，创建一个包含旧版本的链
            std::vector<VersionedRow> new_chain;
            VersionedRow v;
            v.data = old_row;
            v.create_tx = INVALID_TX_ID;
            v.delete_tx = INVALID_TX_ID;
            v.committed = true;
            new_chain.push_back(v);
            bptree_->Insert(key, new_chain);
        }
        root_page_id_ = bptree_->GetRootPageId();
    }
}

} // namespace toydb
