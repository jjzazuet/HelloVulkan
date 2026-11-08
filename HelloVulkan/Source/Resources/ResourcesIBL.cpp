#include "ResourcesIBL.h"
#include "PipelineEquirect2Cube.h"
#include "PipelineCubeFilter.h"
#include "PipelineBRDFLUT.h"
#include "Utility.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

ResourcesIBL::ResourcesIBL(VulkanContext& ctx, const std::string& hdrFile)
{
	Create(ctx, hdrFile);
	SetDebugNames(ctx);
}

ResourcesIBL::~ResourcesIBL()
{
	Destroy();
}

void ResourcesIBL::Create(VulkanContext& ctx, const std::string& hdrFile)
{
	// Create a cubemap from the input HDR
	{
		PipelineEquirect2Cube e2c(ctx, hdrFile);
		e2c.OffscreenRender(ctx, &environmentCubemap_);
	}

	// Cube filtering
	{
		PipelineCubeFilter cubeFilter(ctx, &(environmentCubemap_));
		cubeFilter.OffscreenRender(ctx, &diffuseCubemap_, CubeFilterType::Diffuse);
		cubeFilter.OffscreenRender(ctx, &specularCubemap_, CubeFilterType::Specular);
	}

	// BRDF look up table
	{
		PipelineBRDFLUT brdfLUTCompute(ctx);
		brdfLUTCompute.CreateLUT(ctx, &brdfLut_);
	}

	cubemapMipmapCount_ = static_cast<float>(Utility::MipMapCount(IBLConfig::InputCubeSideLength));
}

void ResourcesIBL::Destroy()
{
	environmentCubemap_.Destroy();
	diffuseCubemap_.Destroy();
	specularCubemap_.Destroy();
	brdfLut_.Destroy();
}

void ResourcesIBL::SetDebugNames(VulkanContext& ctx)
{
	environmentCubemap_.SetDebugName(ctx, "Environment_Cubemap");
	diffuseCubemap_.SetDebugName(ctx, "Diffuse_Cubemap");
	specularCubemap_.SetDebugName(ctx, "Specular_Cubemap");
	brdfLut_.SetDebugName(ctx, "BRDF_LUT");
}
namespace
{
uint32_t BytesPerPixel(VkFormat format)
{
	switch (format)
	{
		case VK_FORMAT_R32G32B32A32_SFLOAT: return 16u;
		case VK_FORMAT_R32G32_SFLOAT: return 8u;
		default: throw std::runtime_error("Unsupported IBL dump format");
	}
}

void DumpImage(
	VulkanContext& ctx,
	VulkanImage& image,
	const std::string& directory,
	const std::string& prefix)
{
	const uint32_t bpp = BytesPerPixel(image.imageFormat_);
	std::vector<VulkanBuffer> staging;

	VkCommandBuffer commandBuffer = ctx.BeginOneTimeGraphicsCommand();
	VulkanImage::TransitionLayoutCommand(
		commandBuffer, image.image_, image.imageFormat_,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		0u, image.mipCount_, 0u, image.layerCount_);

	for (uint32_t layer = 0; layer < image.layerCount_; ++layer)
	{
		for (uint32_t mip = 0; mip < image.mipCount_; ++mip)
		{
			const uint32_t width = std::max(1u, image.width_ >> mip);
			const uint32_t height = std::max(1u, image.height_ >> mip);
			VulkanBuffer stagingBuffer;
			stagingBuffer.CreateBuffer(
				ctx,
				static_cast<VkDeviceSize>(width) * height * bpp,
				VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				VMA_MEMORY_USAGE_CPU_ONLY);
			const VkBufferImageCopy region = {
				.bufferOffset = 0,
				.bufferRowLength = 0,
				.bufferImageHeight = 0,
				.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1},
				.imageOffset = {0, 0, 0},
				.imageExtent = {width, height, 1},
			};
			vkCmdCopyImageToBuffer(
				commandBuffer, image.image_,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				stagingBuffer.buffer_, 1, &region);
			staging.push_back(stagingBuffer);
		}
	}

	VulkanImage::TransitionLayoutCommand(
		commandBuffer, image.image_, image.imageFormat_,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		0u, image.mipCount_, 0u, image.layerCount_);
	ctx.EndOneTimeGraphicsCommand(commandBuffer);

	size_t index = 0;
	for (uint32_t layer = 0; layer < image.layerCount_; ++layer)
	{
		for (uint32_t mip = 0; mip < image.mipCount_; ++mip)
		{
			const uint32_t width = std::max(1u, image.width_ >> mip);
			const uint32_t height = std::max(1u, image.height_ >> mip);
			const size_t size = static_cast<size_t>(width) * height * bpp;
			std::vector<uint8_t> bytes(size);
			staging[index].DownloadBufferData(ctx, bytes.data(), size);
			const std::string path = directory + "/" + prefix + "_l" + std::to_string(layer)
				+ "_m" + std::to_string(mip) + ".raw";
			std::ofstream out(path, std::ios::binary);
			out.write(reinterpret_cast<const char*>(bytes.data()), size);
			staging[index].Destroy();
			++index;
		}
	}
}
} // namespace

void ResourcesIBL::Dump(VulkanContext& ctx, const std::string& directory)
{
	std::filesystem::create_directories(directory);
	DumpImage(ctx, environmentCubemap_, directory, "env");
	DumpImage(ctx, diffuseCubemap_, directory, "diffuse");
	DumpImage(ctx, specularCubemap_, directory, "specular");
	DumpImage(ctx, brdfLut_, directory, "brdf_lut");

	std::ofstream manifest(directory + "/ibl.json");
	manifest << "{\n";
	manifest << "  \"faceOrder\": [\"+X\",\"-X\",\"+Y\",\"-Y\",\"+Z\",\"-Z\"],\n";
	manifest << "  \"images\": [\n";
	manifest << "    {\"name\":\"env\",\"width\":" << environmentCubemap_.width_
		<< ",\"height\":" << environmentCubemap_.height_
		<< ",\"layers\":" << environmentCubemap_.layerCount_
		<< ",\"mips\":" << environmentCubemap_.mipCount_
		<< ",\"format\":" << static_cast<int>(environmentCubemap_.imageFormat_) << "},\n";
	manifest << "    {\"name\":\"diffuse\",\"width\":" << diffuseCubemap_.width_
		<< ",\"height\":" << diffuseCubemap_.height_
		<< ",\"layers\":" << diffuseCubemap_.layerCount_
		<< ",\"mips\":" << diffuseCubemap_.mipCount_
		<< ",\"format\":" << static_cast<int>(diffuseCubemap_.imageFormat_) << "},\n";
	manifest << "    {\"name\":\"specular\",\"width\":" << specularCubemap_.width_
		<< ",\"height\":" << specularCubemap_.height_
		<< ",\"layers\":" << specularCubemap_.layerCount_
		<< ",\"mips\":" << specularCubemap_.mipCount_
		<< ",\"format\":" << static_cast<int>(specularCubemap_.imageFormat_) << "},\n";
	manifest << "    {\"name\":\"brdf_lut\",\"width\":" << brdfLut_.width_
		<< ",\"height\":" << brdfLut_.height_
		<< ",\"layers\":" << brdfLut_.layerCount_
		<< ",\"mips\":" << brdfLut_.mipCount_
		<< ",\"format\":" << static_cast<int>(brdfLut_.imageFormat_) << "}\n";
	manifest << "  ]\n}\n";
}
