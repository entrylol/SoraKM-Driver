#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>

namespace sora_driver {

	inline constexpr wchar_t kDevicePath[] = L"\\\\.\\sorakmv1";

	namespace codes {
		inline constexpr std::uint32_t attach =
			CTL_CODE(FILE_DEVICE_UNKNOWN, 0x696, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);

		inline constexpr std::uint32_t read =
			CTL_CODE(FILE_DEVICE_UNKNOWN, 0x697, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);

		inline constexpr std::uint32_t write =
			CTL_CODE(FILE_DEVICE_UNKNOWN, 0x698, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);

		inline constexpr std::uint32_t batch_read =
			CTL_CODE(FILE_DEVICE_UNKNOWN, 0x699, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);

		inline constexpr std::uint32_t get_module_base =
			CTL_CODE(FILE_DEVICE_UNKNOWN, 0x69A, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);
	} // namespace codes

	inline constexpr std::uint32_t kMaxBatchReadEntries = 64;
	inline constexpr std::uint32_t kMaxModuleNameLength = 260;

	struct Request {
		void* process_id;

		void* target;
		void* buffer;

		std::size_t size;
		std::size_t return_size;
	};

	struct BatchReadEntry {
		void* target;
		void* buffer;
		std::size_t size;
		std::size_t return_size;
	};

	struct BatchReadRequest {
		std::uint32_t count;
		BatchReadEntry entries[kMaxBatchReadEntries];
	};

	struct ModuleRequest {
		wchar_t module_name[kMaxModuleNameLength];
		void* base_address;
	};

	std::uint32_t get_process_id_by_name(const wchar_t* process_name);

	class Comms {
	public:
		Comms() = default;
		~Comms();

		Comms(const Comms&) = delete;
		Comms& operator=(const Comms&) = delete;

		bool open();
		void close();
		bool is_open() const;

		bool attach(void* process_id);
		bool attach(const wchar_t* process_name);
		bool read_memory(void* target, void* buffer, std::size_t size, std::size_t* bytes_read = nullptr);
		bool write_memory(void* target, const void* buffer, std::size_t size, std::size_t* bytes_written = nullptr);
		bool batch_read_memory(BatchReadEntry* entries, std::uint32_t count);
		bool get_module_base(const wchar_t* module_name, void** base_address);

		template <typename T>
		bool read(void* address, T& value) {
			return read_memory(address, &value, sizeof(T));
		}

		template <typename T>
		bool write(void* address, const T& value) {
			return write_memory(address, &value, sizeof(T));
		}

		std::uint32_t attached_process_id() const {
			return attached_process_id_;
		}

	private:
		void* handle_ = nullptr;
		std::uint32_t attached_process_id_ = 0;
	};

} // namespace sora_driver
