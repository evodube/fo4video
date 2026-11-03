#pragma once

namespace fv
{
	// The address of a game class's main vtable. On 1.10.163 it comes from the known vtable address; on the newer
	// game versions (where only the class's RTTI address is known) it is found through that RTTI. 0 if not found.
	std::uintptr_t FindVtable(const REL::ID& a_vtableOG, const REL::ID& a_rtti);
}
