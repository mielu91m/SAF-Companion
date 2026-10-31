#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <Windows.h>

namespace MCF
{
	template <typename T>
	class simple_array
	{
	public:
		simple_array() {}

		simple_array(std::vector<T>& a_dataSource) {
			data = a_dataSource.data();
			count = a_dataSource.size();
		}

		std::uint64_t size() const {
			return count;
		}

		T& operator[](std::uint64_t a_idx) {
			return data[a_idx];
		}

		const T& operator[](std::uint64_t a_idx) const {
			return data[a_idx];
		}
	private:
		T* data = nullptr;
		std::uint64_t count = 0;
	};

	class simple_string_view
	{
	public:
		inline simple_string_view() {}

		inline simple_string_view(const char* a_begin, std::uint64_t a_size) {
			data = a_begin;
			size = a_size;
		}

		inline simple_string_view(const std::string_view& a_sv) {
			data = a_sv.data();
			size = a_sv.size();
		}

		inline simple_string_view(const char* a_str) {
			data = a_str;
			size = a_str ? std::char_traits<char>::length(a_str) : 0;
		}

		inline simple_string_view(const std::string& a_str) {
			data = a_str.data();
			size = a_str.size();
		}

		inline std::string_view get() const {
			return std::string_view(data, size);
		}

		inline operator std::string_view() const {
			return get();
		}

		const char* data = nullptr;
		std::uint64_t size = 0;
	};

	class ConsoleInterface
	{
	public:
		virtual RE::NiPointer<RE::TESObjectREFR> GetSelectedReference() = 0;
		virtual RE::TESForm* HexStrToForm(const simple_string_view& a_str) = 0;
		virtual void PrintLn(const simple_string_view& a_txt) = 0;
		virtual void PreventDefaultPrint() = 0;
	};

	typedef void (*CommandCallback)(const simple_array<simple_string_view>& a_args, const char* a_fullString, ConsoleInterface* a_intfc);

	namespace detail
	{
		typedef void (*RegCommand_Def)(const char* a_name, MCF::CommandCallback a_func);
	}

	inline bool RegisterCommand(const char* a_name, CommandCallback a_func) {
		const auto hndl = GetModuleHandleA("ModernCommandFramework.dll");
		if (hndl != NULL) {
			const auto addr = GetProcAddress(hndl, "RegisterCommand");
			if (addr != NULL) {
				(reinterpret_cast<detail::RegCommand_Def>(addr))(a_name, a_func);
				return true;
			}
		}
		return false;
	}
}
