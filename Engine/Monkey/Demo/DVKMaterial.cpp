#include "DVKMaterial.h"
#include "DVKDefaultRes.h"

namespace vk_demo
{

	DVKRingBuffer*	DVKMaterial::ringBuffer = nullptr;
	int32			DVKMaterial::ringBufferRefCount = 0;

	void DVKMaterial::InitRingBuffer(std::shared_ptr<VulkanDevice> vulkanDevice)
	{
		ringBuffer = new DVKRingBuffer();
		ringBuffer->device		 = vulkanDevice->GetInstanceHandle();
		ringBuffer->bufferSize   = 32 * 1024 * 1024; // 32MB
		ringBuffer->bufferOffset = ringBuffer->bufferSize;
		ringBuffer->minAlignment = vulkanDevice->GetLimits().minUniformBufferOffsetAlignment;
		ringBuffer->realBuffer   = vk_demo::DVKBuffer::CreateBuffer(
			vulkanDevice,
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			ringBuffer->bufferSize
		);
		ringBuffer->realBuffer->Map();

		ringBufferRefCount = 0;
	}

	void DVKMaterial::DestroyRingBuffer()
	{
		delete ringBuffer;
		ringBuffer = nullptr;
		ringBufferRefCount = 0;
	}

	DVKMaterial::~DVKMaterial()
	{
		shader = nullptr;

		delete descriptorSet;
		descriptorSet = nullptr;

		textures.clear();
		uniformBuffers.clear();

		vulkanDevice = nullptr;
        
        if (pipeline) {
            delete pipeline;
            pipeline = nullptr;
        }

		ringBufferRefCount -= 1;
		if (ringBufferRefCount == 0) {
			DestroyRingBuffer();
		}
	}

	DVKMaterial* DVKMaterial::Create(std::shared_ptr<VulkanDevice> vulkanDevice, VkRenderPass renderPass, VkPipelineCache pipelineCache, DVKShader* shader)
	{
		// 初始化全局RingBuffer
		if (ringBufferRefCount == 0) {
			InitRingBuffer(vulkanDevice);
		}
		ringBufferRefCount += 1;

		// 创建材质
		DVKMaterial* material   = new DVKMaterial();
		material->vulkanDevice  = vulkanDevice;
		material->shader        = shader;
        material->renderPass    = renderPass;
        material->pipelineCache = pipelineCache;
		material->Prepare();
        
		return material;
	}

	void DVKMaterial::Prepare()
	{
        // 创建descriptorSet
        descriptorSet = shader->AllocateDescriptorSet();
        
		// 从Shader获取UniformBuffer信息
        for (auto it = shader->uboParams.begin(); it != shader->uboParams.end(); ++it)
        {
            DVKSimulateUniformBuffer uboBuffer = {};
            uboBuffer.binding        = it->second.binding;
            uboBuffer.descriptorType = it->second.descriptorType;
            uboBuffer.set            = it->second.set;
            uboBuffer.stageFlags     = it->second.stageFlags;
			uboBuffer.dataSize       = it->second.bufferSize;
			uboBuffer.bufferInfo     = {};
			uboBuffer.bufferInfo.buffer = ringBuffer->realBuffer->buffer;
			uboBuffer.bufferInfo.offset = 0;
			uboBuffer.bufferInfo.range  = uboBuffer.dataSize;
            uniformBuffers.insert(std::make_pair(it->first, uboBuffer));
            // WriteBuffer，从今以后所有的UniformBuffer改为Dynamic的方式
            descriptorSet->WriteBuffer(it->first, &(uboBuffer.bufferInfo));
        }
        
        // 设置Offset的索引,DynamicOffset的顺序跟set和binding顺序相关
		dynamicOffsetCount = 0;
        std::vector<DVKDescriptorSetLayoutInfo>& setLayouts = shader->setLayoutsInfo.setLayouts;
        for (int32 i = 0; i < setLayouts.size(); ++i)
        {
            std::vector<VkDescriptorSetLayoutBinding>& bindings = setLayouts[i].bindings;
            for (int32 j = 0; j < bindings.size(); ++j)
            {
                for (auto it = uniformBuffers.begin(); it != uniformBuffers.end(); ++it)
                {
                    if (it->second.set            == setLayouts[i].set &&
						it->second.binding        == bindings[j].binding &&
                        it->second.descriptorType == bindings[j].descriptorType &&
                        it->second.stageFlags     == bindings[j].stageFlags
                    )
                    {
						it->second.dynamicIndex = dynamicOffsetCount;
						dynamicOffsetCount     += 1;
                        break;
                    }
                }
            }
        }
        
		// 从Shader中获取Texture信息，包含attachment信息
        for (auto it = shader->texParams.begin(); it != shader->texParams.end(); ++it)
        {
            DVKSimulateTexture texture = {};
            texture.texture         = nullptr;
            texture.binding         = it->second.binding;
            texture.descriptorType  = it->second.descriptorType;
            texture.set             = it->second.set;
            texture.stageFlags      = it->second.stageFlags;
            textures.insert(std::make_pair(it->first, texture));
        }
	}
    
    void DVKMaterial::PreparePipeline()
    {
        if (pipeline) {
            delete pipeline;
            pipeline = nullptr;
        }
        
		// input binding info
		// 一个材质简单的只能绑定一种
		int32 stride = 0;
		for (int32 i = 0; i < shader->attributes.size(); ++i) {
			stride += VertexAttributeToSize(shader->attributes[i]);
		}
		VkVertexInputBindingDescription vertexInputBinding = {};
		vertexInputBinding.binding = 0;
		vertexInputBinding.stride = stride;
		vertexInputBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

		// input attributes
		// 一个材质也只能顺序的绑定一种
		std::vector<VkVertexInputAttributeDescription> vertexInputAttributs;
		int32 offset = 0;
		for (int32 i = 0; i < shader->attributes.size(); ++i)
		{
			VkVertexInputAttributeDescription inputAttribute = {};
			inputAttribute.binding = 0;
			inputAttribute.location = i;
			inputAttribute.format = VertexAttributeToVkFormat(shader->attributes[i]);
			inputAttribute.offset = offset;
			offset += VertexAttributeToSize(shader->attributes[i]);
			vertexInputAttributs.push_back(inputAttribute);
		}

		// pipeline
        pipelineInfo.shader = shader;
        pipeline = DVKPipeline::Create(
            vulkanDevice,
            pipelineCache,
            pipelineInfo,
            { vertexInputBinding },
			vertexInputAttributs,
            shader->pipelineLayout,
            renderPass
        );
    }

	void DVKMaterial::BeginFrame()
	{
		perObjectIndexes.clear();
	}

	void DVKMaterial::EndFrame()
	{

	}
    
	void DVKMaterial::BeginObject()
	{
		int32 index = perObjectIndexes.size();
		perObjectIndexes.push_back(index);

		int32 offsetStart = index * dynamicOffsetCount;
		if (offsetStart + dynamicOffsetCount > dynamicOffsets.size())
		{
			for (int32 i = 0; i < dynamicOffsetCount; ++i) {
				dynamicOffsets.push_back(0);
			}
		}
		else
		{
			for (int32 offsetIndex = offsetStart; offsetIndex < dynamicOffsetCount; ++offsetIndex) {
				dynamicOffsets[offsetIndex] = 0;
			}
		}
	}

	void DVKMaterial::EndObject()
	{

	}

	void DVKMaterial::BindDescriptorSets(VkCommandBuffer commandBuffer, VkPipelineBindPoint bindPoint, int32 objIndex)
	{
		int32 offsetStart  = perObjectIndexes[objIndex] * dynamicOffsetCount;
		uint32* dynOffsets = dynamicOffsets.data() + offsetStart;

		vkCmdBindDescriptorSets(
			commandBuffer, 
			VK_PIPELINE_BIND_POINT_GRAPHICS, 
			GetPipelineLayout(), 
			0, GetDescriptorSets().size(), GetDescriptorSets().data(), 
			dynamicOffsetCount, dynOffsets
		);
	}

    void DVKMaterial::SetUniform(const std::string& name, void* dataPtr, uint32 size)
    {
        auto it = uniformBuffers.find(name);
        if (it == uniformBuffers.end()) {
            MLOGE("Uniform %s not found.", name.c_str());
            return;
        }
        
        if (it->second.dataSize != size) {
            MLOGE("Uniform %s size not match, dst=%ud src=%ud", name.c_str(), it->second.dataSize, size);
            return;
        }

		// 获取Object的起始位置以及DynamicOffset的起始位置
		int32 objIndex     = perObjectIndexes.back();
		int32 offsetStart  = objIndex * dynamicOffsetCount;
		uint32* dynOffsets = dynamicOffsets.data() + offsetStart;

		// 拷贝数据至ringbuffer
		uint8* ringCPUData = (uint8*)(ringBuffer->GetMappedPointer());
		uint64 ringOffset  = ringBuffer->AllocateMemory(it->second.dataSize);
		uint64 bufferSize  = it->second.dataSize;
		
		// 拷贝数据
		memcpy(ringCPUData + ringOffset, dataPtr, bufferSize);
		// 记录Offset
		dynOffsets[it->second.dynamicIndex] = ringOffset;
    }
    
    void DVKMaterial::SetTexture(const std::string& name, DVKTexture* texture)
    {
        auto it = textures.find(name);
        if (it == textures.end()) {
            MLOGE("Texture %s not found.", name.c_str());
            return;
        }
        
        if (it->second.texture != texture) {
            it->second.texture = texture;
            descriptorSet->WriteImage(name, texture);
        }
    }
    
	void DVKMaterial::SetInputAttachment(const std::string& name, DVKTexture* texture)
	{
		auto it = textures.find(name);
		if (it == textures.end()) {
			MLOGE("Texture %s not found.", name.c_str());
			return;
		}

		if (it->second.texture != texture) {
			it->second.texture = texture;
			descriptorSet->WriteInputAttachment(name, texture);
		}
	}

};
