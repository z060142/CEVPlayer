// SHA-256 of a file (streaming, cancellable). Used for the source hash of an imported video (.cryasset Details).
#pragma once

#include <atomic>
#include <string>

namespace CEVEditor
{
	//! Lower case hex digest of the file at `utf8Path`. False when the file cannot be read (or `cancel` was set).
	bool Sha256File(const std::string& utf8Path, std::string& hexOut, const std::atomic<bool>* cancel = nullptr);
	//! Digest of a memory block (selftest / small data).
	std::string Sha256Hex(const void* data, size_t size);
}
