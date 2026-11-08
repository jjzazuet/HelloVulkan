#include "AppPBRClusterForward.h"
#include "PushConstants.h"
#include "Utility.h"
#include "Configs.h"

#include "PipelineSkybox.h"
#include "PipelineClear.h"
#include "PipelineFinish.h"
#include "PipelineTonemap.h"
#include "PipelineResolveMS.h"

#include "glm/ext.hpp"
#include "imgui_impl_vulkan.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>

AppPBRClusterForward::AppPBRClusterForward() 
{
}

void AppPBRClusterForward::Init()
{
	uiData_.pbrPC_.albedoMultipler = 0.01f;
	camera_->SetPositionAndTarget(glm::vec3(0.0f, 1.0f, 6.0f), glm::vec3(0.0, 2.5, 0.0));

	// Initialize attachments
	InitSharedResources();

	// Initialize lights
	InitLights();

	// Image-Based Lighting
	resourcesIBL_ = AddResources<ResourcesIBL>(vulkanContext_, AppConfig::TextureFolder + "dikhololo_night_4k.hdr");
	if (const char* iblDump = std::getenv("ZTGFX_IBL_DUMP"))
	{
		resourcesIBL_->Dump(vulkanContext_, iblDump);
	}

	resCF_ = AddResources<ResourcesClusterForward>();
	resCF_->CreateBuffers(vulkanContext_, resourcesLight_->GetLightCount());

	std::vector<ModelCreateInfo> dataArray = {
		{
			.filename = AppConfig::ModelFolder + "Sponza/Sponza.gltf",
			.instanceCount = 1,
			.playAnimation = false
		}
	};
	scene_ = std::make_unique<Scene>(vulkanContext_, dataArray);

	// Pipelines
	AddPipeline<PipelineClear>(vulkanContext_); // This is responsible to clear swapchain image
	AddPipeline<PipelineSkybox>(
		vulkanContext_,
		&(resourcesIBL_->environmentCubemap_),
		resourcesShared_,
		// This is the first offscreen render pass so we need to clear the color attachment and depth attachment
		RenderPassBit::ColorClear | RenderPassBit::DepthClear);
	aabbPtr_ = AddPipeline<PipelineAABBGenerator>(vulkanContext_, resCF_);
	lightCullPtr_ = AddPipeline<PipelineLightCulling>(vulkanContext_, resourcesLight_, resCF_);
	pbrOpaquePtr_ = AddPipeline<PipelinePBRClusterForward>(
		vulkanContext_,
		scene_.get(),
		resourcesLight_,
		resCF_,
		resourcesIBL_,
		resourcesShared_,
		MaterialType::Opaque);
	pbrTransparentPtr_ = AddPipeline<PipelinePBRClusterForward>(
		vulkanContext_,
		scene_.get(),
		resourcesLight_,
		resCF_,
		resourcesIBL_,
		resourcesShared_,
		MaterialType::Transparent);
	// The Java reference renderer is single-sample, so the ground-truth capture
	// runs without MSAA: no resolve pass, and the tonemap samples the offscreen
	// colour image directly.
	const bool multisampled =
		resourcesShared_->multiSampledColorImage_.multisampleCount_ != VK_SAMPLE_COUNT_1_BIT;
	lightPtr_ = AddPipeline<PipelineLightRender>(
		vulkanContext_,
		resourcesLight_,
		resourcesShared_,
		multisampled ? 0u : RenderPassBit::ColorShaderReadOnly);
	if (multisampled)
	{
		// Resolve multiSampledColorImage_ to singleSampledColorImage_
		AddPipeline<PipelineResolveMS>(vulkanContext_, resourcesShared_);
	}
	// This is on-screen render pass that transfers the colour image to the swapchain
	AddPipeline<PipelineTonemap>(
		vulkanContext_,
		multisampled
			? &(resourcesShared_->singleSampledColorImage_)
			: &(resourcesShared_->multiSampledColorImage_));
	imguiPtr_ = AddPipeline<PipelineImGui>(vulkanContext_, vulkanInstance_.GetInstance(), glfwWindow_);
	// Present swapchain image
	AddPipeline<PipelineFinish>(vulkanContext_);
}

void AppPBRClusterForward::InitLights()
{
	std::vector<LightData> lights;

	float pi2 = glm::two_pi<float>();
	constexpr uint32_t NR_LIGHTS = 1000;
	for (uint32_t i = 0; i < NR_LIGHTS; ++i)
	{
		float yPos = Utility::RandomNumber(-2.f, 10.0f);
		float radius = Utility::RandomNumber(0.0f, 10.0f);
		float rad = Utility::RandomNumber(0.0f, pi2);
		float xPos = glm::cos(rad);

		glm::vec4 position(
			glm::cos(rad) * radius,
			yPos,
			glm::sin(rad) * radius,
			1.f
		);

		glm::vec4 color(
			Utility::RandomNumber(0.0f, 1.0f),
			Utility::RandomNumber(0.0f, 1.0f),
			Utility::RandomNumber(0.0f, 1.0f),
			1.f
		);

		LightData l;
		l.color_ = color;
		l.position_ = position;
		l.radius_ = Utility::RandomNumber(0.5f, 2.0f);

		lights.push_back(l);
	}

	lights_ = lights;
	resourcesLight_ = AddResources<ResourcesLight>();
	resourcesLight_->AddLights(vulkanContext_, lights);
}

void AppPBRClusterForward::UpdateUBOs()
{
	// Camera UBO
	CameraUBO ubo = camera_->GetCameraUBO();
	for (auto& pipeline : pipelines_)
	{
		pipeline->SetCameraUBO(vulkanContext_, ubo);
	}

	// Clustered forward
	ClusterForwardUBO cfUBO = camera_->GetClusterForwardUBO();
	aabbPtr_->SetClusterForwardUBO(vulkanContext_, cfUBO);
	lightCullPtr_->ResetGlobalIndex(vulkanContext_);
	lightCullPtr_->SetClusterForwardUBO(vulkanContext_, cfUBO);
	pbrOpaquePtr_->SetClusterForwardUBO(vulkanContext_, cfUBO);
	pbrTransparentPtr_->SetClusterForwardUBO(vulkanContext_, cfUBO);
}

void AppPBRClusterForward::UpdateUI()
{
	if (!ShowImGui())
	{
		imguiPtr_->ImGuiDrawEmpty();
		return;
	}

	imguiPtr_->ImGuiStart();
	imguiPtr_->ImGuiSetWindow("Clustered Forward Shading", 450, 350);
	imguiPtr_->ImGuiShowFrameData(&frameCounter_);
	ImGui::Checkbox("Render Lights", &uiData_.renderLights_);
	imguiPtr_->ImGuiShowPBRConfig(&uiData_.pbrPC_, resourcesIBL_->cubemapMipmapCount_);
	imguiPtr_->ImGuiEnd();

	for (auto& pipeline : pipelines_)
	{
		pipeline->UpdateFromUIData(vulkanContext_, uiData_);
	}
}

// This is called from main.cpp

std::string AppPBRClusterForward::GroundTruthMetadata() const
{
	std::ostringstream out;
	const CameraUBO cam = camera_->GetCameraUBO();
	const float width = static_cast<float>(vulkanContext_.GetSwapchainWidth());
	const float height = static_cast<float>(vulkanContext_.GetSwapchainHeight());

	out << "\"scene\":\"Assets/Models/Sponza/Sponza.gltf\",";
	out << "\"camera\":{\"position\":[" << cam.position.x << "," << cam.position.y
		<< "," << cam.position.z << "],\"target\":[0,2.5,0],\"yfov\":"
		<< CameraConfig::Zoom << ",\"aspect\":" << (width / height)
		<< ",\"near\":" << cam.cameraNear << ",\"far\":" << cam.cameraFar
		<< ",\"view\":[";
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			out << (c || r ? "," : "") << cam.view[c][r];
	out << "],\"projection\":[";
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			out << (c || r ? "," : "") << cam.projection[c][r];
	out << "]},";

	out << "\"light\":{\"position\":[" << lights_[0].position_.x << ","
		<< lights_[0].position_.y << "," << lights_[0].position_.z
		<< "],\"color\":[" << lights_[0].color_.x << "," << lights_[0].color_.y
		<< "," << lights_[0].color_.z << "],\"radius\":" << lights_[0].radius_
		<< ",\"count\":" << lights_.size() << ",\"seed\":\"0x5EED\"},";

	const glm::mat4 inverseView = glm::inverse(cam.view);
	out << "\"uniforms\":{\"projection\":[";
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			out << (c || r ? "," : "") << cam.projection[c][r];
	out << "],\"modelview\":[";
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			out << (c || r ? "," : "") << cam.view[c][r];
	out << "],\"inverseModelview\":[";
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			out << (c || r ? "," : "") << inverseView[c][r];
	out << "],\"exposure\":1},";

	VkPhysicalDeviceProperties deviceProperties{};
	vkGetPhysicalDeviceProperties(vulkanContext_.GetPhysicalDevice(), &deviceProperties);
	out << "\"device\":{\"name\":\"" << deviceProperties.deviceName
		<< "\",\"driverVersion\":" << deviceProperties.driverVersion
		<< ",\"apiVersion\":" << deviceProperties.apiVersion
		<< ",\"vendorID\":" << deviceProperties.vendorID
		<< ",\"deviceID\":" << deviceProperties.deviceID << "},";

	out << "\"settings\":{\"ibl\":\"Assets/Textures/dikhololo_night_4k.hdr\","
		<< "\"tonemap\":\"reinhard+gamma\",\"gamma\":2.2,\"msaa\":"
		<< static_cast<int>(resourcesShared_->multiSampledColorImage_.multisampleCount_)
		<< ",\"orb\":{\"radius\":0.1,\"orbSize\":0.7,\"glowFalloff\":2.0,"
		<< "\"glowStrength\":0.5}},";

	out << "\"lights\":[";
	for (size_t i = 0; i < lights_.size(); ++i)
	{
		const LightData& light = lights_[i];
		if (i) out << ",";
		out << "{\"position\":[" << light.position_.x << "," << light.position_.y
			<< "," << light.position_.z << "],\"color\":[" << light.color_.x << ","
			<< light.color_.y << "," << light.color_.z << "],\"radius\":"
			<< light.radius_ << "}";
	}
	out << "]";
	return out.str();
}


namespace
{
uint32_t FloatBits(float value)
{
	uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

std::string HexBits(float value)
{
	char text[11];
	std::snprintf(text, sizeof(text), "0x%08x", FloatBits(value));
	return text;
}

void WriteBits(std::ostringstream& out, const float* values, size_t count)
{
	for (size_t i = 0; i < count; ++i)
	{
		if (i) out << ",";
		out << "\"" << HexBits(values[i]) << "\"";
	}
}
} // namespace

void AppPBRClusterForward::WriteGroundTruthCaptureState(const std::string& imagePath)
{
	// Exact light buffer: 48-byte std430 stride used by the shaders. Padding is
	// zeroed so the file is deterministic (the GPU never reads it).
	constexpr size_t LIGHT_STRIDE = 48;
	std::vector<uint8_t> lightBytes(lights_.size() * LIGHT_STRIDE, 0);
	for (size_t i = 0; i < lights_.size(); ++i)
	{
		float* record = reinterpret_cast<float*>(lightBytes.data() + i * LIGHT_STRIDE);
		const LightData& light = lights_[i];
		record[0] = light.position_.x;
		record[1] = light.position_.y;
		record[2] = light.position_.z;
		record[3] = light.position_.w;
		record[4] = light.color_.x;
		record[5] = light.color_.y;
		record[6] = light.color_.z;
		record[7] = light.color_.w;
		record[8] = light.radius_;
	}

	const std::string lightsPath = imagePath + ".lights.bin";
	if (FILE* file = std::fopen(lightsPath.c_str(), "wb"))
	{
		std::fwrite(lightBytes.data(), 1, lightBytes.size(), file);
		std::fclose(file);
	}

	// Depth buffer readback: needed to explain orb occlusion-boundary pixels.
	const VulkanImage& depthImage = resourcesShared_->depthImage_;
	uint32_t depthBytesPerPixel = 4;
	if (depthImage.imageFormat_ == VK_FORMAT_D16_UNORM)
	{
		depthBytesPerPixel = 2;
	}
	const uint32_t depthWidth = vulkanContext_.GetSwapchainWidth();
	const uint32_t depthHeight = vulkanContext_.GetSwapchainHeight();
	const size_t depthSize = static_cast<size_t>(depthWidth) * depthHeight * depthBytesPerPixel;
	VulkanBuffer depthStaging;
	depthStaging.CreateBuffer(
		vulkanContext_, static_cast<VkDeviceSize>(depthSize),
		VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	{
		VkCommandBuffer cb = vulkanContext_.BeginOneTimeGraphicsCommand();
		VulkanImage::TransitionLayoutCommand(
			cb, depthImage.image_, depthImage.imageFormat_,
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		const VkBufferImageCopy region = {
			.bufferOffset = 0,
			.bufferRowLength = 0,
			.bufferImageHeight = 0,
			.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
			.imageOffset = {0, 0, 0},
			.imageExtent = {depthWidth, depthHeight, 1},
		};
		vkCmdCopyImageToBuffer(
			cb, depthImage.image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			depthStaging.buffer_, 1, &region);
		VulkanImage::TransitionLayoutCommand(
			cb, depthImage.image_, depthImage.imageFormat_,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
		vulkanContext_.EndOneTimeGraphicsCommand(cb);
	}
	std::vector<uint8_t> depthBytes(depthSize);
	depthStaging.DownloadBufferData(vulkanContext_, depthBytes.data(), depthSize);
	const std::string depthPath = imagePath + ".depth.bin";
	if (FILE* file = std::fopen(depthPath.c_str(), "wb"))
	{
		std::fwrite(depthBytes.data(), 1, depthBytes.size(), file);
		std::fclose(file);
	}
	depthStaging.Destroy();

	// Cluster cell and light-index buffers, to diff Java CPU culling against the
	// C++ compute pass. Both are GPU-only, so copy through a staging buffer.
	auto dumpDeviceBuffer = [&](VulkanBuffer& src, VkDeviceSize size, const std::string& path)
	{
		VulkanBuffer staging;
		staging.CreateBuffer(vulkanContext_, size,
			VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		VkCommandBuffer cb = vulkanContext_.BeginOneTimeGraphicsCommand();
		const VkBufferCopy region = {.srcOffset = 0, .dstOffset = 0, .size = size};
		vkCmdCopyBuffer(cb, src.buffer_, staging.buffer_, 1, &region);
		vulkanContext_.EndOneTimeGraphicsCommand(cb);
		std::vector<uint8_t> data(size);
		staging.DownloadBufferData(vulkanContext_, data.data(), size);
		if (FILE* file = std::fopen(path.c_str(), "wb"))
		{
			std::fwrite(data.data(), 1, data.size(), file);
			std::fclose(file);
		}
		staging.Destroy();
	};
	constexpr VkDeviceSize CELLS_SIZE =
		static_cast<VkDeviceSize>(ClusterForwardConfig::ClusterCount) * 8;
	constexpr VkDeviceSize INDICES_SIZE =
		static_cast<VkDeviceSize>(ClusterForwardConfig::maxLightPerCluster) *
		ClusterForwardConfig::ClusterCount * sizeof(uint32_t);
	dumpDeviceBuffer(resCF_->lightCellsBuffer_, CELLS_SIZE, imagePath + ".cells.bin");
	dumpDeviceBuffer(resCF_->lightIndicesBuffer_, INDICES_SIZE, imagePath + ".indices.bin");

	const ClusterForwardUBO cluster = camera_->GetClusterForwardUBO();

	const CameraUBO cam = camera_->GetCameraUBO();
	const glm::mat4 inverseView = glm::inverse(cam.view);

	std::ostringstream out;
	out << "{";
	out << "\"format\":\"float32-bits\",";
	out << "\"width\":" << vulkanContext_.GetSwapchainWidth()
		<< ",\"height\":" << vulkanContext_.GetSwapchainHeight();
	out << ",\"camera\":{";
	out << "\"position\":[\"" << HexBits(cam.position.x) << "\",\""
		<< HexBits(cam.position.y) << "\",\"" << HexBits(cam.position.z) << "\"]";
	out << ",\"near\":\"" << HexBits(cam.cameraNear) << "\"";
	out << ",\"far\":\"" << HexBits(cam.cameraFar) << "\"";
	out << ",\"view\":["; WriteBits(out, &cam.view[0][0], 16); out << "]";
	out << ",\"projection\":["; WriteBits(out, &cam.projection[0][0], 16); out << "]";
	out << ",\"inverseModelview\":["; WriteBits(out, &inverseView[0][0], 16); out << "]";
	out << "}";
	out << ",\"lights\":{\"file\":\"lights.bin\",\"count\":" << lights_.size()
		<< ",\"stride\":" << LIGHT_STRIDE << "}";
	out << ",\"sampler\":{\"magFilter\":\"LINEAR\",\"minFilter\":\"LINEAR\""
		<< ",\"mipmapMode\":\"LINEAR\",\"addressMode\":\"REPEAT\""
		<< ",\"mipLodBias\":\"" << HexBits(0.0f) << "\""
		<< ",\"anisotropyEnable\":false,\"maxAnisotropy\":\"" << HexBits(1.0f) << "\""
		<< ",\"minLod\":\"" << HexBits(0.0f) << "\""
		<< ",\"maxLod\":\"mipCount\""
		<< ",\"unnormalizedCoordinates\":false}";
	out << ",\"orbPipeline\":{\"topology\":\"triangle-list\""
		<< ",\"depthTest\":true,\"depthWrite\":false"
		<< ",\"depthCompareOp\":\"LESS_OR_EQUAL\""
		<< ",\"blendEnable\":true"
		<< ",\"srcColorBlendFactor\":\"SRC_ALPHA\""
		<< ",\"dstColorBlendFactor\":\"ONE_MINUS_SRC_ALPHA\""
		<< ",\"srcAlphaBlendFactor\":\"ONE_MINUS_SRC_ALPHA\""
		<< ",\"dstAlphaBlendFactor\":\"ZERO\""
		<< ",\"vertexCount\":6,\"msaa\":1}";
	out << ",\"tonemap\":{\"exposure\":\"" << HexBits(1.0f) << "\""
		<< ",\"gamma\":\"" << HexBits(2.2f) << "\"}";
	out << ",\"cluster\":{\"screenSize\":[\"" << HexBits(cluster.screenSize.x)
		<< "\",\"" << HexBits(cluster.screenSize.y) << "\"]"
		<< ",\"sliceScaling\":\"" << HexBits(cluster.sliceScaling) << "\""
		<< ",\"sliceBias\":\"" << HexBits(cluster.sliceBias) << "\""
		<< ",\"cameraNear\":\"" << HexBits(cluster.cameraNear) << "\""
		<< ",\"cameraFar\":\"" << HexBits(cluster.cameraFar) << "\""
		<< ",\"sliceCountX\":" << cluster.sliceCountX
		<< ",\"sliceCountY\":" << cluster.sliceCountY
		<< ",\"sliceCountZ\":" << cluster.sliceCountZ << "}";
	out << ",\"depth\":{\"file\":\"depth.bin\",\"format\":"
		<< static_cast<int>(depthImage.imageFormat_)
		<< ",\"width\":" << depthWidth << ",\"height\":" << depthHeight
		<< ",\"bytesPerPixel\":" << depthBytesPerPixel << "}";
	out << ",\"clusters\":{\"cells\":\"cells.bin\",\"indices\":\"indices.bin\""
		<< ",\"count\":" << ClusterForwardConfig::ClusterCount
		<< ",\"cellStride\":8"
		<< ",\"maxLightPerCluster\":" << ClusterForwardConfig::maxLightPerCluster << "}";
	out << "}";

	const std::string statePath = imagePath + ".state.json";
	if (FILE* file = std::fopen(statePath.c_str(), "wb"))
	{
		const std::string text = out.str();
		std::fwrite(text.data(), 1, text.size(), file);
		std::fwrite("\n", 1, 1, file);
		std::fclose(file);
	}
}

void AppPBRClusterForward::MainLoop()
{
	InitVulkan({
		.suportBufferDeviceAddress_ = true,
		// Single-sample capture: the Java reference renderer used for parity
		// does not implement MSAA yet.
		.supportMSAA_ = false
		});

	Init();

	// Main loop
	while (StillRunning())
	{
		PollEvents();
		ProcessTiming();
		ProcessInput();
		DrawFrame();
	}

	// Destroy all objects
	scene_.reset();

	DestroyResources();
}