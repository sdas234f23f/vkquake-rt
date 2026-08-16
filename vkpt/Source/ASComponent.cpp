// Copyright (c) 2021 Sultim Tsyrendashiev
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "ASComponent.h"

vkpt::ASComponent::ASComponent(VkDevice _device, const char *_debugName)
:
    device(_device),
    as(VK_NULL_HANDLE),
    debugName(_debugName)
{}

vkpt::BLASComponent::BLASComponent(VkDevice _device, VertexCollectorFilterTypeFlags _filter)
:
    ASComponent(_device, VertexCollectorFilterTypeFlags_GetNameForBLAS(_filter)),
    filter(_filter),
    geomCount(0)
{}

vkpt::TLASComponent::TLASComponent(VkDevice _device, const char *_debugName)
: 
    ASComponent(_device, _debugName)
{}

vkpt::ASComponent::~ASComponent()
{
    Destroy();
}

void vkpt::ASComponent::CreateBuffer(const std::shared_ptr<MemoryAllocator> &allocator, VkDeviceSize size)
{
    assert(!buffer.IsInitted());

    buffer.Init(
        allocator, size,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        GetBufferDebugName()
    );
}

void vkpt::ASComponent::Destroy()
{
    assert(device != VK_NULL_HANDLE);

    buffer.Destroy();

    if (as != VK_NULL_HANDLE)
    {
        svkDestroyAccelerationStructureKHR(device, as, nullptr);
        as = VK_NULL_HANDLE;
    }
}

void vkpt::ASComponent::RecreateIfNotValid(const VkAccelerationStructureBuildSizesInfoKHR &buildSizes, const std::shared_ptr<MemoryAllocator> &allocator)
{
    if (!IsValid(buildSizes))
    {
        // destroy
        Destroy();

        // create
        CreateBuffer(allocator, buildSizes.accelerationStructureSize);
        CreateAS(buildSizes.accelerationStructureSize);
    }
}

void vkpt::BLASComponent::CreateAS(VkDeviceSize size)
{
    assert(device != VK_NULL_HANDLE);

    VkAccelerationStructureCreateInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    info.size = size;
    info.buffer = buffer.GetBuffer();

    VkResult r = svkCreateAccelerationStructureKHR(device, &info, nullptr, &as);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, as, VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, debugName);
}

void vkpt::TLASComponent::CreateAS(VkDeviceSize size)
{
    assert(device != VK_NULL_HANDLE);

    VkAccelerationStructureCreateInfoKHR tlasInfo = {};
    tlasInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    tlasInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlasInfo.size = size;
    tlasInfo.buffer = buffer.GetBuffer();

    VkResult r = svkCreateAccelerationStructureKHR(device, &tlasInfo, nullptr, &as);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, as, VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, debugName);
}

bool vkpt::ASComponent::IsValid(const VkAccelerationStructureBuildSizesInfoKHR &buildSizes) const
{
    return buffer.IsInitted() && buffer.GetSize() >= buildSizes.accelerationStructureSize;
}

VkAccelerationStructureKHR vkpt::ASComponent::GetAS() const
{
    return as;
}

VkDeviceAddress vkpt::ASComponent::GetASAddress() const
{
    assert(buffer.IsInitted());
    return GetASAddress(as);
}

VkDeviceAddress vkpt::ASComponent::GetASAddress(VkAccelerationStructureKHR as) const
{
    assert(device != VK_NULL_HANDLE);
    assert(as != VK_NULL_HANDLE);

    VkAccelerationStructureDeviceAddressInfoKHR addressInfo = {};
    addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addressInfo.accelerationStructure = as;

    return svkGetAccelerationStructureDeviceAddressKHR(device, &addressInfo);
}

const char *vkpt::BLASComponent::GetBufferDebugName() const
{
    return "BLAS buffer";
}

const char *vkpt::TLASComponent::GetBufferDebugName() const
{
    return "TLAS buffer";
}

vkpt::VertexCollectorFilterTypeFlags vkpt::BLASComponent::GetFilter() const
{
    return filter;
}

void vkpt::BLASComponent::SetGeometryCount(uint32_t geomCount)
{
    this->geomCount = geomCount;
}

bool vkpt::BLASComponent::IsEmpty() const
{
    return geomCount == 0;
}

uint32_t vkpt::BLASComponent::GetGeomCount() const
{
    return geomCount;
}