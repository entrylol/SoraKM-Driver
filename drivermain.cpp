#include <ntifs.h>

extern "C" {
	NTKERNELAPI NTSTATUS IoCreateDriver(PUNICODE_STRING DriverName,
		PDRIVER_INITIALIZE InitializationFunction);

	NTKERNELAPI NTSTATUS MmCopyVirtualMemory(PEPROCESS SourceProcess, PVOID SourceAddress,
		PEPROCESS TargetProcess, PVOID TargetAddress,
		SIZE_T BufferSize, KPROCESSOR_MODE PreviousMode,
		PSIZE_T ReturnSize);
}

void debug_print(PCSTR text) {
}

namespace driver {
	namespace codes {
		// Used to setup the driver.
		constexpr ULONG attach =
			CTL_CODE(FILE_DEVICE_UNKNOWN, 0x696, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);

		// Read process memory.
		constexpr ULONG read =
			CTL_CODE(FILE_DEVICE_UNKNOWN, 0x697, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);

		// Write process memory.
		constexpr ULONG write =
			CTL_CODE(FILE_DEVICE_UNKNOWN, 0x698, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);

		// Read multiple process memory regions in one IOCTL.
		constexpr ULONG batch_read =
			CTL_CODE(FILE_DEVICE_UNKNOWN, 0x699, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);

		// Get base address of a loaded module in the attached process.
		constexpr ULONG get_module_base =
			CTL_CODE(FILE_DEVICE_UNKNOWN, 0x69A, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);
	}	// namespace codes

	constexpr ULONG kMaxBatchReadEntries = 64;
	constexpr ULONG kMaxModuleNameLength = 260;

	// Shared between user mode & kernel mode.
	struct Request {
		HANDLE process_id;

		PVOID target;
		PVOID buffer;

		SIZE_T size;
		SIZE_T return_size;
	};

	struct BatchReadEntry {
		PVOID target;
		PVOID buffer;
		SIZE_T size;
		SIZE_T return_size;
	};

	struct BatchReadRequest {
		ULONG count;
		BatchReadEntry entries[kMaxBatchReadEntries];
	};

	struct ModuleRequest {
		WCHAR module_name[kMaxModuleNameLength];
		PVOID base_address;
	};

	// EPROCESS.Peb offset for Win10 2004+ / Win11 x64.
	constexpr ULONG kEprocessPebOffset = 0x550;

	PVOID get_process_peb(PEPROCESS process) {
		return *reinterpret_cast<PVOID*>(reinterpret_cast<PUCHAR>(process) + kEprocessPebOffset);
	}

	NTSTATUS copy_from_process(PEPROCESS process, PVOID source, PVOID destination, SIZE_T size,
		PSIZE_T bytes_copied) {
		return MmCopyVirtualMemory(process, source, PsGetCurrentProcess(), destination, size,
			KernelMode, bytes_copied);
	}

	NTSTATUS get_module_base_address(PEPROCESS process, PCWSTR module_name, PVOID* base_address) {
		if (process == nullptr || module_name == nullptr || base_address == nullptr) {
			return STATUS_INVALID_PARAMETER;
		}

		*base_address = nullptr;

		const PVOID peb = get_process_peb(process);
		if (peb == nullptr) {
			return STATUS_UNSUCCESSFUL;
		}

		PVOID ldr = nullptr;
		SIZE_T bytes = 0;
		NTSTATUS status = copy_from_process(process,
			reinterpret_cast<PVOID>(reinterpret_cast<PUCHAR>(peb) + 0x18),
			&ldr, sizeof(ldr), &bytes);
		if (status != STATUS_SUCCESS || ldr == nullptr) {
			return status != STATUS_SUCCESS ? status : STATUS_UNSUCCESSFUL;
		}

		LIST_ENTRY list_head{};
		status = copy_from_process(process,
			reinterpret_cast<PVOID>(reinterpret_cast<PUCHAR>(ldr) + 0x10),
			&list_head, sizeof(list_head), &bytes);
		if (status != STATUS_SUCCESS) {
			return status;
		}

		UNICODE_STRING target_name{};
		RtlInitUnicodeString(&target_name, module_name);

		PVOID current_entry = list_head.Flink;
		const PVOID list_head_address =
			reinterpret_cast<PVOID>(reinterpret_cast<PUCHAR>(ldr) + 0x10);

		for (ULONG i = 0; i < 512 && current_entry != list_head_address; ++i) {
			PVOID dll_base = nullptr;
			status = copy_from_process(process,
				reinterpret_cast<PVOID>(reinterpret_cast<PUCHAR>(current_entry) + 0x30),
				&dll_base, sizeof(dll_base), &bytes);
			if (status != STATUS_SUCCESS) {
				return status;
			}

			UNICODE_STRING base_dll_name{};
			status = copy_from_process(process,
				reinterpret_cast<PVOID>(reinterpret_cast<PUCHAR>(current_entry) + 0x58),
				&base_dll_name, sizeof(base_dll_name), &bytes);
			if (status != STATUS_SUCCESS) {
				return status;
			}

			if (base_dll_name.Buffer != nullptr && base_dll_name.Length > 0 &&
				base_dll_name.Length < static_cast<USHORT>(kMaxModuleNameLength * sizeof(WCHAR))) {
				WCHAR name_buffer[kMaxModuleNameLength]{};
				const SIZE_T name_bytes = base_dll_name.Length;

				status = copy_from_process(process, base_dll_name.Buffer, name_buffer, name_bytes, &bytes);
				if (status == STATUS_SUCCESS) {
					UNICODE_STRING entry_name{};
					entry_name.Length = base_dll_name.Length;
					entry_name.MaximumLength = static_cast<USHORT>(name_bytes + sizeof(WCHAR));
					entry_name.Buffer = name_buffer;

					if (RtlCompareUnicodeString(&entry_name, &target_name, TRUE) == 0) {
						*base_address = dll_base;
						return STATUS_SUCCESS;
					}
				}
			}

			LIST_ENTRY entry_links{};
			status = copy_from_process(process, current_entry, &entry_links, sizeof(entry_links), &bytes);
			if (status != STATUS_SUCCESS) {
				return status;
			}

			current_entry = entry_links.Flink;
		}

		return STATUS_NOT_FOUND;
	}

	NTSTATUS create(PDEVICE_OBJECT device_object, PIRP irp) {
		UNREFERENCED_PARAMETER(device_object);

		irp->IoStatus.Status = STATUS_SUCCESS;
		irp->IoStatus.Information = 0;
		IoCompleteRequest(irp, IO_NO_INCREMENT);

		return STATUS_SUCCESS;
	}

	NTSTATUS close(PDEVICE_OBJECT device_object, PIRP irp) {
		UNREFERENCED_PARAMETER(device_object);

		irp->IoStatus.Status = STATUS_SUCCESS;
		irp->IoStatus.Information = 0;
		IoCompleteRequest(irp, IO_NO_INCREMENT);

		return STATUS_SUCCESS;
	}

	// Note: Todo
	NTSTATUS device_control(PDEVICE_OBJECT device_object, PIRP irp) {
		UNREFERENCED_PARAMETER(device_object);

		debug_print("[+] Device control called.\n");

		NTSTATUS status = STATUS_UNSUCCESSFUL;

		// We need this to determine which code was passed through.
		PIO_STACK_LOCATION stack_irp = IoGetCurrentIrpStackLocation(irp);

		// Access the request object sent from user mode.
		auto request = reinterpret_cast<Request*>(irp->AssociatedIrp.SystemBuffer);

		if (stack_irp == nullptr || request == nullptr) {
			IoCompleteRequest(irp, IO_NO_INCREMENT);
			return status;
		}

		// The target process we want access to.
		static PEPROCESS target_process = nullptr;

		const ULONG control_code = stack_irp->Parameters.DeviceIoControl.IoControlCode;
		switch (control_code) {
		case codes::attach:
			status = PsLookupProcessByProcessId(request->process_id, &target_process);
			break;

		case codes::read:
			if (target_process != nullptr)
				status = MmCopyVirtualMemory(target_process, request->target,
					PsGetCurrentProcess(), request->buffer,
					request->size, KernelMode, &request->return_size);
			break;

		case codes::write:
			if (target_process != nullptr)
				status = MmCopyVirtualMemory(PsGetCurrentProcess(), request->buffer,
					target_process, request->target,
					request->size, KernelMode, &request->return_size);
			break;

		case codes::batch_read: {
			auto batch_request = reinterpret_cast<BatchReadRequest*>(irp->AssociatedIrp.SystemBuffer);
			const ULONG input_length = stack_irp->Parameters.DeviceIoControl.InputBufferLength;

			if (batch_request == nullptr || target_process == nullptr) {
				status = STATUS_INVALID_PARAMETER;
				break;
			}

			if (batch_request->count == 0 || batch_request->count > kMaxBatchReadEntries) {
				status = STATUS_INVALID_PARAMETER;
				break;
			}

			const SIZE_T required_length =
				sizeof(ULONG) + static_cast<SIZE_T>(batch_request->count) * sizeof(BatchReadEntry);
			if (input_length < required_length) {
				status = STATUS_BUFFER_TOO_SMALL;
				break;
			}

			status = STATUS_SUCCESS;
			for (ULONG i = 0; i < batch_request->count; ++i) {
				auto& entry = batch_request->entries[i];
				entry.return_size = 0;

				const NTSTATUS entry_status = MmCopyVirtualMemory(
					target_process, entry.target,
					PsGetCurrentProcess(), entry.buffer,
					entry.size, KernelMode, &entry.return_size);

				if (entry_status != STATUS_SUCCESS) {
					status = entry_status;
				}
			}
			break;
		}

		case codes::get_module_base: {
			auto module_request = reinterpret_cast<ModuleRequest*>(irp->AssociatedIrp.SystemBuffer);
			if (module_request == nullptr || target_process == nullptr) {
				status = STATUS_INVALID_PARAMETER;
				break;
			}

			module_request->base_address = nullptr;
			status = get_module_base_address(target_process, module_request->module_name,
				&module_request->base_address);
			break;
		}

		default:
			break;
		}

		irp->IoStatus.Status = status;

		if (control_code == codes::batch_read) {
			const auto batch_request = reinterpret_cast<BatchReadRequest*>(irp->AssociatedIrp.SystemBuffer);
			if (batch_request != nullptr && batch_request->count > 0 &&
				batch_request->count <= kMaxBatchReadEntries) {
				irp->IoStatus.Information =
					sizeof(ULONG) + static_cast<ULONG>(batch_request->count) * sizeof(BatchReadEntry);
			} else {
				irp->IoStatus.Information = 0;
			}
		} else if (control_code == codes::get_module_base) {
			irp->IoStatus.Information = sizeof(ModuleRequest);
		} else {
			irp->IoStatus.Information = sizeof(Request);
		}

		IoCompleteRequest(irp, IO_NO_INCREMENT);

		return status;
	}

}	// namespace driver


// "Real" entry point.
NTSTATUS driver_main(PDRIVER_OBJECT driver_object, PUNICODE_STRING registry_path) {
	UNREFERENCED_PARAMETER(registry_path);

	UNICODE_STRING device_name = {};
	RtlInitUnicodeString(&device_name, L"\\Device\\sorakmv1");

	// Create driver device obj.
	PDEVICE_OBJECT device_object = nullptr;
	NTSTATUS status = IoCreateDevice(driver_object, 0, &device_name, FILE_DEVICE_UNKNOWN,
		FILE_DEVICE_SECURE_OPEN, FALSE, &device_object);

	if (status != STATUS_SUCCESS) {
		debug_print("[-] Failed to create driver device.\n");
		return status;
	}

	debug_print("[+] Driver device succesfully created.\n");

	UNICODE_STRING symbolic_link = {};
	RtlInitUnicodeString(&symbolic_link, L"\\??\\sorakmv1");

	status = IoCreateSymbolicLink(&symbolic_link, &device_name);
	if (status != STATUS_SUCCESS) {
		debug_print("[-] Failed to establish symboly link.\n");
		return status;
	}

	debug_print("[+] Driver symbolic link succesfully established.\n");

	// Allow us to send small amounts of data between um/km.
	SetFlag(device_object->Flags, DO_BUFFERED_IO);

	// set the driver handlers to our functions with our logic.
	driver_object->MajorFunction[IRP_MJ_CREATE] = driver::create;
	driver_object->MajorFunction[IRP_MJ_CLOSE] = driver::close;
	driver_object->MajorFunction[IRP_MJ_DEVICE_CONTROL] = driver::device_control;

	// we have initialized our device.
	ClearFlag(device_object->Flags, DO_DEVICE_INITIALIZING);

	debug_print("[+] Driver initialized succesfully.\n");

	return status;
}

// KdMapper will call this "entry point" but params will be null.
extern "C" NTSTATUS DriverEntry() {
	debug_print("[+] HelloWorld from the kernel!\n");

	UNICODE_STRING driver_name = {};
	RtlInitUnicodeString(&driver_name, L"\\Driver\\sorakmv1");

	return IoCreateDriver(&driver_name, &driver_main);
}