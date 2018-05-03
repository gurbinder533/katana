#pragma once
#include <cuda.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include "bfs_pull_cuda.h"
#include "galois/runtime/cuda/DeviceSync.h"

struct CUDA_Context : public CUDA_Context_Common {
	struct CUDA_Context_Field<uint32_t> dist_current;
};

struct CUDA_Context* get_CUDA_context(int id) {
	struct CUDA_Context* ctx;
	ctx = (struct CUDA_Context* ) calloc(1, sizeof(struct CUDA_Context));
	ctx->id = id;
	return ctx;
}

bool init_CUDA_context(struct CUDA_Context* ctx, int device) {
	return init_CUDA_context_common(ctx, device);
}

void load_graph_CUDA(struct CUDA_Context* ctx, MarshalGraph &g, unsigned num_hosts) {
	size_t mem_usage = mem_usage_CUDA_common(g, num_hosts);
	mem_usage += mem_usage_CUDA_field(&ctx->dist_current, g, num_hosts);
	printf("[%d] Host memory for communication context: %3u MB\n", ctx->id, mem_usage/1048756);
	load_graph_CUDA_common(ctx, g, num_hosts);
	load_graph_CUDA_field(ctx, &ctx->dist_current, num_hosts);
	reset_CUDA_context(ctx);
}

void reset_CUDA_context(struct CUDA_Context* ctx) {
	ctx->dist_current.data.zero_gpu();
}

void get_bitset_dist_current_cuda(struct CUDA_Context* ctx, uint64_t* bitset_compute) {
	ctx->dist_current.is_updated.cpu_rd_ptr()->copy_to_cpu(bitset_compute);
}

void bitset_dist_current_reset_cuda(struct CUDA_Context* ctx) {
	ctx->dist_current.is_updated.cpu_rd_ptr()->reset();
}

void bitset_dist_current_reset_cuda(struct CUDA_Context* ctx, size_t begin, size_t end) {
	reset_bitset_field(&ctx->dist_current, begin, end);
}

uint32_t get_node_dist_current_cuda(struct CUDA_Context* ctx, unsigned LID) {
	uint32_t *dist_current = ctx->dist_current.data.cpu_rd_ptr();
	return dist_current[LID];
}

void set_node_dist_current_cuda(struct CUDA_Context* ctx, unsigned LID, uint32_t v) {
	uint32_t *dist_current = ctx->dist_current.data.cpu_wr_ptr();
	dist_current[LID] = v;
}

void add_node_dist_current_cuda(struct CUDA_Context* ctx, unsigned LID, uint32_t v) {
	uint32_t *dist_current = ctx->dist_current.data.cpu_wr_ptr();
	dist_current[LID] += v;
}

bool min_node_dist_current_cuda(struct CUDA_Context* ctx, unsigned LID, uint32_t v) {
	uint32_t *dist_current = ctx->dist_current.data.cpu_wr_ptr();
	if (dist_current[LID] > v){
		dist_current[LID] = v;
		return true;
	}
	return false;
}

void batch_get_node_dist_current_cuda(struct CUDA_Context* ctx, unsigned from_id, uint32_t *v) {
	batch_get_shared_field<uint32_t, sharedMaster, false>(ctx, &ctx->dist_current, from_id, v);
}

void batch_get_node_dist_current_cuda(struct CUDA_Context* ctx, unsigned from_id, uint64_t *bitset_comm, unsigned int *offsets, uint32_t *v, size_t *v_size, DataCommMode *data_mode) {
	batch_get_shared_field<uint32_t, sharedMaster, false>(ctx, &ctx->dist_current, from_id, bitset_comm, offsets, v, v_size, data_mode);
}

void batch_get_mirror_node_dist_current_cuda(struct CUDA_Context* ctx, unsigned from_id, uint32_t *v) {
	batch_get_shared_field<uint32_t, sharedMirror, false>(ctx, &ctx->dist_current, from_id, v);
}

void batch_get_mirror_node_dist_current_cuda(struct CUDA_Context* ctx, unsigned from_id, uint64_t *bitset_comm, unsigned int *offsets, uint32_t *v, size_t *v_size, DataCommMode *data_mode) {
	batch_get_shared_field<uint32_t, sharedMirror, false>(ctx, &ctx->dist_current, from_id, bitset_comm, offsets, v, v_size, data_mode);
}

void batch_get_reset_node_dist_current_cuda(struct CUDA_Context* ctx, unsigned from_id, uint32_t *v, uint32_t i) {
	batch_get_shared_field<uint32_t, sharedMirror, true>(ctx, &ctx->dist_current, from_id, v, i);
}

void batch_get_reset_node_dist_current_cuda(struct CUDA_Context* ctx, unsigned from_id, uint64_t *bitset_comm, unsigned int *offsets, uint32_t *v, size_t *v_size, DataCommMode *data_mode, uint32_t i) {
	batch_get_shared_field<uint32_t, sharedMirror, true>(ctx, &ctx->dist_current, from_id, bitset_comm, offsets, v, v_size, data_mode, i);
}

void batch_set_mirror_node_dist_current_cuda(struct CUDA_Context* ctx, unsigned from_id, uint64_t *bitset_comm, unsigned int *offsets, uint32_t *v, size_t v_size, DataCommMode data_mode) {
	batch_set_shared_field<uint32_t, sharedMirror, setOp>(ctx, &ctx->dist_current, from_id, bitset_comm, offsets, v, v_size, data_mode);
}

void batch_set_node_dist_current_cuda(struct CUDA_Context* ctx, unsigned from_id, uint64_t *bitset_comm, unsigned int *offsets, uint32_t *v, size_t v_size, DataCommMode data_mode) {
	batch_set_shared_field<uint32_t, sharedMaster, setOp>(ctx, &ctx->dist_current, from_id, bitset_comm, offsets, v, v_size, data_mode);
}

void batch_add_node_dist_current_cuda(struct CUDA_Context* ctx, unsigned from_id, uint64_t *bitset_comm, unsigned int *offsets, uint32_t *v, size_t v_size, DataCommMode data_mode) {
	batch_set_shared_field<uint32_t, sharedMaster, addOp>(ctx, &ctx->dist_current, from_id, bitset_comm, offsets, v, v_size, data_mode);
}

void batch_min_node_dist_current_cuda(struct CUDA_Context* ctx, unsigned from_id, uint64_t *bitset_comm, unsigned int *offsets, uint32_t *v, size_t v_size, DataCommMode data_mode) {
	batch_set_shared_field<uint32_t, sharedMaster, minOp>(ctx, &ctx->dist_current, from_id, bitset_comm, offsets, v, v_size, data_mode);
}

void batch_reset_node_dist_current_cuda(struct CUDA_Context* ctx, size_t begin, size_t end, uint32_t v) {
	reset_data_field<uint32_t>(&ctx->dist_current, begin, end, v);
}
