// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace MCPBridge
{
	void Start();
	void OnUIReady();
	void Stop();
	void OnVMStarting();
	void OnGameChanged();
	void OnVMPaused(int breakpoint_cpu, bool stepping);
	void OnVMDestroyed();
} // namespace MCPBridge
