#include "page_manager.h"
#include <filesystem>
#include <iostream>
#include <cstring>

namespace toydb {

PageManager::PageManager(const std::string& db_file) : db_file_(db_file), next_page_id_(0) {
    // Open the database file for random access (read and write). Create if it doesn't exist.
    // Use std::ios::ate to seek to end initially (to determine file size), but allow random access
    db_io_.open(db_file_, std::ios::in | std::ios::out | std::ios::binary);
    if (!db_io_.is_open()) {
        // If file doesn't exist, create it
        db_io_.open(db_file_, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
        if (!db_io_.is_open()) {
            throw std::runtime_error("Failed to open database file: " + db_file_);
        }
    }

    // Get the size of the file to determine next_page_id
    db_io_.seekg(0, std::ios::end);
    uint64_t file_size = db_io_.tellg();
    db_io_.seekp(0, std::ios::end);

    if (file_size == 0) {
        // New database file, start page IDs from CATALOG_PAGE_ID
        next_page_id_ = CATALOG_PAGE_ID;
    } else {
        next_page_id_ = file_size / PAGE_SIZE;
    }
    std::cout << "PageManager: Initialized with next_page_id: " << next_page_id_ << std::endl;
}

PageManager::~PageManager() {
    FlushAllPages();
    db_io_.close();
    std::cout << "PageManager: Closed database file." << std::endl;
}

Page* PageManager::FetchPage(uint32_t page_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if page is in buffer
    if (page_buffer_.count(page_id)) {
        std::cout << "PageManager: Fetching page " << page_id << " from buffer." << std::endl;
        return page_buffer_[page_id].get();
    }

    // Page not in buffer, read from file
    if (page_id >= next_page_id_) {
        // Page does not exist on disk yet (or is beyond current file size)
        std::cerr << "PageManager: Attempted to fetch non-existent page: " << page_id << std::endl;
        return nullptr;
    }

    std::unique_ptr<Page> page = std::make_unique<Page>(page_id);
    ReadPageFromFile(page_id, page.get());
    // Store the page in the buffer, transferring ownership
    Page* raw_page_ptr = page.get();
    page_buffer_[page_id] = std::move(page);
    std::cout << "PageManager: Fetched page " << page_id << " from disk." << std::endl;
    return raw_page_ptr;
}

void PageManager::FlushPage(uint32_t page_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!page_buffer_.count(page_id)) {
        std::cerr << "PageManager: Attempted to flush non-buffered page: " << page_id << std::endl;
        return;
    }

    Page* page = page_buffer_[page_id].get();
    if (!page->is_dirty) {
        return;
    }
    std::cout << "PageManager: Flushing page " << page->page_id << " to disk." << std::endl;
    WritePageToFile(page);
    page->is_dirty = false;
}

void PageManager::AddPageToBuffer(std::unique_ptr<Page> page) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t page_id = page->page_id;
    if (page_buffer_.count(page_id)) {
        std::cerr << "PageManager: Page " << page_id << " already in buffer, overwriting." << std::endl;
    }
    page_buffer_[page_id] = std::move(page);
}

uint32_t PageManager::NewPage() {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t new_page_id = next_page_id_++;
    std::unique_ptr<Page> new_page = std::make_unique<Page>(new_page_id);
    new_page->is_dirty = true; // New pages are always dirty
    // Add to buffer directly (already holding lock, so don't call AddPageToBuffer which also locks)
    if (page_buffer_.count(new_page_id)) {
        std::cerr << "PageManager: Page " << new_page_id << " already in buffer, overwriting." << std::endl;
    }
    page_buffer_[new_page_id] = std::move(new_page);
    std::cout << "PageManager: Allocated new page with ID: " << new_page_id << std::endl;
    return new_page_id;
}

void PageManager::FlushAllPages() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::cout << "PageManager: Flushing all dirty pages." << std::endl;
    for (auto const& [page_id, page_ptr] : page_buffer_) {
        if (page_ptr->is_dirty) {
            WritePageToFile(page_ptr.get());
            page_ptr->is_dirty = false;
        }
    }
}

void PageManager::ReadPageFromFile(uint32_t page_id, Page* page) {
    // 确保page数据已清零（Page构造函数会清零，但为了安全再次确认）
    std::memset(page->data, 0, PAGE_SIZE);
    
    db_io_.seekg(page_id * PAGE_SIZE);
    if (db_io_.fail()) {
        throw std::runtime_error("Failed to seek to page " + std::to_string(page_id) + " in file.");
    }
    
    db_io_.read(page->data, PAGE_SIZE);
    if (db_io_.fail() && !db_io_.eof()) {
        throw std::runtime_error("Failed to read page " + std::to_string(page_id) + " from file.");
    }
    
    // 如果读取的字节数少于PAGE_SIZE（文件末尾），剩余部分保持为0
    // 这是正常的，因为Page构造函数已经将data初始化为0
}

void PageManager::WritePageToFile(Page* page) {
    db_io_.seekp(page->page_id * PAGE_SIZE);
    db_io_.write(page->data, PAGE_SIZE);
    db_io_.flush(); // Ensure data is written to disk immediately
    if (db_io_.fail()) {
        throw std::runtime_error("Failed to write page " + std::to_string(page->page_id) + " to file.");
    }
}

} // namespace toydb
