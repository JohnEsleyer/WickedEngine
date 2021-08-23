#include "wiGPUBVH.h"
#include "wiScene.h"
#include "wiRenderer.h"
#include "shaders/ShaderInterop_BVH.h"
#include "wiProfiler.h"
#include "wiResourceManager.h"
#include "wiGPUSortLib.h"
#include "wiTextureHelper.h"
#include "wiBackLog.h"
#include "wiEvent.h"

//#define BVH_VALIDATE // slow but great for debug!
#ifdef BVH_VALIDATE
#include <set>
#endif // BVH_VALIDATE

using namespace wiGraphics;
using namespace wiScene;
using namespace wiECS;

enum CSTYPES_BVH
{
	CSTYPE_BVH_PRIMITIVES,
	CSTYPE_BVH_HIERARCHY,
	CSTYPE_BVH_PROPAGATEAABB,
	CSTYPE_BVH_COUNT
};
static Shader computeShaders[CSTYPE_BVH_COUNT];

void wiGPUBVH::Update(const wiScene::Scene& scene)
{
	GraphicsDevice* device = wiRenderer::GetDevice();

	if (!primitiveCounterBuffer.IsValid())
	{
		GPUBufferDesc desc;
		desc.BindFlags = BIND_SHADER_RESOURCE;
		desc.StructureByteStride = sizeof(uint);
		desc.ByteWidth = desc.StructureByteStride;
		desc.CPUAccessFlags = 0;
		desc.Format = FORMAT_UNKNOWN;
		desc.MiscFlags = RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		desc.Usage = USAGE_DEFAULT;
		device->CreateBuffer(&desc, nullptr, &primitiveCounterBuffer);
		device->SetName(&primitiveCounterBuffer, "primitiveCounterBuffer");
	}

	// Pre-gather scene properties:
	uint totalTriangles = 0;
	for (size_t i = 0; i < scene.objects.GetCount(); ++i)
	{
		const ObjectComponent& object = scene.objects[i];

		if (object.meshID != INVALID_ENTITY)
		{
			const MeshComponent& mesh = *scene.meshes.GetComponent(object.meshID);

			totalTriangles += (uint)mesh.indices.size() / 3;
		}
	}
	for (size_t i = 0; i < scene.hairs.GetCount(); ++i)
	{
		const wiHairParticle& hair = scene.hairs[i];

		if (hair.meshID != INVALID_ENTITY)
		{
			totalTriangles += hair.segmentCount * hair.strandCount * 2;
		}
	}

	if (totalTriangles > primitiveCapacity)
	{
		primitiveCapacity = std::max(2u, totalTriangles);

		GPUBufferDesc desc;

		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.StructureByteStride = sizeof(BVHNode);
		desc.ByteWidth = desc.StructureByteStride * primitiveCapacity * 2;
		desc.CPUAccessFlags = 0;
		desc.Format = FORMAT_UNKNOWN;
		desc.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.Usage = USAGE_DEFAULT;
		device->CreateBuffer(&desc, nullptr, &bvhNodeBuffer);
		device->SetName(&bvhNodeBuffer, "BVHNodeBuffer");

		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.StructureByteStride = sizeof(uint);
		desc.ByteWidth = desc.StructureByteStride * primitiveCapacity * 2;
		desc.CPUAccessFlags = 0;
		desc.Format = FORMAT_UNKNOWN;
		desc.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.Usage = USAGE_DEFAULT;
		device->CreateBuffer(&desc, nullptr, &bvhParentBuffer);
		device->SetName(&bvhParentBuffer, "BVHParentBuffer");

		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.StructureByteStride = sizeof(uint);
		desc.ByteWidth = desc.StructureByteStride * (((primitiveCapacity - 1) + 31) / 32); // bitfield for internal nodes
		desc.CPUAccessFlags = 0;
		desc.Format = FORMAT_UNKNOWN;
		desc.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.Usage = USAGE_DEFAULT;
		device->CreateBuffer(&desc, nullptr, &bvhFlagBuffer);
		device->SetName(&bvhFlagBuffer, "BVHFlagBuffer");

		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.StructureByteStride = sizeof(uint);
		desc.ByteWidth = desc.StructureByteStride * primitiveCapacity;
		desc.CPUAccessFlags = 0;
		desc.Format = FORMAT_UNKNOWN;
		desc.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.Usage = USAGE_DEFAULT;
		device->CreateBuffer(&desc, nullptr, &primitiveIDBuffer);
		device->SetName(&primitiveIDBuffer, "primitiveIDBuffer");

		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.StructureByteStride = sizeof(BVHPrimitive);
		desc.ByteWidth = desc.StructureByteStride * primitiveCapacity;
		desc.CPUAccessFlags = 0;
		desc.Format = FORMAT_UNKNOWN;
		desc.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.Usage = USAGE_DEFAULT;
		device->CreateBuffer(&desc, nullptr, &primitiveBuffer);
		device->SetName(&primitiveBuffer, "primitiveBuffer");

		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.ByteWidth = desc.StructureByteStride * primitiveCapacity;
		desc.CPUAccessFlags = 0;
		desc.Format = FORMAT_UNKNOWN;
		desc.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.Usage = USAGE_DEFAULT;
		desc.StructureByteStride = sizeof(float); // morton buffer is float because sorting must be done and gpu sort operates on floats for now!
		device->CreateBuffer(&desc, nullptr, &primitiveMortonBuffer);
		device->SetName(&primitiveMortonBuffer, "primitiveMortonBuffer");
	}
}
void wiGPUBVH::Build(const Scene& scene, CommandList cmd) const
{
	GraphicsDevice* device = wiRenderer::GetDevice();

	auto range = wiProfiler::BeginRangeGPU("BVH Rebuild", cmd);

	uint32_t primitiveCount = 0;

	device->BindResource(CS, &scene.instanceBuffer, SBSLOT_INSTANCEARRAY, cmd);

	device->EventBegin("BVH - Primitive Builder", cmd);
	{
		device->BindComputeShader(&computeShaders[CSTYPE_BVH_PRIMITIVES], cmd);
		const GPUResource* uavs[] = {
			&primitiveIDBuffer,
			&primitiveBuffer,
			&primitiveMortonBuffer,
		};
		device->BindUAVs(CS, uavs, 0, arraysize(uavs), cmd);

		for (size_t i = 0; i < scene.objects.GetCount(); ++i)
		{
			const ObjectComponent& object = scene.objects[i];

			if (object.meshID != INVALID_ENTITY)
			{
				const MeshComponent& mesh = *scene.meshes.GetComponent(object.meshID);

				for (size_t j = 0; j < mesh.subsets.size(); ++j)
				{
					auto& subset = mesh.subsets[j];

					BVHPushConstants push;
					push.instanceIndex = (uint)i;
					push.subsetIndex = (uint)j;
					push.primitiveCount = subset.indexCount / 3;
					push.primitiveOffset = primitiveCount;
					device->PushConstants(&push, sizeof(push), cmd);

					primitiveCount += push.primitiveCount;

					device->Dispatch(
						(push.primitiveCount + BVH_BUILDER_GROUPSIZE - 1) / BVH_BUILDER_GROUPSIZE,
						1,
						1,
						cmd
					);
				}

			}
		}

		for (size_t i = 0; i < scene.hairs.GetCount(); ++i)
		{
			const wiHairParticle& hair = scene.hairs[i];

			if (hair.meshID != INVALID_ENTITY)
			{
				BVHPushConstants push;
				push.instanceIndex = (uint)(scene.objects.GetCount() + i);
				push.subsetIndex = 0;
				push.primitiveCount = hair.segmentCount * hair.strandCount * 2;
				push.primitiveOffset = primitiveCount;
				device->PushConstants(&push, sizeof(push), cmd);

				primitiveCount += push.primitiveCount;

				device->Dispatch(
					(push.primitiveCount + BVH_BUILDER_GROUPSIZE - 1) / BVH_BUILDER_GROUPSIZE,
					1,
					1,
					cmd
				);
			}
		}

		GPUBarrier barriers[] = {
			GPUBarrier::Memory()
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
		device->UnbindUAVs(0, arraysize(uavs), cmd);
	}
	device->UpdateBuffer(&primitiveCounterBuffer, &primitiveCount, cmd);
	device->EventEnd(cmd);

	device->EventBegin("BVH - Sort Primitive Mortons", cmd);
	wiGPUSortLib::Sort(primitiveCount, primitiveMortonBuffer, primitiveCounterBuffer, 0, primitiveIDBuffer, cmd);
	device->EventEnd(cmd);

	device->EventBegin("BVH - Build Hierarchy", cmd);
	{
		device->BindComputeShader(&computeShaders[CSTYPE_BVH_HIERARCHY], cmd);
		const GPUResource* uavs[] = {
			&bvhNodeBuffer,
			&bvhParentBuffer,
			&bvhFlagBuffer
		};
		device->BindUAVs(CS, uavs, 0, arraysize(uavs), cmd);

		const GPUResource* res[] = {
			&primitiveCounterBuffer,
			&primitiveIDBuffer,
			&primitiveMortonBuffer,
		};
		device->BindResources(CS, res, TEXSLOT_ONDEMAND0, arraysize(res), cmd);

		device->Dispatch((primitiveCount + BVH_BUILDER_GROUPSIZE - 1) / BVH_BUILDER_GROUPSIZE, 1, 1, cmd);

		GPUBarrier barriers[] = {
			GPUBarrier::Memory()
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
		device->UnbindUAVs(0, arraysize(uavs), cmd);
	}
	device->EventEnd(cmd);

	device->EventBegin("BVH - Propagate AABB", cmd);
	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory()
		};
		device->Barrier(barriers, arraysize(barriers), cmd);

		device->BindComputeShader(&computeShaders[CSTYPE_BVH_PROPAGATEAABB], cmd);
		const GPUResource* uavs[] = {
			&bvhNodeBuffer,
			&bvhFlagBuffer,
		};
		device->BindUAVs(CS, uavs, 0, arraysize(uavs), cmd);

		const GPUResource* res[] = {
			&primitiveCounterBuffer,
			&primitiveIDBuffer,
			&primitiveBuffer,
			&bvhParentBuffer,
		};
		device->BindResources(CS, res, TEXSLOT_ONDEMAND0, arraysize(res), cmd);

		device->Dispatch((primitiveCount + BVH_BUILDER_GROUPSIZE - 1) / BVH_BUILDER_GROUPSIZE, 1, 1, cmd);

		device->Barrier(barriers, arraysize(barriers), cmd);
		device->UnbindUAVs(0, arraysize(uavs), cmd);
	}
	device->EventEnd(cmd);

	wiProfiler::EndRange(range); // BVH rebuild

#ifdef BVH_VALIDATE
	{
		GPUBufferDesc readback_desc;
		bool download_success;

		// Download primitive count:
		readback_desc = primitiveCounterBuffer.GetDesc();
		readback_desc.Usage = USAGE_STAGING;
		readback_desc.CPUAccessFlags = CPU_ACCESS_READ;
		readback_desc.BindFlags = 0;
		readback_desc.MiscFlags = 0;
		GPUBuffer readback_primitiveCounterBuffer;
		device->CreateBuffer(&readback_desc, nullptr, &readback_primitiveCounterBuffer);
		uint primitiveCount;
		download_success = device->DownloadResource(&primitiveCounterBuffer, &readback_primitiveCounterBuffer, &primitiveCount, cmd);
		assert(download_success);

		if (primitiveCount > 0)
		{
			const uint leafNodeOffset = primitiveCount - 1;

			// Validate node buffer:
			readback_desc = bvhNodeBuffer.GetDesc();
			readback_desc.Usage = USAGE_STAGING;
			readback_desc.CPUAccessFlags = CPU_ACCESS_READ;
			readback_desc.BindFlags = 0;
			readback_desc.MiscFlags = 0;
			GPUBuffer readback_nodeBuffer;
			device->CreateBuffer(&readback_desc, nullptr, &readback_nodeBuffer);
			vector<BVHNode> nodes(readback_desc.ByteWidth / sizeof(BVHNode));
			download_success = device->DownloadResource(&bvhNodeBuffer, &readback_nodeBuffer, nodes.data(), cmd);
			assert(download_success);
			set<uint> visitedLeafs;
			vector<uint> stack;
			stack.push_back(0);
			while (!stack.empty())
			{
				uint nodeIndex = stack.back();
				stack.pop_back();

				if (nodeIndex >= leafNodeOffset)
				{
					// leaf node
					assert(visitedLeafs.count(nodeIndex) == 0); // leaf node was already visited, this must not happen!
					visitedLeafs.insert(nodeIndex);
				}
				else
				{
					// internal node
					BVHNode& node = nodes[nodeIndex];
					stack.push_back(node.LeftChildIndex);
					stack.push_back(node.RightChildIndex);
				}
			}
			for (uint i = 0; i < primitiveCount; ++i)
			{
				uint nodeIndex = leafNodeOffset + i;
				BVHNode& leaf = nodes[nodeIndex];
				assert(leaf.LeftChildIndex == 0 && leaf.RightChildIndex == 0); // a leaf must have no children
				assert(visitedLeafs.count(nodeIndex) > 0); // every leaf node must have been visited in the traversal above
			}

			// Validate flag buffer:
			readback_desc = bvhFlagBuffer.GetDesc();
			readback_desc.Usage = USAGE_STAGING;
			readback_desc.CPUAccessFlags = CPU_ACCESS_READ;
			readback_desc.BindFlags = 0;
			readback_desc.MiscFlags = 0;
			GPUBuffer readback_flagBuffer;
			device->CreateBuffer(&readback_desc, nullptr, &readback_flagBuffer);
			vector<uint> flags(readback_desc.ByteWidth / sizeof(uint));
			download_success = device->DownloadResource(&bvhFlagBuffer, &readback_flagBuffer, flags.data(), cmd);
			assert(download_success);
			for (auto& x : flags)
			{
				if (x > 2)
				{
					assert(0); // flagbuffer anomaly detected: node can't have more than two children (AABB propagation step)!
					break;
				}
			}
		}
	}
#endif // BVH_VALIDATE

}
void wiGPUBVH::Bind(SHADERSTAGE stage, CommandList cmd) const
{
	GraphicsDevice* device = wiRenderer::GetDevice();

	const GPUResource* res[] = {
		&primitiveCounterBuffer,
		&primitiveBuffer,
		&bvhNodeBuffer,
	};
	device->BindResources(stage, res, TEXSLOT_BVH_COUNTER, arraysize(res), cmd);
}

void wiGPUBVH::Clear()
{
	primitiveCapacity = 0;
}

namespace wiGPUBVH_Internal
{
	void LoadShaders()
	{
		wiRenderer::LoadShader(CS, computeShaders[CSTYPE_BVH_PRIMITIVES], "bvh_primitivesCS.cso");
		wiRenderer::LoadShader(CS, computeShaders[CSTYPE_BVH_HIERARCHY], "bvh_hierarchyCS.cso");
		wiRenderer::LoadShader(CS, computeShaders[CSTYPE_BVH_PROPAGATEAABB], "bvh_propagateaabbCS.cso");
	}
}

void wiGPUBVH::Initialize()
{
	static wiEvent::Handle handle = wiEvent::Subscribe(SYSTEM_EVENT_RELOAD_SHADERS, [](uint64_t userdata) { wiGPUBVH_Internal::LoadShaders(); });
	wiGPUBVH_Internal::LoadShaders();
}
