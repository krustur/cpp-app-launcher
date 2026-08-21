#include "SimpleJson.h"

#include <cctype>
#include <limits>
#include <stdexcept>

namespace simple_json {
namespace {

void SkipWhitespace(const std::string& text, size_t* index) {
  while (*index < text.size() &&
         std::isspace(static_cast<unsigned char>(text[*index])) != 0) {
    ++(*index);
  }
}

bool Consume(const std::string& text, size_t* index, char expected) {
  SkipWhitespace(text, index);
  if (*index >= text.size() || text[*index] != expected) {
    return false;
  }

  ++(*index);
  return true;
}

bool ReadString(const std::string& text, size_t* index, std::string* value,
                std::string* error_message) {
  SkipWhitespace(text, index);
  if (*index >= text.size() || text[*index] != '"') {
    if (error_message != nullptr) {
      *error_message = "Expected string";
    }
    return false;
  }

  ++(*index);
  std::string parsed;
  while (*index < text.size()) {
    char current = text[*index];
    ++(*index);

    if (current == '"') {
      *value = parsed;
      return true;
    }

    if (current == '\\') {
      if (*index >= text.size()) {
        if (error_message != nullptr) {
          *error_message = "Incomplete escape sequence";
        }
        return false;
      }

      char escaped = text[*index];
      ++(*index);
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          parsed.push_back(escaped);
          break;
        case 'b':
          parsed.push_back('\b');
          break;
        case 'f':
          parsed.push_back('\f');
          break;
        case 'n':
          parsed.push_back('\n');
          break;
        case 'r':
          parsed.push_back('\r');
          break;
        case 't':
          parsed.push_back('\t');
          break;
        default:
          if (error_message != nullptr) {
            *error_message = "Unsupported escape sequence";
          }
          return false;
      }
      continue;
    }

    parsed.push_back(current);
  }

  if (error_message != nullptr) {
    *error_message = "Unterminated string";
  }
  return false;
}

bool ReadInt(const std::string& text, size_t* index, int* value,
             std::string* error_message) {
  SkipWhitespace(text, index);
  if (*index >= text.size()) {
    if (error_message != nullptr) {
      *error_message = "Expected integer";
    }
    return false;
  }

  size_t start = *index;
  if (text[*index] == '-') {
    ++(*index);
  }

  size_t digits_begin = *index;
  while (*index < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[*index])) != 0) {
    ++(*index);
  }

  if (digits_begin == *index) {
    if (error_message != nullptr) {
      *error_message = "Expected integer";
    }
    return false;
  }

  try {
    long parsed = std::stol(text.substr(start, *index - start));
    if (parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max()) {
      if (error_message != nullptr) {
        *error_message = "Integer out of range";
      }
      return false;
    }

    *value = static_cast<int>(parsed);
    return true;
  } catch (const std::exception&) {
    if (error_message != nullptr) {
      *error_message = "Invalid integer";
    }
    return false;
  }
}

}  // namespace

bool Document::Parse(const std::string& text, std::string* error_message) {
  string_values_.clear();
  int_values_.clear();

  size_t index = 0;
  if (!Consume(text, &index, '{')) {
    if (error_message != nullptr) {
      *error_message = "Expected opening brace";
    }
    return false;
  }

  SkipWhitespace(text, &index);
  if (index < text.size() && text[index] == '}') {
    ++index;
    SkipWhitespace(text, &index);
    return index == text.size();
  }

  while (index < text.size()) {
    std::string key;
    if (!ReadString(text, &index, &key, error_message)) {
      return false;
    }

    if (!Consume(text, &index, ':')) {
      if (error_message != nullptr) {
        *error_message = "Expected colon";
      }
      return false;
    }

    SkipWhitespace(text, &index);
    if (index >= text.size()) {
      if (error_message != nullptr) {
        *error_message = "Expected value";
      }
      return false;
    }

    if (text[index] == '"') {
      std::string value;
      if (!ReadString(text, &index, &value, error_message)) {
        return false;
      }
      string_values_[key] = value;
    } else {
      int value = 0;
      if (!ReadInt(text, &index, &value, error_message)) {
        if (error_message != nullptr && error_message->empty()) {
          *error_message = "Only top-level strings and integers are supported";
        }
        return false;
      }
      int_values_[key] = value;
    }

    SkipWhitespace(text, &index);
    if (index >= text.size()) {
      if (error_message != nullptr) {
        *error_message = "Unexpected end of JSON";
      }
      return false;
    }

    if (text[index] == '}') {
      ++index;
      SkipWhitespace(text, &index);
      if (index != text.size()) {
        if (error_message != nullptr) {
          *error_message = "Trailing characters after JSON object";
        }
        return false;
      }
      return true;
    }

    if (text[index] != ',') {
      if (error_message != nullptr) {
        *error_message = "Expected comma";
      }
      return false;
    }

    ++index;
  }

  if (error_message != nullptr) {
    *error_message = "Unexpected end of JSON";
  }
  return false;
}

std::optional<std::string> Document::GetString(const std::string& key) const {
  auto it = string_values_.find(key);
  if (it == string_values_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<int> Document::GetInt(const std::string& key) const {
  auto it = int_values_.find(key);
  if (it == int_values_.end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace simple_json
