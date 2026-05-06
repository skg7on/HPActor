#pragma once

#include <hpactor/cli/output_formatter.hpp>

namespace hpactor {
namespace cli {

class TabularFormatter : public OutputFormatter {
public:
    void header(const std::string& title) override;
    void table(const std::vector<std::string>& columns,
               const std::vector<std::vector<std::string>>& rows) override;
    void key_value(const std::map<std::string, std::string>& pairs) override;
    void tree(const TreeNode& root) override;
    void raw(const std::string& text) override;
    void error(const std::string& message) override;
    std::string finalize() override;

private:
    std::string buffer_;
};

}  // namespace cli
}  // namespace hpactor
