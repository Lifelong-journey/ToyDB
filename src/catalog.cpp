#include "catalog.h"
#include <iostream>

namespace toydb {

void Catalog::Serialize(Page* page) const {
    // 注意：不要清零page数据，因为可能还有其他数据需要保留
    // 我们只需要确保序列化的部分是正确的即可
    // std::memset(page->data, 0, PAGE_SIZE); // 注释掉，避免覆盖已有数据
    
    char* data = page->data;
    size_t offset = 0;

    // Serialize number of tables
    size_t num_tables = tables_.size();
    std::memcpy(data + offset, &num_tables, sizeof(size_t));
    offset += sizeof(size_t);

    // Serialize each table
    for (const auto& pair : tables_) {
        // Calculate serialized size for each table
        size_t table_serialized_size = sizeof(size_t) + pair.second->GetName().length() + // Table name (length + string)
                                       sizeof(size_t) + // Number of columns
                                       sizeof(uint32_t); // Root page ID
        
        // Add size for each column
        for (const auto& col : pair.second->GetColumns()) {
            table_serialized_size += sizeof(size_t) + col.name.length() + // Column name (length + string)
                                     sizeof(ColumnType) + // Column type
                                     sizeof(size_t); // Column length
        }

        pair.second->Serialize(page, offset); // Serialize Table metadata to the same page
        offset += table_serialized_size;
    }
    page->is_dirty = true;
}

void Catalog::Deserialize(const Page* page, PageManager& page_manager) {
    const char* data = page->data;
    size_t offset = 0;

    // Deserialize number of tables
    size_t num_tables;
    std::memcpy(&num_tables, data + offset, sizeof(size_t));
    offset += sizeof(size_t);
    
    // 安全检查：如果num_tables异常大，可能是数据损坏（未初始化的page或文件损坏）
    // 检查前8个字节是否全为0（未初始化的page）
    bool is_uninitialized = true;
    for (size_t i = 0; i < 8; ++i) {
        if (data[i] != 0) {
            is_uninitialized = false;
            break;
        }
    }
    
    if (is_uninitialized) {
        // Page未初始化，没有表需要反序列化
        return;
    }
    
    if (num_tables > 1000) {
        std::cerr << "Catalog: Invalid num_tables value: " << num_tables << ", possible data corruption. Skipping deserialization." << std::endl;
        return;
    }

    // Deserialize each table
    for (size_t i = 0; i < num_tables; ++i) {
        // We need to know the size of the serialized table data to advance the offset correctly
        // The Table::Deserialize method will handle reading its own data and advancing the offset internally.
        // Here, we just need to create a dummy table object to call Deserialize on.
        // The actual table object will be constructed within Deserialize implicitly or by placement new.
        // For simplicity, let's assume tables are small enough to not span multiple pages for now

        // We need to re-read the name length and column count to correctly calculate the offset for the next table
        // This implies that Table::Deserialize should return the size it read, or we manually calculate it here.
        // Let's adjust Table::Deserialize to return the size consumed.

        // Temporarily create a table object, which will be overwritten by deserialization.
        // This is a bit awkward, but necessary if Table::Deserialize expects a pre-existing object.
        // A better approach would be static Deserialize method that returns a unique_ptr<Table>.

        // For now, let's manually deserialize the fields here, similar to what's in Table::Deserialize,
        // to get the size and then reconstruct the table.

        std::string temp_name;
        std::vector<Column> temp_columns;
        uint32_t temp_root_page_id;

        // Deserialize table name
        size_t name_len;
        std::memcpy(&name_len, data + offset, sizeof(size_t));
        offset += sizeof(size_t);
        temp_name.resize(name_len);
        std::memcpy(&temp_name[0], data + offset, name_len);
        offset += name_len;

        // Deserialize number of columns
        size_t num_columns;
        std::memcpy(&num_columns, data + offset, sizeof(size_t));
        offset += sizeof(size_t);

        // Deserialize each column
        temp_columns.resize(num_columns);
        for (size_t j = 0; j < num_columns; ++j) {
            size_t col_name_len;
            std::memcpy(&col_name_len, data + offset, sizeof(size_t));
            offset += sizeof(size_t);
            temp_columns[j].name.resize(col_name_len);
            std::memcpy(&temp_columns[j].name[0], data + offset, col_name_len);
            offset += col_name_len;

            std::memcpy(&temp_columns[j].type, data + offset, sizeof(ColumnType));
            offset += sizeof(ColumnType);

            std::memcpy(&temp_columns[j].length, data + offset, sizeof(size_t));
            offset += sizeof(size_t);
        }

        // Deserialize B+ tree root_page_id
        std::memcpy(&temp_root_page_id, data + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);

        // Add the deserialized table to the catalog
        // 注意：Table构造函数会使用temp_root_page_id创建B+树，所以这里直接传入即可
        auto table = std::make_unique<Table>(temp_name, std::move(temp_columns), page_manager, temp_root_page_id);
        tables_[temp_name] = std::move(table);
    }
}

} // namespace toydb
