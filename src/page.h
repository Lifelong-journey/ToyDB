#pragma once

#include <vector>
#include <cstdint>

namespace toydb {

const uint32_t PAGE_SIZE = 4096; // 4KB page size

// Page represents a fixed-size block of data that can be read from or written to disk.
struct Page {
    char data[PAGE_SIZE];
    uint32_t page_id;
    bool is_dirty;

    Page(uint32_t id) : page_id(id), is_dirty(false) {
        // Initialize data to zeros
        std::fill(data, data + PAGE_SIZE, 0);
    }
};

} // namespace toydb

