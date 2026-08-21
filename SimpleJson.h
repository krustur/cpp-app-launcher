#pragma once

#include <optional>
#include <string>
#include <unordered_map>

namespace simple_json {

class Document {
 public:
  bool Parse(const std::string& text, std::string* error_message = nullptr);

  std::optional<std::string> GetString(const std::string& key) const;
  std::optional<int> GetInt(const std::string& key) const;

 private:
  std::unordered_map<std::string, std::string> string_values_;
  std::unordered_map<std::string, int> int_values_;
};

}  // namespace simple_json
