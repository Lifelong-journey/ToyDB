#pragma once

#include "operator.h"
#include <memory>
#include <vector>
#include <string>
#include <functional>

namespace toydb {

// 排序算子：对输入行进行排序
// 注意：需要物化所有输入行才能排序
class SortOperator : public Operator {
public:
    using CompareFunc = std::function<bool(const Row&, const Row&)>;
    
    SortOperator(
        std::unique_ptr<Operator> child,
        CompareFunc compare_func
    );
    
    std::optional<Row> Next() override;
    void Reset() override;
    std::vector<std::string> GetOutputColumns() const override;

private:
    std::unique_ptr<Operator> child_;
    CompareFunc compare_func_;
    
    std::vector<Row> sorted_rows_;
    size_t current_index_;
    bool initialized_;
    
    void Initialize();
};

} // namespace toydb

