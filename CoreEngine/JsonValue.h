#pragma once

template <std::unsigned_integral UInt>
bool TryGetJsonUnsignedInteger(const nlohmann::json& value, UInt& output) noexcept
{
    UInt parsedValue{};

    if (value.is_number_unsigned())
    {
        const auto number = value.get<nlohmann::json::number_unsigned_t>();
        if (number > (std::numeric_limits<UInt>::max)())
        {
            return false;
        }
        parsedValue = static_cast<UInt>(number);
    }
    else if (value.is_number_integer())
    {
        const auto number = value.get<nlohmann::json::number_integer_t>();
        if (number < 0 || static_cast<nlohmann::json::number_unsigned_t>(number) > (std::numeric_limits<UInt>::max)())
        {
            return false;
        }
        parsedValue = static_cast<UInt>(number);
    }
    else if (value.is_number_float())
    {
        constexpr double maximumExactlyRepresentableJsonInteger = 9007199254740991.0;
        const auto number = value.get<nlohmann::json::number_float_t>();
        if (!std::isfinite(number) || number < 0 || number > maximumExactlyRepresentableJsonInteger ||
            number > static_cast<double>((std::numeric_limits<UInt>::max)()) || std::trunc(number) != number)
        {
            return false;
        }
        parsedValue = static_cast<UInt>(number);
    }
    else
    {
        return false;
    }

    output = parsedValue;
    return true;
}
