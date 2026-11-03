#include "Vtable.h"

namespace fv
{
	namespace
	{
		// MSVC x64 RTTI: every vtable is preceded by a pointer to its "complete object locator", which names the
		// class's type descriptor (as an offset from the module base) and itself (to tell it from look-alikes).
		struct CompleteObjectLocator
		{
			std::uint32_t signature;         // 1 on x64
			std::uint32_t offset;            // 0 = the class's main vtable
			std::uint32_t cdOffset;
			std::int32_t  typeDescriptor;    // RVA
			std::int32_t  classDescriptor;   // RVA
			std::int32_t  self;              // RVA of this locator
		};

		std::uintptr_t ScanForVtable(std::uintptr_t a_typeDescriptor)
		{
			const auto& module = REX::FModule::GetExecutingModule();
			const auto  base = module.GetBaseAddress();
			const auto  rdata = module.GetSection(".rdata");
			const auto  begin = rdata.GetAddress();
			const auto  size = rdata.GetSize();
			if (!begin || size < sizeof(CompleteObjectLocator)) {
				return 0;
			}
			const auto tdRva = static_cast<std::int32_t>(a_typeDescriptor - base);

			// 1) the locator of the class's main vtable
			std::uintptr_t locator = 0;
			for (std::uintptr_t at = begin; at + sizeof(CompleteObjectLocator) <= begin + size; at += 4) {
				const auto* col = reinterpret_cast<const CompleteObjectLocator*>(at);
				if (col->signature == 1 && col->offset == 0 && col->typeDescriptor == tdRva && col->self == static_cast<std::int32_t>(at - base)) {
					locator = at;
					break;
				}
			}
			if (!locator) {
				return 0;
			}
			// 2) the vtable: the slot right after the pointer to that locator
			for (std::uintptr_t at = begin; at + 16 <= begin + size; at += 8) {
				if (*reinterpret_cast<const std::uintptr_t*>(at) == locator) {
					return at + 8;
				}
			}
			return 0;
		}
	}

	std::uintptr_t FindVtable(const REL::ID& a_vtableOG, const REL::ID& a_rtti)
	{
		if (REX::FModule::IsRuntimeOG()) {
			return REL::Relocation<std::uintptr_t>{ a_vtableOG }.address();
		}
		static std::mutex                                      lock;
		static std::unordered_map<std::uintptr_t, std::uintptr_t> cache;
		const auto                                             td = REL::Relocation<std::uintptr_t>{ a_rtti }.address();
		std::scoped_lock                                       guard(lock);
		if (auto it = cache.find(td); it != cache.end()) {
			return it->second;
		}
		const auto vtable = ScanForVtable(td);
		if (!vtable) {
			log::warn("couldn't find a vtable on this game version (RTTI at {:#x})", td);
		}
		cache.emplace(td, vtable);
		return vtable;
	}
}
