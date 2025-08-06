#include "onnx_inference.h"
#include <onnxruntime_c_api.h>
#include <stdio.h>
#include <stdlib.h>
#include "main.h"

static const OrtApi *ort = NULL;
static OrtEnv *env = NULL;
static OrtSession *session = NULL;
static OrtMemoryInfo *mem_info = NULL;
static char *input_name = NULL;
static char **output_names = NULL;
static size_t num_outputs = 0;

int onnx_init(const char *model_path) {
    ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    OrtStatus *status = ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "dpdk-ml", &env);
    if (status) return -1;

    OrtSessionOptions *session_options;
    OrtStatus *SessionCreation = ort->CreateSessionOptions(&session_options);
    if (SessionCreation) {
        fprintf(stderr, "ONNX Runtime error: %s\n", ort->GetErrorMessage(SessionCreation));
        return -1;
    }
    ort->SetIntraOpNumThreads(session_options, 1);
    ort->SetSessionGraphOptimizationLevel(session_options, ORT_ENABLE_BASIC);

    status = ort->CreateSession(env, model_path, session_options, &session);
    ort->ReleaseSessionOptions(session_options);
    if (status) return -1;

    ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_info);

    // Get input name
    OrtAllocator *allocator;
    ort->GetAllocatorWithDefaultOptions(&allocator);
    OrtStatus *Inputstatus = ort->SessionGetInputName(session, 0, allocator, &input_name);
    if (Inputstatus != NULL) {
        fprintf(stderr, "Failed to get input name: %s\n", ort->GetErrorMessage(Inputstatus));
        ort->ReleaseStatus(Inputstatus);
        return -1;
    }
    
    printf("Input name: %s\n", input_name);
    
    // Get the number of outputs
    status = ort->SessionGetOutputCount(session, &num_outputs);
    if (status != NULL) {
        fprintf(stderr, "Failed to get output count: %s\n", ort->GetErrorMessage(status));
        ort->ReleaseStatus(status);
        return -1;
    }
    
    printf("Model has %zu outputs\n", num_outputs);
    
    // Allocate array for output names
    output_names = malloc(num_outputs * sizeof(char*));
    if (!output_names) {
        fprintf(stderr, "Failed to allocate memory for output names\n");
        return -1;
    }
    
    // Get all output names
    for (size_t i = 0; i < num_outputs; i++) {
        status = ort->SessionGetOutputName(session, i, allocator, &output_names[i]);
        if (status != NULL) {
            fprintf(stderr, "Failed to get output name %zu: %s\n", i, ort->GetErrorMessage(status));
            ort->ReleaseStatus(status);
            return -1;
        }
        printf("Output %zu name: %s\n", i, output_names[i]);
    }

    return 0;
}

int onnx_infer(struct ml_input_features *features, float *result) {
    OrtValue *input_tensor = NULL;
    OrtValue **output_tensors = malloc(num_outputs * sizeof(OrtValue*));
    if (!output_tensors) {
        fprintf(stderr, "Failed to allocate memory for output tensors\n");
        return -1;
    }
    
    for (size_t i = 0; i < num_outputs; i++) {
        output_tensors[i] = NULL;
    }

    int64_t input_shape[2] = {1, 23};
    OrtStatus *status = ort->CreateTensorWithDataAsOrtValue(
        mem_info, features, sizeof(struct ml_input_features),
        input_shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_tensor);

    if (status != NULL) {
        fprintf(stderr, "Tensor creation failed: %s\n", ort->GetErrorMessage(status));
        ort->ReleaseStatus(status);
        free(output_tensors);
        return -1;
    }

    const char *input_names[] = {input_name};
    
    // Use the dynamically retrieved output names
    status = ort->Run(session, NULL, input_names, 
                     (const OrtValue* const*)&input_tensor, 1,
                     (const char* const*)output_names, num_outputs, 
                     output_tensors);
                     
    if (status != NULL) {
        fprintf(stderr, "ONNX Run failed: %s\n", ort->GetErrorMessage(status));
        ort->ReleaseStatus(status);
        ort->ReleaseValue(input_tensor);
        free(output_tensors);
        return -1;
    }

    
    if (num_outputs >= 2) {
        // Try to get the second output - usually probabilities
        float *prob_data;
        status = ort->GetTensorMutableData(output_tensors[1], (void**)&prob_data);
        
        if (status == NULL) {
            // Check if this is a dictionary or direct probability array
            size_t elem_count;
            const OrtTensorTypeAndShapeInfo* tensor_info;
            status = ort->GetTensorTypeAndShape(output_tensors[1], &tensor_info);
            if (status == NULL) {
                status = ort->GetTensorShapeElementCount(tensor_info, &elem_count);
                if (status == NULL) {
                    // Try to get the attack probability (class 1)
                    if (elem_count > 1) {
                        *result = prob_data[1];  // For direct array output [benign_prob, attack_prob]
                    } else {
                        *result = prob_data[0];  // For single value output (attack probability)
                    }
                    printf("Attack probability from 2nd output: %f\n", *result);
                    
                    // Release resources and return success
                    ort->ReleaseTensorTypeAndShapeInfo(tensor_info);
                    ort->ReleaseValue(input_tensor);
                    for (size_t i = 0; i < num_outputs; i++) {
                        if (output_tensors[i]) ort->ReleaseValue(output_tensors[i]);
                    }
                    free(output_tensors);
                    return 0;
                }
                ort->ReleaseTensorTypeAndShapeInfo(tensor_info);
            }
        }
        
        // Only try the first output if second output failed
        float *label_data;
        status = ort->GetTensorMutableData(output_tensors[0], (void**)&label_data);
        if (status == NULL) {
            *result = label_data[0];
            printf("Using first output as result: %f\n", *result);
            // ... [rest of this block unchanged] ...
        } 
    } else if (num_outputs == 1) {
        // Only one output - use it
        float *output_data;
        status = ort->GetTensorMutableData(output_tensors[0], (void**)&output_data);
        if (status == NULL) {
            *result = output_data[0];
            printf("Using single output: %f\n", *result);
            ort->ReleaseValue(input_tensor);
            for (size_t i = 0; i < num_outputs; i++) {
                if (output_tensors[i]) ort->ReleaseValue(output_tensors[i]);
            }
            free(output_tensors);
            return 0;
        }
    }

    // If we get here, we failed to extract any useful output
    fprintf(stderr, "Failed to extract any meaningful output from model\n");
    *result = 0.5f; // Default value in case of failure
    
    // Release resources
    ort->ReleaseValue(input_tensor);
    for (size_t i = 0; i < num_outputs; i++) {
        if (output_tensors[i]) ort->ReleaseValue(output_tensors[i]);
    }
    free(output_tensors);
    
    return -1;
}

void onnx_cleanup() {
    if (session) ort->ReleaseSession(session);
    if (env) ort->ReleaseEnv(env);
    if (mem_info) ort->ReleaseMemoryInfo(mem_info);
    
    // Free output names
    if (output_names) {
        OrtAllocator *allocator;
        ort->GetAllocatorWithDefaultOptions(&allocator);
        
        for (size_t i = 0; i < num_outputs; i++) {
            if (output_names[i]) {
                allocator->Free(allocator, output_names[i]);
            }
        }
        free(output_names);
    }
}




