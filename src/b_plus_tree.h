#pragma once

#include <vector>
#include <string>
#include <memory>
#include <mutex>

#include "page.h"
#include "page_manager.h"
#include "definitions.h" // Include common definitions (includes VersionedRow)

namespace toydb {

// Forward declarations
class Table; // Forward declaration of Table class

template <typename KeyType, typename ValueType>
class BPlusTree;

template <typename KeyType, typename ValueType>
class BPlusTreeNode {
public:
    BPlusTreeNode(bool is_leaf, int max_size, uint32_t page_id = 0, uint32_t parent_page_id = INVALID_PAGE_ID) 
        : is_leaf_(is_leaf), max_size_(max_size), page_id_(page_id), parent_page_id_(parent_page_id) {}
    virtual ~BPlusTreeNode() = default;

    bool IsLeaf() const { return is_leaf_; }
    bool IsFull() const { return keys_.size() == max_size_; }

    // Pointers for tree traversal and splitting
    // BPlusTreeNode<KeyType, ValueType>* parent_ = nullptr; // Removed, parent relationship managed by page IDs

    std::vector<KeyType> keys_;
    bool is_leaf_;
    int max_size_;
    uint32_t page_id_; // The page ID where this node is stored
    uint32_t parent_page_id_; // The page ID of the parent node

    // Serialization/Deserialization methods
    virtual void Serialize(Page* page) const = 0;
    virtual void Deserialize(const Page* page) = 0;
};

template <typename KeyType, typename ValueType>
class BPlusTreeLeafNode : public BPlusTreeNode<KeyType, ValueType> {
public:
    BPlusTreeLeafNode(int max_size, uint32_t page_id = 0, uint32_t parent_page_id = INVALID_PAGE_ID)
        : BPlusTreeNode<KeyType, ValueType>(true, max_size, page_id, parent_page_id), 
          next_leaf_page_id_(INVALID_PAGE_ID), prev_leaf_page_id_(INVALID_PAGE_ID) {}
    std::vector<ValueType> values_;
    uint32_t next_leaf_page_id_;
    uint32_t prev_leaf_page_id_;

    void Serialize(Page* page) const override;
    void Deserialize(const Page* page) override;
};

template <typename KeyType, typename ValueType>
class BPlusTreeInternalNode : public BPlusTreeNode<KeyType, ValueType> {
public:
    BPlusTreeInternalNode(int max_size, uint32_t page_id = 0, uint32_t parent_page_id = INVALID_PAGE_ID) 
        : BPlusTreeNode<KeyType, ValueType>(false, max_size, page_id, parent_page_id) {}
    std::vector<uint32_t> children_page_ids_; // Store children as page IDs

    void Serialize(Page* page) const override;
    void Deserialize(const Page* page) override;
};

// Generic ToString helper for various ValueTypes
template <typename T>
inline std::string ToString(const T& value) {
    return std::to_string(value);
}

template <>
inline std::string ToString(const std::string& value) {
    return value;
}

template <>
inline std::string ToString(const std::vector<std::string>& value) {
    std::string formatted_row = "[";
    for (size_t i = 0; i < value.size(); ++i) {
        formatted_row += "'" + value[i] + "'";
        if (i < value.size() - 1) {
            formatted_row += ", ";
        }
    }
    formatted_row += "]";
    return formatted_row;
}

// 针对 VersionedRow 的 ToString：只打印用户可见的数据部分
template <>
inline std::string ToString(const VersionedRow& value) {
    return ToString(value.data);
}

// 针对版本链的 ToString：打印版本链中最新可见版本的数据
template <>
inline std::string ToString(const std::vector<VersionedRow>& value) {
    if (value.empty()) {
        return "[]";
    }
    // 返回版本链中最后一个版本的数据（通常是最新的）
    return ToString(value.back().data);
}

template <typename KeyType, typename ValueType>
class BPlusTree {
public:
    // Update constructor to accept PageManager reference
    explicit BPlusTree(PageManager& page_manager, uint32_t root_page_id = 0)
        : page_manager_(page_manager), root_page_id_(root_page_id), fanout_(4) {}
    ~BPlusTree(); // Destructor will be responsible for flushing root

    void Insert(const KeyType &key, const ValueType &value);
    bool Search(const KeyType &key, ValueType &value);
    void Delete(const KeyType &key);
    
    // Get all key-value pairs (for full table scan)
    std::vector<std::pair<KeyType, ValueType>> GetAllValues();

    uint32_t GetRootPageId() const { return root_page_id_; }
    void SetRootPageId(uint32_t page_id) { root_page_id_ = page_id; }

private:
    PageManager& page_manager_;
    uint32_t root_page_id_; // The page ID of the root node
    int fanout_; // Represents M for M-ary tree, also max_size for nodes

    // 简单的树级别互斥锁，保证单棵 B+ 树在多线程下的结构修改是安全的
    mutable std::mutex tree_mutex_;

    // Helper functions
    std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> GetNode(uint32_t page_id);
    void FreeNode(BPlusTreeNode<KeyType, ValueType>* node); // Will be removed, unique_ptr handles deletion
    void MarkDirty(uint32_t page_id);

    // Original B+ tree methods (will be updated to use PageManager and unique_ptr)
    std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>> FindLeaf(const KeyType &key);
    std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>> FindLeaf(const KeyType &key, uint32_t& parent_page_id);
    void SplitLeafNode(std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>> leaf_ptr, uint32_t parent_page_id);
    void SplitInternalNode(std::unique_ptr<BPlusTreeInternalNode<KeyType, ValueType>> internal_ptr);
    void InsertIntoParent(std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> node_ptr, const KeyType &key, uint32_t new_node_page_id, uint32_t parent_of_node_page_id);

    // Deletion helper functions
    bool Redistribute(std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> node_ptr, std::unique_ptr<BPlusTreeInternalNode<KeyType, ValueType>> parent_ptr, size_t node_idx);
    void Merge(std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> node_ptr, std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> sibling_ptr, std::unique_ptr<BPlusTreeInternalNode<KeyType, ValueType>> parent_ptr, size_t node_idx);
    void RemoveEntry(std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> node_ptr, const KeyType &key, uint32_t child_page_id);
    uint32_t FindParentPageId(uint32_t child_page_id);
};

} // namespace toydb
