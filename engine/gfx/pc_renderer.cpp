#if !defined(DRAGON_PS4) && !defined(__PS4__) && !defined(__ORBIS__)

#include "renderer.hpp"
#include "software_rasterizer.hpp"

namespace dragon::gfx
{
	namespace
	{
		class PcDebugRenderer final : public IRenderBackend
		{
		public:
			bool init(int width, int height) override
			{
				return rast_.init(width > 0 ? width : 1280, height > 0 ? height : 720, 0);
			}

			int draw_width() const override { return rast_.width(); }

			int draw_height() const override { return rast_.height(); }

			void shutdown() override
			{
				rast_.shutdown();
			}

			bool upload_scene(const world::WorldScene&) override
			{
				return true;
			}

			void render_frame(const world::WorldScene* scene, const RenderFrameParams& params, platform::PerfMetrics& metrics) override
			{
				metrics.active_gpu_backend = backend_name();
				rast_.render(scene, params, metrics);
			}

			const char* backend_name() const override
			{
				return "PC Debug 3D Textured Rasterizer (MT)";
			}

			bool save_screenshot_bmp(const std::string& path) const override
			{
				return rast_.save_bmp(path);
			}

		private:
			SoftwareRasterizer rast_;
		};
	}

	std::unique_ptr<IRenderBackend> create_platform_renderer()
	{
		return std::make_unique<PcDebugRenderer>();
	}
}

#endif
