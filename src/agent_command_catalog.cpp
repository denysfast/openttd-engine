/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

/** @file agent_command_catalog.cpp Catalog of every engine command and generic normal-command dispatch. */

#include "stdafx.h"
#include "agent_command_catalog.h"

#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "core/backup_type.hpp"
#include "core/enum_type.hpp"
#include "core/format.hpp"
#include "core/pool_type.hpp"
#include "network/network.h"

#include "airport_cmd.h"
#include "aircraft_cmd.h"
#include "autoreplace_cmd.h"
#include "blueprint_cmd.h"
#include "rail_route_cmd.h"
#include "company_cmd.h"
#include "depot_cmd.h"
#include "dock_cmd.h"
#include "economy_cmd.h"
#include "engine_cmd.h"
#include "goal_cmd.h"
#include "group_cmd.h"
#include "industry_cmd.h"
#include "landscape_cmd.h"
#include "league_cmd.h"
#include "misc_cmd.h"
#include "news_cmd.h"
#include "object_cmd.h"
#include "order_cmd.h"
#include "rail_cmd.h"
#include "road_cmd.h"
#include "roadveh_cmd.h"
#include "settings_cmd.h"
#include "signs_cmd.h"
#include "station_cmd.h"
#include "story_cmd.h"
#include "subsidy_cmd.h"
#include "terraform_cmd.h"
#include "timetable_cmd.h"
#include "town_cmd.h"
#include "train_cmd.h"
#include "tree_cmd.h"
#include "tunnelbridge_cmd.h"
#include "vehicle_cmd.h"
#include "viewport_cmd.h"
#include "water_cmd.h"
#include "waypoint_cmd.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "safeguards.h"

namespace InGameAgent {

namespace {

/** What the engine reported for the command that was posted last. */
struct PostedCommand {
	bool captured = false; ///< The engine reported a result, so the command really ran.
	CommandCost cost; ///< Actual cost, or the failure of the command.
};

/** Capture slot of the command that is currently being posted. */
PostedCommand _posting;

/* -- type inspection --------------------------------------------------------------------- */

/** Recognise a StrongType::Typedef and keep its tag, so the catalog can name it. */
template <typename T>
struct StrongTypedefInfo {
	static constexpr bool is = false; ///< Whether T is a StrongType::Typedef.
};

/** @copydoc StrongTypedefInfo */
template <typename TBase, typename TTag, typename... TProps>
struct StrongTypedefInfo<StrongType::Typedef<TBase, TTag, TProps...>> {
	static constexpr bool is = true; ///< Whether T is a StrongType::Typedef.
	using Base = TBase; ///< The wrapped value type.
	using Tag = TTag; ///< The tag that keeps the typedef distinct.
};

/** Recognise an EnumBitSet and keep its enum, storage and end marker. */
template <typename T>
struct EnumBitSetInfo {
	static constexpr bool is = false; ///< Whether T is an EnumBitSet.
};

/** @copydoc EnumBitSetInfo */
template <typename TEnum, typename TStorage, TEnum TEnd>
struct EnumBitSetInfo<EnumBitSet<TEnum, TStorage, TEnd>> {
	static constexpr bool is = true; ///< Whether T is an EnumBitSet.
	using Enum = TEnum; ///< The enum the bits are taken from.
	using Storage = TStorage; ///< The integer that stores the bits.
	static constexpr TEnum end = TEnd; ///< Last valid value + 1, as far as the enum declares it.
};

/** Recognise a pool id and keep its tag, so the catalog can name it. */
template <typename T>
struct PoolIdInfo {
	static constexpr bool is = false; ///< Whether T is a PoolID.
};

/** @copydoc PoolIdInfo */
template <typename TBase, typename TTag, TBase TMax, TBase TInvalid>
struct PoolIdInfo<PoolID<TBase, TTag, TMax, TInvalid>> {
	static constexpr bool is = true; ///< Whether T is a PoolID.
	using Tag = TTag; ///< The tag that keeps the pool id distinct.
};

/** Recognise a type that wraps a single value it can be constructed from (`BaseType`). */
template <typename T, typename = void>
struct HasBaseType {
	static constexpr bool value = false; ///< Whether T has a `BaseType` it can be built from.
};

/** @copydoc HasBaseType */
template <typename T>
struct HasBaseType<T, std::void_t<typename T::BaseType>> {
	static constexpr bool value = true; ///< Whether T has a `BaseType` it can be built from.
	using Base = typename T::BaseType; ///< The wrapped value type.
};

/** Recognise a std::vector. */
template <typename T>
struct StdVectorInfo {
	static constexpr bool is = false; ///< Whether T is a std::vector.
};

/** @copydoc StdVectorInfo */
template <typename TValue, typename TAlloc>
struct StdVectorInfo<std::vector<TValue, TAlloc>> {
	static constexpr bool is = true; ///< Whether T is a std::vector.
	using Value = TValue; ///< Element type.
};

/** Recognise a std::array. */
template <typename T>
struct StdArrayInfo {
	static constexpr bool is = false; ///< Whether T is a std::array.
};

/** @copydoc StdArrayInfo */
template <typename TValue, size_t TSize>
struct StdArrayInfo<std::array<TValue, TSize>> {
	static constexpr bool is = true; ///< Whether T is a std::array.
	using Value = TValue; ///< Element type.
	static constexpr size_t size = TSize; ///< Number of elements.
};

/** Recognise a std::tuple. */
template <typename T>
struct StdTupleInfo {
	static constexpr bool is = false; ///< Whether T is a std::tuple.
};

/** @copydoc StdTupleInfo */
template <typename... Ts>
struct StdTupleInfo<std::tuple<Ts...>> {
	static constexpr bool is = true; ///< Whether T is a std::tuple.
};

/**
 * The name of a type as the compiler spells it.
 * @return The raw type name, empty when the compiler gives no usable spelling.
 */
template <typename T>
constexpr std::string_view RawTypeName()
{
#if defined(__clang__) || defined(__GNUC__)
	const std::string_view signature = __PRETTY_FUNCTION__;
	const std::string_view prefix = "T = ";
	const size_t start = signature.find(prefix);
	if (start == std::string_view::npos) return {};
	const size_t begin = start + prefix.size();
	const size_t end = signature.find_first_of(";]", begin);
	if (end == std::string_view::npos) return {};
	return signature.substr(begin, end - begin);
#elif defined(_MSC_VER)
	const std::string_view signature = __FUNCSIG__;
	const std::string_view prefix = "RawTypeName<";
	const size_t start = signature.find(prefix);
	if (start == std::string_view::npos) return {};
	const size_t begin = start + prefix.size();
	const size_t end = signature.find('>', begin);
	if (end == std::string_view::npos) return {};
	return signature.substr(begin, end - begin);
#else
	return {};
#endif
}

/**
 * Turn a StrongType tag into the name of the type it belongs to: `TileIndexTag` becomes `TileIndex`.
 * @return The type name derived from the tag.
 */
template <typename TTag>
std::string TagTypeName()
{
	std::string_view raw = RawTypeName<TTag>();
	for (const std::string_view prefix : {"struct ", "class ", "enum "}) {
		if (raw.starts_with(prefix)) {
			raw.remove_prefix(prefix.size());
			break;
		}
	}
	if (raw.ends_with("Tag")) raw.remove_suffix(3);
	return std::string(raw);
}

/**
 * The catalog name of a command argument type.
 * @return A stable, readable type name.
 */
template <typename T>
std::string TypeName()
{
	using U = std::remove_cvref_t<T>;
	if constexpr (std::is_same_v<U, bool>) return "bool";
	else if constexpr (std::is_same_v<U, uint8_t>) return "uint8_t";
	else if constexpr (std::is_same_v<U, uint16_t>) return "uint16_t";
	else if constexpr (std::is_same_v<U, uint32_t>) return "uint32_t";
	else if constexpr (std::is_same_v<U, uint64_t>) return "uint64_t";
	else if constexpr (std::is_same_v<U, int8_t>) return "int8_t";
	else if constexpr (std::is_same_v<U, int16_t>) return "int16_t";
	else if constexpr (std::is_same_v<U, int32_t>) return "int32_t";
	else if constexpr (std::is_same_v<U, int64_t>) return "int64_t";
	else if constexpr (std::is_same_v<U, Money>) return "Money";
	else if constexpr (std::is_same_v<U, std::string>) return "string";
	else if constexpr (StrongTypedefInfo<U>::is) return TagTypeName<typename StrongTypedefInfo<U>::Tag>();
	else if constexpr (PoolIdInfo<U>::is) return TagTypeName<typename PoolIdInfo<U>::Tag>();
	else if constexpr (EnumBitSetInfo<U>::is) return "EnumBitSet<" + TypeName<typename EnumBitSetInfo<U>::Enum>() + ">";
	else if constexpr (StdVectorInfo<U>::is) return "vector<" + TypeName<typename StdVectorInfo<U>::Value>() + ">";
	else if constexpr (StdArrayInfo<U>::is) return "array<" + TypeName<typename StdArrayInfo<U>::Value>() + ">";
	else if constexpr (StdTupleInfo<U>::is) return std::string(RawTypeName<U>());
	else if constexpr (HasBaseType<U>::value) return std::string(RawTypeName<U>());
	else return std::string(RawTypeName<U>());
}

/**
 * The catalog kind of a command argument type.
 * @return The kind name: boolean, integer, enum, strong_id, enum_bit_set, string, vector, array,
 *         tuple, or unsupported.
 */
template <typename T>
const char *ArgKind()
{
	using U = std::remove_cvref_t<T>;
	if constexpr (std::is_same_v<U, bool>) return "boolean";
	else if constexpr (std::is_integral_v<U>) return "integer";
	else if constexpr (std::is_enum_v<U>) return "enum";
	else if constexpr (EnumBitSetInfo<U>::is) return "enum_bit_set";
	else if constexpr (std::is_same_v<U, std::string>) return "string";
	else if constexpr (StdVectorInfo<U>::is) return "vector";
	else if constexpr (StdArrayInfo<U>::is) return "array";
	else if constexpr (StdTupleInfo<U>::is) return "tuple";
	else if constexpr (HasBaseType<U>::value) return "strong_id";
	else return "unsupported";
}

/* -- what the decoder accepts ------------------------------------------------------------ */

template <typename T>
constexpr bool ComputeDecodable();

/** Whether every element of a tuple can be decoded. */
template <typename... Ts>
constexpr bool TupleDecodable(const std::tuple<Ts...> *)
{
	return (ComputeDecodable<Ts>() && ...);
}

/**
 * Whether the generic decoder can build a value of type T from JSON.
 * @return True when T is supported.
 */
template <typename T>
constexpr bool ComputeDecodable()
{
	using U = std::remove_cvref_t<T>;
	if constexpr (std::is_same_v<U, bool>) return true;
	else if constexpr (std::is_integral_v<U>) return true;
	else if constexpr (std::is_enum_v<U>) return true;
	else if constexpr (EnumBitSetInfo<U>::is) return true;
	else if constexpr (std::is_same_v<U, std::string>) return true;
	else if constexpr (StdVectorInfo<U>::is) return ComputeDecodable<typename StdVectorInfo<U>::Value>();
	else if constexpr (StdArrayInfo<U>::is) return ComputeDecodable<typename StdArrayInfo<U>::Value>();
	else if constexpr (StdTupleInfo<U>::is) return TupleDecodable(static_cast<const U *>(nullptr));
	else if constexpr (HasBaseType<U>::value) return ComputeDecodable<typename HasBaseType<U>::Base>();
	else return false;
}

/** Whether the generic decoder can build a value of type T from JSON. */
template <typename T>
inline constexpr bool IsDecodableArg = ComputeDecodable<T>();

/** Whether every argument of a command can be decoded. */
template <typename Ttuple>
struct ArgsDecodable;

/** @copydoc ArgsDecodable */
template <typename... Ts>
struct ArgsDecodable<std::tuple<Ts...>> {
	static constexpr bool value = (IsDecodableArg<Ts> && ...); ///< True when every argument is decodable.
};

/* -- JSON decoding ----------------------------------------------------------------------- */

/**
 * Describe a JSON value for an error message.
 * @param value The value to describe.
 * @return A short description of the JSON type.
 */
std::string DescribeJsonValue(const Json &value)
{
	if (value.is_null()) return "null";
	if (value.is_boolean()) return "a boolean";
	if (value.is_number_unsigned() || value.is_number_integer()) return "an integer";
	if (value.is_number_float()) return "a float";
	if (value.is_string()) return "a string";
	if (value.is_array()) return "an array";
	if (value.is_object()) return "an object";
	return "an unknown value";
}

/**
 * Decode a JSON value into an integral type, rejecting values that do not fit.
 * @param value The JSON value.
 * @param out Receives the decoded value.
 * @param normalized Receives the value as the catalog reports it back.
 * @param error Receives the reason when the value is rejected.
 * @return True when the value was decoded.
 */
template <typename T>
bool DecodeIntegral(const Json &value, T &out, Json &normalized, std::string &error)
{
	static_assert(std::is_integral_v<T>);

	if (value.is_number_unsigned()) {
		const uint64_t raw = value.get<uint64_t>();
		if (raw > static_cast<uint64_t>(std::numeric_limits<T>::max())) {
			error = "value " + fmt::format("{}", raw) + " does not fit in " + TypeName<T>();
			return false;
		}
		out = static_cast<T>(raw);
		normalized = out;
		return true;
	}

	if (value.is_number_integer()) {
		const int64_t raw = value.get<int64_t>();
		if constexpr (std::is_signed_v<T>) {
			if (raw < static_cast<int64_t>(std::numeric_limits<T>::min()) || raw > static_cast<int64_t>(std::numeric_limits<T>::max())) {
				error = "value " + fmt::format("{}", raw) + " does not fit in " + TypeName<T>();
				return false;
			}
		} else {
			if (raw < 0 || static_cast<uint64_t>(raw) > static_cast<uint64_t>(std::numeric_limits<T>::max())) {
				error = "value " + fmt::format("{}", raw) + " does not fit in " + TypeName<T>();
				return false;
			}
		}
		out = static_cast<T>(raw);
		normalized = out;
		return true;
	}

	error = "expected an integer, got " + DescribeJsonValue(value);
	return false;
}

/**
 * Decode a JSON value into a bitset of enum values.
 *
 * Accepts either the bits themselves as one integer, or an array of bit indices (which are the
 * enum values). Bits outside the range the enum declares are rejected.
 * @param value The JSON value.
 * @param out Receives the decoded bitset.
 * @param normalized Receives the bits as the catalog reports them back.
 * @param error Receives the reason when the value is rejected.
 * @return True when the value was decoded.
 */
template <typename T>
bool DecodeBitSet(const Json &value, T &out, Json &normalized, std::string &error)
{
	using Info = EnumBitSetInfo<T>;
	using Storage = typename Info::Storage;
	constexpr uint64_t valid_bits = static_cast<uint64_t>(to_underlying(Info::end));

	uint64_t bits = 0;
	if (value.is_array()) {
		for (const Json &item : value) {
			if (!item.is_number_unsigned() && !item.is_number_integer()) {
				error = "expected integer bit indices, got " + DescribeJsonValue(item);
				return false;
			}
			if (item.is_number_integer() && item.get<int64_t>() < 0) {
				error = "bit index " + fmt::format("{}", item.get<int64_t>()) + " is negative";
				return false;
			}
			const uint64_t bit = item.get<uint64_t>();
			if (bit >= valid_bits) {
				error = "bit index " + fmt::format("{}", bit) + " is outside the enum (0.." + fmt::format("{}", valid_bits - 1) + ")";
				return false;
			}
			bits |= (uint64_t{1} << bit);
		}
	} else if (value.is_number_unsigned() || value.is_number_integer()) {
		if (value.is_number_integer() && value.get<int64_t>() < 0) {
			error = "bitset value " + fmt::format("{}", value.get<int64_t>()) + " is negative";
			return false;
		}
		bits = value.get<uint64_t>();
		if (bits > static_cast<uint64_t>(std::numeric_limits<Storage>::max())) {
			error = "bitset value " + fmt::format("{}", bits) + " does not fit in " + TypeName<T>();
			return false;
		}
		if (valid_bits < static_cast<uint64_t>(std::numeric_limits<Storage>::digits) && (bits >> valid_bits) != 0) {
			error = "bitset value " + fmt::format("{}", bits) + " sets bits outside the enum (0.." + fmt::format("{}", valid_bits - 1) + ")";
			return false;
		}
	} else {
		error = "expected a bitset integer or an array of bit indices, got " + DescribeJsonValue(value);
		return false;
	}

	out = T{static_cast<Storage>(bits)};
	normalized = static_cast<Storage>(bits);
	return true;
}

/**
 * Decode the elements of a tuple argument.
 * @param value The JSON array to decode.
 * @param out Receives the decoded tuple.
 * @param elements Receives the normalized elements, already sized to the tuple.
 * @param error Receives the reason when a value is rejected.
 * @param ok Set to false when a value is rejected.
 */
template <typename Ttuple, size_t... I>
void DecodeTupleElements(const Json &value, Ttuple &out, Json &elements, std::string &error, bool &ok, std::index_sequence<I...>);

/**
 * Decode a JSON value into a command argument.
 * @param value The JSON value.
 * @param out Receives the decoded argument.
 * @param normalized Receives the argument as the catalog reports it back.
 * @param error Receives the reason when the value is rejected.
 * @return True when the value was decoded; false when the value or the type is not supported.
 */
template <typename T>
bool DecodeArg(const Json &value, T &out, Json &normalized, std::string &error)
{
	using U = std::remove_cvref_t<T>;

	if constexpr (std::is_same_v<U, bool>) {
		if (!value.is_boolean()) {
			error = "expected a boolean, got " + DescribeJsonValue(value);
			return false;
		}
		out = value.get<bool>();
		normalized = out;
		return true;
	} else if constexpr (std::is_integral_v<U>) {
		return DecodeIntegral<U>(value, out, normalized, error);
	} else if constexpr (std::is_enum_v<U>) {
		using Base = std::underlying_type_t<U>;
		Base decoded{};
		if (!DecodeArg<Base>(value, decoded, normalized, error)) return false;
		out = static_cast<U>(decoded);
		return true;
	} else if constexpr (EnumBitSetInfo<U>::is) {
		return DecodeBitSet<U>(value, out, normalized, error);
	} else if constexpr (std::is_same_v<U, std::string>) {
		if (!value.is_string()) {
			error = "expected a string, got " + DescribeJsonValue(value);
			return false;
		}
		out = value.get<std::string>();
		normalized = out;
		return true;
	} else if constexpr (StdVectorInfo<U>::is) {
		if (!value.is_array()) {
			error = "expected an array, got " + DescribeJsonValue(value);
			return false;
		}
		using Value = typename StdVectorInfo<U>::Value;
		out.clear();
		Json elements = Json::array();
		for (const Json &item : value) {
			Value element{};
			Json element_normalized;
			if (!DecodeArg(item, element, element_normalized, error)) return false;
			out.push_back(std::move(element));
			elements.push_back(std::move(element_normalized));
		}
		normalized = std::move(elements);
		return true;
	} else if constexpr (StdArrayInfo<U>::is) {
		if (!value.is_array() || value.size() != StdArrayInfo<U>::size) {
			error = "expected an array of " + fmt::format("{}", StdArrayInfo<U>::size) + " elements, got " + DescribeJsonValue(value);
			return false;
		}
		using Value = typename StdArrayInfo<U>::Value;
		Json elements = Json::array();
		bool ok = true;
		size_t index = 0;
		for (const Json &item : value) {
			Value element{};
			Json element_normalized;
			ok = ok && DecodeArg(item, element, element_normalized, error);
			if (!ok) return false;
			out[index++] = std::move(element);
			elements.push_back(std::move(element_normalized));
		}
		normalized = std::move(elements);
		return true;
	} else if constexpr (StdTupleInfo<U>::is) {
		constexpr size_t count = std::tuple_size_v<U>;
		if (!value.is_array() || value.size() != count) {
			error = "expected an array of " + fmt::format("{}", count) + " elements, got " + DescribeJsonValue(value);
			return false;
		}
		Json elements = Json::array();
		bool ok = true;
		DecodeTupleElements(value, out, elements, error, ok, std::make_index_sequence<count>{});
		if (!ok) return false;
		normalized = std::move(elements);
		return true;
	} else if constexpr (HasBaseType<U>::value) {
		using Base = typename HasBaseType<U>::Base;
		Base decoded{};
		if (!DecodeArg<Base>(value, decoded, normalized, error)) return false;
		out = U{decoded};
		return true;
	} else {
		error = "argument type " + TypeName<U>() + " has no JSON decoding";
		return false;
	}
}

/**
 * Decode the elements of a tuple argument.
 * @param value The JSON array to decode.
 * @param out Receives the decoded tuple.
 * @param elements Receives the normalized elements, already sized to the tuple.
 * @param error Receives the reason when a value is rejected.
 * @param ok Set to false when a value is rejected.
 */
template <typename Ttuple, size_t... I>
void DecodeTupleElements(const Json &value, Ttuple &out, Json &elements, std::string &error, bool &ok, std::index_sequence<I...>)
{
	Json element;
	(((ok = ok && DecodeArgAt(value, I, std::get<I>(out), element, error)), elements.push_back(std::move(element))), ...);
}

/**
 * Decode one command argument at its position in the argument array.
 * @param args The JSON array of arguments.
 * @param index The position of the argument.
 * @param out Receives the decoded argument.
 * @param normalized Receives the argument as the catalog reports it back.
 * @param error Receives the reason when the value is rejected, prefixed with the position.
 * @return True when the value was decoded.
 */
template <typename T>
bool DecodeArgAt(const Json &args, size_t index, T &out, Json &normalized, std::string &error)
{
	std::string detail;
	if (!DecodeArg(args[index], out, normalized, detail)) {
		error = "argument " + fmt::format("{}", index) + ": " + detail;
		return false;
	}
	return true;
}

/**
 * Decode the ordered arguments of a command.
 * @param args The JSON array of arguments.
 * @param out Receives the decoded arguments.
 * @param normalized Receives the arguments as the catalog reports them back.
 * @param error Receives the reason when the arguments are rejected.
 * @return True when the arguments were decoded.
 */
template <typename Ttuple, size_t... I>
bool DecodeArgsAt(const Json &args, Ttuple &out, Json &normalized, std::string &error, std::index_sequence<I...>)
{
	normalized = Json::array();
	bool ok = true;
	std::string detail;
	Json element;
	(((ok = ok && DecodeArgAt(args, I, std::get<I>(out), element, detail)), normalized.push_back(std::move(element))), ...);
	if (!ok) error = detail;
	return ok;
}

/**
 * Check the shape of the argument array and decode it.
 * @param args The JSON array of arguments.
 * @param out Receives the decoded arguments.
 * @param normalized Receives the arguments as the catalog reports them back.
 * @param error Receives the reason when the arguments are rejected.
 * @return True when the arguments were decoded.
 */
template <typename Ttuple>
bool DecodeArgs(const Json &args, Ttuple &out, Json &normalized, std::string &error)
{
	constexpr size_t count = std::tuple_size_v<Ttuple>;
	if (!args.is_array()) {
		error = "command arguments must be a JSON array, got " + DescribeJsonValue(args);
		return false;
	}
	if (args.size() != count) {
		error = "command takes " + fmt::format("{}", count) + " argument(s), got " + fmt::format("{}", args.size());
		return false;
	}
	return DecodeArgsAt(args, out, normalized, error, std::make_index_sequence<count>{});
}

/* -- catalog metadata -------------------------------------------------------------------- */

/** The catalog name of a command type. */
std::string_view CommandTypeName(CommandType type)
{
	static constexpr std::string_view names[] = {
		"LandscapeConstruction", "VehicleConstruction", "MoneyManagement", "VehicleManagement",
		"RouteManagement", "OtherManagement", "CompanySetting", "ServerSetting", "Cheat",
	};
	static_assert(std::size(names) == to_underlying(CommandType::End));
	const size_t index = to_underlying(type);
	return index < std::size(names) ? names[index] : "Unknown";
}

/** One command flag and the name the catalog uses for it. */
struct CommandFlagName {
	CommandFlag flag; ///< The flag.
	std::string_view name; ///< The catalog name of the flag.
};

/** All command flags, in declaration order. */
constexpr CommandFlagName _command_flag_names[] = {
	{CommandFlag::Server, "Server"},
	{CommandFlag::Spectator, "Spectator"},
	{CommandFlag::Offline, "Offline"},
	{CommandFlag::Auto, "Auto"},
	{CommandFlag::AllTiles, "AllTiles"},
	{CommandFlag::NoTest, "NoTest"},
	{CommandFlag::NoWater, "NoWater"},
	{CommandFlag::ClientID, "ClientID"},
	{CommandFlag::Deity, "Deity"},
	{CommandFlag::StrCtrl, "StrCtrl"},
	{CommandFlag::NoEst, "NoEst"},
	{CommandFlag::Location, "Location"},
};

/**
 * List the flags of a command.
 * @param flags The flags of the command.
 * @return The catalog names of the flags that are set.
 */
Json FlagsJson(CommandFlags flags)
{
	Json names = Json::array();
	for (const CommandFlagName &entry : _command_flag_names) {
		if (flags.Test(entry.flag)) names.push_back(std::string(entry.name));
	}
	return names;
}

/**
 * Describe which companies may issue a command, as far as the flags say.
 * @param flags The flags of the command.
 * @return The company constraints of the command.
 */
Json CompanyConstraints(CommandFlags flags)
{
	return {
		{"server_only", flags.Test(CommandFlag::Server)},
		{"spectator_allowed", flags.Test(CommandFlag::Spectator)},
		{"deity_allowed", flags.Test(CommandFlag::Deity)},
		{"offline_only", flags.Test(CommandFlag::Offline)},
		{"needs_client_id", flags.Test(CommandFlag::ClientID)},
	};
}

/**
 * The command name without the `Cmd` prefix.
 * @param name The proc name of the command.
 * @return The alias the catalog also accepts.
 */
std::string CommandAlias(std::string_view name)
{
	if (name.starts_with("Cmd")) name.remove_prefix(3);
	return std::string(name);
}

/**
 * Why a command's arguments cannot be decoded yet.
 * @return A description of the arguments that have no JSON decoding, empty when all are supported.
 */
template <typename... Ts>
std::string UnsupportedArgsReason(const std::tuple<Ts...> *)
{
	std::string reason;
	size_t index = 0;
	const auto add = [&reason](size_t position, const std::string &type) {
		if (!reason.empty()) reason += "; ";
		reason += "argument " + fmt::format("{}", position) + " is " + type + " which has no JSON decoding yet";
	};
	(((IsDecodableArg<Ts> ? void() : add(index, TypeName<Ts>())), ++index), ...);
	return reason;
}

/**
 * Describe the arguments of a command.
 * @param out Receives one entry per argument, in call order.
 */
template <typename... Ts, size_t... I>
void DescribeArgs(const std::tuple<Ts...> *, Json &out, std::index_sequence<I...>)
{
	((out.push_back(Json{
		{"index", I},
		{"name", nullptr},
		{"type", TypeName<Ts>()},
		{"kind", std::string(ArgKind<Ts>())},
		{"supported", IsDecodableArg<Ts>},
	})), ...);
}

/**
 * List the extra values a command proc returns.
 * @return One entry per extra return value, empty when the proc only returns a CommandCost.
 */
template <typename... Ts>
Json ReturnNames(const std::tuple<Ts...> *)
{
	Json names = Json::array();
	(names.push_back(TypeName<Ts>()), ...);
	return names;
}

/**
 * Build the catalog entry of a command.
 * @param full Whether to include the argument schema and the return values.
 * @return The catalog entry.
 */
template <Commands Tcmd>
Json DescribeCommand(bool full);

/**
 * Dispatch one command from JSON arguments.
 * @param args The JSON array of arguments.
 * @param location Tile for commands that take their location outside their arguments.
 * @param dry_run Whether to only ask for the cost.
 * @param cost Receives the actual result of the command.
 * @param normalized Receives the arguments as the catalog reports them back.
 * @param error Receives the reason when the arguments are rejected or the engine reported nothing.
 * @param error_code Receives the error code that fits the reason.
 * @return True when the command was posted (or estimated) and the engine reported a result.
 */
template <Commands Tcmd>
bool DispatchTypedCommand(const Json &args, TileIndex location, bool dry_run, CommandCost &cost, Json &normalized, std::string &error, const char *&error_code)
{
	typename CommandTraits<Tcmd>::Args values{};
	if (!DecodeArgs(args, values, normalized, error)) {
		error_code = "invalid_arguments";
		return false;
	}

	if (dry_run) {
		cost = ExtractCommandCost(std::apply([](auto... arguments) {
			return Command<Tcmd>::Do(DoCommandFlag::QueryCost, arguments...);
		}, values));
		return true;
	}

	_posting = {};
	if constexpr (CommandTraits<Tcmd>::flags.Test(CommandFlag::Location)) {
		std::apply([location](auto... arguments) {
			Command<Tcmd>::Post((StringID)0, &AgentCommandCatalogCallback, location, arguments...);
		}, values);
	} else {
		std::apply([](auto... arguments) {
			Command<Tcmd>::Post((StringID)0, &AgentCommandCatalogCallback, arguments...);
		}, values);
	}
	if (!_posting.captured) {
		/* Post can refuse the command before the proc runs, e.g. when the game is paused for it. */
		error = "the engine did not run the command";
		error_code = "command_not_run";
		return false;
	}
	cost = _posting.cost;
	return true;
}

/** Signature of the per-command argument decoder. */
using DecodeProc = bool (*)(const Json &args, Json &normalized, std::string &error);

/** Signature of the per-command dispatch function. */
using DispatchProc = bool (*)(const Json &args, TileIndex location, bool dry_run, CommandCost &cost, Json &normalized, std::string &error, const char *&error_code);

/** The dispatch entry of a command, or nullptr when its arguments have no JSON decoding. */
template <Commands Tcmd>
constexpr DispatchProc MakeDispatchProc()
{
	if constexpr (ArgsDecodable<typename CommandTraits<Tcmd>::Args>::value) {
		return &DispatchTypedCommand<Tcmd>;
	} else {
		return nullptr;
	}
}

/**
 * Decode the arguments of one command without running it.
 * @param args The JSON array of arguments.
 * @param normalized Receives the arguments as the catalog reports them back.
 * @param error Receives the reason when the arguments are rejected.
 * @return True when the arguments were decoded.
 */
template <Commands Tcmd>
bool DecodeTypedCommand(const Json &args, Json &normalized, std::string &error)
{
	typename CommandTraits<Tcmd>::Args values{};
	return DecodeArgs(args, values, normalized, error);
}

/** The argument decoder of a command, or nullptr when its arguments have no JSON decoding. */
template <Commands Tcmd>
constexpr DecodeProc MakeDecodeProc()
{
	if constexpr (ArgsDecodable<typename CommandTraits<Tcmd>::Args>::value) {
		return &DecodeTypedCommand<Tcmd>;
	} else {
		return nullptr;
	}
}

/** The catalog and dispatch operations of one command. */
struct CommandOps {
	Json (*describe)(bool full); ///< Build the catalog entry of the command.
	DecodeProc decode; ///< Decode the arguments of the command, or nullptr when it is a gap.
	DispatchProc dispatch; ///< Run the command, or nullptr when it is a gap.
};

/** Build the per-command table from the engine's own command traits. */
template <typename T, T... i>
constexpr auto MakeCommandOps(std::integer_sequence<T, i...>)
{
	return EnumIndexArray<CommandOps, Commands, static_cast<Commands>(sizeof...(i))>{{{
		{ &DescribeCommand<static_cast<Commands>(i)>, MakeDecodeProc<static_cast<Commands>(i)>(), MakeDispatchProc<static_cast<Commands>(i)>() }...
	}}};
}

/** Catalog and dispatch operations of every engine command, indexed by `Commands`. */
constexpr auto _command_ops = MakeCommandOps(std::make_integer_sequence<std::underlying_type_t<Commands>, to_underlying(Commands::End)>{});

/* -- command description ----------------------------------------------------------------- */

/** @copydoc DescribeCommand */
template <Commands Tcmd>
Json DescribeCommand(bool full)
{
	using Traits = CommandTraits<Tcmd>;
	const bool supported = MakeDispatchProc<Tcmd>() != nullptr;

	Json entry = Json::object();
	entry["id"] = to_underlying(Tcmd);
	entry["name"] = std::string(Traits::name);
	entry["alias"] = CommandAlias(Traits::name);
	entry["type"] = std::string(CommandTypeName(Traits::type));
	entry["flags"] = FlagsJson(Traits::flags);
	entry["location_argument"] = Traits::flags.Test(CommandFlag::Location);
	entry["pause"] = {{"allowed_when_paused", IsCommandAllowedWhilePaused(Tcmd)}};
	entry["company"] = CompanyConstraints(Traits::flags);

	Json support = {{"supported", supported}, {"status", supported ? "dispatchable" : "gap"}};
	if (!supported) support["reason"] = UnsupportedArgsReason(static_cast<const typename Traits::Args *>(nullptr));
	entry["support"] = std::move(support);

	if (full) {
		Json arguments = Json::array();
		DescribeArgs(static_cast<const typename Traits::Args *>(nullptr), arguments, std::make_index_sequence<std::tuple_size_v<typename Traits::Args>>{});
		entry["arguments"] = std::move(arguments);

		using RetTypes = typename Traits::RetTypes;
		if constexpr (std::is_void_v<RetTypes>) {
			entry["returns"] = Json::array();
		} else {
			entry["returns"] = ReturnNames(static_cast<const RetTypes *>(nullptr));
		}
	}

	return entry;
}

/* -- name lookup ------------------------------------------------------------------------- */

/**
 * Lower case a name, so command names are matched case insensitively.
 * @param text The text to fold.
 * @return The folded text.
 */
std::string ToLower(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return text;
}

/**
 * The names the catalog accepts for a command, mapped onto the command.
 * @return Proc names and aliases, lower cased.
 */
const std::map<std::string, Commands> &CommandNames()
{
	static const std::map<std::string, Commands> names = [] {
		std::map<std::string, Commands> map;
		for (int i = 0; i < to_underlying(Commands::End); i++) {
			const Commands cmd = static_cast<Commands>(i);
			const std::string name(GetCommandName(cmd));
			map[ToLower(name)] = cmd;
			map[ToLower(CommandAlias(name))] = cmd;
		}
		return map;
	}();
	return names;
}

/**
 * Find the command a request names, by id, proc name or alias.
 * @param value The `command` field of the request.
 * @return The command, or nothing when the field does not name one.
 */
std::optional<Commands> FindCommand(const Json &value)
{
	if (value.is_number_unsigned() || value.is_number_integer()) {
		if (value.is_number_integer() && value.get<int64_t>() < 0) return std::nullopt;
		const uint64_t id = value.get<uint64_t>();
		if (id >= static_cast<uint64_t>(to_underlying(Commands::End))) return std::nullopt;
		return static_cast<Commands>(id);
	}
	if (value.is_string()) {
		const auto it = CommandNames().find(ToLower(value.get<std::string>()));
		if (it != CommandNames().end()) return it->second;
	}
	return std::nullopt;
}

/* -- results ----------------------------------------------------------------------------- */

/**
 * Build a failed tool result.
 * @param code The machine readable error code.
 * @param message Human readable message.
 * @param detail Optional structured detail.
 * @return Tool result with `ok` set to false.
 */
Json MakeError(std::string_view code, std::string_view message, Json detail = Json::object())
{
	Json result = Json::object();
	result["ok"] = false;
	result["error"] = std::string(code);
	result["message"] = std::string(message);
	if (!detail.empty()) result["detail"] = std::move(detail);
	return result;
}

/**
 * Switch the company that pays and owns the result of a command, and restore the previous company
 * when the call returns.
 */
class CompanyContext {
public:
	explicit CompanyContext(CompanyID company) : backup(_current_company, company) {}

private:
	Backup<CompanyID> backup; ///< Restores the previous company on destruction.
};

/* -- methods ----------------------------------------------------------------------------- */

/**
 * Read the command catalog.
 * @param params The method parameters.
 * @return The method result.
 */
Json ReadCommandCatalog(const Json &params)
{
	std::string op = "list";
	if (params.is_object() && params.contains("op")) {
		if (!params["op"].is_string()) return MakeError("invalid_params", "'op' must be a string");
		op = params["op"].get<std::string>();
	}

	if (op != "list" && op != "describe" && op != "validate") {
		return MakeError("unsupported_op", "world.commands supports op=list, op=describe and op=validate", {{"op", op}});
	}

	if (op == "validate") {
		if (!params.contains("command")) return MakeError("invalid_params", "op=validate needs the engine command in 'command'");
		const std::optional<Commands> cmd = FindCommand(params["command"]);
		if (!cmd) return MakeError("unknown_command", "the request does not name an engine command", {{"command", params["command"]}});

		Json args = Json::array();
		if (params.contains("args")) {
			if (!params["args"].is_array()) return MakeError("invalid_params", "'args' must be an array of the command's arguments");
			args = params["args"];
		}

		const DecodeProc decode = _command_ops[*cmd].decode;
		if (decode == nullptr) {
			return MakeError("unsupported_argument_types", "this command has arguments the generic decoder does not support yet", _command_ops[*cmd].describe(true)["support"]);
		}

		Json normalized = Json::array();
		std::string error;
		if (!decode(args, normalized, error)) {
			return MakeError("invalid_arguments", error, {{"command", std::string(GetCommandName(*cmd))}, {"id", to_underlying(*cmd)}});
		}
		return {
			{"ok", true},
			{"op", op},
			{"command", std::string(GetCommandName(*cmd))},
			{"id", to_underlying(*cmd)},
			{"arguments", std::move(normalized)},
		};
	}

	const bool full = op == "describe";
	const bool single = full && params.is_object() && params.contains("command");

	if (single) {
		const std::optional<Commands> cmd = FindCommand(params["command"]);
		if (!cmd) return MakeError("unknown_command", "the request does not name an engine command", {{"command", params["command"]}});
		Json commands = Json::array();
		commands.push_back(_command_ops[*cmd].describe(true));
		return {
			{"ok", true},
			{"op", op},
			{"count", 1},
			{"commands", std::move(commands)},
		};
	}

	Json commands = Json::array();
	size_t dispatchable = 0;
	for (int i = 0; i < to_underlying(Commands::End); i++) {
		Json entry = _command_ops[static_cast<Commands>(i)].describe(full);
		if (entry["support"]["supported"].get<bool>()) dispatchable++;
		commands.push_back(std::move(entry));
	}

	return {
		{"ok", true},
		{"op", op},
		{"count", commands.size()},
		{"total", static_cast<size_t>(to_underlying(Commands::End))},
		{"dispatchable", dispatchable},
		{"commands", std::move(commands)},
	};
}

/**
 * Run one engine command through the normal player command path.
 * @param params The method parameters.
 * @param company Company that pays and owns the result.
 * @return The method result.
 */
Json DispatchWorldCommand(const Json &params, CompanyID company)
{
	if (!params.is_object()) return MakeError("invalid_params", "world.command needs an object with 'command' and 'args'");
	if (!params.contains("command")) return MakeError("invalid_params", "world.command needs the engine command in 'command'");

	const std::optional<Commands> cmd = FindCommand(params["command"]);
	if (!cmd) return MakeError("unknown_command", "the request does not name an engine command", {{"command", params["command"]}});

	Json args = Json::array();
	if (params.contains("args")) {
		if (!params["args"].is_array()) return MakeError("invalid_params", "'args' must be an array of the command's arguments");
		args = params["args"];
	}

	bool dry_run = false;
	if (params.contains("dry_run")) {
		if (!params["dry_run"].is_boolean()) return MakeError("invalid_params", "'dry_run' must be a boolean");
		dry_run = params["dry_run"].get<bool>();
	}

	TileIndex location{};
	if (params.contains("location")) {
		uint32_t tile = 0;
		Json ignored;
		std::string error;
		if (!DecodeIntegral<uint32_t>(params["location"], tile, ignored, error)) {
			return MakeError("invalid_params", "'location' must be a tile index: " + error);
		}
		location = TileIndex{tile};
	}

	const DispatchProc dispatch = _command_ops[*cmd].dispatch;
	if (dispatch == nullptr) {
		return MakeError("unsupported_argument_types", "this command has arguments the generic decoder does not support yet", _command_ops[*cmd].describe(true)["support"]);
	}

	/* The company pays and owns the result, so it has to exist before anything is estimated or run. */
	if (!Company::IsValidID(company)) return MakeError("invalid_company", "company does not exist", {{"company_id", company.base()}});

	/* Post is the player command path: in a network game it only queues the command for the server,
	 * so the engine's own answer would not come back. Reject instead of guessing. */
	if (!dry_run && _networking) return MakeError("multiplayer_unsupported", "catalog commands run through the single-player command path only");

	CommandCost cost;
	Json normalized = Json::array();
	std::string error;
	const char *error_code = "invalid_arguments";
	{
		const CompanyContext context(company);
		if (!dispatch(args, location, dry_run, cost, normalized, error, error_code)) {
			return MakeError(error_code, error, {{"command", std::string(GetCommandName(*cmd))}, {"id", to_underlying(*cmd)}});
		}
	}

	Json result = {
		{"ok", cost.Succeeded()},
		{"command", std::string(GetCommandName(*cmd))},
		{"id", to_underlying(*cmd)},
		{"dry_run", dry_run},
		{"cost", static_cast<int64_t>(cost.GetCost())},
		{"expense_type", to_underlying(cost.GetExpensesType())},
		{"arguments", std::move(normalized)},
	};
	if (cost.Failed()) {
		result["error"] = "command_failed";
		result["message"] = "the engine rejected the command";
		result["message_id"] = cost.GetErrorMessage().base();
	}
	return result;
}

} // namespace

/**
 * Capture the actual cost of the command that was just posted.
 * @param cmd The command that ran.
 * @param result The actual cost, or the failure of the command.
 * @param tile Location the command was posted for.
 */
void AgentCommandCatalogCallback(Commands, const CommandCost &result, TileIndex)
{
	_posting.captured = true;
	_posting.cost = result;
}

Json CommandCatalogCapabilities()
{
	/* Derive the counts and the undecodable types from the engine's own table, so the document
	 * cannot claim more than the dispatcher accepts. */
	size_t total = 0;
	size_t dispatchable = 0;
	std::vector<std::string> unsupported_types;
	std::vector<Json> gaps;

	for (int i = 0; i < to_underlying(Commands::End); i++) {
		const Json entry = _command_ops[static_cast<Commands>(i)].describe(true);
		total++;
		if (entry["support"]["supported"].get<bool>()) {
			dispatchable++;
			continue;
		}
		for (const Json &argument : entry["arguments"]) {
			if (argument["supported"].get<bool>()) continue;
			const std::string type = argument["type"].get<std::string>();
			if (std::ranges::find(unsupported_types, type) == unsupported_types.end()) unsupported_types.push_back(type);
		}
		gaps.push_back({
			{"command", entry["name"]},
			{"id", entry["id"]},
			{"reason", entry["support"]["reason"]},
		});
	}

	Json methods = Json::array();
	methods.push_back({
		{"method", "world.commands"},
		{"op", "list"},
		{"access", "read"},
		{"description", "every engine command with its id, name, flags, pause and company constraints"},
	});
	methods.push_back({
		{"method", "world.commands"},
		{"op", "describe"},
		{"access", "read"},
		{"description", "the same, plus the ordered argument schema and the extra return values of one command or all of them"},
	});
	methods.push_back({
		{"method", "world.commands"},
		{"op", "validate"},
		{"access", "read"},
		{"description", "decode one command's arguments without running it, and report them as they were decoded"},
	});
	methods.push_back({
		{"method", "world.command"},
		{"access", "write"},
		{"description", "post one engine command through the normal player command path, optionally as a cost estimate only"},
		{"params", {
			{"command", "engine command name, alias or numeric id"},
			{"args", "ordered positional arguments, as op=describe reports them"},
			{"dry_run", "estimate the cost only, never execute"},
			{"location", "tile for commands whose location_argument is true"},
		}},
	});

	Json argument_kinds = Json::array({"boolean", "integer", "enum", "enum_bit_set", "strong_id", "string", "vector", "array", "tuple"});

	Json gap_list = Json::array();
	gap_list.push_back({
		{"gap", "named_arguments"},
		{"detail", "arguments are positional; naming them needs a durable generator that emits the header names of every command"},
	});
	gap_list.push_back({
		{"gap", "enum_value_validation"},
		{"detail", "enum arguments are range checked against the underlying integer type only; membership of the enum's own values is not checked"},
	});
	gap_list.push_back({
		{"gap", "complex_argument_types"},
		{"detail", "commands whose arguments are engine structs or pointers are catalogued but not dispatchable"},
		{"types", unsupported_types},
		{"commands", gaps},
	});
	gap_list.push_back({
		{"gap", "special_return_values"},
		{"detail", "the generic callback reports the actual CommandCost only; created ids from procs that return more than a CommandCost are not reported"},
	});
	gap_list.push_back({
		{"gap", "encoded_string_arguments"},
		{"detail", "EncodedString arguments need a StringID plus its parameters, which has no durable JSON form yet"},
	});
	gap_list.push_back({
		{"gap", "multiplayer"},
		{"detail", "writes use the single-player command path; a network game rejects them instead of queueing them"},
	});

	return {
		{"ok", true},
		{"methods", std::move(methods)},
		{"commands", {
			{"total", total},
			{"dispatchable", dispatchable},
			{"gaps", total - dispatchable},
			{"engine_end", static_cast<size_t>(to_underlying(Commands::End))},
		}},
		{"argument_kinds", std::move(argument_kinds)},
		{"gaps", std::move(gap_list)},
	};
}

std::optional<Json> DispatchCommandCatalog(std::string_view method, const Json &params, CompanyID company)
{
	if (method == "world.commands") return ReadCommandCatalog(params);
	if (method == "world.command") return DispatchWorldCommand(params, company);
	return std::nullopt;
}

} // namespace InGameAgent
