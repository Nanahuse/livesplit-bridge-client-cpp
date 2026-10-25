#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace livesplit::bridge::detail {

class schema_error : public std::runtime_error {
public:
    explicit schema_error(std::string message) : std::runtime_error(std::move(message)) {}
};

class duplicate_key_scanner final : public nlohmann::json_sax<nlohmann::json> {
public:
    using number_integer_t = nlohmann::json::number_integer_t;
    using number_unsigned_t = nlohmann::json::number_unsigned_t;
    using number_float_t = nlohmann::json::number_float_t;
    using string_t = nlohmann::json::string_t;
    using binary_t = nlohmann::json::binary_t;

    bool null() override { return true; }
    bool boolean(bool) override { return true; }
    bool number_integer(number_integer_t) override { return true; }
    bool number_unsigned(number_unsigned_t) override { return true; }
    bool number_float(number_float_t, const string_t&) override { return true; }
    bool string(string_t&) override { return true; }
    bool binary(binary_t&) override { return true; }

    bool start_object(std::size_t) override {
        frames_.push_back(frame{.is_object = true});
        return true;
    }

    bool key(string_t& value) override {
        if (!frames_.empty() && frames_.back().is_object) {
            if (!frames_.back().keys.insert(value).second) {
                error_ = "duplicate JSON property: " + value;
                return false;
            }
        }
        return true;
    }

    bool end_object() override {
        if (!frames_.empty()) frames_.pop_back();
        return true;
    }

    bool start_array(std::size_t) override {
        frames_.push_back(frame{.is_object = false});
        return true;
    }

    bool end_array() override {
        if (!frames_.empty()) frames_.pop_back();
        return true;
    }

    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception& exception) override {
        if (error_.empty()) error_ = exception.what();
        return false;
    }

    const std::string& error() const noexcept { return error_; }

private:
    struct frame {
        bool is_object;
        std::set<std::string> keys;
    };

    std::vector<frame> frames_;
    std::string error_;
};

inline nlohmann::json parse_strict(std::string_view text) {
    duplicate_key_scanner scanner;
    if (!nlohmann::json::sax_parse(
            text.begin(),
            text.end(),
            &scanner,
            nlohmann::json::input_format_t::json,
            true)) {
        throw schema_error(scanner.error().empty() ? "invalid JSON" : scanner.error());
    }
    try {
        return nlohmann::json::parse(text.begin(), text.end());
    } catch (const std::exception& error) {
        throw schema_error(error.what());
    }
}

inline const nlohmann::json& require_object(const nlohmann::json& value, std::string_view path) {
    if (!value.is_object()) {
        throw schema_error(std::string(path) + ": expected an object");
    }
    return value;
}

inline const nlohmann::json& require_array(const nlohmann::json& value, std::string_view path) {
    if (!value.is_array()) {
        throw schema_error(std::string(path) + ": expected an array");
    }
    return value;
}

inline const nlohmann::json& require_member(
    const nlohmann::json& object,
    std::string_view key,
    std::string_view path) {
    const auto iterator = object.find(key);
    if (iterator == object.end()) {
        throw schema_error(std::string(path) + ": missing required property");
    }
    return *iterator;
}

inline const nlohmann::json* optional_member(const nlohmann::json& object, std::string_view key) {
    const auto iterator = object.find(key);
    return iterator == object.end() ? nullptr : &*iterator;
}

inline void reject_unknown(
    const nlohmann::json& object,
    std::initializer_list<std::string_view> allowed,
    std::string_view type) {
    const std::set<std::string_view> names(allowed);
    for (const auto& [key, value] : object.items()) {
        (void)value;
        if (!names.contains(key)) {
            throw schema_error(std::string(type) + ": unknown property " + key);
        }
    }
}

inline std::string_view get_string(const nlohmann::json& value, std::string_view path) {
    if (!value.is_string()) {
        throw schema_error(std::string(path) + ": expected a string");
    }
    const auto& text = value.get_ref<const std::string&>();
    return text;
}

inline bool get_bool(const nlohmann::json& value, std::string_view path) {
    if (!value.is_boolean()) {
        throw schema_error(std::string(path) + ": expected a boolean");
    }
    return value.get<bool>();
}

inline std::uint32_t get_uint32(
    const nlohmann::json& value,
    std::string_view path,
    std::uint32_t minimum,
    std::uint32_t maximum) {
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
        throw schema_error(std::string(path) + ": expected an integer");
    }
    std::uint64_t number = 0;
    if (value.is_number_unsigned()) {
        number = value.get<std::uint64_t>();
    } else {
        const auto signed_number = value.get<std::int64_t>();
        if (signed_number < 0) {
            throw schema_error(std::string(path) + ": integer is out of range");
        }
        number = static_cast<std::uint64_t>(signed_number);
    }
    if (number < minimum || number > maximum) {
        throw schema_error(std::string(path) + ": integer is out of range");
    }
    return static_cast<std::uint32_t>(number);
}

inline std::int32_t get_int32(
    const nlohmann::json& value,
    std::string_view path,
    std::int32_t minimum,
    std::int32_t maximum) {
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
        throw schema_error(std::string(path) + ": expected an integer");
    }
    std::int64_t number = 0;
    if (value.is_number_unsigned()) {
        const auto unsigned_number = value.get<std::uint64_t>();
        if (unsigned_number > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
            throw schema_error(std::string(path) + ": integer is out of range");
        }
        number = static_cast<std::int64_t>(unsigned_number);
    } else {
        number = value.get<std::int64_t>();
    }
    if (number < minimum || number > maximum) {
        throw schema_error(std::string(path) + ": integer is out of range");
    }
    return static_cast<std::int32_t>(number);
}

inline std::int64_t get_int64(
    const nlohmann::json& value,
    std::string_view path,
    std::int64_t minimum,
    std::int64_t maximum) {
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
        throw schema_error(std::string(path) + ": expected an integer");
    }
    std::int64_t number = 0;
    if (value.is_number_unsigned()) {
        const auto unsigned_number = value.get<std::uint64_t>();
        if (unsigned_number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw schema_error(std::string(path) + ": integer is out of range");
        }
        number = static_cast<std::int64_t>(unsigned_number);
    } else {
        number = value.get<std::int64_t>();
    }
    if (number < minimum || number > maximum) {
        throw schema_error(std::string(path) + ": integer is out of range");
    }
    return number;
}

inline void require_null(const nlohmann::json& value, std::string_view path) {
    if (!value.is_null()) {
        throw schema_error(std::string(path) + ": expected null");
    }
}

inline void require_const_string(
    const nlohmann::json& value,
    std::string_view expected,
    std::string_view path) {
    const auto actual = get_string(value, path);
    if (actual != expected) {
        throw schema_error(std::string(path) + ": expected " + std::string(expected));
    }
}

template <typename Variant, typename... Alternatives>
Variant parse_variant(const nlohmann::json& document, std::string_view type) {
    std::optional<Variant> result;
    std::size_t matches = 0;
    auto attempt = [&]<typename Alternative>() {
        try {
            Alternative candidate = document.get<Alternative>();
            ++matches;
            if (matches == 1) {
                result.emplace(std::move(candidate));
            }
        } catch (const schema_error&) {
        }
    };
    (attempt.template operator()<Alternatives>(), ...);
    if (matches != 1) {
        throw schema_error(
            std::string(type) + ": expected exactly one matching alternative, got " +
            std::to_string(matches));
    }
    return std::move(*result);
}

}  // namespace livesplit::bridge::detail
