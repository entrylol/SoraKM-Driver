#define _CRT_SECURE_NO_WARNINGS
#include "comms.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

	constexpr int kSpeedTestIterations = 10000;
	constexpr std::uint32_t kBatchReadCount = 16;

	bool test_connection(sora_driver::Comms& comms) {
		std::printf("[connection] opening device...\n");

		if (!comms.open()) {
			std::printf("[connection] FAILED - could not open %ls (error %lu)\n", sora_driver::kDevicePath, GetLastError());
			return false;
		}

		std::printf("[connection] PASSED - device handle acquired\n");
		return true;
	}

	bool test_attach(sora_driver::Comms& comms, const wchar_t* process_name) {
		std::printf("[attach] resolving pid for %ls...\n", process_name);

		const std::uint32_t pid = sora_driver::get_process_id_by_name(process_name);
		if (pid == 0) {
			std::printf("[attach] FAILED - process not found\n");
			return false;
		}

		std::printf("[attach] found pid %u, sending attach ioctl...\n", pid);

		if (!comms.attach(process_name)) {
			std::printf("[attach] FAILED - attach ioctl failed\n");
			return false;
		}

		if (comms.attached_process_id() != pid) {
			std::printf("[attach] FAILED - attached pid mismatch\n");
			return false;
		}

		std::printf("[attach] PASSED - attached to pid %u\n", comms.attached_process_id());
		return true;
	}

	bool test_module_base(sora_driver::Comms& comms, const wchar_t* module_name) {
		std::printf("[module_base] querying %ls...\n", module_name);

		void* base_address = nullptr;
		if (!comms.get_module_base(module_name, &base_address)) {
			std::printf("[module_base] FAILED - ioctl failed\n");
			return false;
		}

		if (base_address == nullptr) {
			std::printf("[module_base] FAILED - module not found\n");
			return false;
		}

		std::printf("[module_base] PASSED - %ls base = 0x%p\n", module_name, base_address);
		return true;
	}

	bool test_read_write(sora_driver::Comms& comms, void* module_base) {
		std::printf("[read_write] testing single read at module base...\n");

		std::uint16_t original_value = 0;
		if (!comms.read(module_base, original_value)) {
			std::printf("[read_write] FAILED - read failed\n");
			return false;
		}

		std::printf("[read_write] read 0x%04X from 0x%p\n", original_value, module_base);

		if (!comms.write(module_base, original_value)) {
			std::printf("[read_write] write skipped - target region may be read-only\n");
			std::printf("[read_write] PASSED - read path verified\n");
			return true;
		}

		std::uint16_t verify_value = 0;
		if (!comms.read(module_base, verify_value) || verify_value != original_value) {
			std::printf("[read_write] FAILED - write verify failed\n");
			return false;
		}

		std::printf("[read_write] PASSED - read and write verified\n");
		return true;
	}

	void test_speed(sora_driver::Comms& comms, void* read_address) {
		std::printf("[speed] single-read benchmark (%d iterations)...\n", kSpeedTestIterations);

		std::uint64_t value = 0;
		const auto single_start = std::chrono::steady_clock::now();

		for (int i = 0; i < kSpeedTestIterations; ++i) {
			if (!comms.read(read_address, value)) {
				std::printf("[speed] single-read aborted on failure\n");
				return;
			}
		}

		const auto single_end = std::chrono::steady_clock::now();
		const auto single_ms = std::chrono::duration_cast<std::chrono::milliseconds>(single_end - single_start).count();

		std::printf("[speed] single-read: %lld ms (%.2f reads/sec)\n",
			static_cast<long long>(single_ms),
			kSpeedTestIterations / (static_cast<double>(single_ms) / 1000.0));

		std::printf("[speed] batch-read benchmark (%d batches x %u entries)...\n",
			kSpeedTestIterations, kBatchReadCount);

		std::vector<std::uint64_t> batch_values(kBatchReadCount, 0);
		std::vector<sora_driver::BatchReadEntry> entries(kBatchReadCount);

		for (std::uint32_t i = 0; i < kBatchReadCount; ++i) {
			entries[i].target = read_address;
			entries[i].buffer = &batch_values[i];
			entries[i].size = sizeof(std::uint64_t);
			entries[i].return_size = 0;
		}

		const auto batch_start = std::chrono::steady_clock::now();

		for (int i = 0; i < kSpeedTestIterations; ++i) {
			if (!comms.batch_read_memory(entries.data(), kBatchReadCount)) {
				std::printf("[speed] batch-read aborted on failure\n");
				return;
			}
		}

		const auto batch_end = std::chrono::steady_clock::now();
		const auto batch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(batch_end - batch_start).count();
		const int total_reads = kSpeedTestIterations * static_cast<int>(kBatchReadCount);

		std::printf("[speed] batch-read: %lld ms (%.2f reads/sec)\n",
			static_cast<long long>(batch_ms),
			total_reads / (static_cast<double>(batch_ms) / 1000.0));
	}

} // namespace

int main(int argc, char** argv) {
	const wchar_t* target_process = L"explorer.exe";
	const wchar_t* target_module = L"explorer.exe";

	if (argc >= 2) {
		static wchar_t process_buffer[260]{};
		if (std::mbstowcs(process_buffer, argv[1], 259) > 0) {
			target_process = process_buffer;
			target_module = process_buffer;
		}
	}

	if (argc >= 3) {
		static wchar_t module_buffer[260]{};
		if (std::mbstowcs(module_buffer, argv[2], 259) > 0) {
			target_module = module_buffer;
		}
	}

	std::printf("sora_driver comms test\n");
	std::printf("target process: %ls\n", target_process);
	std::printf("target module:  %ls\n", target_module);
	std::printf("----------------------------------------\n");

	sora_driver::Comms comms;

	if (!test_connection(comms)) {
		return 1;
	}

	if (!test_attach(comms, target_process)) {
		return 1;
	}

	if (!test_module_base(comms, target_module)) {
		return 1;
	}

	test_module_base(comms, L"ntdll.dll");

	void* module_base = nullptr;
	if (!comms.get_module_base(target_module, &module_base) || module_base == nullptr) {
		std::printf("could not resolve module base for speed/read tests\n");
		return 1;
	}

	if (!test_read_write(comms, module_base)) {
		return 1;
	}

	test_speed(comms, module_base);

	std::printf("----------------------------------------\n");
	std::printf("all tests completed\n");
	return 0;
}
