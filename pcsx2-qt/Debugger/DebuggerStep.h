// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "DebugTools/DebugInterface.h"
#include "DebugTools/MIPSAnalyst.h"

namespace DebuggerStep
{
	inline u32 Target(DebugInterface& cpu, bool over)
	{
		const u32 pc = cpu.getPC();
		const auto info = MIPSAnalyst::GetOpcodeInfo(&cpu, pc);
		if (info.isSyscall && !over)
			return info.branchTarget;
		if (!info.isBranch)
			return pc + 4;
		if (info.isConditional)
			return info.conditionMet ? info.branchTarget : pc + 8;
		if (over && info.isLinkedBranch)
			return pc + 8;
		return info.branchTarget;
	}
} // namespace DebuggerStep
