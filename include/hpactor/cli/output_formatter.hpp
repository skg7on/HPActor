#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hpactor {
namespace cli {

struct TreeNode {
    std::string name;
    std::string description;
    std::vector<TreeNode> children;
};

class OutputFormatter {
public:
    virtual ~OutputFormatter() = default;
    virtual void header(const std::string& title) = 0;
    virtual void table(const std::vector<std::string>& columns,
                       const std::vector<std::vector<std::string>>& rows) = 0;
    virtual void key_value(const std::map<std::string, std::string>& pairs) = 0;
    virtual void tree(const TreeNode& root) = 0;
    virtual void raw(const std::string& text) = 0;
    virtual void error(const std::string& message) = 0;
    virtual std::string finalize() = 0;

    static std::unique_ptr<OutputFormatter> create(const std::string& format);
};

}  // namespace cli
}  // namespace hpactor
