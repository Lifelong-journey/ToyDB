#include "b_plus_tree.h"
#include <iostream>
#include "table.h" // Include table.h for FormatRow
#include <cstring> // For std::memcpy
#include <unordered_set>

namespace toydb {

template <typename KeyType, typename ValueType>
std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>> BPlusTree<KeyType, ValueType>::FindLeaf(const KeyType &key) {
    uint32_t dummy_parent_page_id; // This will not be used in this overload
    return FindLeaf(key, dummy_parent_page_id);
}

template <typename KeyType, typename ValueType>
std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>> BPlusTree<KeyType, ValueType>::FindLeaf(const KeyType &key, uint32_t& parent_page_id) {
    if (root_page_id_ == INVALID_PAGE_ID) {
        std::cout << "FindLeaf: Root page ID is INVALID, returning nullptr" << std::endl;
        parent_page_id = INVALID_PAGE_ID; // Set parent_page_id to invalid
        return nullptr;
    }

    std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> current_node_ptr = GetNode(root_page_id_);
    if (current_node_ptr == nullptr) {
        std::cerr << "FindLeaf: Failed to get root node with page_id: " << root_page_id_ << ". Resetting root_page_id_ to INVALID." << std::endl;
        root_page_id_ = INVALID_PAGE_ID; // reset to allow recreation on next insert
        parent_page_id = INVALID_PAGE_ID; // Set parent_page_id to invalid
        return nullptr;
    }

    // If the root itself is a leaf, its parent is invalid (0)
    if (current_node_ptr->IsLeaf()) {
        parent_page_id = INVALID_PAGE_ID;
        return std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>>(static_cast<BPlusTreeLeafNode<KeyType, ValueType>*>(current_node_ptr.release()));
    }

    uint32_t current_parent_page_id = root_page_id_;

    while (!current_node_ptr->IsLeaf()) {
        BPlusTreeInternalNode<KeyType, ValueType>* internal_node = static_cast<BPlusTreeInternalNode<KeyType, ValueType>*>(current_node_ptr.get());
        size_t i = 0;
        while (i < internal_node->keys_.size() && key >= internal_node->keys_[i]) {
            i++;
        }
        if (i >= internal_node->children_page_ids_.size()) {
            std::cerr << "FindLeaf: Error - child page ID index out of bounds!" << std::endl;
            parent_page_id = INVALID_PAGE_ID;
            return nullptr;
        }
        uint32_t next_page_id = internal_node->children_page_ids_[i];
        current_parent_page_id = current_node_ptr->page_id_; // Update current parent before moving to child

        current_node_ptr = GetNode(next_page_id); // Get new unique_ptr for next node
        if (current_node_ptr == nullptr) {
            std::cerr << "FindLeaf: Failed to get child node with page_id: " << next_page_id << std::endl;
            parent_page_id = INVALID_PAGE_ID;
            return nullptr;
        }
    }
    std::cout << "FindLeaf: Reached leaf node." << std::endl;
    parent_page_id = current_parent_page_id;
    return std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>>(static_cast<BPlusTreeLeafNode<KeyType, ValueType>*>(current_node_ptr.release()));
}

template <typename KeyType, typename ValueType>
void BPlusTree<KeyType, ValueType>::Insert(const KeyType &key, const ValueType &value) {
    std::lock_guard<std::mutex> guard(tree_mutex_);
    std::cout << "Insert: Inserting key " << key << std::endl;
    // If root_page_id_ points to a non-existent page (e.g., stale catalog entry),
    // reset it so we rebuild the tree from scratch.
    if (root_page_id_ != INVALID_PAGE_ID) {
        if (page_manager_.FetchPage(root_page_id_) == nullptr) {
            std::cerr << "Insert: Root page_id " << root_page_id_ << " missing. Resetting tree." << std::endl;
            root_page_id_ = INVALID_PAGE_ID;
        }
    }

    if (root_page_id_ == INVALID_PAGE_ID) {
        std::cout << "Insert: Root page ID is INVALID, creating new leaf root." << std::endl;
        root_page_id_ = page_manager_.NewPage();
        auto new_root_node = std::make_unique<BPlusTreeLeafNode<KeyType, ValueType>>(fanout_, root_page_id_, INVALID_PAGE_ID);
        new_root_node->keys_.push_back(key);
        new_root_node->values_.push_back(value);
        Page* root_page = page_manager_.FetchPage(root_page_id_);
        new_root_node->Serialize(root_page);
        MarkDirty(root_page_id_);
        return;
    }

    uint32_t leaf_parent_page_id = INVALID_PAGE_ID;
    std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>> leaf_ptr = FindLeaf(key, leaf_parent_page_id);
    // BPlusTreeLeafNode<KeyType, ValueType>* leaf = leaf_ptr.get(); // No need to get raw pointer here

    if (leaf_ptr == nullptr) {
        std::cerr << "Insert: Error - FindLeaf returned nullptr! Aborting insert." << std::endl;
        return; // Should not happen if root is properly initialized
    }
    std::cout << "Insert: Found leaf for key " << key << ", current keys: " << leaf_ptr->keys_.size() << ", page_id: " << leaf_ptr->page_id_ << std::endl;

    // Find the insertion point in the leaf node
    size_t i = 0;
    while (i < leaf_ptr->keys_.size() && key > leaf_ptr->keys_[i]) {
        i++;
    }
    leaf_ptr->keys_.insert(leaf_ptr->keys_.begin() + i, key);
    leaf_ptr->values_.insert(leaf_ptr->values_.begin() + i, value);
    std::cout << "Insert: Key " << key << " inserted into leaf. New keys size: " << leaf_ptr->keys_.size() << ", page_id: " << leaf_ptr->page_id_ << std::endl;
    
    Page* leaf_page = page_manager_.FetchPage(leaf_ptr->page_id_);
    leaf_ptr->Serialize(leaf_page);
    MarkDirty(leaf_ptr->page_id_);

    if (leaf_ptr->IsFull()) {
        std::cout << "Insert: Leaf is full, splitting." << std::endl;
        SplitLeafNode(std::move(leaf_ptr), leaf_parent_page_id); // Pass unique_ptr and parent page ID
    }
    // leaf_ptr is now moved or goes out of scope, no delete needed here
}

template <typename KeyType, typename ValueType>
void BPlusTree<KeyType, ValueType>::SplitLeafNode(std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>> leaf_ptr, uint32_t parent_page_id) {
    BPlusTreeLeafNode<KeyType, ValueType>* leaf = leaf_ptr.get();
    std::cout << "SplitLeafNode: Splitting leaf node with page_id: " << leaf->page_id_ << ". Current keys: " << leaf->keys_.size() << ". Parent page_id: " << parent_page_id << std::endl;
    uint32_t new_leaf_page_id = page_manager_.NewPage();
    auto new_leaf_ptr = std::make_unique<BPlusTreeLeafNode<KeyType, ValueType>>(leaf->max_size_, new_leaf_page_id, parent_page_id);
    BPlusTreeLeafNode<KeyType, ValueType>* new_leaf = new_leaf_ptr.get();

    // Move half the keys and values to the new leaf
    size_t split_idx = leaf->max_size_ / 2;
    std::cout << "SplitLeafNode: Moving keys from index " << split_idx << std::endl;
    new_leaf->keys_.insert(new_leaf->keys_.begin(),
                           std::make_move_iterator(leaf->keys_.begin() + split_idx),
                           std::make_move_iterator(leaf->keys_.end()));
    new_leaf->values_.insert(new_leaf->values_.begin(),
                            std::make_move_iterator(leaf->values_.begin() + split_idx),
                            std::make_move_iterator(leaf->values_.end()));
    leaf->keys_.erase(leaf->keys_.begin() + split_idx, leaf->keys_.end());
    leaf->values_.erase(leaf->values_.begin() + split_idx, leaf->values_.end());
    std::cout << "SplitLeafNode: Old leaf keys: " << leaf->keys_.size() << ", New leaf keys: " << new_leaf->keys_.size() << std::endl;


    uint32_t old_next_leaf_page_id = leaf->next_leaf_page_id_; // Assuming next_leaf_page_id_ exists now

    new_leaf->prev_leaf_page_id_ = leaf->page_id_;
    new_leaf->next_leaf_page_id_ = old_next_leaf_page_id;

    leaf->next_leaf_page_id_ = new_leaf_page_id;

    if (old_next_leaf_page_id != INVALID_PAGE_ID) {
        std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>> old_next_leaf_ptr(
            static_cast<BPlusTreeLeafNode<KeyType, ValueType>*>(GetNode(old_next_leaf_page_id).release()));
        old_next_leaf_ptr->prev_leaf_page_id_ = new_leaf_page_id;
        Page* old_next_leaf_page = page_manager_.FetchPage(old_next_leaf_ptr->page_id_);
        old_next_leaf_ptr->Serialize(old_next_leaf_page);
        MarkDirty(old_next_leaf_page_id);
    }
    std::cout << "SplitLeafNode: Sibling page IDs updated." << std::endl;

    // Mark nodes dirty and flush
    Page* leaf_page = page_manager_.FetchPage(leaf->page_id_);
    leaf->Serialize(leaf_page);
    MarkDirty(leaf->page_id_);

    Page* new_leaf_page = page_manager_.FetchPage(new_leaf->page_id_);
    new_leaf->Serialize(new_leaf_page);
    MarkDirty(new_leaf->page_id_);

    // Promote the first key of the new leaf to the parent
    KeyType promoted_key = new_leaf->keys_[0];
    std::cout << "SplitLeafNode: Promoted key: " << promoted_key << std::endl;
    InsertIntoParent(std::move(leaf_ptr), promoted_key, new_leaf_page_id, parent_page_id); // Pass unique_ptr and parent_page_id
    std::cout << "SplitLeafNode: InsertIntoParent called." << std::endl;

    // new_leaf_ptr unique_ptr will handle deletion when it goes out of scope
}

template <typename KeyType, typename ValueType>
void BPlusTree<KeyType, ValueType>::InsertIntoParent(std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> node_ptr, const KeyType &key, uint32_t new_node_page_id, uint32_t parent_of_node_page_id) {
    BPlusTreeNode<KeyType, ValueType>* node = node_ptr.get();
    std::cout << "InsertIntoParent: Node page_id: " << node->page_id_ << ", Key: " << key << ", New node page_id: " << new_node_page_id << ", Parent of node page_id: " << parent_of_node_page_id << std::endl;
    
    if (node->page_id_ == root_page_id_) {
        std::cout << "InsertIntoParent: Node is root, creating new root." << std::endl;
        uint32_t new_root_page_id = page_manager_.NewPage();
        auto new_root = std::make_unique<BPlusTreeInternalNode<KeyType, ValueType>>(fanout_, new_root_page_id, INVALID_PAGE_ID);
        new_root->keys_.push_back(key);
        new_root->children_page_ids_.push_back(node->page_id_);
        new_root->children_page_ids_.push_back(new_node_page_id);
        
        // Update parent_page_id_ for the children
        // The node_ptr passed to this function is the left child.
        node->parent_page_id_ = new_root_page_id;
        Page* child_node_1_page = page_manager_.FetchPage(node->page_id_);
        node->Serialize(child_node_1_page);
        MarkDirty(node->page_id_);

        std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> child_node_2_ptr = GetNode(new_node_page_id);
        child_node_2_ptr->parent_page_id_ = new_root_page_id;
        Page* child_node_2_page = page_manager_.FetchPage(child_node_2_ptr->page_id_);
        child_node_2_ptr->Serialize(child_node_2_page);
        MarkDirty(child_node_2_ptr->page_id_);

        Page* new_root_page = page_manager_.FetchPage(new_root_page_id);
        new_root->Serialize(new_root_page);
        MarkDirty(new_root_page_id);
        this->root_page_id_ = new_root_page_id; // Update BPlusTree's root_page_id_ here
        std::cout << "InsertIntoParent: New root created with page_id: " << this->root_page_id_ << std::endl;
        return; // new_root unique_ptr handles deletion.
    }

    uint32_t parent_page_id = parent_of_node_page_id; // Use the passed parent_of_node_page_id
    if (parent_page_id == INVALID_PAGE_ID) {
        std::cerr << "InsertIntoParent: Parent page ID is INVALID for node: " << node->page_id_ << std::endl;
        return;
    }
    std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> parent_node_ptr = GetNode(parent_page_id);
    BPlusTreeInternalNode<KeyType, ValueType>* parent = static_cast<BPlusTreeInternalNode<KeyType, ValueType>*>(parent_node_ptr.get());
    
    std::cout << "InsertIntoParent: Parent page_id: " << parent->page_id_ << ", keys size: " << parent->keys_.size() << ", children size: " << parent->children_page_ids_.size() << std::endl;

    // Find insertion point in parent
    size_t i = 0;
    while (i < parent->keys_.size() && key > parent->keys_[i]) {
        i++;
    }
    parent->keys_.insert(parent->keys_.begin() + i, key);
    parent->children_page_ids_.insert(parent->children_page_ids_.begin() + i + 1, new_node_page_id);

    // Update parent_page_id_ for the new child
    std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> new_child_node_ptr = GetNode(new_node_page_id);
    new_child_node_ptr->parent_page_id_ = parent->page_id_;
    Page* new_child_node_page = page_manager_.FetchPage(new_child_node_ptr->page_id_);
    new_child_node_ptr->Serialize(new_child_node_page);
    MarkDirty(new_child_node_ptr->page_id_);

    Page* parent_page = page_manager_.FetchPage(parent->page_id_);
    parent->Serialize(parent_page);
    MarkDirty(parent->page_id_);

    if (parent->IsFull()) {
        std::cout << "InsertIntoParent: Parent is full, splitting internal node." << std::endl;
        std::unique_ptr<BPlusTreeInternalNode<KeyType, ValueType>> internal_parent_ptr(
            static_cast<BPlusTreeInternalNode<KeyType, ValueType>*>(parent_node_ptr.release()));
        SplitInternalNode(std::move(internal_parent_ptr));
    }
}

template <typename KeyType, typename ValueType>
void BPlusTree<KeyType, ValueType>::SplitInternalNode(std::unique_ptr<BPlusTreeInternalNode<KeyType, ValueType>> internal_ptr) {
    BPlusTreeInternalNode<KeyType, ValueType>* internal = internal_ptr.get();
    std::cout << "SplitInternalNode: Splitting internal node with page_id: " << internal->page_id_ << ". Current keys: " << internal->keys_.size() << ", children: " << internal->children_page_ids_.size() << ". Parent page_id: " << internal->parent_page_id_ << std::endl;
    uint32_t new_internal_page_id = page_manager_.NewPage();
    auto new_internal_ptr = std::make_unique<BPlusTreeInternalNode<KeyType, ValueType>>(internal->max_size_, new_internal_page_id, internal->parent_page_id_);
    BPlusTreeInternalNode<KeyType, ValueType>* new_internal = new_internal_ptr.get();

    size_t split_idx = internal->max_size_ / 2;
    KeyType promoted_key = internal->keys_[split_idx];

    std::cout << "SplitInternalNode: Promoted key: " << promoted_key << ", split_idx: " << split_idx << std::endl;
    new_internal->keys_.insert(new_internal->keys_.begin(),
                                std::make_move_iterator(internal->keys_.begin() + split_idx + 1),
                                std::make_move_iterator(internal->keys_.end()));
    internal->keys_.erase(internal->keys_.begin() + split_idx, internal->keys_.end());
    std::cout << "SplitInternalNode: Keys moved. Old internal keys: " << internal->keys_.size() << ", New internal keys: " << new_internal->keys_.size() << std::endl;

    new_internal->children_page_ids_.insert(new_internal->children_page_ids_.begin(),
                                    std::make_move_iterator(internal->children_page_ids_.begin() + split_idx + 1),
                                    std::make_move_iterator(internal->children_page_ids_.end()));
    internal->children_page_ids_.erase(internal->children_page_ids_.begin() + split_idx + 1, internal->children_page_ids_.end());
    std::cout << "SplitInternalNode: Children page IDs moved. Old internal children: " << internal->children_page_ids_.size() << ", New internal children: " << new_internal->children_page_ids_.size() << std::endl;

    // Update parent_page_id_ for children of new_internal
    for (uint32_t child_page_id : new_internal->children_page_ids_) {
        std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> child_node_ptr = GetNode(child_page_id);
        child_node_ptr->parent_page_id_ = new_internal_page_id;
        Page* child_node_page = page_manager_.FetchPage(child_node_ptr->page_id_);
        child_node_ptr->Serialize(child_node_page);
        MarkDirty(child_node_ptr->page_id_);
    }

    Page* internal_page = page_manager_.FetchPage(internal->page_id_);
    internal->Serialize(internal_page);
    MarkDirty(internal->page_id_);

    Page* new_internal_page = page_manager_.FetchPage(new_internal->page_id_);
    new_internal->Serialize(new_internal_page);
    MarkDirty(new_internal->page_id_);

    InsertIntoParent(std::move(internal_ptr), promoted_key, new_internal_page_id, internal->parent_page_id_); // Pass unique_ptr and parent_page_id
    std::cout << "SplitInternalNode: InsertIntoParent called." << std::endl;
}

template <typename KeyType, typename ValueType>
bool BPlusTree<KeyType, ValueType>::Search(const KeyType &key, ValueType &value) {
    std::lock_guard<std::mutex> guard(tree_mutex_);
    std::cout << "Search: Searching for key: " << key << std::endl;
    uint32_t dummy_parent_page_id;
    std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>> leaf_ptr = FindLeaf(key, dummy_parent_page_id);
    // BPlusTreeLeafNode<KeyType, ValueType>* leaf = leaf_ptr.get(); // No need to get raw pointer here

    if (leaf_ptr == nullptr) {
        std::cout << "Search: Leaf not found for key: " << key << std::endl;
        return false;
    }

    size_t i = 0;
    while (i < leaf_ptr->keys_.size() && key > leaf_ptr->keys_[i]) {
        i++;
    }

    if (i < leaf_ptr->keys_.size() && key == leaf_ptr->keys_[i]) {
        value = leaf_ptr->values_[i];
        std::cout << "Search: Found key " << key << ", value: " << ToString(value) << std::endl;
        return true;
    }
    std::cout << "Search: Key " << key << " not found in leaf." << std::endl;
    return false;
}

template <typename KeyType, typename ValueType>
std::vector<std::pair<KeyType, ValueType>> BPlusTree<KeyType, ValueType>::GetAllValues() {
    std::lock_guard<std::mutex> guard(tree_mutex_);
    std::vector<std::pair<KeyType, ValueType>> result;
    
    if (root_page_id_ == INVALID_PAGE_ID) {
        return result; // Empty tree
    }
    
    // Find the leftmost leaf node
    std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> current_node_ptr = GetNode(root_page_id_);
    if (current_node_ptr == nullptr) {
        return result;
    }
    
    // Traverse to the leftmost leaf
    while (!current_node_ptr->IsLeaf()) {
        BPlusTreeInternalNode<KeyType, ValueType>* internal_node = 
            static_cast<BPlusTreeInternalNode<KeyType, ValueType>*>(current_node_ptr.get());
        if (internal_node->children_page_ids_.empty()) {
            return result;
        }
        uint32_t leftmost_child_id = internal_node->children_page_ids_[0];
        current_node_ptr = GetNode(leftmost_child_id);
        if (current_node_ptr == nullptr) {
            return result;
        }
    }
    
    // Now current_node_ptr is a leaf node, traverse all leaf nodes
    uint32_t current_leaf_page_id = current_node_ptr->page_id_;
    std::unordered_set<uint32_t> visited_pages; // 防止循环引用
    
    while (current_leaf_page_id != INVALID_PAGE_ID) {
        // 检查是否已经访问过，防止循环
        if (visited_pages.count(current_leaf_page_id) > 0) {
            std::cerr << "GetAllValues: Detected cycle in leaf chain at page_id: " << current_leaf_page_id << std::endl;
            break;
        }
        visited_pages.insert(current_leaf_page_id);
        
        std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> node_ptr = GetNode(current_leaf_page_id);
        if (node_ptr == nullptr || !node_ptr->IsLeaf()) {
            break;
        }
        
        BPlusTreeLeafNode<KeyType, ValueType>* leaf = 
            static_cast<BPlusTreeLeafNode<KeyType, ValueType>*>(node_ptr.get());
        
        // Add all key-value pairs from this leaf
        for (size_t i = 0; i < leaf->keys_.size(); ++i) {
            result.push_back({leaf->keys_[i], leaf->values_[i]});
        }
        
        // Move to next leaf
        uint32_t next_leaf_page_id = leaf->next_leaf_page_id_;
        // 验证 next_leaf_page_id 是否有效（不能是已访问的页面，也不能超出范围）
        if (next_leaf_page_id != INVALID_PAGE_ID && 
            next_leaf_page_id < page_manager_.GetNextPageId() &&
            visited_pages.count(next_leaf_page_id) == 0) {
            current_leaf_page_id = next_leaf_page_id;
        } else {
            break; // 无效的 next_leaf_page_id，停止遍历
        }
    }
    
    return result;
}

template <typename KeyType, typename ValueType>
void BPlusTree<KeyType, ValueType>::Delete(const KeyType &key) {
    std::lock_guard<std::mutex> guard(tree_mutex_);
    std::cout << "Delete: Deleting key: " << key << std::endl;
    if (root_page_id_ == INVALID_PAGE_ID) {
        std::cout << "Delete: Tree is empty." << std::endl;
        return;
    }

    uint32_t leaf_parent_page_id;
    std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>> leaf_ptr = FindLeaf(key, leaf_parent_page_id);
    BPlusTreeLeafNode<KeyType, ValueType>* leaf = leaf_ptr.get();

    if (leaf == nullptr) {
        std::cout << "Delete: Key " << key << " not found." << std::endl;
        return;
    }

    // Find the key in the leaf node
    size_t key_idx = 0;
    while (key_idx < leaf->keys_.size() && key > leaf->keys_[key_idx]) {
        key_idx++;
    }

    if (key_idx == leaf->keys_.size() || key != leaf->keys_[key_idx]) {
        std::cout << "Delete: Key " << key << " not found in leaf." << std::endl;
        return;
    }

    // Remove the entry from the leaf node
    leaf->keys_.erase(leaf->keys_.begin() + key_idx);
    leaf->values_.erase(leaf->values_.begin() + key_idx);
    std::cout << "Delete: Key " << key << " removed from leaf. New size: " << leaf->keys_.size() << ", page_id: " << leaf->page_id_ << std::endl;
    Page* leaf_page = page_manager_.FetchPage(leaf->page_id_);
    leaf->Serialize(leaf_page);
    MarkDirty(leaf->page_id_);

    // Handle underflow
    if (leaf->keys_.size() < (leaf->max_size_ / 2)) { // Check for underflow
        std::cout << "Delete: Leaf underflow detected for key " << key << ", page_id: " << leaf->page_id_ << std::endl;
        if (leaf->page_id_ == root_page_id_) { // If it's the root leaf
            if (leaf->keys_.empty()) {
                root_page_id_ = INVALID_PAGE_ID; // Tree becomes empty
                std::cout << "Delete: Root leaf empty, tree is now empty." << std::endl;
            }
            return;
        }

        // Find parent and sibling page IDs
        uint32_t parent_page_id = FindParentPageId(leaf->page_id_);
        if (parent_page_id == INVALID_PAGE_ID) {
            std::cerr << "Delete: Could not find parent for leaf node " << leaf->page_id_ << std::endl;
            return;
        }
        std::unique_ptr<BPlusTreeInternalNode<KeyType, ValueType>> parent_node_ptr(
            static_cast<BPlusTreeInternalNode<KeyType, ValueType>*>(GetNode(parent_page_id).release()));
        BPlusTreeInternalNode<KeyType, ValueType>* parent = parent_node_ptr.get();

        size_t leaf_idx = 0;
        while (leaf_idx < parent->children_page_ids_.size() && parent->children_page_ids_[leaf_idx] != leaf->page_id_) {
            leaf_idx++;
        }

        if (!Redistribute(std::move(leaf_ptr), std::move(parent_node_ptr), leaf_idx)) {
            // Merge will take ownership of both node_ptr and sibling_ptr
            // Need to determine sibling to pass it as unique_ptr
            uint32_t sibling_page_id = INVALID_PAGE_ID;
            if (leaf_idx > 0) {
                sibling_page_id = parent->children_page_ids_[leaf_idx - 1];
            } else if (leaf_idx < parent->children_page_ids_.size() - 1) {
                sibling_page_id = parent->children_page_ids_[leaf_idx + 1];
            }

            if (sibling_page_id == INVALID_PAGE_ID) {
                std::cerr << "Delete: Could not find sibling for leaf node " << leaf->page_id_ << std::endl;
                return;
            }
            std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> sibling_node_ptr = GetNode(sibling_page_id);
        if (!sibling_node_ptr) {
            std::cerr << "Delete: Failed to fetch sibling page_id " << sibling_page_id << " for leaf node " << leaf->page_id_ << std::endl;
            return;
        }
            Merge(std::move(leaf_ptr), std::move(sibling_node_ptr), std::move(parent_node_ptr), leaf_idx);
        }
    }
}

template <typename KeyType, typename ValueType>
void BPlusTree<KeyType, ValueType>::RemoveEntry(std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> node_ptr, const KeyType &key, uint32_t child_page_id) {
    BPlusTreeNode<KeyType, ValueType>* node = node_ptr.get();
    std::cout << "RemoveEntry: Removing key " << key << " from node page_id: " << node->page_id_ << std::endl;
    size_t key_idx = 0;
    while (key_idx < node->keys_.size() && key > node->keys_[key_idx]) {
        key_idx++;
    }

    if (key_idx < node->keys_.size() && key == node->keys_[key_idx]) {
        node->keys_.erase(node->keys_.begin() + key_idx);
    } else {
        std::cerr << "RemoveEntry: Key to remove not found in node page_id: " << node->page_id_ << std::endl;
        // This case should ideally not happen if logic is correct
    }

    if (child_page_id != INVALID_PAGE_ID) {
        size_t child_idx = 0;
        BPlusTreeInternalNode<KeyType, ValueType>* internal_node = static_cast<BPlusTreeInternalNode<KeyType, ValueType>*>(node);
        while (child_idx < internal_node->children_page_ids_.size() && internal_node->children_page_ids_[child_idx] != child_page_id) {
            child_idx++;
        }
        if (child_idx < internal_node->children_page_ids_.size()) {
            internal_node->children_page_ids_.erase(internal_node->children_page_ids_.begin() + child_idx);
        } else {
            std::cerr << "RemoveEntry: Child node page_id " << child_page_id << " to remove not found in node page_id: " << node->page_id_ << std::endl;
        }
    }
    Page* node_page = page_manager_.FetchPage(node->page_id_);
    node->Serialize(node_page);
    MarkDirty(node->page_id_);

    // Handle underflow for internal nodes
    if (!node->IsLeaf() && node->keys_.size() < (node->max_size_ / 2) && node->page_id_ != root_page_id_) {
        std::cout << "RemoveEntry: Internal node underflow detected for page_id: " << node->page_id_ << std::endl;
        uint32_t parent_page_id = FindParentPageId(node->page_id_);
        if (parent_page_id == INVALID_PAGE_ID) {
            std::cerr << "RemoveEntry: Could not find parent for internal node " << node->page_id_ << std::endl;
            return;
        }
        std::unique_ptr<BPlusTreeInternalNode<KeyType, ValueType>> parent_node_ptr(
            static_cast<BPlusTreeInternalNode<KeyType, ValueType>*>(GetNode(parent_page_id).release()));
        BPlusTreeInternalNode<KeyType, ValueType>* parent = parent_node_ptr.get();

        size_t node_idx = 0;
        while (node_idx < parent->children_page_ids_.size() && parent->children_page_ids_[node_idx] != node->page_id_) {
            node_idx++;
        }
        if (!Redistribute(std::move(node_ptr), std::move(parent_node_ptr), node_idx)) {
            // Merge will take ownership of both node_ptr and sibling_ptr
            uint32_t sibling_page_id = INVALID_PAGE_ID;
            if (node_idx > 0) {
                sibling_page_id = parent->children_page_ids_[node_idx - 1];
            } else if (node_idx < parent->children_page_ids_.size() - 1) {
                sibling_page_id = parent->children_page_ids_[node_idx + 1];
            }

            if (sibling_page_id == INVALID_PAGE_ID) {
                std::cerr << "RemoveEntry: Could not find sibling for internal node " << node->page_id_ << std::endl;
                return;
            }
            std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> sibling_node_ptr = GetNode(sibling_page_id);
            if (!sibling_node_ptr) {
                std::cerr << "RemoveEntry: Failed to fetch sibling page_id " << sibling_page_id << " for internal node " << node->page_id_ << std::endl;
                return;
            }
            Merge(std::move(node_ptr), std::move(sibling_node_ptr), std::move(parent_node_ptr), node_idx);
        }
    } else if (node->page_id_ == root_page_id_ && !node->IsLeaf() && node->keys_.empty() && !static_cast<BPlusTreeInternalNode<KeyType, ValueType>*>(node)->children_page_ids_.empty()) {
        // If root is internal and empty, and has one child, make child new root
        root_page_id_ = static_cast<BPlusTreeInternalNode<KeyType, ValueType>*>(node)->children_page_ids_[0];
        std::cout << "RemoveEntry: New root set from child with page_id: " << root_page_id_ << std::endl;
    } else if (node->page_id_ == root_page_id_ && node->keys_.empty() && node->IsLeaf()){
        root_page_id_ = INVALID_PAGE_ID; // Tree becomes empty
        std::cout << "RemoveEntry: Root is empty leaf, tree is now empty." << std::endl;
    }
}

template <typename KeyType, typename ValueType>
uint32_t BPlusTree<KeyType, ValueType>::FindParentPageId(uint32_t child_page_id) {
    if (root_page_id_ == INVALID_PAGE_ID || child_page_id == root_page_id_) {
        return INVALID_PAGE_ID;
    }

    // Now that parent_page_id is stored in the node, we can retrieve it directly.
    // We need to fetch the child node to get its parent_page_id_.
    std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> child_node_ptr = GetNode(child_page_id);
    if (child_node_ptr == nullptr) {
        return INVALID_PAGE_ID;
    }
    return child_node_ptr->parent_page_id_;
}

template <typename KeyType, typename ValueType>
bool BPlusTree<KeyType, ValueType>::Redistribute(std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> node_ptr, std::unique_ptr<BPlusTreeInternalNode<KeyType, ValueType>> parent_ptr, size_t node_idx) {
    BPlusTreeNode<KeyType, ValueType>* node = node_ptr.get();
    BPlusTreeInternalNode<KeyType, ValueType>* parent = parent_ptr.get();
    if (!node || !parent) {
        return false;
    }
    if (node_idx >= parent->children_page_ids_.size()) {
        return false;
    }
    std::cout << "Redistribute: Redistributing node with page_id: " << node->page_id_ << " at index " << node_idx << ". Parent page_id: " << parent->page_id_ << std::endl;

    size_t sibling_idx_left = node_idx - 1;
    size_t sibling_idx_right = node_idx + 1;

    // Try to borrow from left sibling
    if (node_idx > 0) {
        uint32_t left_page_id = parent->children_page_ids_[sibling_idx_left];
        if (left_page_id != INVALID_PAGE_ID) {
            std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> left_sibling_ptr = GetNode(left_page_id);
            BPlusTreeNode<KeyType, ValueType>* left_sibling = left_sibling_ptr.get();
            if (left_sibling != nullptr && left_sibling->keys_.size() > (left_sibling->max_size_ / 2)) {
            std::cout << "Redistribute: Borrowing from left sibling with page_id: " << left_sibling->page_id_ << std::endl;
            if (node->IsLeaf()) {
                auto current_leaf = static_cast<BPlusTreeLeafNode<KeyType, ValueType>*>(node);
                auto left_leaf = static_cast<BPlusTreeLeafNode<KeyType, ValueType>*>(left_sibling);

                current_leaf->keys_.insert(current_leaf->keys_.begin(), left_leaf->keys_.back());
                current_leaf->values_.insert(current_leaf->values_.begin(), left_leaf->values_.back());
                left_leaf->keys_.pop_back();
                left_leaf->values_.pop_back();

                parent->keys_[sibling_idx_left] = current_leaf->keys_[0];
            } else { // Internal node
                auto current_internal = static_cast<BPlusTreeInternalNode<KeyType, ValueType>*>(node);
                auto left_internal = static_cast<BPlusTreeInternalNode<KeyType, ValueType>*>(left_sibling);

                current_internal->keys_.insert(current_internal->keys_.begin(), parent->keys_[sibling_idx_left]);
                parent->keys_[sibling_idx_left] = left_internal->keys_.back();
                left_internal->keys_.pop_back();

                current_internal->children_page_ids_.insert(current_internal->children_page_ids_.begin(), left_internal->children_page_ids_.back());
                left_internal->children_page_ids_.pop_back();
                // Update parent_page_id for the moved child
                std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> moved_child_ptr = GetNode(current_internal->children_page_ids_[0]);
                if (!moved_child_ptr) {
                    std::cout << "Redistribute: moved_child null (left sibling). Abort redistribute." << std::endl;
                    return false;
                }
                moved_child_ptr->parent_page_id_ = current_internal->page_id_;
                if (Page* moved_child_page = page_manager_.FetchPage(moved_child_ptr->page_id_)) {
                    moved_child_ptr->Serialize(moved_child_page);
                    MarkDirty(moved_child_ptr->page_id_);
                } else {
                    std::cout << "Redistribute: failed to fetch moved child page (left sibling)." << std::endl;
                    return false;
                }
            }
            Page* node_page = page_manager_.FetchPage(node->page_id_);
            node->Serialize(node_page);
            MarkDirty(node->page_id_);

            Page* left_sibling_page = page_manager_.FetchPage(left_sibling->page_id_);
            left_sibling->Serialize(left_sibling_page);
            MarkDirty(left_sibling->page_id_);

            Page* parent_page = page_manager_.FetchPage(parent->page_id_);
            parent->Serialize(parent_page);
            MarkDirty(parent->page_id_);
            return true;
        }
        }
    }

    // Try to borrow from right sibling
    if (node_idx < parent->children_page_ids_.size() - 1) {
        uint32_t right_page_id = parent->children_page_ids_[sibling_idx_right];
        if (right_page_id != INVALID_PAGE_ID) {
            std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> right_sibling_ptr = GetNode(right_page_id);
            BPlusTreeNode<KeyType, ValueType>* right_sibling = right_sibling_ptr.get();
            if (right_sibling != nullptr && right_sibling->keys_.size() > (right_sibling->max_size_ / 2)) {
            std::cout << "Redistribute: Borrowing from right sibling with page_id: " << right_sibling->page_id_ << "\n";
            if (node->IsLeaf()) {
                auto current_leaf = static_cast<BPlusTreeLeafNode<KeyType, ValueType>*>(node);
                auto right_leaf = static_cast<BPlusTreeLeafNode<KeyType, ValueType>*>(right_sibling);

                current_leaf->keys_.push_back(right_leaf->keys_[0]);
                current_leaf->values_.push_back(right_leaf->values_[0]);
                right_leaf->keys_.erase(right_leaf->keys_.begin());
                right_leaf->values_.erase(right_leaf->values_.begin());

                parent->keys_[node_idx] = right_leaf->keys_[0];
            } else { // Internal node
                auto current_internal = static_cast<BPlusTreeInternalNode<KeyType, ValueType>*>(node);
                auto right_internal = static_cast<BPlusTreeInternalNode<KeyType, ValueType>*>(right_sibling);

                current_internal->keys_.push_back(parent->keys_[node_idx]);
                parent->keys_[node_idx] = right_internal->keys_[0];
                right_internal->keys_.erase(right_internal->keys_.begin());

                current_internal->children_page_ids_.push_back(right_internal->children_page_ids_[0]);
                right_internal->children_page_ids_.erase(right_internal->children_page_ids_.begin());
                // Update parent_page_id for the moved child
                std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> moved_child_ptr = GetNode(current_internal->children_page_ids_.back());
                if (!moved_child_ptr) {
                    std::cout << "Redistribute: moved_child null (right sibling). Abort redistribute." << std::endl;
                    return false;
                }
                moved_child_ptr->parent_page_id_ = current_internal->page_id_;
                if (Page* moved_child_page = page_manager_.FetchPage(moved_child_ptr->page_id_)) {
                    moved_child_ptr->Serialize(moved_child_page);
                    MarkDirty(moved_child_ptr->page_id_);
                } else {
                    std::cout << "Redistribute: failed to fetch moved child page (right sibling)." << std::endl;
                    return false;
                }
            }
            Page* node_page = page_manager_.FetchPage(node->page_id_);
            node->Serialize(node_page);
            MarkDirty(node->page_id_);

            Page* right_sibling_page = page_manager_.FetchPage(right_sibling->page_id_);
            right_sibling->Serialize(right_sibling_page);
            MarkDirty(right_sibling->page_id_);

            Page* parent_page = page_manager_.FetchPage(parent->page_id_);
            parent->Serialize(parent_page);
            MarkDirty(parent->page_id_);
            return true;
        }
        }
    }
    return false;
}

template <typename KeyType, typename ValueType>
void BPlusTree<KeyType, ValueType>::Merge(std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> node_ptr, std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> sibling_ptr, std::unique_ptr<BPlusTreeInternalNode<KeyType, ValueType>> parent_ptr, size_t node_idx) {
    BPlusTreeNode<KeyType, ValueType>* node = node_ptr.get();
    BPlusTreeNode<KeyType, ValueType>* sibling = sibling_ptr.get();
    BPlusTreeInternalNode<KeyType, ValueType>* parent = parent_ptr.get();
    std::cout << "Merge: Merging node with page_id: " << node->page_id_ << ". Sibling page_id: " << sibling->page_id_ << ". Parent page_id: " << parent->page_id_ << ", node_idx: " << node_idx << std::endl;

    // Determine sibling and merge direction
    // The node_ptr and sibling_ptr are already passed correctly with ownership.
    std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> left_node_uptr;
    std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> right_node_uptr;
    BPlusTreeNode<KeyType, ValueType>* left_node = nullptr;
    BPlusTreeNode<KeyType, ValueType>* right_node = nullptr;
    size_t parent_key_idx;

    if (node_idx > 0) { // node is right sibling, sibling_ptr is left sibling
        left_node_uptr = std::move(sibling_ptr);
        left_node = left_node_uptr.get();
        right_node_uptr = std::move(node_ptr);
        right_node = right_node_uptr.get();
        parent_key_idx = node_idx - 1;
    } else { // node is left sibling, sibling_ptr is right sibling
        left_node_uptr = std::move(node_ptr);
        left_node = left_node_uptr.get();
        right_node_uptr = std::move(sibling_ptr);
        right_node = right_node_uptr.get();
        parent_key_idx = node_idx;
    }

    if (left_node == nullptr || right_node == nullptr) {
        std::cerr << "Merge: Error - couldn't find sibling nodes." << std::endl;
        return;
    }

    std::cout << "Merge: Merging " << (left_node->IsLeaf() ? "leaf" : "internal") << " nodes. Left page_id: " << left_node->page_id_ << ", Right page_id: " << right_node->page_id_ << std::endl;

    if (left_node->IsLeaf()) {
        auto left_leaf = static_cast<BPlusTreeLeafNode<KeyType, ValueType>*>(left_node);
        auto right_leaf = static_cast<BPlusTreeLeafNode<KeyType, ValueType>*>(right_node);

        left_leaf->keys_.insert(left_leaf->keys_.end(),
                                std::make_move_iterator(right_leaf->keys_.begin()),
                                std::make_move_iterator(right_leaf->keys_.end()));
        left_leaf->values_.insert(left_leaf->values_.end(),
                                 std::make_move_iterator(right_leaf->values_.begin()),
                                 std::make_move_iterator(right_leaf->values_.end()));

        // Update sibling pointers for the merged leaf chain
        left_leaf->next_leaf_page_id_ = right_leaf->next_leaf_page_id_;
        if (right_leaf->next_leaf_page_id_ != INVALID_PAGE_ID) {
            std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>> next_next_leaf_ptr(
                static_cast<BPlusTreeLeafNode<KeyType, ValueType>*>(GetNode(right_leaf->next_leaf_page_id_).release()));
            if (next_next_leaf_ptr) {
                next_next_leaf_ptr->prev_leaf_page_id_ = left_leaf->page_id_;
                Page* next_next_leaf_page = page_manager_.FetchPage(next_next_leaf_ptr->page_id_);
                if (next_next_leaf_page) {
                    next_next_leaf_ptr->Serialize(next_next_leaf_page);
                    MarkDirty(next_next_leaf_ptr->page_id_);
                } else {
                    std::cerr << "Merge: Failed to fetch next leaf page_id " << next_next_leaf_ptr->page_id_ << ", reset next pointer." << std::endl;
                    left_leaf->next_leaf_page_id_ = INVALID_PAGE_ID;
                }
            } else {
                std::cerr << "Merge: next_next_leaf_ptr null for page_id " << right_leaf->next_leaf_page_id_ << ", reset next pointer." << std::endl;
                left_leaf->next_leaf_page_id_ = INVALID_PAGE_ID;
            }
        }
    } else { // Internal nodes
        auto left_internal = static_cast<BPlusTreeInternalNode<KeyType, ValueType>*>(left_node);
        auto right_internal = static_cast<BPlusTreeInternalNode<KeyType, ValueType>*>(right_node);

        left_internal->keys_.push_back(parent->keys_[parent_key_idx]); // Move key from parent
        left_internal->keys_.insert(left_internal->keys_.end(),
                                     std::make_move_iterator(right_internal->keys_.begin()),
                                     std::make_move_iterator(right_internal->keys_.end()));

        left_internal->children_page_ids_.insert(left_internal->children_page_ids_.end(),
                                         std::make_move_iterator(right_internal->children_page_ids_.begin()),
                                         std::make_move_iterator(right_internal->children_page_ids_.end()));

        // Update parent_page_id_ for children moved from right_internal to left_internal
        for (uint32_t child_page_id : right_internal->children_page_ids_) {
            std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> child_node_ptr = GetNode(child_page_id);
            child_node_ptr->parent_page_id_ = left_internal->page_id_;
            Page* child_node_page = page_manager_.FetchPage(child_node_ptr->page_id_);
            child_node_ptr->Serialize(child_node_page);
            MarkDirty(child_node_ptr->page_id_);
        }
    }

    Page* left_node_page = page_manager_.FetchPage(left_node->page_id_);
    left_node->Serialize(left_node_page);
    MarkDirty(left_node->page_id_);

    // Remove the merged node and the key from the parent
    RemoveEntry(std::move(parent_ptr), parent->keys_[parent_key_idx], right_node->page_id_);
    // right_node_uptr will be deleted when it goes out of scope after RemoveEntry if it's the one being removed
}

// Serialization functions
template <typename KeyType, typename ValueType>
void BPlusTreeLeafNode<KeyType, ValueType>::Serialize(Page* page) const {
    char* data = page->data; // Access data directly
    size_t offset = 0;

    // Write metadata
    *reinterpret_cast<bool*>(data + offset) = this->is_leaf_; offset += sizeof(bool);
    *reinterpret_cast<int*>(data + offset) = this->max_size_; offset += sizeof(int);
    *reinterpret_cast<uint32_t*>(data + offset) = this->page_id_; offset += sizeof(uint32_t);
    *reinterpret_cast<uint32_t*>(data + offset) = this->parent_page_id_; offset += sizeof(uint32_t);
    // Directly serialize next_leaf_page_id_ and prev_leaf_page_id_
    *reinterpret_cast<uint32_t*>(data + offset) = this->next_leaf_page_id_; offset += sizeof(uint32_t);
    *reinterpret_cast<uint32_t*>(data + offset) = this->prev_leaf_page_id_; offset += sizeof(uint32_t);
    *reinterpret_cast<size_t*>(data + offset) = this->keys_.size(); offset += sizeof(size_t);

    // Write keys
    for (const auto& key : this->keys_) {
        // Assuming KeyType can be directly cast to char* for serialization
        // This is a simplification; for complex types, a more robust serialization is needed.
        std::memcpy(data + offset, &key, sizeof(KeyType));
        offset += sizeof(KeyType);
    }

    // Write values (for leaf nodes)
    for (const auto& value : this->values_) {
        // Assuming ValueType (std::string or std::vector<std::string>) needs special handling.
        // For std::string, write length first, then string data.
        // For std::vector<std::string>, write vector size, then for each string, write length then string data.
        if constexpr (std::is_same_v<ValueType, std::string>) {
            size_t str_len = value.length();
            std::memcpy(data + offset, &str_len, sizeof(size_t));
            offset += sizeof(size_t);
            std::memcpy(data + offset, value.c_str(), str_len);
            offset += str_len;
        } else if constexpr (std::is_same_v<ValueType, std::vector<std::string>>) {
            size_t vec_size = value.size();
            std::memcpy(data + offset, &vec_size, sizeof(size_t));
            offset += sizeof(size_t);
            for (const auto& str_val : value) {
                size_t str_len = str_val.length();
                std::memcpy(data + offset, &str_len, sizeof(size_t));
                offset += sizeof(size_t);
                std::memcpy(data + offset, str_val.c_str(), str_len);
                offset += str_len;
            }
        } else if constexpr (std::is_same_v<ValueType, VersionedRow>) {
            // Serialize VersionedRow: create_tx, delete_tx, committed, then data vector
            std::memcpy(data + offset, &value.create_tx, sizeof(TxId));
            offset += sizeof(TxId);
            std::memcpy(data + offset, &value.delete_tx, sizeof(TxId));
            offset += sizeof(TxId);
            std::memcpy(data + offset, &value.committed, sizeof(bool));
            offset += sizeof(bool);
            // Serialize data vector (same as std::vector<std::string>)
            size_t vec_size = value.data.size();
            std::memcpy(data + offset, &vec_size, sizeof(size_t));
            offset += sizeof(size_t);
            for (const auto& str_val : value.data) {
                size_t str_len = str_val.length();
                std::memcpy(data + offset, &str_len, sizeof(size_t));
                offset += sizeof(size_t);
                std::memcpy(data + offset, str_val.c_str(), str_len);
                offset += str_len;
            }
        } else if constexpr (std::is_same_v<ValueType, std::vector<VersionedRow>>) {
            // Serialize version chain: vector size, then each VersionedRow
            size_t chain_size = value.size();
            std::memcpy(data + offset, &chain_size, sizeof(size_t));
            offset += sizeof(size_t);
            for (const auto& ver : value) {
                // Serialize each VersionedRow: create_tx, delete_tx, committed, then data vector
                std::memcpy(data + offset, &ver.create_tx, sizeof(TxId));
                offset += sizeof(TxId);
                std::memcpy(data + offset, &ver.delete_tx, sizeof(TxId));
                offset += sizeof(TxId);
                std::memcpy(data + offset, &ver.committed, sizeof(bool));
                offset += sizeof(bool);
                // Serialize data vector
                size_t vec_size = ver.data.size();
                std::memcpy(data + offset, &vec_size, sizeof(size_t));
                offset += sizeof(size_t);
                for (const auto& str_val : ver.data) {
                    size_t str_len = str_val.length();
                    std::memcpy(data + offset, &str_len, sizeof(size_t));
                    offset += sizeof(size_t);
                    std::memcpy(data + offset, str_val.c_str(), str_len);
                    offset += str_len;
                }
            }
        } else {
            // Fallback for other simple ValueTypes, direct memcpy
            std::memcpy(data + offset, &value, sizeof(ValueType));
            offset += sizeof(ValueType);
        }
    }
    
    page->is_dirty = true;
}

template <typename KeyType, typename ValueType>
void BPlusTreeLeafNode<KeyType, ValueType>::Deserialize(const Page* page) {
    const char* data = page->data; // Access data directly
    size_t offset = 0;

    // Read metadata
    this->is_leaf_ = *reinterpret_cast<const bool*>(data + offset); offset += sizeof(bool);
    this->max_size_ = *reinterpret_cast<const int*>(data + offset); offset += sizeof(int);
    this->page_id_ = *reinterpret_cast<const uint32_t*>(data + offset); offset += sizeof(uint32_t);
    this->parent_page_id_ = *reinterpret_cast<const uint32_t*>(data + offset); offset += sizeof(uint32_t);
    this->next_leaf_page_id_ = *reinterpret_cast<const uint32_t*>(data + offset); offset += sizeof(uint32_t);
    this->prev_leaf_page_id_ = *reinterpret_cast<const uint32_t*>(data + offset); offset += sizeof(uint32_t);
    size_t num_keys = *reinterpret_cast<const size_t*>(data + offset); offset += sizeof(size_t);

    // Read keys
    this->keys_.resize(num_keys);
    for (size_t i = 0; i < num_keys; ++i) {
        std::memcpy(&this->keys_[i], data + offset, sizeof(KeyType));
        offset += sizeof(KeyType);
    }

    // Read values
    this->values_.resize(num_keys);
    for (size_t i = 0; i < num_keys; ++i) {
        if constexpr (std::is_same_v<ValueType, std::string>) {
            size_t str_len;
            std::memcpy(&str_len, data + offset, sizeof(size_t));
            offset += sizeof(size_t);
            this->values_[i].resize(str_len);
            std::memcpy(&this->values_[i][0], data + offset, str_len);
            offset += str_len;
        } else if constexpr (std::is_same_v<ValueType, std::vector<std::string>>) {
            size_t vec_size;
            std::memcpy(&vec_size, data + offset, sizeof(size_t));
            offset += sizeof(size_t);
            this->values_[i].resize(vec_size);
            for (size_t j = 0; j < vec_size; ++j) {
                size_t str_len;
                std::memcpy(&str_len, data + offset, sizeof(size_t));
                offset += sizeof(size_t);
                this->values_[i][j].resize(str_len);
                std::memcpy(&this->values_[i][j][0], data + offset, str_len);
                offset += str_len;
            }
        } else if constexpr (std::is_same_v<ValueType, VersionedRow>) {
            // Deserialize VersionedRow: create_tx, delete_tx, committed, then data vector
            std::memcpy(&this->values_[i].create_tx, data + offset, sizeof(TxId));
            offset += sizeof(TxId);
            std::memcpy(&this->values_[i].delete_tx, data + offset, sizeof(TxId));
            offset += sizeof(TxId);
            std::memcpy(&this->values_[i].committed, data + offset, sizeof(bool));
            offset += sizeof(bool);
            // Deserialize data vector (same as std::vector<std::string>)
            size_t vec_size;
            std::memcpy(&vec_size, data + offset, sizeof(size_t));
            offset += sizeof(size_t);
            this->values_[i].data.resize(vec_size);
            for (size_t j = 0; j < vec_size; ++j) {
                size_t str_len;
                std::memcpy(&str_len, data + offset, sizeof(size_t));
                offset += sizeof(size_t);
                this->values_[i].data[j].resize(str_len);
                std::memcpy(&this->values_[i].data[j][0], data + offset, str_len);
                offset += str_len;
            }
        } else if constexpr (std::is_same_v<ValueType, std::vector<VersionedRow>>) {
            // Deserialize version chain: vector size, then each VersionedRow
            size_t chain_size;
            std::memcpy(&chain_size, data + offset, sizeof(size_t));
            offset += sizeof(size_t);
            this->values_[i].resize(chain_size);
            for (size_t j = 0; j < chain_size; ++j) {
                // Deserialize each VersionedRow: create_tx, delete_tx, committed, then data vector
                std::memcpy(&this->values_[i][j].create_tx, data + offset, sizeof(TxId));
                offset += sizeof(TxId);
                std::memcpy(&this->values_[i][j].delete_tx, data + offset, sizeof(TxId));
                offset += sizeof(TxId);
                std::memcpy(&this->values_[i][j].committed, data + offset, sizeof(bool));
                offset += sizeof(bool);
                // Deserialize data vector
                size_t vec_size;
                std::memcpy(&vec_size, data + offset, sizeof(size_t));
                offset += sizeof(size_t);
                this->values_[i][j].data.resize(vec_size);
                for (size_t k = 0; k < vec_size; ++k) {
                    size_t str_len;
                    std::memcpy(&str_len, data + offset, sizeof(size_t));
                    offset += sizeof(size_t);
                    this->values_[i][j].data[k].resize(str_len);
                    std::memcpy(&this->values_[i][j].data[k][0], data + offset, str_len);
                    offset += str_len;
                }
            }
        } else {
            std::memcpy(&this->values_[i], data + offset, sizeof(ValueType));
            offset += sizeof(ValueType);
        }
    }
}

template <typename KeyType, typename ValueType>
void BPlusTreeInternalNode<KeyType, ValueType>::Serialize(Page* page) const {
    char* data = page->data; // Access data directly
    size_t offset = 0;

    // Write metadata
    *reinterpret_cast<bool*>(data + offset) = this->is_leaf_; offset += sizeof(bool);
    *reinterpret_cast<int*>(data + offset) = this->max_size_; offset += sizeof(int);
    *reinterpret_cast<uint32_t*>(data + offset) = this->page_id_; offset += sizeof(uint32_t);
    *reinterpret_cast<uint32_t*>(data + offset) = this->parent_page_id_; offset += sizeof(uint32_t);
    *reinterpret_cast<size_t*>(data + offset) = this->keys_.size(); offset += sizeof(size_t);
    *reinterpret_cast<size_t*>(data + offset) = this->children_page_ids_.size(); offset += sizeof(size_t);

    // Write keys
    for (const auto& key : this->keys_) {
        std::memcpy(data + offset, &key, sizeof(KeyType));
        offset += sizeof(KeyType);
    }

    // Write children page IDs
    for (const auto& child_page_id : this->children_page_ids_) {
        std::memcpy(data + offset, &child_page_id, sizeof(uint32_t));
        offset += sizeof(uint32_t);
    }
    page->is_dirty = true;
}

template <typename KeyType, typename ValueType>
void BPlusTreeInternalNode<KeyType, ValueType>::Deserialize(const Page* page) {
    const char* data = page->data; // Access data directly
    size_t offset = 0;

    // Read metadata
    this->is_leaf_ = *reinterpret_cast<const bool*>(data + offset); offset += sizeof(bool);
    this->max_size_ = *reinterpret_cast<const int*>(data + offset); offset += sizeof(int);
    this->page_id_ = *reinterpret_cast<const uint32_t*>(data + offset); offset += sizeof(uint32_t);
    this->parent_page_id_ = *reinterpret_cast<const uint32_t*>(data + offset); offset += sizeof(uint32_t);
    size_t num_keys = *reinterpret_cast<const size_t*>(data + offset); offset += sizeof(size_t);
    size_t num_children = *reinterpret_cast<const size_t*>(data + offset); offset += sizeof(size_t);

    // Read keys
    this->keys_.resize(num_keys);
    for (size_t i = 0; i < num_keys; ++i) {
        std::memcpy(&this->keys_[i], data + offset, sizeof(KeyType));
        offset += sizeof(KeyType);
    }

    // Read children page IDs
    this->children_page_ids_.resize(num_children);
    for (size_t i = 0; i < num_children; ++i) {
        std::memcpy(&this->children_page_ids_[i], data + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
    }
}

// BPlusTree implementation
template <typename KeyType, typename ValueType>
BPlusTree<KeyType, ValueType>::~BPlusTree() {
    static bool logged = false;
    if (!logged) {
        std::cout << "BPlusTree: Destructor called." << std::endl;
        logged = true;
    }
}

template <typename KeyType, typename ValueType>
std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> BPlusTree<KeyType, ValueType>::GetNode(uint32_t page_id) {
    Page* page = page_manager_.FetchPage(page_id);
    if (!page) {
        std::cerr << "GetNode: Failed to fetch page " << page_id << std::endl;
        return nullptr;
    }

    // Read the is_leaf flag to determine node type
    bool is_leaf = *reinterpret_cast<const bool*>(page->data);
    std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> node;
    if (is_leaf) {
        auto leaf_node = std::make_unique<BPlusTreeLeafNode<KeyType, ValueType>>(fanout_, page_id, INVALID_PAGE_ID);
        leaf_node->Deserialize(page);
        node = std::move(leaf_node);
    } else {
        auto internal_node = std::make_unique<BPlusTreeInternalNode<KeyType, ValueType>>(fanout_, page_id, INVALID_PAGE_ID);
        internal_node->Deserialize(page);
        node = std::move(internal_node);
    }
    return node;
}

// FreeNode is no longer needed as unique_ptr handles deletion.
// template <typename KeyType, typename ValueType>
// void BPlusTree<KeyType, ValueType>::FreeNode(BPlusTreeNode<KeyType, ValueType>* node) {
//     if (node == nullptr) {
//         return;
//     }
//     std::cout << "FreeNode: Deleting node with page_id: " << node->page_id_ << std::endl;
//     delete node; // Delete the dynamically allocated node. Ownership is now explicit with unique_ptr.
// }

template <typename KeyType, typename ValueType>
void BPlusTree<KeyType, ValueType>::MarkDirty(uint32_t page_id) {
    std::cout << "MarkDirty: Marking page " << page_id << " as dirty." << std::endl;
    Page* page = page_manager_.FetchPage(page_id);
    if (page) {
        page->is_dirty = true;
        // No need to serialize here; serialize before returning from Insert/Delete/Split.
    }
}

// Explicit template instantiation
template class BPlusTree<int, std::string>;
template class BPlusTree<int, std::vector<std::string>>;
// Explicit template instantiation for version chain
template class BPlusTree<int, std::vector<VersionedRow>>;

} // namespace toydb
