### `b_plus_tree.cpp/.h` 详细说明（ToyDB B+ 树索引实现）

本模块实现了一个基于磁盘页的通用 B+ 树模板，用于：

- 以整型主键 `int` 作为 Key；
- 以多种 Value 类型作为叶子值，本项目关键用例是：
  - `std::vector<VersionedRow>`：存储同一主键下的 MVCC 版本链。

内部设计：

- 每个节点对应一个磁盘页（由 `PageManager` 管理）；
- Node 中不再保存裸指针，而是通过 `page_id` 和 `parent_page_id` 以及 `children_page_ids_` 进行树结构导航；
- 节点通过 `Serialize/Deserialize` 与 `Page` 之间读写。

下文按类和函数详细解释。

---

## 1. 模板类与节点结构

### 1.1 `BPlusTreeNode<KeyType, ValueType>`

```cpp
template <typename KeyType, typename ValueType>
class BPlusTreeNode {
public:
    BPlusTreeNode(bool is_leaf, int max_size,
                  uint32_t page_id = 0,
                  uint32_t parent_page_id = INVALID_PAGE_ID)
        : is_leaf_(is_leaf), max_size_(max_size),
          page_id_(page_id), parent_page_id_(parent_page_id) {}
    virtual ~BPlusTreeNode() = default;

    bool IsLeaf() const { return is_leaf_; }
    bool IsFull() const { return keys_.size() == max_size_; }

    std::vector<KeyType> keys_;
    bool is_leaf_;
    int max_size_;
    uint32_t page_id_;        // 本节点所在页 ID
    uint32_t parent_page_id_; // 父节点页 ID

    virtual void Serialize(Page* page) const = 0;
    virtual void Deserialize(const Page* page) = 0;
};
```

- 这是叶子节点和内部节点的基类：
  - `is_leaf_` 区分内部节点和叶子；
  - `max_size_` 控制节点能容纳的最大 key 数，用于判断是否分裂（`IsFull()`）。
  - `page_id_` 与 `parent_page_id_` 用于在磁盘上查找节点和追踪父节点。
  - `keys_` 保存节点内的有序 key 列表。
- 序列化接口：
  - `Serialize(Page*)` 要将节点内状态写入页；
  - `Deserialize(const Page*)` 要从页中读回节点内容。

### 1.2 `BPlusTreeLeafNode<KeyType, ValueType>`

```cpp
template <typename KeyType, typename ValueType>
class BPlusTreeLeafNode : public BPlusTreeNode<KeyType, ValueType> {
public:
    BPlusTreeLeafNode(int max_size, uint32_t page_id = 0,
                      uint32_t parent_page_id = INVALID_PAGE_ID)
        : BPlusTreeNode<KeyType, ValueType>(true, max_size, page_id, parent_page_id),
          next_leaf_page_id_(INVALID_PAGE_ID), prev_leaf_page_id_(INVALID_PAGE_ID) {}

    std::vector<ValueType> values_;
    uint32_t next_leaf_page_id_;
    uint32_t prev_leaf_page_id_;

    void Serialize(Page* page) const override;
    void Deserialize(const Page* page) override;
};
```

- 叶子节点除了 `keys_`，还保存与之对应的 `values_`。
- 通过 `next_leaf_page_id_` 和 `prev_leaf_page_id_` 形成**叶层双向链表**，方便区间扫描与 GetAllValues。

### 1.3 `BPlusTreeInternalNode<KeyType, ValueType>`

```cpp
template <typename KeyType, typename ValueType>
class BPlusTreeInternalNode : public BPlusTreeNode<KeyType, ValueType> {
public:
    BPlusTreeInternalNode(int max_size, uint32_t page_id = 0,
                          uint32_t parent_page_id = INVALID_PAGE_ID)
        : BPlusTreeNode<KeyType, ValueType>(false, max_size, page_id, parent_page_id) {}

    std::vector<uint32_t> children_page_ids_;

    void Serialize(Page* page) const override;
    void Deserialize(const Page* page) override;
};
```

- 内部节点存储分割 key 和子节点页 ID：
  - `keys_[i]` 是 `children_page_ids_[i]` 与 `children_page_ids_[i+1]` 之间的分界键。

---

## 2. `BPlusTree<KeyType, ValueType>` 主类

```cpp
template <typename KeyType, typename ValueType>
class BPlusTree {
public:
    explicit BPlusTree(PageManager& page_manager, uint32_t root_page_id = 0)
        : page_manager_(page_manager), root_page_id_(root_page_id), fanout_(4) {}
    ~BPlusTree();

    void Insert(const KeyType &key, const ValueType &value);
    bool Search(const KeyType &key, ValueType &value);
    void Delete(const KeyType &key);
    std::vector<std::pair<KeyType, ValueType>> GetAllValues();

    uint32_t GetRootPageId() const { return root_page_id_; }
    void SetRootPageId(uint32_t page_id) { root_page_id_ = page_id; }

private:
    PageManager& page_manager_;
    uint32_t root_page_id_;
    int fanout_;
    mutable std::mutex tree_mutex_;

    // 辅助函数
    std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> GetNode(uint32_t page_id);
    void MarkDirty(uint32_t page_id);

    std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>> FindLeaf(const KeyType &key);
    std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>> FindLeaf(const KeyType &key,
                                                                    uint32_t& parent_page_id);
    void SplitLeafNode(std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>> leaf_ptr,
                       uint32_t parent_page_id);
    void SplitInternalNode(std::unique_ptr<BPlusTreeInternalNode<KeyType, ValueType>> internal_ptr);
    void InsertIntoParent(std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> node_ptr,
                          const KeyType &key,
                          uint32_t new_node_page_id,
                          uint32_t parent_of_node_page_id);

    // 删除相关
    bool Redistribute(std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> node_ptr,
                      std::unique_ptr<BPlusTreeInternalNode<KeyType, ValueType>> parent_ptr,
                      size_t node_idx);
    void Merge(std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> node_ptr,
               std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> sibling_ptr,
               std::unique_ptr<BPlusTreeInternalNode<KeyType, ValueType>> parent_ptr,
               size_t node_idx);
    void RemoveEntry(std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> node_ptr,
                     const KeyType &key, uint32_t child_page_id);
    uint32_t FindParentPageId(uint32_t child_page_id);
};
```

关键成员：

- `page_manager_`：负责将节点与磁盘页转换。
- `root_page_id_`：B+ 树根节点页 ID。
- `fanout_`：定义每个节点最多的 key 数（本项目用 4，便于测试和 debug）。
- `tree_mutex_`：保护整棵树的结构修改（插入/删除），防止并发破坏结构。

---

## 3. 查找叶子结点：`FindLeaf`

```cpp
template <typename KeyType, typename ValueType>
std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>>
BPlusTree<KeyType, ValueType>::FindLeaf(const KeyType &key) {
    uint32_t dummy_parent_page_id;
    return FindLeaf(key, dummy_parent_page_id);
}
```

带父节点页面 ID 输出的版本：

```cpp
template <typename KeyType, typename ValueType>
std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>>
BPlusTree<KeyType, ValueType>::FindLeaf(const KeyType &key, uint32_t& parent_page_id) {
    if (root_page_id_ == INVALID_PAGE_ID) {
        std::cout << "FindLeaf: Root page ID is INVALID, returning nullptr" << std::endl;
        parent_page_id = INVALID_PAGE_ID;
        return nullptr;
    }

    auto current_node_ptr = GetNode(root_page_id_);
    if (!current_node_ptr) { ... return nullptr; }

    if (current_node_ptr->IsLeaf()) {
        parent_page_id = INVALID_PAGE_ID;
        return std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>>(
            static_cast<BPlusTreeLeafNode<KeyType, ValueType>*>(current_node_ptr.release()));
    }

    uint32_t current_parent_page_id = root_page_id_;

    while (!current_node_ptr->IsLeaf()) {
        auto* internal_node =
          static_cast<BPlusTreeInternalNode<KeyType, ValueType>*>(current_node_ptr.get());
        size_t i = 0;
        while (i < internal_node->keys_.size() && key >= internal_node->keys_[i]) {
            i++;
        }
        if (i >= internal_node->children_page_ids_.size()) { ... 错误处理 ... }

        uint32_t next_page_id = internal_node->children_page_ids_[i];
        current_parent_page_id = current_node_ptr->page_id_;

        current_node_ptr = GetNode(next_page_id);
        if (!current_node_ptr) { ... 错误处理 ... }
    }

    parent_page_id = current_parent_page_id;
    return std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>>(
        static_cast<BPlusTreeLeafNode<KeyType, ValueType>*>(current_node_ptr.release()));
}
```

算法：

- 从根节点开始，根据 key 与 `keys_` 的大小关系向下选择合适的子结点；
- 一直到遇到叶子结点为止；
- 中间通过 `GetNode(page_id)` 每次从磁盘读取对应节点。

---

## 4. 插入：`Insert`

```cpp
template <typename KeyType, typename ValueType>
void BPlusTree<KeyType, ValueType>::Insert(const KeyType &key, const ValueType &value) {
    std::lock_guard<std::mutex> guard(tree_mutex_);
    std::cout << "Insert: Inserting key " << key << std::endl;

    if (root_page_id_ == INVALID_PAGE_ID) {
        std::cout << "Insert: Root page ID is INVALID, creating new leaf root." << std::endl;
        root_page_id_ = page_manager_.NewPage();
        auto new_root_node = std::make_unique<BPlusTreeLeafNode<KeyType, ValueType>>(
            fanout_, root_page_id_, INVALID_PAGE_ID);
        new_root_node->keys_.push_back(key);
        new_root_node->values_.push_back(value);
        Page* root_page = page_manager_.FetchPage(root_page_id_);
        new_root_node->Serialize(root_page);
        MarkDirty(root_page_id_);
        return;
    }

    uint32_t leaf_parent_page_id = INVALID_PAGE_ID;
    auto leaf_ptr = FindLeaf(key, leaf_parent_page_id);
    if (!leaf_ptr) { ... abort insert ... }

    // 在叶子节点中找到插入位置，并插入新键与新值
    size_t i = 0;
    while (i < leaf_ptr->keys_.size() && key > leaf_ptr->keys_[i]) {
        i++;
    }
    leaf_ptr->keys_.insert(leaf_ptr->keys_.begin() + i, key);
    leaf_ptr->values_.insert(leaf_ptr->values_.begin() + i, value);

    Page* leaf_page = page_manager_.FetchPage(leaf_ptr->page_id_);
    leaf_ptr->Serialize(leaf_page);
    MarkDirty(leaf_ptr->page_id_);

    if (leaf_ptr->IsFull()) {
        SplitLeafNode(std::move(leaf_ptr), leaf_parent_page_id);
    }
}
```

逻辑：

- 如果树为空，直接创建一个新的叶子作为根。
- 否则：
  - 定位到包含该 key 的叶子节点；
  - 在 `keys_` 中找到插入点（保持有序），同时插入对应 `value`；
  - 序列化写回叶子到页；
  - 若叶子已满，调用 `SplitLeafNode` 分裂。

---

## 5. 叶子分裂：`SplitLeafNode`

```cpp
template <typename KeyType, typename ValueType>
void BPlusTree<KeyType, ValueType>::SplitLeafNode(
    std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>> leaf_ptr,
    uint32_t parent_page_id) {

    BPlusTreeLeafNode<KeyType, ValueType>* leaf = leaf_ptr.get();
    uint32_t new_leaf_page_id = page_manager_.NewPage();
    auto new_leaf_ptr = std::make_unique<BPlusTreeLeafNode<KeyType, ValueType>>(
        leaf->max_size_, new_leaf_page_id, parent_page_id);
    BPlusTreeLeafNode<KeyType, ValueType>* new_leaf = new_leaf_ptr.get();

    size_t split_idx = leaf->max_size_ / 2;
    // 将后半部分 key/value 移动到新叶子
    new_leaf->keys_.insert(new_leaf->keys_.begin(),
        std::make_move_iterator(leaf->keys_.begin() + split_idx),
        std::make_move_iterator(leaf->keys_.end()));
    new_leaf->values_.insert(new_leaf->values_.begin(),
        std::make_move_iterator(leaf->values_.begin() + split_idx),
        std::make_move_iterator(leaf->values_.end()));
    leaf->keys_.erase(leaf->keys_.begin() + split_idx, leaf->keys_.end());
    leaf->values_.erase(leaf->values_.begin() + split_idx, leaf->values_.end());

    // 维护叶子链表指针
    uint32_t old_next_leaf_page_id = leaf->next_leaf_page_id_;
    new_leaf->prev_leaf_page_id_ = leaf->page_id_;
    new_leaf->next_leaf_page_id_ = old_next_leaf_page_id;
    leaf->next_leaf_page_id_ = new_leaf_page_id;
    if (old_next_leaf_page_id != INVALID_PAGE_ID) {
        auto old_next_leaf_ptr =
          std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>>(
            static_cast<BPlusTreeLeafNode<KeyType, ValueType>*>(
                GetNode(old_next_leaf_page_id).release()));
        old_next_leaf_ptr->prev_leaf_page_id_ = new_leaf_page_id;
        Page* old_next_leaf_page = page_manager_.FetchPage(old_next_leaf_ptr->page_id_);
        old_next_leaf_ptr->Serialize(old_next_leaf_page);
        MarkDirty(old_next_leaf_page_id);
    }

    // 序列化旧叶和新叶
    Page* leaf_page = page_manager_.FetchPage(leaf->page_id_);
    leaf->Serialize(leaf_page);
    MarkDirty(leaf->page_id_);

    Page* new_leaf_page = page_manager_.FetchPage(new_leaf->page_id_);
    new_leaf->Serialize(new_leaf_page);
    MarkDirty(new_leaf->page_id_);

    // 提升新叶的第一个 key 到父节点
    KeyType promoted_key = new_leaf->keys_[0];
    InsertIntoParent(std::move(leaf_ptr), promoted_key, new_leaf_page_id, parent_page_id);
}
```

算法要点：

- 选取中间位置 `split_idx`，将后半部分元素移动到新叶子。
- 更新叶层链表前驱/后继指针，保持顺序扫描仍然可行。
- 将新叶子首 key 作为分割键，插入父节点。

---

## 6. 插入到父节点：`InsertIntoParent`

```cpp
template <typename KeyType, typename ValueType>
void BPlusTree<KeyType, ValueType>::InsertIntoParent(
    std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> node_ptr,
    const KeyType &key,
    uint32_t new_node_page_id,
    uint32_t parent_of_node_page_id) {

    BPlusTreeNode<KeyType, ValueType>* node = node_ptr.get();

    // 情况 1：当前节点是根 -> 需要创建新根
    if (node->page_id_ == root_page_id_) {
        uint32_t new_root_page_id = page_manager_.NewPage();
        auto new_root = std::make_unique<BPlusTreeInternalNode<KeyType, ValueType>>(
            fanout_, new_root_page_id, INVALID_PAGE_ID);
        new_root->keys_.push_back(key);
        new_root->children_page_ids_.push_back(node->page_id_);
        new_root->children_page_ids_.push_back(new_node_page_id);

        // 更新两个孩子的 parent_page_id_
        node->parent_page_id_ = new_root_page_id;
        Page* child_node_1_page = page_manager_.FetchPage(node->page_id_);
        node->Serialize(child_node_1_page);
        MarkDirty(node->page_id_);

        auto child_node_2_ptr = GetNode(new_node_page_id);
        child_node_2_ptr->parent_page_id_ = new_root_page_id;
        Page* child_node_2_page = page_manager_.FetchPage(child_node_2_ptr->page_id_);
        child_node_2_ptr->Serialize(child_node_2_page);
        MarkDirty(child_node_2_ptr->page_id_);

        Page* new_root_page = page_manager_.FetchPage(new_root_page_id);
        new_root->Serialize(new_root_page);
        MarkDirty(new_root_page_id);
        root_page_id_ = new_root_page_id;
        return;
    }

    // 情况 2：普通内部节点，往父节点中插入
    uint32_t parent_page_id = parent_of_node_page_id;
    auto parent_node_ptr = GetNode(parent_page_id);
    auto* parent =
      static_cast<BPlusTreeInternalNode<KeyType, ValueType>*>(parent_node_ptr.get());

    // 在父节点 keys_ 中找到插入位置 i，使得 keys_[i-1] < key <= keys_[i]
    size_t i = 0;
    while (i < parent->keys_.size() && key > parent->keys_[i]) {
        i++;
    }
    parent->keys_.insert(parent->keys_.begin() + i, key);
    parent->children_page_ids_.insert(parent->children_page_ids_.begin() + i + 1, new_node_page_id);

    // 更新新子节点 parent_page_id_
    auto new_child_node_ptr = GetNode(new_node_page_id);
    new_child_node_ptr->parent_page_id_ = parent->page_id_;
    Page* new_child_node_page = page_manager_.FetchPage(new_child_node_ptr->page_id_);
    new_child_node_ptr->Serialize(new_child_node_page);
    MarkDirty(new_child_node_ptr->page_id_);

    // 重写父节点到磁盘
    Page* parent_page = page_manager_.FetchPage(parent->page_id_);
    parent->Serialize(parent_page);
    MarkDirty(parent->page_id_);

    if (parent->IsFull()) {
        auto internal_parent_ptr =
          std::unique_ptr<BPlusTreeInternalNode<KeyType, ValueType>>(
            static_cast<BPlusTreeInternalNode<KeyType, ValueType>*>(parent_node_ptr.release()));
        SplitInternalNode(std::move(internal_parent_ptr));
    }
}
```

逻辑要点：

- 若分裂的是根结点，需新建根结点并提升一层。
- 否则在父结点中插入新 key 和右子指针，如果父结点满，则继续分裂上推。

---

## 7. 内部结点分裂：`SplitInternalNode`

```cpp
template <typename KeyType, typename ValueType>
void BPlusTree<KeyType, ValueType>::SplitInternalNode(
    std::unique_ptr<BPlusTreeInternalNode<KeyType, ValueType>> internal_ptr) {
    auto* internal = internal_ptr.get();
    uint32_t new_internal_page_id = page_manager_.NewPage();
    auto new_internal_ptr = std::make_unique<BPlusTreeInternalNode<KeyType, ValueType>>(
        internal->max_size_, new_internal_page_id, internal->parent_page_id_);
    auto* new_internal = new_internal_ptr.get();

    // 选取中间 key 作为向上提升的 key，左边保留在原节点，右边移到新节点
    size_t mid_idx = internal->keys_.size() / 2;
    KeyType up_key = internal->keys_[mid_idx];

    // new_internal 接收右半部分 keys_ 与 children_page_ids_
    ...
    // internal 保留左半部分
    ...

    // 更新右半部分子节点的 parent_page_id_ 为 new_internal_page_id
    ...

    // 将 internal/new_internal 序列化回各自页
    ...

    // 将 up_key 和 new_internal_page_id 插入到父节点
    InsertIntoParent(std::move(internal_ptr), up_key, new_internal_page_id,
                     internal->parent_page_id_);
}
```

> 源码中有详细的移动向量与序列化逻辑，这里从算法层面概括：  
> 通过提取中间 key 向上，用 B+ 树标准算法保持树的平衡。

---

## 8. 搜索：`Search` 与 `GetAllValues`

### 8.1 `Search(const KeyType&, ValueType&)`

```cpp
template <typename KeyType, typename ValueType>
bool BPlusTree<KeyType, ValueType>::Search(const KeyType &key, ValueType &value) {
    std::lock_guard<std::mutex> guard(tree_mutex_);
    if (root_page_id_ == INVALID_PAGE_ID) {
        return false;
    }

    uint32_t parent_page_id = INVALID_PAGE_ID;
    auto leaf_ptr = FindLeaf(key, parent_page_id);
    if (!leaf_ptr) return false;

    // 在 leaf_ptr->keys_ 中线性查找给定 key
    for (size_t i = 0; i < leaf_ptr->keys_.size(); ++i) {
        if (leaf_ptr->keys_[i] == key) {
            value = leaf_ptr->values_[i];
            return true;
        }
    }
    return false;
}
```

要点：

- 在树级别加锁保护搜索过程。
- 利用 `FindLeaf` 找到包含 key 的叶子，再在叶子中做线性查找。

### 8.2 `GetAllValues()`

```cpp
template <typename KeyType, typename ValueType>
std::vector<std::pair<KeyType, ValueType>>
BPlusTree<KeyType, ValueType>::GetAllValues() {
    std::lock_guard<std::mutex> guard(tree_mutex_);
    std::vector<std::pair<KeyType, ValueType>> result;

    if (root_page_id_ == INVALID_PAGE_ID) return result;

    // 从最左叶子开始
    uint32_t page_id = root_page_id_;
    // 向下走到最左叶子
    ...
    auto leaf_ptr = 最左叶子;

    while (leaf_ptr) {
        for (size_t i = 0; i < leaf_ptr->keys_.size(); ++i) {
            result.emplace_back(leaf_ptr->keys_[i], leaf_ptr->values_[i]);
        }
        if (leaf_ptr->next_leaf_page_id_ == INVALID_PAGE_ID) break;
        leaf_ptr = GetNode(leaf_ptr->next_leaf_page_id_) as leaf;
    }
    return result;
}
```

要点：

- 用于实现**全表扫描**（例如 SELECT 无 WHERE 或 JOIN 等场景）。
- 遍历叶层链表，把所有 key/value 收集为一个 `vector<pair<...>>`。

---

## 9. 删除：`Delete` 与辅佐函数

### 9.1 `Delete(const KeyType& key)`

```cpp
template <typename KeyType, typename ValueType>
void BPlusTree<KeyType, ValueType>::Delete(const KeyType &key) {
    std::lock_guard<std::mutex> guard(tree_mutex_);
    if (root_page_id_ == INVALID_PAGE_ID) return;

    uint32_t parent_page_id = INVALID_PAGE_ID;
    auto leaf_ptr = FindLeaf(key, parent_page_id);
    if (!leaf_ptr) return;

    // 从 leaf_ptr->keys_ 和 values_ 移除对应 key
    ...

    // 若叶子仍满足最小占用，直接写回；否则需要调用 RemoveEntry(...) 做合并/重新分配
    ...
}
```

### 9.2 `RemoveEntry`, `Redistribute`, `Merge`, `FindParentPageId`

删除逻辑参考标准 B+ 树算法：

- `RemoveEntry`：
  - 从给定节点中移除 key 和对应的 child/page；
  - 如果节点仍满足最小占用（不欠满），序列化写回即可；
  - 否则需要从兄弟节点重分配或与兄弟合并；
  - 在合并的情况下，还需要从父节点删除相应分割键，并可能递归向上修复。

- `Redistribute`：
  - 当某个结点欠满而兄弟结点有多余元素时，通过在兄弟和当前结点之间移动若干 key/child 来补足当前结点。
  - 同时更新父结点的分割键。

- `Merge`：
  - 当兄弟也没有多余元素时，将当前节点和兄弟节点合并成一个节点；
  - 从父节点中删除对应的分割键；
  - 在极端情况下，如果父节点成为空并且它是根，则需要将新合并节点提升为根。

- `FindParentPageId(child_page_id)`：
  - 在树中整体扫描以找到指定 child 页的父节点（这是一个辅助函数，用于删除时追溯父节点）。

> 当前版本的删除实现更多用于回滚回放与简单场景，并未在高并发写入压力下做极端优化；但整体逻辑遵循 B+ 树的标准删除算法。

---

## 10. 节点读写：`GetNode` 与 `MarkDirty`

### 10.1 `GetNode(uint32_t page_id)`

```cpp
template <typename KeyType, typename ValueType>
std::unique_ptr<BPlusTreeNode<KeyType, ValueType>>
BPlusTree<KeyType, ValueType>::GetNode(uint32_t page_id) {
    Page* page = page_manager_.FetchPage(page_id);
    if (!page) return nullptr;

    // 从页头或某个标识位判断是叶子节点还是内部节点
    // 然后构造对应的 LeafNode 或 InternalNode 对象并调用 Deserialize
    ...
}
```

作用：

- 负责从 `PageManager` 取出原始 `Page` 对象；
- 判断节点类型（叶/内部），构造相应子类实例，并调用其 `Deserialize`。

### 10.2 `MarkDirty(uint32_t page_id)`

```cpp
template <typename KeyType, typename ValueType>
void BPlusTree<KeyType, ValueType>::MarkDirty(uint32_t page_id) {
    Page* page = page_manager_.FetchPage(page_id);
    if (page) {
        page->is_dirty = true;
    }
}
```

- 用于告知 `PageManager`：该页数据已被修改，需要之后写回磁盘。
- 常在节点 `Serialize` 之后被调用。

---

## 11. 析构函数：`~BPlusTree`

```cpp
template <typename KeyType, typename ValueType>
BPlusTree<KeyType, ValueType>::~BPlusTree() {
    // 当前实现中主要依赖 PageManager 的 FlushAllPages，
    // 析构函数更多是一个 debug 输出的钩子。
}
```

在项目日志中可以看到 `"BPlusTree: Destructor called."` 这样的一条信息，表示索引对象生命周期结束。  
实际数据持久化是通过：

- 插入/删除时调用 `Serialize + MarkDirty`；
- `PageManager` 的 `FlushPage/FlushAllPages` 在合适时机将脏页写回磁盘。

---

## 12. 小结

- B+ 树模块为 ToyDB 提供了**基于磁盘页、支持 MVCC 版本链的主键索引**：
  - 插入：通过追加到版本链并在叶子分裂时调整树结构；
  - 搜索：从根到叶，线性查找叶子中的 key；
  - 删除：按标准 B+ 树算法做节点合并与重分配；
  - 全表扫描：利用叶层链表一次性取出所有 key/value。
- 系统整体通过：
  - `PageManager` 提供页级 IO；
  - `BPlusTree` 实现树结构；
  - `Table` 使用 `BPlusTree<int, std::vector<VersionedRow>>` 实现行存储；
  - 上层 `main.cpp` 的 SQL 执行逻辑配合 MVCC 和锁管理，实现一个教学用的小型事务数据库。


