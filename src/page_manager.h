#pragma once

#include <string>
#include <fstream>
#include <unordered_map>
#include <memory>
#include <mutex>

#include "page.h"
#include "definitions.h" // Include common definitions for CATALOG_PAGE_ID

namespace toydb {

// PageManager handles reading and writing pages to/from a file.
class PageManager {
public:
    explicit PageManager(const std::string& db_file);
    ~PageManager();

    // Reads a page from disk. If not in buffer, loads it.
    Page* FetchPage(uint32_t page_id);

    // Writes a page to disk if it's dirty.
    void FlushPage(uint32_t page_id);

    // Allocates a new page and returns its ID.
    uint32_t NewPage();

    // Writes all dirty pages to disk.
    void FlushAllPages();

    // Get the next available page ID
    uint32_t GetNextPageId() const { return next_page_id_; }

private:
    std::string db_file_;
    std::fstream db_io_;
    uint32_t next_page_id_;
    
    // For simplicity, a direct map to unique_ptr for pages (in a real system, this would be a buffer pool)
    std::unordered_map<uint32_t, std::unique_ptr<Page>> page_buffer_;
    std::mutex mutex_;

    // Helper to read a page from file into a Page object
    void ReadPageFromFile(uint32_t page_id, Page* page);
    // Helper to write a Page object to file
    void WritePageToFile(Page* page);

    // Store a copy of the page in the buffer
    void AddPageToBuffer(std::unique_ptr<Page> page);

};

} // namespace toydb
