#include "Screen.h"

#include "Settings.h"

namespace fv::Screen
{
	namespace
	{
		constexpr std::uint16_t kPlaceholderWidth = 4;
		constexpr std::uint16_t kPlaceholderHeight = 8;

		std::string Str(const RE::BSFixedString& a_str)
		{
			const std::string_view sv = a_str;
			return sv.data() ? std::string(sv) : std::string();
		}

		std::string Lower(std::string a_str)
		{
			for (auto& c : a_str) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return a_str;
		}

		bool ContainsAny(const std::string& a_haystackLower, const std::vector<std::string>& a_needles)
		{
			for (const auto& n : a_needles) {
				if (!n.empty() && a_haystackLower.find(n) != std::string::npos) {
					return true;
				}
			}
			return false;
		}

		RE::BSShaderProperty* GetShaderProperty(RE::BSGeometry* a_geom)
		{
			if (!a_geom) {
				return nullptr;
			}
			return netimmerse_cast<RE::BSShaderProperty*>(a_geom->properties[1].get());
		}

		RE::NiPointer<RE::NiTexture>& GlowTexture(RE::BSShaderMaterial* a_material)
		{
			// BSLightingShaderMaterialGlowmap::spGlowMapTexture at 0xC0 (per F4SE 0.6.23)
			return *reinterpret_cast<RE::NiPointer<RE::NiTexture>*>(reinterpret_cast<std::uintptr_t>(a_material) + 0xC0);
		}

		std::string BaseTextureName(RE::BSShaderProperty* a_prop)
		{
			if (!a_prop || !a_prop->material) {
				return {};
			}
			auto* mat = a_prop->material;
			switch (mat->GetType()) {
			case RE::BSShaderMaterial::Type::kEffect:
				{
					auto* em = static_cast<RE::BSEffectShaderMaterial*>(mat);
					if (!em->sourceTexturePath.empty()) {
						return Str(em->sourceTexturePath);
					}
					return em->baseTexture ? std::string(em->baseTexture->GetName()) : std::string();
				}
			case RE::BSShaderMaterial::Type::kLighting:
				{
					auto* lm = static_cast<RE::BSLightingShaderMaterialBase*>(mat);
					return lm->diffuseTexture ? std::string(lm->diffuseTexture->GetName()) : std::string();
				}
			default:
				return {};
			}
		}

		struct GeomInfo
		{
			RE::BSGeometry* geom;
			std::string     pathLower;  // ancestor names joined with '/'
		};

		void Collect(RE::NiAVObject* a_obj, const std::string& a_path, std::vector<GeomInfo>& a_out, int a_depth = 0)
		{
			if (!a_obj || a_depth > 64) {
				return;
			}
			const auto name = Str(a_obj->name);
			const auto path = a_path.empty() ? name : a_path + "/" + name;

			if (auto* geom = a_obj->IsGeometry()) {
				a_out.push_back({ geom, Lower(path) });
				return;
			}
			if (auto* node = a_obj->IsNode()) {
				for (auto& child : node->children) {
					if (child) {
						Collect(child.get(), path, a_out, a_depth + 1);
					}
				}
			}
		}

		bool IsSRGBFormat(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
			case DXGI_FORMAT_BC1_UNORM_SRGB:
			case DXGI_FORMAT_BC2_UNORM_SRGB:
			case DXGI_FORMAT_BC3_UNORM_SRGB:
			case DXGI_FORMAT_BC7_UNORM_SRGB:
				return true;
			default:
				return false;
			}
		}

		bool TextureIsSRGB(RE::NiTexture* a_tex)
		{
			if (!a_tex) {
				return false;
			}
			if (auto* rt = a_tex->rendererTexture; rt && rt->srView) {
				D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
				reinterpret_cast<ID3D11ShaderResourceView*>(rt->srView)->GetDesc(&desc);
				return IsSRGBFormat(desc.Format);
			}
			return a_tex->isSRGB;
		}

		// --- placeholder texture loading ---

		RE::NiTexture* LoadByPathOG(const std::string& a_path)
		{
			// F4SE 0.6.23 LoadTextureByPath (used by its Scaleform MountImage). Returns with +1 ref.
			using func_t = void (*)(const char*, bool, RE::NiTexture*&, std::int32_t, std::uint64_t, std::uint64_t);
			static REL::Relocation<func_t> func{ REL::Offset(0x027D61F0) };
			RE::NiTexture* tex = nullptr;
			func(a_path.c_str(), true, tex, 0, 0, 0);
			return tex;
		}

		RE::NiPointer<RE::NiTexture> LoadViaTextureSet(const std::string& a_path)
		{
			RE::NiPointer<RE::NiTexture> out;
			auto*                        set = RE::BSShaderTextureSet::CreateObject();
			if (!set) {
				return out;
			}
			RE::NiPointer<RE::BSShaderTextureSet> hold(set);
			// call by vtable slot: SetTextureFilename = 0x2C, GetTexture(type, NiPointer&, srgb) = 0x2B (per F4SE)
			auto** vtbl = *reinterpret_cast<void***>(set);
			using set_t = void (*)(RE::BSTextureSet*, std::uint32_t, const char*);
			using get_t = void (*)(RE::BSTextureSet*, std::uint32_t, RE::NiPointer<RE::NiTexture>*, bool);
			reinterpret_cast<set_t>(vtbl[0x2C])(set, 0, a_path.c_str());
			reinterpret_cast<get_t>(vtbl[0x2B])(set, 0, &out, false);
			return out;
		}

		RE::NiPointer<RE::NiTexture> LoadPlaceholder(const std::string& a_path)
		{
			if (REX::FModule::IsRuntimeOG()) {
				RE::NiTexture* raw = LoadByPathOG(a_path);
				if (!raw) {
					return {};
				}
				RE::NiPointer<RE::NiTexture> tex(raw);
				raw->DecRefCount();  // balance LoadTextureByPath's ref; NiPointer holds ours
				return tex;
			}
			return LoadViaTextureSet(a_path);
		}

		std::array<RE::NiPointer<RE::NiTexture>, 16> g_slotTextures;
	}

	bool IsPlaceholderTexture(RE::NiTexture* a_tex)
	{
		if (!a_tex || !a_tex->rendererTexture) {
			return false;
		}
		const auto& h = a_tex->rendererTexture->header;
		return h.width == kPlaceholderWidth && h.height == kPlaceholderHeight;
	}

	std::string RefName(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return "<none>";
		}
		const char* name = a_ref->GetDisplayFullName();
		return std::format("'{}' [{:08X}]", name && *name ? name : "", a_ref->GetFormID());
	}

	RE::NiPointer<RE::NiTexture> LoadSlotTexture(int a_slot)
	{
		if (a_slot < 0 || a_slot >= static_cast<int>(g_slotTextures.size())) {
			return {};
		}
		if (g_slotTextures[a_slot]) {
			return g_slotTextures[a_slot];
		}

		const auto& settings = Settings::Get();
		const auto  file = std::format("fo4video\\slot{:02}.dds", a_slot);

		std::vector<std::string> candidates;
		if (!settings.placeholderPrefix.empty()) {
			candidates.push_back(settings.placeholderPrefix + file);
		} else {
			candidates.push_back("Textures\\" + file);
			candidates.push_back(file);
		}

		RE::NiPointer<RE::NiTexture> fallback;
		for (const auto& path : candidates) {
			auto tex = LoadPlaceholder(path);
			if (!tex) {
				log::info("slot {}: '{}' -> no texture", a_slot, path);
				continue;
			}
			auto* rt = tex->rendererTexture;
			log::info("slot {}: '{}' -> NiTexture '{}' renderer={} size={}x{}", a_slot, path, tex->GetName(),
				static_cast<void*>(rt), rt ? rt->header.width : 0, rt ? rt->header.height : 0);
			if (IsPlaceholderTexture(tex.get())) {
				g_slotTextures[a_slot] = tex;
				return tex;
			}
			if (!rt && !fallback) {
				fallback = tex;  // still streaming in; verified again before we touch it
			}
		}

		if (fallback) {
			log::warn("slot {}: placeholder not resident yet, will verify before use", a_slot);
			g_slotTextures[a_slot] = fallback;
			return fallback;
		}

		log::error("slot {}: could not load placeholder texture '{}'. Is Data\\Textures\\fo4video installed?", a_slot, file);
		return {};
	}

	RE::BSGeometry* FindScreenGeometry(RE::NiAVObject* a_root)
	{
		if (!a_root) {
			return nullptr;
		}
		const auto& settings = Settings::Get();

		if (!settings.forceNode.empty()) {
			if (auto* obj = a_root->GetObjectByName(RE::BSFixedString(settings.forceNode))) {
				std::vector<GeomInfo> sub;
				Collect(obj, {}, sub);
				for (auto& g : sub) {
					if (GetShaderProperty(g.geom)) {
						return g.geom;
					}
				}
			}
			log::warn("sForceNode '{}' not found or has no shaded geometry", settings.forceNode);
		}

		std::vector<GeomInfo> geoms;
		Collect(a_root, {}, geoms);

		RE::BSGeometry* best = nullptr;
		int             bestScore = 0;
		RE::BSGeometry* onlyEffect = nullptr;
		int             effectCount = 0;

		for (auto& g : geoms) {
			auto* prop = GetShaderProperty(g.geom);
			if (!prop || !prop->material) {
				continue;
			}
			const auto type = prop->material->GetType();
			if (type != RE::BSShaderMaterial::Type::kEffect && type != RE::BSShaderMaterial::Type::kLighting) {
				continue;
			}
			if (type == RE::BSShaderMaterial::Type::kEffect) {
				onlyEffect = g.geom;
				++effectCount;
			}

			int        score = 0;
			const auto ownName = Lower(Str(g.geom->name));
			if (ContainsAny(ownName, settings.screenNames)) {
				score = 3;
			} else if (ContainsAny(g.pathLower, settings.screenNames)) {
				score = 2;
			} else if (ContainsAny(Lower(BaseTextureName(prop)), settings.screenNames)) {
				score = 1;
			}
			if (score > bestScore) {
				best = g.geom;
				bestScore = score;
			}
		}

		if (best) {
			return best;
		}
		if (effectCount == 1) {
			return onlyEffect;  // a single glowing surface on a TV is almost always the screen
		}
		return nullptr;
	}

	bool Bind(RE::TESObjectREFR* a_ref, RE::NiTexture* a_slotTexture, ScreenBinding& a_binding)
	{
		a_binding = {};
		if (!a_ref || !a_slotTexture) {
			return false;
		}
		auto* root = a_ref->Get3D();
		if (!root) {
			return false;
		}

		auto* geom = FindScreenGeometry(root);
		if (!geom) {
			log::warn("{}: no screen geometry found. Press the dump key (or set sForceNode) to pick one.", RefName(a_ref));
			return false;
		}
		auto* prop = GetShaderProperty(geom);
		if (!prop || !prop->material) {
			return false;
		}

		// give this TV its own copy of the material so other TVs of the same model are untouched
		prop->SetMaterial(prop->material, true);
		auto* mat = prop->material;
		if (!mat) {
			return false;
		}

		const auto& settings = Settings::Get();
		const auto  type = mat->GetType();

		for (int i = 0; i < 2; ++i) {
			a_binding.originalUVOffset[i] = mat->texCoordOffset[i];
			a_binding.originalUVScale[i] = mat->texCoordScale[i];
		}

		if (type == RE::BSShaderMaterial::Type::kEffect) {
			auto* em = static_cast<RE::BSEffectShaderMaterial*>(mat);
			a_binding.isEffect = true;
			a_binding.originalBase = em->baseTexture;
			a_binding.originalWasSRGB = TextureIsSRGB(em->baseTexture.get());
			a_binding.originalBaseColor = em->baseColor;
			a_binding.originalBaseColorScale = em->baseColorScale;

			em->baseTexture = RE::NiPointer<RE::NiTexture>(a_slotTexture);
			em->baseColor = RE::NiColorA{ 1.0f, 1.0f, 1.0f, 1.0f };
			em->baseColorScale = settings.brightness;
		} else if (type == RE::BSShaderMaterial::Type::kLighting) {
			auto* lm = static_cast<RE::BSLightingShaderMaterialBase*>(mat);
			a_binding.originalBase = lm->diffuseTexture;
			a_binding.originalWasSRGB = TextureIsSRGB(lm->diffuseTexture.get());
			lm->diffuseTexture = RE::NiPointer<RE::NiTexture>(a_slotTexture);

			if (mat->GetFeature() == RE::BSShaderMaterial::Feature::kGlowmap) {
				auto& glow = GlowTexture(mat);
				a_binding.hadGlow = true;
				a_binding.originalGlow = glow;
				glow = RE::NiPointer<RE::NiTexture>(a_slotTexture);
			}
		} else {
			return false;
		}

		if (settings.resetUV) {
			for (int i = 0; i < 2; ++i) {
				mat->texCoordOffset[i] = RE::NiPoint2{ 0.0f, 0.0f };
				mat->texCoordScale[i] = RE::NiPoint2{ 1.0f, 1.0f };
			}
		}

		a_binding.root = RE::NiPointer<RE::NiAVObject>(root);
		a_binding.geometry = RE::NiPointer<RE::BSGeometry>(geom);
		a_binding.material = mat;
		a_binding.bound = true;

		log::info("{}: bound screen '{}' ({} shader{}, original texture '{}', sRGB={})", RefName(a_ref), Str(geom->name),
			a_binding.isEffect ? "effect" : "lighting", a_binding.hadGlow ? " + glowmap" : "",
			a_binding.originalBase ? std::string(a_binding.originalBase->GetName()) : std::string("<none>"), a_binding.originalWasSRGB);
		return true;
	}

	void Unbind(ScreenBinding& a_binding)
	{
		if (!a_binding.bound) {
			a_binding = {};
			return;
		}

		auto* prop = GetShaderProperty(a_binding.geometry.get());
		if (prop && prop->material == a_binding.material && a_binding.material) {
			auto* mat = a_binding.material;
			for (int i = 0; i < 2; ++i) {
				mat->texCoordOffset[i] = a_binding.originalUVOffset[i];
				mat->texCoordScale[i] = a_binding.originalUVScale[i];
			}
			if (a_binding.isEffect) {
				auto* em = static_cast<RE::BSEffectShaderMaterial*>(mat);
				em->baseTexture = a_binding.originalBase;
				em->baseColor = a_binding.originalBaseColor;
				em->baseColorScale = a_binding.originalBaseColorScale;
			} else {
				auto* lm = static_cast<RE::BSLightingShaderMaterialBase*>(mat);
				lm->diffuseTexture = a_binding.originalBase;
				if (a_binding.hadGlow) {
					GlowTexture(mat) = a_binding.originalGlow;
				}
			}
		}
		a_binding = {};
	}

	void Dump(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			log::info("dump: no reference selected");
			return;
		}
		auto* root = a_ref->Get3D();
		log::info("==== dump {} root '{}' ====", RefName(a_ref), root ? Str(root->name) : std::string("<3D not loaded>"));
		if (!root) {
			return;
		}

		std::vector<GeomInfo> geoms;
		Collect(root, {}, geoms);
		auto* chosen = FindScreenGeometry(root);

		int index = 0;
		for (auto& g : geoms) {
			auto*       prop = GetShaderProperty(g.geom);
			std::string shader = "none";
			std::string extra;
			if (prop && prop->material) {
				auto* mat = prop->material;
				switch (mat->GetType()) {
				case RE::BSShaderMaterial::Type::kEffect:
					shader = "effect";
					break;
				case RE::BSShaderMaterial::Type::kLighting:
					shader = std::format("lighting(feature {})", static_cast<int>(mat->GetFeature()));
					break;
				default:
					shader = std::format("type {}", static_cast<int>(mat->GetType()));
					break;
				}
				extra = std::format(" tex='{}' uvOff=({:.2f},{:.2f}) uvScale=({:.2f},{:.2f})", BaseTextureName(prop),
					mat->texCoordOffset[0].x, mat->texCoordOffset[0].y, mat->texCoordScale[0].x, mat->texCoordScale[0].y);
			}
			log::info("  [{}]{} '{}' path='{}' shader={}{}", index++, g.geom == chosen ? " <== SCREEN" : "", Str(g.geom->name),
				g.pathLower, shader, extra);
		}
		if (!chosen) {
			log::info("  no screen picked automatically. Put the name of the right one in sForceNode in fo4video.ini");
		}
		log::info("==== end dump ====");
	}
}
