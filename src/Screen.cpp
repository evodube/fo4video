#include "Screen.h"
#include "Vtable.h"

#include "Settings.h"

namespace fv::Screen
{
	namespace
	{
		constexpr std::uint16_t kPlaceholderWidth = 4;
		constexpr std::uint16_t kPlaceholderHeight = 8;
		constexpr auto          kChoiceIni = "Data\\F4SE\\Plugins\\RealVideos_Screens.ini";
		constexpr const char*   kGlowName = "RealVideosGlow";

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

		// The renderer caches each mesh's draw commands (including which textures to bind). After changing the
		// material we make the engine rebuild them, otherwise some objects keep drawing the old texture.
		void RebuildRenderPasses(RE::BSShaderProperty* a_prop)
		{
			if (!a_prop) {
				return;
			}
			using F = RE::BSShaderProperty::EShaderPropertyFlag;
			const bool twoSided = a_prop->flags.all(F::kTwoSided);
			a_prop->SetFlag(F::kTwoSided, !twoSided);  // any flag change marks the pass cache dirty
			a_prop->SetFlag(F::kTwoSided, twoSided);
			a_prop->lastRenderPassState = std::numeric_limits<std::int32_t>::max();
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
			RE::BSGeometry*    geom;
			std::string        name;
			std::string        pathLower;  // ancestor names joined with '/'
			RE::TESObjectREFR* owner{ nullptr };
			RE::NiAVObject*    root{ nullptr };
			std::string        ownerKey;
			bool               nearby{ false };
		};

		void Collect(RE::NiAVObject* a_obj, const std::string& a_path, std::vector<Candidate>& a_out, int a_depth = 0)
		{
			if (!a_obj || a_depth > 64) {
				return;
			}
			const auto name = Str(a_obj->name);
			const auto path = a_path.empty() ? name : a_path + "/" + name;

			if (auto* geom = a_obj->IsGeometry()) {
				// editor/idle markers are invisible in game
				if (IsScreenShader(GetShaderProperty(geom)) && Lower(name).find("marker") == std::string::npos && name != kGlowName) {
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

		std::string ModelKey(RE::TESObjectREFR* a_ref);
		int         Score(const Candidate& a_c);

		// Videos of the Wasteland's Radiation Queen (all editions: plain, Clean, Vault-Tec, Nuka Cola) is a media player
		// that drives a TV next to it; it isn't a screen. Its glowing holotape uses a VotW material, which made it look
		// like a video surface. Matched by its mesh (VotWR\RadiationQueen.nif / RadiationQueenClean.nif), so the Radiation
		// Star and Ceiling Projector, which have the same holotape mesh, still work.
		bool IsMediaPlayer(RE::TESObjectREFR* a_ref)
		{
			auto* base = a_ref ? a_ref->GetObjectReference() : nullptr;
			auto* model = base ? base->As<RE::TESModel>() : nullptr;
			const char* path = model ? model->GetModel() : nullptr;
			if (!path) {
				return false;
			}
			auto lower = Lower(path);
			std::ranges::replace(lower, '/', '\\');
			if (lower.starts_with("meshes\\")) {
				lower.erase(0, 7);
			}
			return lower.starts_with("votwr\\radiationqueen");
		}

		// The model path of a reference's base object, lower case, without "meshes\\"
		std::string ModelPath(RE::TESObjectREFR* a_ref)
		{
			auto* base = a_ref ? a_ref->GetObjectReference() : nullptr;
			auto* model = base ? base->As<RE::TESModel>() : nullptr;
			const char* path = model ? model->GetModel() : nullptr;
			if (!path) {
				return {};
			}
			auto lower = Lower(path);
			std::ranges::replace(lower, '/', '\\');
			if (lower.starts_with("meshes\\")) {
				lower.erase(0, 7);
			}
			return lower;
		}

		void CollectRef(RE::TESObjectREFR* a_ref, bool a_nearby, std::vector<Candidate>& a_out)
		{
			auto* root = a_ref ? a_ref->Get3D() : nullptr;
			if (!root || IsMediaPlayer(a_ref)) {
				return;  // a media player never shows a video, neither when picked nor as a screen next to something else
			}
			const auto first = a_out.size();
			Collect(root, {}, a_out);
			const auto key = ModelKey(a_ref);
			for (auto i = first; i < a_out.size(); ++i) {
				a_out[i].owner = a_ref;
				a_out[i].root = root;
				a_out[i].ownerKey = key;
				a_out[i].nearby = a_nearby;
			}
		}

		constexpr int kScreenLike = 20;  // score a nearby mesh needs to be offered as a screen

		// Every loaded cell (the interior, or the loaded exterior grid). Objects that belong together (Rebuild's
		// drive-in screen, projector and film) can sit in different exterior cells.
		template <class F>
		void ForEachLoadedCell(F&& a_fn)
		{
			std::vector<RE::TESObjectCELL*> seen;
			auto visit = [&](RE::TESObjectCELL* a_cell) {
				if (a_cell && std::ranges::find(seen, a_cell) == seen.end()) {
					seen.push_back(a_cell);
					a_fn(a_cell);
				}
			};
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				visit(player->GetParentCell());
			}
			auto* tes = RE::TES::GetSingleton();
			if (!tes) {
				return;
			}
			if (tes->interiorCell) {
				visit(tes->interiorCell);
				return;
			}
			if (auto* grid = tes->gridCells; grid && grid->gridCell) {
				const auto dim = std::min<std::uint32_t>(grid->dimension, 15);
				for (std::uint32_t i = 0; i < dim * dim; ++i) {
					auto* cell = grid->gridCell[i].cell;
					if (cell && cell->IsAttached()) {
						visit(cell);
					}
				}
			}
			// persistent objects of the worldspace (mods often place screens/projectors as persistent refs; they
			// aren't listed in the cell they stand in)
			if (tes->worldSpace) {
				visit(tes->worldSpace->persistentCell);
			}
		}

		// The selected object's meshes first, then screen-like meshes of objects around it, nearest first
		// (a projector's screen, or a drive-in screen that can't be clicked in the console).
		std::vector<Candidate> Candidates(RE::TESObjectREFR* a_ref, bool a_includeNearby)
		{
			// a video poster only ever plays on itself: never on a TV or film surface next to it
			if (a_includeNearby && IsVideoPoster(a_ref)) {
				a_includeNearby = false;
			}
			std::vector<Candidate> out;
			CollectRef(a_ref, false, out);

			const float radius = Settings::Get().nearbyRadius;
			if (!a_includeNearby || radius <= 0.0f || !a_ref) {
				return out;
			}

			const auto                                     origin = a_ref->GetPosition();
			std::vector<std::pair<float, RE::TESObjectREFR*>> refs;
			auto                                           gather = [&](RE::TESObjectCELL* a_cell) {
                if (!a_cell) {
                    return;
                }
                a_cell->ForEachReferenceInRange(origin, radius, [&](RE::TESObjectREFR* a_other) {
                    if (a_other && a_other != a_ref && !a_other->IsActor() && !a_other->IsDisabled() && a_other->Get3D() &&
                        std::ranges::find(refs, a_other, &std::pair<float, RE::TESObjectREFR*>::second) == refs.end()) {
                        refs.emplace_back(origin.GetSquaredDistance(a_other->GetPosition()), a_other);
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
			};
			gather(a_ref->GetParentCell());
			if (auto* player = RE::PlayerCharacter::GetSingleton(); player && player->GetParentCell() != a_ref->GetParentCell()) {
				gather(player->GetParentCell());
			}
			ForEachLoadedCell(gather);
			std::ranges::sort(refs, {}, &std::pair<float, RE::TESObjectREFR*>::first);

			int used = 0;
			for (auto& [dist, other] : refs) {
				if (used >= 16) {
					break;
				}
				std::vector<Candidate> theirs;
				CollectRef(other, true, theirs);
				bool any = false;
				for (auto& c : theirs) {
					if (Score(c) >= kScreenLike) {
						out.push_back(std::move(c));
						any = true;
					}
				}
				used += any ? 1 : 0;
			}
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
			// a video poster's face: the poster quad, not the frame (which shares the poster texture on its edges)
			if (a_c.owner && IsVideoPoster(a_c.owner) && (nameLower.starts_with("poster") || nameLower.starts_with("advertsposter"))) {
				score += 100;
			}
			if (ContainsAny(nameLower, settings.screenNames)) {
				score += 30;
			}
			if (ContainsAny(texLower, settings.screenNames)) {
				score += 25;
			}
			if (ContainsAny(a_c.pathLower, settings.screenNames)) {
				score += 10;
			}
			// surfaces made to show moving pictures (VotW, TV animations, film reels)
			if (ContainsAny(texLower, { "votw", "tvanim", "standby", "film", "reel", "movie", "projector" }) ||
				ContainsAny(nameLower, { "film", "movie", "projection" })) {
				score += 20;
			}
			// the housing of a projector screen shares the "screen" name but not the clean screen texture
			if (ContainsAny(texLower, { "speaker", "housing", "frame", "case", "cabinet" })) {
				score -= 30;
			}
			// light beams, flares and other effects are never the screen
			if (ContainsAny(nameLower, { "beam", "flare", "ray", "fog", "dust", "cone", "volum", "glare", "halo" }) ||
				ContainsAny(texLower, { "beam", "flare", "lightray", "fog", "dust", "glare", "halo" })) {
				score -= 100;
			}
			if (ContainsAny(nameLower, { "glass", "noise" }) || ContainsAny(texLower, { "noise", "colorblackutility" })) {
				score -= 20;  // overlays drawn on top of a TV screen
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

		// Surfaces made to show moving pictures (VotW screens, TV animations, film reels, Far Harbor's drive-in movie).
		bool HasVideoHint(const Candidate& a_c)
		{
			const auto texLower = Lower(BaseTextureName(GetShaderProperty(a_c.geom)));
			const auto nameLower = Lower(a_c.name);
			return ContainsAny(texLower, { "votw", "tvanim", "standby", "film", "reel", "movie" }) ||
			       ContainsAny(nameLower, { "film", "movie", "projection" }) || ContainsAny(a_c.pathLower, { "movie" });
		}

		int BestCandidate(const std::vector<Candidate>& a_candidates)
		{
			int best = -1;
			int bestScore = std::numeric_limits<int>::min();
			for (int i = 0; i < static_cast<int>(a_candidates.size()); ++i) {
				if (a_candidates[i].nearby) {
					continue;  // never jump to a neighbouring object automatically
				}
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
			float aspect{ 0.0f };  // physical width/height of the screen face, 0 = unknown
		};

		// What we learn from a mesh's vertex + triangle data.
		struct MeshShape
		{
			UVBounds              all;           // UV range of the whole mesh
			std::array<float, 4>  face[2]{};     // UV rect(s) of the screen face (front, and back if it has its own)
			int                   faces{ 0 };
			float                 aspect{ 0.0f };  // width/height of the screen face
			int                   flipHint{ 0 };   // orientation of face 0: bit 0 flip vertical, bit 1 mirror, bit 2 rotated 90
			float                 faceAspect[2]{};  // width/height of each face as the viewer sees it
			int                   faceOrient[2]{};  // orientation bits per face (see flipHint)
			RE::NiPoint3          faceNormal[2]{};  // world-space outward normal
			RE::NiPoint3          faceCenter[2]{};  // world-space centre
			bool                  partial{ false };  // screen face uses only part of the texture (frame shares it)
			bool                  skinned{ false };
			double                windingVote{ 0.0 };
		};

		struct Vec3
		{
			double x{}, y{}, z{};
			Vec3   operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
			double Dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
			Vec3   Cross(const Vec3& o) const { return { y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x }; }
			double Len() const { return std::sqrt(x * x + y * y + z * z); }
		};

		// Least-squares P = A + u*Du + v*Dv over the given vertices (exact for a flat, evenly mapped face).
		bool FitUVMap(const std::vector<std::array<double, 5>>& a_pts, Vec3& a_du, Vec3& a_dv)
		{
			double n = 0, su = 0, sv = 0, suu = 0, suv = 0, svv = 0;
			double b[3][3]{};
			for (const auto& p : a_pts) {
				const double u = p[3], v = p[4];
				n += 1;
				su += u;
				sv += v;
				suu += u * u;
				suv += u * v;
				svv += v * v;
				for (int c = 0; c < 3; ++c) {
					b[c][0] += p[c];
					b[c][1] += u * p[c];
					b[c][2] += v * p[c];
				}
			}
			const double m[3][3]{ { n, su, sv }, { su, suu, suv }, { sv, suv, svv } };
			const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
			                   m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
			if (std::abs(det) < 1e-12) {
				return false;
			}
			double inv[3][3];
			inv[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
			inv[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
			inv[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
			inv[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
			inv[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
			inv[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
			double du[3], dv[3];
			for (int c = 0; c < 3; ++c) {
				du[c] = inv[1][0] * b[c][0] + inv[1][1] * b[c][1] + inv[1][2] * b[c][2];
				dv[c] = inv[2][0] * b[c][0] + inv[2][1] * b[c][1] + inv[2][2] * b[c][2];
			}
			a_du = { du[0], du[1], du[2] };
			a_dv = { dv[0], dv[1], dv[2] };
			return a_du.Len() > 1e-9 && a_dv.Len() > 1e-9;
		}

		// a_texAspect: width/height of the mesh's original texture in pixels (0 = unknown). Artists paint a
		// screen's area upright into the texture, so a landscape area means "not rotated" and a portrait one
		// means "lying sideways" - more reliable than the mesh's rest pose (skinned / animated screens).
		MeshShape AnalyzeMesh(RE::BSGeometry* a_geom, float a_texAspect = 0.0f)
		{
			MeshShape out;
			auto*     tri = a_geom ? a_geom->IsTriShape() : nullptr;
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
			const std::uint32_t uvOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_TEXCOORD0);
			const std::uint32_t count = tri->numVertices;
			const bool          fullPrecision = desc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_FULLPREC);
			if (stride == 0 || uvOffset + 4 > stride || count == 0 || (fullPrecision ? 12u : 6u) > stride) {
				return out;
			}

			// --- vertices ---
			const auto*                        data = static_cast<const std::uint8_t*>(shape->vertexBuffer->data);
			const bool                         hasNormals = desc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_NORMAL);
			const std::uint32_t                nOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_NORMAL);
			std::vector<Vec3>                  nrm(count);
			std::vector<Vec3>                  pos(count);
			std::vector<std::array<float, 2>>  uv(count);
			float                              u0 = FLT_MAX, v0 = FLT_MAX, u1 = -FLT_MAX, v1 = -FLT_MAX;
			double                             extent = 0;
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto*   vert = data + static_cast<std::size_t>(i) * stride;
				std::uint16_t h[2];
				std::memcpy(h, vert + uvOffset, sizeof(h));
				uv[i] = { HalfToFloat(h[0]), HalfToFloat(h[1]) };
				if (fullPrecision) {
					float f[3];
					std::memcpy(f, vert, sizeof(f));
					pos[i] = { f[0], f[1], f[2] };
				} else {
					std::uint16_t hp[3];
					std::memcpy(hp, vert, sizeof(hp));
					pos[i] = { HalfToFloat(hp[0]), HalfToFloat(hp[1]), HalfToFloat(hp[2]) };
				}
				if (hasNormals && nOffset + 3 <= stride) {
					const auto* nb = vert + nOffset;
					nrm[i] = { nb[0] / 127.5 - 1.0, nb[1] / 127.5 - 1.0, nb[2] / 127.5 - 1.0 };
				}
				if (std::isfinite(uv[i][0]) && std::isfinite(uv[i][1])) {
					u0 = std::min(u0, uv[i][0]);
					u1 = std::max(u1, uv[i][0]);
					v0 = std::min(v0, uv[i][1]);
					v1 = std::max(v1, uv[i][1]);
				}
				extent = std::max({ extent, std::abs(pos[i].x), std::abs(pos[i].y), std::abs(pos[i].z) });
			}
			if (u1 - u0 < 1e-4f || v1 - v0 < 1e-4f || std::abs(u0) > 1000.f || std::abs(v1) > 1000.f) {
				return out;
			}
			out.all = { u0, v0, u1, v1, true };

			// --- triangles: find the biggest flat, connected face ---
			const auto* indices = shape->indexBuffer ? static_cast<const std::uint16_t*>(shape->indexBuffer->data) : nullptr;
			const std::uint32_t triCount = tri->numTriangles;
			if (!indices || triCount == 0 || triCount > 200000) {
				return out;
			}

			struct Tri
			{
				std::uint16_t v[3];
				Vec3          n;
				double        d, area;
			};
			std::vector<Tri> tris;
			tris.reserve(triCount);
			for (std::uint32_t t = 0; t < triCount; ++t) {
				Tri tr{ { indices[t * 3], indices[t * 3 + 1], indices[t * 3 + 2] } };
				if (tr.v[0] >= count || tr.v[1] >= count || tr.v[2] >= count) {
					return out;  // index data doesn't look like what we expect; don't guess
				}
				const Vec3   n = (pos[tr.v[1]] - pos[tr.v[0]]).Cross(pos[tr.v[2]] - pos[tr.v[0]]);
				const double len = n.Len();
				tr.area = len * 0.5;
				tr.n = len > 0 ? Vec3{ n.x / len, n.y / len, n.z / len } : Vec3{};
				tr.d = tr.n.Dot(pos[tr.v[0]]);
				tris.push_back(tr);
			}

			// union-find: same plane + sharing a vertex position
			std::vector<std::uint32_t> parent(tris.size());
			for (std::uint32_t i = 0; i < parent.size(); ++i) {
				parent[i] = i;
			}
			auto find = [&](std::uint32_t a) {
				while (parent[a] != a) {
					parent[a] = parent[parent[a]];
					a = parent[a];
				}
				return a;
			};
			const double quant = std::max(extent * 1e-4, 1e-3);
			std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> byPos;
			auto key = [&](const Vec3& p) {
				const auto q = [&](double c) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::llround(c / quant)) & 0x1FFFFF); };
				return (q(p.x) << 42) | (q(p.y) << 21) | q(p.z);
			};
			for (std::uint32_t t = 0; t < tris.size(); ++t) {
				if (tris[t].area <= 0) {
					continue;
				}
				for (auto vi : tris[t].v) {
					byPos[key(pos[vi])].push_back(t);
				}
			}
			for (auto& [k, list] : byPos) {
				for (std::size_t a = 1; a < list.size(); ++a) {
					const auto& ta = tris[list[0]];
					const auto& tb = tris[list[a]];
					// smooth neighbours belong to the same surface (flat screens and curved CRT glass alike);
					// a sharp edge (bevel, frame, post) starts a new one
					if (ta.n.Dot(tb.n) > 0.94) {
						parent[find(list[a])] = find(list[0]);
					}
				}
			}

			std::unordered_map<std::uint32_t, double> islandArea;
			for (std::uint32_t t = 0; t < tris.size(); ++t) {
				if (tris[t].area > 0) {
					islandArea[find(t)] += tris[t].area;
				}
			}
			std::vector<std::pair<double, std::uint32_t>> islands;
			for (auto& [root, area] : islandArea) {
				islands.emplace_back(area, root);
			}
			std::ranges::sort(islands, std::greater{});
			if (islands.empty()) {
				return out;
			}

			auto islandRect = [&](std::uint32_t a_root, std::vector<std::array<double, 5>>& a_pts) {
				std::array<float, 4> r{ FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX };
				std::vector<bool>    seen(count, false);
				for (std::uint32_t t = 0; t < tris.size(); ++t) {
					if (tris[t].area <= 0 || find(t) != a_root) {
						continue;
					}
					for (auto vi : tris[t].v) {
						r[0] = std::min(r[0], uv[vi][0]);
						r[1] = std::min(r[1], uv[vi][1]);
						r[2] = std::max(r[2], uv[vi][0]);
						r[3] = std::max(r[3], uv[vi][1]);
						if (!seen[vi]) {
							seen[vi] = true;
							a_pts.push_back({ pos[vi].x, pos[vi].y, pos[vi].z, uv[vi][0], uv[vi][1] });
						}
					}
				}
				return r;
			};

			// Orientation of a face for someone standing in front of it: which texture axis is the picture's
			// horizontal, and whether it runs left-to-right / top-to-bottom.
			const auto& rot = a_geom->GetWorldRotate();
			auto toWorld = [&](const Vec3& v) {
				const RE::NiPoint3 w = rot * RE::NiPoint3(static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z));
				return Vec3{ w.x, w.y, w.z };
			};
			auto normalize = [](Vec3 v) {
				const double l = v.Len();
				return l > 1e-9 ? Vec3{ v.x / l, v.y / l, v.z / l } : Vec3{};
			};
			const bool skinned = a_geom->skinInstance != nullptr;  // animated by bones: its rest pose says little about "up"
			out.skinned = skinned;
			// Which triangle winding is the visible side? Compare windings with the vertex normals over the whole
			// mesh (most of it is authored correctly), then trust the winding per face: some screens have both
			// sides' vertex normals pointing the same way, which made "the side facing you" a coin toss.
			double windingVote = 0.0;
			if (hasNormals) {
				for (const auto& tr : tris) {
					if (tr.area > 0) {
						const Vec3 vn{ nrm[tr.v[0]].x + nrm[tr.v[1]].x + nrm[tr.v[2]].x, nrm[tr.v[0]].y + nrm[tr.v[1]].y + nrm[tr.v[2]].y,
							nrm[tr.v[0]].z + nrm[tr.v[1]].z + nrm[tr.v[2]].z };
						windingVote += tr.area * tr.n.Dot(vn);
					}
				}
			}
			// NIF triangles are wound so the visible side follows the right-hand rule (that's how the exporters
			// build them); the vote is only logged. Two-sided screens whose vertex normals all point one way cancel
			// the vote out, and trusting it put the video on the back.
			constexpr double windingSign = 1.0;
			out.windingVote = windingVote;
			auto orientFace = [&](std::uint32_t a_root, const std::array<float, 4>& a_rect, const std::vector<std::array<double, 5>>& a_pts,
								  float& a_aspect, int& a_orient, RE::NiPoint3& a_normal, RE::NiPoint3& a_center) {
				a_aspect = 0.0f;
				a_orient = 0;
				if (!a_pts.empty()) {
					double cx = 0, cy = 0, cz = 0;
					for (const auto& p : a_pts) {
						cx += p[0];
						cy += p[1];
						cz += p[2];
					}
					const double inv = 1.0 / a_pts.size();
					a_center = a_geom->GetWorldTransform() * RE::NiPoint3(static_cast<float>(cx * inv), static_cast<float>(cy * inv), static_cast<float>(cz * inv));
				}
				Vec3 du, dv;
				if (a_rect[2] - a_rect[0] < 1e-4f || a_rect[3] - a_rect[1] < 1e-4f || !FitUVMap(a_pts, du, dv)) {
					return;
				}
				// outward normal: the face's triangle winding (the side the renderer draws), oriented like the
				// mesh's normals; vertex normals only if the winding cancels out
				Vec3 n{}, vn{};
				for (std::uint32_t t = 0; t < tris.size(); ++t) {
					if (tris[t].area > 0 && find(t) == a_root) {
						const auto& tr = tris[t];
						n = { n.x + tr.n.x * tr.area * windingSign, n.y + tr.n.y * tr.area * windingSign, n.z + tr.n.z * tr.area * windingSign };
						for (auto vi : tr.v) {
							vn = { vn.x + nrm[vi].x, vn.y + nrm[vi].y, vn.z + nrm[vi].z };
						}
					}
				}
				if (n.Len() < 1e-9) {
					n = vn;
				}
				if (n.Len() < 1e-6) {
					n = du.Cross(dv);
				}
				const Vec3 N = normalize(toWorld(n));
				a_normal = RE::NiPoint3(static_cast<float>(N.x), static_cast<float>(N.y), static_cast<float>(N.z));
				const Vec3 Du = toWorld(du), Dv = toWorld(dv);
				const Vec3 up{ 0, 0, 1 };
				const Vec3 forward{ -N.x, -N.y, -N.z };  // the viewer looks into the face
				Vec3       right = normalize(forward.Cross(up));
				if (right.Len() < 0.5) {
					right = normalize(Du);  // face is flat on the floor/ceiling: keep the texture's own axes
				}
				const Vec3 down = normalize(forward.Cross(right));  // = -(right x forward)

				bool swap = std::abs(Du.Dot(right)) < std::abs(Dv.Dot(right));
				auto shapeFor = [&](bool a_swap) {
					const double spanH = a_swap ? (a_rect[3] - a_rect[1]) : (a_rect[2] - a_rect[0]);
					const double spanV = a_swap ? (a_rect[2] - a_rect[0]) : (a_rect[3] - a_rect[1]);
					const double w = (a_swap ? Dv : Du).Len() * spanH;
					const double h = (a_swap ? Du : Dv).Len() * spanV;
					return w > 1e-6 && h > 1e-6 ? w / h : 0.0;
				};
				// shape of the screen area inside the texture, in pixels
				const double texel = a_texAspect > 0.0f ? (a_rect[2] - a_rect[0]) / (a_rect[3] - a_rect[1]) * a_texAspect : 0.0;
				if (texel > 1.2) {
					swap = false;  // painted landscape: upright in the texture
				} else if (texel > 0.0 && texel < 0.83) {
					swap = true;   // painted portrait: the picture lies sideways in the texture
				} else if (skinned || shapeFor(swap) < 0.9 && shapeFor(!swap) > 1.0) {
					// screens are landscape: if the geometry says portrait (or can't be trusted), turn it the other way
					swap = shapeFor(true) > shapeFor(false) && shapeFor(false) < 0.9;
				}
				const Vec3 hAxis = swap ? Dv : Du;  // texture axis that runs left-right on screen
				const Vec3 vAxis = swap ? Du : Dv;
				if (!skinned) {
					if (hAxis.Dot(right) < 0) {
						a_orient |= 2;
					}
					if (vAxis.Dot(down) < 0) {
						a_orient |= 1;
					}
				}
				if (swap) {
					a_orient |= 4;
				}
				double aspect = shapeFor(swap);
				if (texel > 0.0 && (skinned || aspect < 0.9)) {
					aspect = swap ? 1.0 / texel : texel;  // geometry can't be trusted here; the texture can
				}
				a_aspect = aspect > 0.2 && aspect < 6.0 ? static_cast<float>(aspect) : 0.0f;
			};

			// the largest flat face, plus any other big face parallel to it (the back, or a front split from it)
			auto islandNormal = [&](std::uint32_t a_root) {
				for (std::uint32_t t = 0; t < tris.size(); ++t) {
					if (tris[t].area > 0 && find(t) == a_root) {
						return tris[t].n;
					}
				}
				return Vec3{};
			};
			const Vec3 mainNormal = islandNormal(islands[0].second);
			for (std::size_t i = 0; i < islands.size() && out.faces < 2; ++i) {
				if (islands[i].first < islands[0].first * 0.5) {
					break;
				}
				if (i > 0 && std::abs(islandNormal(islands[i].second).Dot(mainNormal)) < 0.95) {
					continue;
				}
				std::vector<std::array<double, 5>> pts;
				const auto                         rect = islandRect(islands[i].second, pts);
				bool                               duplicate = false;
				for (int k = 0; k < out.faces; ++k) {
					const auto& o = out.face[k];
					duplicate |= std::abs(rect[0] - o[0]) < 1e-3f && std::abs(rect[1] - o[1]) < 1e-3f && std::abs(rect[2] - o[2]) < 1e-3f &&
					             std::abs(rect[3] - o[3]) < 1e-3f;
				}
				if (duplicate) {
					continue;
				}
				const int k = out.faces++;
				out.face[k] = rect;
				orientFace(islands[i].second, rect, pts, out.faceAspect[k], out.faceOrient[k], out.faceNormal[k], out.faceCenter[k]);
			}
			out.aspect = out.faceAspect[0];
			out.flipHint = out.faceOrient[0];

			const auto& f = out.face[0];
			const float meshArea = (u1 - u0) * (v1 - v0);
			const float faceArea = (f[2] - f[0]) * (f[3] - f[1]);
			out.partial = faceArea > 0 && faceArea < meshArea * 0.85f;
			return out;
		}

		UVBounds ReadUVBounds(RE::BSGeometry* a_geom, float a_texAspect = 0.0f)
		{
			const auto shape = AnalyzeMesh(a_geom, a_texAspect);
			UVBounds   out = shape.all;
			out.aspect = shape.aspect;
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

		// width / height in pixels, 0 if unknown
		float TextureAspect(RE::NiTexture* a_tex)
		{
			auto* rt = a_tex ? a_tex->rendererTexture : nullptr;
			if (!rt || !rt->srView) {
				return 0.0f;
			}
			Microsoft::WRL::ComPtr<ID3D11Resource> res;
			reinterpret_cast<ID3D11ShaderResourceView*>(rt->srView)->GetResource(res.GetAddressOf());
			Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
			if (!res || FAILED(res.As(&tex)) || !tex) {
				return 0.0f;
			}
			D3D11_TEXTURE2D_DESC desc{};
			tex->GetDesc(&desc);
			return desc.Height > 0 ? static_cast<float>(desc.Width) / static_cast<float>(desc.Height) : 0.0f;
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

		// a_wait: blocking load. Only safe on the game's main thread: the remote's actions run as game tasks on its worker
		// threads, and a blocking load there waits for texture streaming that may need that very thread - the game froze
		// on the first Play for some players. Off the main thread the texture object comes back at once and its picture
		// streams in; nothing touches it until it's there (see IsPlaceholderTexture).
		RE::NiTexture* LoadByPathOG(const std::string& a_path, bool a_wait)
		{
			// F4SE 0.6.23 LoadTextureByPath (used by its Scaleform MountImage). Returns with +1 ref.
			using func_t = void (*)(const char*, bool, RE::NiTexture*&, std::int32_t, std::uint64_t, std::uint64_t);
			static REL::Relocation<func_t> func{ REL::Offset(0x027D61F0) };
			RE::NiTexture* tex = nullptr;
			func(a_path.c_str(), a_wait, tex, 0, 0, 0);
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

		bool g_preloading = false;  // PreloadSlotTextures is running (main thread, save just loaded): blocking loads are fine

		RE::NiPointer<RE::NiTexture> LoadPlaceholder(const std::string& a_path)
		{
			if (REX::FModule::IsRuntimeOG()) {
				RE::NiTexture* raw = LoadByPathOG(a_path, g_preloading);
				if (!raw) {
					return {};
				}
				RE::NiPointer<RE::NiTexture> tex(raw);
				raw->DecRefCount();  // balance LoadTextureByPath's ref; NiPointer holds ours
				return tex;
			}
			return LoadViaTextureSet(a_path);
		}

		std::array<RE::NiPointer<RE::NiTexture>, 40> g_slotTextures;  // 0-19 pictures, 20-39 glow maps

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

	// Contraptions Workshop's powered (lit) posters: Decorations > Wall Decorations > Posters, the ones that need power
	bool IsVideoPoster(RE::TESObjectREFR* a_ref)
	{
		return a_ref && ModelPath(a_ref).starts_with("dlc05\\setdressing\\workshop\\posterframes\\");
	}

	// A powered poster shows its "Dark" overlay while it has no power (the A frames); the B frames have no overlay and
	// count as powered.
	bool PosterPowered(RE::TESObjectREFR* a_ref)
	{
		auto* root = a_ref ? a_ref->Get3D() : nullptr;
		if (!root) {
			return false;
		}
		RE::NiAVObject* dark = nullptr;
		std::function<void(RE::NiAVObject*, int)> find = [&](RE::NiAVObject* a_obj, int a_depth) {
			if (!a_obj || dark || a_depth > 16) {
				return;
			}
			if (_stricmp(Str(a_obj->name).c_str(), "Dark") == 0) {
				dark = a_obj;
				return;
			}
			if (auto* node = a_obj->IsNode()) {
				for (auto& child : node->children) {
					find(child.get(), a_depth + 1);
				}
			}
		};
		find(root, 0);
		return !dark || dark->GetAppCulled();
	}

	bool IsSupported(RE::TESObjectREFR* a_ref, std::string* a_what)
	{
		auto set = [&](const char* a_text) {
			if (a_what) {
				*a_what = a_text;
			}
		};
		auto* root = a_ref ? a_ref->Get3D() : nullptr;
		if (!root) {
			set("not loaded");
			return false;
		}
		if (IsMediaPlayer(a_ref)) {
			set("Videos of the Wasteland media player (Radiation Queen) - aim at the TV instead");
			return false;
		}
		if (IsVideoPoster(a_ref)) {
			set("video poster");
			return true;
		}
		const auto rootName = Lower(Str(root->name));
		const auto key = Lower(ModelKey(a_ref));
		std::vector<Candidate> meshes;
		CollectRef(a_ref, false, meshes);
		bool tv = false, votw = key.starts_with("votwmaster.esm"), cinema = rootName.find("drivein") != std::string::npos;
		for (const auto& c : meshes) {
			const auto name = Lower(c.name);
			const auto tex = Lower(BaseTextureName(GetShaderProperty(c.geom)));
			tv |= name.starts_with("screenanim");
			votw |= tex.find("votw") != std::string::npos;
			cinema |= name.find("driveinscreen") != std::string::npos || name.find("projectorlightbeam") != std::string::npos ||
			          tex.find("drivein") != std::string::npos;
		}
		set(tv ? "TV" : votw ? "Videos of the Wasteland projector/screen" : cinema ? "drive-in cinema" : "not a TV, projector or cinema screen");
		return tv || votw || cinema;
	}

	RE::TESObjectREFR* PickAimed(float a_maxDistance)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!player || !camera || !camera->cameraRoot) {
			return nullptr;
		}
		// where the camera is, looking where the player looks (pitch: positive = down)
		const auto   origin = camera->cameraRoot->GetWorldTranslate();
		const float  pitch = player->data.angle.x;
		const float  yaw = player->data.angle.z;
		const RE::NiPoint3 dir{ std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), -std::sin(pitch) };

		std::vector<std::pair<float, RE::TESObjectREFR*>> hits;
		ForEachLoadedCell([&](RE::TESObjectCELL* a_cell) {
			a_cell->ForEachReferenceInRange(origin, a_maxDistance + 3000.0f, [&](RE::TESObjectREFR* a_ref) {
				auto* root = a_ref && !a_ref->IsActor() && !a_ref->IsDisabled() ? a_ref->Get3D() : nullptr;
				if (!root || root->worldBound.fRadius <= 0.0f || std::ranges::find(hits, a_ref, &std::pair<float, RE::TESObjectREFR*>::second) != hits.end()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const auto& b = root->worldBound;
				const auto  oc = b.center - origin;
				const float t = oc.Dot(dir);  // distance along the view line to the point nearest the object
				const float miss2 = oc.SqrLength() - t * t;
				if (t > -b.fRadius && t - b.fRadius < a_maxDistance && miss2 <= b.fRadius * b.fRadius) {
					hits.emplace_back(t, a_ref);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		});
		std::ranges::sort(hits, {}, &std::pair<float, RE::TESObjectREFR*>::first);
		for (auto& [t, ref] : hits) {
			if (IsSupported(ref)) {
				return ref;
			}
		}
		return nullptr;
	}

	std::vector<RE::NiPointer<RE::TESObjectREFR>> FindNearby(const RE::NiPoint3& a_center, float a_radius)
	{
		std::vector<std::pair<float, RE::NiPointer<RE::TESObjectREFR>>> found;
		ForEachLoadedCell([&](RE::TESObjectCELL* a_cell) {
			a_cell->ForEachReferenceInRange(a_center, a_radius, [&](RE::TESObjectREFR* a_ref) {
				if (a_ref && !a_ref->IsActor() && !a_ref->IsDisabled() && !a_ref->IsDeleted() && a_ref->Get3D() &&
					std::ranges::none_of(found, [&](const auto& f) { return f.second.get() == a_ref; }) && IsSupported(a_ref)) {
					found.emplace_back(a_ref->GetPosition().GetDistance(a_center), RE::NiPointer<RE::TESObjectREFR>(a_ref));
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		});
		std::ranges::sort(found, {}, &std::pair<float, RE::NiPointer<RE::TESObjectREFR>>::first);
		std::vector<RE::NiPointer<RE::TESObjectREFR>> out;
		for (auto& f : found) {
			out.push_back(std::move(f.second));
		}
		return out;
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

	// every placeholder, when a save loads (main thread; the game is loading textures then anyway), so pressing Play
	// never has to load one from a game task
	void PreloadSlotTextures()
	{
		int ready = 0, loading = 0;
		g_preloading = true;
		for (int i = 0; i < static_cast<int>(g_slotTextures.size()); ++i) {
			if (auto tex = LoadSlotTexture(i)) {
				(tex->rendererTexture ? ready : loading) += 1;
			}
		}
		g_preloading = false;
		log::info("placeholder textures: {} ready, {} still loading", ready, loading);
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
			log::info("slot {}: placeholder still loading, will verify before use", a_slot);
			g_slotTextures[a_slot] = fallback;
			return fallback;
		}

		log::error("slot {}: could not load placeholder texture '{}'. Is Data\\Textures\\RealVideos installed?", a_slot, file);
		return {};
	}

	namespace
	{
		// 0.10 changed the automatic orientation and side of the VotW projector screens; turns saved for them
		// with older versions would now point the wrong way, so forget those once (mesh choices are kept).
		void MigrateChoices()
		{
			static bool done = false;
			if (done) {
				return;
			}
			done = true;
			if (::GetPrivateProfileIntA("Migration", "ProjectorTurns010", 0, kChoiceIni) != 0) {
				return;
			}
			std::vector<char> buf(65536);
			const auto        len = ::GetPrivateProfileSectionA("Screens.v4", buf.data(), static_cast<DWORD>(buf.size()), kChoiceIni);
			int               reset = 0;
			for (const char* line = buf.data(); line < buf.data() + len && *line; line += std::strlen(line) + 1) {
				const std::string entry = line;
				const auto        eq = entry.find('=');
				if (eq == std::string::npos) {
					continue;
				}
				const auto key = entry.substr(0, eq);
				auto       value = entry.substr(eq + 1);
				const auto b1 = value.find('|');
				const auto b2 = b1 == std::string::npos ? std::string::npos : value.find('|', b1 + 1);
				if (b2 == std::string::npos) {
					continue;
				}
				const auto name = Lower(value.substr(b1 + 1, b2 - b1 - 1));
				const auto b3 = value.find('|', b2 + 1);
				const auto flip = value.substr(b2 + 1, b3 == std::string::npos ? std::string::npos : b3 - b2 - 1);
				if ((name.starts_with("ref_attach_node") || name == "box001") && flip != "0" && !flip.empty()) {
					value = value.substr(0, b2 + 1) + "0" + (b3 == std::string::npos ? std::string() : value.substr(b3));
					::WritePrivateProfileStringA("Screens.v4", key.c_str(), value.c_str(), kChoiceIni);
					++reset;
				}
			}
			::WritePrivateProfileStringA("Migration", "ProjectorTurns010", "1", kChoiceIni);
			log::info("reset {} saved projector-screen turn(s) from older versions", reset);
		}
	}

	ScreenChoice LoadChoice(RE::TESObjectREFR* a_ref)
	{
		MigrateChoices();
		ScreenChoice choice;
		const auto   key = ModelKey(a_ref);
		if (key.empty()) {
			return choice;
		}
		char buf[512]{};
		::GetPrivateProfileStringA("Screens.v4", key.c_str(), "", buf, sizeof(buf), kChoiceIni);
		const std::string value = buf;
		if (value.empty()) {
			return choice;
		}
		// index|name|flip|owner   (owner = "plugin.esp/0001234" of a nearby object, or empty)
		std::vector<std::string> parts;
		std::size_t              start = 0;
		while (true) {
			const auto bar = value.find('|', start);
			parts.push_back(value.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
			if (bar == std::string::npos) {
				break;
			}
			start = bar + 1;
		}
		try {
			choice.index = std::stoi(parts[0]);
			if (parts.size() > 1) {
				choice.name = parts[1];
			}
			if (parts.size() > 2 && !parts[2].empty()) {
				choice.flip = std::stoi(parts[2]) & 15;
			}
			if (parts.size() > 3) {
				choice.owner = parts[3];
				std::ranges::replace(choice.owner, '/', '|');
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
		auto owner = a_choice.owner;
		std::ranges::replace(owner, '|', '/');
		const auto value = std::format("{}|{}|{}|{}", a_choice.index, a_choice.name, a_choice.flip, owner);
		::WritePrivateProfileStringA("Screens.v4", key.c_str(), value.c_str(), kChoiceIni);
		log::info("saved screen choice for {}: {}", key, value);
	}

	int CountCandidates(RE::TESObjectREFR* a_ref)
	{
		return static_cast<int>(Candidates(a_ref, true).size());
	}

	int CountOwnCandidates(RE::TESObjectREFR* a_ref)
	{
		return static_cast<int>(Candidates(a_ref, false).size());
	}

	namespace
	{
		void HideLayer(ScreenBinding& a_binding, RE::NiAVObject* a_obj)
		{
			if (!a_obj || std::ranges::any_of(a_binding.hiddenLayers, [&](const auto& h) { return h.get() == a_obj; })) {
				return;
			}
			a_binding.hiddenLayerWasCulled.push_back(a_obj->GetAppCulled());
			a_binding.hiddenLayers.emplace_back(a_obj);
			a_obj->SetAppCulled(true);
		}

		// hide the glowing (effect shader) layers drawn over a_geom: a TV's static / glass, the drive-in movie's stills
		// a_tvLayers: also a TV's glass / static layers whatever their shader (the glass is often a reflective lit layer)
		void HideLayersOver(ScreenBinding& a_binding, RE::BSGeometry* a_geom, RE::NiAVObject* a_root, bool a_tvLayers = false)
		{
			if (!a_geom || !a_root) {
				return;
			}
			std::vector<Candidate> layers;
			Collect(a_root, {}, layers);
			const auto& mine = a_geom->worldBound;
			for (auto& c : layers) {
				auto* other = c.geom;
				auto* otherProp = GetShaderProperty(other);
				if (other == a_geom || other->GetAppCulled() || !otherProp || !otherProp->material) {
					continue;
				}
				const auto nameLower = Lower(c.name);
				const bool tvLayer = a_tvLayers && (ContainsAny(nameLower, { "glass", "noise", "static" }) ||
				                                       ContainsAny(Lower(BaseTextureName(otherProp)), { "noise", "static" }));
				if (!tvLayer && otherProp->material->GetType() != RE::BSShaderMaterial::Type::kEffect) {
					continue;
				}
				const auto& theirs = other->worldBound;
				const float reach = std::max(mine.fRadius, theirs.fRadius) * 1.2f;
				if (mine.center.GetDistance(theirs.center) <= reach) {
					HideLayer(a_binding, other);
				}
			}
		}

	}

	namespace
	{
		// A fresh NiAlphaProperty: blending on, ONE/ZERO (opaque, just moves the mesh to the late pass) or
		// SRC_ALPHA/INV_SRC_ALPHA (see-through where the texture's alpha is 0): it only moves the screen into the late
		// blended pass. Built by hand like the game does (zeroed object + vtable) since no constructor is exposed.
		RE::NiAlphaProperty* NewBlendAlpha(bool a_seeThrough)
		{
			auto* mem = static_cast<std::byte*>(RE::aligned_alloc(alignof(RE::NiAlphaProperty), sizeof(RE::NiAlphaProperty)));
			if (!mem) {
				return nullptr;
			}
			std::memset(mem, 0, sizeof(RE::NiAlphaProperty));
			const auto vtbl = FindVtable(RE::NiAlphaProperty::VTABLE[0], RE::NiAlphaProperty::RTTI);
			if (!vtbl) {
				RE::aligned_free(mem);
				return nullptr;
			}
			*reinterpret_cast<std::uintptr_t*>(mem) = vtbl;
			REL::Relocation<std::uint32_t*> objects{ RE::ID::NiRefObject::Objects };
			::InterlockedIncrement(reinterpret_cast<volatile LONG*>(objects.get()));  // the game's live object count (its dtor lowers it)
			auto* alpha = reinterpret_cast<RE::NiAlphaProperty*>(mem);
			// bit 0 blend on, bits 1-4 source, bits 5-8 destination
			using AF = RE::NiAlphaProperty::AlphaFunction;
			const auto src = a_seeThrough ? AF::kSrcAlpha : AF::kOne;
			const auto dst = a_seeThrough ? AF::kInvSrcAlpha : AF::kZero;
			alpha->flags.flags = static_cast<std::uint16_t>(0x1 | (static_cast<std::uint16_t>(src) << 1) | (static_cast<std::uint16_t>(dst) << 5));
			alpha->alphaTestRef = 0;
			return alpha;
		}

		// Draw a lit screen with a glowing (effect) shader carrying the video, like a TV, so its brightness doesn't
		// depend on the time of day or which way it faces. The screen's own property is put back on Unbind.
		// The original alpha property lets the texture's alpha cut or blend the mesh: then the see-through areas of
		// the face-only glow would show up on the lit screen too.
		// BSOrderedNode ("OrderedRenderingNode" in VotW meshes) draws its children as one group, in order. Putting a
		// blended mesh in there dragged its neighbours (the screen's metal posts, frame) into the blended pass with
		// different lighting: they came out bright.
		bool IsOrderedNode(RE::NiAVObject* a_obj)
		{
			if (!a_obj) {
				return false;
			}
			for (const RE::NiRTTI* rtti = a_obj->GetRTTI(); rtti; rtti = rtti->GetBaseRTTI()) {
				if (rtti->GetName() && std::string_view(rtti->GetName()) == "BSOrderedNode") {
					return true;
				}
			}
			return Lower(Str(a_obj->name)).find("orderedrenderingnode") != std::string::npos;
		}

		bool UsesAlpha(RE::BSGeometry* a_geom)
		{
			auto* alpha = a_geom ? static_cast<RE::NiAlphaProperty*>(a_geom->properties[0].get()) : nullptr;
			return alpha && (alpha->GetAlphaBlending() || alpha->GetAlphaTesting());
		}

		// Hang a mesh under another node without moving it in the world.
		void Rehang(RE::BSGeometry* a_geom, RE::NiNode* a_to, const RE::NiTransform* a_local)
		{
			RE::NiPointer<RE::BSGeometry> hold(a_geom);  // detaching drops the parent's reference
			const RE::NiTransform        world = a_geom->world;
			if (auto* from = a_geom->parent) {
				from->DetachChild(a_geom);
			}
			if (a_local) {
				a_geom->local = *a_local;
			} else {
				const auto& A = a_to->world;
				const float s = A.scale != 0.0f ? A.scale : 1.0f;
				const auto  inv = A.rotate.Transpose();
				a_geom->local.rotate = inv * world.rotate;
				a_geom->local.translate = inv * ((world.translate - A.translate) / s);
				a_geom->local.scale = world.scale / s;
			}
			a_geom->world = world;
			a_to->AttachChild(a_geom, true);
		}

		// Put a lifted screen back into its ordered group, in its old place.
		void Unlift(ScreenBinding& a_binding, RE::BSGeometry* a_geom)
		{
			if (a_binding.liftedFrom && a_geom && a_geom->parent == a_binding.liftedTo.get()) {
				Rehang(a_geom, a_binding.liftedFrom.get(), &a_binding.liftedLocal);
			}
			a_binding.liftedFrom.reset();
			a_binding.liftedTo.reset();
		}

		// Glow map: swap the screen's plain lit material for a glow-map one (the kind the game uses for neon signs)
		// carrying the same textures plus our glow texture (black, with the video where the picture is). The mesh
		// stays lit by the world, so its frame and back react to light like any other object; only the picture
		// gives off light. Nothing here is undone by the game; Unbind puts the plain material back.
		bool MakeGlowMap(ScreenBinding& a_binding, RE::BSShaderProperty* a_prop, RE::BSShaderMaterial* a_mat, RE::NiTexture* a_glowTexture)
		{
			using F = RE::BSShaderProperty::EShaderPropertyFlag;
			if (!a_glowTexture || a_mat->GetType() != RE::BSShaderMaterial::Type::kLighting ||
				a_mat->GetFeature() != RE::BSShaderMaterial::Feature::kDefault) {
				log::info("'{}': glow map not possible here (material kind {}); using the glowing shader", a_binding.geometryName,
					static_cast<int>(a_mat->GetFeature()));
				return false;
			}
			// a new glow-map material, made the way the game makes one (its own class's Create)
			const auto glowVtbl = reinterpret_cast<std::uintptr_t*>(
				FindVtable(RE::VTABLE::BSLightingShaderMaterialGlowmap[0], RE::RTTI::BSLightingShaderMaterialGlowmap));
			const auto litVtbl = reinterpret_cast<std::uintptr_t*>(FindVtable(RE::VTABLE::BSLightingShaderMaterial[0], RE::RTTI::BSLightingShaderMaterial));
			if (!glowVtbl || !litVtbl) {
				log::warn("'{}': glow map not available on this game version; using the glowing shader", a_binding.geometryName);
				return false;
			}
			using create_t = RE::BSShaderMaterial* (*)(RE::BSShaderMaterial*);
			auto* glowMat = reinterpret_cast<create_t>(glowVtbl[1])(a_mat);
			if (!glowMat || glowMat->GetFeature() != RE::BSShaderMaterial::Feature::kGlowmap) {
				log::warn("'{}': couldn't make a glow-map material; using the glowing shader", a_binding.geometryName);
				return false;
			}
			// the screen's own settings and textures (the plain lit material's copy: only the shared part)
			using copy_t = void (*)(RE::BSShaderMaterial*, const RE::BSShaderMaterial*);
			reinterpret_cast<copy_t>(litVtbl[2])(glowMat, a_mat);
			GlowTexture(glowMat) = RE::NiPointer<RE::NiTexture>(a_glowTexture);
			glowMat->IncRef();
			// No shine on the picture: the screen's material can be glossy, and lights (even ones out of sight, or the
			// moon) then put a bright white spot on the video that moves as you walk around. Only our copy is changed,
			// so the screen looks as before once the video stops.
			{
				auto* base = static_cast<RE::BSLightingShaderMaterialBase*>(glowMat);
				base->specularColor = RE::NiColor{ 0.0f, 0.0f, 0.0f };
				base->specularColorScale = 0.0f;
			}

			auto* lit = static_cast<RE::BSLightingShaderProperty*>(a_prop);
			static RE::NiColor white{ 1.0f, 1.0f, 1.0f };
			a_binding.originalEmitColor = lit->emitColor;
			a_binding.originalEmitScale = lit->emitColorScale;
			a_binding.originalGlowFlag = a_prop->flags.all(F::kGlowMap);
			a_binding.originalOwnEmitFlag = a_prop->flags.all(F::kOwnEmit);

			a_prop->material = glowMat;  // the property's reference to the plain material is kept for Unbind
			lit->emitColor = &white;
			lit->emitColorScale = 1.0f;
			a_prop->SetFlag(F::kGlowMap, true);
			a_prop->SetFlag(F::kOwnEmit, true);
			RebuildRenderPasses(a_prop);
			a_binding.glowMap = true;
			a_binding.glowMaterial = glowMat;
			log::info("'{}': glow map: the frame keeps the game's lighting, only the picture gives off light", a_binding.geometryName);
			return true;
		}

		// The drive-in screen and other huge screens. Its world size can read small for a moment after build mode
		// (the cinema then lost its stretch-to-fill), so the mesh's own size and the drive-in's name count too.
		bool IsBigScreen(RE::BSGeometry* a_geom)
		{
			const auto& settings = Settings::Get();
			if (!a_geom || settings.bigScreenRadius <= 0.0f) {
				return false;
			}
			const float size = std::max(a_geom->worldBound.fRadius, a_geom->modelBound.fRadius * a_geom->world.scale);
			if (size >= settings.bigScreenRadius) {
				return true;
			}
			for (RE::NiAVObject* node = a_geom; node; node = node->parent) {
				if (Lower(Str(node->name)).find("drivein") != std::string::npos) {
					return true;
				}
			}
			return false;
		}

		void MakeSelfLit(ScreenBinding& a_binding, RE::BSGeometry* a_geom, RE::BSShaderProperty* a_litProp, RE::BSShaderMaterial* a_litMat,
			RE::NiTexture* a_slotTexture, bool a_faceOnly)
		{
			using F = RE::BSShaderProperty::EShaderPropertyFlag;
			auto* created = RE::BSEffectShaderProperty::Create();
			if (!created) {
				return;
			}
			RE::NiPointer<RE::NiProperty> keep(created);
			if (!created->material || created->material->GetType() != RE::BSShaderMaterial::Type::kEffect) {
				log::warn("'{}': no effect material to glow with; staying lit by the world", a_binding.geometryName);
				return;
			}
			created->SetMaterial(created->material, true);  // our own copy
			auto* em = static_cast<RE::BSEffectShaderMaterial*>(created->material);
			if (!em) {
				return;
			}
			em->baseTexture = RE::NiPointer<RE::NiTexture>(a_slotTexture);
			em->baseColor = RE::NiColorA{ 1.0f, 1.0f, 1.0f, 1.0f };
			em->baseColorScale = 1.0f;
			for (int i = 0; i < 2; ++i) {
				em->texCoordOffset[i] = a_litMat->texCoordOffset[i];
				em->texCoordScale[i] = a_litMat->texCoordScale[i];
			}
			created->SetFlag(F::kZBufferTest, true);
			created->SetFlag(F::kZBufferWrite, true);
			created->SetFlag(F::kSkinned, a_litProp->flags.all(F::kSkinned));
			created->SetFlag(F::kTwoSided, a_litProp->flags.all(F::kTwoSided));
			created->SetFlag(F::kEffectLighting, false);
			// no soft-particle fading (it faded the picture to see-through wherever something stood just behind
			// the screen, e.g. the drive-in's catwalk along its top), no falloff, no alpha tricks
			created->SetFlag(F::kSoftEffect, false);
			created->SetFlag(F::kFalloff, false);
			created->SetFlag(F::kRGBFalloff, false);
			created->SetFlag(F::kVertexAlpha, false);
			created->SetFlag(F::kScreendoorAlphaFade, false);
			created->SetFlag(F::kRefraction, false);
			created->SetFlag(F::kEnvMap, false);
			em->softFalloffDepth = 0.0f;
			em->falloffStartOpacity = 1.0f;
			em->falloffStopOpacity = 1.0f;
			created->SetFlag(F::kGrayscaleToPaletteColor, false);
			created->SetFlag(F::kGrayscaleToPaletteAlpha, false);

			if (a_faceOnly) {
				// a copy of the mesh, nudged a hair in front of the video face, glowing only where the video is.
				// The real screen keeps its own lit shader: its back, frame and pole look exactly as without video.
				// hang the copy outside any ordered-rendering group (see IsOrderedNode)
				RE::NiNode* parent = a_geom->parent;
				while (parent && parent->parent && IsOrderedNode(parent)) {
					parent = parent->parent;
				}
				RE::NiPointer<RE::NiObject> copyObj(parent ? a_geom->Clone() : nullptr);
				auto* copy = copyObj ? copyObj->IsGeometry() : nullptr;
				if (!copy) {
					log::warn("'{}': couldn't copy the screen for the face-only glow; the whole mesh glows", a_binding.geometryName);
				} else {
					created->SetFlag(F::kZBufferWrite, false);
					RE::NiPointer<RE::NiProperty> alpha(NewBlendAlpha(true));
					if (!alpha) {
						return;
					}
					copy->properties[0] = alpha;
					if (!created->SetupGeometry(copy)) {
						log::warn("'{}': the glowing shader doesn't fit this mesh; staying lit by the world", a_binding.geometryName);
						return;
					}
					// The copy is blended by the picture's alpha (1 on the video, 0 elsewhere). Setup switches vertex
					// colours on when the mesh has them, and their alpha (often 0 on these meshes) then hid the whole
					// copy. Nothing but the texture may decide what's see-through.
					const auto flagsBefore = created->flags.underlying();
					created->SetFlag(F::kVertexAlpha, false);
					created->SetFlag(F::kVertexColors, false);
					created->alpha = 1.0f;
					em->baseColor = RE::NiColorA{ 1.0f, 1.0f, 1.0f, 1.0f };
					log::info("'{}': glowing copy shader flags {:#x} -> {:#x}", a_binding.geometryName, flagsBefore, created->flags.underlying());
					copy->properties[1] = keep;
					copy->name = RE::BSFixedString(kGlowName);
					RE::NiPoint3 offset{};
					if (a_binding.haveGlowNormal) {
						offset = a_binding.glowNormal * 0.3f;  // world units, about 4 mm: no flicker against the real face
					}
					// placed by its world position, relative to the node it hangs from (may be higher up than the screen's own)
					const auto& A = parent->world;
					const auto& G = a_geom->world;
					const float parentScale = A.scale != 0.0f ? A.scale : 1.0f;
					const auto  inv = A.rotate.Transpose();
					copy->local.rotate = inv * G.rotate;
					copy->local.translate = inv * ((G.translate + offset - A.translate) / parentScale);
					copy->local.scale = G.scale / parentScale;
					copy->world = G;
					copy->world.translate += offset;
					copy->worldBound = a_geom->worldBound;
					copy->worldBound.center += offset;
					copy->SetAppCulled(false);
					parent->AttachChild(copy, true);
					a_binding.overlay = RE::NiPointer<RE::NiAVObject>(copy);
					a_binding.overlayParent = RE::NiPointer<RE::NiNode>(parent);
					a_binding.glowOverlay = true;
					a_binding.effectProp = keep;
					a_binding.effectMaterial = em;
					a_binding.selfLit = true;
					RebuildRenderPasses(created);
					log::info("'{}': only the picture glows (glowing copy under '{}'{})", a_binding.geometryName, Str(parent->name),
						parent != a_geom->parent ? ", outside the ordered group" : "");
					return;
				}
			}
			// late pass: an effect shader drawn with the opaque world got painted over by whatever stands behind the
			// screen (the drive-in's catwalk showed through the top of the picture at night)
			// A blended mesh inside an ordered-rendering group (VotW screens) dragged its neighbours (metal posts, frame)
			// into the late pass, where they came out bright: the screen is lifted out of the group while it glows.
			const bool latePass = Settings::Get().selfLitLatePass;
			if (latePass && IsOrderedNode(a_geom->parent)) {
				RE::NiNode* host = a_geom->parent;
				while (host && host->parent && IsOrderedNode(host)) {
					host = host->parent;
				}
				if (host && host != a_geom->parent && !IsOrderedNode(host)) {
					a_binding.liftedFrom = RE::NiPointer<RE::NiNode>(a_geom->parent);
					a_binding.liftedTo = RE::NiPointer<RE::NiNode>(host);
					a_binding.liftedLocal = a_geom->local;
					Rehang(a_geom, host, nullptr);
					log::info("'{}': lifted out of its ordered group (now under '{}') while it glows", a_binding.geometryName, Str(host->name));
				}
			}
			RE::NiPointer<RE::NiProperty> alpha;
			if (latePass && !IsOrderedNode(a_geom->parent)) {
				alpha = RE::NiPointer<RE::NiProperty>(NewBlendAlpha(false));
			}
			RE::NiPointer<RE::NiProperty> previousAlpha = a_geom->properties[0];
			if (alpha) {
				a_geom->properties[0] = alpha;
			}
			if (!created->SetupGeometry(a_geom)) {
				a_geom->properties[0] = previousAlpha;
				Unlift(a_binding, a_geom);
				log::warn("'{}': the glowing shader doesn't fit this mesh; staying lit by the world", a_binding.geometryName);
				return;
			}
			if (alpha) {
				a_binding.alphaSwapped = true;
				a_binding.originalAlpha = previousAlpha;
				a_binding.ourAlpha = alpha;
			}
			a_binding.originalProp = a_geom->properties[1];
			a_geom->properties[1] = keep;
			a_binding.effectProp = keep;
			a_binding.effectMaterial = em;
			a_binding.selfLit = true;
			RebuildRenderPasses(created);
			log::info("'{}': the whole mesh glows{}", a_binding.geometryName, alpha ? " (drawn after the world)" : "");
		}
	}

	bool Bind(RE::TESObjectREFR* a_ref, RE::NiTexture* a_slotTexture, ScreenBinding& a_binding, int a_forceIndex, RE::NiTexture* a_glowTexture)
	{
		a_binding = {};
		if (!a_ref || !a_slotTexture) {
			return false;
		}
		auto* root = a_ref->Get3D();
		if (!root) {
			return false;
		}
		if (IsMediaPlayer(a_ref)) {
			// also covers videos saved on one before this was added, and Papyrus calls
			log::info("{}: a media player (Radiation Queen), not a screen; nothing played on it", RefName(a_ref));
			return false;
		}

		auto candidates = Candidates(a_ref, true);
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
			auto matches = [&](int i) {
				return candidates[i].name == saved.name && (saved.owner.empty() || candidates[i].ownerKey == saved.owner);
			};
			if (saved.index < count && matches(saved.index)) {
				index = saved.index;
			} else {
				for (int i = 0; i < count; ++i) {
					if (matches(i)) {
						index = i;
						break;
					}
				}
			}
		}
		if (index >= 0 && a_forceIndex < 0 && !candidates[index].nearby && Score(candidates[index]) < 0) {
			index = -1;  // an old choice of a light beam or similar: pick again
		}
		// TVs always play on their ScreenAnim mesh (vanilla and VotW TVs), whatever was saved; F7 can still
		// move it for the moment
		if (a_forceIndex < 0 && settings.classicTV) {
			for (int i = 0; i < count; ++i) {
				const auto lower = Lower(candidates[i].name);
				auto*      p = GetShaderProperty(candidates[i].geom);
				if (!candidates[i].nearby && lower.starts_with("screenanim") && lower.find("noise") == std::string::npos && p && p->material &&
					p->material->GetType() == RE::BSShaderMaterial::Type::kEffect) {
					index = i;
					break;
				}
			}
		}
		if (index < 0 && !settings.forceNode.empty()) {
			for (int i = 0; i < count; ++i) {
				if (candidates[i].nearby) {
					continue;
				}
				if (_stricmp(candidates[i].name.c_str(), settings.forceNode.c_str()) == 0) {
					index = i;
					break;
				}
			}
		}
		if (index < 0) {
			index = BestCandidate(candidates);
			const int ownScore = index >= 0 ? Score(candidates[index]) : std::numeric_limits<int>::min();
			const bool ownHint = index >= 0 && HasVideoHint(candidates[index]);
			if (ownScore >= 10 && !ownHint) {
				// a plain screen with a film/video surface right in front of it (drive-in screen + Far Harbor movie):
				// play on the film surface
				const auto origin = a_ref->GetPosition();
				int        best = -1, bestScore = std::numeric_limits<int>::min();
				for (int i = 0; i < count; ++i) {
					const auto& c = candidates[i];
					if (c.nearby && HasVideoHint(c) && origin.GetDistance(c.owner->GetPosition()) < 3000.0f) {
						const int sc = Score(c);
						if (sc > bestScore) {
							best = i;
							bestScore = sc;
						}
					}
				}
				if (best >= 0) {
					log::info("{}: has a film surface in front of it, using {}", RefName(a_ref), RefName(candidates[best].owner));
					index = best;
				}
			} else if (ownScore < 10) {
				// a projector (or anything without a screen of its own): use the best screen nearby
				int best = -1, bestScore = kScreenLike - 1;
				for (int i = 0; i < count; ++i) {
					if (candidates[i].nearby) {
						// a film/video surface beats a plain screen behind it (Far Harbor's movie over the drive-in screen)
						const int sc = Score(candidates[i]) + (HasVideoHint(candidates[i]) ? 100 : 0);
						if (sc > bestScore) {
							best = i;
							bestScore = sc;
						}
					}
				}
				if (best >= 0) {
					log::info("{}: has no screen of its own, using nearby {}", RefName(a_ref), RefName(candidates[best].owner));
					index = best;
				}
			}
		}
		// The drive-in cinema always plays on its Screen_3 mesh, whether the screen, its projector or its film
		// was selected, and whatever mesh was picked before.
		{
			bool cinema = false;
			for (const auto& c : candidates) {
				if (c.nearby) {
					continue;
				}
				const auto name = Lower(c.name);
				const auto tex = Lower(BaseTextureName(GetShaderProperty(c.geom)));
				const auto root = Lower(c.root ? Str(c.root->name) : std::string());
				cinema |= root.find("drivein") != std::string::npos || name.find("projectorlightbeam") != std::string::npos ||
				          name.find("texholder") != std::string::npos || name.starts_with("moviebg") || tex.find("moviefx") != std::string::npos;
			}
			if (cinema) {
				int screen3 = -1;
				for (int i = 0; i < count; ++i) {
					if (_stricmp(candidates[i].name.c_str(), "Screen_3") == 0) {
						screen3 = i;  // own mesh first, then the nearest drive-in screen
						break;
					}
				}
				if (screen3 >= 0 && screen3 != index) {
					log::info("{}: drive-in cinema: using '{}' of {}", RefName(a_ref), candidates[screen3].name, RefName(candidates[screen3].owner));
					index = screen3;
				}
			}
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

		const auto origTexLower = Lower(BaseTextureName(prop));
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

		// TV screens (glowing effect-shader screens such as ScreenAnim): exactly like 0.1 - the video covers the
		// whole texture and the mesh's own UVs pick the part that shows. No face detection, bars or hiding.
		const bool filmSurface = ContainsAny(origTexLower, { "moviefx", "drivein" }) ||
		                         ContainsAny(Lower(candidates[index].name), { "drivein", "texholder" });
		const bool isTV = std::ranges::any_of(candidates, [](const Candidate& c) {
			const auto lower = Lower(c.name);
			return !c.nearby && lower.starts_with("screenanim") && lower.find("noise") == std::string::npos;
		});
		a_binding.classic = settings.classicTV && isTV && a_binding.isEffect && !candidates[index].nearby && !filmSurface;
		const int userFlip = a_forceIndex >= 0 || saved.index >= 0 ? saved.flip : 0;
		int       flip = userFlip & 3;
		MeshShape shape{};

		if (a_binding.classic) {
			const bool flipV = (userFlip & 1) != 0;  // Shift+F7 can still flip / mirror it
			const bool flipU = (userFlip & 2) != 0;
			a_binding.uvScale = { flipU ? -1.0f : 1.0f, flipV ? -1.0f : 1.0f };
			a_binding.uvOffset = { flipU ? 1.0f : 0.0f, flipV ? 1.0f : 0.0f };
			a_binding.overrideUV = true;
			for (int i = 0; i < 2; ++i) {
				mat->texCoordOffset[i] = a_binding.uvOffset;
				mat->texCoordScale[i] = a_binding.uvScale;
			}
			a_binding.screenAspect = 0.0f;  // stretch to fill, no black bars
			// the vanilla TV filter: its palette tint and the static / glass layers drawn over the picture
			if (settings.tvClean) {
				a_binding.originalFlags = prop->flags.underlying();
				prop->SetFlag(RE::BSShaderProperty::EShaderPropertyFlag::kGrayscaleToPaletteColor, false);
				prop->SetFlag(RE::BSShaderProperty::EShaderPropertyFlag::kGrayscaleToPaletteAlpha, false);
				a_binding.flagsChanged = true;
				HideLayersOver(a_binding, geom, candidates[index].root, true);
			}
		} else {
			// stop UV/flipbook animation controllers (Videos of the Wasteland scrolls its screens this way)
			if (settings.stopScreenAnimation && prop->controllers) {
				a_binding.savedControllers = prop->controllers;
				prop->controllers.reset();
			}

			// work out the screen face, which way is up, and whether the frame shares the texture
			shape = AnalyzeMesh(geom, TextureAspect(a_binding.originalBase.get()));
			// VotW's controlled ceiling projector screen is lowered by bones; its stored pose can't say which way is
			// up, and the picture came out upside down
			// (clean and dirty versions alike): turn it 180 degrees
			if (origTexLower.find("ceilingprojectorscreen") != std::string::npos && Lower(candidates[index].name).starts_with("ref_attach_node")) {
				shape.flipHint = 3;
				shape.faceOrient[0] = 3;
				shape.faceOrient[1] = 3;
				log::info("{}: controlled ceiling projector screen: turned 180 degrees", RefName(a_ref));
			}
			// video posters: the poster texture is a tile of a sheet whose shape made the automatic check think the
			// picture lies on its side (it turned one poster model each way); their UVs are upright as they are
			if (IsVideoPoster(a_ref)) {
				shape.flipHint = 0;
				shape.faceOrient[0] = 0;
				shape.faceOrient[1] = 0;
			}
			flip = (userFlip ^ shape.flipHint) & 7;
			const auto inTexture = [](const std::array<float, 4>& r) { return r[0] >= -0.01f && r[1] >= -0.01f && r[2] <= 1.01f && r[3] <= 1.01f; };
			const std::array<float, 4> whole{ shape.all.u0, shape.all.v0, shape.all.u1, shape.all.v1 };
			const bool rotated = (flip & 4) != 0;

			// which faces get the video: normally only the one facing you (Ctrl+Shift+F7 picks the other side)
			std::vector<int> chosen;
			for (int k = 0; k < shape.faces; ++k) {
				chosen.push_back(k);
			}
			// how far in front of each face the player is (negative = behind it)
			std::array<float, 2> facing{ 0.0f, 0.0f };
			bool                 sidesOpposite = true;
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				auto eye = player->GetPosition();
				eye.z += 120.0f;  // roughly eye height
				for (int k = 0; k < shape.faces && k < 2; ++k) {
					const auto c = shape.faceCenter[k];
					const auto n = shape.faceNormal[k];
					facing[k] = n.x * (eye.x - c.x) + n.y * (eye.y - c.y) + n.z * (eye.z - c.z);
				}
			}
			if (shape.faces == 2 && !settings.bothSides) {
				// the side facing you; remembered for this mesh so Shift+F7 / F7 don't jump to the other side
				const auto& n0 = shape.faceNormal[0];
				const auto& n1 = shape.faceNormal[1];
				sidesOpposite = n0.x * n1.x + n0.y * n1.y + n0.z * n1.z < -0.3f;
				// the smaller of the two faces is the front on every two-sided screen seen so far (VotW Radiation
				// Star, ceiling projector screens); where the player stands doesn't matter
				int front = 1;
				log::info("{}: sides: normals ({:.2f},{:.2f},{:.2f}) / ({:.2f},{:.2f},{:.2f}), you are {:.0f} / {:.0f} in front -> face {}{}", RefName(a_ref),
					n0.x, n0.y, n0.z, n1.x, n1.y, n1.z, facing[0], facing[1], (userFlip & 8) ? 1 : 2, (userFlip & 8) ? " (other side picked)" : "");
				if (userFlip & 8) {
					front = 1 - front;
				}
				chosen = { front };
			}
			// The drive-in screen's sheet faces away from the audience, so the picture came out mirrored. This used
			// to depend on where the player stood when the video started, which made it mirrored only some of the
			// time; now it's fixed for the drive-in (Ctrl+Shift+F7 undoes it) and nothing else is turned round.
			const auto rootLower = Lower(candidates[index].root ? Str(candidates[index].root->name) : std::string());
			const bool driveIn = rootLower.find("drivein") != std::string::npos;
			const bool fromBehind = shape.faces == 1 && driveIn != ((userFlip & 8) != 0);
			if (fromBehind) {
				flip ^= 2;
			}
			const bool facesInTexture = std::ranges::all_of(chosen, [&](int k) { return inTexture(shape.face[k]); }) && !chosen.empty();

			a_binding.screenAspect = chosen.empty() ? 0.0f : shape.faceAspect[chosen[0]];
			a_binding.faceCount = static_cast<int>(chosen.size());
			if (chosen.size() == 1) {
				a_binding.glowNormal = shape.faceNormal[chosen[0]];
				a_binding.haveGlowNormal = true;
			}
			for (std::size_t i = 0; i < chosen.size() && i < 2; ++i) {
				const int k = chosen[i];
				a_binding.faceRects[i] = shape.face[k];
				a_binding.faceAspect[i] = shape.faceAspect[k];
				a_binding.faceOrient[i] = (userFlip ^ shape.faceOrient[k] ^ (fromBehind ? 2 : 0)) & 7;
			}
			const bool otherSideHidden = shape.faces == 2 && chosen.size() == 1;
			if (settings.composite && (shape.partial || otherSideHidden) && facesInTexture) {
				a_binding.composite = true;  // frame shares the texture (or we skip the back): video only inside the face
			} else if (settings.composite && rotated && shape.all.ok && inTexture(whole)) {
				// whole mesh is the screen but its texture lies sideways: draw the video rotated
				a_binding.composite = true;
				a_binding.faceCount = 1;
				a_binding.faceRects[0] = whole;
				a_binding.faceAspect[0] = a_binding.screenAspect;
			}

			if (!a_binding.composite && settings.fitUV) {
				// map the mesh's UV range onto the whole video (shader does: uv' = uv * scale + offset)
				float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
				if (shape.all.ok) {
					u0 = shape.all.u0;
					v0 = shape.all.v0;
					u1 = shape.all.u1;
					v1 = shape.all.v1;
				}
				const bool  flipV = (flip & 1) != 0;
				const bool  flipU = (flip & 2) != 0;
				const float su = 1.0f / (u1 - u0);
				const float sv = 1.0f / (v1 - v0);
				a_binding.uvScale = { flipU ? -su : su, flipV ? -sv : sv };
				a_binding.uvOffset = { flipU ? u1 * su : -u0 * su, flipV ? v1 * sv : -v0 * sv };
				a_binding.overrideUV = true;
				for (int i = 0; i < 2; ++i) {
					mat->texCoordOffset[i] = a_binding.uvOffset;
					mat->texCoordScale[i] = a_binding.uvScale;
				}
			}

			// effect screens that colour their texture through a palette would tint the video
			if (a_binding.isEffect) {
				a_binding.originalFlags = prop->flags.underlying();
				prop->SetFlag(RE::BSShaderProperty::EShaderPropertyFlag::kGrayscaleToPaletteColor, false);
				prop->SetFlag(RE::BSShaderProperty::EShaderPropertyFlag::kGrayscaleToPaletteAlpha, false);
				a_binding.flagsChanged = true;
			}

			// glowing layers drawn over the same spot (a TV's noise/glass overlays, the stills and credits of the
			// drive-in movie) would cover the video: hide them while we play
			if (a_binding.isEffect && settings.hideOverlays) {
				HideLayersOver(a_binding, geom, candidates[index].root);
			}

			// the back of a two-sided sheet shows the same texture: stop drawing it so only the front has video
			using F = RE::BSShaderProperty::EShaderPropertyFlag;
			if (!settings.bothSides && a_binding.composite && shape.faces == 2 && a_binding.faceCount == 1 && prop->flags.all(F::kTwoSided)) {
				prop->SetFlag(F::kTwoSided, false);
				a_binding.twoSidedCleared = true;
			}
		}

		a_binding.geometryName = candidates[index].name;
		const bool tryGlowMap = settings.glowMap && settings.selfLit && !a_binding.isEffect && !a_binding.classic && a_binding.composite &&
		                        !IsBigScreen(geom) &&
		                        !prop->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kSkinned);
		if (tryGlowMap && MakeGlowMap(a_binding, prop, mat, a_glowTexture)) {
			// done: lit screen with a glowing picture
		} else if (settings.selfLit && !a_binding.isEffect && !a_binding.classic && geom->IsTriShape()) {
			// The glowing copy (picture only) never showed up in game, so it stays off whatever the INI says.
			const bool faceOnly = false && settings.selfLitFaceOnly && a_binding.composite && a_binding.faceCount >= 1 && !UsesAlpha(geom) &&
			                      !prop->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kSkinned);
			MakeSelfLit(a_binding, geom, prop, mat, a_slotTexture, faceOnly);
		}

		a_binding.owner = candidates[index].owner->GetHandle();
		a_binding.ownerKey = candidates[index].ownerKey;
		a_binding.ownerIsNearby = candidates[index].nearby;
		a_binding.root = RE::NiPointer<RE::NiAVObject>(candidates[index].root);
		a_binding.geometry = RE::NiPointer<RE::BSGeometry>(geom);
		a_binding.material = mat;
		a_binding.slotTexture = RE::NiPointer<RE::NiTexture>(a_slotTexture);
		a_binding.candidateIndex = index;
		a_binding.candidateCount = count;
		a_binding.flip = userFlip;
		a_binding.bigScreen = !a_binding.classic && IsBigScreen(geom);
		// big screens (the drive-in cinema) fill the whole screen instead of keeping black bars
		if (settings.posterStretch && IsVideoPoster(a_ref)) {
			a_binding.screenAspect = 0.0f;  // video poster: fill the whole poster, no black bars
			a_binding.faceAspect[0] = a_binding.faceAspect[1] = 0.0f;
			log::info("{}: video poster: stretching the video to fill it", RefName(a_ref));
		} else if (settings.bigScreenStretch && a_binding.bigScreen) {
			a_binding.screenAspect = 0.0f;
			a_binding.faceAspect[0] = a_binding.faceAspect[1] = 0.0f;
			log::info("{}: big screen (size {:.0f}): stretching the video to fill it", RefName(a_ref), geom->worldBound.fRadius);
		} else if (settings.projectorStretch && !a_binding.classic && !a_binding.bigScreen) {
			a_binding.screenAspect = 0.0f;  // projector: fill the screen, no black bars
			a_binding.faceAspect[0] = a_binding.faceAspect[1] = 0.0f;
		}
		RebuildRenderPasses(prop);
		a_binding.geometryName = candidates[index].name;
		a_binding.bound = true;
		if (a_binding.classic) {
			log::info("{}: bound TV screen {}/{} '{}' (effect shader, texture '{}', sRGB={}): whole video, mesh UVs untouched like 0.1, flip {}",
				RefName(a_ref), index + 1, count, candidates[index].name,
				a_binding.originalBase ? std::string(a_binding.originalBase->GetName()) : std::string("<none>"), a_binding.originalWasSRGB, flip);
			return true;
		}
		if (shape.faces == 2) {
			const auto& a = shape.face[0];
			const auto& b = shape.face[1];
			const float ix = std::min(a[2], b[2]) - std::max(a[0], b[0]);
			const float iy = std::min(a[3], b[3]) - std::max(a[1], b[1]);
			const float smaller = std::max(1e-6f, std::min((a[2] - a[0]) * (a[3] - a[1]), (b[2] - b[0]) * (b[3] - b[1])));
			const float overlap = ix > 0 && iy > 0 ? ix * iy / smaller : 0.0f;
			log::info("{}: two faces: [{:.3f},{:.3f}]-[{:.3f},{:.3f}] aspect {:.2f} orient {} / [{:.3f},{:.3f}]-[{:.3f},{:.3f}] aspect {:.2f} orient {}, texture overlap {:.0f}%, showing {}{}",
				RefName(a_ref), a[0], a[1], a[2], a[3], shape.faceAspect[0], shape.faceOrient[0], b[0], b[1], b[2], b[3], shape.faceAspect[1],
				shape.faceOrient[1], overlap * 100.0f, a_binding.faceCount == 2 ? "both" : (a_binding.faceRects[0] == a ? "first" : "second"),
				a_binding.twoSidedCleared ? ", back of sheet culled" : "");
		}

		log::info("{}: bound mesh {}/{} '{}'{} ({} shader{}, texture '{}', sRGB={}, mesh uv [{:.3f},{:.3f}]-[{:.3f},{:.3f}], screen face {} [{:.3f},{:.3f}]-[{:.3f},{:.3f}], {}, aspect {:.2f}, flip {} (auto {}){}, controllers {}, hidden layers {})",
			RefName(a_ref), index + 1, count, candidates[index].name,
			candidates[index].nearby ? std::format(" of nearby {}", RefName(candidates[index].owner)) : std::string(), a_binding.isEffect ? "effect" : "lighting",
			a_binding.hadGlow ? " + glowmap" : "",
			a_binding.originalBase ? std::string(a_binding.originalBase->GetName()) : std::string("<none>"), a_binding.originalWasSRGB,
			shape.all.u0, shape.all.v0, shape.all.u1, shape.all.v1, shape.faces, shape.face[0][0], shape.face[0][1], shape.face[0][2], shape.face[0][3],
			a_binding.composite ? "video only inside the face" : (a_binding.overrideUV ? "video over whole mesh" : "uv untouched"), a_binding.screenAspect,
			flip, shape.flipHint, shape.skinned ? ", skinned" : "", a_binding.savedControllers ? "stopped" : "none", a_binding.hiddenLayers.size());
		return true;
	}

	void Unbind(ScreenBinding& a_binding)
	{
		if (!a_binding.bound) {
			a_binding = {};
			return;
		}
		if (a_binding.glowOverlay && a_binding.overlay && a_binding.overlayParent && a_binding.overlay->parent == a_binding.overlayParent.get()) {
			a_binding.overlayParent->DetachChild(a_binding.overlay.get());  // the glowing copy goes away
		}
		if (a_binding.selfLit && !a_binding.glowOverlay && a_binding.geometry && a_binding.geometry->properties[1].get() == a_binding.effectProp.get()) {
			a_binding.geometry->properties[1] = a_binding.originalProp;  // the screen's own (lit) shader back
			if (a_binding.alphaSwapped && a_binding.geometry->properties[0].get() == a_binding.ourAlpha.get()) {
				a_binding.geometry->properties[0] = a_binding.originalAlpha;
			}
			if (auto* own = GetShaderProperty(a_binding.geometry.get())) {
				RebuildRenderPasses(own);
			}
		}
		Unlift(a_binding, a_binding.geometry.get());  // back into its ordered group
		if (a_binding.glowMap && a_binding.geometry) {
			using F = RE::BSShaderProperty::EShaderPropertyFlag;
			auto* gp = GetShaderProperty(a_binding.geometry.get());
			if (gp && gp->material == a_binding.glowMaterial) {
				auto* lit = static_cast<RE::BSLightingShaderProperty*>(gp);
				lit->emitColor = a_binding.originalEmitColor;
				lit->emitColorScale = a_binding.originalEmitScale;
				gp->SetFlag(F::kGlowMap, a_binding.originalGlowFlag);
				gp->SetFlag(F::kOwnEmit, a_binding.originalOwnEmitFlag);
				gp->material = a_binding.material;  // the plain material back (the reference it always held)
				GlowTexture(a_binding.glowMaterial).reset();
				(void)a_binding.glowMaterial->DecRef();
			}
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
			RebuildRenderPasses(prop);
			if (a_binding.twoSidedCleared) {
				prop->SetFlag(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided, true);
			}
			if (a_binding.flagsChanged) {
				const auto had = [&](RE::BSShaderProperty::EShaderPropertyFlag f) {
					return (a_binding.originalFlags & static_cast<std::uint64_t>(f)) != 0;
				};
				prop->SetFlag(RE::BSShaderProperty::EShaderPropertyFlag::kGrayscaleToPaletteColor, had(RE::BSShaderProperty::EShaderPropertyFlag::kGrayscaleToPaletteColor));
				prop->SetFlag(RE::BSShaderProperty::EShaderPropertyFlag::kGrayscaleToPaletteAlpha, had(RE::BSShaderProperty::EShaderPropertyFlag::kGrayscaleToPaletteAlpha));
			}
		}
		for (std::size_t i = 0; i < a_binding.hiddenLayers.size(); ++i) {
			if (auto& layer = a_binding.hiddenLayers[i]) {
				layer->SetAppCulled(i < a_binding.hiddenLayerWasCulled.size() && a_binding.hiddenLayerWasCulled[i]);
			}
		}
		if (a_binding.material) {
			// drop our ref; never free it ourselves (the engine owns it)
			(void)a_binding.material->DecRef();
		}
		a_binding = {};
	}

	bool StillValid(const ScreenBinding& a_binding)
	{
		if (!a_binding.bound) {
			return false;
		}
		auto owner = a_binding.owner.get();
		auto* a_currentRoot = owner ? owner->Get3D() : nullptr;
		if (!a_currentRoot || a_binding.root.get() != a_currentRoot) {
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
		if (a_binding.glowOverlay && (!a_binding.overlay || a_binding.overlay->parent != a_binding.overlayParent.get())) {
			return false;  // the glowing copy was dropped (3D rebuilt)
		}
		if (a_binding.selfLit && !a_binding.glowOverlay) {
			return prop && prop == a_binding.effectProp.get() && prop->material == a_binding.effectMaterial &&
			       static_cast<RE::BSEffectShaderMaterial*>(a_binding.effectMaterial)->baseTexture.get() == a_binding.slotTexture.get();
		}
		if (a_binding.glowMap) {
			return prop && prop->material == a_binding.glowMaterial &&
			       static_cast<RE::BSLightingShaderMaterialBase*>(a_binding.glowMaterial)->diffuseTexture.get() == a_binding.slotTexture.get();
		}
		if (!prop || prop->material != a_binding.material) {
			return false;
		}
		if (a_binding.isEffect) {
			return static_cast<RE::BSEffectShaderMaterial*>(a_binding.material)->baseTexture.get() == a_binding.slotTexture.get();
		}
		return static_cast<RE::BSLightingShaderMaterialBase*>(a_binding.material)->diffuseTexture.get() == a_binding.slotTexture.get();
	}

	void KeepOverlaysHidden(ScreenBinding& a_binding)
	{
		for (auto& layer : a_binding.hiddenLayers) {
			if (layer && !layer->GetAppCulled()) {
				layer->SetAppCulled(true);  // the film's own animation switched it back on
			}
		}
	}

	void ReapplyUV(const ScreenBinding& a_binding)
	{
		if (!a_binding.bound || !a_binding.material || !a_binding.overrideUV) {
			return;
		}
		auto* mat = a_binding.material;
		for (int i = 0; i < 2; ++i) {
			mat->texCoordOffset[i] = a_binding.uvOffset;
			mat->texCoordScale[i] = a_binding.uvScale;
			if (a_binding.glowMap && a_binding.glowMaterial) {
				a_binding.glowMaterial->texCoordOffset[i] = a_binding.uvOffset;
				a_binding.glowMaterial->texCoordScale[i] = a_binding.uvScale;
			}
			if (a_binding.selfLit && a_binding.effectMaterial) {
				a_binding.effectMaterial->texCoordOffset[i] = a_binding.uvOffset;
				a_binding.effectMaterial->texCoordScale[i] = a_binding.uvScale;
			}
		}
	}

	void SetGlow(const ScreenBinding& a_binding, float a_scale)
	{
		if (a_binding.bound && a_binding.glowMap && a_binding.geometry) {
			if (auto* gp = GetShaderProperty(a_binding.geometry.get()); gp && gp->material == a_binding.glowMaterial) {
				static_cast<RE::BSLightingShaderProperty*>(gp)->emitColorScale = std::clamp(a_scale, 0.0f, 16.0f);
			}
			return;
		}
		if (!a_binding.bound || !a_binding.selfLit || !a_binding.effectMaterial) {
			return;
		}
		auto* em = static_cast<RE::BSEffectShaderMaterial*>(a_binding.effectMaterial);
		em->baseColorScale = std::clamp(a_scale, 0.0f, 16.0f);
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

		const auto candidates = Candidates(a_ref, true);
		const int  best = BestCandidate(candidates);
		for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
			const auto& c = candidates[i];
			auto*       prop = GetShaderProperty(c.geom);
			auto*       mat = prop->material;
			const auto  shape = AnalyzeMesh(c.geom);
			const auto  uv = ReadUVBounds(c.geom);
			log::info("  [{}]{}{} '{}' path='{}' shader={} score={} tex='{}' meshUV={} [{:.3f},{:.3f}]-[{:.3f},{:.3f}] face={} [{:.3f},{:.3f}]-[{:.3f},{:.3f}]{} aspect={:.2f} matUV off=({:.2f},{:.2f}) scale=({:.2f},{:.2f}) controllers={}",
				i + 1, i == best ? " <== auto pick" : "", c.nearby ? std::format(" (nearby {} {})", RefName(c.owner), c.ownerKey) : std::string(), c.name, c.pathLower,
				mat->GetType() == RE::BSShaderMaterial::Type::kEffect ? "effect" : std::format("lighting/{}", static_cast<int>(mat->GetFeature())),
				Score(c), BaseTextureName(prop), uv.ok ? "ok" : "n/a", uv.u0, uv.v0, uv.u1, uv.v1, shape.faces, shape.face[0][0], shape.face[0][1],
				shape.face[0][2], shape.face[0][3], shape.partial ? " (part of texture)" : "", uv.aspect,
				mat->texCoordOffset[0].x, mat->texCoordOffset[0].y, mat->texCoordScale[0].x, mat->texCoordScale[0].y,
				prop->controllers ? "yes" : "no");
		}
		log::info("==== end dump ({} candidates) ====", candidates.size());

		// every mesh in the object, screen or not (frames, poles, lights...), and what draws it right now
		std::function<void(RE::NiAVObject*, int)> walk = [&](RE::NiAVObject* a_obj, int a_depth) {
			if (!a_obj || a_depth > 64) {
				return;
			}
			if (auto* geom = a_obj->IsGeometry()) {
				auto*       prop = GetShaderProperty(geom);
				auto*       mat = prop ? prop->material : nullptr;
				auto*       alpha = static_cast<RE::NiAlphaProperty*>(geom->properties[0].get());
				std::string kind = !mat ? "none" : mat->GetType() == RE::BSShaderMaterial::Type::kEffect ? "effect" : "lighting";
				log::info("  mesh '{}' shader={} tex='{}' alpha={} culled={} radius={:.0f}", Str(geom->name), kind, prop ? BaseTextureName(prop) : "",
					alpha ? std::format("blend={} test={} flags={:#x}", alpha->GetAlphaBlending(), alpha->GetAlphaTesting(), alpha->flags.flags) : "none",
					geom->GetAppCulled(), geom->worldBound.fRadius);
				return;
			}
			if (auto* node = a_obj->IsNode()) {
				for (auto& child : node->children) {
					walk(child.get(), a_depth + 1);
				}
			}
		};
		log::info("==== all meshes of {} ====", RefName(a_ref));
		walk(root, 0);
		log::info("==== end ====");
	}
}
