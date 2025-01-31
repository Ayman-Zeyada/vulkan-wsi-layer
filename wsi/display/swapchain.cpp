/*
 * Copyright (c) 2024-2025 Arm Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * @file swapchain.cpp
 *
 * @brief Contains the class implementation for a display swapchain.
 */

#include <vulkan/vk_icd.h>
#include <vulkan/vulkan.h>
#include <errno.h>

#include <util/macros.hpp>
#include <wsi/extensions/image_compression_control.hpp>
#include <wsi/extensions/present_id.hpp>
#include <wsi/swapchain_base.hpp>

#include "swapchain.hpp"

namespace wsi
{

namespace display
{

swapchain::swapchain(layer::device_private_data &dev_data, const VkAllocationCallbacks *pAllocator,
                     surface &wsi_surface)
   : wsi::swapchain_base(dev_data, pAllocator)
   , m_wsi_allocator(nullptr)
   , m_display_mode(wsi_surface.get_display_mode())
   , m_image_creation_parameters({}, m_allocator, {}, {})
{
   m_image_create_info.format = VK_FORMAT_UNDEFINED;
}

swapchain::~swapchain()
{
   /* Call the base class teardown */
   teardown();

   /* Free WSI allocator. */
   if (m_wsi_allocator != nullptr)
   {
      wsialloc_delete(m_wsi_allocator);
   }
   m_wsi_allocator = nullptr;
}

static void page_flip_event(int fd, unsigned int sequence, unsigned int tv_sec, unsigned int tv_usec, void *user_data)
{
   UNUSED(fd);
   UNUSED(sequence);
   UNUSED(tv_sec);
   UNUSED(tv_usec);
   bool *done = (bool *)user_data;
   *done = true;
}

VkResult swapchain::add_required_extensions(VkDevice device, const VkSwapchainCreateInfoKHR *swapchain_create_info)
{
   auto compression_control = wsi_ext_image_compression_control::create(device, swapchain_create_info);
   if (compression_control)
   {
      if (!add_swapchain_extension(m_allocator.make_unique<wsi_ext_image_compression_control>(*compression_control)))
      {
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
   }

   if (m_device_data.is_present_id_enabled())
   {
      if (!add_swapchain_extension(m_allocator.make_unique<wsi_ext_present_id>()))
      {
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
   }

   if (m_device_data.should_layer_handle_frame_boundary_events())
   {
      if (!add_swapchain_extension(m_allocator.make_unique<wsi_ext_frame_boundary>(m_device_data)))
      {
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
   }

   return VK_SUCCESS;
}

VkResult swapchain::init_platform(VkDevice device, const VkSwapchainCreateInfoKHR *swapchain_create_info,
                                  bool &use_presentation_thread)
{
   UNUSED(device);
   UNUSED(swapchain_create_info);
   UNUSED(use_presentation_thread);
   WSIALLOC_ASSERT_VERSION();
   if (wsialloc_new(&m_wsi_allocator) != WSIALLOC_ERROR_NONE)
   {
      WSI_LOG_ERROR("Failed to create wsi allocator.");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   return VK_SUCCESS;
}

VkResult swapchain::bind_swapchain_image(VkDevice &device, const VkBindImageMemoryInfo *bind_image_mem_info,
                                         const VkBindImageMemorySwapchainInfoKHR *bind_sc_info)
{
   UNUSED(device);
   const wsi::swapchain_image &swapchain_image = m_swapchain_images[bind_sc_info->imageIndex];
   auto image_data = reinterpret_cast<display_image_data *>(swapchain_image.data);
   return image_data->external_mem.bind_swapchain_image_memory(bind_image_mem_info->image);
}

VkResult swapchain::get_surface_compatible_formats(const VkImageCreateInfo &info,
                                                   util::vector<wsialloc_format> &importable_formats,
                                                   util::vector<uint64_t> &exportable_modifers,
                                                   util::vector<VkDrmFormatModifierPropertiesEXT> &drm_format_props)
{
   TRY_LOG(util::get_drm_format_properties(m_device_data.physical_device, info.format, drm_format_props),
           "Failed to get format properties");

   auto &display = drm_display::get_display();
   if (!display.has_value())
   {
      WSI_LOG_ERROR("DRM display not available.");
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   for (const auto &prop : drm_format_props)
   {
      drm_format_pair drm_format{ util::drm::vk_to_drm_format(info.format), prop.drmFormatModifier };

      if (!display->is_format_supported(drm_format))
      {
         continue;
      }

      VkExternalImageFormatPropertiesKHR external_props = {};
      external_props.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES_KHR;

      VkImageFormatProperties2KHR format_props = {};
      format_props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2_KHR;
      format_props.pNext = &external_props;

      VkResult result = VK_SUCCESS;
      {
         VkPhysicalDeviceExternalImageFormatInfoKHR external_info = {};
         external_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO_KHR;
         external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

         VkPhysicalDeviceImageDrmFormatModifierInfoEXT drm_mod_info = {};
         drm_mod_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
         drm_mod_info.pNext = &external_info;
         drm_mod_info.drmFormatModifier = prop.drmFormatModifier;
         drm_mod_info.sharingMode = info.sharingMode;
         drm_mod_info.queueFamilyIndexCount = info.queueFamilyIndexCount;
         drm_mod_info.pQueueFamilyIndices = info.pQueueFamilyIndices;

         VkPhysicalDeviceImageFormatInfo2KHR image_info = {};
         image_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2_KHR;
         image_info.pNext = &drm_mod_info;
         image_info.format = info.format;
         image_info.type = info.imageType;
         image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
         image_info.usage = info.usage;
         image_info.flags = info.flags;

         VkImageCompressionControlEXT compression_control = {};

         if (m_device_data.is_swapchain_compression_control_enabled())
         {
            auto *ext = get_swapchain_extension<wsi_ext_image_compression_control>();
            /* For Image compression control, additional requirements to be satisfied such as
               existence of VkImageCompressionControlEXT in swaphain_create_info for
               the ext to be added to the list. so we check whether we got a valid pointer
               and proceed if yes. */
            if (ext)
            {
               compression_control = ext->get_compression_control_properties();
               compression_control.pNext = image_info.pNext;
               image_info.pNext = &compression_control;
            }
         }
         result = m_device_data.instance_data.disp.GetPhysicalDeviceImageFormatProperties2KHR(
            m_device_data.physical_device, &image_info, &format_props);
      }
      if (result != VK_SUCCESS)
      {
         continue;
      }
      if (format_props.imageFormatProperties.maxExtent.width < info.extent.width ||
          format_props.imageFormatProperties.maxExtent.height < info.extent.height ||
          format_props.imageFormatProperties.maxExtent.depth < info.extent.depth)
      {
         continue;
      }
      if (format_props.imageFormatProperties.maxMipLevels < info.mipLevels ||
          format_props.imageFormatProperties.maxArrayLayers < info.arrayLayers)
      {
         continue;
      }
      if ((format_props.imageFormatProperties.sampleCounts & info.samples) != info.samples)
      {
         continue;
      }

      if (external_props.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT_KHR)
      {
         if (!exportable_modifers.try_push_back(drm_format.modifier))
         {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
         }
      }

      if (external_props.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT_KHR)
      {
         uint64_t flags =
            (prop.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_DISJOINT_BIT) ? 0 : WSIALLOC_FORMAT_NON_DISJOINT;
         wsialloc_format import_format{ drm_format.fourcc, drm_format.modifier, flags };
         if (!importable_formats.try_push_back(import_format))
         {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
         }
      }
   }

   return VK_SUCCESS;
}

VkResult swapchain::allocate_wsialloc(VkImageCreateInfo &image_create_info, display_image_data *image_data,
                                      util::vector<wsialloc_format> &importable_formats,
                                      wsialloc_format *allocated_format, bool avoid_allocation)
{
   bool is_protected_memory = (image_create_info.flags & VK_IMAGE_CREATE_PROTECTED_BIT) != 0;
   uint64_t allocation_flags = is_protected_memory ? WSIALLOC_ALLOCATE_PROTECTED : 0;
   if (avoid_allocation)
   {
      allocation_flags |= WSIALLOC_ALLOCATE_NO_MEMORY;
   }

   if (m_device_data.is_swapchain_compression_control_enabled())
   {
      auto *ext = get_swapchain_extension<wsi_ext_image_compression_control>();
      /* For Image compression control, additional requirements to be satisfied such as
         existence of VkImageCompressionControlEXT in swaphain_create_info for
         the ext to be added to the list. so we check whether we got a valid pointer
         and proceed if yes. */
      if (ext)
      {
         if (ext->get_bitmask_for_image_compression_flags() & VK_IMAGE_COMPRESSION_FIXED_RATE_EXPLICIT_EXT)
         {
            allocation_flags |= WSIALLOC_ALLOCATE_HIGHEST_FIXED_RATE_COMPRESSION;
         }
      }
   }

   wsialloc_allocate_info alloc_info = { importable_formats.data(), static_cast<unsigned>(importable_formats.size()),
                                         image_create_info.extent.width, image_create_info.extent.height,
                                         allocation_flags };
   wsialloc_allocate_result alloc_result = { .format = { .fourcc = 0, .modifier = 0, .flags = 0 },
                                             .average_row_strides = { 0 },
                                             .offsets = { 0 },
                                             .buffer_fds = { 0 },
                                             .is_disjoint = false };

   /* Clear buffer_fds and average_row_strides for error purposes */
   for (int i = 0; i < WSIALLOC_MAX_PLANES; ++i)
   {
      alloc_result.buffer_fds[i] = -1;
   }
   const auto res = wsialloc_alloc(m_wsi_allocator, &alloc_info, &alloc_result);
   if (res != WSIALLOC_ERROR_NONE)
   {
      WSI_LOG_ERROR("Failed allocation of DMA Buffer. WSI error: %d", static_cast<int>(res));
      if (res == WSIALLOC_ERROR_NOT_SUPPORTED)
      {
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
      }
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   *allocated_format = alloc_result.format;
   auto &external_memory = image_data->external_mem;
   external_memory.set_strides(alloc_result.average_row_strides);
   external_memory.set_buffer_fds(alloc_result.buffer_fds);
   external_memory.set_offsets(alloc_result.offsets);

   uint32_t num_planes = util::drm::drm_fourcc_format_get_num_planes(alloc_result.format.fourcc);

   if (!avoid_allocation)
   {
      uint32_t num_memory_planes = 0;

      for (uint32_t i = 0; i < num_planes; ++i)
      {
         auto it = std::find(std::begin(alloc_result.buffer_fds) + i + 1, std::end(alloc_result.buffer_fds),
                             alloc_result.buffer_fds[i]);
         if (it == std::end(alloc_result.buffer_fds))
         {
            num_memory_planes++;
         }
      }

      assert(alloc_result.is_disjoint == (num_memory_planes > 1));
      external_memory.set_num_memories(num_memory_planes);
   }

   external_memory.set_format_info(alloc_result.is_disjoint, num_planes);
   external_memory.set_memory_handle_type(VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
   return VK_SUCCESS;
}

static VkResult fill_image_create_info(VkImageCreateInfo &image_create_info,
                                       util::vector<VkSubresourceLayout> &image_plane_layouts,
                                       VkImageDrmFormatModifierExplicitCreateInfoEXT &drm_mod_info,
                                       VkExternalMemoryImageCreateInfoKHR &external_info,
                                       display_image_data &image_data, uint64_t modifier)
{
   TRY_LOG_CALL(image_data.external_mem.fill_image_plane_layouts(image_plane_layouts));

   if (image_data.external_mem.is_disjoint())
   {
      image_create_info.flags |= VK_IMAGE_CREATE_DISJOINT_BIT;
   }

   image_data.external_mem.fill_drm_mod_info(image_create_info.pNext, drm_mod_info, image_plane_layouts, modifier);
   image_data.external_mem.fill_external_info(external_info, &drm_mod_info);
   image_create_info.pNext = &external_info;
   image_create_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
   return VK_SUCCESS;
}

VkResult swapchain::allocate_image(display_image_data *image_data)
{
   util::vector<wsialloc_format> importable_formats(util::allocator(m_allocator, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND));
   auto &m_allocated_format = m_image_creation_parameters.m_allocated_format;
   if (!importable_formats.try_push_back(m_allocated_format))
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   TRY_LOG_CALL(allocate_wsialloc(m_image_create_info, image_data, importable_formats, &m_allocated_format, false));

   return VK_SUCCESS;
}

VkResult swapchain::create_framebuffer(const VkImageCreateInfo &image_create_info, display_image_data *image_data)
{
   VkResult ret_code = VK_SUCCESS;
   std::array<uint32_t, util::MAX_PLANES> strides{ 0, 0, 0, 0 };
   std::array<uint64_t, util::MAX_PLANES> modifiers{ 0, 0, 0, 0 };
   const drm_format_pair allocated_format{ m_image_creation_parameters.m_allocated_format.fourcc,
                                           m_image_creation_parameters.m_allocated_format.modifier };

   auto &display = drm_display::get_display();
   if (!display.has_value())
   {
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   drm_gem_handle_array<util::MAX_PLANES> buffer_handles{ display->get_drm_fd() };

   const auto &buffer_fds = image_data->external_mem.get_buffer_fds();

   for (uint32_t plane = 0; plane < image_data->external_mem.get_num_planes(); plane++)
   {
      assert(image_data->external_mem.get_strides()[plane] > 0);
      strides[plane] = image_data->external_mem.get_strides()[plane];
      modifiers[plane] = allocated_format.modifier;
      if (drmPrimeFDToHandle(display->get_drm_fd(), buffer_fds[plane], &buffer_handles[plane]) != 0)
      {
         WSI_LOG_ERROR("Failed to convert buffer FD to GEM handle: %s", std::strerror(errno));
         return VK_ERROR_INITIALIZATION_FAILED;
      }
   }

   if (!display->is_format_supported(allocated_format))
   {
      WSI_LOG_ERROR("Format not supported.");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   int error = 0;
   if (display->supports_fb_modifiers())
   {
      error = drmModeAddFB2WithModifiers(
         display->get_drm_fd(), image_create_info.extent.width, image_create_info.extent.height,
         allocated_format.fourcc, buffer_handles.data(), strides.data(), image_data->external_mem.get_offsets().data(),
         modifiers.data(), &image_data->fb_id, DRM_MODE_FB_MODIFIERS);
   }
   else
   {
      error = drmModeAddFB2(display->get_drm_fd(), image_create_info.extent.width, image_create_info.extent.height,
                            allocated_format.fourcc, buffer_handles.data(), strides.data(),
                            image_data->external_mem.get_offsets().data(), &image_data->fb_id, 0);
   }

   if (error != 0)
   {
      WSI_LOG_ERROR("Failed to create framebuffer: %s", strerror(errno));
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   return ret_code;
}

VkResult swapchain::allocate_and_bind_swapchain_image(VkImageCreateInfo image_create_info, swapchain_image &image)
{
   std::unique_lock<std::recursive_mutex> image_status_lock(m_image_status_mutex);
   image.status = swapchain_image::FREE;
   assert(image.data != nullptr);
   auto image_data = static_cast<display_image_data *>(image.data);
   TRY_LOG(allocate_image(image_data), "Failed to allocate image");

   image_status_lock.unlock();

   TRY_LOG(create_framebuffer(image_create_info, image_data), "Failed to create framebuffer");

   TRY_LOG(image_data->external_mem.import_memory_and_bind_swapchain_image(image.image),
           "Failed to import memory and bind swapchain image");

   /* Initialize presentation fence. */
   auto present_fence = sync_fd_fence_sync::create(m_device_data);
   if (!present_fence.has_value())
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   image_data->present_fence = std::move(present_fence.value());

   return VK_SUCCESS;
}

VkResult swapchain::create_swapchain_image(VkImageCreateInfo image_create_info, swapchain_image &image)
{
   /* Create image_data */
   auto image_data = m_allocator.create<display_image_data>(1, m_device, m_allocator);
   if (image_data == nullptr)
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   image.data = image_data;

   if (m_image_create_info.format == VK_FORMAT_UNDEFINED)
   {
      util::vector<wsialloc_format> importable_formats(
         util::allocator(m_allocator, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND));
      util::vector<uint64_t> exportable_modifiers(util::allocator(m_allocator, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND));

      /* Query supported modifers. */
      util::vector<VkDrmFormatModifierPropertiesEXT> drm_format_props(
         util::allocator(m_allocator, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND));

      TRY_LOG_CALL(
         get_surface_compatible_formats(image_create_info, importable_formats, exportable_modifiers, drm_format_props));

      /* TODO: Handle exportable images which use ICD allocated memory in preference to an external allocator. */
      if (importable_formats.empty())
      {
         WSI_LOG_ERROR("Export/Import not supported.");
         return VK_ERROR_INITIALIZATION_FAILED;
      }

      wsialloc_format allocated_format = { .fourcc = 0, .modifier = 0, .flags = 0 };

      TRY_LOG_CALL(allocate_wsialloc(image_create_info, image_data, importable_formats, &allocated_format, true));

      for (auto &prop : drm_format_props)
      {
         if (prop.drmFormatModifier == allocated_format.modifier)
         {
            image_data->external_mem.set_num_memories(prop.drmFormatModifierPlaneCount);
         }
      }

      TRY_LOG_CALL(fill_image_create_info(
         image_create_info, m_image_creation_parameters.m_image_layout, m_image_creation_parameters.m_drm_mod_info,
         m_image_creation_parameters.m_external_info, *image_data, allocated_format.modifier));

      m_image_create_info = image_create_info;
      m_image_creation_parameters.m_allocated_format = allocated_format;
   }

   return m_device_data.disp.CreateImage(m_device, &m_image_create_info, get_allocation_callbacks(), &image.image);
}

void swapchain::present_image(const pending_present_request &pending_present)
{
   int drm_res = 0;
   display_image_data *image_data =
      reinterpret_cast<display_image_data *>(m_swapchain_images[pending_present.image_index].data);
   const auto &display = drm_display::get_display();
   if (!display.has_value())
   {
      set_error_state(VK_ERROR_SURFACE_LOST_KHR);
      return;
   }

   if (m_first_present)
   {
      /* Now we can set the mode of the new swapchain. */
      drmModeModeInfo modeInfo = m_display_mode->get_drm_mode();

      uint32_t connector_id = display->get_connector_id();
      drm_res = drmModeSetCrtc(display->get_drm_fd(), display->get_crtc_id(), image_data->fb_id, 0, 0, &connector_id, 1,
                               &modeInfo);

      if (drm_res != 0)
      {
         WSI_LOG_ERROR("drmModeSetCrtc failed: %s\n", std::strerror(errno));
         set_error_state(VK_ERROR_SURFACE_LOST_KHR);
         return;
      }
   }
   /* The swapchain has already started presenting. */
   else
   {

      bool page_flip_complete = false;

      drm_res = drmModePageFlip(display->get_drm_fd(), display->get_crtc_id(), image_data->fb_id,
                                DRM_MODE_PAGE_FLIP_EVENT, (void *)&page_flip_complete);

      if (drm_res != 0)
      {
         WSI_LOG_ERROR("drmModePageFlip failed: %s\n", std::strerror(errno));
         set_error_state(VK_ERROR_SURFACE_LOST_KHR);
         return;
      }

      fd_set fds;
      FD_ZERO(&fds);
      FD_SET(display->get_drm_fd(), &fds);

      do
      {
         struct timeval t;
         t.tv_sec = 1;
         t.tv_usec = 0;
         drm_res = select(display->get_drm_fd() + 1, &fds, NULL, NULL, &t);

         if (drm_res < 0)
         {
            if (errno != EINTR && errno != EAGAIN)
            {
               WSI_LOG_ERROR("select() failed with errno: %d\n", errno);
               set_error_state(VK_ERROR_SURFACE_LOST_KHR);
               break;
            }
            WSI_LOG_ERROR("select() failed with %d, carrying on with page flip\n", errno);
         }
         else if (drm_res == 0)
         {
            WSI_LOG_ERROR("select() timed out, carrying on with page flip\n");
         }
         else
         {
            int result = FD_ISSET(display->get_drm_fd(), &fds);
            assert(result > 0);
            UNUSED(result);
            drmEventContext ev = {};
            ev.version = DRM_EVENT_CONTEXT_VERSION;
            ev.page_flip_handler = page_flip_event;

            drmHandleEvent(display->get_drm_fd(), &ev);
         }
      } while ((drm_res == -1 && (errno == EINTR || errno == EAGAIN)) || drm_res == 0 || !page_flip_complete);
   }

   /* Find currently presented image */
   uint32_t presented_index = m_swapchain_images.size();
   if (!m_first_present)
   {
      for (uint32_t i = 0; i < m_swapchain_images.size(); ++i)
      {
         if (m_swapchain_images[i].status == swapchain_image::PRESENTED)
         {
            presented_index = i;
            break;
         }
      }
      /* There should always be a presented image, unless there was an error */
      assert(presented_index < m_swapchain_images.size());
   }
   /* The image is on screen, change the image status to PRESENTED. */
   m_swapchain_images[pending_present.image_index].status = swapchain_image::PRESENTED;

   if (m_device_data.is_present_id_enabled())
   {
      auto *ext = get_swapchain_extension<wsi_ext_present_id>(true);
      ext->set_present_id(pending_present.present_id);
   }

   /* And release the old one. */
   if (presented_index < m_swapchain_images.size())
   {
      unpresent_image(presented_index);
   }

   return;
}

VkResult swapchain::image_set_present_payload(swapchain_image &image, VkQueue queue,
                                              const queue_submit_semaphores &semaphores, const void *submission_pnext)
{
   auto image_data = reinterpret_cast<display_image_data *>(image.data);
   return image_data->present_fence.set_payload(queue, semaphores, submission_pnext);
}

VkResult swapchain::image_wait_present(swapchain_image &image, uint64_t timeout)
{
   auto data = reinterpret_cast<display_image_data *>(image.data);
   return data->present_fence.wait_payload(timeout);
}

void swapchain::destroy_image(swapchain_image &image)
{
   std::unique_lock<std::recursive_mutex> image_status_lock(m_image_status_mutex);

   if (image.status != swapchain_image::INVALID)
   {
      if (image.image != VK_NULL_HANDLE)
      {
         m_device_data.disp.DestroyImage(m_device, image.image, get_allocation_callbacks());
         image.image = VK_NULL_HANDLE;
      }

      image.status = swapchain_image::INVALID;
   }

   image_status_lock.unlock();

   if (image.data != nullptr)
   {
      auto image_data = reinterpret_cast<display_image_data *>(image.data);
      auto &display = drm_display::get_display();
      if (!display.has_value())
      {
         return;
      }
      if (image_data->fb_id != std::numeric_limits<uint32_t>::max())
      {
         int result = drmModeRmFB(display->get_drm_fd(), image_data->fb_id);
         assert(result == 0);
         UNUSED(result);
      }

      m_allocator.destroy(1, image_data);
      image.data = nullptr;
   }
}

} /* namespace display */

} /* namespace wsi*/
