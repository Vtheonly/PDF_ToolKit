#pragma once

namespace pdftoolkit {

/// Native core version (independent of the Python engine's version).
/// The Python engine (2.0.0) stays authoritative until parity (AGENTS.md
/// §5.6); the native core counts from 0.1.0.
[[nodiscard]] const char* native_version() noexcept;

}  // namespace pdftoolkit
