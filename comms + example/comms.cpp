#include "comms.h"

#include <TlHelp32.h>

namespace sora_driver {

	std::uint32_t get_process_id_by_name(const wchar_t* process_name) {
		if (process_name == nullptr || *process_name == L'\0') {
			return 0;
		}

		const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE) {
			return 0;
		}

		PROCESSENTRY32W entry{};
		entry.dwSize = sizeof(entry);

		std::uint32_t process_id = 0;
		if (Process32FirstW(snapshot, &entry)) {
			do {
				if (_wcsicmp(entry.szExeFile, process_name) == 0) {
					process_id = entry.th32ProcessID;
					break;
				}
			} while (Process32NextW(snapshot, &entry));
		}

		CloseHandle(snapshot);
		return process_id;
	}

	Comms::~Comms() {
		close();
	}

	bool Comms::open() {
		if (is_open()) {
			return true;
		}

		handle_ = CreateFile(L"\\\\.\\sorakmv1", GENERIC_READ, 0, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);

		if (handle_ == INVALID_HANDLE_VALUE) {
			// Optionally log here if we wanted to be redundant, but main.cpp handles it.
			return false;
		}

		return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
	}

	void Comms::close() {
		if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
			CloseHandle(static_cast<HANDLE>(handle_));
		}

		handle_ = nullptr;
		attached_process_id_ = 0;
	}

	bool Comms::is_open() const {
		return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
	}

	bool Comms::attach(void* process_id) {
		if (!is_open() || process_id == nullptr) {
			return false;
		}

		Request request{};
		request.process_id = process_id;

		const BOOL ok = DeviceIoControl(
			static_cast<HANDLE>(handle_),
			codes::attach,
			&request,
			sizeof(request),
			&request,
			sizeof(request),
			nullptr,
			nullptr);

		if (ok != TRUE) {
			attached_process_id_ = 0;
			return false;
		}

		attached_process_id_ = static_cast<std::uint32_t>(
			reinterpret_cast<std::uintptr_t>(process_id));
		return true;
	}

	bool Comms::attach(const wchar_t* process_name) {
		const std::uint32_t process_id = get_process_id_by_name(process_name);
		if (process_id == 0) {
			return false;
		}

		return attach(reinterpret_cast<void*>(static_cast<std::uintptr_t>(process_id)));
	}

	bool Comms::read_memory(void* target, void* buffer, std::size_t size, std::size_t* bytes_read) {
		if (!is_open() || buffer == nullptr || size == 0) {
			return false;
		}

		Request request{};
		request.target = target;
		request.buffer = buffer;
		request.size = size;
		request.return_size = 0;

		const BOOL ok = DeviceIoControl(
			static_cast<HANDLE>(handle_),
			codes::read,
			&request,
			sizeof(request),
			&request,
			sizeof(request),
			nullptr,
			nullptr);

		if (ok != TRUE) {
			return false;
		}

		if (bytes_read != nullptr) {
			*bytes_read = request.return_size;
		}

		return true;
	}

	bool Comms::write_memory(void* target, const void* buffer, std::size_t size, std::size_t* bytes_written) {
		if (!is_open() || buffer == nullptr || size == 0) {
			return false;
		}

		Request request{};
		request.target = target;
		request.buffer = const_cast<void*>(buffer);
		request.size = size;
		request.return_size = 0;

		const BOOL ok = DeviceIoControl(
			static_cast<HANDLE>(handle_),
			codes::write,
			&request,
			sizeof(request),
			&request,
			sizeof(request),
			nullptr,
			nullptr);

		if (ok != TRUE) {
			return false;
		}

		if (bytes_written != nullptr) {
			*bytes_written = request.return_size;
		}

		return true;
	}

	bool Comms::batch_read_memory(BatchReadEntry* entries, std::uint32_t count) {
		if (!is_open() || entries == nullptr || count == 0 || count > kMaxBatchReadEntries) {
			return false;
		}

		BatchReadRequest request{};
		request.count = count;

		for (std::uint32_t i = 0; i < count; ++i) {
			request.entries[i] = entries[i];
			request.entries[i].return_size = 0;
		}

		const DWORD buffer_length =
			static_cast<DWORD>(sizeof(std::uint32_t) + static_cast<std::size_t>(count) * sizeof(BatchReadEntry));

		const BOOL ok = DeviceIoControl(
			static_cast<HANDLE>(handle_),
			codes::batch_read,
			&request,
			buffer_length,
			&request,
			buffer_length,
			nullptr,
			nullptr);

		if (ok != TRUE) {
			return false;
		}

		for (std::uint32_t i = 0; i < count; ++i) {
			entries[i].return_size = request.entries[i].return_size;
		}

		return true;
	}

	bool Comms::get_module_base(const wchar_t* module_name, void** base_address) {
		if (!is_open() || module_name == nullptr || base_address == nullptr) {
			return false;
		}

		ModuleRequest request{};
		if (wcsncpy_s(request.module_name, module_name, kMaxModuleNameLength - 1) != 0) {
			return false;
		}

		request.base_address = nullptr;

		const BOOL ok = DeviceIoControl(
			static_cast<HANDLE>(handle_),
			codes::get_module_base,
			&request,
			sizeof(request),
			&request,
			sizeof(request),
			nullptr,
			nullptr);

		if (ok != TRUE) {
			return false;
		}

		*base_address = request.base_address;
		return request.base_address != nullptr;
	}

} // namespace sora_driver
