#pragma once

#include "operator.h"
#include <memory>

namespace toydb {

// LIMIT 算子：限制输出行数
class LimitOperator : public Operator {
public:
    LimitOperator(
        std::unique_ptr<Operator> child,
        size_t limit
    );
    
    std::optional<Row> Next() override;
    void Reset() override;
    std::vector<std::string> GetOutputColumns() const override;

private:
    std::unique_ptr<Operator> child_;
    size_t limit_;
    size_t count_;
};

} // namespace toydb

