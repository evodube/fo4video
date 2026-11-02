#include "Screen.h"

#include "Settings.h"

namespace fv::Screen
{
	namespace
	{
		constexpr std::uint16_t kPlaceholderWidth = 4;
		constexpr std::uint16_t kPlaceholderHeight = 8;
		constexpr auto          kChoiceIni = "Data\\F4SE\\Plugins\\RealVideos_Screens.ini";

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

		bool ContainsAny(const std::string& a_haystackLower, std::span<const std::string> a_needles)
		{
			for (const auto& n : a_needles) {
				if (!n.empty() && a_haystackLower.find(n) != std::string::npos) {
					return true;
				}
			}
			return false;
		}

		bool ContainsAny(const std::string& a_haystackLower, std::initializer_list<std::string_view> a_needles)
		{
			for (const auto n : a_needles) {
				if (a_haystackLower.find(n) != std::string::npos) {
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

		bool IsScreenShader(RE::BSShaderProperty* a_prop)
		{
			if (!a_prop || !a_prop->material) {
				return false;
			}
			const auto type = a_prop->material->GetType();
			return type == RE::BSShaderMaterial::Type::kEffect || type == RE::BSShaderMaterial::Type::kLighting;
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

		struct Candidate
		{
			RE::BSGeometry* geom;
			std::string     name;
			std::string     pathLower;  // ancestor names joined with '/'
		};

		void Collect(RE::NiAVObject* a_obj, const std::string& a_path, std::vector<Candidate>& a_out, int a_depth = 0)
		{
			if (!a_obj || a_depth > 64) {
				return;
			}
			const auto name = Str(a_obj->name);
			const auto path = a_path.empty() ? name : a_path + "/" + name;

			if (auto* geom = a_obj->IsGeometry()) {
				if (IsScreenShader(GetShaderProperty(geom))) {
					a_out.push_back({ geom, name, Lower(path) });
				}
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

		std::vector<Candidate> Candidates(RE::NiAVObject* a_root)
		{
			std::vector<Candidate> out;
			Collect(a_root, {}, out);
			return out;
		}

		int Score(const Candidate& a_c)
		{
			const auto& settings = Settings::Get();
			auto*       prop = GetShaderProperty(a_c.geom);
			const auto  nameLower = Lower(a_c.name);
			const auto  texLower = Lower(BaseTextureName(prop));
			const bool  effect = prop && prop->material && prop->material->GetType() == RE::BSShaderMaterial::Type::kEffect;

			int score = 0;
			if (ContainsAny(nameLower, settings.screenNames)) {
				score += 30;
			}
			if (ContainsAny(texLower, settings.screenNames)) {
				score += 25;
			}
			if (ContainsAny(a_c.pathLower, settings.screenNames)) {
				score += 10;
			}
			// light beams, flares and other effects are never the screen
			if (ContainsAny(nameLower, { "beam", "flare", "ray", "fog", "dust", "cone", "volum", "glare", "halo" }) ||
				ContainsAny(texLower, { "beam", "flare", "lightray", "fog", "dust", "glare", "halo" })) {
				score -= 100;
			}
			// building materials are frames, backs and stands
			if (ContainsAny(texLower, { "concrete", "metal", "wood", "rust", "brick", "plaster", "girder", "steel" })) {
				score -= 15;
			}
			if (effect) {
				score += 3;
			}
			return score;
		}

		int BestCandidate(const std::vector<Candidate>& a_candidates)
		{
			int best = -1;
			int bestScore = std::numeric_limits<int>::min();
			for (int i = 0; i < static_cast<int>(a_candidates.size()); ++i) {
				const int s = Score(a_candidates[i]);
				if (s > bestScore) {
					best = i;
					bestScore = s;
				}
			}
			return best;
		}

		// --- UV bounds from the CPU copy of the vertex data ---

		float HalfToFloat(std::uint16_t a_h)
		{
			const std::uint32_t sign = (a_h & 0x8000u) << 16;
			std::uint32_t       exp = (a_h >> 10) & 0x1F;
			std::uint32_t       mant = a_h & 0x3FF;
			std::uint32_t       bits;
			if (exp == 0) {
				if (mant == 0) {
					bits = sign;
				} else {
					exp = 127 - 15 + 1;
					while (!(mant & 0x400)) {
						mant <<= 1;
						--exp;
					}
					mant &= 0x3FF;
					bits = sign | (exp << 23) | (mant << 13);
				}
			} else if (exp == 31) {
				bits = sign | 0x7F800000 | (mant << 13);
			} else {
				bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
			}
			float f;
			std::memcpy(&f, &bits, sizeof(f));
			return f;
		}

		struct UVBounds
		{
			float u0{ 0 }, v0{ 0 }, u1{ 1 }, v1{ 1 };
			bool  ok{ false };
		};

		UVBounds ReadUVBounds(RE::BSGeometry* a_geom)
		{
			UVBounds out;
			auto*    tri = a_geom ? a_geom->IsTriShape() : nullptr;
			if (!tri) {
				return out;
			}
			auto* shape = static_cast<RE::BSGraphics::TriShape*>(a_geom->rendererData);
			if (!shape || !shape->vertexBuffer || !shape->vertexBuffer->data) {
				return out;
			}
			auto desc = a_geom->vertexDesc;
			if (!desc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_UV)) {
				return out;
			}
			const std::uint32_t stride = desc.GetSize();
			const std::uint32_t offset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_TEXCOORD0);
			const std::uint32_t count = tri->numVertices;
			if (stride == 0 || offset + 4 > stride || count == 0) {
				return out;
			}

			const auto* data = static_cast<const std::uint8_t*>(shape->vertexBuffer->data);
			float       u0 = FLT_MAX, v0 = FLT_MAX, u1 = -FLT_MAX, v1 = -FLT_MAX;
			for (std::uint32_t i = 0; i < count; ++i) {
				std::uint16_t h[2];
				std::memcpy(h, data + static_cast<std::size_t>(i) * stride + offset, sizeof(h));
				const float u = HalfToFloat(h[0]);
				const float v = HalfToFloat(h[1]);
				if (!std::isfinite(u) || !std::isfinite(v)) {
					continue;
				}
				u0 = std::min(u0, u);
				u1 = std::max(u1, u);
				v0 = std::min(v0, v);
				v1 = std::max(v1, v);
			}
			if (u1 - u0 < 1e-4f || v1 - v0 < 1e-4f || std::abs(u0) > 1000.f || std::abs(v1) > 1000.f) {
				return out;
			}
			out = { u0, v0, u1, v1, true };
			return out;
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

		std::string ModelKey(RE::TESObjectREFR* a_ref)
		{
			auto* base = a_ref ? a_ref->GetObjectReference() : nullptr;
			if (!base) {
				return {};
			}
			if (auto* file = base->GetFile(0)) {
				return std::format("{}|{:06X}", file->GetFilename(), base->GetLocalFormID());
			}
			return std::format("{:08X}", base->GetFormID());
		}
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
		const auto  file = std::format("RealVideos\\slot{:02}.dds", a_slot);

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

		log::error("slot {}: could not load placeholder texture '{}'. Is Data\\Textures\\RealVideos installed?", a_slot, file);
		return {};
	}

	ScreenChoice LoadChoice(RE::TESObjectREFR* a_ref)
	{
		ScreenChoice choice;
		const auto   key = ModelKey(a_ref);
		if (key.empty()) {
			return choice;
		}
		char buf[512]{};
		::GetPrivateProfileStringA("Screens", key.c_str(), "", buf, sizeof(buf), kChoiceIni);
		const std::string value = buf;
		if (value.empty()) {
			return choice;
		}
		// index|name|flip
		const auto p1 = value.find('|');
		const auto p2 = p1 == std::string::npos ? std::string::npos : value.find('|', p1 + 1);
		try {
			choice.index = std::stoi(value.substr(0, p1));
			if (p1 != std::string::npos) {
				choice.name = value.substr(p1 + 1, p2 == std::string::npos ? std::string::npos : p2 - p1 - 1);
			}
			if (p2 != std::string::npos) {
				choice.flip = std::stoi(value.substr(p2 + 1)) & 3;
			}
		} catch (...) {
			return {};
		}
		return choice;
	}

	void SaveChoice(RE::TESObjectREFR* a_ref, const ScreenChoice& a_choice)
	{
		const auto key = ModelKey(a_ref);
		if (key.empty()) {
			return;
		}
		const auto value = std::format("{}|{}|{}", a_choice.index, a_choice.name, a_choice.flip);
		::WritePrivateProfileStringA("Screens", key.c_str(), value.c_str(), kChoiceIni);
		log::info("saved screen choice for {}: {}", key, value);
	}

	int CountCandidates(RE::TESObjectREFR* a_ref)
	{
		auto* root = a_ref ? a_ref->Get3D() : nullptr;
		return root ? static_cast<int>(Candidates(root).size()) : 0;
	}

	bool Bind(RE::TESObjectREFR* a_ref, RE::NiTexture* a_slotTexture, ScreenBinding& a_binding, int a_forceIndex)
	{
		a_binding = {};
		if (!a_ref || !a_slotTexture) {
			return false;
		}
		auto* root = a_ref->Get3D();
		if (!root) {
			return false;
		}

		const auto candidates = Candidates(root);
		if (candidates.empty()) {
			log::warn("{}: no textured meshes found", RefName(a_ref));
			return false;
		}
		const int   count = static_cast<int>(candidates.size());
		const auto& settings = Settings::Get();
		const auto  saved = LoadChoice(a_ref);

		int index = -1;
		if (a_forceIndex >= 0) {
			index = a_forceIndex % count;
		} else if (saved.index >= 0) {
			if (saved.index < count && candidates[saved.index].name == saved.name) {
				index = saved.index;
			} else {
				for (int i = 0; i < count; ++i) {
					if (candidates[i].name == saved.name) {
						index = i;
						break;
					}
				}
			}
		}
		if (index < 0 && !settings.forceNode.empty()) {
			for (int i = 0; i < count; ++i) {
				if (_stricmp(candidates[i].name.c_str(), settings.forceNode.c_str()) == 0) {
					index = i;
					break;
				}
			}
		}
		if (index < 0) {
			index = BestCandidate(candidates);
		}
		if (index < 0) {
			return false;
		}

		auto* geom = candidates[index].geom;
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
		mat->IncRef();

		const auto type = mat->GetType();
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
		} else {
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
		}

		// stop UV/flipbook animation controllers (Videos of the Wasteland scrolls its screens this way)
		if (settings.stopScreenAnimation && prop->controllers) {
			a_binding.savedControllers = prop->controllers;
			prop->controllers.reset();
		}

		// map the mesh's UV range onto the whole video
		const int flip = a_forceIndex >= 0 || saved.index >= 0 ? saved.flip : 0;
		UVBounds  uv{};
		if (settings.fitUV) {
			uv = ReadUVBounds(geom);
			float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
			if (uv.ok) {
				u0 = uv.u0;
				v0 = uv.v0;
				u1 = uv.u1;
				v1 = uv.v1;
			}
			// shader does: uv' = uv * scale + offset
			const bool flipV = (flip & 1) != 0;
			const bool flipU = (flip & 2) != 0;
			const float su = 1.0f / (u1 - u0);
			const float sv = 1.0f / (v1 - v0);
			a_binding.uvScale = { flipU ? -su : su, flipV ? -sv : sv };
			a_binding.uvOffset = { flipU ? u1 * su : -u0 * su, flipV ? v1 * sv : -v0 * sv };
			for (int i = 0; i < 2; ++i) {
				mat->texCoordOffset[i] = a_binding.uvOffset;
				mat->texCoordScale[i] = a_binding.uvScale;
			}
		}

		a_binding.root = RE::NiPointer<RE::NiAVObject>(root);
		a_binding.geometry = RE::NiPointer<RE::BSGeometry>(geom);
		a_binding.material = mat;
		a_binding.slotTexture = RE::NiPointer<RE::NiTexture>(a_slotTexture);
		a_binding.candidateIndex = index;
		a_binding.candidateCount = count;
		a_binding.flip = flip;
		a_binding.geometryName = candidates[index].name;
		a_binding.bound = true;

		log::info("{}: bound mesh {}/{} '{}' ({} shader{}, texture '{}', sRGB={}, uv {} [{:.3f},{:.3f}]-[{:.3f},{:.3f}], flip {}, controllers {})",
			RefName(a_ref), index + 1, count, candidates[index].name, a_binding.isEffect ? "effect" : "lighting",
			a_binding.hadGlow ? " + glowmap" : "",
			a_binding.originalBase ? std::string(a_binding.originalBase->GetName()) : std::string("<none>"), a_binding.originalWasSRGB,
			settings.fitUV ? (uv.ok ? "fit" : "unread") : "untouched", uv.u0, uv.v0, uv.u1, uv.v1, flip,
			a_binding.savedControllers ? "stopped" : "none");
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
			if (a_binding.savedControllers && !prop->controllers) {
				prop->controllers = a_binding.savedControllers;
			}
		}
		if (a_binding.material) {
			// drop our ref; never free it ourselves (the engine owns it)
			(void)a_binding.material->DecRef();
		}
		a_binding = {};
	}

	bool StillValid(const ScreenBinding& a_binding, RE::NiAVObject* a_currentRoot)
	{
		if (!a_binding.bound || a_binding.root.get() != a_currentRoot || !a_currentRoot) {
			return false;
		}
		auto* geom = a_binding.geometry.get();
		bool  attached = false;
		for (RE::NiAVObject* node = geom; node; node = node->parent) {
			if (node == a_currentRoot) {
				attached = true;
				break;
			}
		}
		if (!attached) {
			return false;
		}
		auto* prop = GetShaderProperty(geom);
		if (!prop || prop->material != a_binding.material) {
			return false;
		}
		if (a_binding.isEffect) {
			return static_cast<RE::BSEffectShaderMaterial*>(a_binding.material)->baseTexture.get() == a_binding.slotTexture.get();
		}
		return static_cast<RE::BSLightingShaderMaterialBase*>(a_binding.material)->diffuseTexture.get() == a_binding.slotTexture.get();
	}

	void ReapplyUV(const ScreenBinding& a_binding)
	{
		if (!a_binding.bound || !a_binding.material || !Settings::Get().fitUV) {
			return;
		}
		auto* mat = a_binding.material;
		for (int i = 0; i < 2; ++i) {
			mat->texCoordOffset[i] = a_binding.uvOffset;
			mat->texCoordScale[i] = a_binding.uvScale;
		}
	}

	void Dump(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			log::info("dump: no reference selected");
			return;
		}
		auto* root = a_ref->Get3D();
		log::info("==== dump {} model key '{}' root '{}' ====", RefName(a_ref), ModelKey(a_ref),
			root ? Str(root->name) : std::string("<3D not loaded>"));
		if (!root) {
			return;
		}

		const auto candidates = Candidates(root);
		const int  best = BestCandidate(candidates);
		for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
			const auto& c = candidates[i];
			auto*       prop = GetShaderProperty(c.geom);
			auto*       mat = prop->material;
			const auto  uv = ReadUVBounds(c.geom);
			log::info("  [{}]{} '{}' path='{}' shader={} score={} tex='{}' meshUV={} [{:.3f},{:.3f}]-[{:.3f},{:.3f}] matUV off=({:.2f},{:.2f}) scale=({:.2f},{:.2f}) controllers={}",
				i + 1, i == best ? " <== auto pick" : "", c.name, c.pathLower,
				mat->GetType() == RE::BSShaderMaterial::Type::kEffect ? "effect" : std::format("lighting/{}", static_cast<int>(mat->GetFeature())),
				Score(c), BaseTextureName(prop), uv.ok ? "ok" : "n/a", uv.u0, uv.v0, uv.u1, uv.v1,
				mat->texCoordOffset[0].x, mat->texCoordOffset[0].y, mat->texCoordScale[0].x, mat->texCoordScale[0].y,
				prop->controllers ? "yes" : "no");
		}
		log::info("==== end dump ({} candidates) ====", candidates.size());
	}
}
