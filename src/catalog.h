#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include <cstring> // For memcpy

#include "table.h"
#include "page.h"
#include "page_manager.h"

namespace toydb {

class Catalog {
public:
    Catalog() = default;

    void AddTable(std::unique_ptr<Table> table) {
        tables_[table->GetName()] = std::move(table);
    }

    Table* GetTable(const std::string& name) {
        auto it = tables_.find(name);
        if (it != tables_.end()) {
            return it->second.get();
        }
        return nullptr;
    }

    const std::unordered_map<std::string, std::unique_ptr<Table>>& GetTables() const {
        return tables_;
    }

    // Serialization/Deserialization methods for Catalog metadata
    void Serialize(Page* page) const;
    void Deserialize(const Page* page, PageManager& page_manager);

    // Remove a table by name (used by DROP TABLE)
    void RemoveTable(const std::string& name) {
        tables_.erase(name);
    }

private:
    std::unordered_map<std::string, std::unique_ptr<Table>> tables_;
};

} // namespace toydb
