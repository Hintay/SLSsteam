#pragma once

#include "libmem/libmem.h"

#include <string>

namespace Diagnostics
{
	bool tryGetModuleSHA256(const lm_module_t& module, std::string& out);
	void logStartupModuleSummary();
	void logPatternSummary();
}
