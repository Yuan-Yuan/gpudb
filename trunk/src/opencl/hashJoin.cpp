#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <CL/cl.h>
#include "../include/common.h"
#include "../include/hashJoin.h"
#include "../include/gpuOpenclLib.h"
#include "scanImpl.cpp"

/*
 * hashJoin implements the foreign key join between a fact table and dimension table.
 *
 * Prerequisites:
 *	1. the data to be joined can be fit into GPU device memory.
 *	2. dimension table is not compressed
 *	
 * Input:
 *	jNode: contains information about the two joined tables.
 *	pp: records statistics such as kernel execution time
 *
 * Output:
 * 	A new table node
 */

struct tableNode * hashJoin(struct joinNode *jNode, struct clContext * context,struct statistic *pp){
	struct tableNode * res = NULL;

	int *cpu_count, *resPsum;
	long count = 0;
	int i;

	cl_int error = 0;

	cl_mem gpu_hashNum;
	cl_mem gpu_result;
	cl_mem  gpu_bucket, gpu_fact, gpu_dim;
	cl_mem gpu_count,  gpu_psum, gpu_resPsum;

	size_t localSize = 256;
	int blockNum = jNode->leftTable->tupleNum / localSize +1; 
	if(blockNum > 2048)
		blockNum = 2048;
	size_t globalSize = blockNum * localSize;

	size_t threadNum = globalSize;

	res = (struct tableNode*) malloc(sizeof(struct tableNode));
	res->totalAttr = jNode->totalAttr;
	res->tupleSize = jNode->tupleSize;
	res->attrType = (int *) malloc(res->totalAttr * sizeof(int));
	res->attrSize = (int *) malloc(res->totalAttr * sizeof(int));
	res->attrIndex = (int *) malloc(res->totalAttr * sizeof(int));
	res->attrTotalSize = (int *) malloc(res->totalAttr * sizeof(int));
	res->dataPos = (int *) malloc(res->totalAttr * sizeof(int));
	res->dataFormat = (int *) malloc(res->totalAttr * sizeof(int));
	res->content = (char **) malloc(res->totalAttr * sizeof(char *));

	for(i=0;i<jNode->leftOutputAttrNum;i++){
		int pos = jNode->leftPos[i];
		res->attrType[pos] = jNode->leftOutputAttrType[i];
		int index = jNode->leftOutputIndex[i];
		res->attrSize[pos] = jNode->leftTable->attrSize[index];
		res->dataFormat[pos] = UNCOMPRESSED;
	}

	for(i=0;i<jNode->rightOutputAttrNum;i++){
		int pos = jNode->rightPos[i];
		res->attrType[pos] = jNode->rightOutputAttrType[i];
		int index = jNode->rightOutputIndex[i];
		res->attrSize[pos] = jNode->rightTable->attrSize[index];
		res->dataFormat[pos] = UNCOMPRESSED;
	}

	long primaryKeySize = sizeof(int) * jNode->rightTable->tupleNum;

/*
 * 	build hash table on GPU
 */

	cl_mem gpu_psum1;

	gpu_hashNum = clCreateBuffer(context->context, CL_MEM_READ_WRITE, sizeof(int)*HSIZE, NULL, &error);

	context->kernel = clCreateKernel(context->program,"cl_memset_int",0);
	clSetKernelArg(context->kernel,0,sizeof(cl_mem), (void*)gpu_hashNum);
	int tmp = HSIZE;
	clSetKernelArg(context->kernel,1,sizeof(int), (void*)&tmp);
	clEnqueueNDRangeKernel(context->queue, context->kernel, 1, 0, &globalSize,&localSize,0,0,0);

	gpu_count = clCreateBuffer(context->context,CL_MEM_READ_WRITE,sizeof(int)*threadNum,NULL,&error);
	gpu_resPsum = clCreateBuffer(context->context,CL_MEM_READ_WRITE,sizeof(int)*threadNum,NULL,&error);

	gpu_psum = clCreateBuffer(context->context,CL_MEM_READ_WRITE,sizeof(int)*HSIZE,NULL,&error);
	gpu_bucket = clCreateBuffer(context->context,CL_MEM_READ_WRITE,2*primaryKeySize,NULL,&error);

	gpu_psum1 = clCreateBuffer(context->context,CL_MEM_READ_WRITE,sizeof(int)*HSIZE,NULL,&error);

	int dataPos = jNode->rightTable->dataPos[jNode->rightKeyIndex];

	if(dataPos == MEM || dataPos == PINNED){
		gpu_dim = clCreateBuffer(context->context,CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,primaryKeySize, jNode->rightTable->content[jNode->rightKeyIndex],&error);

	}else if (dataPos == GPU || dataPos == UVA){
		gpu_dim = (cl_mem)jNode->rightTable->content[jNode->rightKeyIndex];
	}

	context->kernel = clCreateKernel(context->program,"count_hash_num",0);
	clSetKernelArg(context->kernel,0,sizeof(cl_mem),(void*)&gpu_dim);
	clSetKernelArg(context->kernel,1,sizeof(long),(void*)&jNode->rightTable->tupleNum);
	clSetKernelArg(context->kernel,2,sizeof(cl_mem),(void*)&gpu_hashNum);
	clEnqueueNDRangeKernel(context->queue, context->kernel, 1, 0, &globalSize,&localSize,0,0,0);

	scanImpl(gpu_hashNum,HSIZE,gpu_psum, context,pp);

	clEnqueueCopyBuffer(context->queue,gpu_psum,gpu_psum1,0,0,sizeof(int)*HSIZE,0,0,0);

	context->kernel = clCreateKernel(context->program,"build_hash_table",0); 
	clSetKernelArg(context->kernel,0,sizeof(cl_mem),(void*)&gpu_dim);
	clSetKernelArg(context->kernel,1,sizeof(long),(void*)&jNode->rightTable->tupleNum);
	clSetKernelArg(context->kernel,2,sizeof(cl_mem),(void*)&gpu_psum1);
	clSetKernelArg(context->kernel,3,sizeof(cl_mem),(void*)&gpu_bucket);
	clEnqueueNDRangeKernel(context->queue, context->kernel, 1, 0, &globalSize,&localSize,0,0,0);

	if (dataPos == MEM || dataPos == PINNED)
		clReleaseMemObject(gpu_dim);

	clReleaseMemObject(gpu_psum1);

/*
 *	join on GPU
 */

	cl_mem gpuFactFilter;

	dataPos = jNode->leftTable->dataPos[jNode->leftKeyIndex];
	int format = jNode->leftTable->dataFormat[jNode->leftKeyIndex];

	long foreignKeySize = jNode->leftTable->attrTotalSize[jNode->leftKeyIndex];
	long filterSize = jNode->leftTable->attrSize[jNode->leftKeyIndex] * jNode->leftTable->tupleNum;

	if(dataPos == MEM || dataPos == PINNED){
		gpu_fact = clCreateBuffer(context->context,CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,foreignKeySize,jNode->leftTable->content[jNode->leftKeyIndex],&error);

	}else if (dataPos == GPU || dataPos == UVA){
		gpu_fact = (cl_mem)jNode->leftTable->content[jNode->leftKeyIndex];
	}

	gpuFactFilter = clCreateBuffer(context->context,CL_MEM_READ_WRITE,filterSize,NULL,&error);

	if(format == UNCOMPRESSED){
		context->kernel = clCreateKernel(context->program,"count_join_result",0);
		clSetKernelArg(context->kernel,0,sizeof(cl_mem),(void *)&gpu_hashNum);
		clSetKernelArg(context->kernel,1,sizeof(cl_mem),(void *)&gpu_psum);
		clSetKernelArg(context->kernel,2,sizeof(cl_mem),(void *)&gpu_bucket);
		clSetKernelArg(context->kernel,3,sizeof(cl_mem),(void *)&gpu_fact);
		clSetKernelArg(context->kernel,4,sizeof(long),(void *)&jNode->leftTable->tupleNum);
		clSetKernelArg(context->kernel,5,sizeof(cl_mem),(void *)&gpu_count);
		clSetKernelArg(context->kernel,6,sizeof(cl_mem),(void *)&gpuFactFilter);
		clEnqueueNDRangeKernel(context->queue, context->kernel, 1, 0, &globalSize,&localSize,0,0,0);

	}else if(format == DICT){
		int dNum;
		struct dictHeader * dheader;

		if(dataPos == MEM || dataPos == UVA || dataPos == PINNED){
			dheader = (struct dictHeader *) jNode->leftTable->content[jNode->leftKeyIndex];
			dNum = dheader->dictNum;

		}else if (dataPos == GPU){
			dheader = (struct dictHeader *) malloc(sizeof(struct dictHeader));
			memset(dheader,0,sizeof(struct dictHeader));
			clEnqueueReadBuffer(context->queue, gpu_fact, CL_TRUE, 0, sizeof(struct dictHeader), dheader,0,0,0);
			dNum = dheader->dictNum;
		}
		free(dheader);

		cl_mem gpuDictFilter = clCreateBuffer(context->context,CL_MEM_READ_WRITE,dNum*sizeof(int),NULL,&error);

		context->kernel = clCreateKernel(context->program,"count_join_result_dict",0);
		clSetKernelArg(context->kernel,0,sizeof(cl_mem),(void *)&gpu_hashNum);
		clSetKernelArg(context->kernel,1,sizeof(cl_mem),(void *)&gpu_psum);
		clSetKernelArg(context->kernel,2,sizeof(cl_mem),(void *)&gpu_bucket);
		clSetKernelArg(context->kernel,3,sizeof(cl_mem),(void *)&gpu_fact);
		clSetKernelArg(context->kernel,4,sizeof(int),(void *)&dNum);
		clSetKernelArg(context->kernel,5,sizeof(cl_mem),(void *)&gpuDictFilter);
		clEnqueueNDRangeKernel(context->queue, context->kernel, 1, 0, &globalSize,&localSize,0,0,0);

		context->kernel = clCreateKernel(context->program,"transform_dict_filter",0);
		clSetKernelArg(context->kernel,0,sizeof(cl_mem),(void *)&gpuDictFilter);
		clSetKernelArg(context->kernel,1,sizeof(cl_mem),(void *)&gpu_fact);
		clSetKernelArg(context->kernel,2,sizeof(long),(void *)&jNode->leftTable->tupleNum);
		clSetKernelArg(context->kernel,3,sizeof(int),(void *)&dNum);
		clSetKernelArg(context->kernel,4,sizeof(cl_mem),(void *)&gpuFactFilter);

		clEnqueueNDRangeKernel(context->queue, context->kernel, 1, 0, &globalSize,&localSize,0,0,0);

		clReleaseMemObject(gpuDictFilter);

		context->kernel = clCreateKernel(context->program,"filter_count",0);
		clSetKernelArg(context->kernel,0,sizeof(long),(void *)&jNode->leftTable->tupleNum);
		clSetKernelArg(context->kernel,1,sizeof(cl_mem),(void *)&gpu_count);
		clSetKernelArg(context->kernel,2,sizeof(cl_mem),(void *)&gpuFactFilter);
		clEnqueueNDRangeKernel(context->queue, context->kernel, 1, 0, &globalSize,&localSize,0,0,0);

	}else if (format == RLE){

		long offset = 0;
		context->kernel = clCreateKernel(context->program,"count_join_result_rle",0);
		clSetKernelArg(context->kernel,0,sizeof(cl_mem),(void*)&gpu_hashNum);
		clSetKernelArg(context->kernel,1,sizeof(cl_mem),(void*)&gpu_psum);
		clSetKernelArg(context->kernel,2,sizeof(cl_mem),(void*)&gpu_bucket);
		clSetKernelArg(context->kernel,3,sizeof(cl_mem),(void*)&gpu_fact);
		clSetKernelArg(context->kernel,4,sizeof(long),(void*)&jNode->leftTable->tupleNum);
		clSetKernelArg(context->kernel,5,sizeof(long),(void*)&offset);
		clSetKernelArg(context->kernel,6,sizeof(cl_mem),(void*)&gpuFactFilter);
		clEnqueueNDRangeKernel(context->queue, context->kernel, 1, 0, &globalSize,&localSize,0,0,0);

		context->kernel = clCreateKernel(context->program,"filter_count",0);
		clSetKernelArg(context->kernel,0,sizeof(long),(void *)&jNode->leftTable->tupleNum);
		clSetKernelArg(context->kernel,1,sizeof(cl_mem),(void *)&gpu_count);
		clSetKernelArg(context->kernel,2,sizeof(cl_mem),(void *)&gpuFactFilter);
		clEnqueueNDRangeKernel(context->queue, context->kernel, 1, 0, &globalSize,&localSize,0,0,0);
	}


	cpu_count = (int *) malloc(sizeof(int)*threadNum);
	memset(cpu_count,0,sizeof(int)*threadNum);

	clEnqueueReadBuffer(context->queue, gpu_count, CL_TRUE, 0, sizeof(int)*threadNum, cpu_count,0,0,0);

	resPsum = (int *) malloc(sizeof(int)*threadNum);
	memset(resPsum,0,sizeof(int)*threadNum);

	scanImpl(gpu_count,threadNum,gpu_resPsum, context,pp);

	clEnqueueReadBuffer(context->queue, gpu_resPsum, CL_TRUE, 0, sizeof(int)*threadNum, resPsum,0,0,0);

	count = resPsum[threadNum-1] + cpu_count[threadNum-1];
	res->tupleNum = count;
	printf("joinNum %d\n",count);

	if(dataPos == MEM || dataPos == PINNED){
		clReleaseMemObject(gpu_fact);
	}

	clReleaseMemObject(gpu_bucket);
		
	free(resPsum);
	free(cpu_count);

	for(i=0; i<res->totalAttr; i++){
		int index, pos;
		long colSize = 0, resSize = 0;
		int leftRight = 0;

		int attrSize, attrType;
		char * table;
		int found = 0 , dataPos, format;

		if (jNode->keepInGpu[i] == 1)
			res->dataPos[i] = GPU;
		else
			res->dataPos[i] = MEM;

		for(int k=0;k<jNode->leftOutputAttrNum;k++){
			if (jNode->leftPos[k] == i){
				found = 1;
				leftRight = 0;
				pos = k;
				break;
			}
		}
		if(!found){
			for(int k=0;k<jNode->rightOutputAttrNum;k++){
				if(jNode->rightPos[k] == i){
					found = 1;
					leftRight = 1;
					pos = k;
					break;
				}
			}
		}

		if(leftRight == 0){
			index = jNode->leftOutputIndex[pos];
			dataPos = jNode->leftTable->dataPos[index];
			format = jNode->leftTable->dataFormat[index];

			table = jNode->leftTable->content[index];
			attrSize  = jNode->leftTable->attrSize[index];
			attrType  = jNode->leftTable->attrType[index];
			colSize = jNode->leftTable->attrTotalSize[index];

			resSize = res->tupleNum * attrSize;
		}else{
			index = jNode->rightOutputIndex[pos];
			dataPos = jNode->rightTable->dataPos[index];
			format = jNode->rightTable->dataFormat[index];

			table = jNode->rightTable->content[index];
			attrSize = jNode->rightTable->attrSize[index];
			attrType = jNode->rightTable->attrType[index];
			colSize = jNode->rightTable->attrTotalSize[index];

			resSize = attrSize * res->tupleNum;
			leftRight = 1;
		}


		gpu_result = clCreateBuffer(context->context,CL_MEM_READ_WRITE,resSize,NULL,&error);

		if(leftRight == 0){
			if(format == UNCOMPRESSED){

				if(dataPos == MEM || dataPos == PINNED){
					gpu_fact = clCreateBuffer(context->context,CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR,colSize,table,&error);
				}else{
					gpu_fact = (cl_mem)table;
				}

				if(attrSize == sizeof(int)){
					context->kernel = clCreateKernel(context->program,"joinFact_int",0);
				}else{
					context->kernel = clCreateKernel(context->program,"joinFact_other",0);
				}
				clSetKernelArg(context->kernel,0,sizeof(cl_mem),(void*)&gpu_resPsum);
				clSetKernelArg(context->kernel,1,sizeof(cl_mem),(void*)&gpu_fact);
				clSetKernelArg(context->kernel,2,sizeof(int),(void*)&attrSize);
				clSetKernelArg(context->kernel,3,sizeof(long),(void*)&jNode->leftTable->tupleNum);
				clSetKernelArg(context->kernel,4,sizeof(cl_mem),(void*)&gpuFactFilter);
				clSetKernelArg(context->kernel,5,sizeof(cl_mem),(void*)&gpu_result);
				clEnqueueNDRangeKernel(context->queue, context->kernel, 1, 0, &globalSize,&localSize,0,0,0);

			}else if (format == DICT){
				struct dictHeader * dheader;
				int byteNum;

				assert(dataPos == MEM || dataPos == PINNED);

				dheader = (struct dictHeader *)table;
				byteNum = dheader->bitNum/8;
				
				cl_mem gpuDictHeader = clCreateBuffer(context->context,CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,sizeof(struct dictHeader), dheader,&error);

				if(dataPos == MEM || dataPos == PINNED){
					gpu_fact = clCreateBuffer(context->context,CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,colSize-sizeof(struct dictHeader), table + sizeof(struct dictHeader), &error);
				}else{
					gpu_fact = (cl_mem)(table + sizeof(struct dictHeader));
				}

				if (attrSize == sizeof(int))
					context->kernel = clCreateKernel(context->program,"joinFact_dict_int",0);
				else
					context->kernel = clCreateKernel(context->program,"joinFact_dict_other",0);

				clSetKernelArg(context->kernel,0,sizeof(cl_mem),(void*)&gpu_resPsum);
				clSetKernelArg(context->kernel,1,sizeof(cl_mem),(void*)&gpu_fact);
				clSetKernelArg(context->kernel,2,sizeof(cl_mem),(void*)&gpuDictHeader);
				clSetKernelArg(context->kernel,4,sizeof(int),(void*)&byteNum);
				clSetKernelArg(context->kernel,5,sizeof(int),(void*)&attrSize);
				clSetKernelArg(context->kernel,6,sizeof(long),(void*)&jNode->leftTable->tupleNum);
				clSetKernelArg(context->kernel,7,sizeof(cl_mem),(void*)&gpuFactFilter);
				clSetKernelArg(context->kernel,8,sizeof(cl_mem),(void*)&gpu_result);
				clEnqueueNDRangeKernel(context->queue, context->kernel, 1, 0, &globalSize,&localSize,0,0,0);

				clReleaseMemObject(gpuDictHeader);

			}else if (format == RLE){

				if(dataPos == MEM || dataPos == PINNED){
					gpu_fact = clCreateBuffer(context->context,CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,colSize,table,&error);
				}else{
					gpu_fact = (cl_mem)table;
				}

				int dNum = (colSize - sizeof(struct rleHeader))/(3*sizeof(int));

				cl_mem gpuRle = clCreateBuffer(context->context,CL_MEM_READ_WRITE,jNode->leftTable->tupleNum * sizeof(int), NULL, &error);;

				long offset = 0;
				context->kernel = clCreateKernel(context->program,"unpack_rle",0);
				clSetKernelArg(context->kernel,0,sizeof(cl_mem),(void*)&gpu_fact);
				clSetKernelArg(context->kernel,1,sizeof(cl_mem),(void*)&gpuRle);
				clSetKernelArg(context->kernel,2,sizeof(long),(void*)&jNode->leftTable->tupleNum);
				clSetKernelArg(context->kernel,3,sizeof(long),(void*)&offset);
				clSetKernelArg(context->kernel,4,sizeof(int),(void*)&dNum);
				clEnqueueNDRangeKernel(context->queue, context->kernel, 1, 0, &globalSize,&localSize,0,0,0);

				context->kernel = clCreateKernel(context->program,"joinFact_int",0);
				clSetKernelArg(context->kernel,0,sizeof(cl_mem), (void*)gpu_resPsum);
				clSetKernelArg(context->kernel,1,sizeof(cl_mem), (void*)gpuRle);
				clSetKernelArg(context->kernel,2,sizeof(int), (void*)&attrSize);
				clSetKernelArg(context->kernel,3,sizeof(long),(void*)&jNode->leftTable->tupleNum);
				clSetKernelArg(context->kernel,4,sizeof(cl_mem), (void*)gpuFactFilter);
				clSetKernelArg(context->kernel,5,sizeof(cl_mem), (void*)gpu_result);
				clEnqueueNDRangeKernel(context->queue, context->kernel, 1, 0, &globalSize,&localSize,0,0,0);

				clReleaseMemObject(gpuRle);

			}

		}else{
			if(format == UNCOMPRESSED){

				if(dataPos == MEM || dataPos == PINNED){
					gpu_fact = clCreateBuffer(context->context,CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,colSize,table,&error);
				}else{
					gpu_fact = (cl_mem)table;
				}

				if(attrType == sizeof(int))
					context->kernel = clCreateKernel(context->program,"joinDim_int",0);
				else
					context->kernel = clCreateKernel(context->program,"joinDim_other",0);

				clSetKernelArg(context->kernel,0,sizeof(cl_mem),(void*)&gpu_resPsum);
				clSetKernelArg(context->kernel,1,sizeof(cl_mem),(void*)&gpu_fact);
				clSetKernelArg(context->kernel,2,sizeof(int),(void*)&attrSize);
				clSetKernelArg(context->kernel,3,sizeof(long),(void*)&jNode->leftTable->tupleNum);
				clSetKernelArg(context->kernel,4,sizeof(cl_mem),(void*)&gpuFactFilter);
				clSetKernelArg(context->kernel,5,sizeof(cl_mem),(void*)&gpu_result);
				
				clEnqueueNDRangeKernel(context->queue, context->kernel, 1, 0, &globalSize,&localSize,0,0,0);

			}else if (format == DICT){
				struct dictHeader * dheader;
				int byteNum;
				assert(dataPos == MEM || dataPos == PINNED);

				dheader = (struct dictHeader *)table;
				byteNum = dheader->bitNum/8;

				cl_mem gpuDictHeader = clCreateBuffer(context->context,CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,sizeof(struct dictHeader),dheader,&error);

				if(dataPos == MEM || dataPos == PINNED){
					gpu_fact = clCreateBuffer(context->context,CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,colSize-sizeof(struct dictHeader),table+sizeof(struct dictHeader),&error);
				}else{
					gpu_fact = (cl_mem)(table + sizeof(struct dictHeader));
				}

				if(attrType == sizeof(int))
					context->kernel = clCreateKernel(context->program,"joinDim_dict_int",0);
				else
					context->kernel = clCreateKernel(context->program,"joinDim_dict_other",0);

				clSetKernelArg(context->kernel,0,sizeof(cl_mem),(void*)&gpu_resPsum);
				clSetKernelArg(context->kernel,1,sizeof(cl_mem),(void*)&gpu_fact);
				clSetKernelArg(context->kernel,2,sizeof(cl_mem),(void*)&gpuDictHeader);
				clSetKernelArg(context->kernel,3,sizeof(int),(void*)&byteNum);
				clSetKernelArg(context->kernel,4,sizeof(int),(void*)&attrSize);
				clSetKernelArg(context->kernel,5,sizeof(long),(void*)&jNode->leftTable->tupleNum);
				clSetKernelArg(context->kernel,6,sizeof(cl_mem),(void*)&gpuFactFilter);
				clSetKernelArg(context->kernel,7,sizeof(cl_mem),(void*)&gpu_result);

				clEnqueueNDRangeKernel(context->queue, context->kernel, 1, 0, &globalSize,&localSize,0,0,0);
				clReleaseMemObject(gpuDictHeader);

			}else if (format == RLE){

				if(dataPos == MEM || dataPos == PINNED){
					gpu_fact = clCreateBuffer(context->context,CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,colSize,table,&error);
				}else{
					gpu_fact = (cl_mem)table;
				}

				long offset = 0;
				context->kernel = clCreateKernel(context->program,"joinDim_rle",0);
				clSetKernelArg(context->kernel,0,sizeof(cl_mem),(void*)&gpu_resPsum);
				clSetKernelArg(context->kernel,1,sizeof(cl_mem),(void*)&gpu_fact);
				clSetKernelArg(context->kernel,2,sizeof(int),(void*)&attrSize);
				clSetKernelArg(context->kernel,3,sizeof(long),(void*)&jNode->leftTable->tupleNum);
				clSetKernelArg(context->kernel,4,sizeof(long),(void*)&offset);
				clSetKernelArg(context->kernel,5,sizeof(cl_mem),(void*)&gpuFactFilter);
				clSetKernelArg(context->kernel,6,sizeof(cl_mem),(void*)&gpu_result);

				clEnqueueNDRangeKernel(context->queue, context->kernel, 1, 0, &globalSize,&localSize,0,0,0);
			}
		}
		
		res->attrTotalSize[i] = resSize;
		res->dataFormat[i] = UNCOMPRESSED;
		if(res->dataPos[i] == MEM){
			res->content[i] = (char *) malloc(resSize);
			memset(res->content[i],0,resSize);
			clEnqueueReadBuffer(context->queue,gpu_result,CL_TRUE,0,resSize,res->content[i],0,0,0);
			clReleaseMemObject(gpu_result);

		}else if(res->dataPos[i] == GPU){
			res->content[i] = (char *)gpu_result;
		}
		if(dataPos == MEM || dataPos == PINNED)
			clReleaseMemObject(gpu_fact);

	}

	clReleaseMemObject(gpuFactFilter);

	clReleaseMemObject(gpu_count);
	clReleaseMemObject(gpu_hashNum);
	clReleaseMemObject(gpu_psum);

	return res;

}