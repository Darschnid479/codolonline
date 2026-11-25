#if defined(DRAGON_PS4) || defined(__PS4__) || defined(__ORBIS__)

#include "renderer.hpp"
#include "software_rasterizer.hpp"

#include <orbis/Pigletv2VSH.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace dragon::gfx
{
	namespace
	{
		// Dynamic EGL + GLES2 function table resolved via sceKernelDlsym on libScePigletv2VSH.sprx
		struct PigletApi
		{
			int32_t mod_piglet = -1;
			int32_t mod_shacc = -1;

			bool (*scePigletSetConfigurationVSH)(const OrbisPglConfig*) = nullptr;
			EGLDisplay (*eglGetDisplay)(EGLNativeDisplayType) = nullptr;
			EGLBoolean (*eglInitialize)(EGLDisplay, EGLint*, EGLint*) = nullptr;
			EGLBoolean (*eglBindAPI)(EGLenum) = nullptr;
			EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = nullptr;
			EGLSurface (*eglCreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*) = nullptr;
			EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = nullptr;
			EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
			EGLBoolean (*eglSwapInterval)(EGLDisplay, EGLint) = nullptr;
			EGLBoolean (*eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
			EGLBoolean (*eglDestroyContext)(EGLDisplay, EGLContext) = nullptr;
			EGLBoolean (*eglDestroySurface)(EGLDisplay, EGLSurface) = nullptr;
			EGLBoolean (*eglTerminate)(EGLDisplay) = nullptr;

			const GLubyte* (*glGetString)(GLenum) = nullptr;
			void (*glViewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
			void (*glClearColor)(GLclampf, GLclampf, GLclampf, GLclampf) = nullptr;
			void (*glClear)(GLbitfield) = nullptr;
			void (*glEnable)(GLenum) = nullptr;
			void (*glDisable)(GLenum) = nullptr;
			void (*glDepthFunc)(GLenum) = nullptr;
			void (*glCullFace)(GLenum) = nullptr;
			void (*glFrontFace)(GLenum) = nullptr;
			void (*glGenBuffers)(GLsizei, GLuint*) = nullptr;
			void (*glDeleteBuffers)(GLsizei, const GLuint*) = nullptr;
			void (*glBindBuffer)(GLenum, GLuint) = nullptr;
			void (*glBufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
			void (*glGenTextures)(GLsizei, GLuint*) = nullptr;
			void (*glDeleteTextures)(GLsizei, const GLuint*) = nullptr;
			void (*glBindTexture)(GLenum, GLuint) = nullptr;
			void (*glActiveTexture)(GLenum) = nullptr;
			void (*glTexParameteri)(GLenum, GLenum, GLint) = nullptr;
			void (*glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
			GLuint (*glCreateShader)(GLenum) = nullptr;
			void (*glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
			void (*glCompileShader)(GLuint) = nullptr;
			void (*glGetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
			void (*glDeleteShader)(GLuint) = nullptr;
			GLuint (*glCreateProgram)(void) = nullptr;
			void (*glAttachShader)(GLuint, GLuint) = nullptr;
			void (*glLinkProgram)(GLuint) = nullptr;
			void (*glGetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
			void (*glUseProgram)(GLuint) = nullptr;
			void (*glDeleteProgram)(GLuint) = nullptr;
			GLint (*glGetAttribLocation)(GLuint, const GLchar*) = nullptr;
			GLint (*glGetUniformLocation)(GLuint, const GLchar*) = nullptr;
			void (*glUniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*) = nullptr;
			void (*glUniform3f)(GLint, GLfloat, GLfloat, GLfloat) = nullptr;
			void (*glUniform1i)(GLint, GLint) = nullptr;
			void (*glUniform1f)(GLint, GLfloat) = nullptr;
			void (*glEnableVertexAttribArray)(GLuint) = nullptr;
			void (*glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) = nullptr;
			void (*glDrawElements)(GLenum, GLsizei, GLenum, const void*) = nullptr;

			template <typename Fn>
			bool resolve(int32_t mod, const char* name, Fn& out)
			{
				void* sym = nullptr;
				if (sceKernelDlsym(mod, name, &sym) != 0 || !sym) return false;
				out = reinterpret_cast<Fn>(sym);
				return true;
			}

			bool try_load()
			{
				const char* sandbox = sceKernelGetFsSandboxRandomWord();
				std::string sys_prefix = sandbox ? ("/" + std::string(sandbox) + "/common/lib/") : std::string();
				const std::vector<std::string> search_dirs = {
					"/data/project_dragon/modules/",
					"/app0/sce_module/",
					sys_prefix
				};

				auto load_one = [&](const char* filename) -> int32_t {
					for (const auto& dir : search_dirs)
					{
						if (dir.empty()) continue;
						const std::string full = dir + filename;
						int32_t mret = 0;
						const uint32_t h = sceKernelLoadStartModule(full.c_str(), 0, nullptr, 0, nullptr, &mret);
						if (static_cast<int32_t>(h) >= 0)
						{
							platform::log_info("Loaded PS4 GPU module: %s (handle=%d)", full.c_str(), static_cast<int>(h));
							return static_cast<int32_t>(h);
						}
					}
					return -1;
				};

				mod_piglet = load_one("libScePigletv2VSH.sprx");
				if (mod_piglet < 0) return false;

				// Runtime GLSL compiler (optional if absent, fail cleanly back to VideoOut)
				mod_shacc = load_one("libSceShaccVSH.sprx");
				if (mod_shacc < 0)
				{
					platform::log_warn("libSceShaccVSH.sprx not found; runtime GLSL compilation unavailable, falling back to VideoOut");
					return false;
				}

				bool ok = true;
				ok &= resolve(mod_piglet, "scePigletSetConfigurationVSH", scePigletSetConfigurationVSH);
				ok &= resolve(mod_piglet, "eglGetDisplay", eglGetDisplay);
				ok &= resolve(mod_piglet, "eglInitialize", eglInitialize);
				ok &= resolve(mod_piglet, "eglBindAPI", eglBindAPI);
				ok &= resolve(mod_piglet, "eglChooseConfig", eglChooseConfig);
				ok &= resolve(mod_piglet, "eglCreateWindowSurface", eglCreateWindowSurface);
				ok &= resolve(mod_piglet, "eglCreateContext", eglCreateContext);
				ok &= resolve(mod_piglet, "eglMakeCurrent", eglMakeCurrent);
				ok &= resolve(mod_piglet, "eglSwapInterval", eglSwapInterval);
				ok &= resolve(mod_piglet, "eglSwapBuffers", eglSwapBuffers);
				ok &= resolve(mod_piglet, "eglDestroyContext", eglDestroyContext);
				ok &= resolve(mod_piglet, "eglDestroySurface", eglDestroySurface);
				ok &= resolve(mod_piglet, "eglTerminate", eglTerminate);
				ok &= resolve(mod_piglet, "glGetString", glGetString);
				ok &= resolve(mod_piglet, "glViewport", glViewport);
				ok &= resolve(mod_piglet, "glClearColor", glClearColor);
				ok &= resolve(mod_piglet, "glClear", glClear);
				ok &= resolve(mod_piglet, "glEnable", glEnable);
				ok &= resolve(mod_piglet, "glDisable", glDisable);
				ok &= resolve(mod_piglet, "glDepthFunc", glDepthFunc);
				ok &= resolve(mod_piglet, "glCullFace", glCullFace);
				ok &= resolve(mod_piglet, "glFrontFace", glFrontFace);
				ok &= resolve(mod_piglet, "glGenBuffers", glGenBuffers);
				ok &= resolve(mod_piglet, "glDeleteBuffers", glDeleteBuffers);
				ok &= resolve(mod_piglet, "glBindBuffer", glBindBuffer);
				ok &= resolve(mod_piglet, "glBufferData", glBufferData);
				ok &= resolve(mod_piglet, "glGenTextures", glGenTextures);
				ok &= resolve(mod_piglet, "glDeleteTextures", glDeleteTextures);
				ok &= resolve(mod_piglet, "glBindTexture", glBindTexture);
				ok &= resolve(mod_piglet, "glActiveTexture", glActiveTexture);
				ok &= resolve(mod_piglet, "glTexParameteri", glTexParameteri);
				ok &= resolve(mod_piglet, "glTexImage2D", glTexImage2D);
				ok &= resolve(mod_piglet, "glCreateShader", glCreateShader);
				ok &= resolve(mod_piglet, "glShaderSource", glShaderSource);
				ok &= resolve(mod_piglet, "glCompileShader", glCompileShader);
				ok &= resolve(mod_piglet, "glGetShaderiv", glGetShaderiv);
				ok &= resolve(mod_piglet, "glDeleteShader", glDeleteShader);
				ok &= resolve(mod_piglet, "glCreateProgram", glCreateProgram);
				ok &= resolve(mod_piglet, "glAttachShader", glAttachShader);
				ok &= resolve(mod_piglet, "glLinkProgram", glLinkProgram);
				ok &= resolve(mod_piglet, "glGetProgramiv", glGetProgramiv);
				ok &= resolve(mod_piglet, "glUseProgram", glUseProgram);
				ok &= resolve(mod_piglet, "glDeleteProgram", glDeleteProgram);
				ok &= resolve(mod_piglet, "glGetAttribLocation", glGetAttribLocation);
				ok &= resolve(mod_piglet, "glGetUniformLocation", glGetUniformLocation);
				ok &= resolve(mod_piglet, "glUniformMatrix4fv", glUniformMatrix4fv);
				ok &= resolve(mod_piglet, "glUniform3f", glUniform3f);
				ok &= resolve(mod_piglet, "glUniform1i", glUniform1i);
				ok &= resolve(mod_piglet, "glUniform1f", glUniform1f);
				ok &= resolve(mod_piglet, "glEnableVertexAttribArray", glEnableVertexAttribArray);
				ok &= resolve(mod_piglet, "glVertexAttribPointer", glVertexAttribPointer);
				ok &= resolve(mod_piglet, "glDrawElements", glDrawElements);
				return ok;
			}
		};

		constexpr const char* kWorldVertShader = R"(
			attribute vec3 aPos;
			attribute vec3 aNormal;
			attribute vec2 aTexCoord;
			attribute vec2 aLmapCoord;
			attribute vec4 aColor;
			uniform mat4 uViewProj;
			varying vec2 vUV;
			varying vec2 vLUV;
			varying vec3 vNormal;
			varying vec4 vColor;
			varying float vDist;
			void main() {
				vec4 clip = uViewProj * vec4(aPos, 1.0);
				gl_Position = clip;
				vUV = aTexCoord;
				vLUV = aLmapCoord;
				vNormal = aNormal;
				vColor = aColor;
				vDist = clip.w;
			}
		)";

		constexpr const char* kWorldFragShader = R"(
			precision mediump float;
			uniform sampler2D uDiffuse;
			uniform sampler2D uLightmap;
			uniform vec3 uSunDir;
			uniform float uUseLightmap;
			uniform float uFullbright;
			varying vec2 vUV;
			varying vec2 vLUV;
			varying vec3 vNormal;
			varying vec4 vColor;
			varying float vDist;
			void main() {
				vec4 tex = texture2D(uDiffuse, vUV);
				if (tex.a < 0.5) discard;
				vec3 lit = vec3(1.0);
				if (uFullbright < 0.5) {
					float ndl = max(0.0, dot(normalize(vNormal), -uSunDir));
					float shade = mix(0.45 + 0.65 * ndl, 0.85 + 0.25 * ndl, uUseLightmap);
					vec3 vmod = mix(vec3(1.0), vColor.rgb, 0.35);
					lit = vec3(shade) * vmod;
					if (uUseLightmap > 0.5) {
						vec3 lm = sqrt(texture2D(uLightmap, vLUV).rgb) * 1.45;
						lit *= clamp(lm, vec3(0.35), vec3(1.85));
					}
				}
				float fog = clamp((vDist - 1800.0) / 6500.0, 0.0, 0.55);
				vec3 col = mix(tex.rgb * lit, vec3(0.62, 0.69, 0.77), fog);
				gl_FragColor = vec4(col, 1.0);
			}
		)";

		// Native PS4 Renderer supporting Hardware Piglet GLES2 + Automatic Native VideoOut Direct-Memory Scanout
		class Ps4NativeRenderer final : public IRenderBackend
		{
		public:
			bool init(int width, int height) override
			{
				out_w_ = width > 0 ? width : 1920;
				out_h_ = height > 0 ? height : 1080;

				// 1. Try hardware-accelerated PS4 GPU via Piglet OpenGL ES 2.0 + Shacc
				if (try_init_piglet_gpu())
				{
					use_piglet_gpu_ = true;
					platform::log_info("PS4 Renderer active: Native Hardware GPU (Piglet GLES2 on AMD Liverpool GCN)");
					return true;
				}

				// 2. Graceful fallback: Native libSceVideoOut Direct-Memory Scanout + Multithreaded 3D Rasterizer
				platform::log_info("Piglet/Shacc modules not present — initializing native libSceVideoOut Direct-Memory 3D backend");
				if (!init_video_out())
				{
					platform::log_error("sceVideoOutOpen / direct memory allocation failed");
					return false;
				}
				// 960x540 internal raster -> 2x fast integer blit to 1920x1080 VideoOut scanout
				if (!sw_rast_.init(960, 540, 4))
					return false;

				use_piglet_gpu_ = false;
				platform::log_info("PS4 Renderer active: Native libSceVideoOut Direct-Memory Scanout (1920x1080)");
				return true;
			}

			void shutdown() override
			{
				if (use_piglet_gpu_)
				{
					destroy_gpu_scene();
					if (program_) gl_.glDeleteProgram(program_);
					if (white_tex_) gl_.glDeleteTextures(1, &white_tex_);
					if (egl_display_ != EGL_NO_DISPLAY)
					{
						gl_.eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
						if (egl_surface_ != EGL_NO_SURFACE) gl_.eglDestroySurface(egl_display_, egl_surface_);
						if (egl_context_ != EGL_NO_CONTEXT) gl_.eglDestroyContext(egl_display_, egl_context_);
						gl_.eglTerminate(egl_display_);
					}
					use_piglet_gpu_ = false;
				}
				sw_rast_.shutdown();
				if (video_handle_ >= 0)
				{
					sceVideoOutClose(video_handle_);
					video_handle_ = -1;
				}
				platform::free_direct_memory(fb_dmem_);
			}

			bool upload_scene(const world::WorldScene& scene) override
			{
				if (!use_piglet_gpu_) return true;
				destroy_gpu_scene();
				if (scene.vertices.empty()) return true;

				gl_.glGenBuffers(1, &vbo_);
				gl_.glBindBuffer(GL_ARRAY_BUFFER, vbo_);
				gl_.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(scene.vertices.size() * sizeof(world::RenderVertex)),
					scene.vertices.data(), GL_STATIC_DRAW);

				// Build per-surface 16-bit local IBO chunks (universal GLES2 compatibility without OES_element_index_uint)
				// Store unified 32-bit indices for surfaces with vertices <= 65535 by re-basing vertex pointer per surface
				std::vector<std::uint16_t> idx16;
				idx16.reserve(scene.indices.size());
				surface_draws_.clear();
				for (const auto& s : scene.surfaces)
				{
					GpuSurfDraw d{};
					d.surface_index = s.index;
					d.base_vertex = s.first_vertex;
					d.first_idx16 = static_cast<std::uint32_t>(idx16.size());
					d.index_count = s.tri_count * 3u;
					d.diffuse_tex = upload_or_get_tex(s.material ? s.material->color_texture.get() : nullptr);
					d.lightmap_tex = (s.lightmap_index < scene.lightmaps.size())
						? upload_or_get_tex(scene.lightmaps[s.lightmap_index].get()) : 0;
					for (std::uint32_t k = 0; k < d.index_count; ++k)
					{
						const std::uint32_t abs_v = scene.indices[s.first_index + k];
						const std::uint32_t rel = abs_v >= s.first_vertex ? (abs_v - s.first_vertex) : 0;
						idx16.push_back(static_cast<std::uint16_t>(std::min<std::uint32_t>(rel, 65535u)));
					}
					surface_draws_.push_back(d);
				}

				gl_.glGenBuffers(1, &ibo_);
				gl_.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
				gl_.glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(idx16.size() * sizeof(std::uint16_t)),
					idx16.data(), GL_STATIC_DRAW);

				platform::log_info("Uploaded CODOL scene to PS4 GPU: %zu verts, %zu surface batches, %zu GPU textures",
					scene.vertices.size(), surface_draws_.size(), gpu_textures_.size());
				return true;
			}

			void render_frame(const world::WorldScene* scene, const RenderFrameParams& params, platform::PerfMetrics& metrics) override
			{
				metrics.active_gpu_backend = backend_name();
				if (use_piglet_gpu_)
				{
					render_frame_piglet(scene, params, metrics);
					return;
				}

				sw_rast_.render(scene, params, metrics);

				const std::uint64_t tp0 = platform::time_now_ns();
				std::uint32_t* dst = reinterpret_cast<std::uint32_t*>(fb_ptrs_[cur_fb_]);
				sw_rast_.blit_to_bgra(dst, out_w_, out_h_, out_w_);

				const int64_t fid = static_cast<int64_t>(flip_id_++);
				sceVideoOutSubmitFlip(video_handle_, cur_fb_, ORBIS_VIDEO_OUT_FLIP_VSYNC, fid);

				// Wait for flip completion
				OrbisKernelEvent evt{};
				int ev_count = 0;
				for (int guard = 0; guard < 16; ++guard)
				{
					OrbisVideoOutFlipStatus st{};
					sceVideoOutGetFlipStatus(video_handle_, &st);
					if (st.flipArg >= fid) break;
					if (sceKernelWaitEqueue(flip_queue_, &evt, 1, &ev_count, nullptr) != 0) break;
				}
				cur_fb_ = (cur_fb_ + 1) % 2;
				metrics.present_ms = static_cast<double>(platform::time_now_ns() - tp0) * 1e-6;
			}

			const char* backend_name() const override
			{
				return use_piglet_gpu_ ? "PS4 Native GPU (Piglet GLES2 / AMD GCN)" : "PS4 Native VideoOut Direct-Memory 3D";
			}

			bool save_screenshot_bmp(const std::string& path) const override
			{
				return !use_piglet_gpu_ && sw_rast_.save_bmp(path);
			}

		private:
			struct GpuSurfDraw
			{
				int surface_index = 0;
				std::uint32_t base_vertex = 0;
				std::uint32_t first_idx16 = 0;
				std::uint32_t index_count = 0;
				GLuint diffuse_tex = 0;
				GLuint lightmap_tex = 0;
			};

			bool try_init_piglet_gpu()
			{
				if (!gl_.try_load()) return false;

				OrbisPglConfig cfg{};
				std::memset(&cfg, 0, sizeof(cfg));
				cfg.size = sizeof(cfg);
				cfg.flags = ORBIS_PGL_FLAGS_USE_COMPOSITE_EXT | ORBIS_PGL_FLAGS_USE_FLEXIBLE_MEMORY | 0x60;
				cfg.processOrder = 1;
				cfg.systemSharedMemorySize = 250ULL * 1024ULL * 1024ULL;
				cfg.videoSharedMemorySize = 512ULL * 1024ULL * 1024ULL;
				cfg.maxMappedFlexibleMemory = 170ULL * 1024ULL * 1024ULL;
				cfg.drawCommandBufferSize = 2ULL * 1024ULL * 1024ULL;
				cfg.lcueResourceBufferSize = 2ULL * 1024ULL * 1024ULL;
				cfg.dbgPosCmd_0x40 = static_cast<uint32_t>(out_w_);
				cfg.dbgPosCmd_0x44 = static_cast<uint32_t>(out_h_);
				cfg.unk_0x5C = 2;

				if (!gl_.scePigletSetConfigurationVSH(&cfg)) return false;
				egl_display_ = gl_.eglGetDisplay(EGL_DEFAULT_DISPLAY);
				if (egl_display_ == EGL_NO_DISPLAY) return false;
				EGLint maj = 0, min = 0;
				if (!gl_.eglInitialize(egl_display_, &maj, &min)) return false;
				if (!gl_.eglBindAPI(EGL_OPENGL_ES_API)) return false;

				const EGLint attribs[] = {
					EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
					EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 0,
					EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
					EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
					EGL_NONE
				};
				EGLConfig config{};
				EGLint ncfg = 0;
				if (!gl_.eglChooseConfig(egl_display_, attribs, &config, 1, &ncfg) || ncfg < 1) return false;

				OrbisPglWindow win{ 0, static_cast<khronos_uint32_t>(out_w_), static_cast<khronos_uint32_t>(out_h_) };
				const EGLint win_attribs[] = { EGL_RENDER_BUFFER, EGL_BACK_BUFFER, EGL_NONE };
				egl_surface_ = gl_.eglCreateWindowSurface(egl_display_, config, &win, win_attribs);
				if (egl_surface_ == EGL_NO_SURFACE) return false;

				const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
				egl_context_ = gl_.eglCreateContext(egl_display_, config, EGL_NO_CONTEXT, ctx_attribs);
				if (egl_context_ == EGL_NO_CONTEXT) return false;

				if (!gl_.eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_)) return false;
				gl_.eglSwapInterval(egl_display_, 1);

				// Compile shaders
				auto compile_sh = [&](GLenum type, const char* src) -> GLuint {
					GLuint s = gl_.glCreateShader(type);
					gl_.glShaderSource(s, 1, &src, nullptr);
					gl_.glCompileShader(s);
					GLint ok = 0;
					gl_.glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
					if (!ok) { gl_.glDeleteShader(s); return 0; }
					return s;
				};
				const GLuint vs = compile_sh(GL_VERTEX_SHADER, kWorldVertShader);
				const GLuint fs = compile_sh(GL_FRAGMENT_SHADER, kWorldFragShader);
				if (!vs || !fs)
				{
					if (vs) gl_.glDeleteShader(vs);
					if (fs) gl_.glDeleteShader(fs);
					return false;
				}
				program_ = gl_.glCreateProgram();
				gl_.glAttachShader(program_, vs);
				gl_.glAttachShader(program_, fs);
				gl_.glLinkProgram(program_);
				gl_.glDeleteShader(vs);
				gl_.glDeleteShader(fs);
				GLint linked = 0;
				gl_.glGetProgramiv(program_, GL_LINK_STATUS, &linked);
				if (!linked) { gl_.glDeleteProgram(program_); program_ = 0; return false; }

				loc_pos_ = gl_.glGetAttribLocation(program_, "aPos");
				loc_nrm_ = gl_.glGetAttribLocation(program_, "aNormal");
				loc_uv_  = gl_.glGetAttribLocation(program_, "aTexCoord");
				loc_luv_ = gl_.glGetAttribLocation(program_, "aLmapCoord");
				loc_col_ = gl_.glGetAttribLocation(program_, "aColor");
				loc_vp_  = gl_.glGetUniformLocation(program_, "uViewProj");
				loc_diff_= gl_.glGetUniformLocation(program_, "uDiffuse");
				loc_lmap_= gl_.glGetUniformLocation(program_, "uLightmap");
				loc_sun_ = gl_.glGetUniformLocation(program_, "uSunDir");
				loc_ulm_ = gl_.glGetUniformLocation(program_, "uUseLightmap");
				loc_fb_  = gl_.glGetUniformLocation(program_, "uFullbright");

				// 1x1 white stand-in texture
				const std::uint8_t white[4] = { 210, 210, 210, 255 };
				gl_.glGenTextures(1, &white_tex_);
				gl_.glBindTexture(GL_TEXTURE_2D, white_tex_);
				gl_.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
				gl_.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
				gl_.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

				return true;
			}

			GLuint upload_or_get_tex(const assets::DecodedTexture* tex)
			{
				if (!tex || !tex->is_valid()) return white_tex_;
				auto it = gpu_textures_.find(tex);
				if (it != gpu_textures_.end()) return it->second;
				GLuint id = 0;
				gl_.glGenTextures(1, &id);
				gl_.glBindTexture(GL_TEXTURE_2D, id);
				gl_.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				gl_.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				gl_.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
				gl_.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
				gl_.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex->width, tex->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex->rgba.data());
				gpu_textures_.emplace(tex, id);
				return id;
			}

			void destroy_gpu_scene()
			{
				if (!use_piglet_gpu_) return;
				if (vbo_) { gl_.glDeleteBuffers(1, &vbo_); vbo_ = 0; }
				if (ibo_) { gl_.glDeleteBuffers(1, &ibo_); ibo_ = 0; }
				for (auto& [ptr, id] : gpu_textures_) gl_.glDeleteTextures(1, &id);
				gpu_textures_.clear();
				surface_draws_.clear();
			}

			void render_frame_piglet(const world::WorldScene* scene, const RenderFrameParams& params, platform::PerfMetrics& metrics)
			{
				const std::uint64_t t0 = platform::time_now_ns();
				gl_.glViewport(0, 0, out_w_, out_h_);
				gl_.glClearColor(0.38f, 0.54f, 0.72f, 1.0f);
				gl_.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
				gl_.glEnable(GL_DEPTH_TEST);
				gl_.glDepthFunc(GL_LEQUAL);
				gl_.glEnable(GL_CULL_FACE);
				gl_.glFrontFace(GL_CW);
				gl_.glCullFace(GL_BACK);

				metrics.visible_surfaces = 0;
				metrics.drawn_triangles = 0;

				if (scene && vbo_ && ibo_ && program_)
				{
					math::Vec3 fwd, right, up;
					math::angles_to_basis(params.yaw_deg, params.pitch_deg, fwd, right, up);
					const float aspect = static_cast<float>(out_w_) / static_cast<float>(out_h_);
					const math::Mat4 view = math::Mat4::look_at(params.eye, params.eye + fwd, { 0, 0, 1 });
					const math::Mat4 proj = math::Mat4::perspective(params.fov_deg, aspect, 4.0f, 18000.0f);
					const math::Mat4 vp = math::Mat4::multiply(proj, view);
					const math::Frustum frustum = math::Frustum::from_view_proj(vp);

					gl_.glUseProgram(program_);
					gl_.glUniformMatrix4fv(loc_vp_, 1, GL_FALSE, vp.m);
					const math::Vec3 sun = scene->has_sun ? scene->sun_light.direction : math::normalize({ -0.45f, -0.35f, -0.82f });
					gl_.glUniform3f(loc_sun_, sun.x, sun.y, sun.z);
					gl_.glUniform1i(loc_diff_, 0);
					gl_.glUniform1i(loc_lmap_, 1);

					gl_.glBindBuffer(GL_ARRAY_BUFFER, vbo_);
					gl_.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
					if (loc_pos_ >= 0) gl_.glEnableVertexAttribArray(static_cast<GLuint>(loc_pos_));
					if (loc_nrm_ >= 0) gl_.glEnableVertexAttribArray(static_cast<GLuint>(loc_nrm_));
					if (loc_uv_  >= 0) gl_.glEnableVertexAttribArray(static_cast<GLuint>(loc_uv_));
					if (loc_luv_ >= 0) gl_.glEnableVertexAttribArray(static_cast<GLuint>(loc_luv_));
					if (loc_col_ >= 0) gl_.glEnableVertexAttribArray(static_cast<GLuint>(loc_col_));

					for (const auto& d : surface_draws_)
					{
						const auto& s = scene->surfaces[static_cast<std::size_t>(d.surface_index)];
						if (s.is_nodraw || s.is_sky || d.index_count == 0) continue;
						if (s.translucent || s.emissive) continue;
						if (params.frustum_cull && !frustum.intersects_aabb(s.bounds_center, s.bounds_half)) continue;

						const std::uintptr_t base_off = static_cast<std::uintptr_t>(d.base_vertex) * sizeof(world::RenderVertex);
						if (loc_pos_ >= 0) gl_.glVertexAttribPointer(static_cast<GLuint>(loc_pos_), 3, GL_FLOAT, GL_FALSE, sizeof(world::RenderVertex), reinterpret_cast<const void*>(base_off + offsetof(world::RenderVertex, x)));
						if (loc_nrm_ >= 0) gl_.glVertexAttribPointer(static_cast<GLuint>(loc_nrm_), 3, GL_FLOAT, GL_FALSE, sizeof(world::RenderVertex), reinterpret_cast<const void*>(base_off + offsetof(world::RenderVertex, nx)));
						if (loc_uv_  >= 0) gl_.glVertexAttribPointer(static_cast<GLuint>(loc_uv_),  2, GL_FLOAT, GL_FALSE, sizeof(world::RenderVertex), reinterpret_cast<const void*>(base_off + offsetof(world::RenderVertex, u)));
						if (loc_luv_ >= 0) gl_.glVertexAttribPointer(static_cast<GLuint>(loc_luv_), 2, GL_FLOAT, GL_FALSE, sizeof(world::RenderVertex), reinterpret_cast<const void*>(base_off + offsetof(world::RenderVertex, lu)));
						if (loc_col_ >= 0) gl_.glVertexAttribPointer(static_cast<GLuint>(loc_col_), 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(world::RenderVertex), reinterpret_cast<const void*>(base_off + offsetof(world::RenderVertex, r)));

						gl_.glActiveTexture(GL_TEXTURE0);
						gl_.glBindTexture(GL_TEXTURE_2D, d.diffuse_tex ? d.diffuse_tex : white_tex_);
						const bool use_lm = params.use_lightmap && !s.is_sky && d.lightmap_tex != 0;
						gl_.glActiveTexture(GL_TEXTURE1);
						gl_.glBindTexture(GL_TEXTURE_2D, use_lm ? d.lightmap_tex : white_tex_);
						gl_.glUniform1f(loc_ulm_, use_lm ? 1.0f : 0.0f);
						gl_.glUniform1f(loc_fb_, (params.fullbright || s.is_sky || s.emissive) ? 1.0f : 0.0f);

						gl_.glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(d.index_count), GL_UNSIGNED_SHORT,
							reinterpret_cast<const void*>(static_cast<std::uintptr_t>(d.first_idx16) * sizeof(std::uint16_t)));

						++metrics.visible_surfaces;
						metrics.drawn_triangles += d.index_count / 3u;
					}
				}

				const std::uint64_t t1 = platform::time_now_ns();
				gl_.eglSwapBuffers(egl_display_, egl_surface_);
				const std::uint64_t t2 = platform::time_now_ns();
				metrics.render_ms = static_cast<double>(t1 - t0) * 1e-6;
				metrics.present_ms = static_cast<double>(t2 - t1) * 1e-6;
				metrics.frame_ms = static_cast<double>(t2 - t0) * 1e-6;
			}

			bool init_video_out()
			{
				video_handle_ = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, nullptr);
				if (video_handle_ < 0) return false;
				if (sceKernelCreateEqueue(&flip_queue_, "dragon flip queue") < 0) return false;
				sceVideoOutAddFlipEvent(flip_queue_, video_handle_, nullptr);

				const std::size_t fb_bytes = static_cast<std::size_t>(out_w_) * out_h_ * 4u;
				if (!platform::alloc_direct_memory(fb_bytes * 2u, 0x200000, fb_dmem_))
					return false;

				fb_ptrs_[0] = fb_dmem_.ptr;
				fb_ptrs_[1] = static_cast<std::uint8_t*>(fb_dmem_.ptr) + fb_bytes;
				std::memset(fb_dmem_.ptr, 0, fb_bytes * 2u);

				OrbisVideoOutBufferAttribute attr{};
				sceVideoOutSetBufferAttribute(&attr, 0x80000000 /* SRGB BGRA8 */, 1, 0,
					static_cast<uint32_t>(out_w_), static_cast<uint32_t>(out_h_), static_cast<uint32_t>(out_w_));
				if (sceVideoOutRegisterBuffers(video_handle_, 0, fb_ptrs_, 2, &attr) != 0)
					return false;

				sceVideoOutSetFlipRate(video_handle_, 0);
				return true;
			}

			int out_w_ = 1920;
			int out_h_ = 1080;
			bool use_piglet_gpu_ = false;

			// Piglet GLES2 state
			PigletApi gl_;
			EGLDisplay egl_display_ = EGL_NO_DISPLAY;
			EGLSurface egl_surface_ = EGL_NO_SURFACE;
			EGLContext egl_context_ = EGL_NO_CONTEXT;
			GLuint program_ = 0;
			GLuint vbo_ = 0;
			GLuint ibo_ = 0;
			GLuint white_tex_ = 0;
			GLint loc_pos_ = -1, loc_nrm_ = -1, loc_uv_ = -1, loc_luv_ = -1, loc_col_ = -1;
			GLint loc_vp_ = -1, loc_diff_ = -1, loc_lmap_ = -1, loc_sun_ = -1, loc_ulm_ = -1, loc_fb_ = -1;
			std::unordered_map<const assets::DecodedTexture*, GLuint> gpu_textures_;
			std::vector<GpuSurfDraw> surface_draws_;

			// Native VideoOut direct-memory scanout state
			int32_t video_handle_ = -1;
			OrbisKernelEqueue flip_queue_{};
			platform::DirectMemBlock fb_dmem_{};
			void* fb_ptrs_[2]{ nullptr, nullptr };
			int cur_fb_ = 0;
			std::uint64_t flip_id_ = 1;
			SoftwareRasterizer sw_rast_;
		};
	}

	std::unique_ptr<IRenderBackend> create_platform_renderer()
	{
		return std::make_unique<Ps4NativeRenderer>();
	}
}

#endif
