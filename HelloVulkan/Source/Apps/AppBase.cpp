#include "AppBase.h"
#include "Configs.h"
#include "ResourcesShared.h"
#include "VulkanCheck.h"

#include "volk.h"
#include "imgui_impl_vulkan.h"
#include "glslang/Include/glslang_c_interface.h"

#include <iostream>
#include <array>

AppBase::AppBase() :
	shouldRecreateSwapchain_(false)
{
	InitGLFW();
	InitGLSLang();
	InitCamera();
}

void AppBase::InitVulkan(ContextConfig config)
{
	// Initialize Volk
	const VkResult res = volkInitialize();
	if (res != VK_SUCCESS)
	{
		throw std::runtime_error("Volk Cannot be initialized");
	}

	// Initialize Vulkan instance
	vulkanInstance_.Create();
	vulkanInstance_.SetupDebugCallbacks();
	vulkanInstance_.CreateWindowSurface(glfwWindow_);
	vulkanContext_.Create(vulkanInstance_, config);
}

void AppBase::InitGLSLang()
{
	glslang_initialize_process();
}

void AppBase::InitGLFW()
{
	glfwSetErrorCallback([](int error, const char* description)
	{
		throw std::runtime_error(std::string("GLFW Error: ") + description);
	});
	
	glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
	glfwInit();
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
	glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_FALSE);

	constexpr int w = AppConfig::InitialScreenWidth;
	constexpr int h = AppConfig::InitialScreenHeight;

	// GLFW window creation
	windowWidth_ = static_cast<uint32_t>(w);
	windowHeight_ = static_cast<uint32_t>(h);
	glfwWindow_ = glfwCreateWindow(
		w,
		h,
		AppConfig::ScreenTitle.c_str(),
		nullptr,
		nullptr);
	if (glfwWindow_ == nullptr)
	{
		glfwTerminate();
		throw std::runtime_error("Failed to create GLFW window");
	}

	glfwSetWindowUserPointer(glfwWindow_, this);
	
	auto funcFramebuffer = [](GLFWwindow* window, int width, int height)
		{ static_cast<AppBase*>(glfwGetWindowUserPointer(window))->FrameBufferSizeCallback(window, width, height); };
	glfwSetFramebufferSizeCallback(glfwWindow_, funcFramebuffer);

	auto funcCursor = [](GLFWwindow* window, double x, double y)
		{ static_cast<AppBase*>(glfwGetWindowUserPointer(window))->MouseCallback(window, x, y); };
	glfwSetCursorPosCallback(glfwWindow_, funcCursor);

	auto funcMouse = [](GLFWwindow* window, int button, int action, int mods)
		{ static_cast<AppBase*>(glfwGetWindowUserPointer(window))->MouseButtonCallback(window, button, action, mods); };
	glfwSetMouseButtonCallback(glfwWindow_, funcMouse);

	auto funcScroll = [](GLFWwindow* window, double xOffset, double yOffset)
		{ static_cast<AppBase*>(glfwGetWindowUserPointer(window))->ScrollCallback(window, xOffset, yOffset); };
	glfwSetScrollCallback(glfwWindow_, funcScroll);

	auto funcKey = [](GLFWwindow* window, int key, int scancode, int action, int mods)
		{ static_cast<AppBase*>(glfwGetWindowUserPointer(window))->KeyCallback(window, key, scancode, action, mods); };
	glfwSetKeyCallback(glfwWindow_, funcKey);
}

// TODO Analyze this function for possible performance improvement

void AppBase::RecordGroundTruthCapture(VkCommandBuffer commandBuffer, uint32_t swapchainImageIndex)
{
	const uint32_t width = vulkanContext_.GetSwapchainWidth();
	const uint32_t height = vulkanContext_.GetSwapchainHeight();
	if (captureStaging_.buffer_ == nullptr)
	{
		captureStaging_.CreateBuffer(
			vulkanContext_,
			static_cast<VkDeviceSize>(width) * height * 4,
			VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VMA_MEMORY_USAGE_CPU_ONLY);
	}

	const VkImage image = vulkanContext_.GetSwapchainImage(swapchainImageIndex);
	const VkImageMemoryBarrier toTransfer = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vkCmdPipelineBarrier(commandBuffer,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nullptr, 0, nullptr, 1, &toTransfer);

	const VkBufferImageCopy region = {
		.bufferOffset = 0,
		.bufferRowLength = 0,
		.bufferImageHeight = 0,
		.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		.imageOffset = {0, 0, 0},
		.imageExtent = {width, height, 1},
	};
	vkCmdCopyImageToBuffer(commandBuffer, image,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, captureStaging_.buffer_, 1, &region);

	const VkImageMemoryBarrier toPresent = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.dstAccessMask = 0,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vkCmdPipelineBarrier(commandBuffer,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		0, 0, nullptr, 0, nullptr, 1, &toPresent);
}

void AppBase::WriteGroundTruthImage()
{
	const char* path = std::getenv("ZTGFX_GROUND_TRUTH_IMAGE");
	if (path == nullptr || path[0] == '\0')
	{
		return;
	}
	const uint32_t width = vulkanContext_.GetSwapchainWidth();
	const uint32_t height = vulkanContext_.GetSwapchainHeight();
	const VkFormat format = vulkanContext_.GetSwapchainImageFormat();
	const bool bgra =
		format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;

	void* mapped = nullptr;
	vmaMapMemory(captureStaging_.vmaAllocator_, captureStaging_.vmaAllocation_, &mapped);
	const auto* pixels = static_cast<const uint8_t*>(mapped);

	FILE* out = fopen(path, "wb");
	if (out == nullptr)
	{
		throw std::runtime_error(std::string("cannot open ground-truth image ") + path);
	}
	fprintf(out, "P6\n%u %u\n255\n", width, height);
	for (uint32_t y = 0; y < height; ++y)
	{
		for (uint32_t x = 0; x < width; ++x)
		{
			const uint8_t* pixel = pixels + (static_cast<size_t>(y) * width + x) * 4;
			const uint8_t rgb[3] = {
				bgra ? pixel[2] : pixel[0],
				pixel[1],
				bgra ? pixel[0] : pixel[2],
			};
			fwrite(rgb, 1, 3, out);
		}
	}
	fclose(out);
	vmaUnmapMemory(captureStaging_.vmaAllocator_, captureStaging_.vmaAllocation_);

	std::string metaPath = std::string(path) + ".json";
	FILE* meta = fopen(metaPath.c_str(), "wb");
	if (meta != nullptr)
	{
		fprintf(meta,
			"{\"version\":2,\"width\":%u,\"height\":%u,\"format\":%d,\"frame\":%u",
			width, height, static_cast<int>(format), captureFrame_);
		const std::string extra = GroundTruthMetadata();
		if (!extra.empty())
		{
			fprintf(meta, ",%s", extra.c_str());
		}
		fprintf(meta, "}\n");
		fclose(meta);
	}
}

void AppBase::DrawFrame()
{
	FrameData& frameData = vulkanContext_.GetCurrentFrameData();
	{
		ZoneScopedNC("WaitForFences", tracy::Color::GreenYellow);
		vkWaitForFences(vulkanContext_.GetDevice(), 1, &(frameData.queueSubmitFence_), VK_TRUE, UINT64_MAX);
	}

	{
		ZoneScopedNC("AcquireNextImageKHR", tracy::Color::PaleGreen);
		VkResult result = vulkanContext_.GetNextSwapchainImage(frameData.nextSwapchainImageSemaphore_);
		if (result == VK_ERROR_OUT_OF_DATE_KHR)
		{
			OnWindowResized();
			return;
		}
	}

	uint32_t swapchainImageIndex = vulkanContext_.GetCurrentSwapchainImageIndex();
	
	{
		ZoneScopedNC("ResetFences_ResetCommandBuffer", tracy::Color::Orange);
		vkResetFences(vulkanContext_.GetDevice(), 1, &(frameData.queueSubmitFence_));
		vkResetCommandBuffer(frameData.graphicsCommandBuffer_, 0);
	}
	
	{
		ZoneScopedNC("UpdateUI", tracy::Color::Aquamarine1);
		// ImGui first then UBOs, because ImGui sets a few UBO values
		UpdateUI();
	}

	{
		ZoneScopedNC("UpdateUBOs", tracy::Color::BlueViolet);
		// Send UBOs to buffers
		UpdateUBOs();
	}

	{
		ZoneScopedNC("RecordCommandBuffer", tracy::Color::OrangeRed);
		// Start recording command buffers
		FillCommandBuffer(frameData.graphicsCommandBuffer_);
	}

	{
		ZoneScopedNC("QueueSubmit", tracy::Color::VioletRed);

		constexpr VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
		std::array<VkSemaphore, 1> waitSemaphores{ frameData.nextSwapchainImageSemaphore_ };

		// Submit Queue
		// TODO Set code below as a function in VulkanDevice
		const VkSubmitInfo submitInfo =
		{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.pNext = nullptr,
			.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size()),
			.pWaitSemaphores = waitSemaphores.data(),
			.pWaitDstStageMask = waitStages,
			.commandBufferCount = 1u,
			.pCommandBuffers = &(frameData.graphicsCommandBuffer_),
			.signalSemaphoreCount = 1u,
			.pSignalSemaphores = &(frameData.graphicsQueueSemaphore_)
		};
	
		VK_CHECK(vkQueueSubmit(vulkanContext_.GetGraphicsQueue(), 1, &submitInfo, frameData.queueSubmitFence_));
	}

	if (capturePending_)
	{
		vkQueueWaitIdle(vulkanContext_.GetGraphicsQueue());
		WriteGroundTruthImage();
		std::exit(0);
	}
	
	{
		ZoneScopedNC("QueuePresentKHR", tracy::Color::VioletRed1);
		// Present
		// TODO Set code below as a function in VulkanContext
		const VkPresentInfoKHR presentInfo =
		{
			.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
			.pNext = nullptr,
			.waitSemaphoreCount = 1u,
			.pWaitSemaphores = &(frameData.graphicsQueueSemaphore_),
			.swapchainCount = 1u,
			.pSwapchains = vulkanContext_.GetSwapchainPtr(),
			.pImageIndices = &swapchainImageIndex
		};

		VkResult result = vkQueuePresentKHR(vulkanContext_.GetGraphicsQueue(), &presentInfo);
		if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || shouldRecreateSwapchain_)
		{
			OnWindowResized();
		}
	}

	// Do this after the end of the draw
	vulkanContext_.IncrementFrameIndex();

	// Mouse input
	if (uiData_.mouseLeftPressed_)
	{
		uiData_.mouseLeftPressed_ = false;
		uiData_.mouseLeftHold_ = true;
	}

	// End Tracy frame
	FrameMark;
}

void AppBase::UpdateUI()
{
	// Empty, must be implemented in a derived class
}

// Fill/record command buffer
void AppBase::FillCommandBuffer(VkCommandBuffer commandBuffer)
{
	constexpr VkCommandBufferBeginInfo beginInfo =
	{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.pNext = nullptr,
		.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
		.pInheritanceInfo = nullptr
	};
	
	VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));
	
	{
		TracyVkZoneC(vulkanContext_.GetTracyContext(), commandBuffer, "Render", tracy::Color::OrangeRed);

		// Iterate through all pipelines to fill the command buffer
		const bool groundTruth = std::getenv("ZTGFX_GROUND_TRUTH_IMAGE") != nullptr;
		for (const auto& pip : pipelines_)
		{
			if (groundTruth && pip->skipInGroundTruthCapture())
			{
				continue;
			}
			pip->FillCommandBuffer(vulkanContext_, commandBuffer);
		}
	}

	TracyVkCollect(vulkanContext_.GetTracyContext(), commandBuffer);

	if (std::getenv("ZTGFX_GROUND_TRUTH_IMAGE") != nullptr && captureFrame_ >= 2
		&& !capturePending_)
	{
		RecordGroundTruthCapture(commandBuffer, vulkanContext_.GetCurrentSwapchainImageIndex());
		capturePending_ = true;
	}
	++captureFrame_;

	VK_CHECK(vkEndCommandBuffer(commandBuffer));
}

// Recreate resources when window is resized
void AppBase::OnWindowResized()
{
	int width = 0, height = 0;
	glfwGetFramebufferSize(glfwWindow_, &width, &height);
	while (width == 0 || height == 0)
	{
		glfwGetFramebufferSize(glfwWindow_, &width, &height);
		glfwWaitEvents();
	}

	vkDeviceWaitIdle(vulkanContext_.GetDevice());

	vulkanContext_.RecreateSwapchainResources(
		vulkanInstance_,
		windowWidth_,
		windowHeight_
	);

	for (const auto& res : resources_)
	{
		res->OnWindowResized(vulkanContext_);
	}

	for (const auto& pip : pipelines_)
	{
		pip->OnWindowResized(vulkanContext_);
	}

	shouldRecreateSwapchain_ = false;
}

void AppBase::InitCamera()
{
	camera_ = std::make_unique<Camera>(glm::vec3(0.0f, 0.0f, 3.0f));
}

void AppBase::ProcessTiming()
{
	// Per-frame time
	float currentFrame = static_cast<float>(glfwGetTime());
	frameCounter_.Update(currentFrame);
}

bool AppBase::ShowImGui()
{
	return uiData_.imguiShow_;
}

bool AppBase::StillRunning()
{
	if (glfwWindowShouldClose(glfwWindow_))
	{
		vkDeviceWaitIdle(vulkanContext_.GetDevice());
		return false;
	}
	return true;
}

void AppBase::PollEvents()
{
	glfwPollEvents();
}

void AppBase::DestroyResources()
{
	for (auto& res : resources_) { res.reset(); }
	for (auto& pip : pipelines_) { pip.reset(); }

	glfwDestroyWindow(glfwWindow_);
	glfwTerminate();

	vulkanContext_.Destroy();
	vulkanInstance_.Destroy();
}

void AppBase::FrameBufferSizeCallback(GLFWwindow* window, int width, int height)
{
	windowWidth_ = static_cast<uint32_t>(width);
	windowHeight_ = static_cast<uint32_t>(height);

	camera_->SetScreenSize(
		static_cast<float>(windowWidth_),
		static_cast<float>(windowHeight_)
	);

	shouldRecreateSwapchain_ = true;
}

void AppBase::MouseCallback(GLFWwindow* window, double xposIn, double yposIn)
{
	const float xPos = static_cast<float>(xposIn);
	const float yPos = static_cast<float>(yposIn);
	if (uiData_.mouseFirstUse_)
	{
		uiData_.mousePositionX_ = xPos;
		uiData_.mousePositionY_ = yPos;
		uiData_.mouseFirstUse_ = false;
		return;
	}

	if (uiData_.mouseLeftPressed_ || uiData_.mouseLeftHold_)
	{
		const float xOffset = xPos - uiData_.mousePositionX_;
		const float yOffset = uiData_.mousePositionY_ - yPos; // Reversed since y-coordinates go from bottom to top
		camera_->ProcessMouseMovement(xOffset, yOffset);
	}

	uiData_.mousePositionX_ = xPos;
	uiData_.mousePositionY_ = yPos;
	
}

void AppBase::MouseButtonCallback(GLFWwindow* window, int button, int action, int mods)
{
	if (!ImGui::GetIO().WantCaptureMouse && button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS)
	{
		uiData_.mouseLeftPressed_ = true;
	}
	else if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE)
	{
		uiData_.mouseLeftPressed_ = false;
		uiData_.mouseLeftHold_ = false;
	}
}

void AppBase::ScrollCallback(GLFWwindow* window, double xoffset, double yoffset)
{
	camera_->ProcessMouseScroll(static_cast<float>(yoffset));
}

void AppBase::KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
	if (key == GLFW_KEY_I && action == GLFW_PRESS)
	{
		uiData_.imguiShow_ = !uiData_.imguiShow_;
	}
}

// Process all input: query GLFW whether relevant keys are pressed/released this frame and react accordingly
void AppBase::ProcessInput()
{
	if (glfwGetKey(glfwWindow_, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
		glfwGetKey(glfwWindow_, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS)
	{
		if (glfwGetKey(glfwWindow_, GLFW_KEY_N) == GLFW_PRESS) { uiData_.gizmoMode_ = GizmoMode::None; }
		else if (glfwGetKey(glfwWindow_, GLFW_KEY_T) == GLFW_PRESS) { uiData_.gizmoMode_ = GizmoMode::Translate; }
		else if (glfwGetKey(glfwWindow_, GLFW_KEY_R) == GLFW_PRESS) { uiData_.gizmoMode_ = GizmoMode::Rotate; }
		else if (glfwGetKey(glfwWindow_, GLFW_KEY_S) == GLFW_PRESS) { uiData_.gizmoMode_ = GizmoMode::Scale; }
	}
	else
	{
		if (glfwGetKey(glfwWindow_, GLFW_KEY_ESCAPE) == GLFW_PRESS)
			{ glfwSetWindowShouldClose(glfwWindow_, true); }
		
		if (glfwGetKey(glfwWindow_, GLFW_KEY_W) == GLFW_PRESS)
			{ camera_->ProcessKeyboard(CameraMovement::Forward, frameCounter_.GetDeltaSecond()); }
		else if (glfwGetKey(glfwWindow_, GLFW_KEY_S) == GLFW_PRESS)
			{ camera_->ProcessKeyboard(CameraMovement::Backward, frameCounter_.GetDeltaSecond()); }
		else if (glfwGetKey(glfwWindow_, GLFW_KEY_A) == GLFW_PRESS)
			{ camera_->ProcessKeyboard(CameraMovement::Left, frameCounter_.GetDeltaSecond()); }
		else if (glfwGetKey(glfwWindow_, GLFW_KEY_D) == GLFW_PRESS)
			{ camera_->ProcessKeyboard(CameraMovement::Right, frameCounter_.GetDeltaSecond()); }
	}
}

// TODO Remove this function
void AppBase::InitSharedResources()
{
	if (!resourcesShared_)
	{
		resourcesShared_ = AddResources<ResourcesShared>();
	}
	resourcesShared_->Create(vulkanContext_);
}