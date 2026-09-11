#pragma once

#include <filesystem>
#include <string>
#include <vector>

class Preprocessor {
public:
  bool run(const std::filesystem::path& input, std::string& output);

private:
  bool expand(const std::filesystem::path& input, std::string& output);
  bool handleInclude(const std::filesystem::path& input, const std::string& operand, int line,
                     int col, std::string& output);
  bool handleDeclare(const std::filesystem::path& input, int line, int col);
  bool handleAssignment(const std::filesystem::path& input, const std::string& name, int line,
                        int col);
  bool handleActivation(const std::filesystem::path& input, const std::string& name, int line,
                        int col);
  bool handleConditional(const std::filesystem::path& input, const std::string& name, int line,
                         int col);
  bool handleDo(const std::filesystem::path& input, const std::string& name, int line, int col);
  bool handleGoto(const std::filesystem::path& input, const std::string& name, int line, int col);
  bool handleProcedure(const std::filesystem::path& input, const std::string& name, int line,
                       int col);
  bool unsupported(const std::filesystem::path& input, const std::string& name, int line, int col);
  bool error(const std::filesystem::path& input, int line, int col, const std::string& message);

  std::vector<std::filesystem::path> active_;
};
