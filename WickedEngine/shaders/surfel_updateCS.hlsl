#include "globals.hlsli"
#include "ShaderInterop_SurfelGI.h"
#include "brdf.hlsli"

RAWBUFFER(surfelStatsBuffer, TEXSLOT_ONDEMAND0);
STRUCTUREDBUFFER(surfelDataBuffer, SurfelData, TEXSLOT_ONDEMAND1);

RWSTRUCTUREDBUFFER(surfelBuffer, Surfel, 0);
RWSTRUCTUREDBUFFER(surfelGridBuffer, SurfelGridCell, 1);

[numthreads(SURFEL_INDIRECT_NUMTHREADS, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	uint surfel_count = surfelStatsBuffer.Load(SURFEL_STATS_OFFSET_COUNT);
	if (DTid.x >= surfel_count)
		return;

	uint surfel_index = DTid.x;
	SurfelData surfel_data = surfelDataBuffer[surfel_index];
	Surfel surfel = surfelBuffer[surfel_index];


	PrimitiveID prim;
	prim.unpack(surfel_data.primitiveID);

	Surface surface;
	if (surface.load(prim, unpack_half2(surfel_data.bary), surfel_data.uid))
	{
		surfel.normal = pack_unitvector(surface.facenormal);
		surfel.position = surface.P;

		int3 center_cell = surfel_gridpos(surfel.position);
		for (int i = -1; i <= 1; ++i)
		{
			if (center_cell.x + i < 0 || center_cell.x + i >= SURFEL_GRID_DIMENSIONS.x)
				continue;

			for (int j = -1; j <= 1; ++j)
			{
				if (center_cell.y + j < 0 || center_cell.y + j >= SURFEL_GRID_DIMENSIONS.y)
					continue;

				for (int k = -1; k <= 1; ++k)
				{
					if (center_cell.z + k < 0 || center_cell.z + k >= SURFEL_GRID_DIMENSIONS.z)
						continue;

					int3 gridpos = uint3(center_cell + int3(i, j, k));

					//float3 center = surfel.position;
					//float radius = GetSurfelRadius();
					//float3 cellmin = (float3)gridpos + g_xFrame_WorldBoundsMin;
					//float3 cellmax = cellmin + GetSurfelRadius();
					//float3 closestPointInAabb = min(max(center, cellmin), cellmax);
					//float dist = distance(closestPointInAabb, center);
					//if (dist < radius)
					{
						uint cellindex = surfel_cellindex(gridpos);
						InterlockedAdd(surfelGridBuffer[cellindex].count, 1);
					}

				}
			}
		}

		surfel.color = float4(surfel_data.mean, 1);
	}
	else
	{
		surfel.color = 0;
	}

	surfelBuffer[surfel_index] = surfel;
}
