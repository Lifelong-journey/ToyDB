#pragma once

#include <vector>
#include <string>
#include <memory>
#include <optional>
#include "definitions.h"

namespace toydb {

// Row 类型：表示一行数据
using Row = std::vector<std::string>;

// 算子基类：火山模型的核心接口
class Operator {
public:
    virtual ~Operator() = default;
    
    // 火山模型的核心方法：获取下一行数据
    // 返回 std::nullopt 表示没有更多数据
    virtual std::optional<Row> Next() = 0;
    
    // 重置算子状态（用于某些需要多次扫描的算子）
    virtual void Reset() {}
    
    // 获取输出列的元数据（用于投影等操作）
    virtual std::vector<std::string> GetOutputColumns() const { return {}; }
};

} // namespace toydb

